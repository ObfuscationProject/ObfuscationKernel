#pragma once

#include "ok/fs/ext4_metadata_transaction.hpp"
#include "ok/fs/ext4_regular_move_journal.hpp"
#include "ok/fs/ext4_orphan_chain.hpp"

namespace ok::fs
{
// A separate, one-shot writer over caller-exclusive quiesced media. The public
// source-prototype API permits ordinary hosted models; it is not device/session
// authority, VFS admission, permission policy or cache publication. No legacy
// namespace/rename completion policy is selected. An owner must separately
// validate strict move recovery before checkpointing any home image.
class Ext4RegularMoveTransaction final
{
    template <usize, usize> friend class Ext4OwnedTransaction;
    using Block = Ext4RegularMoveBlock;
    static constexpr usize home_capacity = Ext4RegularMovePlan::capacity;

  public:
    Ext4RegularMoveTransaction() = default;
    Ext4RegularMoveTransaction(const Ext4RegularMoveTransaction &) = delete;
    Ext4RegularMoveTransaction &operator=(const Ext4RegularMoveTransaction &) = delete;
    Ext4RegularMoveTransaction(Ext4RegularMoveTransaction &&) = delete;
    Ext4RegularMoveTransaction &operator=(Ext4RegularMoveTransaction &&) = delete;
    [[nodiscard]] const Ext4TransactionReport &report() const { return report_; }
    [[nodiscard]] const Ext4RecoveryInfo *binding() const { return binding_.info(); }
    [[nodiscard]] const Ext4RegularMoveJournalPlan &journal_plan() const { return journal_; }
    [[nodiscard]] const Ext4RegularMovePlan &canonical_plan() const { return scratch_.regular_move_plan; }

    // Read-only preparation. Planner decoding reads these exact raw inode-table
    // slots, and every mutated slot is in its corresponding complete preimage.
    [[nodiscard]] Status prepare(driver::BlockDevice &device, Ext4RecoveryBounds bounds,
                                 Ext4RegularMoveUpdate update)
    {
        if (report_.phase != Ext4TransactionPhase::idle)
            return Status::already_exists("regular move writer is one-shot");
        if (overlap(this, sizeof(*this), update.old_name.data(), update.old_name.size()) ||
            overlap(this, sizeof(*this), update.new_name.data(), update.new_name.size()))
            return Status::invalid_argument("regular move writer names alias state");
        report_.phase = Ext4TransactionPhase::validating;
        device_ = &device; bounds_ = bounds; geometry_ = device.geometry();
        if (!geometry_.writable || geometry_.block_size != 512)
            return fail(Status::unsupported("regular move writer requires writable 512-byte sectors"));
        if (update.from_parent < 2 || update.from_parent > 8192 || update.to_parent < 2 ||
            update.to_parent > 8192 || update.source < 11 || update.source > 8192 ||
            update.from_parent == update.to_parent || update.source == update.from_parent ||
            update.source == update.to_parent)
            return fail(Status::invalid_argument("regular move writer inode identity"));
        if (auto status = binding_.inspect(device, bounds); !status) return fail(status);
        const auto &binding = *binding_.info();
        if (!binding.needs_recovery || !binding.empty_log || binding.layout.block_size != 1024 ||
            binding.layout.filesystem_blocks != 32768 || binding.layout.journal_first_block > 0xffffffffULL)
            return fail(Status::unsupported("regular move writer requires bounded dirty empty journal"));
        journal_first_ = u32(binding.layout.journal_first_block);
        if (auto status = read(1, super_); !status) return fail(status);
        if (auto status = read(2, descriptors_); !status) return fail(status);
        if (auto status = read(journal_first_, old_head_); !status) return fail(status);
        for (u32 group = 0; group < 4; ++group)
        {
            const usize offset = group * 32;
            block_bitmap_blocks_[group] = ext4_checked::le32(descriptors_, offset);
            inode_bitmap_blocks_[group] = ext4_checked::le32(descriptors_, offset + 4);
            if (auto status = read(block_bitmap_blocks_[group], block_bitmaps_[group]); !status) return fail(status);
            if (auto status = read(inode_bitmap_blocks_[group], inode_bitmaps_[group]); !status) return fail(status);
        }
        // Preserve the raw internal-journal inode home too: its checked mapping
        // is an ancillary input, even when it is not one of the mutated tables.
        journal_table_block_ = table_for(8);
        if (!journal_table_block_) return fail(Status::unsupported("regular move journal inode placement"));
        if (auto status = read(journal_table_block_, journal_table_); !status) return fail(status);
        if (auto status = journal_identity(binding); !status) return fail(status);
        Ext4OrphanChainProof proof{};
        const Ext4OrphanChainSource orphan_source{super_, descriptors_, inode_bitmaps_, block_bitmaps_, {}};
        if (auto status = Ext4OrphanChainValidator::validate(binding, orphan_source, proof); !status) return fail(status);
        if (proof.count() || proof.observed_head())
            return fail(Status::unsupported("regular move writer requires independently empty orphan chain"));
        orphan_ = {proof.filesystem_uuid(), proof.observed_head(), true};

        from_table_block_ = table_for(update.from_parent);
        to_table_block_ = table_for(update.to_parent);
        source_table_block_ = table_for(update.source);
        if (!from_table_block_ || !to_table_block_ || !source_table_block_)
            return fail(Status::unsupported("regular move writer inode-table placement"));
        if (auto status = read(from_table_block_, from_table_); !status) return fail(status);
        if (to_table_block_ == from_table_block_) to_table_ = from_table_;
        else if (auto status = read(to_table_block_, to_table_); !status) return fail(status);
        if (source_table_block_ == from_table_block_) source_table_ = from_table_;
        else if (source_table_block_ == to_table_block_) source_table_ = to_table_;
        else if (auto status = read(source_table_block_, source_table_); !status) return fail(status);
        from_directory_block_ = ext4_checked::le32(from_table_, slot(update.from_parent) + 60);
        to_directory_block_ = ext4_checked::le32(to_table_, slot(update.to_parent) + 60);
        if (auto status = read(from_directory_block_, from_directory_); !status) return fail(status);
        if (auto status = read(to_directory_block_, to_directory_); !status) return fail(status);
        const Ext4RegularMoveSource source{from_table_block_, to_table_block_, source_table_block_,
            from_directory_block_, to_directory_block_, super_, descriptors_, from_table_, to_table_, source_table_,
            from_directory_, to_directory_, inode_bitmaps_, block_bitmaps_, orphan_};
        // This repeats both complete directory checks, source lookup, destination
        // absence, allocation/generation and raw slot decoding before readiness.
        if (auto status = Ext4RegularMoveJournalPlanner::plan(binding, update, source, old_head_, scratch_, journal_); !status)
            return fail(status);
        if (!journal_.ready || journal_.count < 3 || journal_.count > home_capacity)
            return fail(Status::fault("regular move writer constructed journal shape"));
        // A shared journal-table/home must denote one identical complete image.
        for (usize index = 0; index < journal_.count; ++index)
            if (journal_.targets[index] == journal_table_block_ && scratch_.regular_move_plan.before[index] != journal_table_)
                return fail(Status::io_error("regular move journal inode/table snapshot differs"));
        report_.sequence = journal_.sequence; report_.inode = update.source;
        report_.target_block = from_directory_block_; report_.phase = Ext4TransactionPhase::preflight;
        return {};
    }

    template <usize Capacity> requires (Capacity >= home_capacity + 1)
    [[nodiscard]] Status logical_images(std::array<u64, Capacity> &blocks,
        std::array<Block, Capacity> &images, usize &count) const
    {
        if (report_.phase != Ext4TransactionPhase::preflight)
            return Status::bad_descriptor("regular move writer has no prepared images");
        if (overlap(this, sizeof(*this), &blocks, sizeof(blocks)) ||
            overlap(this, sizeof(*this), &images, sizeof(images)) || overlap(this, sizeof(*this), &count, sizeof(count)) ||
            overlap(&blocks, sizeof(blocks), &images, sizeof(images)) ||
            overlap(&blocks, sizeof(blocks), &count, sizeof(count)) || overlap(&images, sizeof(images), &count, sizeof(count)))
            return Status::invalid_argument("regular move logical images alias state");
        if (!journal_.count || journal_.count > home_capacity)
            return Status::fault("regular move logical image count exceeds admitted homes");
        blocks[0] = journal_first_; images[0] = journal_.journal_head;
        for (usize index = 0; index < home_capacity && index < journal_.count; ++index)
        { blocks[index + 1] = journal_.targets[index]; images[index + 1] = scratch_.regular_move_plan.after[index]; }
        count = journal_.count + 1;
        return {};
    }

    [[nodiscard]] const Block *original_preimage(u64 block) const
    {
        if (report_.phase != Ext4TransactionPhase::preflight) return nullptr;
        if (block == journal_first_) return &old_head_;
        for (usize index = 0; index < journal_.count; ++index)
            if (block == journal_.targets[index]) return &scratch_.regular_move_plan.before[index];
        return nullptr;
    }

    [[nodiscard]] Status publish_prepared()
    {
        if (report_.phase != Ext4TransactionPhase::preflight)
            return Status::bad_descriptor("regular move writer is not prepared");
        // Compare every full snapshot before even the initial flush. This
        // requires the caller's continuing exclusive session; it is not a lock.
        if (auto status = unchanged(journal_first_, old_head_); !status) return fail(status);
        if (auto status = unchanged(1, super_); !status) return fail(status);
        if (auto status = unchanged(2, descriptors_); !status) return fail(status);
        if (auto status = unchanged(journal_table_block_, journal_table_); !status) return fail(status);
        for (usize group = 0; group < 4; ++group)
        {
            if (auto status = unchanged(inode_bitmap_blocks_[group], inode_bitmaps_[group]); !status) return fail(status);
            if (auto status = unchanged(block_bitmap_blocks_[group], block_bitmaps_[group]); !status) return fail(status);
        }
        for (usize index = 0; index < journal_.count; ++index)
            if (auto status = unchanged(journal_.targets[index], scratch_.regular_move_plan.before[index]); !status) return fail(status);
        if (auto status = flush(); !status) return fail(status);
        report_.phase = Ext4TransactionPhase::payloads;
        if (auto status = write(journal_first_ + 1, journal_.descriptor); !status) return fail(status);
        for (usize index = 0; index < journal_.count; ++index)
            if (auto status = write(journal_first_ + u32(index) + 2, journal_.payloads[index]); !status) return fail(status);
        if (auto status = write(journal_first_ + u32(journal_.count) + 3, journal_.end); !status) return fail(status);
        report_.phase = Ext4TransactionPhase::payload_flush;
        if (auto status = flush(); !status) return fail(status);
        report_.payloads_durable = true; report_.phase = Ext4TransactionPhase::commit_record;
        if (auto status = write(journal_first_ + u32(journal_.count) + 2, journal_.commit); !status) return fail(status);
        report_.phase = Ext4TransactionPhase::commit_flush;
        if (auto status = flush(); !status) return fail(status);
        report_.commit_record_durable = true; report_.recovery_bit_durable = true;
        report_.phase = Ext4TransactionPhase::publication;
        if (auto status = write(journal_first_, journal_.journal_head); !status) return fail(status);
        report_.phase = Ext4TransactionPhase::publication_flush;
        if (auto status = flush(); !status) return fail(status);
        report_.publication_durable = true; report_.phase = Ext4TransactionPhase::published;
        return {};
    }

    // Equality with what this writer published, never recovery/checkpoint proof.
    [[nodiscard]] Status matches_published()
    {
        if (!report_.published()) return Status::bad_descriptor("regular move writer is not durably published");
        if (auto status = unchanged(journal_first_, journal_.journal_head); !status) return fail(status);
        if (auto status = unchanged(journal_first_ + 1, journal_.descriptor); !status) return fail(status);
        for (usize index = 0; index < journal_.count; ++index)
            if (auto status = unchanged(journal_first_ + u32(index) + 2, journal_.payloads[index]); !status) return fail(status);
        if (auto status = unchanged(journal_first_ + u32(journal_.count) + 2, journal_.commit); !status) return fail(status);
        if (auto status = unchanged(journal_first_ + u32(journal_.count) + 3, journal_.end); !status) return fail(status);
        return {};
    }

  private:
    [[nodiscard]] static bool overlap(const void *one, usize a, const void *two, usize b)
    {
        if (!a || !b) return false;
        const usize x = reinterpret_cast<usize>(one), y = reinterpret_cast<usize>(two);
        return (x >= y && x - y < b) || (y >= x && y - x < a);
    }
    [[nodiscard]] static usize slot(u32 inode) { return ((inode - 1) % 4) * 256; }
    [[nodiscard]] u32 table_for(u32 inode) const
    {
        if (!inode || inode > 8192) return 0;
        const u32 first = ext4_checked::le32(descriptors_, ((inode - 1) / 2048) * 32 + 8);
        const u32 offset = ((inode - 1) % 2048) / 4;
        return first <= 32767 - offset ? first + offset : 0;
    }
    [[nodiscard]] Status journal_identity(const Ext4RecoveryInfo &binding) const
    {
        using namespace ext4_checked;
        const auto inode = std::span<const u8>{journal_table_}.subspan(slot(8), 256);
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
            return Status::io_error("regular move current internal journal inode differs from binding");
        return {};
    }
    [[nodiscard]] Status stable() const
    {
        const auto now = device_->geometry();
        return now.block_size == geometry_.block_size && now.block_count == geometry_.block_count &&
            now.writable == geometry_.writable ? Status{} : Status::io_error("regular move device geometry changed");
    }
    [[nodiscard]] Status bounded(u32 block) const
    {
        const auto *info = binding_.info();
        if (!info || block >= info->layout.filesystem_blocks ||
            u64{block} * 2 >= bounds_.sector_count || bounds_.sector_count - u64{block} * 2 < 2)
            return Status::invalid_argument("regular move block outside admitted bounds");
        return stable();
    }
    [[nodiscard]] Status read(u32 block, Block &image) const
    {
        if (auto status = bounded(block); !status) return status;
        if (auto status = device_->read_blocks(bounds_.first_sector + u64{block} * 2,
                std::as_writable_bytes(std::span{image})); !status) return status;
        return stable();
    }
    [[nodiscard]] Status unchanged(u32 block, const Block &before) const
    {
        Block current{};
        if (auto status = read(block, current); !status) return status;
        return current == before ? Status{} : Status::io_error("regular move exact current preimage differs");
    }
    [[nodiscard]] Status write(u32 block, const Block &image)
    {
        if (auto status = bounded(block); !status) return status;
        ++report_.write_attempts;
        if (auto status = device_->write_blocks(bounds_.first_sector + u64{block} * 2,
                std::as_bytes(std::span{image})); !status) return status;
        ++report_.completed_writes;
        return stable();
    }
    [[nodiscard]] Status flush()
    {
        if (auto status = stable(); !status) return status;
        ++report_.flush_attempts;
        if (auto status = device_->flush(); !status) return status;
        ++report_.completed_flushes;
        return stable();
    }
    [[nodiscard]] Status fail(Status status)
    {
        report_.failed_phase = report_.phase; report_.phase = Ext4TransactionPhase::failed;
        report_.status = status; return status;
    }
    driver::BlockDevice *device_{};
    Ext4RecoveryBounds bounds_{};
    driver::BlockGeometry geometry_{};
    Ext4RecoveryBinding binding_{};
    Ext4RegularMoveJournalScratch scratch_{};
    Ext4RegularMoveJournalPlan journal_{};
    Ext4TransactionReport report_{};
    u32 journal_first_{}, journal_table_block_{}, from_table_block_{}, to_table_block_{}, source_table_block_{};
    u32 from_directory_block_{}, to_directory_block_{};
    Block old_head_{}, super_{}, descriptors_{}, journal_table_{}, from_table_{}, to_table_{}, source_table_{};
    Block from_directory_{}, to_directory_{};
    std::array<Block, 4> inode_bitmaps_{}, block_bitmaps_{};
    std::array<u32, 4> inode_bitmap_blocks_{}, block_bitmap_blocks_{};
    Ext4RegularMoveEmptyOrphanAuthorization orphan_{};
};
} // namespace ok::fs
