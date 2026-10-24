#pragma once

#include "ok/fs/ext4_metadata_transaction.hpp"
#include "ok/fs/ext4_first_data_plan.hpp"

namespace ok::fs
{
// Only the canonical owner can publish this checked first allocation. The owner
// seals and writes/flushes the full initialized data image before publication.
class Ext4FirstDataTransaction final
{
    template <usize, usize> friend class Ext4OwnedTransaction;
    using Block = std::array<u8, 1024>;
    static constexpr usize home_capacity = 4;
    using Completion = Ext4RecoveryCompletion<home_capacity, 2>;
    class ProposedDevice final : public driver::BlockDevice
    {
      public:
        explicit ProposedDevice(Ext4FirstDataTransaction &owner) : owner_(owner) {}
        driver::BlockGeometry geometry() const override { return owner_.device_->geometry(); }
        Status read_blocks(u64 sector, std::span<std::byte> out) override
        {
            if (auto status = owner_.device_->read_blocks(sector, out); !status) return status;
            if (sector < owner_.bounds_.first_sector || out.size() % 512)
                return Status::invalid_argument("first data proposed read bounds");
            for (usize offset = 0; offset < out.size(); offset += 512)
            {
                const u64 relative = sector - owner_.bounds_.first_sector + offset / 512;
                if (const auto *image = owner_.proposed(relative / 2))
                    for (usize byte = 0; byte < 512; ++byte)
                        out[offset + byte] = std::byte((*image)[(relative % 2) * 512 + byte]);
            }
            return {};
        }
        Status write_blocks(u64, std::span<const std::byte>) override
        { return Status::denied("first data proposed view is read-only"); }
        Status flush() override { return Status::denied("first data proposed view cannot flush"); }
      private:
        Ext4FirstDataTransaction &owner_;
    };

  public:
    Ext4FirstDataTransaction() : clean_(true, true),
        proposed_(false, true, Ext4ReplayPolicy::first_data_allocation) {}
    Ext4FirstDataTransaction(const Ext4FirstDataTransaction &) = delete;
    Ext4FirstDataTransaction &operator=(const Ext4FirstDataTransaction &) = delete;
    [[nodiscard]] const Ext4TransactionReport &report() const { return report_; }

  private:
    [[nodiscard]] Status prepare(driver::BlockDevice &device, Ext4RecoveryBounds bounds,
                                  Ext4FirstDataUpdate update, u32 offset, const Block &data_image)
    {
        if (report_.phase != Ext4TransactionPhase::idle)
            return Status::already_exists("first data transaction is one-shot");
        report_.phase = Ext4TransactionPhase::validating;
        device_ = &device; bounds_ = bounds; geometry_ = device.geometry();
        if (!update.size || update.size > 1024 || offset >= update.size ||
            update.seconds > 0x7fffffffU || update.nanoseconds >= 1000000000U)
            return fail(Status::invalid_argument("first data range or timestamp"));
        // The caller supplies only [offset, size); every other byte must be
        // initialized to zero, including the sparse gap and bytes beyond EOF.
        for (usize byte = 0; byte < data_image.size(); ++byte)
            if ((byte < offset || byte >= update.size) && data_image[byte])
                return fail(Status::invalid_argument("first data image is not fully zero-filled"));
        if (auto status = clean_.prepare(device, bounds); !status) return fail(status);
        if (clean_.report().already_clean || !clean_.report().empty_log)
            return fail(Status::unsupported("first data requires dirty empty journal"));
        auto &preview = clean_.preview_;
        auto &view = preview.view_;
        const auto &super = preview.codec_->superblock();
        if (super.first_block != 1 || super.sequence > 0xfffffffcU || super.max_blocks < 8)
            return fail(Status::unsupported("first data journal ring or sequence"));
        const auto inode = preview.inode(update.inode);
        if (!inode) return fail(inode.status());
        if (update.inode < view.first_ordinary_inode() || inode.value().mode >> 12 != 8 ||
            inode.value().size || inode.value().links != 1)
            return fail(Status::unsupported("first data target is not an allocated empty regular inode"));
        if (auto status = preview.raw_block(1, source_super_); !status) return fail(status);
        if (auto status = preview.raw_block(2, source_descriptors_); !status) return fail(status);
        u32 selected{}, selected_group{};
        bool unsupported_space{};
        for (u32 group = 0; group < view.group_count_; ++group)
        {
            const u16 free_blocks = ext4_checked::le16(source_descriptors_, group * 32 + 12);
            if (!free_blocks) continue;
            if (view.groups_[group].flags & 2) { unsupported_space = true; continue; }
            if (auto status = preview.raw_block(view.groups_[group].block_bitmap, source_bitmap_); !status)
                return fail(status);
            const auto candidate = Ext4FirstDataPlanner::select_block(*preview.binding_.info(),
                source_super_, source_descriptors_, source_bitmap_, group);
            if (!candidate) return fail(candidate.status());
            selected = candidate.value(); selected_group = group; break;
        }
        if (!selected)
            return fail(unsupported_space ? Status::unsupported("first data free blocks require uninitialized group support") :
                Status::no_space("first data supported block space is exhausted"));
        // A clear bit alone cannot authorize overwriting a block. Scan every
        // allocated record (including reserved records) before any data I/O.
        const auto unreferenced = view.unreferenced_free_data_block(selected);
        if (!unreferenced) return fail(unreferenced.status());
        if (unreferenced.value() != selected) return fail(Status::fault("first data ownership identity"));
        const u32 inode_group = (update.inode - 1) / view.inodes_per_group_;
        if (inode_group >= view.group_count_) return fail(Status::unsupported("first data inode group"));
        const u32 table = view.groups_[inode_group].table +
            ((update.inode - 1) % view.inodes_per_group_) / 4;
        if (auto status = preview.raw_block(table, source_inode_); !status) return fail(status);
        const Ext4FirstDataSource source{selected_group, selected, table,
            source_super_, source_descriptors_, source_bitmap_, source_inode_};
        if (auto status = Ext4FirstDataPlanner::plan(*preview.binding_.info(), update, source, plan_); !status)
            return fail(status);
        if (plan_.count != home_capacity) return fail(Status::fault("first data participant count"));
        home_count_ = home_capacity;
        journal_ = clean_.journal_;
        journal_first_ = u32(preview.layout().journal_first_block);
        data_ = data_image;
        report_.sequence = super.sequence + 1;
        report_.inode = update.inode; report_.target_block = table;
        header(descriptor_, 1); header(commit_, 2);
        const u32 seed = view.checksum_seed();
        for (usize index = 0; index < home_count_; ++index)
        {
            payload_[index] = plan_.after[index];
            const u32 flags = (index ? jbd2::flag_same_uuid : 0) |
                (index + 1 == home_count_ ? jbd2::flag_last_tag : 0);
            tag(index ? 44 + (index - 1) * 16 : 12, plan_.targets[index], flags, payload_[index], seed);
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
            return fail(Status::fault("first data proposed replay shape"));
        report_.phase = Ext4TransactionPhase::preflight;
        return {};
    }
    template <usize Capacity> requires (Capacity >= 7)
    [[nodiscard]] Status logical_images(std::array<u64, Capacity> &blocks,
        std::array<Block, Capacity> &images, usize &count) const
    {
        if (report_.phase != Ext4TransactionPhase::preflight)
            return Status::bad_descriptor("first data has no prepared image");
        blocks[0] = journal_first_; images[0] = journal_;
        for (usize index = 0; index < home_count_; ++index)
        { blocks[index + 1] = plan_.targets[index]; images[index + 1] = plan_.after[index]; }
        blocks[home_count_ + 1] = plan_.physical; images[home_count_ + 1] = data_;
        count = home_count_ + 2; return {};
    }
    [[nodiscard]] Status publish_prepared()
    {
        if (report_.phase != Ext4TransactionPhase::preflight)
            return Status::bad_descriptor("first data is not prepared");
        if (auto status = flush(); !status) return fail(status);
        report_.phase = Ext4TransactionPhase::payloads;
        if (auto status = write(journal_first_ + 1, descriptor_); !status) return fail(status);
        for (usize index = 0; index < home_count_; ++index)
            if (auto status = write(journal_first_ + u32(index) + 2, payload_[index]); !status) return fail(status);
        if (auto status = write(journal_first_ + u32(home_count_) + 3, end_); !status) return fail(status);
        report_.phase = Ext4TransactionPhase::payload_flush;
        if (auto status = flush(); !status) return fail(status);
        report_.payloads_durable = true;
        report_.phase = Ext4TransactionPhase::commit_record;
        if (auto status = write(journal_first_ + u32(home_count_) + 2, commit_); !status) return fail(status);
        report_.phase = Ext4TransactionPhase::commit_flush;
        if (auto status = flush(); !status) return fail(status);
        report_.commit_record_durable = true; report_.recovery_bit_durable = true;
        report_.phase = Ext4TransactionPhase::publication;
        if (auto status = write(journal_first_, journal_); !status) return fail(status);
        report_.phase = Ext4TransactionPhase::publication_flush;
        if (auto status = flush(); !status) return fail(status);
        report_.publication_durable = true; report_.phase = Ext4TransactionPhase::published;
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
    void header(Block &bytes, u32 type)
    {
        Completion::be32(bytes, 0, jbd2::magic);
        Completion::be32(bytes, 4, type);
        Completion::be32(bytes, 8, report_.sequence);
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
            now.writable == geometry_.writable ? Status{} : Status::io_error("first data device changed");
    }
    [[nodiscard]] Status write(u32 block, const Block &bytes)
    {
        if (auto status = stable(); !status) return status;
        ++report_.write_attempts;
        if (auto status = device_->write_blocks(bounds_.first_sector + u64{block} * 2,
            std::as_bytes(std::span{bytes})); !status) return status;
        ++report_.completed_writes; return stable();
    }
    [[nodiscard]] Status flush()
    {
        if (auto status = stable(); !status) return status;
        ++report_.flush_attempts;
        if (auto status = device_->flush(); !status) return status;
        ++report_.completed_flushes; return stable();
    }
    [[nodiscard]] Status fail(Status status)
    {
        report_.failed_phase = report_.phase; report_.phase = Ext4TransactionPhase::failed;
        report_.status = status; return status;
    }
    driver::BlockDevice *device_{};
    Ext4RecoveryBounds bounds_{};
    driver::BlockGeometry geometry_{};
    Completion clean_{}, proposed_{};
    ProposedDevice proposed_device_{*this};
    Ext4TransactionReport report_{};
    u32 journal_first_{};
    usize home_count_{};
    Ext4FirstDataPlan plan_{};
    std::array<Block, home_capacity> payload_{};
    Block source_super_{}, source_descriptors_{}, source_bitmap_{}, source_inode_{}, data_{};
    Block journal_{}, descriptor_{}, commit_{}, end_{};
};
} // namespace ok::fs
