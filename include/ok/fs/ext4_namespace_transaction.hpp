#pragma once

#include "ok/fs/ext4_metadata_transaction.hpp"
#include "ok/fs/ext4_namespace_witness.hpp"
#include "ok/fs/ext4_orphan_chain.hpp"

namespace ok::fs
{
// An exclusively owned, mounted-dirty namespace transaction. The owner seals
// every home image and the journal head before this writer can publish them.
// This class does not grant VFS admission or expose a mounted operation.
class Ext4NamespaceTransaction final
{
    template <usize, usize> friend class Ext4OwnedTransaction;
    using Block = Ext4NamespaceBlock;
    static constexpr usize home_capacity = Ext4NamespacePlan::capacity;

  public:
    Ext4NamespaceTransaction() = default;
    Ext4NamespaceTransaction(const Ext4NamespaceTransaction &) = delete;
    Ext4NamespaceTransaction &operator=(const Ext4NamespaceTransaction &) = delete;
    [[nodiscard]] const Ext4TransactionReport &report() const { return report_; }

  private:
    [[nodiscard]] Status prepare(driver::BlockDevice &device, Ext4RecoveryBounds bounds,
                                 Ext4NamespaceUpdate update)
    {
        if (report_.phase != Ext4TransactionPhase::idle)
            return Status::already_exists("namespace transaction is one-shot");
        report_.phase = Ext4TransactionPhase::validating;
        device_ = &device; bounds_ = bounds; geometry_ = device.geometry();
        if (!geometry_.writable || geometry_.block_size != 512)
            return fail(Status::unsupported("namespace writer requires writable 512-byte sectors"));
        if ((update.operation != Ext4NamespaceOperation::replace &&
             update.operation != Ext4NamespaceOperation::unlink) ||
            update.parent < 2 || update.parent > 8192 ||
            update.source < 11 || update.source > 8192 ||
            (update.operation == Ext4NamespaceOperation::replace &&
             (update.destination < 11 || update.destination > 8192)))
            return fail(Status::invalid_argument("namespace writer inode identity"));
        if (auto status = binding_.inspect(device, bounds); !status) return fail(status);
        const auto &binding = *binding_.info();
        if (!binding.needs_recovery || !binding.empty_log ||
            binding.layout.block_size != 1024 || binding.layout.filesystem_blocks != 32768 ||
            binding.layout.journal_first_block > 0xffffffffULL)
            return fail(Status::unsupported("namespace writer requires bounded dirty empty journal"));
        journal_first_ = u32(binding.layout.journal_first_block);
        if (auto status = read(1, super_); !status) return fail(status);
        if (auto status = read(2, descriptors_); !status) return fail(status);
        if (auto status = read(journal_first_, old_head_); !status) return fail(status);
        for (u32 group = 0; group < 4; ++group)
        {
            const usize offset = group * 32;
            if (auto status = read(ext4_checked::le32(descriptors_, offset), block_bitmaps_[group]); !status)
                return fail(status);
            if (auto status = read(ext4_checked::le32(descriptors_, offset + 4), inode_bitmaps_[group]); !status)
                return fail(status);
        }
        Ext4OrphanChainProof orphan_proof{};
        const Ext4OrphanChainSource orphan_source{super_, descriptors_, inode_bitmaps_, block_bitmaps_, {}};
        if (auto status = Ext4OrphanChainValidator::validate(binding, orphan_source, orphan_proof); !status)
            return fail(status);
        if (orphan_proof.count() || orphan_proof.observed_head())
            return fail(Status::unsupported("namespace writer requires independently empty orphan chain"));
        orphan_ = {orphan_proof.filesystem_uuid(), orphan_proof.observed_head(), true};

        const u32 removed = update.operation == Ext4NamespaceOperation::unlink ? update.source : update.destination;
        parent_block_ = table_for(update.parent);
        source_block_ = table_for(update.source);
        destination_block_ = table_for(removed);
        if (!parent_block_ || !source_block_ || !destination_block_)
            return fail(Status::unsupported("namespace writer inode-table placement"));
        if (auto status = read(parent_block_, parent_); !status) return fail(status);
        if (source_block_ == parent_block_) source_ = parent_;
        else if (auto status = read(source_block_, source_); !status) return fail(status);
        if (destination_block_ == parent_block_) destination_ = parent_;
        else if (destination_block_ == source_block_) destination_ = source_;
        else if (auto status = read(destination_block_, destination_); !status) return fail(status);
        directory_block_ = ext4_checked::le32(parent_, ((update.parent - 1) % 4) * 256 + 60);
        if (!directory_block_ || directory_block_ == parent_block_ ||
            directory_block_ == source_block_ || directory_block_ == destination_block_)
            return fail(Status::unsupported("namespace writer directory placement"));
        if (auto status = read(directory_block_, directory_); !status) return fail(status);
        const Ext4NamespaceSource old_source{parent_block_, source_block_, destination_block_, directory_block_,
            super_, descriptors_, parent_, source_, destination_, directory_,
            inode_bitmaps_, block_bitmaps_, orphan_};
        if (auto status = Ext4NamespaceDurableJournalPlanner::plan(binding, update,
                old_source, old_head_, scratch_, journal_); !status)
            return fail(status);
        if (!journal_.ready || journal_.no_op || journal_.count < 2 || journal_.count > home_capacity)
            return fail(Status::unsupported("namespace writer has no nonempty bounded transaction"));
        report_.sequence = journal_.sequence;
        report_.inode = update.source;
        report_.target_block = directory_block_;
        report_.phase = Ext4TransactionPhase::preflight;
        return {};
    }

    template <usize Capacity>
        requires (Capacity >= home_capacity + 1)
    [[nodiscard]] Status logical_images(std::array<u64, Capacity> &blocks,
                                        std::array<Block, Capacity> &images,
                                        usize &count) const
    {
        if (report_.phase != Ext4TransactionPhase::preflight)
            return Status::bad_descriptor("namespace writer has no prepared image");
        blocks[0] = journal_first_; images[0] = journal_.journal_head;
        for (usize index = 0; index < journal_.count; ++index)
        { blocks[index + 1] = journal_.targets[index]; images[index + 1] = scratch_.namespace_plan.after[index]; }
        count = journal_.count + 1;
        return {};
    }

    [[nodiscard]] Status publish_prepared()
    {
        if (report_.phase != Ext4TransactionPhase::preflight)
            return Status::bad_descriptor("namespace writer is not prepared");
        if (auto status = flush(report_.flush_attempts, report_.completed_flushes); !status) return fail(status);
        report_.phase = Ext4TransactionPhase::payloads;
        if (auto status = write(journal_first_ + 1, journal_.descriptor,
                report_.write_attempts, report_.completed_writes); !status) return fail(status);
        for (usize index = 0; index < journal_.count; ++index)
            if (auto status = write(journal_first_ + u32(index) + 2, journal_.payloads[index],
                    report_.write_attempts, report_.completed_writes); !status) return fail(status);
        if (auto status = write(journal_first_ + u32(journal_.count) + 3, journal_.end,
                report_.write_attempts, report_.completed_writes); !status) return fail(status);
        report_.phase = Ext4TransactionPhase::payload_flush;
        if (auto status = flush(report_.flush_attempts, report_.completed_flushes); !status) return fail(status);
        report_.payloads_durable = true;
        report_.phase = Ext4TransactionPhase::commit_record;
        if (auto status = write(journal_first_ + u32(journal_.count) + 2, journal_.commit,
                report_.write_attempts, report_.completed_writes); !status) return fail(status);
        report_.phase = Ext4TransactionPhase::commit_flush;
        if (auto status = flush(report_.flush_attempts, report_.completed_flushes); !status) return fail(status);
        report_.commit_record_durable = true;
        report_.recovery_bit_durable = true;
        report_.phase = Ext4TransactionPhase::publication;
        if (auto status = write(journal_first_, journal_.journal_head,
                report_.write_attempts, report_.completed_writes); !status) return fail(status);
        report_.phase = Ext4TransactionPhase::publication_flush;
        if (auto status = flush(report_.flush_attempts, report_.completed_flushes); !status) return fail(status);
        report_.publication_durable = true;
        report_.phase = Ext4TransactionPhase::published;
        return {};
    }

    [[nodiscard]] u32 table_for(u32 inode) const
    {
        if (!inode || inode > 8192) return 0;
        const u32 group = (inode - 1) / 2048;
        const u32 first = ext4_checked::le32(descriptors_, group * 32 + 8);
        const u32 offset = ((inode - 1) % 2048) / 4;
        return first <= 32767 - offset ? first + offset : 0;
    }
    [[nodiscard]] Status stable() const
    {
        const auto now = device_->geometry();
        return now.block_size == geometry_.block_size && now.block_count == geometry_.block_count &&
            now.writable == geometry_.writable ? Status{} : Status::io_error("namespace writer device changed");
    }
    [[nodiscard]] Status read(u32 block, Block &image) const
    {
        const auto *info = binding_.info();
        if (!info || block >= info->layout.filesystem_blocks)
            return Status::invalid_argument("namespace writer read bounds");
        if (auto status = stable(); !status) return status;
        if (auto status = device_->read_blocks(bounds_.first_sector + u64{block} * 2,
                std::as_writable_bytes(std::span{image})); !status) return status;
        return stable();
    }
    [[nodiscard]] Status write(u32 block, const Block &image,
                               usize &attempts, usize &completed)
    {
        if (auto status = stable(); !status) return status;
        ++attempts;
        if (auto status = device_->write_blocks(bounds_.first_sector + u64{block} * 2,
                std::as_bytes(std::span{image})); !status) return status;
        ++completed;
        return stable();
    }
    [[nodiscard]] Status flush(usize &attempts, usize &completed)
    {
        if (auto status = stable(); !status) return status;
        ++attempts;
        if (auto status = device_->flush(); !status) return status;
        ++completed;
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
    Ext4RecoveryBinding binding_{};
    Ext4NamespaceJournalScratch scratch_{};
    Ext4NamespaceJournalPlan journal_{};
    Ext4TransactionReport report_{};
    u32 journal_first_{}, parent_block_{}, source_block_{}, destination_block_{}, directory_block_{};
    Block old_head_{};
    Block super_{}, descriptors_{}, parent_{}, source_{}, destination_{}, directory_{};
    std::array<Block, 4> inode_bitmaps_{}, block_bitmaps_{};
    Ext4NamespaceEmptyOrphanAuthorization orphan_{};
};
} // namespace ok::fs
