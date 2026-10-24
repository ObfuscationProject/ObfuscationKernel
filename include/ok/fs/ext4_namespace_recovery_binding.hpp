#pragma once

#include "ok/fs/ext4_namespace_cold_replay.hpp"

namespace ok::fs
{
enum class Ext4NamespaceRecoveryPhase : u8
{
    idle, bootstrap, journal, before_binding, verifying, ready, failed,
};
struct Ext4NamespaceRecoveryReport
{
    Ext4NamespaceRecoveryPhase phase{Ext4NamespaceRecoveryPhase::idle};
    Status status{};
    usize count{}, old_homes{}, new_homes{};
    u32 active_sequence{}, parsed_next_sequence{}, linux_cleanup_sequence{};
    bool recognized_marker{}, old_binding_validated{}, durable_witness{}, ready{};
};

// One read-only cold admission against an owner-exclusive block session. The
// owner supplies lifetime, epoch, registration and cache exclusion. No device
// write, FLUSH, mount publication or checkpoint authority is provided here.
class Ext4NamespaceRecoveryBinding final
{
    template <usize, usize> friend class Ext4OwnedTransaction;
    friend class Ext4NamespaceResultHistory;
    using Block = Ext4NamespaceBlock;
    static constexpr usize capacity = Ext4NamespacePlan::capacity;
    struct JournalReader
    {
        Ext4NamespaceRecoveryBinding &owner;
        [[nodiscard]] Status read_block(u32 number, std::span<u8> bytes)
        {
            if (number >= owner.journal_blocks_ || bytes.size() != 1024)
                return Status::invalid_argument("namespace recovery journal reader bounds");
            return owner.read_block(owner.journal_first_ + number, bytes);
        }
    };
    class BeforeDevice final : public driver::BlockDevice
    {
      public:
        explicit BeforeDevice(Ext4NamespaceRecoveryBinding &owner) : owner_(owner) {}
        [[nodiscard]] driver::BlockGeometry geometry() const override { return owner_.device_->geometry(); }
        [[nodiscard]] Status read_blocks(u64 sector, std::span<std::byte> bytes) override
        {
            if (auto status = owner_.stable(); !status) return status;
            if (sector < owner_.bounds_.first_sector || bytes.empty() || bytes.size() % 512)
                return Status::invalid_argument("namespace before overlay read bounds");
            const u64 relative = sector - owner_.bounds_.first_sector;
            if (relative >= owner_.bounds_.sector_count ||
                bytes.size() / 512 > owner_.bounds_.sector_count - relative)
                return Status::invalid_argument("namespace before overlay partition bounds");
            if (auto status = owner_.device_->read_blocks(sector, bytes); !status) return status;
            for (usize offset = 0; offset < bytes.size(); offset += 512)
            {
                const u64 within = relative + offset / 512;
                const u64 block = within / 2;
                const Block *image = block == owner_.journal_first_ ? &owner_.old_head_ : nullptr;
                if (!image)
                    for (usize index = 0; index < owner_.count_; ++index)
                        if (owner_.targets_[index] == block) { image = &owner_.before_[index]; break; }
                if (image)
                    for (usize byte = 0; byte < 512; ++byte)
                        bytes[offset + byte] = std::byte((*image)[(within % 2) * 512 + byte]);
            }
            return owner_.stable();
        }
        [[nodiscard]] Status write_blocks(u64, std::span<const std::byte>) override
        { return Status::denied("namespace before overlay is read-only"); }
        [[nodiscard]] Status flush() override
        { return Status::denied("namespace before overlay cannot flush"); }
      private:
        Ext4NamespaceRecoveryBinding &owner_;
    };

  public:
    Ext4NamespaceRecoveryBinding() = default;
    Ext4NamespaceRecoveryBinding(const Ext4NamespaceRecoveryBinding &) = delete;
    Ext4NamespaceRecoveryBinding &operator=(const Ext4NamespaceRecoveryBinding &) = delete;
    Ext4NamespaceRecoveryBinding(Ext4NamespaceRecoveryBinding &&) = delete;
    Ext4NamespaceRecoveryBinding &operator=(Ext4NamespaceRecoveryBinding &&) = delete;
    [[nodiscard]] const Ext4NamespaceRecoveryReport &report() const { return report_; }
    [[nodiscard]] bool ready() const { return report_.ready; }
    [[nodiscard]] const Ext4NamespaceColdResult *proof() const { return ready() ? &proof_ : nullptr; }
    [[nodiscard]] const Ext4NamespaceColdResult *result() const { return proof(); }
    [[nodiscard]] const Ext4NamespaceColdScratch *scratch() const
    { return ready() ? &cold_scratch_ : nullptr; }
    [[nodiscard]] const Ext4RecoveryInfo *info() const
    { return ready() ? &active_info_ : nullptr; }
    [[nodiscard]] const Ext4RecoveryInfo *old_binding() const
    { return ready() ? old_binding_.info() : nullptr; }
    [[nodiscard]] const Block &current_head() const { return head_; }
    [[nodiscard]] const Block &journal_head() const { return head_; }
    [[nodiscard]] const Block &old_head() const { return old_head_; }
    [[nodiscard]] usize count() const { return ready() ? count_ : 0; }
    [[nodiscard]] u32 journal_first() const { return ready() ? journal_first_ : 0; }
    [[nodiscard]] u32 journal_sequence() const { return ready() ? report_.active_sequence : 0; }
    [[nodiscard]] u32 parsed_next_sequence() const { return ready() ? report_.parsed_next_sequence : 0; }
    [[nodiscard]] u32 linux_cleanup_sequence() const { return ready() ? report_.linux_cleanup_sequence : 0; }
    [[nodiscard]] u32 target(usize index) const { return ready() && index < count_ ? targets_[index] : 0; }
    [[nodiscard]] const Block *before(usize index) const
    { return ready() && index < count_ ? &before_[index] : nullptr; }
    [[nodiscard]] const Block *after(usize index) const
    { return ready() && index < count_ ? &after_[index] : nullptr; }
    [[nodiscard]] const Block *current(usize index) const
    { return ready() && index < count_ ? &current_[index] : nullptr; }
    [[nodiscard]] Status read_old_sectors(u64 relative, std::span<std::byte> bytes)
    {
        if (!ready()) return Status::not_initialized("namespace before overlay is unavailable");
        if (relative >= bounds_.sector_count || bytes.empty() || bytes.size() % 512 ||
            bytes.size() / 512 > bounds_.sector_count - relative)
            return Status::invalid_argument("namespace before overlay read range");
        return before_device_.read_blocks(bounds_.first_sector + relative, bytes);
    }
    [[nodiscard]] Status stable() const
    {
        if (!device_) return Status::not_initialized("namespace recovery device absent");
        const auto now = device_->geometry();
        return now.block_size == geometry_.block_size && now.block_count == geometry_.block_count &&
            now.writable == geometry_.writable ? Status{} :
            Status::io_error("namespace recovery device geometry changed");
    }

    [[nodiscard]] Status prepare(driver::BlockDevice &device, Ext4RecoveryBounds bounds)
    {
        if (report_.phase != Ext4NamespaceRecoveryPhase::idle)
            return Status::already_exists("namespace recovery binding is one-shot");
        device_ = &device; geometry_ = device.geometry(); bounds_ = bounds;
        report_.phase = Ext4NamespaceRecoveryPhase::bootstrap;
        if (!geometry_.writable || geometry_.block_size != 512 ||
            bounds.first_sector >= geometry_.block_count || bounds.sector_count < 65536 ||
            bounds.sector_count > geometry_.block_count - bounds.first_sector)
            return fail(Status::invalid_argument("namespace recovery session geometry or bounds"));
        if (auto status = read_block(1, super_); !status) return fail(status);
        if (ext4_checked::le16(super_, 56) != 0xef53 ||
            jbd2::detail::crc32c(~u32{0}, std::span<const u8>{super_}.first(1020)) !=
                ext4_checked::le32(super_, 1020) ||
            ext4_checked::le32(super_, 24) || ext4_checked::le32(super_, 4) != 32768 ||
            ext4_checked::le32(super_, 0) != 8192 || ext4_checked::le32(super_, 20) != 1 ||
            ext4_checked::le32(super_, 32) != 8192 || ext4_checked::le32(super_, 40) != 2048 ||
            ext4_checked::le16(super_, 88) != 256 || ext4_checked::le32(super_, 92) != 4 ||
            ext4_checked::le32(super_, 96) != 0x46 || ext4_checked::le32(super_, 100) != 0x400 ||
            ext4_checked::le32(super_, 224) != 8 || super_[373] != 1)
            return fail(Status::unsupported("namespace recovery bootstrap superblock"));
        for (usize index = 0; index < uuid_.size(); ++index) uuid_[index] = super_[104 + index];
        if (auto status = read_block(2, descriptors_); !status) return fail(status);
        const u32 seed = jbd2::detail::crc32c(~u32{0}, uuid_);
        Block group_copy = descriptors_;
        const u16 expected = ext4_checked::le16(group_copy, 30);
        group_copy[30] = group_copy[31] = 0;
        if (u16(jbd2::detail::crc32c(jbd2::detail::crc32c(seed, ext4_checked::encoded(0)),
                std::span<const u8>{group_copy}.first(32))) != expected)
            return fail(Status::io_error("namespace recovery group-zero descriptor checksum"));
        const u32 table = ext4_checked::le32(descriptors_, 8);
        if (table < 3 || table >= 32768 - 512)
            return fail(Status::invalid_argument("namespace recovery journal table bounds"));
        if (auto status = read_block(table + 1, table_block_); !status) return fail(status);
        const auto inode = std::span<const u8>{table_block_}.subspan(3 * 256, 256);
        if (auto status = ext4_checked::inode_checksum(seed, 8, inode); !status) return fail(status);
        journal_blocks_ = ext4_checked::le16(inode, 56);
        journal_first_ = ext4_checked::le32(inode, 60);
        if (ext4_checked::le16(inode, 0) != 0100600 || ext4_checked::le16(inode, 26) != 1 ||
            ext4_checked::le32(inode, 20) || ext4_checked::le32(inode, 32) != 0x80000 ||
            ext4_checked::le16(inode, 40) != 0xf30a || ext4_checked::le16(inode, 42) != 1 ||
            ext4_checked::le16(inode, 44) != 4 || ext4_checked::le16(inode, 46) ||
            ext4_checked::le32(inode, 52) || ext4_checked::le16(inode, 58) ||
            journal_blocks_ < 1024 || journal_blocks_ > 32768 ||
            journal_first_ < 3 || journal_first_ >= 32768 ||
            journal_blocks_ > 32768 - journal_first_ ||
            ext4_checked::le32(inode, 4) != journal_blocks_ * 1024 ||
            ext4_checked::le32(inode, 28) != journal_blocks_ * 2)
            return fail(Status::unsupported("namespace recovery contiguous journal inode"));
        report_.phase = Ext4NamespaceRecoveryPhase::journal;
        if (auto status = read_block(journal_first_, head_); !status) return fail(status);
        const auto codec = jbd2::Codec::open(head_, journal_blocks_, uuid_,
            jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!codec) return fail(codec.status());
        const auto &journal = codec.value().superblock();
        if (journal.block_size != 1024 || journal.max_blocks != journal_blocks_ ||
            journal.first_block != 1 || journal.start_block != 1 ||
            journal.sequence < 2 || journal.sequence > 0xfffffffdU ||
            (journal.features != jbd2::feature_checksum_v3 &&
             journal.features != (jbd2::feature_revoke | jbd2::feature_checksum_v3)))
            return fail(Status::unsupported("namespace recovery active journal head profile"));
        report_.active_sequence = journal.sequence;
        if (auto status = read_block(journal_first_ + 1, descriptor_); !status) return fail(status);
        if (!Ext4NamespaceWitnessCodec::present(descriptor_))
            return fail(Status::unsupported("namespace recovery durable descriptor marker absent"));
        report_.recognized_marker = true;
        if (auto status = codec.value().descriptor(descriptor_, journal.sequence,
                [&](const jbd2::Tag &tag) -> Status {
                    if (count_ == capacity || tag.target_block > 0xffffffffULL ||
                        tag.target_block >= 32768 ||
                        (tag.target_block >= journal_first_ &&
                         tag.target_block - journal_first_ < journal_blocks_))
                        return Status::invalid_argument("namespace recovery target capacity or bounds");
                    for (usize prior = 0; prior < count_; ++prior)
                        if (targets_[prior] == tag.target_block)
                            return Status::io_error("namespace recovery duplicate home target");
                    targets_[count_++] = u32(tag.target_block);
                    return {};
                }); !status) return fail(status);
        if (count_ < 2 || count_ + 3 >= journal_blocks_)
            return fail(Status::unsupported("namespace recovery action count or ring"));
        for (usize index = 0; index < count_; ++index)
            if (auto status = read_block(journal_first_ + u32(index) + 2, payloads_[index]); !status)
                return fail(status);
        if (auto status = read_block(journal_first_ + u32(count_) + 2, commit_); !status)
            return fail(status);
        if (auto status = read_block(journal_first_ + u32(count_) + 3, end_); !status)
            return fail(status);
        const u64 journal_end = u64{journal_first_} + journal_blocks_;
        const std::array<jbd2::BlockRange, 2> allowed{{{0, journal_first_},
            {journal_end, 32768 - journal_end}}};
        JournalReader reader{*this};
        const auto parsed = jbd2::build_replay_plan(codec.value(), reader,
            {32768, std::span<const jbd2::BlockRange>{allowed}.first(journal_end < 32768 ? 2 : 1)},
            {record_scratch_, payload_scratch_, actions_, revocations_});
        if (!parsed) return fail(parsed.status());
        const auto &replay = parsed.value();
        if (replay.empty_log || replay.incomplete_tail || replay.committed_transactions != 1 ||
            replay.revoked_actions || replay.revoke_records || replay.actions.size() != count_ ||
            replay.next_sequence != journal.sequence + 1 || !jbd2::detail::all_zero(end_))
            return fail(Status::io_error("namespace recovery journal transaction grammar"));
        for (usize index = 0; index < count_; ++index)
        {
            const auto &action = replay.actions[index];
            if (action.target_block != targets_[index] || action.journal_block != index + 2 ||
                action.sequence != journal.sequence || action.transaction_order)
                return fail(Status::io_error("namespace recovery journal action order"));
            after_[index] = payloads_[index];
            if (action.restore_magic)
                for (usize byte = 0; byte < 4; ++byte)
                    after_[index][byte] = u8(jbd2::magic >> (24 - byte * 8));
        }
        Ext4NamespaceWitness witness{};
        if (auto status = Ext4NamespaceWitnessCodec::decode(descriptor_,
                std::span<const Block>{after_}.first(count_),
                std::span<Block>{before_}.first(count_), witness); !status) return fail(status);
        old_head_ = head_;
        big32(old_head_, 24, journal.sequence - 1);
        big32(old_head_, 28, 0); big32(old_head_, 252, 0);
        big32(old_head_, 252, jbd2::detail::crc32c(~u32{0}, old_head_));

        report_.phase = Ext4NamespaceRecoveryPhase::before_binding;
        if (auto status = old_binding_.inspect(before_device_, bounds); !status) return fail(status);
        const auto &old = *old_binding_.info();
        if (!old.needs_recovery || !old.empty_log || old.journal_sequence != journal.sequence - 1 ||
            old.uuid != uuid_ || old.layout.first_sector != bounds.first_sector ||
            old.layout.block_size != 1024 || old.layout.filesystem_blocks != 32768 ||
            old.layout.journal_first_block != journal_first_ || old.layout.journal_blocks != journal_blocks_)
            return fail(Status::io_error("namespace recovery before binding differs from bootstrap"));
        report_.old_binding_validated = true;
        report_.phase = Ext4NamespaceRecoveryPhase::verifying;
        for (u32 group = 0; group < 4; ++group)
        {
            const usize offset = group * 32;
            if (auto status = read_block(ext4_checked::le32(descriptors_, offset),
                    block_bitmaps_[group]); !status) return fail(status);
            if (auto status = read_block(ext4_checked::le32(descriptors_, offset + 4),
                    inode_bitmaps_[group]); !status) return fail(status);
        }
        for (usize index = 0; index < count_; ++index)
            if (auto status = read_block(targets_[index], current_[index]); !status) return fail(status);
        Ext4RecoveryInfo active = old;
        active.empty_log = false; active.journal_sequence = journal.sequence;
        const Ext4NamespaceReplayRecords records{head_, descriptor_,
            std::span<const Block>{payloads_}.first(count_), commit_, end_};
        const Ext4NamespaceColdAncillary ancillary{super_, descriptors_, inode_bitmaps_, block_bitmaps_};
        const Ext4NamespaceReplayHomes homes{std::span<const u32>{targets_}.first(count_),
            std::span<const Block>{current_}.first(count_)};
        if (auto status = Ext4NamespaceColdReplayVerifier::verify(active, records, ancillary,
                homes, cold_scratch_, proof_); !status) return fail(status);
        if (!proof_.durable_old_witness || proof_.homes.count != count_ ||
            cold_scratch_.old_head != old_head_ ||
            cold_scratch_.canonical.journal_head != head_ ||
            cold_scratch_.canonical.count != count_)
            return fail(Status::io_error("namespace recovery before overlay is incomplete"));
        for (usize index = 0; index < count_; ++index)
            if (targets_[index] != cold_scratch_.canonical.targets[index] ||
                before_[index] != cold_scratch_.before[index] ||
                after_[index] != cold_scratch_.after[index])
                return fail(Status::io_error("namespace recovery canonical overlay differs"));
        if (auto status = stable(); !status) return fail(status);
        report_.count = count_; report_.old_homes = proof_.homes.old_homes;
        report_.new_homes = proof_.homes.new_homes;
        report_.parsed_next_sequence = replay.next_sequence;
        report_.linux_cleanup_sequence = replay.next_sequence + 1;
        active_info_ = active;
        report_.durable_witness = true; report_.ready = true;
        report_.phase = Ext4NamespaceRecoveryPhase::ready;
        return {};
    }
    [[nodiscard]] Status inspect(driver::BlockDevice &device, Ext4RecoveryBounds bounds)
    { return prepare(device, bounds); }

  private:
    static void big32(Block &bytes, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - index * 8)); }
    [[nodiscard]] Status read_block(u32 block, std::span<u8> image) const
    {
        if (block >= 32768 || image.size() != 1024 || u64{block} * 2 + 2 > bounds_.sector_count)
            return Status::invalid_argument("namespace recovery block read bounds");
        if (auto status = stable(); !status) return status;
        if (auto status = device_->read_blocks(bounds_.first_sector + u64{block} * 2,
                std::as_writable_bytes(image)); !status) return status;
        return stable();
    }
    [[nodiscard]] Status fail(Status status)
    {
        report_.phase = Ext4NamespaceRecoveryPhase::failed;
        report_.status = status; report_.ready = false;
        proof_ = {};
        return status;
    }

    driver::BlockDevice *device_{};
    driver::BlockGeometry geometry_{};
    Ext4RecoveryBounds bounds_{};
    BeforeDevice before_device_{*this};
    Ext4RecoveryBinding old_binding_{};
    Ext4NamespaceRecoveryReport report_{};
    Ext4NamespaceColdScratch cold_scratch_{};
    Ext4NamespaceColdResult proof_{};
    Ext4RecoveryInfo active_info_{};
    std::array<u8, 16> uuid_{};
    u32 journal_first_{}, journal_blocks_{};
    usize count_{};
    std::array<u32, capacity> targets_{};
    std::array<Block, capacity> payloads_{}, before_{}, after_{}, current_{};
    Block super_{}, descriptors_{}, table_block_{}, head_{}, old_head_{}, descriptor_{}, commit_{}, end_{};
    std::array<Block, 4> inode_bitmaps_{}, block_bitmaps_{};
    Block record_scratch_{}, payload_scratch_{};
    std::array<jbd2::ReplayAction, capacity> actions_{};
    std::array<jbd2::Revocation, 1> revocations_{};
};
using Ext4NamespaceRecoveryAdmission = Ext4NamespaceRecoveryBinding;
} // namespace ok::fs
