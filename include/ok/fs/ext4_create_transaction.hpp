#pragma once

#include "ok/fs/ext4_metadata_transaction.hpp"
#include "ok/fs/ext4_create_plan.hpp"
#include "ok/fs/ext4_create_witness.hpp"
#include "ok/fs/ext4_directory_create_plan.hpp"
#include "ok/fs/ext4_directory_create_witness.hpp"
#include "ok/fs/ext4_first_data_plan.hpp"
#include <new>

namespace ok::fs
{
// Only the canonical owner can publish this checked mounted-dirty transaction.
class Ext4CreateTransaction final
{
    template <usize, usize> friend class Ext4OwnedTransaction;
    using Block = std::array<u8, 1024>;
    static constexpr usize home_capacity = 8;
    using Completion = Ext4RecoveryCompletion<home_capacity, 2>;
    class ProposedDevice final : public driver::BlockDevice
    {
      public:
        explicit ProposedDevice(Ext4CreateTransaction &owner) : owner_(owner) {}
        driver::BlockGeometry geometry() const override { return owner_.device_->geometry(); }
        Status read_blocks(u64 sector, std::span<std::byte> out) override
        {
            if (auto status = owner_.device_->read_blocks(sector, out); !status) return status;
            if (sector < owner_.bounds_.first_sector || out.size() % 512)
                return Status::invalid_argument("create proposed read bounds");
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
        { return Status::denied("create proposed view is read-only"); }
        Status flush() override { return Status::denied("create proposed view cannot flush"); }
      private:
        Ext4CreateTransaction &owner_;
    };

  public:
    Ext4CreateTransaction() : clean_(true, true),
        proposed_(false, true, Ext4ReplayPolicy::directory_create) {}
    Ext4CreateTransaction(const Ext4CreateTransaction &) = delete;
    Ext4CreateTransaction &operator=(const Ext4CreateTransaction &) = delete;
    [[nodiscard]] const Ext4TransactionReport &report() const { return report_; }

  private:
    [[nodiscard]] Status prepare(driver::BlockDevice &device, Ext4RecoveryBounds bounds, Ext4CreateUpdate update)
    {
        if (report_.phase != Ext4TransactionPhase::idle)
            return Status::already_exists("create transaction is one-shot");
        report_.phase = Ext4TransactionPhase::validating;
        device_ = &device; bounds_ = bounds; geometry_ = device.geometry();
        const bool directory_create = update.attributes.type == NodeType::directory;
        directory_create_ = directory_create;
        if (!directory_create && update.attributes.type != NodeType::regular)
            return fail(Status::unsupported("create requires a bounded regular file or directory"));
        if (update.seconds > 0x7fffffffU || update.nanoseconds >= 1000000000U || update.parent < 2)
            return fail(Status::invalid_argument("create timestamp or parent identity"));
        if (directory_create)
        {
            clean_.require_one_kib();
            // The directory planner supports exactly 1 KiB ext4 blocks. Probe
            // this before generic completion can inspect a larger-block volume;
            // the Owner reservation includes this one bounded physical READ.
            if (geometry_.block_size != 512 || bounds_.sector_count < 4)
                return fail(Status::unsupported("directory create requires bounded 512-byte sectors"));
            if (auto status = stable(); !status) return fail(status);
            if (auto status = device.read_blocks(bounds_.first_sector + 2,
                    std::as_writable_bytes(std::span{source_super_})); !status) return fail(status);
            if (auto status = stable(); !status) return fail(status);
            if (ext4_checked::le16(source_super_, 56) != 0xef53 || ext4_checked::le32(source_super_, 24) != 0)
                return fail(Status::unsupported("directory create requires 1 KiB ext4 blocks"));
        }
        if (auto status = clean_.prepare(device, bounds); !status) return fail(status);
        if (clean_.report().already_clean || !clean_.report().empty_log)
            return fail(Status::unsupported("create requires dirty empty journal"));
        auto &preview = clean_.preview_;
        auto &view = preview.view_;
        const auto &super = preview.codec_->superblock();
        if (super.first_block != 1 || super.sequence > 0xfffffffcU)
            return fail(Status::unsupported("create journal sequence or ring start"));
        auto exists = preview.lookup(update.parent, update.name);
        if (exists || exists.status().code() != StatusCode::not_found)
            return fail(exists ? Status::already_exists("create destination exists") : exists.status());
        const auto physical = view.unique_initialized_directory_block(update.parent);
        if (!physical) return fail(physical.status());
        const u32 directory_block = physical.value();
        if (auto status = preview.raw_block(1, source_super_); !status) return fail(status);
        if (auto status = preview.raw_block(2, source_descriptors_); !status) return fail(status);
        u32 selected_inode{}, selected_group{};
        bool unsupported_free_group{};
        for (u32 group = 0; group < view.group_count_; ++group)
        {
            // This slice does not initialize previously uninitialized groups.
            if (view.groups_[group].flags & 3)
            {
                unsupported_free_group |= ext4_checked::le16(source_descriptors_, group * 32 + 14) != 0;
                continue;
            }
            if (auto status = preview.raw_block(view.groups_[group].inode_bitmap, source_bitmap_); !status)
                return fail(status);
            auto inode = Ext4CreatePlanner::select_inode(*preview.binding_.info(), source_super_,
                source_descriptors_, source_bitmap_, group);
            if (!inode)
            {
                if (inode.status().code() == StatusCode::no_space) continue;
                return fail(inode.status());
            }
            selected_inode = inode.value(); selected_group = group; break;
        }
        if (!selected_inode)
            return fail(unsupported_free_group ?
                Status::unsupported("create free inode requires uninitialized group support") :
                Status::no_space("create ordinary inode allocation is exhausted"));
        const auto table_for = [&](u32 number) -> u32 {
            const u32 group = (number - 1) / view.inodes_per_group_;
            return group < view.group_count_ ? view.groups_[group].table +
                ((number - 1) % view.inodes_per_group_) / 4 : 0;
        };
        const u32 parent_table = table_for(update.parent), new_table = table_for(selected_inode);
        if (!parent_table || !new_table)
            return fail(Status::unsupported("create inode-table placement"));
        if (auto status = preview.raw_block(parent_table, source_parent_); !status) return fail(status);
        if (new_table == parent_table) source_new_ = source_parent_;
        else if (auto status = preview.raw_block(new_table, source_new_); !status) return fail(status);
        if (auto status = preview.raw_block(directory_block, source_directory_); !status) return fail(status);
        const Ext4CreateSource source{selected_inode, selected_group, parent_table, new_table, directory_block,
            source_super_, source_descriptors_, source_bitmap_, source_parent_, source_new_, source_directory_};
        source_journal_ = clean_.journal_;
        inode_group_ = selected_group; parent_table_ = parent_table;
        new_table_ = new_table; parent_block_ = directory_block;
        if (directory_create)
        {
            if (update.name.size() > candidate_name_.size() - 1)
                return fail(Status::unsupported("directory candidate bounded name"));
            for (usize index = 0; index < update.name.size(); ++index) candidate_name_[index] = update.name[index];
            candidate_update_ = update;
            candidate_update_.name = {candidate_name_.data(), update.name.size()};
            u32 selected_block{}, block_group{};
            bool unsupported_free_blocks{};
            for (u32 group = 0; group < view.group_count_; ++group)
            {
                if (!ext4_checked::le16(source_descriptors_, group * 32 + 12)) continue;
                if (view.groups_[group].flags & 2) { unsupported_free_blocks = true; continue; }
                if (auto status = preview.raw_block(view.groups_[group].block_bitmap, source_block_bitmap_); !status)
                    return fail(status);
                const auto block = Ext4FirstDataPlanner::select_block(*preview.binding_.info(),
                    source_super_, source_descriptors_, source_block_bitmap_, group);
                if (!block) return fail(block.status());
                selected_block = block.value(); block_group = group; break;
            }
            if (!selected_block)
                return fail(unsupported_free_blocks ?
                    Status::unsupported("directory create free blocks require initialized groups") :
                    Status::no_space("directory create supported block allocation is exhausted"));
            // A clear bitmap bit never authorizes overwriting an existing
            // reference, including one in a reserved allocated inode.
            const auto unreferenced = view.unreferenced_free_data_block(selected_block);
            if (!unreferenced) return fail(unreferenced.status());
            if (unreferenced.value() != selected_block)
                return fail(Status::fault("directory create free-block ownership identity"));
            if (auto status = preview.raw_block(selected_block, source_directory_data_); !status)
                return fail(status);
            candidate_block_ = selected_block; block_group_ = block_group;
            old_directory_data_ = source_directory_data_;
            const Ext4DirectoryCreateSource directory_source{source, block_group, selected_block,
                source_block_bitmap_, source_directory_data_};
            initialization_required_ = !jbd2::detail::all_zero(source_directory_data_);
            if (initialization_required_)
            {
                if (auto status = Ext4DirectoryCreatePlanner::admit_candidate(*preview.binding_.info(),
                        candidate_update_, directory_source, plan_); !status) return fail(status);
                if ((plan_.count != 7 && plan_.count != 8) || super.max_blocks < plan_.count + 4)
                    return fail(Status::unsupported("directory candidate journal participant or ring capacity"));
                home_count_ = plan_.count; journal_first_ = u32(preview.layout().journal_first_block);
                journal_ = source_journal_;
                report_.sequence = super.sequence + 1; report_.inode = plan_.inode;
                report_.target_block = plan_.directory_block;
                report_.phase = Ext4TransactionPhase::preflight;
                return {};
            }
            if (auto status = Ext4DirectoryCreatePlanner::plan(*preview.binding_.info(), candidate_update_,
                    directory_source, plan_); !status) return fail(status);
        }
        else if (auto status = Ext4CreatePlanner::plan(*preview.binding_.info(), update, source, plan_); !status)
            return fail(status);
        return build_publication(directory_create ? candidate_update_ : update, directory_create);
    }
    [[nodiscard]] Status build_publication(Ext4CreateUpdate update, bool directory_create)
    {
        auto &preview = clean_.preview_;
        auto &view = preview.view_;
        const auto &super = preview.codec_->superblock();
        if ((directory_create ? (plan_.count != 7 && plan_.count != 8) :
                                (plan_.count != 5 && plan_.count != 6)) ||
            super.max_blocks < plan_.count + 4)
            return fail(Status::unsupported("create journal participant or ring capacity"));
        home_count_ = plan_.count;
        journal_ = clean_.journal_;
        journal_first_ = u32(preview.layout().journal_first_block);
        report_.sequence = super.sequence + 1;
        report_.inode = plan_.inode; report_.target_block = plan_.directory_block;
        header(descriptor_, 1); header(commit_, 2);
        const u32 seed = view.checksum_seed();
        for (usize index = 0; index < home_count_; ++index)
        {
            payload_[index] = plan_.after[index];
            const u32 flags = (index ? jbd2::flag_same_uuid : 0) |
                (index + 1 == home_count_ ? jbd2::flag_last_tag : 0);
            tag(index ? 44 + (index - 1) * 16 : 12, plan_.targets[index], flags, payload_[index], seed);
        }
        const auto witness_status = directory_create ?
            Ext4DirectoryCreateWitnessCodec::encode(update, plan_, descriptor_) :
            Ext4CreateWitnessCodec::encode(update, plan_, descriptor_);
        if (!witness_status)
            return fail(witness_status);
        Completion::be32(descriptor_, 1020, jbd2::detail::crc32c(seed, descriptor_));
        Completion::be32(commit_, 52, update.seconds);
        Completion::be32(commit_, 56, update.nanoseconds);
        Completion::be32(commit_, 16, jbd2::detail::crc32c(seed, commit_));
        Completion::be32(journal_, 24, report_.sequence);
        Completion::be32(journal_, 28, 1);
        Completion::be32(journal_, 252, 0);
        Completion::be32(journal_, 252, jbd2::detail::crc32c(~u32{0}, journal_));
        if (directory_create)
        {
            proposed_.~Completion();
            ::new (static_cast<void *>(&proposed_)) Completion(
                false, true, Ext4ReplayPolicy::bounded_directory_create);
        }
        if (auto status = proposed_.prepare(proposed_device_, bounds_); !status) return fail(status);
        if (proposed_.report().validated_actions != home_count_ || proposed_.report().empty_log)
            return fail(Status::fault("create proposed replay shape"));
        report_.phase = Ext4TransactionPhase::preflight;
        return {};
    }

    // The old journal and allocation/namespace participants are immutable.
    // Only the proved-unallocated data preimage may become zero after its
    // successful ordered flush. Reads use the original bound Session.
    [[nodiscard]] const Block *original_preimage(u64 block) const
    {
        if (!directory_create_) return nullptr;
        if (block == journal_first_) return &source_journal_;
        if (block == candidate_block_) return &old_directory_data_;
        for (usize index = 0; index < home_count_; ++index)
            if (block == plan_.targets[index]) return &plan_.before[index];
        return nullptr;
    }
    [[nodiscard]] Status revalidate_directory(bool initialized, bool repeat_ownership = true)
    {
        if (!directory_create_ || report_.phase != Ext4TransactionPhase::preflight)
            return Status::bad_descriptor("directory candidate is not prepared");
        const auto compare = [&](u32 block, const Block &expected) -> Status {
            if (auto status = stable(); !status) return status;
            if (auto status = device_->read_blocks(bounds_.first_sector + u64{block} * 2,
                    std::as_writable_bytes(std::span{readback_})); !status) return status;
            if (readback_ != expected) return Status::io_error("directory candidate frozen preimage differs");
            return stable();
        };
        if (auto status = compare(journal_first_, source_journal_); !status) return fail(status);
        for (usize index = 0; index < home_count_; ++index)
        {
            const auto &expected = plan_.targets[index] == candidate_block_ && initialized ? end_ : plan_.before[index];
            if (auto status = compare(plan_.targets[index], expected); !status) return fail(status);
        }
        if (repeat_ownership)
        {
            const auto proof = clean_.preview_.view_.unreferenced_free_data_block(candidate_block_);
            if (!proof || proof.value() != candidate_block_)
                return fail(proof ? Status::fault("directory candidate repeated ownership identity") : proof.status());
        }
        return stable();
    }
    [[nodiscard]] Status initialize_directory()
    {
        if (!directory_create_ || !initialization_required_ || initialization_complete_ ||
            report_.phase != Ext4TransactionPhase::preflight)
            return Status::bad_descriptor("directory candidate has no pending initialization");
        report_.phase = Ext4TransactionPhase::data_initialization;
        if (auto status = write(candidate_block_, end_); !status) return fail(status);
        report_.phase = Ext4TransactionPhase::data_initialization_flush;
        if (auto status = flush(); !status) return fail(status);
        report_.phase = Ext4TransactionPhase::data_initialization_readback;
        if (auto status = stable(); !status) return fail(status);
        if (auto status = device_->read_blocks(bounds_.first_sector + u64{candidate_block_} * 2,
                std::as_writable_bytes(std::span{source_directory_data_})); !status) return fail(status);
        if (!jbd2::detail::all_zero(source_directory_data_))
            return fail(Status::io_error("directory candidate initialized readback is not zero"));
        report_.phase = Ext4TransactionPhase::preflight;
        if (auto status = revalidate_directory(true); !status) return fail(status);
        const Ext4CreateSource source{plan_.inode, inode_group_, parent_table_, new_table_, parent_block_,
            source_super_, source_descriptors_, source_bitmap_, source_parent_, source_new_, source_directory_};
        const Ext4DirectoryCreateSource directory_source{source, block_group_, candidate_block_,
            source_block_bitmap_, source_directory_data_};
        if (auto status = Ext4DirectoryCreatePlanner::plan(*clean_.preview_.binding_.info(), candidate_update_,
                directory_source, plan_); !status) return fail(status);
        initialization_complete_ = true;
        return build_publication(candidate_update_, true);
    }

    [[nodiscard]] Status logical_images(std::array<u64, home_capacity + 1> &blocks,
        std::array<Block, home_capacity + 1> &images, usize &count) const
    {
        if (report_.phase != Ext4TransactionPhase::preflight)
            return Status::bad_descriptor("create has no prepared image");
        blocks[0] = journal_first_; images[0] = journal_;
        for (usize index = 0; index < home_count_; ++index)
        { blocks[index + 1] = plan_.targets[index]; images[index + 1] = plan_.after[index]; }
        count = home_count_ + 1; return {};
    }
    [[nodiscard]] Status publish_prepared()
    {
        if (report_.phase != Ext4TransactionPhase::preflight ||
            (initialization_required_ && !initialization_complete_))
            return Status::bad_descriptor("create has no strict initialized publication plan");
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
    [[nodiscard]] Status matches_published()
    {
        if (!report_.published()) return Status::bad_descriptor("create publication is not durable");
        Block observed{};
        const auto check = [&](u32 block, const Block &expected) -> Status {
            if (auto status = stable(); !status) return status;
            if (auto status = device_->read_blocks(bounds_.first_sector + u64{block} * 2,
                    std::as_writable_bytes(std::span{observed})); !status) return status;
            if (observed != expected) return Status::io_error("create publication seal differs");
            return stable();
        };
        if (auto status = check(journal_first_, journal_); !status) return status;
        if (auto status = check(journal_first_ + 1, descriptor_); !status) return status;
        for (usize index = 0; index < home_count_; ++index)
            if (auto status = check(journal_first_ + u32(index) + 2, payload_[index]); !status) return status;
        if (auto status = check(journal_first_ + u32(home_count_) + 2, commit_); !status) return status;
        return check(journal_first_ + u32(home_count_) + 3, end_);
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
            now.writable == geometry_.writable ? Status{} : Status::io_error("create device changed");
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
    u32 journal_first_{}, inode_group_{}, parent_table_{}, new_table_{}, parent_block_{}, block_group_{}, candidate_block_{};
    bool directory_create_{}, initialization_required_{}, initialization_complete_{};
    std::array<char, 32> candidate_name_{};
    Ext4CreateUpdate candidate_update_{};
    usize home_count_{};
    Ext4CreatePlan plan_{};
    std::array<Block, home_capacity> payload_{};
    Block source_super_{}, source_descriptors_{}, source_bitmap_{}, source_parent_{}, source_new_{}, source_directory_{};
    Block source_block_bitmap_{}, source_directory_data_{}, old_directory_data_{}, source_journal_{}, readback_{};
    Block journal_{}, descriptor_{}, commit_{}, end_{};
};
} // namespace ok::fs
