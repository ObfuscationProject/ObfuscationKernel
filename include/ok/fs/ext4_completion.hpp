#pragma once

#include "ok/fs/ext4_replay_view.hpp"

namespace ok::fs
{
enum class Ext4CompletionPhase : u8
{
    idle, validating, preflight, replaying, home_flush, journal_clean,
    journal_flush, filesystem_clean, filesystem_flush, complete, failed,
    directory_table_flush, create_table_flush, create_allocation_flush,
    first_data_allocation_flush, first_data_inode_flush,
};
struct Ext4CompletionReport
{
    Ext4CompletionPhase phase{Ext4CompletionPhase::idle};
    Ext4CompletionPhase failed_phase{Ext4CompletionPhase::idle};
    Status status{};
    usize validated_actions{}, checked_inodes{}, write_attempts{}, completed_writes{}, flush_attempts{}, completed_flushes{};
    u32 journal_sequence{};
    bool empty_log{}, already_clean{}, home_durable{}, journal_durable{}, filesystem_durable{};
    [[nodiscard]] bool destination_may_have_changed() const { return write_attempts != 0; }
    [[nodiscard]] bool complete() const { return phase == Ext4CompletionPhase::complete && filesystem_durable; }
};

// Completes only the admitted single metadata-only transaction profile. The
// caller owns one quiesced device session, excludes all caches/other writers and
// retains ownership after any error until that session is fenced or retired.
// This is recovery completion, never a mount capability or a journal writer.
// Whole-operation prefixes are restartable. Torn checksummed home/control blocks
// remain fail-closed; successful block writes/flushes obey the device contract.
template <usize ActionCapacity = 64, usize RevokeCapacity = 64>
class Ext4RecoveryCompletion final
{
    friend class Ext4MetadataTransaction;
    friend class Ext4RenameTransaction;
    friend class Ext4CreateTransaction;
    friend class Ext4FirstDataTransaction;
    friend class Ext4NextDataTransaction;
    template <usize> friend class Ext4MountedMetadata;
    template <usize, usize> friend class Ext4OwnedTransaction;
    template <usize> friend class Ext4ReclaimComposedHistory;
  public:
    explicit Ext4RecoveryCompletion(bool allow_ordered_data = false, bool retain_dirty = false,
                                    Ext4ReplayPolicy policy = Ext4ReplayPolicy::structural)
        : preview_(allow_ordered_data, retain_dirty, policy), retain_dirty_(retain_dirty), policy_(policy) {}
    Ext4RecoveryCompletion(const Ext4RecoveryCompletion &) = delete;
    Ext4RecoveryCompletion &operator=(const Ext4RecoveryCompletion &) = delete;
    Ext4RecoveryCompletion(Ext4RecoveryCompletion &&) = delete;
    Ext4RecoveryCompletion &operator=(Ext4RecoveryCompletion &&) = delete;
    [[nodiscard]] const Ext4CompletionReport &report() const { return report_; }

    [[nodiscard]] Status run(driver::BlockDevice &device, Ext4RecoveryBounds bounds)
    {
        if (const auto status = prepare(device, bounds); !status)
            return status;
        return execute_prepared();
    }

  private:
    // Mounted startup adds an exact original locator bookend to the inherited
    // strict legacy proof. Reuse off-stack payload storage before execution.
    [[nodiscard]] Status run_legacy_startup(driver::BlockDevice &device, Ext4RecoveryBounds bounds,
                                          std::span<const u8> head, std::span<const u8> descriptor,
                                          usize actions)
    {
        if (head.size() != 1024 || descriptor.size() != 1024 || !actions || actions > ActionCapacity)
            return Status::invalid_argument("legacy startup locator storage or action count");
        if (const auto status = prepare(device, bounds); !status) return status;
        if (preview_.plan_.empty_log || preview_.plan_.actions.size() != actions ||
            preview_.codec_->superblock().start_block != 1 ||
            preview_.codec_->superblock().sequence != jbd2::detail::be32(head, 24))
            return fail(Status::fault("legacy startup strict proof differs from original locator"));
        if (const auto status = preview_.journal_.read_block(0, payload_); !status) return fail(status);
        for (usize index = 0; index < payload_.size(); ++index)
            if (payload_[index] != head[index])
                return fail(Status::fault("legacy startup original journal head changed"));
        if (const auto status = preview_.journal_.read_block(1, payload_); !status) return fail(status);
        for (usize index = 0; index < payload_.size(); ++index)
            if (payload_[index] != descriptor[index])
                return fail(Status::fault("legacy startup original descriptor changed"));
        return execute_prepared();
    }

    [[nodiscard]] Status execute_prepared()
    {
        if (report_.phase != Ext4CompletionPhase::preflight)
            return Status::busy("ext4 completion has no prepared execution");
        const auto &info = *preview_.binding_.info();
        const auto &plan = preview_.plan_;
        if (const auto status = flush(); !status)
            return fail(status);
        if (!info.needs_recovery)
        {
            // An earlier final flush may have failed after completing its write.
            // Even a clean-looking retry must establish durability before success.
            report_.home_durable = report_.journal_durable = report_.filesystem_durable = true;
            return complete();
        }
        if (!plan.empty_log)
        {
            // The exact allocation preview may prove that every home already
            // equals its committed payload. Initial FLUSH made those checked
            // bytes durable; it grants journal cleanup, never home-write authority.
            if (!preview_.data_all_homes_after_)
            {
                report_.phase = Ext4CompletionPhase::replaying;
                for (usize index = 0; index < plan.actions.size(); ++index)
                {
                    if (ext4_create_replay_policy(policy_))
                    {
                        const usize table_count = plan.actions.size() -
                            (policy_ == Ext4ReplayPolicy::bounded_directory_create ? 6U : 4U);
                        if (index == table_count || index + 1 == plan.actions.size())
                        {
                            report_.phase = index == table_count ? Ext4CompletionPhase::create_table_flush :
                                                                   Ext4CompletionPhase::create_allocation_flush;
                            if (const auto status = flush(); !status)
                                return fail(status);
                            report_.phase = Ext4CompletionPhase::replaying;
                        }
                    }
                    if (ext4_data_allocation_policy(policy_) && index == 3)
                    {
                        report_.phase = Ext4CompletionPhase::first_data_allocation_flush;
                        if (const auto status = flush(); !status)
                            return fail(status);
                        report_.phase = Ext4CompletionPhase::replaying;
                    }
                    // Directory replay admits only a table-prefix followed by
                    // one directory action. Make that prefix durable before a
                    // directory home write can take effect independently.
                    if (policy_ == Ext4ReplayPolicy::directory_rename &&
                        index + 1 == plan.actions.size())
                    {
                        report_.phase = Ext4CompletionPhase::directory_table_flush;
                        if (const auto status = flush(); !status)
                            return fail(status);
                        report_.phase = Ext4CompletionPhase::replaying;
                    }
                    const auto &action = plan.actions[index];
                    if (const auto status = preview_.action_payload(action, payload_); !status)
                        return fail(status);
                    if (const auto status = write(u32(action.target_block), payload_); !status)
                        return fail(status);
                }
                report_.phase = ext4_data_allocation_policy(policy_) ?
                    Ext4CompletionPhase::first_data_inode_flush : Ext4CompletionPhase::home_flush;
                if (const auto status = flush(); !status)
                    return fail(status);
            }
            report_.home_durable = true;
            report_.phase = Ext4CompletionPhase::journal_clean;
            if (const auto status = write(u32(info.layout.journal_first_block), journal_); !status)
                return fail(status);
            report_.phase = Ext4CompletionPhase::journal_flush;
            if (const auto status = flush(); !status)
                return fail(status);
        }
        if (retain_dirty_)
        {
            // A mounted journal has an empty tail, but its recovery bit remains
            // durable until the separate, one-way clean freeze.
            report_.home_durable = report_.journal_durable = report_.filesystem_durable = true;
            return complete();
        }
        // Empty-log resume is admitted only after the checked final home view.
        // Its initial flush establishes durability of the already empty journal.
        report_.home_durable = report_.journal_durable = true;
        report_.phase = Ext4CompletionPhase::filesystem_clean;
        if (const auto status = write(1, filesystem_); !status)
            return fail(status);
        report_.phase = Ext4CompletionPhase::filesystem_flush;
        if (const auto status = flush(); !status)
            return fail(status);
        report_.filesystem_durable = true;
        return complete();
    }

  private:
    void require_one_kib() { preview_.require_one_kib_ = true; }
    [[nodiscard]] const Ext4RecoveryInfo *binding() const { return preview_.binding_.info(); }
    [[nodiscard]] Status prepare(driver::BlockDevice &device, Ext4RecoveryBounds bounds)
    { return prepare_impl<false>(device, bounds); }
    // Private inspection of the already authenticated typed child's empty log.
    // This does not broaden legacy preparation or execute a recovery action.
    [[nodiscard]] Status prepare_typed_empty(driver::BlockDevice &device, Ext4RecoveryBounds bounds)
    { return prepare_impl<true>(device, bounds); }
    template <bool TypedEmpty>
    [[nodiscard]] Status prepare_impl(driver::BlockDevice &device, Ext4RecoveryBounds bounds)
    {
        if (report_.phase != Ext4CompletionPhase::idle)
            return Status::already_exists("ext4 completion is one-shot");
        report_.phase = Ext4CompletionPhase::validating;
        device_ = &device;
        geometry_ = device.geometry();
        if (!geometry_.writable)
            return fail(Status::denied("ext4 completion requires writable exclusive media"));
        // This checks ALL retained actions, their before/after metadata and the
        // final superblock/group/root view before any mutation or flush occurs.
        if (const auto status = preview_.open(device, bounds); !status)
            return fail(status);
        if (const auto status = validate_allocated(); !status)
            return fail(status);
        const auto &info = *preview_.binding_.info();
        const auto &plan = preview_.plan_;
        const auto &super = preview_.codec_->superblock();
        if constexpr (TypedEmpty)
            if (!plan.empty_log || !plan.actions.empty() || plan.committed_transactions || plan.revoked_actions || plan.revoke_records)
                return fail(Status::unsupported("typed suffix strict inspection requires an actual empty log"));
        const bool directory_replay = policy_ == Ext4ReplayPolicy::directory_rename;
        const bool create_replay = ext4_create_replay_policy(policy_);
        const bool first_data_replay = ext4_data_allocation_policy(policy_);
        const bool admitted_features = super.features == jbd2::feature_checksum_v3 ||
            super.features == (jbd2::feature_revoke | jbd2::feature_checksum_v3);
        if (info.layout.block_size != 1024 || !admitted_features ||
            plan.incomplete_tail || plan.revoked_actions || plan.revoke_records ||
            (create_replay && (!retain_dirty_ || preview_.allow_ordered_data_ ||
                (!plan.empty_log && (plan.committed_transactions != 1 ||
                 (policy_ == Ext4ReplayPolicy::bounded_directory_create ?
                    (plan.actions.size() != 7 && plan.actions.size() != 8) :
                    (plan.actions.size() != 5 && plan.actions.size() != 6)))))) ||
            (first_data_replay && (!retain_dirty_ || preview_.allow_ordered_data_ ||
                (!plan.empty_log && (plan.committed_transactions != 1 || plan.actions.size() != 4)))) ||
            (directory_replay && (!retain_dirty_ || preview_.allow_ordered_data_ ||
                (!plan.empty_log && (plan.committed_transactions != 1 ||
                 (plan.actions.size() != 2 && plan.actions.size() != 3))))) ||
            (!directory_replay && !create_replay && !first_data_replay && !plan.empty_log &&
             (plan.committed_transactions != 1 ||
                (retain_dirty_ ? (plan.actions.size() != 1 || plan.actions[0].target_block == 1)
                               : (plan.actions.size() != 2 || plan.actions[0].target_block != 1 ||
                                  plan.actions[1].target_block == 1)))))
            return fail(Status::unsupported("ext4 completion transaction action profile"));
        if (retain_dirty_ && !info.needs_recovery && !plan.empty_log)
            return fail(Status::io_error("clean ext4 has a published journal head"));
        if (!plan.empty_log && !directory_replay && !create_replay && !first_data_replay)
        {
            const auto first = preview_.view_.inode_at(u32(plan.actions[retain_dirty_ ? 0 : 1].target_block), 0);
            if (!first || first.value() + 4 <= preview_.view_.first_ordinary_inode())
                return fail(Status::unsupported("ext4 completion second action requires an ordinary inode-table block"));
        }
        report_.validated_actions = preview_.report().validated_actions;
        report_.empty_log = plan.empty_log;
        report_.already_clean = !info.needs_recovery;
        report_.journal_sequence = plan.empty_log ? super.sequence : plan.next_sequence + 1;
        if (!report_.journal_sequence)
            return fail(Status::unsupported("ext4 completion zero cleanup sequence"));
        if (const auto status = preview_.journal_.read_block(0, journal_); !status)
            return fail(status);
        if (jbd2::detail::be32(journal_, 88))
            return fail(Status::unsupported("ext4 completion cyclic journal head outside profile"));
        if (const auto status = preview_.final_block(1, filesystem_); !status)
            return fail(status);
        // All completion bytes are prepared before writes; the retained log
        // records and inode mapping are immutable throughout this operation.
        if (!plan.empty_log)
        {
            be32(journal_, 24, report_.journal_sequence);
            be32(journal_, 28, 0);
            be32(journal_, 252, 0);
            be32(journal_, 252, jbd2::detail::crc32c(~u32{0}, journal_));
        }
        if (!retain_dirty_)
        {
            le32(filesystem_, 96, ext4_checked::le32(filesystem_, 96) & ~u32{4});
            le32(filesystem_, 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{filesystem_}.first(1020)));
        }
        report_.phase = Ext4CompletionPhase::preflight;
        return Status::success();
    }
    [[nodiscard]] Status validate_directory(u32 number)
    {
        u32 dots = 0, parents = 0;
        const auto status = preview_.directory(number, [&](const Ext4ReadOnlyDirent &entry) {
            const auto child = preview_.lookup(number, entry.name);
            if (!child)
                return child.status();
            if (entry.name == ".")
            {
                if (++dots != 1 || child.value() != number)
                    return Status::io_error("ext4 completion directory dot identity");
            }
            if (entry.name == "..")
            {
                const auto parent = preview_.inode(child.value());
                if (++parents != 1 || !parent || (parent.value().mode & 0170000) != 0040000 ||
                    (number != 2 && child.value() == number))
                    return Status::io_error("ext4 completion directory parent identity");
            }
            return Status::success();
        });
        if (!status)
            return status;
        return dots == 1 && parents == 1 ? Status::success() :
               Status::io_error("ext4 completion missing directory dot entries");
    }
    [[nodiscard]] Status validate_allocated()
    {
        auto &view = preview_.view_;
        if (const auto status = validate_directory(2); !status)
            return status;
        std::array<u8, 4096> bitmap{};
        for (u32 group = 0; group < view.group_count_; ++group)
        {
            const auto &g = view.groups_[group];
            if (g.flags & 1)
                continue;
            if (const auto status = view.block(g.inode_bitmap, bitmap); !status)
                return status;
            if (u16(jbd2::detail::crc32c(view.seed_, std::span<const u8>{bitmap}.first(view.inodes_per_group_ / 8))) !=
                g.inode_checksum)
                return Status::io_error("ext4 completion inode bitmap checksum");
            for (u32 local = 0; local < view.inodes_per_group_; ++local)
            {
                const u32 number = group * view.inodes_per_group_ + local + 1;
                if (number < view.first_inode_ || !(bitmap[local / 8] & (1U << (local % 8))))
                    continue;
                if (++report_.checked_inodes > 128)
                    return Status::no_memory("ext4 completion inode inspection budget");
                const auto inode = preview_.inode(number);
                if (!inode)
                    return inode.status();
                if ((inode.value().mode & 0170000) == 0040000)
                    if (const auto status = validate_directory(number); !status)
                        return status;
            }
        }
        return Status::success();
    }
    static void be32(std::span<u8> bytes, usize at, u32 value)
    {
        bytes[at] = u8(value >> 24); bytes[at + 1] = u8(value >> 16);
        bytes[at + 2] = u8(value >> 8); bytes[at + 3] = u8(value);
    }
    static void le32(std::span<u8> bytes, usize at, u32 value)
    {
        bytes[at] = u8(value); bytes[at + 1] = u8(value >> 8);
        bytes[at + 2] = u8(value >> 16); bytes[at + 3] = u8(value >> 24);
    }
    [[nodiscard]] Status stable() const
    {
        const auto now = device_->geometry();
        return now.block_size == geometry_.block_size && now.block_count == geometry_.block_count &&
               now.writable == geometry_.writable ? Status::success() :
               Status::io_error("ext4 completion device geometry changed");
    }
    [[nodiscard]] Status write(u32 block, std::span<const u8> bytes)
    {
        const auto &layout = preview_.layout();
        if (block >= layout.filesystem_blocks || bytes.size() != layout.block_size)
            return Status::invalid_argument("ext4 completion write bounds");
        if (const auto status = stable(); !status)
            return status;
        ++report_.write_attempts;
        if (const auto status = device_->write_blocks(layout.first_sector + u64{block} * 2, std::as_bytes(bytes)); !status)
            return status;
        ++report_.completed_writes;
        return stable();
    }
    [[nodiscard]] Status flush()
    {
        if (const auto status = stable(); !status)
            return status;
        ++report_.flush_attempts;
        if (const auto status = device_->flush(); !status)
            return status;
        ++report_.completed_flushes;
        return stable();
    }
    [[nodiscard]] Status fail(Status status)
    {
        report_.failed_phase = report_.phase;
        report_.phase = Ext4CompletionPhase::failed;
        report_.status = status;
        return status;
    }
    [[nodiscard]] Status complete()
    {
        report_.phase = Ext4CompletionPhase::complete;
        report_.status = Status::success();
        return Status::success();
    }
    driver::BlockDevice *device_{};
    driver::BlockGeometry geometry_{};
    Ext4ReplayPreview<ActionCapacity, RevokeCapacity> preview_{};
    Ext4CompletionReport report_{};
    std::array<u8, 1024> journal_{}, filesystem_{}, payload_{};
    bool retain_dirty_{};
    Ext4ReplayPolicy policy_{};
};
} // namespace ok::fs
