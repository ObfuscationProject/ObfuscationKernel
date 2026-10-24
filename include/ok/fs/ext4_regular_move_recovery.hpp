#pragma once

#include "ok/fs/ext4_regular_move_replay.hpp"
#include "ok/fs/ext4_orphan_chain.hpp"

namespace ok::fs
{
// Actual, read-only cold-device binding for one active OKMVW001 transaction.
// The caller continuously owns quiesced media and excludes caches, replacement
// and other writers. This public source prototype does not acquire that session
// or grant mounted/VFS/capability admission. All retained images are owned here.
class Ext4RegularMoveRecoveryBinding final
{
    friend class Ext4RegularMoveCompletion;
    template <usize, usize> friend class Ext4OwnedTransaction;
    using Block = Ext4RegularMoveBlock;
    static constexpr usize home_capacity = Ext4RegularMovePlan::capacity;
    static constexpr usize journal_capacity = home_capacity + 4;
    static constexpr usize snapshot_capacity = 2 + 8 + 1 + journal_capacity + home_capacity;
    struct Snapshot { u32 block{}; const Block *image{}; };
    struct JournalReader
    {
        const std::array<Block, journal_capacity> &images;
        usize count;
        [[nodiscard]] Status read_block(u32 index, std::span<u8> destination)
        {
            if (index >= count || destination.size() != sizeof(Block))
                return Status::invalid_argument("regular move read outside sealed journal");
            for (usize byte = 0; byte < destination.size(); ++byte) destination[byte] = images[index][byte];
            return {};
        }
    };

  public:
    Ext4RegularMoveRecoveryBinding() = default;
    Ext4RegularMoveRecoveryBinding(const Ext4RegularMoveRecoveryBinding &) = delete;
    Ext4RegularMoveRecoveryBinding &operator=(const Ext4RegularMoveRecoveryBinding &) = delete;
    Ext4RegularMoveRecoveryBinding(Ext4RegularMoveRecoveryBinding &&) = delete;
    Ext4RegularMoveRecoveryBinding &operator=(Ext4RegularMoveRecoveryBinding &&) = delete;

    [[nodiscard]] const Ext4RecoveryInfo *info() const { return admitted_ ? generic_.info() : nullptr; }
    [[nodiscard]] const Ext4RegularMoveReplayValidation *validation() const
    { return admitted_ ? &validation_ : nullptr; }
    [[nodiscard]] const Ext4RegularMovePlan *canonical_plan() const
    { return admitted_ ? &scratch_.canonical : nullptr; }
    [[nodiscard]] usize sealed_snapshot_count() const { return admitted_ ? snapshot_count_ : 0; }

    // One read-only attempt, also supported on read-only devices. No flush,
    // write, family fallback, preimage guessing or mount publication occurs.
    [[nodiscard]] Status inspect(driver::BlockDevice &device, Ext4RecoveryBounds bounds)
    {
        if (attempted_) return Status::already_exists("regular move recovery binding is one-shot");
        attempted_ = true; device_ = &device; bounds_ = bounds; geometry_ = device.geometry();
        if (geometry_.block_size != 512)
            return Status::unsupported("regular move recovery requires 512-byte sectors");
        // Public generic admission owns geometry, head, checksum, allocation and
        // internal-journal extent checks; it never parses a move family.
        if (auto status = generic_.inspect(device, bounds); !status) return status;
        const auto &binding = *generic_.info();
        if (!binding.needs_recovery || binding.empty_log || binding.layout.block_size != 1024 ||
            binding.layout.filesystem_blocks != 32768 || binding.journal_inode != 8 ||
            binding.metadata_count != 16 || binding.layout.journal_first_block < 3 ||
            binding.layout.journal_first_block > 0xffffffffULL)
            return Status::unsupported("regular move recovery bounded active profile");
        journal_first_ = u32(binding.layout.journal_first_block);
        if (auto status = observe(1, super_); !status) return status;
        if (auto status = observe(2, descriptors_); !status) return status;
        for (u32 group = 0; group < 4; ++group)
        {
            const usize at = group * 32;
            block_bitmap_blocks_[group] = ext4_checked::le32(descriptors_, at);
            inode_bitmap_blocks_[group] = ext4_checked::le32(descriptors_, at + 4);
            if (auto status = observe(block_bitmap_blocks_[group], block_bitmaps_[group]); !status) return status;
            if (auto status = observe(inode_bitmap_blocks_[group], inode_bitmaps_[group]); !status) return status;
        }
        journal_table_block_ = ext4_checked::le32(descriptors_, 8) + 1; // inode 8, slot 3.
        if (auto status = observe(journal_table_block_, journal_table_); !status) return status;
        if (auto status = journal_identity(binding); !status) return status;
        if (auto status = observe(journal_first_, journal_[0]); !status) return status;
        auto codec = jbd2::Codec::open(journal_[0], binding.layout.journal_blocks, binding.uuid,
                                     jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!codec) return codec.status();
        const auto &head = codec.value().superblock();
        if (head.sequence != binding.journal_sequence || head.sequence > 0xfffffffdU ||
            head.block_size != 1024 || head.max_blocks != binding.layout.journal_blocks ||
            head.first_block != 1 || head.start_block != 1 ||
            (head.features != jbd2::feature_checksum_v3 &&
             head.features != (jbd2::feature_revoke | jbd2::feature_checksum_v3)))
            return Status::unsupported("regular move recovery ring, sequence or feature profile");
        if (auto status = observe(journal_first_ + 1, journal_[1]); !status) return status;
        if (!Ext4RegularMoveWitnessCodec::present(journal_[1]))
            return Status::unsupported("regular move recovery requires explicit OKMVW001 witness");
        usize tags{};
        if (auto status = codec.value().descriptor(journal_[1], head.sequence,
            [&](const jbd2::Tag &) -> Status {
                if (tags == home_capacity) return Status::unsupported("regular move recovery tag capacity");
                ++tags; return {};
            }); !status) return status;
        if (tags < 3 || tags + 3 >= head.max_blocks)
            return Status::unsupported("regular move recovery tag count");
        journal_count_ = tags + 4; // Head, descriptor, payloads, commit, exact zero end.
        for (usize index = 2; index < journal_count_; ++index)
            if (auto status = observe(journal_first_ + u32(index), journal_[index]); !status) return status;
        if (!jbd2::detail::all_zero(journal_[journal_count_ - 1]))
            return Status::unsupported("regular move recovery requires exact zero journal boundary");

        // Directory-data homes lie outside generic structural metadata. This
        // broader bounded read domain grants no replay authority: the strict
        // semantic validator below must authenticate every canonical target.
        std::array<jbd2::BlockRange, 2> ranges{};
        usize range_count{};
        if (journal_first_ > 3) ranges[range_count++] = {3, journal_first_ - 3};
        const u64 journal_end = binding.layout.journal_first_block + binding.layout.journal_blocks;
        if (journal_end < binding.layout.filesystem_blocks)
            ranges[range_count++] = {journal_end, binding.layout.filesystem_blocks - journal_end};
        JournalReader reader{journal_, journal_count_};
        auto replay = jbd2::build_replay_plan(codec.value(), reader,
            jbd2::TargetDomain{binding.layout.filesystem_blocks,
                std::span<const jbd2::BlockRange>{ranges}.first(range_count)},
            jbd2::PlanStorage{record_, payload_, actions_, revocations_});
        if (!replay) return replay.status();
        if (replay.value().actions.size() != tags)
            return Status::unsupported("regular move recovery parsed action count");
        for (usize index = 0; index < tags; ++index)
        {
            if (replay.value().actions[index].target_block > 0xffffffffULL)
                return Status::unsupported("regular move recovery home address width");
            if (auto status = observe(u32(replay.value().actions[index].target_block), homes_[index]); !status) return status;
        }
        Ext4OrphanChainProof proof{};
        const Ext4OrphanChainSource orphan_source{super_, descriptors_, inode_bitmaps_, block_bitmaps_, {}};
        if (auto status = Ext4OrphanChainValidator::validate(binding, orphan_source, proof); !status) return status;
        if (proof.count() || proof.observed_head())
            return Status::unsupported("regular move recovery requires independently empty orphan chain");
        orphan_ = {proof.filesystem_uuid(), proof.observed_head(), true};
        const Ext4RegularMoveReplayAncillary ancillary{super_, descriptors_, inode_bitmaps_, block_bitmaps_, orphan_};
        if (auto status = Ext4RegularMoveReplayValidator::validate(binding, codec.value(), replay.value(), journal_[1],
                std::span<const Block>{journal_}.subspan(2, tags), std::span<const Block>{homes_}.first(tags),
                ancillary, scratch_, validation_); !status) return status;
        empty_head_ = journal_[0];
        store(empty_head_, 24, head.sequence + 2); store(empty_head_, 28, 0); store(empty_head_, 252, 0);
        store(empty_head_, 252, jbd2::detail::crc32c(~u32{0}, empty_head_));
        auto empty = jbd2::Codec::open(empty_head_, binding.layout.journal_blocks, binding.uuid,
                                     jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!empty || empty.value().superblock().start_block ||
            empty.value().superblock().sequence != head.sequence + 2)
            return Status::fault("regular move recovery constructed empty head");
        // Bookend every observed raw block, including ancillary/control blocks,
        // before admitting the immutable semantic report. External exclusivity
        // must continue beyond this check; it is not a concurrency primitive.
        if (auto status = recheck(); !status) return status;
        admitted_ = true;
        return {};
    }

  private:
    static void store(Block &image, usize at, u32 value)
    { for (usize byte = 0; byte < 4; ++byte) image[at + byte] = u8(value >> ((3 - byte) * 8)); }
    [[nodiscard]] Status stable() const
    {
        const auto now = device_->geometry();
        return now.block_size == geometry_.block_size && now.block_count == geometry_.block_count &&
            now.writable == geometry_.writable ? Status{} : Status::io_error("regular move recovery geometry changed");
    }
    [[nodiscard]] Status bounded(u32 block) const
    {
        const auto *binding = generic_.info();
        if (!binding || block >= binding->layout.filesystem_blocks ||
            u64{block} * 2 >= bounds_.sector_count || bounds_.sector_count - u64{block} * 2 < 2)
            return Status::invalid_argument("regular move recovery block outside admitted bounds");
        return stable();
    }
    [[nodiscard]] Status read(u32 block, Block &image) const
    {
        if (auto status = bounded(block); !status) return status;
        if (auto status = device_->read_blocks(bounds_.first_sector + u64{block} * 2,
                std::as_writable_bytes(std::span{image})); !status) return status;
        return stable();
    }
    [[nodiscard]] Status observe(u32 block, Block &image)
    {
        if (auto status = read(block, image); !status) return status;
        for (usize index = 0; index < snapshot_count_; ++index)
            if (snapshots_[index].block == block)
                return *snapshots_[index].image == image ? Status{} :
                    Status::io_error("regular move recovery duplicate snapshot differs");
        if (snapshot_count_ == snapshots_.size()) return Status::no_memory("regular move recovery snapshot capacity");
        snapshots_[snapshot_count_++] = {block, &image};
        return {};
    }
    [[nodiscard]] Status recheck()
    {
        if (auto status = stable(); !status) return status;
        for (usize index = 0; index < snapshot_count_; ++index)
        {
            if (auto status = read(snapshots_[index].block, probe_); !status) return status;
            if (probe_ != *snapshots_[index].image)
                return Status::io_error("regular move recovery exact sealed snapshot changed");
        }
        return stable();
    }
    [[nodiscard]] Status journal_identity(const Ext4RecoveryInfo &binding) const
    {
        using namespace ext4_checked;
        const auto inode = std::span<const u8>{journal_table_}.subspan(3 * 256, 256);
        const u32 seed = jbd2::detail::crc32c(~u32{0}, binding.uuid);
        if (auto status = inode_checksum(seed, 8, inode); !status) return status;
        if (le16(inode, 0) != 0100600 || le16(inode, 2) || le16(inode, 24) || le16(inode, 26) != 1 ||
            le32(inode, 20) || le32(inode, 32) != 0x80000 || le32(inode, 104) || le32(inode, 108) ||
            le32(inode, 112) || le16(inode, 116) || le16(inode, 118) || le16(inode, 120) || le16(inode, 122) ||
            le16(inode, 40) != 0xf30a || le16(inode, 42) != 1 || le16(inode, 44) != 4 || le16(inode, 46) ||
            le32(inode, 52) || le16(inode, 58) || le16(inode, 56) != binding.layout.journal_blocks ||
            le32(inode, 60) != binding.layout.journal_first_block ||
            le32(inode, 4) != binding.layout.journal_blocks * 1024 ||
            le32(inode, 28) != binding.layout.journal_blocks * 2)
            return Status::io_error("regular move recovery current journal inode differs from binding");
        return {};
    }

    bool attempted_{}, admitted_{};
    driver::BlockDevice *device_{};
    driver::BlockGeometry geometry_{};
    Ext4RecoveryBounds bounds_{};
    Ext4RecoveryBinding generic_{};
    Block super_{}, descriptors_{}, journal_table_{}, empty_head_{}, record_{}, payload_{}, probe_{};
    std::array<Block, 4> inode_bitmaps_{}, block_bitmaps_{};
    std::array<Block, journal_capacity> journal_{};
    std::array<Block, home_capacity> homes_{};
    std::array<u32, 4> inode_bitmap_blocks_{}, block_bitmap_blocks_{};
    u32 journal_first_{}, journal_table_block_{};
    usize journal_count_{}, snapshot_count_{};
    std::array<Snapshot, snapshot_capacity> snapshots_{};
    std::array<jbd2::ReplayAction, home_capacity> actions_{};
    std::array<jbd2::Revocation, 1> revocations_{};
    Ext4RegularMoveEmptyOrphanAuthorization orphan_{};
    Ext4RegularMoveReplayScratch scratch_{};
    Ext4RegularMoveReplayValidation validation_{};
};
} // namespace ok::fs
