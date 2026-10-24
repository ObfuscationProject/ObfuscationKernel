#pragma once

#include "ok/fs/ext4_metadata_transaction.hpp"
#include "ok/fs/ext4_rename_plan.hpp"

namespace ok::fs
{
struct Ext4RenameUpdate
{
    u32 parent{}, source{}, seconds{}, nanoseconds{};
    std::string_view old_name{}, new_name{};
    bool source_directory{};
};

// A private mounted-dirty journal writer. The owner, not this object, controls
// the device session, canonical buffers, publication epoch and VFS visibility.
class Ext4RenameTransaction final
{
    template <usize, usize> friend class Ext4OwnedTransaction;
    using Block = std::array<u8, 1024>;
    using Completion = Ext4RecoveryCompletion<4, 2>;
    class ProposedDevice final : public driver::BlockDevice
    {
      public:
        explicit ProposedDevice(Ext4RenameTransaction &owner) : owner_(owner) {}
        driver::BlockGeometry geometry() const override { return owner_.device_->geometry(); }
        Status read_blocks(u64 sector, std::span<std::byte> out) override
        {
            if (auto status = owner_.device_->read_blocks(sector, out); !status) return status;
            if (sector < owner_.bounds_.first_sector || out.size() % 512)
                return Status::invalid_argument("rename proposed read bounds");
            for (usize offset = 0; offset < out.size(); offset += 512)
            {
                const u64 relative = sector - owner_.bounds_.first_sector + offset / 512;
                const auto *image = owner_.proposed(relative / 2);
                if (image)
                    for (usize byte = 0; byte < 512; ++byte)
                        out[offset + byte] = std::byte((*image)[(relative % 2) * 512 + byte]);
            }
            return {};
        }
        Status write_blocks(u64, std::span<const std::byte>) override
        { return Status::denied("rename proposed view is read-only"); }
        Status flush() override { return Status::denied("rename proposed view cannot flush"); }
      private:
        Ext4RenameTransaction &owner_;
    };

  public:
    Ext4RenameTransaction() : clean_(true, true),
        proposed_(false, true, Ext4ReplayPolicy::directory_rename) {}
    Ext4RenameTransaction(const Ext4RenameTransaction &) = delete;
    Ext4RenameTransaction &operator=(const Ext4RenameTransaction &) = delete;
    [[nodiscard]] const Ext4TransactionReport &report() const { return report_; }

  private:
    [[nodiscard]] Status prepare(driver::BlockDevice &device, Ext4RecoveryBounds bounds, Ext4RenameUpdate update)
    {
        if (report_.phase != Ext4TransactionPhase::idle)
            return Status::already_exists("rename transaction is one-shot");
        report_.phase = Ext4TransactionPhase::validating;
        device_ = &device; bounds_ = bounds; geometry_ = device.geometry();
        if (update.seconds > 0x7fffffffU || update.nanoseconds >= 1000000000U ||
            update.parent == update.source || update.parent < 2 || update.source <= 10)
            return fail(Status::invalid_argument("rename timestamp or inode identity"));
        if (auto status = clean_.prepare(device, bounds); !status) return fail(status);
        if (clean_.report().already_clean || !clean_.report().empty_log)
            return fail(Status::unsupported("rename requires dirty empty journal"));
        auto &preview = clean_.preview_;
        auto &view = preview.view_;
        const auto &super = preview.codec_->superblock();
        if (super.first_block != 1 || super.max_blocks < 8 || super.sequence > 0xfffffffcU)
            return fail(Status::unsupported("rename journal sequence or ring capacity"));
        auto existing = preview.lookup(update.parent, update.old_name);
        if (!existing) return fail(existing.status());
        if (existing.value() != update.source)
            return fail(Status::unsupported("rename source inode differs from named entry"));
        auto absent = preview.lookup(update.parent, update.new_name);
        if (absent || absent.status().code() != StatusCode::not_found)
            return fail(absent ? Status::already_exists("rename destination exists") : absent.status());
        std::array<u8, 256> parent{}, source{};
        if (auto status = view.decode_inode(update.parent, parent); !status) return fail(status);
        if (auto status = view.decode_inode(update.source, source); !status) return fail(status);
        if (!(ext4_checked::le16(parent, 0) & 0200) || !(ext4_checked::le16(parent, 0) & 0100))
            return fail(Status::unsupported("rename parent lacks owner write/search bits"));
        const auto physical = view.unique_initialized_directory_block(update.parent);
        if (!physical) return fail(physical.status());
        directory_block_ = physical.value();
        std::array<u8, 1024> before{};
        if (auto status = preview.raw_block(directory_block_, before); !status) return fail(status);
        if (update.source_directory)
        {
            const auto source_physical = view.unique_initialized_directory_block(update.source);
            if (!source_physical) return fail(source_physical.status());
            source_directory_block_ = source_physical.value();
            if (auto status = preview.raw_block(source_directory_block_, source_directory_before_); !status)
                return fail(status);
            expected_source_type_ = 0040000;
        }
        const auto changed = update.source_directory ?
            Ext4DirectoryRenamePlanner::plan_directory(*preview.binding_.info(),
                update.parent, parent, update.source, source, directory_block_, before,
                source_directory_block_, source_directory_before_, update.old_name, update.new_name) :
            Ext4DirectoryRenamePlanner::plan(*preview.binding_.info(),
                update.parent, parent, update.source, source, directory_block_, before,
                update.old_name, update.new_name);
        if (!changed) return fail(changed.status());
        const auto table_for = [&](u32 number) -> u32 {
            const u32 group = (number - 1) / view.inodes_per_group_;
            if (group >= view.group_count_) return 0;
            return view.groups_[group].table + ((number - 1) % view.inodes_per_group_) / 4;
        };
        const u32 parent_table = table_for(update.parent), source_table = table_for(update.source);
        if (!parent_table || !source_table || parent_table == directory_block_ ||
            source_table == directory_block_)
            return fail(Status::unsupported("rename inode-table placement"));
        target_[0] = parent_table;
        target_[1] = source_table == parent_table ? directory_block_ : source_table;
        home_count_ = source_table == parent_table ? 2 : 3;
        target_[home_count_ - 1] = directory_block_;
        for (usize index = 0; index < home_count_ - 1; ++index)
            if (auto status = preview.raw_block(target_[index], home_[index]); !status) return fail(status);
        before_[home_count_ - 1] = changed.value().before;
        for (usize index = 0; index < home_count_ - 1; ++index)
            before_[index] = home_[index];
        home_[home_count_ - 1] = changed.value().after;
        const usize parent_index = 0, source_index = source_table == parent_table ? 0 : 1;
        if (auto status = update_inode(home_[parent_index], update.parent, update, true,
                                       view.checksum_seed()); !status) return fail(status);
        if (auto status = update_inode(home_[source_index], update.source, update, false,
                                       view.checksum_seed()); !status) return fail(status);
        journal_before_ = journal_ = clean_.journal_;
        journal_first_ = u32(preview.layout().journal_first_block);
        report_.sequence = super.sequence + 1;
        report_.inode = update.source; report_.target_block = directory_block_;
        header(descriptor_, 1); header(commit_, 2);
        const u32 seed = view.checksum_seed();
        for (usize index = 0; index < home_count_; ++index)
        {
            payload_[index] = home_[index];
            const u32 flags = (index ? jbd2::flag_same_uuid : 0) |
                              (index + 1 == home_count_ ? jbd2::flag_last_tag : 0);
            tag(index == 0 ? 12 : 44 + (index - 1) * 16, target_[index], flags, payload_[index], seed);
        }
        Completion::be32(descriptor_, 1020, jbd2::detail::crc32c(seed, descriptor_));
        Completion::be32(commit_, 52, update.seconds);
        Completion::be32(commit_, 56, update.nanoseconds);
        Completion::be32(commit_, 16, jbd2::detail::crc32c(seed, commit_));
        Completion::be32(journal_, 24, report_.sequence);
        Completion::be32(journal_, 28, 1);
        Completion::be32(journal_, 252, 0);
        Completion::be32(journal_, 252, jbd2::detail::crc32c(~u32{0}, journal_));
        if (auto status = proposed_.prepare(proposed_device_, bounds); !status) return fail(status);
        if (proposed_.report().validated_actions != home_count_ || proposed_.report().empty_log)
            return fail(Status::fault("rename proposed replay shape"));
        report_.phase = Ext4TransactionPhase::preflight;
        return {};
    }
    [[nodiscard]] Status logical_images(std::array<u64, 4> &blocks,
                                         std::array<Block, 4> &images, usize &count) const
    {
        if (report_.phase != Ext4TransactionPhase::preflight)
            return Status::bad_descriptor("rename has no prepared image");
        blocks[0] = journal_first_; images[0] = journal_;
        for (usize index = 0; index < home_count_; ++index)
        { blocks[index + 1] = target_[index]; images[index + 1] = home_[index]; }
        count = home_count_ + 1;
        return {};
    }
    [[nodiscard]] Status publish_prepared()
    {
        if (report_.phase != Ext4TransactionPhase::preflight)
            return Status::bad_descriptor("rename is not prepared");
        // Compare the exact validated preimages before the first flush or write.
        if (auto status = unchanged(journal_first_, journal_before_); !status) return fail(status);
        for (usize index = 0; index < home_count_; ++index)
            if (auto status = unchanged(target_[index], before_[index]); !status) return fail(status);
        if (source_directory_block_)
            if (auto status = unchanged(source_directory_block_, source_directory_before_); !status) return fail(status);
        if (auto status = flush(); !status) return fail(status);
        report_.phase = Ext4TransactionPhase::payloads;
        if (auto status = write(journal_first_ + 1, descriptor_); !status) return fail(status);
        for (usize index = 0; index < home_count_; ++index)
            if (auto status = write(journal_first_ + u32(index) + 2, payload_[index]); !status)
                return fail(status);
        if (auto status = write(journal_first_ + u32(home_count_) + 3, end_); !status) return fail(status);
        report_.phase = Ext4TransactionPhase::payload_flush;
        if (auto status = flush(); !status) return fail(status);
        report_.payloads_durable = true;
        report_.phase = Ext4TransactionPhase::commit_record;
        if (auto status = write(journal_first_ + u32(home_count_) + 2, commit_); !status) return fail(status);
        report_.phase = Ext4TransactionPhase::commit_flush;
        if (auto status = flush(); !status) return fail(status);
        report_.commit_record_durable = true;
        report_.recovery_bit_durable = true; // The mounted owner entered dirty.
        report_.phase = Ext4TransactionPhase::publication;
        if (auto status = write(journal_first_, journal_); !status) return fail(status);
        report_.phase = Ext4TransactionPhase::publication_flush;
        if (auto status = flush(); !status) return fail(status);
        report_.publication_durable = true; report_.phase = Ext4TransactionPhase::published;
        return {};
    }
    [[nodiscard]] Status update_inode(Block &block, u32 number, Ext4RenameUpdate update,
                                      bool parent, u32 seed) const
    {
        auto inode = std::span<u8>{block}.subspan(((number - 1) % 4) * 256, 256);
        const u16 type = ext4_checked::le16(inode, 0) & 0170000;
        if (type != (parent ? 0040000 : expected_source_type_))
            return Status::unsupported("rename inode-table slot type");
        const u64 version = u64{ext4_checked::le32(inode, 36)} |
                            (u64{ext4_checked::le32(inode, 152)} << 32);
        if (version == ~u64{0}) return Status::overflow("rename inode version exhausted");
        Completion::le32(inode, 12, update.seconds);
        Completion::le32(inode, 132, update.nanoseconds << 2);
        if (parent)
        {
            Completion::le32(inode, 16, update.seconds);
            Completion::le32(inode, 136, update.nanoseconds << 2);
        }
        Completion::le32(inode, 36, u32(version + 1));
        Completion::le32(inode, 152, u32((version + 1) >> 32));
        inode[124] = inode[125] = inode[130] = inode[131] = 0;
        const u32 crc = jbd2::detail::crc32c(ext4_checked::inode_seed(seed, number, inode), inode);
        inode[124] = u8(crc); inode[125] = u8(crc >> 8);
        inode[130] = u8(crc >> 16); inode[131] = u8(crc >> 24);
        return {};
    }
    [[nodiscard]] const Block *proposed(u64 block) const
    {
        if (block == journal_first_) return &journal_;
        if (block == u64{journal_first_} + 1) return &descriptor_;
        for (usize index = 0; index < home_count_; ++index)
            if (block == u64{journal_first_} + index + 2) return &payload_[index];
        if (block == u64{journal_first_} + home_count_ + 2) return &commit_;
        if (block == u64{journal_first_} + home_count_ + 3) return &end_;
        return nullptr;
    }
    void header(Block &block, u32 type)
    {
        Completion::be32(block, 0, jbd2::magic);
        Completion::be32(block, 4, type);
        Completion::be32(block, 8, report_.sequence);
    }
    void tag(usize at, u32 target, u32 flags, Block &payload, u32 seed)
    {
        if (jbd2::detail::be32(payload, 0) == jbd2::magic)
        { Completion::be32(payload, 0, 0); flags |= jbd2::flag_escape; }
        Completion::be32(descriptor_, at, target);
        Completion::be32(descriptor_, at + 4, flags);
        const auto sequence = std::span<const u8>{descriptor_}.subspan(8, 4);
        Completion::be32(descriptor_, at + 12,
            jbd2::detail::crc32c(jbd2::detail::crc32c(seed, sequence), payload));
    }
    [[nodiscard]] Status stable() const
    {
        const auto now = device_->geometry();
        return now.block_size == geometry_.block_size && now.block_count == geometry_.block_count &&
               now.writable == geometry_.writable ? Status{} : Status::io_error("rename device changed");
    }
    [[nodiscard]] Status unchanged(u32 block, const Block &before)
    {
        if (auto status = stable(); !status) return status;
        Block current{};
        if (auto status = device_->read_blocks(bounds_.first_sector + u64{block} * 2,
                std::as_writable_bytes(std::span{current})); !status) return status;
        if (auto status = stable(); !status) return status;
        return current == before ? Status{} : Status::io_error("rename preimage changed before publication");
    }
    [[nodiscard]] Status write(u32 block, const Block &bytes)
    {
        if (auto status = stable(); !status) return status;
        ++report_.write_attempts;
        if (auto status = device_->write_blocks(bounds_.first_sector + u64{block} * 2,
                std::as_bytes(std::span{bytes})); !status) return status;
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
        report_.failed_phase = report_.phase;
        report_.phase = Ext4TransactionPhase::failed;
        report_.status = status;
        return status;
    }

    driver::BlockDevice *device_{};
    Ext4RecoveryBounds bounds_{};
    driver::BlockGeometry geometry_{};
    Completion clean_{}, proposed_{};
    ProposedDevice proposed_device_{*this};
    Ext4TransactionReport report_{};
    u32 journal_first_{}, directory_block_{};
    u32 source_directory_block_{};
    u16 expected_source_type_{0100000};
    Block source_directory_before_{}, journal_before_{};
    std::array<u32, 3> target_{};
    usize home_count_{};
    Block journal_{}, descriptor_{}, commit_{}, end_{};
    std::array<Block, 3> before_{}, home_{}, payload_{};
};
} // namespace ok::fs
