#pragma once

#include "ok/fs/ext4_namespace_recovery_binding.hpp"

namespace ok::fs
{
// Private prospective bytes. This seal never authorizes transport writes.
class Ext4NamespaceResultHistory final
{
    template <usize, usize> friend class Ext4OwnedTransaction;
    template <usize> friend class Ext4OrphanChainCapture;
    using Block = Ext4NamespaceBlock;
  public:
    Ext4NamespaceResultHistory() = default;
    Ext4NamespaceResultHistory(const Ext4NamespaceResultHistory &) = delete;
    Ext4NamespaceResultHistory &operator=(const Ext4NamespaceResultHistory &) = delete;
    Ext4NamespaceResultHistory(Ext4NamespaceResultHistory &&) = delete;
    Ext4NamespaceResultHistory &operator=(Ext4NamespaceResultHistory &&) = delete;
  private:
    static bool same_info(const Ext4RecoveryInfo &a, const Ext4RecoveryInfo &b)
    {
        const auto &x = a.layout, &y = b.layout;
        if (x.first_sector != y.first_sector || x.filesystem_blocks != y.filesystem_blocks ||
            x.journal_first_block != y.journal_first_block || x.journal_blocks != y.journal_blocks ||
            x.block_size != y.block_size || a.uuid != b.uuid || a.metadata_count != b.metadata_count ||
            a.metadata_count > a.metadata.size() || a.journal_inode != b.journal_inode ||
            a.journal_sequence != b.journal_sequence || a.needs_recovery != b.needs_recovery ||
            a.empty_log != b.empty_log) return false;
        for (usize index = 0; index < a.metadata.size(); ++index)
            if (a.metadata[index].first != b.metadata[index].first ||
                a.metadata[index].count != b.metadata[index].count) return false;
        return true;
    }
    static bool same_proof(const Ext4NamespaceColdResult &a, const Ext4NamespaceColdResult &b)
    {
        const auto &x = a.homes, &y = b.homes;
        return a.operation == b.operation && a.parent == b.parent && a.source == b.source &&
            a.removed_inode == b.removed_inode && a.witness_runs == b.witness_runs &&
            a.witness_bytes == b.witness_bytes && a.durable_old_witness == b.durable_old_witness &&
            x.targets == y.targets && x.home_after == y.home_after && x.count == y.count &&
            x.old_homes == y.old_homes && x.new_homes == y.new_homes && x.all_before == y.all_before &&
            x.all_after == y.all_after && x.orphan_published == y.orphan_published &&
            x.allocation_unchanged == y.allocation_unchanged &&
            x.conditional_on_old_witness == y.conditional_on_old_witness;
    }
    static void big32(Block &bytes, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - index * 8)); }
    [[nodiscard]] Status prepare(const Ext4NamespaceRecoveryBinding &binding)
    {
        if (binding_ || ready_) return Status::already_exists("namespace result history is one-shot");
        binding_ = &binding;
        if (!binding.ready() || !binding.info() || !binding.old_binding() || !binding.result() ||
            !binding.result()->durable_old_witness || !binding.result()->homes.orphan_published ||
            !binding.result()->homes.allocation_unchanged || binding.count() < 2 || binding.count() > 5 ||
            binding.parsed_next_sequence() == ~u32{0} ||
            binding.parsed_next_sequence() != binding.journal_sequence() + 1 ||
            binding.linux_cleanup_sequence() != binding.parsed_next_sequence() + 1 ||
            binding.linux_cleanup_sequence() > ~u32{0} - 3)
            return Status::unsupported("namespace prospective single-orphan result profile");
        old_info_ = *binding.old_binding(); active_info_ = *binding.info(); proof_ = *binding.result();
        count_ = binding.count_; targets_ = binding.targets_; current_ = binding.current_;
        before_ = binding.before_; after_ = binding.after_; payloads_ = binding.payloads_;
        super_ = binding.super_; descriptors_ = binding.descriptors_; table_ = binding.table_block_;
        inode_bitmaps_ = binding.inode_bitmaps_; block_bitmaps_ = binding.block_bitmaps_;
        head_ = binding.head_; old_head_ = binding.old_head_; descriptor_ = binding.descriptor_;
        commit_ = binding.commit_; end_ = binding.end_; bounds_ = binding.bounds_;
        geometry_ = binding.geometry_; device_ = binding.device_;
        journal_first_ = binding.journal_first_; journal_blocks_ = binding.journal_blocks_;
        sequence_ = binding.journal_sequence(); cleanup_sequence_ = binding.linux_cleanup_sequence();
        const Block *old_super{}, *new_super{}, *current_super{};
        for (usize index = 0; index < count_; ++index)
            if (targets_[index] == 1) { old_super = &before_[index]; new_super = &after_[index]; current_super = &current_[index]; }
        if (!old_super || !new_super || ext4_checked::le32(*old_super, 232) ||
            !current_super || *current_super != super_ ||
            ext4_checked::le32(*new_super, 232) != proof_.removed_inode ||
            proof_.removed_inode < 11 || proof_.removed_inode > 8192 ||
            old_info_.uuid != active_info_.uuid || !old_info_.empty_log || active_info_.empty_log ||
            !old_info_.needs_recovery || !active_info_.needs_recovery ||
            old_info_.journal_sequence != sequence_ - 1 || active_info_.journal_sequence != sequence_)
            return Status::fault("namespace prospective old or resulting head differs");
        empty_head_ = head_; big32(empty_head_, 24, cleanup_sequence_); big32(empty_head_, 28, 0);
        big32(empty_head_, 252, 0);
        big32(empty_head_, 252, jbd2::detail::crc32c(~u32{0}, empty_head_));
        const auto codec = jbd2::Codec::open(empty_head_, journal_blocks_, active_info_.uuid,
            jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!codec || codec.value().superblock().sequence != cleanup_sequence_ ||
            codec.value().superblock().start_block)
            return Status::fault("namespace prospective empty head checksum differs");
        ready_ = true; return stable();
    }
    [[nodiscard]] Status stable() const
    {
        if (!ready_ || !binding_ || !binding_->ready() || binding_->device_ != device_ ||
            !binding_->old_binding() || !binding_->info() || !binding_->result() ||
            binding_->count_ != count_ || binding_->targets_ != targets_ || binding_->current_ != current_ ||
            binding_->before_ != before_ || binding_->after_ != after_ || binding_->payloads_ != payloads_ ||
            binding_->super_ != super_ || binding_->descriptors_ != descriptors_ || binding_->table_block_ != table_ ||
            binding_->inode_bitmaps_ != inode_bitmaps_ || binding_->block_bitmaps_ != block_bitmaps_ ||
            binding_->head_ != head_ || binding_->old_head_ != old_head_ || binding_->descriptor_ != descriptor_ ||
            binding_->commit_ != commit_ || binding_->end_ != end_ || binding_->journal_first_ != journal_first_ ||
            binding_->journal_blocks_ != journal_blocks_ || binding_->bounds_.first_sector != bounds_.first_sector ||
            binding_->bounds_.sector_count != bounds_.sector_count || binding_->journal_sequence() != sequence_ ||
            binding_->parsed_next_sequence() != sequence_ + 1 || binding_->linux_cleanup_sequence() != cleanup_sequence_ ||
            !same_info(*binding_->old_binding(), old_info_) || !same_info(*binding_->info(), active_info_) ||
            !same_proof(*binding_->result(), proof_))
            return Status::fault("namespace prospective sealed proof changed");
        const auto &canonical = binding_->cold_scratch_.canonical;
        if (!canonical.ready || canonical.no_op || canonical.count != count_ || canonical.sequence != sequence_ ||
            canonical.targets != targets_ || canonical.journal_head != head_ || canonical.descriptor != descriptor_ ||
            canonical.payloads != payloads_ || canonical.commit != commit_ || canonical.end != end_ ||
            binding_->cold_scratch_.before != before_ || binding_->cold_scratch_.after != after_ ||
            binding_->cold_scratch_.old_head != old_head_)
            return Status::fault("namespace prospective canonical proof changed");
        Block expected_head = head_;
        big32(expected_head, 24, cleanup_sequence_); big32(expected_head, 28, 0); big32(expected_head, 252, 0);
        big32(expected_head, 252, jbd2::detail::crc32c(~u32{0}, expected_head));
        if (empty_head_ != expected_head)
            return Status::fault("namespace prospective derived whole empty head changed");
        const auto geometry = device_->geometry();
        if (geometry.block_size != geometry_.block_size || geometry.block_count != geometry_.block_count ||
            geometry.writable != geometry_.writable || binding_->geometry_.block_size != geometry_.block_size ||
            binding_->geometry_.block_count != geometry_.block_count || binding_->geometry_.writable != geometry_.writable)
            return Status::io_error("namespace prospective original geometry changed");
        return {};
    }
    [[nodiscard]] const Block *physical(u32 block) const
    {
        if (block == 1) return &super_;
        if (block == 2) return &descriptors_;
        if (block == ext4_checked::le32(descriptors_, 8) + 1) return &table_;
        for (usize group = 0; group < 4; ++group)
        {
            if (block == ext4_checked::le32(descriptors_, group * 32)) return &block_bitmaps_[group];
            if (block == ext4_checked::le32(descriptors_, group * 32 + 4)) return &inode_bitmaps_[group];
        }
        if (block == journal_first_) return &head_;
        if (block == journal_first_ + 1) return &descriptor_;
        for (usize index = 0; index < count_; ++index)
        {
            if (block == targets_[index]) return &current_[index];
            if (block == journal_first_ + u32(index) + 2) return &payloads_[index];
        }
        if (block == journal_first_ + u32(count_) + 2) return &commit_;
        if (block == journal_first_ + u32(count_) + 3) return &end_;
        return nullptr;
    }
    [[nodiscard]] Status substitute(u32 block, std::span<std::byte> bytes) const
    {
        if (auto status = stable(); !status) return status;
        if (bytes.size() != 1024) return Status::invalid_argument("namespace prospective block width");
        const Block *current = physical(block), *result{};
        if (current)
            for (usize byte = 0; byte < 1024; ++byte)
                if (bytes[byte] != std::byte((*current)[byte]))
                    return Status::io_error("namespace prospective whole physical source changed");
        if (block == journal_first_) result = &empty_head_;
        else for (usize index = 0; index < count_; ++index)
            if (block == targets_[index]) result = &after_[index];
        if (result)
            for (usize byte = 0; byte < 1024; ++byte) bytes[byte] = std::byte((*result)[byte]);
        return stable();
    }
    const Ext4NamespaceRecoveryBinding *binding_{};
    driver::BlockDevice *device_{};
    driver::BlockGeometry geometry_{};
    Ext4RecoveryBounds bounds_{};
    Ext4RecoveryInfo old_info_{}, active_info_{};
    Ext4NamespaceColdResult proof_{};
    std::array<u32, 5> targets_{};
    std::array<Block, 5> current_{}, before_{}, after_{}, payloads_{};
    std::array<Block, 4> inode_bitmaps_{}, block_bitmaps_{};
    Block super_{}, descriptors_{}, table_{}, head_{}, old_head_{}, empty_head_{}, descriptor_{}, commit_{}, end_{};
    usize count_{};
    u32 journal_first_{}, journal_blocks_{}, sequence_{}, cleanup_sequence_{};
    bool ready_{};
};
} // namespace ok::fs
