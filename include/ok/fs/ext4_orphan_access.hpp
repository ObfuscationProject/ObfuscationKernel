#pragma once

#include "ok/fs/ext4_orphan_chain.hpp"

namespace ok::fs
{
struct Ext4OrphanAccessKey
{
    std::array<u8, 16> filesystem_uuid{};
    u32 observed_head{}, number{}, generation{};
};

// A value copied from a checked, complete orphan-chain image. It does not
// retain a device, descriptor, mount, writer or reclamation authority.
class Ext4OrphanReadOnlySnapshot final
{
  public:
    [[nodiscard]] bool valid() const { return valid_; }
    [[nodiscard]] const std::array<u8, 16> &filesystem_uuid() const { return filesystem_uuid_; }
    [[nodiscard]] u32 observed_head() const { return observed_head_; }
    [[nodiscard]] u32 number() const { return number_; }
    [[nodiscard]] u32 generation() const { return generation_; }
    [[nodiscard]] u32 size() const { return size_; }
    [[nodiscard]] u32 block_count() const { return block_count_; }

    [[nodiscard]] Result<u32> physical_block(u32 logical) const
    {
        if (!valid_) return Status::not_initialized("orphan snapshot is unavailable");
        if (logical >= block_count_) return Status::not_found("orphan logical block is absent");
        return data_blocks_[logical];
    }

  private:
    friend class Ext4OrphanReadOnlyDecoder;
    bool valid_{};
    std::array<u8, 16> filesystem_uuid_{};
    u32 observed_head_{}, number_{}, generation_{}, size_{}, block_count_{};
    std::array<u32, 16> data_blocks_{};
};

// The owner must supply one exclusive, stable old or replay-completed image
// epoch. This pure decoder checks supplied bytes; it cannot establish their
// device provenance, active-descriptor lifetime or global block ownership.
class Ext4OrphanReadOnlyDecoder final
{
  public:
    [[nodiscard]] static Status capture(const Ext4RecoveryInfo &binding,
                                        const Ext4OrphanChainSource &source,
                                        const Ext4OrphanAccessKey &key,
                                        Ext4OrphanReadOnlySnapshot &output)
    {
        output = {};
        if (!key.observed_head || key.number < 11 || key.number > 8192)
            return Status::invalid_argument("orphan access key bounds");
        if (key.filesystem_uuid != binding.uuid)
            return Status::io_error("orphan access filesystem UUID differs");
        Ext4OrphanChainProof proof{};
        if (auto status = Ext4OrphanChainValidator::validate(binding, source, proof); !status)
            return status;
        if (proof.observed_head() != key.observed_head ||
            proof.filesystem_uuid() != key.filesystem_uuid)
            return Status::io_error("orphan access chain identity differs");
        const auto *member = proof.member(key.number, key.generation);
        if (!member) return Status::not_found("orphan access member is absent");
        Ext4OrphanReadOnlySnapshot proposed{};
        proposed.filesystem_uuid_ = proof.filesystem_uuid();
        proposed.observed_head_ = proof.observed_head();
        proposed.number_ = member->number;
        proposed.generation_ = member->generation;
        proposed.size_ = member->size;
        proposed.block_count_ = member->data_count;
        proposed.data_blocks_ = member->data_blocks;
        proposed.valid_ = true;
        output = proposed;
        return {};
    }
};
} // namespace ok::fs
