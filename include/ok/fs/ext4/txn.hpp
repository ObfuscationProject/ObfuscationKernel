#pragma once

// ext4 txn module: final journal replay view, recovery completion, metadata buffers and the
// metadata transaction with its per-mutation writers.

#include "ok/fs/ext4/layout.hpp"
#include "ok/fs/ext4/extent.hpp"
#include "ok/fs/ext4/alloc.hpp"
#include "ok/fs/ext4/dir.hpp"
#include "ok/block/sessions.hpp"
#include <concepts>
#include <new>
#include <type_traits>
#include <algorithm>

namespace ok::fs
{
enum class Ext4ReplayPolicy : u8
{
    structural, directory_rename, directory_create, first_data_allocation,
    next_data_allocation, bounded_data_allocation, bounded_directory_create,
    checked_ownership, mounted_inode, checked_mode
};
[[nodiscard]] constexpr bool ext4_create_replay_policy(Ext4ReplayPolicy policy)
{
    return policy == Ext4ReplayPolicy::directory_create ||
           policy == Ext4ReplayPolicy::bounded_directory_create;
}
[[nodiscard]] constexpr bool ext4_data_allocation_policy(Ext4ReplayPolicy policy)
{
    return policy == Ext4ReplayPolicy::first_data_allocation ||
           policy == Ext4ReplayPolicy::next_data_allocation ||
           policy == Ext4ReplayPolicy::bounded_data_allocation;
}
enum class Ext4PreviewPhase : u8 { idle, binding, planning, validating, ready, failed };
struct Ext4PreviewReport
{
    Ext4PreviewPhase phase{Ext4PreviewPhase::idle};
    Status status{};
    usize planned_actions{}, validated_actions{}, revoked_actions{};
    u32 committed_transactions{}, next_sequence{};
    bool empty_log{}, incomplete_tail{};
};

// A no-write recovery preview, never a VFS mount or permission to execute replay.
// The bounded policy preserves all allocation/mapping/namespace/journal identity
// bytes. It permits regular inode chmod/chown/ctime/version updates and selected
// superblock mount bookkeeping. Every committed nonrevoked action is validated;
// final reads use the last such payload for each target. The owner must exclude
// caches, other writers and replacement throughout this object's lifetime.
template <usize ActionCapacity = 64, usize RevokeCapacity = 64>
    requires (ActionCapacity > 0 && RevokeCapacity > 0)
class Ext4ReplayPreview final
{
    template <usize, usize> friend class Ext4RecoveryCompletion;
    template <usize, usize> friend class Ext4OwnedTransaction;
    friend class Ext4MetadataTransaction;
    friend class Ext4RenameTransaction;
    friend class Ext4CreateTransaction;
    friend class Ext4FirstDataTransaction;
    friend class Ext4NextDataTransaction;
    struct JournalReader
    {
        Ext4ReplayPreview *owner;
        Status read_block(u32 index, std::span<u8> bytes)
        {
            if (index >= owner->layout().journal_blocks)
                return Status::io_error("ext4 preview journal bounds");
            return owner->raw_block(u32(owner->layout().journal_first_block + index), bytes);
        }
    };
    struct FinalReader
    {
        Ext4ReplayPreview *owner;
        Status read_block(u32 index, std::span<u8> bytes) { return owner->final_block(index, bytes); }
    };
    struct RawReader
    {
        Ext4ReplayPreview *owner;
        Status read_block(u32 index, std::span<u8> bytes) { return owner->raw_block(index, bytes); }
    };
    struct CreateBeforeReader
    {
        Ext4ReplayPreview *owner;
        Status read_block(u32 index, std::span<u8> bytes)
        {
            if (bytes.size() != 1024)
                return Status::invalid_argument("directory create before-image block geometry");
            for (usize home = 0; home < owner->plan_.actions.size(); ++home)
                if (owner->plan_.actions[home].target_block == index)
                {
                    for (usize byte = 0; byte < bytes.size(); ++byte)
                        bytes[byte] = owner->create_before_[home][byte];
                    return {};
                }
            return owner->raw_block(index, bytes);
        }
    };
    struct DirectoryTarget { u32 inode{}, block{}; };

  public:
    explicit Ext4ReplayPreview(bool allow_ordered_data = false, bool strict_ordered_inode = false,
                               Ext4ReplayPolicy policy = Ext4ReplayPolicy::structural)
        : allow_ordered_data_(allow_ordered_data), strict_ordered_inode_(strict_ordered_inode), policy_(policy) {}
    Ext4ReplayPreview(const Ext4ReplayPreview &) = delete;
    Ext4ReplayPreview &operator=(const Ext4ReplayPreview &) = delete;
    Ext4ReplayPreview(Ext4ReplayPreview &&) = delete;
    Ext4ReplayPreview &operator=(Ext4ReplayPreview &&) = delete;

    [[nodiscard]] const Ext4PreviewReport &report() const { return report_; }
    [[nodiscard]] bool ready() const { return report_.phase == Ext4PreviewPhase::ready && view_.valid(); }
    [[nodiscard]] Status open(driver::BlockDevice &device, Ext4RecoveryBounds bounds)
    {
        if (report_.phase != Ext4PreviewPhase::idle)
            return Status::already_exists("ext4 replay preview is one-shot");
        report_.phase = Ext4PreviewPhase::binding;
        device_ = &device;
        geometry_ = device.geometry();
        const auto binding_status = policy_ == Ext4ReplayPolicy::bounded_directory_create ?
            binding_.inspect_directory_create_candidate(device, bounds) :
            policy_ == Ext4ReplayPolicy::directory_create ?
            binding_.inspect_create_candidate(device, bounds) :
            ext4_data_allocation_policy(policy_) ?
                binding_.inspect_first_data_candidate(device, bounds) : binding_.inspect(device, bounds);
        if (!binding_status)
            return fail(binding_status);
        // Constrain the actual inspected binding before replay planning or
        // any view/allocated-directory callback can use its decoded geometry.
        if ((require_one_kib_ || policy_ == Ext4ReplayPolicy::bounded_directory_create) &&
            binding_.info()->layout.block_size != 1024)
            return fail(Status::unsupported("selected ext4 route requires an actual 1 KiB binding"));
        if (const auto status = stable(); !status)
            return fail(status);
        if (policy_ == Ext4ReplayPolicy::directory_rename)
        {
            if (const auto status = raw_view_.open(raw_, *binding_.info()); !status)
                return fail(status);
            if (const auto status = directory_domain(); !status)
                return fail(status);
        }
        else if (ext4_create_replay_policy(policy_) ||
                 ext4_data_allocation_policy(policy_))
        {
            // Provisional target admission is intentionally broad only for
            // parsing. validate_directory_create must prove every exact home
            // target before this preview becomes ready or any write can occur.
            domain_count_ = 0;
            if (layout().journal_first_block > 0)
                domain_[domain_count_++] = {0, layout().journal_first_block};
            const u64 after_journal = layout().journal_first_block + layout().journal_blocks;
            if (after_journal < layout().filesystem_blocks)
                domain_[domain_count_++] = {after_journal,
                    layout().filesystem_blocks - after_journal};
            if (!domain_count_)
                return fail(Status::unsupported("ext4 allocation replay has no home domain"));
        }
        else if (policy_ != Ext4ReplayPolicy::structural && policy_ != Ext4ReplayPolicy::checked_ownership &&
                 policy_ != Ext4ReplayPolicy::mounted_inode && policy_ != Ext4ReplayPolicy::checked_mode)
            return fail(Status::invalid_argument("ext4 preview replay policy"));
        report_.phase = Ext4PreviewPhase::planning;
        auto record = std::span<u8>{record_}.first(layout().block_size);
        auto payload = std::span<u8>{after_}.first(layout().block_size);
        if (const auto status = journal_.read_block(0, record); !status)
            return fail(status);
        const auto opened = jbd2::Codec::open(record, layout().journal_blocks, binding_.info()->uuid,
                                             jbd2::DescriptorUuidPolicy::linux_internal_zero);
        if (!opened)
            return fail(opened.status());
        codec_ = opened.value();
        const auto domain = (policy_ == Ext4ReplayPolicy::structural || policy_ == Ext4ReplayPolicy::checked_ownership ||
                             policy_ == Ext4ReplayPolicy::mounted_inode || policy_ == Ext4ReplayPolicy::checked_mode) ? binding_.info()->structural_metadata() :
            jbd2::TargetDomain{layout().filesystem_blocks, std::span<const jbd2::BlockRange>{domain_}.first(domain_count_)};
        const auto planned = jbd2::build_replay_plan(*codec_, journal_, domain,
                                                    {record, payload, actions_, revocations_});
        if (!planned)
            return fail(planned.status());
        plan_ = planned.value();
        report_.planned_actions = plan_.actions.size();
        report_.revoked_actions = plan_.revoked_actions;
        report_.committed_transactions = plan_.committed_transactions;
        report_.next_sequence = plan_.next_sequence;
        report_.empty_log = plan_.empty_log;
        report_.incomplete_tail = plan_.incomplete_tail;
        if ((policy_ == Ext4ReplayPolicy::checked_ownership || policy_ == Ext4ReplayPolicy::checked_mode ||
             policy_ == Ext4ReplayPolicy::mounted_inode) && !plan_.empty_log &&
            (plan_.committed_transactions != 1 || plan_.revoked_actions ||
             (strict_ordered_inode_ ? plan_.actions.size() != 1 || plan_.actions[0].target_block == 1 :
                plan_.actions.size() != 2 || plan_.actions[0].target_block != 1 || plan_.actions[1].target_block == 1)))
            return fail(Status::unsupported("checked metadata replay transaction shape"));
        report_.phase = Ext4PreviewPhase::validating;
        if (ext4_create_replay_policy(policy_) && !plan_.empty_log &&
            (plan_.committed_transactions != 1 || plan_.incomplete_tail || plan_.revoked_actions || plan_.revoke_records ||
             (policy_ == Ext4ReplayPolicy::bounded_directory_create ?
                (plan_.actions.size() != 7 && plan_.actions.size() != 8) :
                (plan_.actions.size() != 5 && plan_.actions.size() != 6))))
            return fail(Status::unsupported("ext4 create replay transaction shape"));
        if (ext4_data_allocation_policy(policy_) && !plan_.empty_log &&
            (plan_.committed_transactions != 1 || plan_.incomplete_tail || plan_.revoked_actions || plan_.revoke_records ||
             plan_.actions.size() != 4))
            return fail(Status::unsupported("ext4 first data replay transaction shape"));
        if (const auto status = view_.open(final_, *binding_.info()); !status)
            return fail(status);
        if (policy_ == Ext4ReplayPolicy::directory_rename)
        {
            if (!plan_.empty_log)
                if (const auto status = validate_directory_rename(); !status)
                    return fail(status);
        }
        else if (ext4_create_replay_policy(policy_))
        {
            if (!plan_.empty_log)
            {
                const auto status = policy_ == Ext4ReplayPolicy::bounded_directory_create ?
                    validate_bounded_directory_create() : validate_directory_create();
                if (!status)
                    return fail(status);
            }
            else if (binding_.create_bitmap_mismatches_ || binding_.first_data_bitmap_mismatches_)
                return fail(Status::io_error("ext4 create replay empty log has split allocation metadata"));
        }
        else if (ext4_data_allocation_policy(policy_))
        {
            if (!plan_.empty_log)
            {
                if (const auto status = validate_data_allocation(); !status)
                    return fail(status);
            }
            else if (binding_.first_data_bitmap_mismatches_)
                return fail(Status::io_error("ext4 first data replay empty log has split allocation metadata"));
        }
        else
        for (usize index = 0; index < plan_.actions.size(); ++index)
        {
            const auto &action = plan_.actions[index];
            auto before = std::span<u8>{before_}.first(layout().block_size);
            auto after = std::span<u8>{after_}.first(layout().block_size);
            if (const auto status = previous_block(u32(action.target_block), index, before); !status)
                return fail(status);
            if (const auto status = action_payload(action, after); !status)
                return fail(status);
            if (const auto status = validate_change(u32(action.target_block), before, after); !status)
                return fail(status);
            ++report_.validated_actions;
        }
        if (const auto status = stable(); !status)
            return fail(status);
        report_.phase = Ext4PreviewPhase::ready;
        report_.status = Status::success();
        return Status::success();
    }

    [[nodiscard]] Result<Ext4ReadOnlyInode> inode(u32 number)
    {
        if (!ready())
            return Status::not_initialized("ext4 replay preview is unavailable");
        auto result = view_.inode(number);
        retire_if_failed(result.status());
        return result;
    }
    [[nodiscard]] Result<u32> lookup(u32 directory, std::string_view name)
    {
        if (!ready())
            return Status::not_initialized("ext4 replay preview is unavailable");
        auto result = view_.lookup(directory, name);
        retire_if_failed(result.status());
        return result;
    }
    [[nodiscard]] Result<usize> read(u32 number, u64 offset, std::span<u8> bytes)
    {
        if (!ready())
            return Status::not_initialized("ext4 replay preview is unavailable");
        auto result = view_.read(number, offset, bytes);
        retire_if_failed(result.status());
        return result;
    }
    template <typename Visitor>
        requires requires(Visitor visitor, const Ext4ReadOnlyDirent &entry) {
            { visitor(entry) } -> std::same_as<Status>;
        }
    [[nodiscard]] Status directory(u32 number, Visitor &&visitor)
    {
        if (!ready())
            return Status::not_initialized("ext4 replay preview is unavailable");
        const auto status = view_.directory(number, static_cast<Visitor &&>(visitor));
        retire_if_failed(status);
        return status;
    }

  private:
    [[nodiscard]] const jbd2::RecoveryLayout &layout() const { return binding_.info()->layout; }
    [[nodiscard]] Status stable() const
    {
        const auto now = device_->geometry();
        return now.block_size == geometry_.block_size && now.block_count == geometry_.block_count &&
                       now.writable == geometry_.writable ? Status::success() :
                       Status::io_error("ext4 preview device geometry changed");
    }
    [[nodiscard]] Status raw_block(u32 index, std::span<u8> bytes)
    {
        if (index >= layout().filesystem_blocks || bytes.size() != layout().block_size)
            return Status::invalid_argument("ext4 preview filesystem read bounds");
        if (const auto status = stable(); !status)
            return status;
        const auto sector = layout().first_sector + u64{index} * (layout().block_size / 512);
        if (const auto status = device_->read_blocks(sector, std::as_writable_bytes(bytes)); !status)
            return status;
        return stable();
    }
    [[nodiscard]] Status action_payload(const jbd2::ReplayAction &action, std::span<u8> bytes)
    {
        if (const auto status = journal_.read_block(action.journal_block, bytes); !status)
            return status;
        if (const auto status = codec_->payload(bytes, action.sequence,
                {action.target_block, action.checksum, action.restore_magic}); !status)
            return status;
        if (action.restore_magic)
        {
            bytes[0] = u8(jbd2::magic >> 24); bytes[1] = u8(jbd2::magic >> 16);
            bytes[2] = u8(jbd2::magic >> 8); bytes[3] = u8(jbd2::magic);
        }
        return Status::success();
    }
    [[nodiscard]] Status previous_block(u32 target, usize before_index, std::span<u8> bytes)
    {
        for (usize index = before_index; index != 0; --index)
            if (plan_.actions[index - 1].target_block == target)
                return action_payload(plan_.actions[index - 1], bytes);
        return raw_block(target, bytes);
    }
    [[nodiscard]] Status final_block(u32 target, std::span<u8> bytes)
    {
        return previous_block(target, plan_.actions.size(), bytes);
    }
    [[nodiscard]] static bool equal(std::span<const u8> one, std::span<const u8> two)
    {
        for (usize index = 0; index < one.size(); ++index)
            if (one[index] != two[index])
                return false;
        return true;
    }
    [[nodiscard]] bool ordinary_directory_target(u32 block) const
    {
        const auto &info = *binding_.info();
        if (block < 2 || block >= info.layout.filesystem_blocks ||
            (block >= info.layout.journal_first_block &&
             u64{block} - info.layout.journal_first_block < info.layout.journal_blocks))
            return false;
        for (usize index = 0; index < info.metadata_count; ++index)
            if (block >= info.metadata[index].first &&
                u64{block} - info.metadata[index].first < info.metadata[index].count)
                return false;
        return true;
    }
    [[nodiscard]] Status directory_domain()
    {
        const auto &info = *binding_.info();
        if (layout().block_size != 1024 || info.metadata_count > info.metadata.size())
            return Status::unsupported("ext4 directory replay requires 1 KiB checked metadata");
        for (usize index = 0; index < info.metadata_count; ++index)
            domain_[domain_count_++] = info.metadata[index];
        std::array<u8, 4096> bitmap{};
        for (u32 group = 0; group < raw_view_.group_count_; ++group)
        {
            const auto &g = raw_view_.groups_[group];
            if (g.flags & 1) continue;
            if (auto status = raw_view_.block(g.inode_bitmap, bitmap); !status) return status;
            if (u16(jbd2::detail::crc32c(raw_view_.seed_,
                    std::span<const u8>{bitmap}.first(raw_view_.inodes_per_group_ / 8))) != g.inode_checksum)
                return Status::io_error("ext4 directory replay inode bitmap checksum");
            for (u32 local = 0; local < raw_view_.inodes_per_group_; ++local)
            {
                const u32 number = group * raw_view_.inodes_per_group_ + local + 1;
                if (number != 2 && number < raw_view_.first_inode_) continue;
                if (!(bitmap[local / 8] & (1U << (local % 8)))) continue;
                std::array<u8, 256> inode{};
                if (auto status = raw_view_.decode_inode(number, inode); !status) return status;
                if ((ext4_checked::le16(inode, 0) & 0170000) != 0040000) continue;
                const auto owned = raw_view_.unique_initialized_directory_block(number);
                if (!owned)
                {
                    if (owned.status().code() == StatusCode::unsupported) continue;
                    return owned.status();
                }
                const u32 physical = owned.value();
                if (!ordinary_directory_target(physical))
                    return Status::unsupported("ext4 directory replay target aliases journal or metadata");
                if (directory_count_ == directories_.size() || domain_count_ == domain_.size())
                    return Status::no_memory("ext4 directory replay target capacity");
                directories_[directory_count_++] = {number, physical};
                domain_[domain_count_++] = {physical, 1};
            }
        }
        return directory_count_ ? Status::success() :
            Status::unsupported("ext4 directory replay has no admitted directory target");
    }
    [[nodiscard]] u32 table_for(u32 number) const
    {
        if (!number || number > raw_view_.inodes_) return 0;
        const u32 group = (number - 1) / raw_view_.inodes_per_group_;
        if (group >= raw_view_.group_count_) return 0;
        return raw_view_.groups_[group].table +
               ((number - 1) % raw_view_.inodes_per_group_) / 4;
    }
    [[nodiscard]] static bool permitted_rename_inode_byte(u32 inode, u32 parent, usize byte)
    {
        const bool shared = (byte >= 12 && byte < 16) || (byte >= 36 && byte < 40) ||
                            (byte >= 124 && byte < 126) || (byte >= 130 && byte < 136) ||
                            (byte >= 152 && byte < 156);
        return shared || (inode == parent && ((byte >= 16 && byte < 20) || (byte >= 136 && byte < 140)));
    }
    [[nodiscard]] Status validate_rename_inode(u32 number, u32 parent,
                                                std::span<const u8> before, std::span<const u8> after,
                                                u16 source_type)
    {
        const u16 type = ext4_checked::le16(before, 0) & 0170000;
        if (type != (number == parent ? 0040000 : source_type) ||
            (ext4_checked::le16(after, 0) & 0170000) != type)
            return Status::unsupported("ext4 directory replay inode type");
        for (usize byte = 0; byte < 256; ++byte)
            if (before[byte] != after[byte] && !permitted_rename_inode_byte(number, parent, byte))
                return Status::unsupported("ext4 directory replay inode identity or mapping change");
        const u64 old_version = u64{ext4_checked::le32(before, 36)} |
                                (u64{ext4_checked::le32(before, 152)} << 32);
        const u64 new_version = u64{ext4_checked::le32(after, 36)} |
                                (u64{ext4_checked::le32(after, 152)} << 32);
        if (old_version == ~u64{0} || new_version != old_version + 1 ||
            ext4_checked::le32(after, 12) > 0x7fffffffU ||
            (ext4_checked::le32(after, 132) & 3) ||
            (ext4_checked::le32(after, 132) >> 2) >= 1000000000U ||
            (number == parent && (ext4_checked::le32(after, 16) > 0x7fffffffU ||
             (ext4_checked::le32(after, 136) & 3) ||
             (ext4_checked::le32(after, 136) >> 2) >= 1000000000U)))
            return Status::unsupported("ext4 directory replay inode timestamp or version");
        if (auto status = ext4_checked::inode_checksum(view_.checksum_seed(), number, before); !status) return status;
        return ext4_checked::inode_checksum(view_.checksum_seed(), number, after);
    }
    [[nodiscard]] Status validate_directory_rename()
    {
        if (plan_.empty_log || plan_.committed_transactions != 1 || plan_.incomplete_tail ||
            plan_.revoked_actions || plan_.revoke_records || (plan_.actions.size() != 2 && plan_.actions.size() != 3))
            return Status::unsupported("ext4 directory replay transaction shape");
        const usize directory_index = plan_.actions.size() - 1;
        const u32 target = u32(plan_.actions[directory_index].target_block);
        u32 parent = 0;
        for (usize index = 0; index < directory_count_; ++index)
            if (directories_[index].block == target)
            {
                if (parent) return Status::io_error("ext4 directory replay shared directory target");
                parent = directories_[index].inode;
            }
        if (!parent) return Status::unsupported("ext4 directory replay target is not admitted directory data");
        for (usize index = 0; index < directory_index; ++index)
        {
            const u32 table = u32(plan_.actions[index].target_block);
            if (!view_.inode_at(table, 0))
                return Status::unsupported("ext4 directory replay requires inode-table actions first");
            for (usize prior = 0; prior < index; ++prior)
                if (plan_.actions[prior].target_block == table)
                    return Status::unsupported("ext4 directory replay duplicate inode-table target");
        }
        std::array<u8, 256> old_parent{}, new_parent{};
        if (auto status = raw_view_.decode_inode(parent, old_parent); !status) return status;
        if (auto status = view_.decode_inode(parent, new_parent); !status) return status;
        auto before = std::span<u8>{before_}.first(1024);
        auto after = std::span<u8>{after_}.first(1024);
        if (auto status = previous_block(target, directory_index, before); !status) return status;
        if (auto status = action_payload(plan_.actions[directory_index], after); !status) return status;
        if (auto status = raw_view_.directory_block(parent, old_parent, before); !status) return status;
        if (auto status = view_.directory_block(parent, new_parent, after); !status) return status;
        if (equal(before, after))
        {
            for (usize index = 0; index < directory_index; ++index)
            {
                const auto &action = plan_.actions[index];
                if (auto status = previous_block(u32(action.target_block), index, before); !status) return status;
                if (auto status = action_payload(action, after); !status) return status;
                if (!equal(before, after))
                    return Status::unsupported("ext4 directory replay already-checkpointed prefix is incomplete");
            }
            report_.validated_actions = plan_.actions.size();
            return Status::success();
        }
        u32 source = 0;
        u8 source_entry_type = 0;
        std::string_view old_name{}, new_name{};
        const auto contains_entry = [](std::span<const u8> block, u32 number, u8 type,
                                       std::string_view name) {
            for (usize offset = 0; offset < 1012; offset += ext4_checked::le16(block, offset + 4))
                if (ext4_checked::le32(block, offset) == number && block[offset + 7] == type &&
                    std::string_view{reinterpret_cast<const char *>(block.data() + offset + 8),
                                     block[offset + 6]} == name) return true;
            return false;
        };
        // Both blocks have already passed complete directory validation. Infer
        // the unique namespace pair, then require the exact checked planner
        // image; matching names alone never admit donor/padding byte changes.
        for (usize offset = 0; offset < 1012; offset += ext4_checked::le16(before, offset + 4))
        {
            const u32 number = ext4_checked::le32(before, offset);
            if (!number) continue;
            const std::string_view name{reinterpret_cast<const char *>(before.data() + offset + 8), before[offset + 6]};
            if (contains_entry(after, number, before[offset + 7], name)) continue;
            if (source || (before[offset + 7] != 1 && before[offset + 7] != 2))
                return Status::unsupported("ext4 directory replay removes multiple entries or unsupported type");
            source = number; old_name = name; source_entry_type = before[offset + 7];
        }
        for (usize offset = 0; offset < 1012; offset += ext4_checked::le16(after, offset + 4))
        {
            const u32 number = ext4_checked::le32(after, offset);
            if (!number) continue;
            const std::string_view name{reinterpret_cast<const char *>(after.data() + offset + 8), after[offset + 6]};
            if (contains_entry(before, number, after[offset + 7], name)) continue;
            if (!new_name.empty() || number != source || after[offset + 7] != source_entry_type)
                return Status::unsupported("ext4 directory replay adds multiple entries or changes inode identity");
            new_name = name;
        }
        if (!source || new_name.empty() || source == parent || source < view_.first_ordinary_inode())
            return Status::unsupported("ext4 directory replay source inode");
        usize source_links = 0, renamed_links = 0;
        for (usize offset = 0; offset < 1012; offset += ext4_checked::le16(before, offset + 4))
            if (ext4_checked::le32(before, offset) == source)
                ++source_links;
        for (usize offset = 0; offset < 1012; offset += ext4_checked::le16(after, offset + 4))
            if (ext4_checked::le32(after, offset) == source) ++renamed_links;
        if (source_links != 1 || renamed_links != 1)
            return Status::unsupported("ext4 directory replay source has another directory alias");
        std::array<u8, 256> old_source{};
        if (auto status = raw_view_.decode_inode(source, old_source); !status) return status;
        std::array<u8, 1024> old_directory{};
        for (usize index = 0; index < 1024; ++index) old_directory[index] = before[index];
        u32 source_block{};
        std::array<u8, 1024> source_directory{};
        if (source_entry_type == 2)
        {
            const auto physical = raw_view_.unique_initialized_directory_block(source);
            if (!physical) return physical.status();
            source_block = physical.value();
            if (auto status = raw_block(source_block, source_directory); !status) return status;
        }
        const auto renamed = source_entry_type == 2 ?
            Ext4DirectoryRenamePlanner::plan_directory(*binding_.info(), parent, old_parent,
                source, old_source, target, old_directory, source_block, source_directory, old_name, new_name) :
            Ext4DirectoryRenamePlanner::plan(*binding_.info(), parent, old_parent,
                source, old_source, target, old_directory, old_name, new_name);
        if (!renamed) return renamed.status();
        if (!equal(renamed.value().after, after))
            return Status::unsupported("ext4 directory replay is not the exact checked rename image");
        const u32 parent_table = table_for(parent), source_table = table_for(source);
        if (!parent_table || !source_table ||
            directory_index != (parent_table == source_table ? 1U : 2U))
            return Status::unsupported("ext4 directory replay inode-table count");
        bool parent_target{}, source_target{};
        for (usize index = 0; index < directory_index; ++index)
        {
            const auto &action = plan_.actions[index];
            const u32 table = u32(action.target_block);
            if (table != parent_table && table != source_table)
                return Status::unsupported("ext4 directory replay unrelated inode table");
            parent_target |= table == parent_table;
            source_target |= table == source_table;
            if (auto status = previous_block(table, index, before); !status) return status;
            if (auto status = action_payload(action, after); !status) return status;
            const auto first = view_.inode_at(table, 0);
            if (!first) return first.status();
            for (usize offset = 0; offset < 1024; offset += 256)
            {
                const auto old_inode = before.subspan(offset, 256), new_inode = after.subspan(offset, 256);
                if (equal(old_inode, new_inode)) continue;
                const u32 number = first.value() + u32(offset / 256);
                if (number != parent && number != source)
                    return Status::unsupported("ext4 directory replay changes unrelated inode");
                if (auto status = validate_rename_inode(number, parent, old_inode, new_inode,
                        source_entry_type == 2 ? 0040000 : 0100000); !status)
                    return status;
            }
        }
        if (!parent_target || !source_target)
            return Status::unsupported("ext4 directory replay missing parent or source table");
        report_.validated_actions = plan_.actions.size();
        return Status::success();
    }
    static void create_le16(std::span<u8> bytes, usize at, u16 value)
    {
        bytes[at] = u8(value); bytes[at + 1] = u8(value >> 8);
    }
    static void create_le32(std::span<u8> bytes, usize at, u32 value)
    {
        create_le16(bytes, at, u16(value)); create_le16(bytes, at + 2, u16(value >> 16));
    }
    [[nodiscard]] Result<u32> create_parent(u32 directory_block)
    {
        auto &view = view_;
        u32 parent{};
        std::array<u8, 4096> bitmap{};
        for (u32 group = 0; group < view.group_count_; ++group)
        {
            const auto &g = view.groups_[group];
            if (g.flags & 1) continue;
            if (auto status = view.block(g.inode_bitmap, bitmap); !status) return status;
            if (u16(jbd2::detail::crc32c(view.seed_,
                    std::span<const u8>{bitmap}.first(view.inodes_per_group_ / 8))) != g.inode_checksum)
                return Status::io_error("ext4 create replay final inode bitmap checksum");
            for (u32 local = 0; local < view.inodes_per_group_; ++local)
            {
                const u32 number = group * view.inodes_per_group_ + local + 1;
                if (number != 2 && number < view.first_inode_) continue;
                if (!(bitmap[local / 8] & (1U << (local % 8)))) continue;
                std::array<u8, 256> inode{};
                if (auto status = view.decode_inode(number, inode); !status) return status;
                if ((ext4_checked::le16(inode, 0) & 0170000) != 0040000) continue;
                const auto owned = view.unique_initialized_directory_block(number);
                if (!owned)
                {
                    if (owned.status().code() == StatusCode::unsupported) continue;
                    return owned.status();
                }
                if (owned.value() != directory_block) continue;
                if (parent) return Status::io_error("ext4 create replay shared directory target");
                parent = number;
            }
        }
        return parent ? Result<u32>{parent} :
               Result<u32>{Status::unsupported("ext4 create replay unowned directory target")};
    }
    [[nodiscard]] Status create_read_images()
    {
        for (usize index = 0; index < plan_.actions.size(); ++index)
        {
            const u32 target = u32(plan_.actions[index].target_block);
            if (auto status = action_payload(plan_.actions[index], create_after_[index]); !status)
                return status;
            if (auto status = raw_block(target, create_raw_[index]); !status)
                return status;
            for (usize previous = 0; previous < index; ++previous)
                if (plan_.actions[previous].target_block == target)
                    return Status::unsupported("ext4 create replay duplicate home target");
            create_is_after_[index] = equal(create_raw_[index], create_after_[index]);
        }
        const usize count = plan_.actions.size();
        const usize table_count = count -
            (policy_ == Ext4ReplayPolicy::bounded_directory_create ? 6U : 4U);
        if (create_is_after_[count - 1])
            for (usize index = 0; index + 1 < count; ++index)
                if (!create_is_after_[index])
                    return Status::unsupported("ext4 create replay directory crossed allocation barrier");
        bool allocation_after{};
        for (usize index = table_count; index + 1 < count; ++index)
            allocation_after |= create_is_after_[index];
        if (allocation_after)
            for (usize index = 0; index < table_count; ++index)
                if (!create_is_after_[index])
                    return Status::unsupported("ext4 create replay allocation crossed table barrier");
        return {};
    }
    [[nodiscard]] Status create_inverse_directory(u32 parent, u32 inode,
                                                  std::string_view name, usize index)
    {
        auto &old = create_before_[index];
        old = create_after_[index];
        usize previous = 1012, inserted = 1012;
        for (usize offset = 0; offset < 1012;)
        {
            const usize length = ext4_checked::le16(old, offset + 4);
            if (!length || length > 1012 - offset)
                return Status::io_error("ext4 create replay final directory bounds");
            if (ext4_checked::le32(old, offset) == inode && old[offset + 6] == name.size() &&
                std::string_view{reinterpret_cast<const char *>(old.data() + offset + 8), name.size()} == name)
            {
                if (inserted != 1012) return Status::unsupported("ext4 create replay duplicate new directory entry");
                inserted = offset;
                break;
            }
            previous = offset;
            offset += length;
        }
        if (inserted == 1012 || previous == 1012 ||
            ext4_checked::le32(old, previous) == 0)
            return Status::unsupported("ext4 create replay cannot invert directory insertion");
        const usize prior_length = ext4_checked::le16(old, previous + 4);
        const usize added_length = ext4_checked::le16(old, inserted + 4);
        if (previous + prior_length != inserted || !added_length ||
            added_length > 1012 - inserted || prior_length + added_length > 65535)
            return Status::unsupported("ext4 create replay insertion layout");
        create_le16(old, previous + 4, u16(prior_length + added_length));
        for (usize byte = inserted; byte < inserted + added_length; ++byte) old[byte] = 0;
        std::array<u8, 256> parent_inode{};
        if (auto status = view_.decode_inode(parent, parent_inode); !status) return status;
        create_le32(old, 1020, jbd2::detail::crc32c(
            ext4_checked::inode_seed(view_.seed_, parent, parent_inode),
            std::span<const u8>{old}.first(1012)));
        return view_.directory_block(parent, parent_inode, old);
    }
    [[nodiscard]] Status create_inverse_parent(std::array<u8, 1024> &table, u32 parent)
    {
        auto inode = std::span<u8>{table}.subspan(((parent - 1) % 4) * 256, 256);
        const u64 version = u64{ext4_checked::le32(inode, 36)} |
                            (u64{ext4_checked::le32(inode, 152)} << 32);
        if (!version) return Status::unsupported("ext4 create replay parent version cannot be inverted");
        create_le32(inode, 36, u32(version - 1));
        create_le32(inode, 152, u32((version - 1) >> 32));
        inode[124] = inode[125] = inode[130] = inode[131] = 0;
        const u32 checksum = jbd2::detail::crc32c(
            ext4_checked::inode_seed(view_.seed_, parent, inode), inode);
        create_le16(inode, 124, u16(checksum));
        create_le16(inode, 130, u16(checksum >> 16));
        return ext4_checked::inode_checksum(view_.seed_, parent, inode);
    }
    [[nodiscard]] Status create_inverse_group(usize index, u32 group, usize bitmap_index,
                                              bool advanced_unused)
    {
        auto &old = create_before_[index];
        old = create_after_[index];
        auto descriptor = std::span<u8>{old}.subspan(group * 32, 32);
        const u16 free_inodes = ext4_checked::le16(descriptor, 14);
        if (free_inodes == 0xffff)
            return Status::overflow("ext4 create replay group free-inode inverse");
        create_le16(descriptor, 14, free_inodes + 1);
        if (advanced_unused)
        {
            const u16 unused = ext4_checked::le16(descriptor, 28);
            if (unused >= view_.inodes_per_group_)
                return Status::overflow("ext4 create replay unused-inode inverse");
            create_le16(descriptor, 28, unused + 1);
        }
        const u16 bitmap_crc = u16(jbd2::detail::crc32c(view_.seed_,
            std::span<const u8>{create_before_[bitmap_index]}.first(view_.inodes_per_group_ / 8)));
        create_le16(descriptor, 26, bitmap_crc);
        create_le16(descriptor, 30, 0);
        const std::array<u8, 4> encoded{u8(group), u8(group >> 8), u8(group >> 16), u8(group >> 24)};
        const u16 checksum = u16(jbd2::detail::crc32c(
            jbd2::detail::crc32c(view_.seed_, encoded), descriptor));
        create_le16(descriptor, 30, checksum);
        return {};
    }
    [[nodiscard]] Status create_inverse_super(usize index)
    {
        auto &old = create_before_[index];
        old = create_after_[index];
        const u32 free_inodes = ext4_checked::le32(old, 16);
        if (free_inodes == ~u32{0})
            return Status::overflow("ext4 create replay super free-inode inverse");
        create_le32(old, 16, free_inodes + 1);
        create_le32(old, 1020,
            jbd2::detail::crc32c(~u32{0}, std::span<const u8>{old}.first(1020)));
        return {};
    }
    [[nodiscard]] Status validate_create_candidate(u32 parent, u32 inode, std::string_view name)
    {
        const usize count = plan_.actions.size(), table_count = count - 4;
        const usize bitmap_index = table_count, descriptor_index = table_count + 1;
        const usize super_index = table_count + 2, directory_index = table_count + 3;
        const u32 group = (inode - 1) / view_.inodes_per_group_;
        if (group >= view_.group_count_ || inode < view_.first_inode_ || inode == parent ||
            (binding_.create_bitmap_mismatches_ && binding_.create_bitmap_mismatch_group_ != group))
            return Status::unsupported("ext4 create replay inode group");
        const u32 parent_group = (parent - 1) / view_.inodes_per_group_;
        const u32 parent_table = view_.groups_[parent_group].table +
            ((parent - 1) % view_.inodes_per_group_) / 4;
        const u32 new_table = view_.groups_[group].table +
            ((inode - 1) % view_.inodes_per_group_) / 4;
        const u32 directory_block = u32(plan_.actions[directory_index].target_block);
        if (u32(plan_.actions[0].target_block) != parent_table ||
            (table_count == 1 && new_table != parent_table) ||
            (table_count == 2 && (new_table == parent_table ||
                u32(plan_.actions[1].target_block) != new_table)) ||
            u32(plan_.actions[bitmap_index].target_block) != view_.groups_[group].inode_bitmap ||
            u32(plan_.actions[descriptor_index].target_block) != view_.first_data_ + 1 ||
            plan_.actions[super_index].target_block != 1 ||
            !ordinary_directory_target(directory_block))
            return Status::unsupported("ext4 create replay participant identity");
        const auto new_inode = std::span<const u8>{create_after_[table_count - 1]}.subspan(
            ((inode - 1) % 4) * 256, 256);
        if ((ext4_checked::le16(new_inode, 0) & 0170000) != 0100000 ||
            ext4_checked::le32(new_inode, 4) || ext4_checked::le32(new_inode, 28) ||
            ext4_checked::le16(new_inode, 26) != 1 ||
            (ext4_checked::le32(new_inode, 132) & 3) ||
            (ext4_checked::le32(new_inode, 132) >> 2) >= 1000000000U)
            return Status::unsupported("ext4 create replay new inode shape");
        const u32 local = (inode - 1) % view_.inodes_per_group_;
        if (!(create_after_[bitmap_index][local / 8] & (1U << (local % 8))))
            return Status::io_error("ext4 create replay final allocation bit clear");

        std::array<u8, 1024> descriptor{};
        if (auto status = journal_.read_block(1, descriptor); !status) return status;
        usize tags{};
        if (auto status = codec_->descriptor(descriptor, plan_.actions[0].sequence,
                [&](const jbd2::Tag &tag) -> Status {
                    if (tags >= count || tag.target_block != plan_.actions[tags].target_block ||
                        tag.checksum != plan_.actions[tags].checksum ||
                        tag.restore_magic != plan_.actions[tags].restore_magic)
                        return Status::io_error("ext4 create witnessed descriptor changed");
                    ++tags; return {};
                }); !status) return status;
        if (tags != count) return Status::io_error("ext4 create witnessed descriptor tag count");
        if (Ext4CreateWitnessCodec::present(descriptor))
        {
            Ext4CreateWitness witness{};
            if (auto status = Ext4CreateWitnessCodec::decode(descriptor,
                    std::span<const Ext4CreateBlock>{create_after_}.first(count),
                    std::span<Ext4CreateBlock>{create_before_}.first(count), witness); !status) return status;
            if (witness.update.parent != parent || witness.inode != inode || witness.update.name != name)
                return Status::unsupported("ext4 create replay witnessed identity differs");
            const Ext4CreateSource source{inode, group, parent_table, new_table, directory_block,
                create_before_[super_index], create_before_[descriptor_index],
                create_before_[bitmap_index], create_before_[0],
                create_before_[table_count == 2 ? 1 : 0], create_before_[directory_index]};
            if (auto status = Ext4CreatePlanner::plan(*binding_.info(), witness.update, source, create_plan_);
                !status) return status;
            if (create_plan_.count != count) return Status::unsupported("ext4 create witnessed home count");
            for (usize index = 0; index < count; ++index)
                if (create_plan_.targets[index] != plan_.actions[index].target_block ||
                    !equal(create_plan_.before[index], create_before_[index]) ||
                    !equal(create_plan_.after[index], create_after_[index]) ||
                    (create_is_after_[index] ? !equal(create_raw_[index], create_after_[index]) :
                                              !equal(create_raw_[index], create_before_[index])))
                    return Status::unsupported("ext4 create witnessed image differs from canonical plan");
            // Re-encoding rejects alternative patch encodings and every reserved
            // byte, even if the outer descriptor CRC was repaired.
            auto canonical_descriptor = descriptor;
            if (auto status = Ext4CreateWitnessCodec::encode(witness.update, create_plan_, canonical_descriptor);
                !status) return status;
            if (canonical_descriptor != descriptor)
                return Status::unsupported("ext4 create witness encoding differs from canonical plan");
            return {};
        }
        // Legacy descriptors cannot prove the destroyed retained generation.
        if (ext4_checked::le32(new_inode, 100) ||
            !jbd2::detail::all_zero(std::span<const u8>{descriptor}.subspan(128, 1020 - 128)))
            return Status::unsupported("ext4 create replay requires exact durable witness");

        for (usize index = 0; index < count; ++index)
            create_before_[index] = create_raw_[index];
        for (usize index = 0; index < table_count; ++index)
            if (create_is_after_[index])
            {
                auto &table = create_before_[index];
                table = create_after_[index];
                if (u32(plan_.actions[index].target_block) == new_table)
                {
                    const usize offset = ((inode - 1) % 4) * 256;
                    for (usize byte = 0; byte < 256; ++byte) table[offset + byte] = 0;
                }
                if (u32(plan_.actions[index].target_block) == parent_table)
                    if (auto status = create_inverse_parent(table, parent); !status) return status;
            }
        if (create_is_after_[bitmap_index])
        {
            create_before_[bitmap_index] = create_after_[bitmap_index];
            create_before_[bitmap_index][local / 8] &= u8(~(1U << (local % 8)));
        }
        if (create_before_[bitmap_index][local / 8] & (1U << (local % 8)))
            return Status::unsupported("ext4 create replay old allocation bit set");
        if (create_is_after_[descriptor_index])
            if (auto status = create_inverse_group(descriptor_index, group, bitmap_index, false); !status)
                return status;
        if (create_is_after_[super_index])
            if (auto status = create_inverse_super(super_index); !status) return status;
        if (create_is_after_[directory_index])
        {
            if (auto status = create_inverse_directory(parent, inode, name, directory_index); !status)
                return status;
        }
        else
        {
            std::array<u8, 256> parent_inode{};
            if (auto status = view_.decode_inode(parent, parent_inode); !status) return status;
            if (auto status = view_.directory_block(parent, parent_inode, create_before_[directory_index]); !status)
                return status;
        }
        const u32 seconds = ext4_checked::le32(new_inode, 12);
        const u32 nanoseconds = ext4_checked::le32(new_inode, 132) >> 2;
        const InitialNodeAttributes attributes{NodeType::regular,
            u32(ext4_checked::le16(new_inode, 0) & 07777),
            u32(ext4_checked::le16(new_inode, 2)) | (u32(ext4_checked::le16(new_inode, 120)) << 16),
            u32(ext4_checked::le16(new_inode, 24)) | (u32(ext4_checked::le16(new_inode, 122)) << 16)};
        const Ext4CreateUpdate update{parent, seconds, nanoseconds, name, attributes};
        const auto canonical = [&]() -> Status
        {
            const Ext4CreateSource source{inode, group, parent_table, new_table, directory_block,
                create_before_[super_index], create_before_[descriptor_index],
                create_before_[bitmap_index], create_before_[0],
                create_before_[table_count == 2 ? 1 : 0], create_before_[directory_index]};
            const auto selected = Ext4CreatePlanner::select_inode(*binding_.info(),
                create_before_[super_index], create_before_[descriptor_index],
                create_before_[bitmap_index], group);
            if (!selected || selected.value() != inode)
                return Status::unsupported("ext4 create replay selected inode differs");
            if (auto status = Ext4CreatePlanner::plan(*binding_.info(), update, source, create_plan_); !status)
                return status;
            if (create_plan_.count != count || create_plan_.inode != inode ||
                create_plan_.parent != parent || create_plan_.directory_block != directory_block)
                return Status::unsupported("ext4 create replay planner identity");
            for (usize index = 0; index < count; ++index)
                if (create_plan_.targets[index] != plan_.actions[index].target_block ||
                    !equal(create_plan_.before[index], create_before_[index]) ||
                    !equal(create_plan_.after[index], create_after_[index]) ||
                    (create_is_after_[index] && !equal(create_raw_[index], create_plan_.after[index])) ||
                    (!create_is_after_[index] && !equal(create_raw_[index], create_plan_.before[index])))
                    return Status::unsupported("ext4 create replay image differs from canonical plan");
            return {};
        };
        if (auto status = canonical(); status) return status;
        // Once the descriptor home is checkpointed, its previous high-water
        // mark is lost. The selected bit at the frontier has two possible
        // predecessors; require the complete planner proof for the second one.
        if (create_is_after_[descriptor_index] &&
            local + ext4_checked::le16(std::span<const u8>{create_after_[descriptor_index]}
                                          .subspan(group * 32, 32), 28) + 1 == view_.inodes_per_group_)
        {
            if (auto status = create_inverse_group(descriptor_index, group, bitmap_index, true); !status)
                return status;
            return canonical();
        }
        return Status::unsupported("ext4 create replay image differs from canonical plan");
    }
    [[nodiscard]] Status validate_directory_create()
    {
        const usize count = plan_.actions.size();
        if (count != 5 && count != 6)
            return Status::unsupported("ext4 create replay transaction shape");
        if (auto status = create_read_images(); !status) return status;
        const u32 directory_block = u32(plan_.actions[count - 1].target_block);
        const auto parent = create_parent(directory_block);
        if (!parent) return parent.status();
        std::array<u8, 256> parent_inode{};
        if (auto status = view_.decode_inode(parent.value(), parent_inode); !status) return status;
        if (auto status = view_.directory_block(parent.value(), parent_inode,
                                                create_after_[count - 1]); !status) return status;
        const u32 new_table = u32(plan_.actions[count == 6 ? 1 : 0].target_block);
        const auto first = view_.inode_at(new_table, 0);
        if (!first) return Status::unsupported("ext4 create replay new table target");
        usize matches{};
        for (usize offset = 0; offset < 1012;)
        {
            const usize length = ext4_checked::le16(create_after_[count - 1], offset + 4);
            if (!length || length > 1012 - offset)
                return Status::io_error("ext4 create replay final directory entry bounds");
            const u32 inode = ext4_checked::le32(create_after_[count - 1], offset);
            const usize name_length = create_after_[count - 1][offset + 6];
            if (inode >= first.value() && inode < first.value() + 4 &&
                inode != parent.value() && name_length && name_length <= 31 &&
                create_after_[count - 1][offset + 7] == 1)
            {
                const auto name = std::string_view{
                    reinterpret_cast<const char *>(create_after_[count - 1].data() + offset + 8),
                    name_length};
                if (validate_create_candidate(parent.value(), inode, name))
                    if (++matches > 1)
                        return Status::unsupported("ext4 create replay ambiguous new inode");
            }
            offset += length;
        }
        if (matches != 1)
            return Status::io_error("ext4 create replay canonical allocation proof");
        report_.validated_actions = count;
        return {};
    }
    [[nodiscard]] Status validate_bounded_directory_create()
    {
        const usize count = plan_.actions.size();
        if (count != 7 && count != 8)
            return Status::unsupported("ext4 directory create replay transaction shape");
        if (auto status = create_read_images(); !status) return status;
        std::array<u8, 1024> descriptor{};
        if (auto status = journal_.read_block(1, descriptor); !status) return status;
        usize tags{};
        if (auto status = codec_->descriptor(descriptor, plan_.actions[0].sequence,
                [&](const jbd2::Tag &tag) -> Status {
                    if (tags >= count || tag.target_block != plan_.actions[tags].target_block ||
                        tag.checksum != plan_.actions[tags].checksum ||
                        tag.restore_magic != plan_.actions[tags].restore_magic)
                        return Status::io_error("ext4 directory create witnessed descriptor changed");
                    ++tags; return {};
                }); !status) return status;
        if (tags != count) return Status::io_error("ext4 directory create witnessed descriptor tag count");
        Ext4DirectoryCreateWitness witness{};
        if (auto status = Ext4DirectoryCreateWitnessCodec::decode(descriptor,
                std::span<const Ext4CreateBlock>{create_after_}.first(count),
                std::span<Ext4CreateBlock>{create_before_}.first(count), witness); !status) return status;
        const u32 parent = witness.update.parent, inode = witness.inode;
        const u32 physical = witness.physical;
        const u32 block_group = physical >= view_.first_data_ ?
            (physical - view_.first_data_) / view_.blocks_per_group_ : view_.group_count_;
        if (witness.update.attributes.type != NodeType::directory || parent < 2 ||
            parent > view_.inodes_ || inode < view_.first_inode_ ||
            inode > view_.inodes_ || parent == inode || physical < 3 ||
            physical >= layout().filesystem_blocks || block_group >= view_.group_count_ ||
            (physical - view_.first_data_) / view_.blocks_per_group_ != block_group)
            return Status::unsupported("ext4 directory create witnessed identity bounds");
        const u32 inode_group = (inode - 1) / view_.inodes_per_group_;
        if ((binding_.create_bitmap_mismatches_ &&
                binding_.create_bitmap_mismatch_group_ != inode_group) ||
            (binding_.first_data_bitmap_mismatches_ &&
                binding_.first_data_bitmap_mismatch_group_ != block_group))
            return Status::unsupported("ext4 directory create split bitmap group differs");
        const usize table_count = count - 6;
        const usize inode_bitmap = table_count, descriptors = table_count + 1;
        const usize superblock = table_count + 2, block_bitmap = table_count + 3;
        const usize new_directory = table_count + 4, parent_directory = table_count + 5;
        const u32 parent_group = (parent - 1) / view_.inodes_per_group_;
        const u32 parent_table = view_.groups_[parent_group].table +
            ((parent - 1) % view_.inodes_per_group_) / 4;
        const u32 new_table = view_.groups_[inode_group].table +
            ((inode - 1) % view_.inodes_per_group_) / 4;
        const u32 directory_block = u32(plan_.actions[parent_directory].target_block);
        if (plan_.actions[0].target_block != parent_table ||
            (table_count == 1 && new_table != parent_table) ||
            (table_count == 2 && (new_table == parent_table || plan_.actions[1].target_block != new_table)) ||
            plan_.actions[inode_bitmap].target_block != view_.groups_[inode_group].inode_bitmap ||
            plan_.actions[descriptors].target_block != view_.first_data_ + 1 ||
            plan_.actions[superblock].target_block != 1 ||
            plan_.actions[block_bitmap].target_block != view_.groups_[block_group].block_bitmap ||
            plan_.actions[new_directory].target_block != physical ||
            !ordinary_directory_target(directory_block))
            return Status::unsupported("ext4 directory create participant identity");
        const Ext4CreateSource inode_source{inode, inode_group, parent_table, new_table, directory_block,
            create_before_[superblock], create_before_[descriptors], create_before_[inode_bitmap],
            create_before_[0], create_before_[table_count == 2 ? 1 : 0], create_before_[parent_directory]};
        const Ext4DirectoryCreateSource source{inode_source, block_group, physical,
            create_before_[block_bitmap], create_before_[new_directory]};
        if (auto status = Ext4DirectoryCreatePlanner::plan(*binding_.info(), witness.update, source,
                create_plan_); !status) return status;
        if (create_plan_.count != count || create_plan_.inode != inode || create_plan_.parent != parent ||
            create_plan_.directory_block != directory_block ||
            create_plan_.allocated_directory_block != physical || create_plan_.block_group != block_group)
            return Status::unsupported("ext4 directory create canonical planner identity");
        for (usize home = 0; home < count; ++home)
            if (create_plan_.targets[home] != plan_.actions[home].target_block ||
                !equal(create_plan_.before[home], create_before_[home]) ||
                !equal(create_plan_.after[home], create_after_[home]) ||
                (create_is_after_[home] ? !equal(create_raw_[home], create_after_[home]) :
                                         !equal(create_raw_[home], create_before_[home])))
                return Status::unsupported("ext4 directory create witnessed image differs from canonical plan");
        auto canonical_descriptor = descriptor;
        if (auto status = Ext4DirectoryCreateWitnessCodec::encode(witness.update, create_plan_,
                canonical_descriptor); !status) return status;
        if (canonical_descriptor != descriptor)
            return Status::unsupported("ext4 directory create witness encoding differs from canonical plan");
        // Reconstruct the complete old allocation view, even when either
        // bitmap or inode-table home has already reached its final image.
        // Final-view ownership alone cannot prove that the old free block
        // was unreferenced before the new directory inode existed.
        if (auto status = create_before_view_.open(create_before_reader_, *binding_.info()); !status)
            return status;
        const auto selected = Ext4FirstDataPlanner::select_block(*binding_.info(),
            create_before_[superblock], create_before_[descriptors], create_before_[block_bitmap], block_group);
        if (!selected || selected.value() != physical)
            return Status::unsupported("ext4 directory create selected free block differs");
        const auto unreferenced = create_before_view_.unreferenced_free_data_block(physical);
        if (!unreferenced) return unreferenced.status();
        if (unreferenced.value() != physical)
            return Status::fault("ext4 directory create before ownership identity");
        const auto old_parent = create_before_view_.unique_initialized_directory_block(parent);
        if (!old_parent || old_parent.value() != directory_block)
            return old_parent ? Status::io_error("ext4 directory create before parent mapping differs") : old_parent.status();
        const auto final_parent = view_.unique_initialized_directory_block(parent);
        if (!final_parent || final_parent.value() != directory_block)
            return final_parent ? Status::io_error("ext4 directory create final parent mapping differs") : final_parent.status();
        const auto final_directory = view_.unique_initialized_directory_block(inode);
        if (!final_directory || final_directory.value() != physical)
            return final_directory ? Status::io_error("ext4 directory create final data mapping differs") : final_directory.status();
        const auto destination = view_.lookup(parent, witness.update.name);
        const auto dot = view_.lookup(inode, "."), dotdot = view_.lookup(inode, "..");
        if (!destination || destination.value() != inode || !dot || dot.value() != inode ||
            !dotdot || dotdot.value() != parent)
            return Status::io_error("ext4 directory create final namespace identity");
        report_.validated_actions = count;
        return {};
    }
    [[nodiscard]] Status first_data_read_images()
    {
        for (usize index = 0; index < 4; ++index)
        {
            const u32 target = u32(plan_.actions[index].target_block);
            if (auto status = action_payload(plan_.actions[index], create_after_[index]); !status)
                return status;
            if (auto status = raw_block(target, create_raw_[index]); !status)
                return status;
            for (usize previous = 0; previous < index; ++previous)
                if (plan_.actions[previous].target_block == target)
                    return Status::unsupported("ext4 first data replay duplicate home target");
            create_is_after_[index] = equal(create_raw_[index], create_after_[index]);
        }
        if (create_is_after_[3] &&
            (!create_is_after_[0] || !create_is_after_[1] || !create_is_after_[2]))
            return Status::unsupported("ext4 first data replay inode crossed allocation barrier");
        return {};
    }
    [[nodiscard]] Result<bool> first_data_candidate(u32 number)
    {
        using namespace ext4_checked;
        const auto after_inode = std::span<const u8>{create_after_[3]}.subspan(((number - 1) % 4) * 256, 256);
        const u32 physical = le32(after_inode, 60);
        if (physical < view_.first_data_ || physical >= layout().filesystem_blocks)
        { first_data_candidate_failure_ = Status::unsupported("ext4 first data replay candidate physical bounds"); return false; }
        const u32 group = (physical - view_.first_data_) / view_.blocks_per_group_;
        const u32 inode_group = (number - 1) / view_.inodes_per_group_;
        const u32 inode_table = inode_group < view_.group_count_ ?
            view_.groups_[inode_group].table + ((number - 1) % view_.inodes_per_group_) / 4 : 0;
        if (group >= view_.group_count_ || (binding_.first_data_bitmap_mismatches_ &&
            binding_.first_data_bitmap_mismatch_group_ != group) ||
            plan_.actions[0].target_block != view_.groups_[group].block_bitmap ||
            plan_.actions[1].target_block != view_.first_data_ + 1 ||
            plan_.actions[2].target_block != 1 ||
            plan_.actions[3].target_block != inode_table)
        { first_data_candidate_failure_ = Status::unsupported("ext4 first data replay participant identity"); return false; }
        for (usize index = 0; index < 4; ++index)
            create_before_[index] = create_after_[index];

        const u32 local = (physical - view_.first_data_) % view_.blocks_per_group_;
        if (!(create_before_[0][local / 8] & (1U << (local % 8))))
        { first_data_candidate_failure_ = Status::io_error("ext4 first data replay after bitmap bit clear"); return false; }
        create_before_[0][local / 8] &= u8(~(1U << (local % 8)));

        auto descriptor = std::span<u8>{create_before_[1]}.subspan(group * 32, 32);
        const u16 free_blocks = le16(descriptor, 12);
        if (free_blocks == 0xffff)
        { first_data_candidate_failure_ = Status::overflow("ext4 first data replay group inverse"); return false; }
        create_le16(descriptor, 12, u16(free_blocks + 1));
        create_le16(descriptor, 24, u16(jbd2::detail::crc32c(view_.seed_,
            std::span<const u8>{create_before_[0]}.first(view_.blocks_per_group_ / 8))));
        create_le16(descriptor, 30, 0);
        create_le16(descriptor, 30, u16(jbd2::detail::crc32c(
            jbd2::detail::crc32c(view_.seed_, encoded(group)), descriptor)));

        const u32 free_total = le32(create_before_[2], 12);
        if (free_total == ~u32{0})
        { first_data_candidate_failure_ = Status::overflow("ext4 first data replay super inverse"); return false; }
        create_le32(create_before_[2], 12, free_total + 1);
        create_le32(create_before_[2], 1020, jbd2::detail::crc32c(
            ~u32{0}, std::span<const u8>{create_before_[2]}.first(1020)));

        auto old_inode = std::span<u8>{create_before_[3]}.subspan(((number - 1) % 4) * 256, 256);
        const u32 created_seconds = le32(after_inode, 8), created_extra = le32(after_inode, 140);
        create_le32(old_inode, 4, 0);
        create_le32(old_inode, 12, created_seconds); create_le32(old_inode, 132, created_extra);
        create_le32(old_inode, 16, created_seconds); create_le32(old_inode, 136, created_extra);
        create_le32(old_inode, 28, 0); create_le32(old_inode, 36, 0);
        create_le16(old_inode, 42, 0);
        for (usize at = 52; at < 100; ++at) old_inode[at] = 0;
        create_le32(old_inode, 152, 0);
        old_inode[124] = old_inode[125] = old_inode[130] = old_inode[131] = 0;
        const u32 crc = jbd2::detail::crc32c(inode_seed(view_.seed_, number, old_inode), old_inode);
        create_le16(old_inode, 124, u16(crc)); create_le16(old_inode, 130, u16(crc >> 16));

        const Ext4FirstDataUpdate update{number, le32(after_inode, 4), le32(after_inode, 12),
                                         le32(after_inode, 132) >> 2};
        const Ext4FirstDataSource source{group, physical, u32(plan_.actions[3].target_block),
            create_before_[2], create_before_[1], create_before_[0], create_before_[3]};
        if (auto status = Ext4FirstDataPlanner::plan(*binding_.info(), update, source, first_data_plan_); !status)
        { first_data_candidate_failure_ = status; return false; }
        if (first_data_plan_.count != 4 || first_data_plan_.inode != number ||
            first_data_plan_.physical != physical)
        { first_data_candidate_failure_ = Status::unsupported("ext4 first data replay planner identity"); return false; }
        for (usize index = 0; index < 4; ++index)
            if (first_data_plan_.targets[index] != plan_.actions[index].target_block ||
                !equal(first_data_plan_.before[index], create_before_[index]) ||
                !equal(first_data_plan_.after[index], create_after_[index]) ||
                (create_is_after_[index] && !equal(create_raw_[index], first_data_plan_.after[index])) ||
                (!create_is_after_[index] && !equal(create_raw_[index], first_data_plan_.before[index])))
            { first_data_candidate_failure_ = Status::unsupported("ext4 first data replay image differs from canonical plan"); return false; }
        const auto unique = view_.unique_first_allocated_data_block(number);
        if (!unique) return unique.status();
        if (unique.value() != physical)
            return Status::io_error("ext4 first data replay final ownership differs");
        return true;
    }
    [[nodiscard]] Result<bool> next_data_candidate(u32 number)
    {
        using namespace ext4_checked;
        const auto reject = [&](Status status) -> Result<bool>
        { first_data_candidate_failure_ = status; return false; };
        const auto after_inode = std::span<const u8>{create_after_[3]}.subspan(((number - 1) % 4) * 256, 256);
        const u32 size = le32(after_inode, 4);
        const u16 entries = le16(after_inode, 42);
        if (size <= 1024 || size > 16 * 1024 || !entries || entries > 4)
            return reject(Status::unsupported("ext4 next data replay final inode range"));
        const usize last = 52 + usize(entries - 1) * 12;
        const u16 length = le16(after_inode, last + 4);
        const u64 end = u64{le32(after_inode, last + 8)} + length;
        if (!length || length > 32768 || le16(after_inode, last + 6) || !end || end > 0xffffffffULL)
            return reject(Status::unsupported("ext4 next data replay final extent range"));
        const u32 physical = u32(end - 1);
        Ext4NextDataPrefix prefix{};
        if (auto status = Ext4NextDataPlanner::inspect_final_inode(*binding_.info(), number,
                create_after_[3], ((number - 1) % 4) * 256, physical, prefix); !status)
            return reject(status);
        if (physical < view_.first_data_ || physical >= layout().filesystem_blocks)
            return reject(Status::unsupported("ext4 next data replay candidate physical bounds"));
        const u32 group = (physical - view_.first_data_) / view_.blocks_per_group_;
        const u32 inode_group = (number - 1) / view_.inodes_per_group_;
        const u32 table = inode_group < view_.group_count_ ?
            view_.groups_[inode_group].table + ((number - 1) % view_.inodes_per_group_) / 4 : 0;
        if (group >= view_.group_count_ || (binding_.first_data_bitmap_mismatches_ &&
            binding_.first_data_bitmap_mismatch_group_ != group) ||
            plan_.actions[0].target_block != view_.groups_[group].block_bitmap ||
            plan_.actions[1].target_block != 2 || plan_.actions[2].target_block != 1 ||
            plan_.actions[3].target_block != table)
            return reject(Status::unsupported("ext4 next data replay participant identity"));
        for (usize index = 0; index < 4; ++index)
            create_before_[index] = create_after_[index];
        const u32 local = (physical - view_.first_data_) % view_.blocks_per_group_;
        if (!(create_before_[0][local / 8] & (1U << (local % 8))))
            return reject(Status::io_error("ext4 next data replay after bitmap bit clear"));
        create_before_[0][local / 8] &= u8(~(1U << (local % 8)));
        auto descriptor = std::span<u8>{create_before_[1]}.subspan(group * 32, 32);
        const u16 free_blocks = le16(descriptor, 12);
        if (free_blocks == 0xffff)
            return reject(Status::overflow("ext4 next data replay group inverse"));
        create_le16(descriptor, 12, u16(free_blocks + 1));
        create_le16(descriptor, 24, u16(jbd2::detail::crc32c(view_.seed_,
            std::span<const u8>{create_before_[0]}.first(view_.blocks_per_group_ / 8))));
        create_le16(descriptor, 30, 0);
        create_le16(descriptor, 30, u16(jbd2::detail::crc32c(
            jbd2::detail::crc32c(view_.seed_, encoded(group)), descriptor)));
        const u32 free_total = le32(create_before_[2], 12);
        if (free_total == ~u32{0})
            return reject(Status::overflow("ext4 next data replay super inverse"));
        create_le32(create_before_[2], 12, free_total + 1);
        create_le32(create_before_[2], 1020, jbd2::detail::crc32c(
            ~u32{0}, std::span<const u8>{create_before_[2]}.first(1020)));
        const auto selected = Ext4FirstDataPlanner::select_block(*binding_.info(), create_before_[2],
            create_before_[1], create_before_[0], group);
        if (!selected) return reject(selected.status());
        if (selected.value() != physical)
            return reject(Status::unsupported("ext4 next data replay inverse block selection"));
        if (!create_is_after_[3])
        {
            // The old home supplies its actual prior mapping and timestamps.
            // Never reconstruct an old mtime/ctime from the committed inode.
            create_before_[3] = create_raw_[3];
            const Ext4NextDataUpdate update{number, prefix.old_blocks * 1024, size,
                le32(after_inode, 12), le32(after_inode, 132) >> 2};
            const Ext4NextDataSource source{group, physical, table,
                create_before_[2], create_before_[1], create_before_[0], create_before_[3]};
            if (auto status = Ext4NextDataPlanner::plan(*binding_.info(), update, source, next_data_plan_); !status)
                return reject(status);
            if (next_data_plan_.count != 4 || next_data_plan_.inode != number ||
                next_data_plan_.physical != physical || next_data_plan_.old_blocks != prefix.old_blocks)
                return reject(Status::unsupported("ext4 next data replay planner identity"));
            for (usize index = 0; index < prefix.old_blocks; ++index)
                if (next_data_plan_.old_physical[index] != prefix.old_physical[index])
                    return reject(Status::unsupported("ext4 next data replay old prefix changed"));
            for (usize index = 0; index < 4; ++index)
                if (next_data_plan_.targets[index] != plan_.actions[index].target_block ||
                    !equal(next_data_plan_.before[index], create_before_[index]) ||
                    !equal(next_data_plan_.after[index], create_after_[index]))
                    return reject(Status::unsupported("ext4 next data replay image differs from canonical plan"));
        }
        // With an after inode, the barrier already required all allocation homes
        // after. The final inode and unique prefix prove that state directly.
        for (usize index = 0; index < 4; ++index)
            if ((!create_is_after_[index] && !equal(create_raw_[index], create_before_[index])) ||
                (create_is_after_[index] && !equal(create_raw_[index], create_after_[index])))
                return reject(Status::unsupported("ext4 next data replay home differs from before/after"));
        const auto owned = view_.prove_next_data_blocks(number, physical, true);
        if (!owned) return owned.status();
        if (owned.value().old_blocks != prefix.old_blocks || owned.value().new_physical != physical)
            return reject(Status::io_error("ext4 next data replay ownership identity differs"));
        for (usize index = 0; index < prefix.old_blocks; ++index)
            if (owned.value().old_physical[index] != prefix.old_physical[index])
                return reject(Status::io_error("ext4 next data replay ownership prefix differs"));
        return true;
    }
    [[nodiscard]] Status validate_data_allocation()
    {
        if (auto status = first_data_read_images(); !status) return status;
        const auto first = view_.inode_at(u32(plan_.actions[3].target_block), 0);
        if (!first) return Status::unsupported("ext4 first data replay inode table target");
        usize matches{};
        for (u32 slot = 0; slot < 4; ++slot)
        {
            const u32 number = first.value() + slot;
            if (number < view_.first_inode_) continue;
            const auto inode = std::span<const u8>{create_after_[3]}.subspan(slot * 256, 256);
            if ((ext4_checked::le16(inode, 0) & 0170000) != 0100000)
                continue;
            const u32 size = ext4_checked::le32(inode, 4);
            const bool first = size && size <= 1024 && ext4_checked::le32(inode, 28) == 2 &&
                ext4_checked::le16(inode, 42) == 1 && ext4_checked::le16(inode, 56) == 1;
            const bool next = size > 1024 && size <= 16 * 1024;
            if ((!first || policy_ == Ext4ReplayPolicy::next_data_allocation) &&
                (!next || policy_ == Ext4ReplayPolicy::first_data_allocation))
                continue;
            const auto candidate = first ? first_data_candidate(number) : next_data_candidate(number);
            if (!candidate) return candidate.status();
            if (candidate.value() && ++matches > 1 && !create_is_after_[3])
                return Status::unsupported("ext4 first data replay ambiguous target inode");
        }
        if (!matches)
            return first_data_candidate_failure_;
        // An old inode home requires one unique complete transition proof.
        // All-after images cannot identify the historical changed inode. Every
        // home already equals its payload; authority is only idempotent cleanup.
        // Completion must not rewrite any home in this separately admitted state.
        if (create_is_after_[3])
            if (auto status = view_.prove_global_data_ownership(); !status)
                return status;
        data_all_homes_after_ = create_is_after_[3];
        report_.validated_actions = 4;
        return {};
    }
    [[nodiscard]] Status validate_change(u32 target, std::span<const u8> before, std::span<const u8> after)
    {
        using namespace ext4_checked;
        const bool metadata_family = policy_ == Ext4ReplayPolicy::checked_ownership ||
            policy_ == Ext4ReplayPolicy::checked_mode || policy_ == Ext4ReplayPolicy::mounted_inode;
        if (equal(before, after))
            return Status::success();
        if (strict_ordered_inode_ && target == 1024 / layout().block_size)
            return Status::unsupported("mounted ordered transaction cannot journal superblock");
        if (target == 1024 / layout().block_size)
        {
            const usize start = 1024 % layout().block_size;
            for (usize index = 0; index < before.size(); ++index)
            {
                const bool inside = index >= start && index - start < 1024;
                const usize offset = inside ? index - start : 1024;
                const bool bookkeeping = (offset >= 44 && offset < 54) || (offset >= 136 && offset < 200) ||
                                         (offset >= 376 && offset < 384) || (offset >= 1020 && offset < 1024);
                if (before[index] != after[index] && (!inside || !bookkeeping))
                    return Status::unsupported("ext4 preview superblock change exceeds bookkeeping policy");
            }
            const auto bytes = after.subspan(start, 1024);
            return jbd2::detail::crc32c(~u32{0}, bytes.first(1020)) == le32(bytes, 1020)
                       ? Status::success() : Status::io_error("ext4 preview payload superblock checksum");
        }
        const auto first = view_.inode_at(target, 0);
        if (!first)
            return Status::unsupported("ext4 preview allocation or structural metadata change");
        u32 changed_inodes = 0;
        for (usize offset = 0; offset < before.size(); offset += 256)
        {
            const auto old_inode = before.subspan(offset, 256), new_inode = after.subspan(offset, 256);
            if (equal(old_inode, new_inode))
                continue;
            if ((strict_ordered_inode_ || metadata_family) && ++changed_inodes != 1)
                return Status::unsupported("mounted ordered transaction changes multiple inodes");
            const u32 number = first.value() + u32(offset / 256);
            const u16 type = le16(old_inode, 0) & 0170000;
            const bool mode_changed = le16(old_inode, 0) != le16(new_inode, 0);
            const bool owners_changed = le16(old_inode, 2) != le16(new_inode, 2) || le16(old_inode, 24) != le16(new_inode, 24) ||
                le32(old_inode, 120) != le32(new_inode, 120);
            const bool data_fields = le32(old_inode, 4) != le32(new_inode, 4) ||
                le32(old_inode, 16) != le32(new_inode, 16) || le32(old_inode, 136) != le32(new_inode, 136);
            // One admitted union, selected from actual bytes before any replay:
            // owner/mode/directory changes never fall through to data policy.
            const bool metadata_change = metadata_family && (policy_ != Ext4ReplayPolicy::mounted_inode ||
                type == 0040000 || owners_changed || mode_changed || !data_fields);
            const bool mode_change = policy_ == Ext4ReplayPolicy::checked_mode ||
                (policy_ == Ext4ReplayPolicy::mounted_inode && mode_changed && !owners_changed);
            const bool ownership_change = metadata_change && !mode_change;
            if (number < view_.first_ordinary_inode() || (type != 0100000 && type != 0040000) ||
                (le16(new_inode, 0) & 0170000) != type)
                return Status::unsupported("ext4 preview only changes ordinary regular inode or same-type directory metadata");
            if (strict_ordered_inode_ && !metadata_change && type != 0100000)
                return Status::unsupported("mounted ordered transaction requires regular inode");
            for (usize byte = 0; byte < 256; ++byte)
            {
                const bool ownership = (byte >= 2 && byte < 4) || (byte >= 24 && byte < 26) ||
                                       (byte >= 120 && byte < 124);
                const bool data_field = !metadata_change && allow_ordered_data_ && type == 0100000 &&
                    ((byte >= 4 && byte < 8) || (byte >= 16 && byte < 20) ||
                     (byte >= 136 && byte < 140));
                const bool permitted = data_field || ((!strict_ordered_inode_ || metadata_change) && byte < 2) || (byte >= 12 && byte < 16) ||
                    (byte >= 36 && byte < 40) || (byte >= 124 && byte < 126) || (byte >= 130 && byte < 136) ||
                    (byte >= 152 && byte < 156) || ((ownership_change || (!metadata_change && !strict_ordered_inode_ && type == 0100000)) && ownership);
                if (old_inode[byte] != new_inode[byte] && !permitted)
                    return Status::unsupported("ext4 preview inode mapping, allocation or namespace change");
            }
            if ((le32(new_inode, 132) >> 2) >= 1000000000)
                return Status::io_error("ext4 preview inode ctime nanoseconds");
            if (ownership_change)
            {
                const bool changed = le16(old_inode, 2) != le16(new_inode, 2) ||
                    le16(old_inode, 24) != le16(new_inode, 24) || le32(old_inode, 120) != le32(new_inode, 120);
                const u16 permissions = (le16(old_inode, 0) & 07777u) & (changed ? ~06000u : ~0u);
                if ((le16(new_inode, 0) & 07777u) != permissions)
                    return Status::unsupported("checked ownership changes only owner, cleared privilege bits and bounded ctime");
            }
            if (metadata_change && (le32(new_inode, 12) > 0x7fffffffU || (le32(new_inode, 132) & 3u)))
                return Status::unsupported("checked metadata requires signed32 ctime without extra epoch bits");
            if (!metadata_change && allow_ordered_data_ && type == 0100000)
            {
                const u32 old_size = le32(old_inode, 4), new_size = le32(new_inode, 4);
                if (!old_size || !new_size || (old_size + 1023ULL) / 1024 != (new_size + 1023ULL) / 1024 ||
                    (le32(new_inode, 136) >> 2) >= 1000000000)
                    return Status::unsupported("ext4 preview ordered-data size or mtime");
            }
            if (strict_ordered_inode_ || metadata_family)
            {
                const u64 old_version = u64{le32(old_inode, 36)} | (u64{le32(old_inode, 152)} << 32);
                const u64 new_version = u64{le32(new_inode, 36)} | (u64{le32(new_inode, 152)} << 32);
                if (old_version == ~u64{0} || new_version != old_version + 1)
                    return Status::unsupported("mounted ordered inode version step");
            }
            if (const auto status = inode_checksum(view_.checksum_seed(), number, old_inode); !status)
                return status;
            if (const auto status = inode_checksum(view_.checksum_seed(), number, new_inode); !status)
                return status;
            const auto checked = view_.inode(number);
            if (!checked)
                return checked.status();
        }
        return Status::success();
    }
    [[nodiscard]] Status fail(Status status)
    {
        report_.phase = Ext4PreviewPhase::failed;
        report_.status = status;
        return status;
    }
    void retire_if_failed(Status status)
    {
        if (!view_.valid())
            static_cast<void>(fail(status));
    }

    driver::BlockDevice *device_{};
    driver::BlockGeometry geometry_{};
    Ext4RecoveryBinding binding_{};
    JournalReader journal_{this};
    FinalReader final_{this};
    RawReader raw_{this};
    CreateBeforeReader create_before_reader_{this};
    Ext4ReadOnlyView<FinalReader> view_{};
    Ext4ReadOnlyView<RawReader> raw_view_{};
    Ext4ReadOnlyView<CreateBeforeReader> create_before_view_{};
    std::optional<jbd2::Codec> codec_{};
    jbd2::ReplayPlan plan_{};
    Ext4PreviewReport report_{};
    std::array<jbd2::ReplayAction, ActionCapacity> actions_{};
    std::array<jbd2::Revocation, RevokeCapacity> revocations_{};
    std::array<jbd2::BlockRange, Ext4RecoveryInfo::group_capacity * 4 + 128> domain_{};
    std::array<DirectoryTarget, 128> directories_{};
    usize domain_count_{}, directory_count_{};
    std::array<u8, 4096> record_{}, before_{}, after_{};
    std::array<std::array<u8, 1024>, 8> create_raw_{}, create_after_{}, create_before_{};
    std::array<bool, 8> create_is_after_{};
    Ext4CreatePlan create_plan_{};
    Ext4FirstDataPlan first_data_plan_{};
    Ext4NextDataPlan next_data_plan_{};
    bool data_all_homes_after_{};
    Status first_data_candidate_failure_{Status::io_error("ext4 first data replay canonical allocation proof")};
    bool require_one_kib_{};
    bool allow_ordered_data_{};
    bool strict_ordered_inode_{};
    Ext4ReplayPolicy policy_{};
};

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
                ++report_.checked_inodes;
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

enum class MetadataOwnerState : u8 { unused, opening, ready, closing, closed, failed };
enum class MetadataBufferState : u8 { unused, loading, ready, failed, exhausted };
struct MetadataOwnerReport
{
    MetadataOwnerState state{MetadataOwnerState::unused};
    StatusCode first_error{StatusCode::ok};
    u64 device_identity{}, registration_generation{}, session_serial{}, first_sector{}, sectors{}, blocks{};
    usize read_pins{}, resident_blocks{};
    bool in_flight{}, transaction_active{};
};
struct MetadataBufferReport
{
    MetadataBufferState state{MetadataBufferState::unused};
    u64 block{}, generation{}, transaction_serial{};
    usize read_pins{};
    bool staged_dirty{};
};

template <class Guard, class Session>
concept MetadataReadDispatchGuard = requires(Guard &guard, Session &session, u64 sector, usize bytes, Status status)
{
    { guard.begin_read(session, sector, bytes) } -> std::same_as<Status>;
    { guard.finish_read(session, status) } -> std::same_as<Status>;
};

// Caller-provisioned ownership/private-version slice. No home writeback or commit.
// Owner, registry and transport storage outlive all handles and admitted calls.
// Quarantine retains backing until external fencing or transport lifetime end.
template <usize Capacity = 8, usize BlockBytes = 1024, usize DeviceCapacity = 4>
    requires (Capacity > 0 && (BlockBytes == 1024 || BlockBytes == 2048 || BlockBytes == 4096))
class MetadataBuffers final
{
    template <usize, usize> friend class Ext4OwnedTransaction;
    template <usize> friend class Ext4EmptyOrphanRecoveryOwner;
  public:
    using Registry = block::BlockSessions<DeviceCapacity>;
    class Pin final
    {
        friend class MetadataBuffers;
        template <usize, usize> friend class Ext4OwnedTransaction;
      public:
        Pin() = default;
        ~Pin() { if (owner_) static_cast<void>(owner_->release(*this)); }
        Pin(const Pin &) = delete;
        Pin &operator=(const Pin &) = delete;
        Pin(Pin &&) = delete;
        Pin &operator=(Pin &&) = delete;
        [[nodiscard]] Status read(usize offset, std::span<std::byte> out) const
        { return owner_ ? owner_->read(*this, offset, out) : Status::not_initialized("metadata pin is unbound"); }
        [[nodiscard]] Status release()
        { return owner_ ? owner_->release(*this) : Status::not_initialized("metadata pin is unbound"); }
        [[nodiscard]] Result<MetadataBufferReport> report() const
        { return owner_ ? owner_->inspect(*this) : Result<MetadataBufferReport>{Status::not_initialized("metadata pin is unbound")}; }
      private:
        MetadataBuffers *owner_{};
        usize slot_{};
        u64 generation_{};
        bool active_{};
    };
    class Transaction final
    {
        friend class MetadataBuffers;
        template <usize, usize> friend class Ext4OwnedTransaction;
      public:
        Transaction() = default;
        ~Transaction() { if (owner_) static_cast<void>(owner_->discard(*this)); }
        Transaction(const Transaction &) = delete;
        Transaction &operator=(const Transaction &) = delete;
        Transaction(Transaction &&) = delete;
        Transaction &operator=(Transaction &&) = delete;
        [[nodiscard]] Status edit(u64 block, usize offset, std::span<const std::byte> bytes)
        { return owner_ ? owner_->edit(*this, block, offset, bytes) : Status::not_initialized("metadata transaction is unbound"); }
        [[nodiscard]] Status read(u64 block, usize offset, std::span<std::byte> out) const
        { return owner_ ? owner_->read(*this, block, offset, out) : Status::not_initialized("metadata transaction is unbound"); }
        [[nodiscard]] Status discard()
        { return owner_ ? owner_->discard(*this) : Status::not_initialized("metadata transaction is unbound"); }
      private:
        MetadataBuffers *owner_{};
        u64 serial_{};
        bool active_{};
    };

    MetadataBuffers() = default;
    ~MetadataBuffers()
    {
        // Explicit close establishes drainage. Destruction is never a reset.
        if (report_.state == MetadataOwnerState::ready)
            static_cast<void>(abort(Status::interrupted("metadata owner destroyed without close")));
    }
    MetadataBuffers(const MetadataBuffers &) = delete;
    MetadataBuffers &operator=(const MetadataBuffers &) = delete;
    MetadataBuffers(MetadataBuffers &&) = delete;
    MetadataBuffers &operator=(MetadataBuffers &&) = delete;

    [[nodiscard]] Status open(Registry &registry, typename Registry::Registration key,
                              block::SessionBounds bounds, block::SessionAccess access)
    {
        {
            smp::ScopedSpinLock guard(lock_);
            if (report_.state != MetadataOwnerState::unused) return Status::already_exists("metadata owner is one-shot");
            report_.state = MetadataOwnerState::opening;
        }
        auto status = registry.acquire(key, bounds, access, session_);
        if (!status)
        {
            smp::ScopedSpinLock guard(lock_);
            report_.state = MetadataOwnerState::closed; report_.first_error = status.code(); return status;
        }
        const auto geometry = session_.geometry();
        const auto identity = session_.report();
        const auto transfer_limit = session_.transfer_limit();
        const auto binding = session_.observe_binding();
        if (!identity) status = identity.status();
        else if (!transfer_limit) status = transfer_limit.status();
        else if (!binding) status = binding.status();
        else if (geometry.block_size > BlockBytes || !geometry.block_size ||
                 BlockBytes % geometry.block_size || bounds.count % (BlockBytes / geometry.block_size))
            status = Status::invalid_argument("metadata block and partition geometry");
        else if (transfer_limit.value() < BlockBytes)
            status = Status::unsupported("metadata block exceeds session transfer limit");
        if (!status)
        {
            const auto closed = session_.close();
            smp::ScopedSpinLock guard(lock_);
            report_.first_error = status.code();
            if (closed) report_.state = MetadataOwnerState::closed;
            else fail(closed.code());
            return status;
        }
        smp::ScopedSpinLock guard(lock_);
        report_.device_identity = identity.value().identity;
        report_.registration_generation = identity.value().registration_generation;
        report_.session_serial = identity.value().session_serial;
        report_.first_sector = bounds.first; report_.sectors = bounds.count;
        sectors_per_block_ = BlockBytes / geometry.block_size;
        report_.blocks = bounds.count / sectors_per_block_;
        writable_ = access == block::SessionAccess::writable;
        session_binding_ = binding.value();
        report_.state = MetadataOwnerState::ready;
        return {};
    }

    [[nodiscard]] Status pin(u64 block, Pin &pin)
    {
        UnguardedRead guard;
        return pin_impl(block, pin, guard);
    }

    // Hooks synchronously account for a miss; they must not perform transport
    // I/O, block or reenter this cache. Hits consume no transport allowance.
    template <class Guard> requires MetadataReadDispatchGuard<Guard, typename Registry::Session>
    [[nodiscard]] Status pin(u64 block, Pin &pin, Guard &guard)
    { return pin_impl(block, pin, guard); }

  private:
    struct UnguardedRead
    {
        Status begin_read(typename Registry::Session &, u64, usize) { return {}; }
        Status finish_read(typename Registry::Session &, Status) { return {}; }
    };

    template <class Guard>
    [[nodiscard]] Status pin_impl(u64 block, Pin &pin, Guard &read_guard)
    {
        Entry *entry{};
        constexpr bool deferred = [] {
            if constexpr (requires { Guard::defer_cache_begin; }) return Guard::defer_cache_begin;
            else return false;
        }();
        usize reserved_slot = Capacity;
        MetadataBufferReport reserved{};
        {
            smp::ScopedSpinLock guard(lock_);
            if (pin.owner_) return Status::already_exists("metadata pin is one-shot");
            if (auto status = admission(); !status) return status;
            if (block >= report_.blocks) return Status::invalid_argument("metadata block outside partition");
            if (report_.read_pins == ~usize{0}) return Status::overflow("metadata pin count exhausted");
            for (usize index = 0; index < Capacity; ++index)
            {
                auto &candidate = entries_[index];
                if ((candidate.report.state == MetadataBufferState::ready || candidate.report.state == MetadataBufferState::loading) &&
                    candidate.report.block == block)
                {
                    if (candidate.report.state != MetadataBufferState::ready) return Status::busy("metadata block is loading");
                    bind_pin(pin, candidate, index);
                    return {};
                }
            }
            if (report_.in_flight) return Status::busy("metadata read already in flight");
            usize selected = Capacity;
            for (usize index = 0; index < Capacity; ++index)
            {
                auto &candidate = entries_[index];
                if ((candidate.report.state != MetadataBufferState::unused && candidate.report.state != MetadataBufferState::ready) ||
                    candidate.report.read_pins || candidate.report.transaction_serial) continue;
                if (candidate.report.generation == ~u64{0}) continue;
                selected = index; break;
            }
            // Do not evict, advance generations or bind a pin for a refused
            // reservation. Exhaustion bookkeeping is also deferred until here.
            if constexpr (!deferred)
            if (selected != Capacity)
                if (auto status = read_guard.begin_read(session_, block * sectors_per_block_, BlockBytes); !status)
                {
                    // An accounting refusal can leave the Session owned. An
                    // integrity rejection can quarantine it; then cached hits
                    // and already-retained pins must stop exposing data.
                    const auto session = session_.report();
                    if (!session || session.value().state != block::DeviceState::owned)
                        fail(status.code());
                    return status;
                }
            if constexpr (deferred)
            {
                if (selected == Capacity) return Status::no_memory("no clean unpinned metadata buffer");
                // Reserve only the serialized miss; no slot, generation or Pin
                // is changed before the borrowed Owner guard can refuse.
                reserved_slot = selected; reserved = entries_[selected].report;
                report_.in_flight = true; entry = &entries_[selected];
            }
            else
            {
            for (usize index = 0; index < selected; ++index)
            {
                auto &candidate = entries_[index];
                if ((candidate.report.state == MetadataBufferState::unused || candidate.report.state == MetadataBufferState::ready) &&
                    !candidate.report.read_pins && !candidate.report.transaction_serial && candidate.report.generation == ~u64{0})
                {
                    if (candidate.report.state == MetadataBufferState::ready) --report_.resident_blocks;
                    candidate.report.state = MetadataBufferState::exhausted;
                }
            }
            if (selected != Capacity)
            {
                auto &candidate = entries_[selected];
                const auto generation = candidate.report.generation + 1;
                if (candidate.report.state == MetadataBufferState::unused) ++report_.resident_blocks;
                candidate.report = {}; candidate.report.block = block; candidate.report.generation = generation;
                candidate.report.state = MetadataBufferState::loading;
                candidate.report.read_pins = 1; ++report_.read_pins;
                pin.owner_ = this; pin.slot_ = selected; pin.generation_ = generation;
                report_.in_flight = true; entry = &candidate;
            }
            if (!entry) return Status::no_memory("no clean unpinned metadata buffer");
            }
        }
        if constexpr (deferred)
        {
            const auto admitted = [&]() -> Status {
                if constexpr (requires { read_guard.begin_cache_read(session_, block * sectors_per_block_,
                    BlockBytes, reserved_slot, reserved.generation + 1); })
                    return read_guard.begin_cache_read(session_, block * sectors_per_block_, BlockBytes,
                        reserved_slot, reserved.generation + 1);
                else return read_guard.begin_read(session_, block * sectors_per_block_, BlockBytes);
            }();
            smp::ScopedSpinLock guard(lock_);
            if (!admitted)
            {
                report_.in_flight = false;
                const auto session = session_.report();
                if (!session || session.value().state != block::DeviceState::owned) fail(admitted.code());
                return admitted;
            }
            const auto &actual = entry->report;
            if (!report_.in_flight || report_.state != MetadataOwnerState::ready || reserved_slot >= Capacity ||
                actual.state != reserved.state || actual.block != reserved.block || actual.generation != reserved.generation ||
                actual.read_pins != reserved.read_pins || actual.transaction_serial != reserved.transaction_serial ||
                actual.staged_dirty != reserved.staged_dirty)
            {
                const auto status = Status::fault("metadata deferred miss reservation changed");
                static_cast<void>(session_.abort(status)); report_.in_flight = false; fail(status.code()); return status;
            }
            const u64 generation = actual.generation + 1;
            if (actual.state == MetadataBufferState::unused) ++report_.resident_blocks;
            entry->report = {}; entry->report.block = block; entry->report.generation = generation;
            entry->report.state = MetadataBufferState::loading; entry->report.read_pins = 1; ++report_.read_pins;
            pin.owner_ = this; pin.slot_ = reserved_slot; pin.generation_ = generation;
        }
        // Entry backing is retained across the unlocked synchronous request.
        auto status = [&]() -> Status {
            if constexpr (requires { session_.read_blocks_guarded(block * sectors_per_block_, entry->canonical, read_guard); })
                return session_.read_blocks_guarded(block * sectors_per_block_, entry->canonical, read_guard);
            else return session_.read_blocks(block * sectors_per_block_, entry->canonical);
        }();
        const auto accounted = read_guard.finish_read(session_, status);
        if (status && !accounted) status = accounted;
        if (!status) static_cast<void>(session_.abort(status));
        smp::ScopedSpinLock guard(lock_);
        report_.in_flight = false;
        if (!status || report_.state == MetadataOwnerState::failed)
        {
            if (!status) fail(status.code());
            --entry->report.read_pins; --report_.read_pins;
            entry->report.state = MetadataBufferState::failed;
            return status ? Status::io_error("metadata owner aborted in flight") : status;
        }
        entry->report.state = MetadataBufferState::ready;
        pin.active_ = true;
        return {};
    }

  public:
    [[nodiscard]] Status begin(std::span<Pin *const> pins, Transaction &transaction)
    {
        smp::ScopedSpinLock guard(lock_);
        if (transaction.owner_) return Status::already_exists("metadata transaction is one-shot");
        if (auto status = admission(); !status) return status;
        if (!writable_) return Status::denied("readonly metadata owner");
        if (report_.in_flight || transaction_) return Status::busy("metadata owner has active work");
        if (pins.empty() || pins.size() > Capacity) return Status::invalid_argument("metadata transaction credit count");
        if (transaction_serial_ == ~u64{0}) return Status::overflow("metadata transaction generation exhausted");
        for (usize index = 0; index < pins.size(); ++index)
        {
            if (!pins[index] || !find(*pins[index])) return Status::bad_descriptor("foreign or released metadata participant");
            for (usize prior = 0; prior < index; ++prior)
                if (pins[prior]->slot_ == pins[index]->slot_)
                    return Status::invalid_argument("duplicate canonical metadata participant");
        }
        // Every participant and all caller-provisioned credits exist before edits.
        transaction.owner_ = this; transaction.serial_ = ++transaction_serial_; transaction.active_ = true;
        transaction_ = &transaction; report_.transaction_active = true;
        for (auto *pin : pins)
        {
            auto &entry = entries_[pin->slot_];
            entry.staged = entry.canonical;
            entry.report.transaction_serial = transaction.serial_; entry.report.staged_dirty = false;
        }
        return {};
    }

    [[nodiscard]] Status close()
    {
        {
            smp::ScopedSpinLock guard(lock_);
            if (auto status = admission(); !status) return status;
            if (report_.read_pins || report_.in_flight || transaction_)
                return Status::busy("metadata owner has retained work");
            report_.state = MetadataOwnerState::closing;
        }
        const auto status = session_.close();
        smp::ScopedSpinLock guard(lock_);
        if (!status) { fail(status.code()); return status; }
        report_.state = MetadataOwnerState::closed; return {};
    }
    [[nodiscard]] Status abort(Status reason)
    {
        if (reason) return Status::invalid_argument("metadata integrity abort requires error");
        {
            smp::ScopedSpinLock guard(lock_);
            if (auto status = admission(); !status) return status;
            fail(reason.code());
        }
        static_cast<void>(session_.abort(reason));
        return {};
    }
    [[nodiscard]] MetadataOwnerReport report() const
    {
        smp::ScopedSpinLock guard(lock_);
        auto result = report_;
        if (result.state == MetadataOwnerState::ready)
        {
            const auto session = session_.report();
            if (!session || session.value().state != block::DeviceState::owned ||
                session.value().identity != result.device_identity ||
                session.value().registration_generation != result.registration_generation ||
                session.value().session_serial != result.session_serial)
            {
                result.state = MetadataOwnerState::failed;
                result.first_error = session && session.value().first_error != StatusCode::ok ?
                    session.value().first_error : StatusCode::fault;
            }
        }
        return result;
    }

  private:
    struct Entry
    {
        MetadataBufferReport report{};
        alignas(64) std::array<std::byte, BlockBytes> canonical{}, staged{};
    };
    enum class TypedCacheAction : u8 { unchanged, replace, retire };
    struct TypedCacheChange
    {
        u64 block{};
        TypedCacheAction action{TypedCacheAction::unchanged};
        std::span<const std::byte> expected{}, replacement{};
    };
    // Construction is confined to the canonical owner. This is retained private
    // state, not an exclusion lease or a public report carrying write authority.
    class TypedReconciliation final
    {
        friend class MetadataBuffers;
        template <usize, usize> friend class Ext4OwnedTransaction;
        TypedReconciliation() = default;
        ~TypedReconciliation() = default;
        TypedReconciliation(const TypedReconciliation &) = delete;
        TypedReconciliation &operator=(const TypedReconciliation &) = delete;
        TypedReconciliation(TypedReconciliation &&) = delete;
        TypedReconciliation &operator=(TypedReconciliation &&) = delete;
        enum class Phase : u8 { unused, prepared, normalized, published, failed };
        const TypedReconciliation *original_{};
        MetadataBuffers *cache_{};
        const typename Registry::Session *session_{};
        Phase phase_{Phase::unused};
        MetadataOwnerReport report_{};
        std::array<Entry, Capacity> entries_{};
        std::array<TypedCacheAction, Capacity> actions_{};
        std::array<MetadataBufferReport, Capacity> final_reports_{};
        std::array<std::array<std::byte, BlockBytes>, Capacity> final_images_{};
        std::array<u64, 32> targets_{};
        std::array<TypedCacheAction, 32> target_actions_{};
        usize targets_count_{}, resident_after_{}, sectors_per_block_{};
        u64 transaction_serial_{}, seal_{};
        bool writable_{};
        block::SessionBindingObservation session_binding_{};
    };
    [[nodiscard]] static bool same_report(const MetadataBufferReport &a, const MetadataBufferReport &b)
    {
        return a.state == b.state && a.block == b.block && a.generation == b.generation &&
            a.transaction_serial == b.transaction_serial && a.read_pins == b.read_pins &&
            a.staged_dirty == b.staged_dirty;
    }
    [[nodiscard]] static bool same_report(const MetadataOwnerReport &a, const MetadataOwnerReport &b)
    {
        return a.state == b.state && a.first_error == b.first_error &&
            a.device_identity == b.device_identity && a.registration_generation == b.registration_generation &&
            a.session_serial == b.session_serial && a.first_sector == b.first_sector && a.sectors == b.sectors &&
            a.blocks == b.blocks && a.read_pins == b.read_pins && a.resident_blocks == b.resident_blocks &&
            a.in_flight == b.in_flight && a.transaction_active == b.transaction_active;
    }
    [[nodiscard]] Status typed_cache_idle() const
    {
        if (report_.state != MetadataOwnerState::ready || report_.first_error != StatusCode::ok || !writable_)
            return Status::denied("typed cache reconciliation requires a healthy writable owner");
        if (report_.in_flight || report_.read_pins || transaction_ || report_.transaction_active)
            return Status::busy("typed cache reconciliation requires drained cache work");
        const auto binding = session_.observe_binding();
        if (!binding || binding.value() != session_binding_)
            return Status::fault("typed cache authoritative original Session binding changed");
        const auto &original = binding.value();
        if (report_.first_sector != original.first_sector || report_.sectors != original.sectors ||
            !original.sector_bytes || original.sector_bytes > BlockBytes ||
            BlockBytes % original.sector_bytes || sectors_per_block_ != BlockBytes / original.sector_bytes ||
            writable_ != original.writable)
            return Status::fault("typed cache prefix differs from its original Session binding");
        const auto current = session_.report();
        if (!current || current.value().state != block::DeviceState::owned ||
            current.value().first_error != StatusCode::ok || current.value().in_flight || current.value().pending_writes ||
            current.value().identity != report_.device_identity ||
            current.value().registration_generation != report_.registration_generation ||
            current.value().session_serial != report_.session_serial)
            return Status::fault("typed cache original Session identity or idle state changed");
        if (!sectors_per_block_ || !report_.sectors || report_.sectors % sectors_per_block_ ||
            report_.blocks != report_.sectors / sectors_per_block_)
            return Status::fault("typed cache geometry or block count changed");
        usize residents = 0;
        for (usize index = 0; index < Capacity; ++index)
        {
            const auto &entry = entries_[index].report;
            if (entry.read_pins || entry.transaction_serial || entry.staged_dirty)
                return Status::busy("typed cache slot retains pins or staged ownership");
            if (entry.state == MetadataBufferState::ready)
            {
                if (!entry.generation || entry.block >= report_.blocks)
                    return Status::fault("typed cache resident identity is invalid");
                for (usize prior = 0; prior < index; ++prior)
                    if (entries_[prior].report.state == MetadataBufferState::ready &&
                        entries_[prior].report.block == entry.block)
                        return Status::fault("typed cache has duplicate resident keys");
                ++residents;
            }
            else if (entry.state != MetadataBufferState::unused && entry.state != MetadataBufferState::exhausted)
                return Status::fault("typed cache has an undrained slot state");
            else if (entry.state == MetadataBufferState::exhausted && entry.generation != ~u64{0})
                return Status::fault("typed cache exhausted generation is invalid");
        }
        if (residents != report_.resident_blocks)
            return Status::fault("typed cache resident counter changed");
        return {};
    }
    [[nodiscard]] static bool overlaps_storage(const void *data, usize bytes, const void *storage, usize extent)
    {
        if (!bytes) return false;
        const uptr first = reinterpret_cast<uptr>(data), other = reinterpret_cast<uptr>(storage);
        if (!data || first > ~uptr{0} - bytes || other > ~uptr{0} - extent) return true;
        return first < other + extent && other < first + bytes;
    }
    [[nodiscard]] static u64 typed_plan_seal(const TypedReconciliation &plan)
    {
        u64 hash = 14695981039346656037ULL;
        const auto mix = [&](u64 value) {
            for (usize n = 0; n < sizeof(value); ++n)
            { hash ^= (value >> (8 * n)) & 0xff; hash *= 1099511628211ULL; }
        };
        const auto buffer = [&](const MetadataBufferReport &r) {
            mix(static_cast<u8>(r.state)); mix(r.block); mix(r.generation); mix(r.transaction_serial);
            mix(r.read_pins); mix(r.staged_dirty);
        };
        mix(reinterpret_cast<uptr>(plan.original_)); mix(reinterpret_cast<uptr>(plan.cache_));
        mix(reinterpret_cast<uptr>(plan.session_)); mix(static_cast<u8>(plan.phase_));
        const auto &r = plan.report_;
        mix(static_cast<u8>(r.state)); mix(static_cast<u32>(r.first_error)); mix(r.device_identity);
        mix(r.registration_generation); mix(r.session_serial); mix(r.first_sector); mix(r.sectors); mix(r.blocks);
        mix(r.read_pins); mix(r.resident_blocks); mix(r.in_flight); mix(r.transaction_active);
        mix(plan.targets_count_); mix(plan.resident_after_); mix(plan.sectors_per_block_);
        mix(plan.transaction_serial_); mix(plan.writable_);
        const auto &binding = plan.session_binding_;
        mix(reinterpret_cast<uptr>(binding.service)); mix(reinterpret_cast<uptr>(binding.session));
        mix(reinterpret_cast<uptr>(binding.device)); mix(binding.registration_slot); mix(binding.maximum_transfer);
        mix(binding.identity); mix(binding.registration_generation); mix(binding.session_serial);
        mix(binding.first_sector); mix(binding.sectors); mix(binding.device_sectors);
        mix(binding.sector_bytes); mix(binding.device_sector_bytes); mix(binding.writable); mix(binding.device_writable);
        for (usize index = 0; index < Capacity; ++index)
        {
            const auto &entry = plan.entries_[index];
            buffer(entry.report); mix(static_cast<u8>(plan.actions_[index])); buffer(plan.final_reports_[index]);
            for (usize byte = 0; byte < BlockBytes; ++byte)
            {
                mix(std::to_integer<u8>(entry.canonical[byte])); mix(std::to_integer<u8>(entry.staged[byte]));
                mix(std::to_integer<u8>(plan.final_images_[index][byte]));
            }
        }
        for (usize index = 0; index < plan.targets_.size(); ++index)
        { mix(plan.targets_[index]); mix(static_cast<u8>(plan.target_actions_[index])); }
        return hash;
    }
    [[nodiscard]] Status prepare_typed_reconciliation(std::span<const TypedCacheChange> changes,
                                                       TypedReconciliation &plan)
    {
        smp::ScopedSpinLock guard(lock_);
        if (plan.phase_ != TypedReconciliation::Phase::unused || plan.original_ || plan.cache_ || plan.session_)
            return Status::already_exists("typed cache plan is one-shot");
        if (auto status = typed_cache_idle(); !status) return status;
        if (changes.size() > plan.targets_.size())
            return Status::unsupported("typed cache reconciliation exceeds bounded target profile");
        if (overlaps_storage(&plan, sizeof(plan), this, sizeof(*this)) ||
            overlaps_storage(changes.data(), changes.size_bytes(), this, sizeof(*this)) ||
            overlaps_storage(changes.data(), changes.size_bytes(), &plan, sizeof(plan)))
            return Status::invalid_argument("typed cache request aliases retained storage");
        usize retirements = 0;
        for (usize target = 0; target < changes.size(); ++target)
        {
            const auto &change = changes[target];
            if (change.block >= report_.blocks ||
                (change.action != TypedCacheAction::replace && change.action != TypedCacheAction::retire) ||
                change.expected.size() != BlockBytes || !change.expected.data() ||
                (change.action == TypedCacheAction::replace &&
                    (change.replacement.size() != BlockBytes || !change.replacement.data())) ||
                (change.action == TypedCacheAction::retire && !change.replacement.empty()))
                return Status::invalid_argument("typed cache target image or bounds are invalid");
            for (usize prior = 0; prior < target; ++prior)
                if (changes[prior].block == change.block)
                    return Status::invalid_argument("typed cache has duplicate affected targets");
            for (const auto bytes : {change.expected, change.replacement})
                if (overlaps_storage(bytes.data(), bytes.size(), this, sizeof(*this)) ||
                    overlaps_storage(bytes.data(), bytes.size(), &plan, sizeof(plan)))
                    return Status::invalid_argument("typed cache image aliases retained storage");
            for (const auto &entry : entries_)
            {
                if (entry.report.state != MetadataBufferState::ready || entry.report.block != change.block) continue;
                for (usize byte = 0; byte < BlockBytes; ++byte)
                    if (entry.canonical[byte] != change.expected[byte])
                        return Status::fault("typed cache affected canonical image is stale");
                if (change.action == TypedCacheAction::replace && entry.report.generation == ~u64{0})
                    return Status::overflow("typed cache replacement generation exhausted");
                if (change.action == TypedCacheAction::retire) ++retirements;
            }
        }
        // Every possible refusal precedes writes to either cache or private plan.
        plan.original_ = &plan; plan.cache_ = this; plan.session_ = &session_;
        plan.phase_ = TypedReconciliation::Phase::prepared; plan.report_ = report_; plan.entries_ = entries_;
        plan.actions_ = {}; plan.final_reports_ = {}; plan.final_images_ = {};
        plan.targets_ = {}; plan.target_actions_ = {};
        plan.sectors_per_block_ = sectors_per_block_; plan.transaction_serial_ = transaction_serial_;
        plan.writable_ = writable_; plan.targets_count_ = changes.size();
        plan.session_binding_ = session_binding_;
        plan.resident_after_ = report_.resident_blocks - retirements;
        for (usize target = 0; target < changes.size(); ++target)
        { plan.targets_[target] = changes[target].block; plan.target_actions_[target] = changes[target].action; }
        for (usize slot = 0; slot < Capacity; ++slot)
        {
            const auto &entry = entries_[slot];
            plan.final_reports_[slot] = entry.report;
            if (entry.report.state != MetadataBufferState::ready) continue;
            for (const auto &change : changes)
            {
                if (entry.report.block != change.block) continue;
                plan.actions_[slot] = change.action;
                if (change.action == TypedCacheAction::replace)
                {
                    ++plan.final_reports_[slot].generation;
                    for (usize byte = 0; byte < BlockBytes; ++byte)
                        plan.final_images_[slot][byte] = change.replacement[byte];
                }
                else
                {
                    const u64 generation = entry.report.generation;
                    plan.final_reports_[slot] = {};
                    plan.final_reports_[slot].generation = generation == ~u64{0} ? generation : generation + 1;
                    plan.final_reports_[slot].state = generation == ~u64{0} ? MetadataBufferState::exhausted :
                                                                                          MetadataBufferState::unused;
                }
            }
        }
        plan.seal_ = typed_plan_seal(plan);
        return {};
    }
    [[nodiscard]] Status validate_typed_reconciliation(const TypedReconciliation &plan,
                                                       typename TypedReconciliation::Phase phase) const
    {
        if (plan.original_ != &plan || plan.cache_ != this || plan.session_ != &session_ || plan.phase_ != phase)
            return Status::bad_descriptor("typed cache plan lost its original binding or phase");
        if (plan.seal_ != typed_plan_seal(plan))
            return Status::fault("typed cache retained plan seal changed");
        if (auto status = typed_cache_idle(); !status) return status;
        if (!same_report(plan.report_, report_) || plan.sectors_per_block_ != sectors_per_block_ ||
            plan.transaction_serial_ != transaction_serial_ || plan.writable_ != writable_ ||
            plan.session_binding_ != session_binding_)
            return Status::fault("typed cache complete owner snapshot changed");
        for (usize index = 0; index < Capacity; ++index)
            if (!same_report(plan.entries_[index].report, entries_[index].report) ||
                plan.entries_[index].canonical != entries_[index].canonical ||
                plan.entries_[index].staged != entries_[index].staged)
                return Status::fault("typed cache complete resident snapshot changed");
        return {};
    }
    [[nodiscard]] Status refuse_typed_reconciliation(TypedReconciliation &plan, Status reason)
    {
        if (plan.original_ == &plan && plan.cache_ == this && plan.session_ == &session_ &&
            (plan.phase_ == TypedReconciliation::Phase::prepared || plan.phase_ == TypedReconciliation::Phase::normalized))
            plan.phase_ = TypedReconciliation::Phase::failed;
        return reason;
    }
    [[nodiscard]] Status normalize_typed_reconciliation(TypedReconciliation &plan)
    {
        smp::ScopedSpinLock guard(lock_);
        if (auto status = validate_typed_reconciliation(plan, TypedReconciliation::Phase::prepared); !status)
            return refuse_typed_reconciliation(plan, status);
        for (usize index = 0; index < Capacity; ++index)
            if (plan.actions_[index] != TypedCacheAction::unchanged)
            {
                entries_[index].staged = entries_[index].canonical;
                plan.entries_[index].staged = entries_[index].staged;
            }
        plan.phase_ = TypedReconciliation::Phase::normalized; plan.seal_ = typed_plan_seal(plan);
        return {};
    }
    [[nodiscard]] Status publish_typed_reconciliation(TypedReconciliation &plan)
    {
        smp::ScopedSpinLock guard(lock_);
        if (auto status = validate_typed_reconciliation(plan, TypedReconciliation::Phase::normalized); !status)
            return refuse_typed_reconciliation(plan, status);
        for (usize index = 0; index < Capacity; ++index)
        {
            if (plan.actions_[index] == TypedCacheAction::unchanged) continue;
            entries_[index].report = plan.final_reports_[index];
            if (plan.actions_[index] == TypedCacheAction::replace)
                entries_[index].canonical = entries_[index].staged = plan.final_images_[index];
            else entries_[index].canonical = entries_[index].staged = {};
        }
        report_.resident_blocks = plan.resident_after_;
        plan.phase_ = TypedReconciliation::Phase::published;
        return {};
    }
    [[nodiscard]] Status admission()
    {
        if (report_.state == MetadataOwnerState::failed) return Status::denied("metadata owner is quarantined");
        if (report_.state != MetadataOwnerState::ready) return Status::not_initialized("metadata owner is not open");
        const auto session = session_.report();
        if (!session || session.value().state != block::DeviceState::owned ||
            session.value().identity != report_.device_identity ||
            session.value().registration_generation != report_.registration_generation ||
            session.value().session_serial != report_.session_serial)
        {
            fail(session && session.value().first_error != StatusCode::ok ? session.value().first_error :
                 StatusCode::fault);
            return Status::denied("metadata owner Session lost authority");
        }
        return {};
    }
    // The owning transaction supplies a checked, bounded physical mapping after
    // all readers drain. Inspect the entire set before changing any slot so a
    // busy or malformed mapping cannot retire only a prefix. No transport I/O
    // or session transition occurs under this lock.
    [[nodiscard]] Result<usize> retire_clean_blocks(std::span<const u64> blocks)
    {
        smp::ScopedSpinLock guard(lock_);
        if (auto status = admission(); !status) return status;
        if (!writable_) return Status::denied("readonly metadata owner cannot retire released blocks");
        if (report_.in_flight || report_.read_pins || transaction_ || report_.transaction_active)
            return Status::busy("metadata owner has retained work during retirement");
        if (blocks.size() > 16)
            return Status::unsupported("released metadata mapping exceeds bounded profile");
        // Keep a private copy: retirement clears slot storage, and even an
        // accidentally aliased caller span must not change the checked set.
        std::array<u64, 16> targets{};
        const usize target_count = blocks.size();
        for (usize index = 0; index < target_count; ++index) targets[index] = blocks[index];
        const auto released = std::span<const u64>{targets}.first(target_count);
        for (usize index = 0; index < target_count; ++index)
        {
            if (targets[index] >= report_.blocks)
                return Status::invalid_argument("released metadata block outside partition");
            for (usize prior = 0; prior < index; ++prior)
                if (targets[index] == targets[prior])
                    return Status::invalid_argument("duplicate released metadata block");
        }
        usize count = 0;
        for (usize index = 0; index < Capacity; ++index)
        {
            const auto &entry = entries_[index];
            if (entry.report.state != MetadataBufferState::ready) continue;
            bool requested = false;
            for (u64 block : released)
                if (entry.report.block == block) { requested = true; break; }
            if (!requested) continue;
            if (entry.report.read_pins || entry.report.transaction_serial || entry.report.staged_dirty)
                return Status::busy("released metadata block is retained or staged");
            if (!entry.report.generation)
                return Status::fault("released metadata slot has no generation");
            for (usize prior = 0; prior < index; ++prior)
                if (entries_[prior].report.state == MetadataBufferState::ready &&
                    entries_[prior].report.block == entry.report.block)
                    return Status::fault("released metadata block has duplicate resident identity");
            ++count;
        }
        if (count > report_.resident_blocks)
            return Status::fault("released metadata resident count underflow");
        for (auto &entry : entries_)
        {
            if (entry.report.state != MetadataBufferState::ready) continue;
            bool requested = false;
            for (u64 block : released)
                if (entry.report.block == block) { requested = true; break; }
            if (!requested) continue;
            const u64 generation = entry.report.generation;
            entry.canonical = {}; entry.staged = {};
            entry.report = {};
            entry.report.generation = generation == ~u64{0} ? generation : generation + 1;
            entry.report.state = generation == ~u64{0} ? MetadataBufferState::exhausted :
                                                        MetadataBufferState::unused;
        }
        report_.resident_blocks -= count;
        return count;
    }
    void fail(StatusCode reason)
    {
        if (report_.first_error == StatusCode::ok) report_.first_error = reason;
        report_.state = MetadataOwnerState::failed;
    }
    void bind_pin(Pin &pin, Entry &entry, usize slot)
    {
        pin.owner_ = this; pin.slot_ = slot; pin.generation_ = entry.report.generation; pin.active_ = true;
        ++entry.report.read_pins; ++report_.read_pins;
    }
    [[nodiscard]] Entry *find(const Pin &pin)
    {
        if (pin.owner_ != this || !pin.active_ || pin.slot_ >= Capacity) return nullptr;
        auto &entry = entries_[pin.slot_];
        return entry.report.generation == pin.generation_ && entry.report.read_pins ? &entry : nullptr;
    }
    [[nodiscard]] Entry *find(const Transaction &transaction, u64 block)
    {
        if (transaction.owner_ != this || !transaction.active_ || transaction_ != &transaction ||
            transaction.serial_ != transaction_serial_) return nullptr;
        for (auto &entry : entries_)
            if (entry.report.transaction_serial == transaction.serial_ && entry.report.block == block) return &entry;
        return nullptr;
    }
    [[nodiscard]] static bool valid_range(usize offset, usize size)
    { return size && offset < BlockBytes && size <= BlockBytes - offset; }
    [[nodiscard]] Status read(const Pin &pin, usize offset, std::span<std::byte> out)
    {
        smp::ScopedSpinLock guard(lock_);
        auto *entry = find(pin);
        if (!entry) return Status::bad_descriptor("released or stale metadata pin");
        if (auto status = admission(); !status) return status;
        if (!valid_range(offset, out.size())) return Status::invalid_argument("metadata byte range");
        for (usize index = 0; index < out.size(); ++index) out[index] = entry->canonical[offset + index];
        return {};
    }
    [[nodiscard]] Status read(const Transaction &transaction, u64 block, usize offset, std::span<std::byte> out)
    {
        smp::ScopedSpinLock guard(lock_);
        auto *entry = find(transaction, block);
        if (!entry) return Status::bad_descriptor("metadata transaction does not own block");
        if (auto status = admission(); !status) return status;
        if (!valid_range(offset, out.size())) return Status::invalid_argument("metadata byte range");
        for (usize index = 0; index < out.size(); ++index) out[index] = entry->staged[offset + index];
        return {};
    }
    [[nodiscard]] Status edit(Transaction &transaction, u64 block, usize offset, std::span<const std::byte> bytes)
    {
        smp::ScopedSpinLock guard(lock_);
        auto *entry = find(transaction, block);
        if (!entry) return Status::bad_descriptor("metadata transaction does not own block");
        if (auto status = admission(); !status) return status;
        if (!valid_range(offset, bytes.size())) return Status::invalid_argument("metadata byte range");
        for (usize index = 0; index < bytes.size(); ++index) entry->staged[offset + index] = bytes[index];
        entry->report.staged_dirty = true;
        return {};
    }
    [[nodiscard]] Status release(Pin &pin)
    {
        smp::ScopedSpinLock guard(lock_);
        auto *entry = find(pin);
        if (!entry) return Status::bad_descriptor("released or stale metadata pin");
        --entry->report.read_pins; --report_.read_pins; pin.active_ = false;
        return {};
    }
    [[nodiscard]] Status discard(Transaction &transaction)
    {
        smp::ScopedSpinLock guard(lock_);
        if (transaction.owner_ != this || !transaction.active_ || transaction_ != &transaction || transaction.serial_ != transaction_serial_)
            return Status::bad_descriptor("retired metadata transaction");
        for (auto &entry : entries_)
            if (entry.report.transaction_serial == transaction.serial_)
            { entry.report.transaction_serial = 0; entry.report.staged_dirty = false; }
        transaction.active_ = false; transaction_ = nullptr; report_.transaction_active = false;
        return {};
    }
    [[nodiscard]] Result<MetadataBufferReport> inspect(const Pin &pin)
    {
        smp::ScopedSpinLock guard(lock_);
        const auto *entry = find(pin);
        return entry ? Result<MetadataBufferReport>{entry->report} : Result<MetadataBufferReport>{Status::bad_descriptor("released metadata pin")};
    }
    typename Registry::Session session_{};
    std::array<Entry, Capacity> entries_{};
    MetadataOwnerReport report_{};
    usize sectors_per_block_{};
    u64 transaction_serial_{};
    Transaction *transaction_{};
    bool writable_{};
    block::SessionBindingObservation session_binding_{};
    mutable smp::SpinLock lock_{};
};

struct Ext4InodeMetadataUpdate
{
    u32 inode{}, uid{}, gid{}, ctime_seconds{}, ctime_nanoseconds{};
    u16 permissions{};
    bool ordered_data{};
    u32 size{}, mtime_seconds{}, mtime_nanoseconds{};
};
enum class Ext4TransactionPhase : u8
{
    idle, validating, preflight, data_initialization, data_initialization_flush, data_initialization_readback, payloads, payload_flush, commit_record,
    commit_flush, recovery_bit, recovery_flush, publication, publication_flush,
    published, failed,
};
struct Ext4TransactionReport
{
    Ext4TransactionPhase phase{Ext4TransactionPhase::idle}, failed_phase{Ext4TransactionPhase::idle};
    Status status{};
    usize write_attempts{}, completed_writes{}, flush_attempts{}, completed_flushes{};
    u32 sequence{}, inode{}, target_block{};
    bool payloads_durable{}, commit_record_durable{}, recovery_bit_durable{}, publication_durable{};
    [[nodiscard]] bool published() const { return phase == Ext4TransactionPhase::published && publication_durable; }
};

// One private transaction on exclusively owned, quiesced media. The mounted
// mode requires a durable recovery bit and journals one inode-table action.
// Success publishes a recoverable journal; a separate owner checkpoints it.
// No VFS mount, policy/permission decision, allocation or file-data write API.
class Ext4MetadataTransaction final
{
    using Block = std::array<u8, 1024>;
    using Completion = Ext4RecoveryCompletion<2, 2>;
    class ProposedDevice final : public driver::BlockDevice
    {
      public:
        explicit ProposedDevice(Ext4MetadataTransaction &owner) : owner_(owner) {}
        driver::BlockGeometry geometry() const override { return owner_.device_->geometry(); }
        Status read_blocks(u64 sector, std::span<std::byte> out) override
        {
            if (const auto status = owner_.device_->read_blocks(sector, out); !status)
                return status;
            if (sector < owner_.bounds_.first_sector || out.size() % 512)
                return Status::invalid_argument("ext4 transaction proposed read bounds");
            // Binding may read sub-block sectors; overlay each sector exactly.
            for (usize offset = 0; offset < out.size(); offset += 512)
            {
                const u64 relative = sector - owner_.bounds_.first_sector + offset / 512;
                const auto *block = owner_.proposed(relative / 2);
                if (block)
                    for (usize byte = 0; byte < 512; ++byte)
                        out[offset + byte] = std::byte((*block)[(relative % 2) * 512 + byte]);
            }
            return {};
        }
        Status write_blocks(u64, std::span<const std::byte>) override
        { return Status::denied("ext4 transaction proposed view is readonly"); }
        Status flush() override { return Status::denied("ext4 transaction proposed view cannot flush"); }
      private:
        Ext4MetadataTransaction &owner_;
    };

  public:
    explicit Ext4MetadataTransaction(bool mounted_dirty = false)
        : clean_(mounted_dirty, mounted_dirty), proposed_(mounted_dirty, mounted_dirty),
          mounted_dirty_(mounted_dirty) {}
    Ext4MetadataTransaction(const Ext4MetadataTransaction &) = delete;
    Ext4MetadataTransaction &operator=(const Ext4MetadataTransaction &) = delete;
    Ext4MetadataTransaction(Ext4MetadataTransaction &&) = delete;
    Ext4MetadataTransaction &operator=(Ext4MetadataTransaction &&) = delete;
    [[nodiscard]] const Ext4TransactionReport &report() const { return report_; }

    [[nodiscard]] Status commit(driver::BlockDevice &device, Ext4RecoveryBounds bounds,
                                Ext4InodeMetadataUpdate update)
    {
        if (const auto status = prepare(device, bounds, update); !status) return status;
        return publish_prepared();
    }

  private:
    template <usize, usize> friend class Ext4OwnedTransaction;
    template <bool AllowDirectory = true, bool CheckedOwnership = false, bool CheckedMode = false>
    [[nodiscard]] Status prepare(driver::BlockDevice &device, Ext4RecoveryBounds bounds,
                                 Ext4InodeMetadataUpdate update)
    {
        if (report_.phase != Ext4TransactionPhase::idle)
            return Status::already_exists("ext4 transaction is one-shot");
        report_.phase = Ext4TransactionPhase::validating;
        device_ = &device; bounds_ = bounds; geometry_ = device.geometry();
        if (update.permissions & ~u16{07777} || update.ctime_seconds > 0x7fffffffU ||
            update.ctime_nanoseconds >= 1000000000 ||
            (update.ordered_data && (update.mtime_seconds > 0x7fffffffU || update.mtime_nanoseconds >= 1000000000)))
            return fail(Status::invalid_argument("ext4 transaction mode or timestamp"));
        if (const auto status = clean_.prepare(device, bounds); !status)
            return fail(status);
        if (clean_.report().already_clean == mounted_dirty_ || !clean_.report().empty_log)
            return fail(Status::unsupported("ext4 transaction requires the selected empty journal state"));
        if (mounted_dirty_ && !update.ordered_data && !CheckedOwnership && !CheckedMode)
            return fail(Status::unsupported("mounted-dirty transaction requires ordered regular data"));
        auto &preview = clean_.preview_;
        const auto &super = preview.codec_->superblock();
        if (super.first_block != 1 || super.max_blocks < 6 || super.sequence > 0xfffffffcU)
            return fail(Status::unsupported("ext4 transaction ring or sequence capacity"));
        const auto inode = preview.inode(update.inode);
        if (!inode)
            return fail(inode.status());
        auto &view = preview.view_;
        const u16 type = inode.value().mode & 0170000;
        if (update.inode < view.first_ordinary_inode() ||
            (type != 0100000 && (!AllowDirectory || type != 0040000)))
            return fail(Status::unsupported("ext4 transaction requires ordinary regular inode or directory"));
        if (!CheckedOwnership && type == 0040000 && (update.uid != inode.value().uid || update.gid != inode.value().gid))
            return fail(Status::unsupported("ext4 directory transaction preserves ownership"));
        if constexpr (CheckedOwnership)
        {
            const bool changed = update.uid != inode.value().uid || update.gid != inode.value().gid;
            const u16 permissions = (inode.value().mode & 07777u) & (changed ? ~06000u : ~0u);
            if (update.ordered_data || update.permissions != permissions)
                return fail(Status::unsupported("checked ownership preserves data and nonprivileged permissions"));
            proposed_.policy_ = Ext4ReplayPolicy::checked_ownership;
            proposed_.preview_.policy_ = Ext4ReplayPolicy::checked_ownership;
        }
        if constexpr (CheckedMode)
        {
            static_assert(!CheckedOwnership);
            if (update.ordered_data || update.uid != inode.value().uid || update.gid != inode.value().gid)
                return fail(Status::unsupported("checked mode preserves ownership and data"));
            proposed_.policy_ = Ext4ReplayPolicy::checked_mode;
            proposed_.preview_.policy_ = Ext4ReplayPolicy::checked_mode;
        }
        if (update.ordered_data && (type != 0100000 || !update.size || !inode.value().size ||
            (u64{update.size} + 1023) / 1024 != (u64{inode.value().size} + 1023) / 1024 ||
            update.uid != inode.value().uid || update.gid != inode.value().gid ||
            update.permissions != (inode.value().mode & 07777)))
            return fail(Status::unsupported("ext4 ordered-data inode identity or block count"));
        const auto local = (update.inode - 1) % view.inodes_per_group_;
        report_.target_block = view.groups_[(update.inode - 1) / view.inodes_per_group_].table + local / 4;
        report_.inode = update.inode; report_.sequence = super.sequence + 1;
        journal_first_ = u32(preview.layout().journal_first_block);
        if (const auto status = preview.raw_block(report_.target_block, inode_); !status)
            return fail(status);
        filesystem_ = clean_.filesystem_; journal_ = clean_.journal_;
        if (!mounted_dirty_)
        {
            Completion::le32(filesystem_, 96, ext4_checked::le32(filesystem_, 96) | 4);
            Completion::le32(filesystem_, 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{filesystem_}.first(1020)));
        }
        auto bytes = std::span<u8>{inode_}.subspan((local % 4) * 256, 256);
        const u64 version = u64{ext4_checked::le32(bytes, 36)} | (u64{ext4_checked::le32(bytes, 152)} << 32);
        if (version == ~u64{0})
            return fail(Status::unsupported("ext4 transaction inode version exhausted"));
        le16(bytes, 0, u16(type | update.permissions));
        le16(bytes, 2, u16(update.uid)); le16(bytes, 120, u16(update.uid >> 16));
        le16(bytes, 24, u16(update.gid)); le16(bytes, 122, u16(update.gid >> 16));
        Completion::le32(bytes, 12, update.ctime_seconds);
        Completion::le32(bytes, 132, update.ctime_nanoseconds << 2);
        if (update.ordered_data)
        {
            Completion::le32(bytes, 4, update.size);
            Completion::le32(bytes, 16, update.mtime_seconds);
            Completion::le32(bytes, 136, update.mtime_nanoseconds << 2);
        }
        Completion::le32(bytes, 36, u32(version + 1)); Completion::le32(bytes, 152, u32((version + 1) >> 32));
        le16(bytes, 124, 0); le16(bytes, 130, 0);
        const auto crc = jbd2::detail::crc32c(ext4_checked::inode_seed(view.checksum_seed(), update.inode, bytes), bytes);
        le16(bytes, 124, u16(crc)); le16(bytes, 130, u16(crc >> 16));

        header(descriptor_, 1); header(commit_, 2);
        const auto seed = view.checksum_seed();
        if (mounted_dirty_)
            tag(12, report_.target_block, jbd2::flag_last_tag, inode_, seed);
        else
        {
            tag(12, 1, 0, filesystem_, seed); // Explicit zero internal-journal tag UUID follows.
            tag(44, report_.target_block, jbd2::flag_same_uuid | jbd2::flag_last_tag, inode_, seed);
        }
        Completion::be32(descriptor_, 1020, jbd2::detail::crc32c(seed, descriptor_));
        Completion::be32(commit_, 52, update.ctime_seconds);
        Completion::be32(commit_, 56, update.ctime_nanoseconds);
        Completion::be32(commit_, 16, jbd2::detail::crc32c(seed, commit_));
        Completion::be32(journal_, 24, report_.sequence); Completion::be32(journal_, 28, 1);
        Completion::be32(journal_, 252, 0);
        Completion::be32(journal_, 252, jbd2::detail::crc32c(~u32{0}, journal_));
        // Admit every generated record/action and final allocated-inode view
        // through the same completion gate, without synthetic writes/flushes.
        proposed_.preview_.allow_ordered_data_ = update.ordered_data;
        if (const auto status = proposed_.prepare(proposed_device_, bounds); !status)
            return fail(status);
        if (proposed_.report().validated_actions != (mounted_dirty_ ? 1U : 2U) || proposed_.report().empty_log)
            return fail(Status::fault("ext4 transaction proposed journal invariant"));

        report_.phase = Ext4TransactionPhase::preflight;
        return {};
    }
    [[nodiscard]] Status logical_images(std::array<Block, 3> &images)
    {
        if (const auto status = proposed_.preview_.final_block(1, images[0]); !status) return status;
        if (const auto status = proposed_.preview_.final_block(report_.target_block, images[1]); !status) return status;
        images[2] = journal_;
        return {};
    }
    [[nodiscard]] Status publish_prepared()
    {
        if (report_.phase != Ext4TransactionPhase::preflight)
            return Status::bad_descriptor("ext4 transaction is not prepared");
        if (const auto status = flush(); !status) return fail(status);
        report_.phase = Ext4TransactionPhase::payloads;
        if (const auto status = write(journal_first_ + 1, descriptor_); !status) return fail(status);
        if (!mounted_dirty_)
            if (const auto status = write(journal_first_ + 2, filesystem_); !status) return fail(status);
        if (const auto status = write(journal_first_ + (mounted_dirty_ ? 2 : 3), inode_); !status) return fail(status);
        if (const auto status = write(journal_first_ + (mounted_dirty_ ? 4 : 5), end_); !status) return fail(status);
        report_.phase = Ext4TransactionPhase::payload_flush;
        if (const auto status = flush(); !status) return fail(status);
        report_.payloads_durable = true;
        report_.phase = Ext4TransactionPhase::commit_record;
        if (const auto status = write(journal_first_ + (mounted_dirty_ ? 3 : 4), commit_); !status) return fail(status);
        report_.phase = Ext4TransactionPhase::commit_flush;
        if (const auto status = flush(); !status) return fail(status);
        report_.commit_record_durable = true;
        if (!mounted_dirty_)
        {
            report_.phase = Ext4TransactionPhase::recovery_bit;
            if (const auto status = write(1, filesystem_); !status) return fail(status);
            report_.phase = Ext4TransactionPhase::recovery_flush;
            if (const auto status = flush(); !status) return fail(status);
        }
        report_.recovery_bit_durable = true;
        report_.phase = Ext4TransactionPhase::publication;
        if (const auto status = write(journal_first_, journal_); !status) return fail(status);
        report_.phase = Ext4TransactionPhase::publication_flush;
        if (const auto status = flush(); !status) return fail(status);
        report_.publication_durable = true; report_.phase = Ext4TransactionPhase::published;
        return {};
    }

  private:
    [[nodiscard]] const Block *proposed(u64 block) const
    {
        if (block == 1 || (!mounted_dirty_ && block == u64{journal_first_} + 2)) return &filesystem_;
        if (block == journal_first_) return &journal_;
        if (block == u64{journal_first_} + 1) return &descriptor_;
        if (block == u64{journal_first_} + (mounted_dirty_ ? 2 : 3)) return &inode_;
        if (block == u64{journal_first_} + (mounted_dirty_ ? 3 : 4)) return &commit_;
        if (block == u64{journal_first_} + (mounted_dirty_ ? 4 : 5)) return &end_;
        return nullptr;
    }
    void header(Block &block, u32 type)
    {
        Completion::be32(block, 0, jbd2::magic); Completion::be32(block, 4, type);
        Completion::be32(block, 8, report_.sequence);
    }
    void tag(usize at, u32 target, u32 flags, Block &payload, u32 seed)
    {
        if (jbd2::detail::be32(payload, 0) == jbd2::magic)
        {
            Completion::be32(payload, 0, 0); flags |= jbd2::flag_escape;
        }
        Completion::be32(descriptor_, at, target); Completion::be32(descriptor_, at + 4, flags);
        const auto seq = std::span<const u8>{descriptor_}.subspan(8, 4);
        Completion::be32(descriptor_, at + 12, jbd2::detail::crc32c(jbd2::detail::crc32c(seed, seq), payload));
    }
    static void le16(std::span<u8> bytes, usize at, u16 value)
    { bytes[at] = u8(value); bytes[at + 1] = u8(value >> 8); }
    [[nodiscard]] Status stable() const
    {
        const auto now = device_->geometry();
        return now.block_size == geometry_.block_size && now.block_count == geometry_.block_count &&
               now.writable == geometry_.writable ? Status{} : Status::io_error("ext4 transaction device changed");
    }
    [[nodiscard]] Status write(u32 block, const Block &bytes)
    {
        if (const auto status = stable(); !status) return status;
        ++report_.write_attempts;
        if (const auto status = device_->write_blocks(bounds_.first_sector + u64{block} * 2, std::as_bytes(std::span{bytes})); !status)
            return status;
        ++report_.completed_writes; return stable();
    }
    [[nodiscard]] Status flush()
    {
        if (const auto status = stable(); !status) return status;
        ++report_.flush_attempts;
        if (const auto status = device_->flush(); !status) return status;
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
    Block journal_{}, filesystem_{}, inode_{}, descriptor_{}, commit_{}, end_{};
    bool mounted_dirty_{};
};

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

// Only the canonical owner can publish this checked next allocation. The owner
// seals and writes/flushes the full initialized data image before publication.
class Ext4NextDataTransaction final
{
    template <usize, usize> friend class Ext4OwnedTransaction;
    using Block = std::array<u8, 1024>;
    static constexpr usize home_capacity = 4;
    using Completion = Ext4RecoveryCompletion<home_capacity, 2>;
    class ProposedDevice final : public driver::BlockDevice
    {
      public:
        explicit ProposedDevice(Ext4NextDataTransaction &owner) : owner_(owner) {}
        driver::BlockGeometry geometry() const override { return owner_.device_->geometry(); }
        Status read_blocks(u64 sector, std::span<std::byte> out) override
        {
            if (auto status = owner_.device_->read_blocks(sector, out); !status) return status;
            if (sector < owner_.bounds_.first_sector || out.size() % 512)
                return Status::invalid_argument("next data proposed read bounds");
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
        { return Status::denied("next data proposed view is read-only"); }
        Status flush() override { return Status::denied("next data proposed view cannot flush"); }
      private:
        Ext4NextDataTransaction &owner_;
    };

  public:
    Ext4NextDataTransaction() : clean_(true, true),
        proposed_(false, true, Ext4ReplayPolicy::next_data_allocation) {}
    Ext4NextDataTransaction(const Ext4NextDataTransaction &) = delete;
    Ext4NextDataTransaction &operator=(const Ext4NextDataTransaction &) = delete;
    [[nodiscard]] const Ext4TransactionReport &report() const { return report_; }

  private:
    [[nodiscard]] Status prepare(driver::BlockDevice &device, Ext4RecoveryBounds bounds,
                                  Ext4NextDataUpdate update, u32 offset, const Block &data_image)
    {
        if (report_.phase != Ext4TransactionPhase::idle)
            return Status::already_exists("next data transaction is one-shot");
        report_.phase = Ext4TransactionPhase::validating;
        device_ = &device; bounds_ = bounds; geometry_ = device.geometry();
        if (update.old_size < 1024 || update.old_size > 15360 || update.old_size % 1024 ||
            update.size <= update.old_size || u64{update.size} > u64{update.old_size} + 1024 ||
            offset < update.old_size || offset >= update.size ||
            update.seconds > 0x7fffffffU || update.nanoseconds >= 1000000000U)
            return fail(Status::invalid_argument("next data range or timestamp"));
        // The caller supplies only [offset, size); every other byte must be
        // initialized to zero, including the sparse gap and bytes beyond EOF.
        for (usize byte = 0; byte < data_image.size(); ++byte)
            if ((byte < offset - update.old_size || byte >= update.size - update.old_size) && data_image[byte])
                return fail(Status::invalid_argument("next data image is not fully zero-filled"));
        if (auto status = clean_.prepare(device, bounds); !status) return fail(status);
        if (clean_.report().already_clean || !clean_.report().empty_log)
            return fail(Status::unsupported("next data requires dirty empty journal"));
        auto &preview = clean_.preview_;
        auto &view = preview.view_;
        const auto &super = preview.codec_->superblock();
        if (super.first_block != 1 || super.sequence > 0xfffffffcU || super.max_blocks < 8)
            return fail(Status::unsupported("next data journal ring or sequence"));
        const auto inode = preview.inode(update.inode);
        if (!inode) return fail(inode.status());
        if (update.inode < view.first_ordinary_inode() || inode.value().mode >> 12 != 8 ||
            inode.value().size != update.old_size || inode.value().links != 1)
            return fail(Status::unsupported("next data target is not an allocated aligned regular inode"));
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
            const auto candidate = Ext4NextDataPlanner::select_block(*preview.binding_.info(),
                source_super_, source_descriptors_, source_bitmap_, group);
            if (!candidate) return fail(candidate.status());
            selected = candidate.value(); selected_group = group; break;
        }
        if (!selected)
            return fail(unsupported_space ? Status::unsupported("next data free blocks require uninitialized group support") :
                Status::no_space("next data supported block space is exhausted"));
        const u32 inode_group = (update.inode - 1) / view.inodes_per_group_;
        if (inode_group >= view.group_count_) return fail(Status::unsupported("next data inode group"));
        const u32 table = view.groups_[inode_group].table +
            ((update.inode - 1) % view.inodes_per_group_) / 4;
        if (auto status = preview.raw_block(table, source_inode_); !status) return fail(status);
        const Ext4NextDataSource source{selected_group, selected, table,
            source_super_, source_descriptors_, source_bitmap_, source_inode_};
        if (auto status = Ext4NextDataPlanner::plan(*preview.binding_.info(), update, source, plan_); !status)
            return fail(status);
        if (plan_.count != home_capacity) return fail(Status::fault("next data participant count"));
        // A clear bit is not ownership. The bounded scan covers every allocated
        // record, including reserved records, and every old mapped block.
        const auto owned = view.prove_next_data_blocks(update.inode, selected, false);
        if (!owned) return fail(owned.status());
        if (owned.value().new_physical != selected || owned.value().old_blocks != plan_.old_blocks ||
            owned.value().old_physical != plan_.old_physical)
            return fail(Status::fault("next data checked mapping differs from plan"));
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
            return fail(Status::fault("next data proposed replay shape"));
        report_.phase = Ext4TransactionPhase::preflight;
        return {};
    }
    template <usize Capacity> requires (Capacity >= 7)
    [[nodiscard]] Status logical_images(std::array<u64, Capacity> &blocks,
        std::array<Block, Capacity> &images, usize &count) const
    {
        if (report_.phase != Ext4TransactionPhase::preflight)
            return Status::bad_descriptor("next data has no prepared image");
        blocks[0] = journal_first_; images[0] = journal_;
        for (usize index = 0; index < home_count_; ++index)
        { blocks[index + 1] = plan_.targets[index]; images[index + 1] = plan_.after[index]; }
        blocks[home_count_ + 1] = plan_.physical; images[home_count_ + 1] = data_;
        count = home_count_ + 2; return {};
    }
    [[nodiscard]] Status publish_prepared()
    {
        if (report_.phase != Ext4TransactionPhase::preflight)
            return Status::bad_descriptor("next data is not prepared");
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
            now.writable == geometry_.writable ? Status{} : Status::io_error("next data device changed");
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
    Ext4NextDataPlan plan_{};
    std::array<Block, home_capacity> payload_{};
    Block source_super_{}, source_descriptors_{}, source_bitmap_{}, source_inode_{}, data_{};
    Block journal_{}, descriptor_{}, commit_{}, end_{};
};

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

struct Ext4ZeroTruncateReadCapacity
{
    block::SessionCounter reads{};
    u32 groups{};
};
enum class Ext4ZeroTruncateSessionPreviewPhase : u8 { unused, preparing, ready, failed };
struct Ext4ZeroTruncateSessionPreviewReport
{
    Ext4ZeroTruncateSessionPreviewPhase phase{Ext4ZeroTruncateSessionPreviewPhase::unused};
    Status status{};
    Ext4ZeroTruncateReadCapacity reserved{};
    Ext4ZeroTruncatePreviewReport preview{};
    block::SessionReport initial{}, observed{};
    block::SessionCounter attempts{}, completions{};
    bool dispatch_pending{};
};

// Own this off-stack object for the exact original Session's excluded-writer
// lifetime. It performs only guarded READs; no report or reservation grants
// publication, checkpoint, FLUSH or WRITE authority. This is not a mounted owner.
template <typename Session>
class Ext4ZeroTruncateSessionPreview final
{
    template <typename> friend class Ext4ZeroTruncateTransaction;
    using Phase = Ext4ZeroTruncateSessionPreviewPhase;
    struct Reader
    {
        Ext4ZeroTruncateSessionPreview &owner;
        [[nodiscard]] Status read_block(u32 number, std::span<u8> out)
        { return owner.read(number, out); }
    };
    struct Dispatch
    {
        Ext4ZeroTruncateSessionPreview &owner;
        [[nodiscard]] Status in_flight_exact(Session &session)
        { return owner.in_flight_exact(session); }
    };
    struct Control
    {
        Phase phase{Phase::unused};
        block::SessionCounter attempts{}, completions{};
        u32 groups{};
        u64 sector{};
        bool pending{};
    };
  public:
    Ext4ZeroTruncateSessionPreview() = default;
    ~Ext4ZeroTruncateSessionPreview()
    {
        if (control_.pending || sealed_.pending)
            static_cast<void>(anchor_.abort(Status::interrupted("zero truncate preview unfinished READ")));
    }
    Ext4ZeroTruncateSessionPreview(const Ext4ZeroTruncateSessionPreview &) = delete;
    Ext4ZeroTruncateSessionPreview &operator=(const Ext4ZeroTruncateSessionPreview &) = delete;
    Ext4ZeroTruncateSessionPreview(Ext4ZeroTruncateSessionPreview &&) = delete;
    Ext4ZeroTruncateSessionPreview &operator=(Ext4ZeroTruncateSessionPreview &&) = delete;

    template <block::SessionCounterPolicy Counter = block::SessionCounter>
    [[nodiscard]] static Result<Ext4ZeroTruncateReadCapacity> capacity(
        const block::SessionReport &initial, u32 groups)
    {
        if (!groups || groups > Ext4RecoveryInfo::group_capacity)
            return Status::invalid_argument("zero truncate preview group capacity");
        if (!stable(initial))
            return Status::fault("zero truncate preview requires a stable original Session");
        // A checked inode decode reads at most D=2+4G blocks. Each old/final
        // view opens with 2+2D+1024 reads and proves ownership with
        // 2D+G+128(D+1)+2+2G reads. Initial/final capture adds 2*13.
        // Overlay hits cost no Session READs; counting them is conservative.
        const block::SessionCounter reads = 2866 + 1062 * block::SessionCounter(groups);
        if (!block::session_counter_fits<Counter>(initial.reads, reads) ||
            !block::session_counter_fits<Counter>(initial.completed_reads, reads))
            return Status::overflow("zero truncate preview whole READ capacity exhausted");
        return Ext4ZeroTruncateReadCapacity{reads, groups};
    }

    [[nodiscard]] Status prepare(Session &session, const Ext4RecoveryInfo &binding)
    {
        if (attempted_) return Status::already_exists("zero truncate Session preview is one-shot");
        attempted_ = true;
        if (overlap(this, sizeof(*this), &session, sizeof(session)) ||
            overlap(this, sizeof(*this), &binding, sizeof(binding)))
            return refuse(Status::invalid_argument("zero truncate Session preview input overlap"));
        if (auto status = anchor_.bind(session); !status) return refuse(status);
        original_ = original_seal_ = &session;
        if constexpr (requires { session.observe_binding(); })
        {
            const auto binding_observation = session.observe_binding();
            if (!binding_observation) return refuse(binding_observation.status());
            original_binding_ = original_binding_seal_ = binding_observation.value();
        }
        const auto actual = session.report();
        if (!actual) return refuse(actual.status());
        observed_ = actual.value();
        if (!binding.metadata_count || binding.metadata_count % 4 ||
            binding.metadata_count > binding.metadata.size() || !binding.structural_metadata().valid())
            return refuse(Status::invalid_argument("zero truncate Session preview metadata domain"));
        const auto reserved = capacity(actual.value(), u32(binding.metadata_count / 4));
        if (!reserved) return refuse(reserved.status());
        const auto geometry = session.geometry();
        const auto bounds = session.bounds();
        const auto transfer = session.transfer_limit();
        if (!transfer) return refuse(transfer.status());
        if (geometry.block_size != 512 || !geometry.block_count || bounds.count != geometry.block_count ||
            bounds.first > ~u64{0} - bounds.count || transfer.value() < 1024 ||
            binding.layout.block_size != 1024 || !binding.layout.filesystem_blocks ||
            binding.layout.first_sector >= geometry.block_count ||
            binding.layout.filesystem_blocks > (geometry.block_count - binding.layout.first_sector) / 2)
            return refuse(Status::unsupported("zero truncate Session preview volume geometry"));
        initial_ = initial_seal_ = progress_ = progress_seal_ = actual.value();
        geometry_ = geometry_seal_ = geometry; bounds_ = bounds_seal_ = bounds;
        binding_ = binding_seal_ = binding;
        reserved_ = reserved.value();
        control_.groups = reserved_.groups; control_.phase = Phase::preparing; sync();
        if (auto status = preview_.prepare(reader_, binding_, workspace_); !status) return reject(status);
        if (auto status = idle_exact(); !status) return status;
        if (preview_.report().read_attempts != control_.attempts ||
            preview_.report().completed_reads != control_.completions)
            return reject(Status::fault("zero truncate preview and Session READ totals differ"));
        control_.phase = Phase::ready; sync(); return {};
    }

    [[nodiscard]] Ext4ZeroTruncateSessionPreviewReport report() const
    {
        return {control_.phase, status_, reserved_, preview_.report(), initial_, observed_,
                control_.attempts, control_.completions, control_.pending};
    }
  private:
    static bool overlap(const void *a, usize a_size, const void *b, usize b_size)
    {
        const auto one = reinterpret_cast<usize>(a), two = reinterpret_cast<usize>(b);
        return one <= two ? two - one < a_size : one - two < b_size;
    }
    static bool stable(const block::SessionReport &value)
    {
        return value.state == block::DeviceState::owned && value.first_error == StatusCode::ok &&
            value.identity && value.registration_generation && value.session_serial &&
            !value.in_flight && !value.pending_writes && value.reads == value.completed_reads &&
            value.writes == value.completed_writes && value.flushes == value.completed_flushes;
    }
    static bool same_geometry(driver::BlockGeometry a, driver::BlockGeometry b)
    { return a.block_size == b.block_size && a.block_count == b.block_count && a.writable == b.writable; }
    static bool same_bounds(block::SessionBounds a, block::SessionBounds b)
    { return a.first == b.first && a.count == b.count; }
    static bool same_control(const Control &a, const Control &b)
    {
        return a.phase == b.phase && a.attempts == b.attempts && a.completions == b.completions &&
            a.groups == b.groups && a.sector == b.sector && a.pending == b.pending;
    }
    void sync() { sealed_ = control_; }
    [[nodiscard]] Status bind_owner_guard(void *context, Status (*check)(void *))
    {
        if (attempted_ || owner_guard_retired_ || owner_context_ || owner_check_ || !context || !check)
            return Status::already_exists("recovery preview owner attachment is one-shot");
        owner_context_ = owner_context_seal_ = context;
        owner_check_ = owner_check_seal_ = check;
        return {};
    }
    void retire_owner_guard()
    {
        owner_context_ = owner_context_seal_ = nullptr;
        owner_check_ = owner_check_seal_ = nullptr;
        owner_guard_retired_ = true;
    }
    [[nodiscard]] bool owner_retirement_idle() const
    {
        return status_ && owner_guard_retired_ && !owner_context_ && !owner_context_seal_ &&
            !owner_check_ && !owner_check_seal_ && !control_.pending && !sealed_.pending &&
            same_control(control_, sealed_);
    }
    [[nodiscard]] bool continuing_owner() const
    {
        if (owner_context_ != owner_context_seal_ || owner_check_ != owner_check_seal_ ||
            bool(owner_context_) != bool(owner_check_)) return false;
        return !owner_check_ || bool(owner_check_(owner_context_));
    }
    [[nodiscard]] bool continuing_binding() const
    {
        if constexpr (requires { original_->observe_binding(); })
        {
            const auto actual = anchor_.observe_binding();
            return actual && original_binding_ == original_binding_seal_ && actual.value() == original_binding_;
        }
        else return true;
    }
    [[nodiscard]] bool terminal_binding() const
    {
        if constexpr (requires { original_->observe_terminal_binding(); })
        {
            const auto actual = anchor_.observe_terminal_binding();
            return actual && original_binding_ == original_binding_seal_ && actual.value() == original_binding_;
        }
        else if constexpr (requires { original_->observe_binding(); }) return false;
        else return true;
    }
    [[nodiscard]] bool rejection_binding() const
    { return continuing_binding() || terminal_binding(); }
    [[nodiscard]] bool retained_exact(bool validate_live_boundary = true) const
    {
        if (!attempted_ || !original_ || original_ != original_seal_ || !anchor_.exact(original_) || control_.phase != Phase::preparing ||
            original_binding_ != original_binding_seal_ ||
            owner_context_ != owner_context_seal_ || owner_check_ != owner_check_seal_ ||
            bool(owner_context_) != bool(owner_check_) ||
            (validate_live_boundary && (!continuing_binding() || !continuing_owner())) ||
            !same_control(control_, sealed_) || !stable(initial_) ||
            !reclaim_history::same_session(initial_, initial_seal_) ||
            !reclaim_history::same_session(progress_, progress_seal_) ||
            !reclaim_history::same_info(binding_, binding_seal_) ||
            !same_geometry(geometry_, geometry_seal_) || !same_bounds(bounds_, bounds_seal_) ||
            control_.attempts != control_.completions || !stable(progress_)) return false;
        const auto bound = capacity(initial_, control_.groups);
        if (!bound || bound.value().reads != reserved_.reads || bound.value().groups != reserved_.groups ||
            binding_.metadata_count != control_.groups * 4 || control_.attempts > reserved_.reads)
            return false;
        auto expected = initial_;
        expected.reads += control_.attempts; expected.completed_reads += control_.completions;
        return reclaim_history::same_session(expected, progress_);
    }
    [[nodiscard]] Status idle_exact()
    {
        if (!retained_exact() || control_.pending)
            return reject(Status::fault("zero truncate preview original idle seals changed"));
        const auto actual = original_->report();
        if (!actual) return reject(actual.status());
        observed_ = actual.value();
        if (!same_geometry(original_->geometry(), geometry_) || !same_bounds(original_->bounds(), bounds_) ||
            !reclaim_history::same_session(actual.value(), progress_))
            return reject(Status::fault("zero truncate preview original idle Session changed"));
        return {};
    }
    [[nodiscard]] Status in_flight_exact(Session &session)
    {
        if (!retained_exact() || !anchor_.exact(&session) || original_ != &session || !control_.pending ||
            !reclaim_history::same_session(flight_, flight_seal_) ||
            !reclaim_history::same_session(flight_, progress_))
            return Status::fault("zero truncate preview original dispatch seals changed");
        const auto actual = session.report();
        if (!actual) return actual.status();
        observed_ = actual.value();
        auto expected = flight_; ++expected.reads; expected.in_flight = true;
        if (!same_geometry(session.geometry(), geometry_) || !same_bounds(session.bounds(), bounds_) ||
            !reclaim_history::same_session(actual.value(), expected))
            return Status::fault("zero truncate preview original in-flight Session changed");
        return {};
    }
    [[nodiscard]] Status read(u32 number, std::span<u8> out)
    {
        if (auto status = idle_exact(); !status) return status;
        if (number >= binding_.layout.filesystem_blocks || out.size() != 1024 ||
            control_.attempts >= reserved_.reads)
            return reject(Status::overflow("zero truncate preview selected READ bounds or credit"));
        flight_ = flight_seal_ = progress_;
        control_.sector = binding_.layout.first_sector + u64(number) * 2;
        control_.pending = true; sync();
        const auto transport = original_->read_blocks_guarded(control_.sector,
            std::as_writable_bytes(out), dispatch_);
        return finish_read(transport);
    }
    [[nodiscard]] Status finish_read(Status transport)
    {
        // The Session drains and quarantines a refused guarded dispatch before
        // exact failed-prefix accounting. Its live owner is already revoked.
        if (!transport && !terminal_binding())
            return refuse(Status::fault("zero truncate preview original terminal binding changed"));
        if (!retained_exact(bool(transport)) || !control_.pending ||
            !reclaim_history::same_session(flight_, flight_seal_))
            return reject(Status::fault("zero truncate preview post-READ seals changed"));
        const auto actual = original_->report();
        if (!actual) return reject(actual.status());
        observed_ = actual.value();
        auto expected = flight_; ++expected.reads;
        if (transport) ++expected.completed_reads;
        else
        {
            expected.state = block::DeviceState::quarantined; expected.first_error = actual.value().first_error;
            if (expected.first_error == StatusCode::ok)
                return reject(Status::fault("zero truncate preview failed READ lacks quarantine"));
        }
        if (!same_geometry(original_->geometry(), geometry_) || !same_bounds(original_->bounds(), bounds_) ||
            !reclaim_history::same_session(actual.value(), expected))
            return reject(Status::fault("zero truncate preview READ attempt/completion prefix changed"));
        ++control_.attempts; if (transport) ++control_.completions;
        progress_ = progress_seal_ = actual.value();
        control_.pending = false; control_.sector = 0; sync();
        return transport ? Status{} : reject(transport);
    }
    [[nodiscard]] Status refuse(Status status)
    { control_.phase = Phase::failed; status_ = status; sync(); return status; }
    [[nodiscard]] Status reject(Status status)
    {
        if (!rejection_binding())
            return refuse(Status::fault("zero truncate preview original rejection binding changed"));
        // The agreed original is captured once for rejection and final
        // observation; a single anchor drift must not leave an in-flight
        // report after the original request has already been drained.
        if (const auto actual = anchor_.abort_observe(status); actual) observed_ = actual.value();
        return refuse(status);
    }
    reclaim_history::SessionAnchor<Session> anchor_{};
    Session *original_{}, *original_seal_{};
    block::SessionReport initial_{}, initial_seal_{}, progress_{}, progress_seal_{}, flight_{}, flight_seal_{}, observed_{};
    driver::BlockGeometry geometry_{}, geometry_seal_{};
    block::SessionBounds bounds_{}, bounds_seal_{};
    Ext4RecoveryInfo binding_{}, binding_seal_{};
    Ext4ZeroTruncateReadCapacity reserved_{};
    Control control_{}, sealed_{};
    Status status_{};
    bool attempted_{};
    block::SessionBindingObservation original_binding_{}, original_binding_seal_{};
    void *owner_context_{}, *owner_context_seal_{};
    Status (*owner_check_)(void *){}, (*owner_check_seal_)(void *){};
    bool owner_guard_retired_{};
    Reader reader_{*this};
    Dispatch dispatch_{*this};
    Ext4ZeroTruncatePreviewWorkspace<Reader> workspace_{};
    Ext4ZeroTruncatePreview<Reader> preview_{};
};

struct Ext4AllocatedEmptyDataReadCapacity
{
    block::SessionCounter reads{};
    u32 groups{};
};
enum class Ext4AllocatedEmptyDataSessionPreviewPhase : u8 { unused, preparing, ready, failed };
struct Ext4AllocatedEmptyDataSessionPreviewReport
{
    Ext4AllocatedEmptyDataSessionPreviewPhase phase{Ext4AllocatedEmptyDataSessionPreviewPhase::unused};
    Status status{};
    Ext4AllocatedEmptyDataReadCapacity reserved{};
    Ext4AllocatedEmptyDataPreviewReport preview{};
    block::SessionReport initial{}, observed{};
    block::SessionCounter attempts{}, completions{};
    bool dispatch_pending{};
};

// Own this off-stack object for the exact original Session's excluded-writer
// lifetime. It performs only guarded READs; no report or reservation grants
// publication, checkpoint, FLUSH or WRITE authority. This is not a mounted owner.
template <typename Session>
class Ext4AllocatedEmptyDataSessionPreview final
{
    template <typename> friend class Ext4AllocatedEmptyDataTransaction;
    using Phase = Ext4AllocatedEmptyDataSessionPreviewPhase;
    struct Reader
    {
        Ext4AllocatedEmptyDataSessionPreview &owner;
        [[nodiscard]] Status read_block(u32 number, std::span<u8> out)
        { return owner.read(number, out); }
    };
    struct Dispatch
    {
        Ext4AllocatedEmptyDataSessionPreview &owner;
        [[nodiscard]] Status in_flight_exact(Session &session)
        { return owner.in_flight_exact(session); }
    };
    struct Control
    {
        Phase phase{Phase::unused};
        block::SessionCounter attempts{}, completions{};
        u32 groups{};
        u64 sector{};
        bool pending{};
    };
  public:
    Ext4AllocatedEmptyDataSessionPreview() = default;
    ~Ext4AllocatedEmptyDataSessionPreview()
    {
        if (control_.pending || sealed_.pending)
            static_cast<void>(anchor_.abort(Status::interrupted("allocated empty data preview unfinished READ")));
    }
    Ext4AllocatedEmptyDataSessionPreview(const Ext4AllocatedEmptyDataSessionPreview &) = delete;
    Ext4AllocatedEmptyDataSessionPreview &operator=(const Ext4AllocatedEmptyDataSessionPreview &) = delete;
    Ext4AllocatedEmptyDataSessionPreview(Ext4AllocatedEmptyDataSessionPreview &&) = delete;
    Ext4AllocatedEmptyDataSessionPreview &operator=(Ext4AllocatedEmptyDataSessionPreview &&) = delete;

    template <block::SessionCounterPolicy Counter = block::SessionCounter>
    [[nodiscard]] static Result<Ext4AllocatedEmptyDataReadCapacity> capacity(
        const block::SessionReport &initial, u32 groups)
    {
        if (!groups || groups > Ext4RecoveryInfo::group_capacity)
            return Status::invalid_argument("allocated empty data preview group capacity");
        if (!stable(initial))
            return Status::fault("allocated empty data preview requires a stable original Session");
        // A checked inode decode reads at most D=2+4G blocks. Each old/final
        // view opens with 2+2D+1024 reads and proves ownership with
        // 2D+G+128(D+1)+2+2G reads. Old candidate proof adds eight
        // metadata reads; initial/final capture adds 2*14, including data.
        // Overlay hits cost no Session READs; counting them is conservative.
        const block::SessionCounter reads = 2876 + 1062 * block::SessionCounter(groups);
        if (!block::session_counter_fits<Counter>(initial.reads, reads) ||
            !block::session_counter_fits<Counter>(initial.completed_reads, reads))
            return Status::overflow("allocated empty data preview whole READ capacity exhausted");
        return Ext4AllocatedEmptyDataReadCapacity{reads, groups};
    }

    [[nodiscard]] Status prepare(Session &session, const Ext4RecoveryInfo &binding)
    {
        if (attempted_) return Status::already_exists("allocated empty data Session preview is one-shot");
        // Refuse borrowing aliases before changing any potentially aliased byte.
        if (overlap(this, sizeof(*this), &session, sizeof(session)) ||
            overlap(this, sizeof(*this), &binding, sizeof(binding)))
            return Status::invalid_argument("allocated empty data Session preview input overlap");
        attempted_ = true;
        if (auto status = anchor_.bind(session); !status) return refuse(status);
        original_ = original_seal_ = &session;
        if constexpr (requires { session.observe_binding(); })
        {
            const auto binding_observation = session.observe_binding();
            if (!binding_observation) return refuse(binding_observation.status());
            original_binding_ = original_binding_seal_ = binding_observation.value();
        }
        const auto actual = session.report();
        if (!actual) return refuse(actual.status());
        observed_ = actual.value();
        if (!binding.metadata_count || binding.metadata_count % 4 ||
            binding.metadata_count > binding.metadata.size() || !binding.structural_metadata().valid())
            return refuse(Status::invalid_argument("allocated empty data Session preview metadata domain"));
        const auto reserved = capacity(actual.value(), u32(binding.metadata_count / 4));
        if (!reserved) return refuse(reserved.status());
        const auto geometry = session.geometry();
        const auto bounds = session.bounds();
        const auto transfer = session.transfer_limit();
        if (!transfer) return refuse(transfer.status());
        if (geometry.block_size != 512 || !geometry.block_count || bounds.count != geometry.block_count ||
            bounds.first > ~u64{0} - bounds.count || transfer.value() < 1024 ||
            binding.layout.block_size != 1024 || !binding.layout.filesystem_blocks ||
            binding.layout.first_sector >= geometry.block_count ||
            binding.layout.filesystem_blocks > (geometry.block_count - binding.layout.first_sector) / 2)
            return refuse(Status::unsupported("allocated empty data Session preview volume geometry"));
        initial_ = initial_seal_ = progress_ = progress_seal_ = actual.value();
        geometry_ = geometry_seal_ = geometry; bounds_ = bounds_seal_ = bounds;
        binding_ = binding_seal_ = binding;
        reserved_ = reserved.value();
        control_.groups = reserved_.groups; control_.phase = Phase::preparing; sync();
        if (auto status = preview_.prepare(reader_, binding_, workspace_); !status) return reject(status);
        if (auto status = idle_exact(); !status) return status;
        if (preview_.report().read_attempts != control_.attempts ||
            preview_.report().completed_reads != control_.completions)
            return reject(Status::fault("allocated empty data preview and Session READ totals differ"));
        control_.phase = Phase::ready; sync(); return {};
    }

    [[nodiscard]] Ext4AllocatedEmptyDataSessionPreviewReport report() const
    {
        return {control_.phase, status_, reserved_, preview_.report(), initial_, observed_,
                control_.attempts, control_.completions, control_.pending};
    }
  private:
    static bool overlap(const void *a, usize a_size, const void *b, usize b_size)
    {
        const auto one = reinterpret_cast<usize>(a), two = reinterpret_cast<usize>(b);
        return one <= two ? two - one < a_size : one - two < b_size;
    }
    static bool stable(const block::SessionReport &value)
    {
        return value.state == block::DeviceState::owned && value.first_error == StatusCode::ok &&
            value.identity && value.registration_generation && value.session_serial &&
            !value.in_flight && !value.pending_writes && value.reads == value.completed_reads &&
            value.writes == value.completed_writes && value.flushes == value.completed_flushes;
    }
    static bool same_geometry(driver::BlockGeometry a, driver::BlockGeometry b)
    { return a.block_size == b.block_size && a.block_count == b.block_count && a.writable == b.writable; }
    static bool same_bounds(block::SessionBounds a, block::SessionBounds b)
    { return a.first == b.first && a.count == b.count; }
    static bool same_control(const Control &a, const Control &b)
    {
        return a.phase == b.phase && a.attempts == b.attempts && a.completions == b.completions &&
            a.groups == b.groups && a.sector == b.sector && a.pending == b.pending;
    }
    void sync() { sealed_ = control_; }
    [[nodiscard]] Status bind_owner_guard(void *context, Status (*check)(void *))
    {
        if (attempted_ || owner_guard_retired_ || owner_context_ || owner_check_ || !context || !check)
            return Status::already_exists("recovery preview owner attachment is one-shot");
        owner_context_ = owner_context_seal_ = context;
        owner_check_ = owner_check_seal_ = check;
        return {};
    }
    void retire_owner_guard()
    {
        owner_context_ = owner_context_seal_ = nullptr;
        owner_check_ = owner_check_seal_ = nullptr;
        owner_guard_retired_ = true;
    }
    [[nodiscard]] bool owner_retirement_idle() const
    {
        return status_ && owner_guard_retired_ && !owner_context_ && !owner_context_seal_ &&
            !owner_check_ && !owner_check_seal_ && !control_.pending && !sealed_.pending &&
            same_control(control_, sealed_);
    }
    [[nodiscard]] bool continuing_owner() const
    {
        if (owner_context_ != owner_context_seal_ || owner_check_ != owner_check_seal_ ||
            bool(owner_context_) != bool(owner_check_)) return false;
        return !owner_check_ || bool(owner_check_(owner_context_));
    }
    [[nodiscard]] bool continuing_binding() const
    {
        if constexpr (requires { original_->observe_binding(); })
        {
            const auto actual = anchor_.observe_binding();
            return actual && original_binding_ == original_binding_seal_ && actual.value() == original_binding_;
        }
        else return true;
    }
    [[nodiscard]] bool terminal_binding() const
    {
        if constexpr (requires { original_->observe_terminal_binding(); })
        {
            const auto actual = anchor_.observe_terminal_binding();
            return actual && original_binding_ == original_binding_seal_ && actual.value() == original_binding_;
        }
        else if constexpr (requires { original_->observe_binding(); }) return false;
        else return true;
    }
    [[nodiscard]] bool rejection_binding() const
    { return continuing_binding() || terminal_binding(); }
    [[nodiscard]] bool retained_exact(bool validate_live_boundary = true) const
    {
        if (!attempted_ || !original_ || original_ != original_seal_ || !anchor_.exact(original_) || control_.phase != Phase::preparing ||
            original_binding_ != original_binding_seal_ ||
            owner_context_ != owner_context_seal_ || owner_check_ != owner_check_seal_ ||
            bool(owner_context_) != bool(owner_check_) ||
            (validate_live_boundary && (!continuing_binding() || !continuing_owner())) ||
            !same_control(control_, sealed_) || !stable(initial_) ||
            !reclaim_history::same_session(initial_, initial_seal_) ||
            !reclaim_history::same_session(progress_, progress_seal_) ||
            !reclaim_history::same_info(binding_, binding_seal_) ||
            !same_geometry(geometry_, geometry_seal_) || !same_bounds(bounds_, bounds_seal_) ||
            control_.attempts != control_.completions || !stable(progress_)) return false;
        const auto bound = capacity(initial_, control_.groups);
        if (!bound || bound.value().reads != reserved_.reads || bound.value().groups != reserved_.groups ||
            binding_.metadata_count != control_.groups * 4 || control_.attempts > reserved_.reads)
            return false;
        auto expected = initial_;
        expected.reads += control_.attempts; expected.completed_reads += control_.completions;
        return reclaim_history::same_session(expected, progress_);
    }
    [[nodiscard]] Status idle_exact()
    {
        if (!retained_exact() || control_.pending)
            return reject(Status::fault("allocated empty data preview original idle seals changed"));
        const auto actual = original_->report();
        if (!actual) return reject(actual.status());
        observed_ = actual.value();
        if (!same_geometry(original_->geometry(), geometry_) || !same_bounds(original_->bounds(), bounds_) ||
            !reclaim_history::same_session(actual.value(), progress_))
            return reject(Status::fault("allocated empty data preview original idle Session changed"));
        return {};
    }
    [[nodiscard]] Status in_flight_exact(Session &session)
    {
        if (!retained_exact() || !anchor_.exact(&session) || original_ != &session || !control_.pending ||
            !reclaim_history::same_session(flight_, flight_seal_) ||
            !reclaim_history::same_session(flight_, progress_))
            return Status::fault("allocated empty data preview original dispatch seals changed");
        const auto actual = session.report();
        if (!actual) return actual.status();
        observed_ = actual.value();
        auto expected = flight_; ++expected.reads; expected.in_flight = true;
        if (!same_geometry(session.geometry(), geometry_) || !same_bounds(session.bounds(), bounds_) ||
            !reclaim_history::same_session(actual.value(), expected))
            return Status::fault("allocated empty data preview original in-flight Session changed");
        return {};
    }
    [[nodiscard]] Status read(u32 number, std::span<u8> out)
    {
        if (auto status = idle_exact(); !status) return status;
        if (number >= binding_.layout.filesystem_blocks || out.size() != 1024 ||
            control_.attempts >= reserved_.reads)
            return reject(Status::overflow("allocated empty data preview selected READ bounds or credit"));
        flight_ = flight_seal_ = progress_;
        control_.sector = binding_.layout.first_sector + u64(number) * 2;
        control_.pending = true; sync();
        const auto transport = original_->read_blocks_guarded(control_.sector,
            std::as_writable_bytes(out), dispatch_);
        return finish_read(transport);
    }
    [[nodiscard]] Status finish_read(Status transport)
    {
        // The Session drains and quarantines a refused guarded dispatch before
        // exact failed-prefix accounting. Its live owner is already revoked.
        if (!transport && !terminal_binding())
            return refuse(Status::fault("allocated empty data preview original terminal binding changed"));
        if (!retained_exact(bool(transport)) || !control_.pending ||
            !reclaim_history::same_session(flight_, flight_seal_))
            return reject(Status::fault("allocated empty data preview post-READ seals changed"));
        const auto actual = original_->report();
        if (!actual) return reject(actual.status());
        observed_ = actual.value();
        auto expected = flight_; ++expected.reads;
        if (transport) ++expected.completed_reads;
        else
        {
            expected.state = block::DeviceState::quarantined; expected.first_error = actual.value().first_error;
            if (expected.first_error == StatusCode::ok)
                return reject(Status::fault("allocated empty data preview failed READ lacks quarantine"));
        }
        if (!same_geometry(original_->geometry(), geometry_) || !same_bounds(original_->bounds(), bounds_) ||
            !reclaim_history::same_session(actual.value(), expected))
            return reject(Status::fault("allocated empty data preview READ attempt/completion prefix changed"));
        ++control_.attempts; if (transport) ++control_.completions;
        progress_ = progress_seal_ = actual.value();
        control_.pending = false; control_.sector = 0; sync();
        return transport ? Status{} : reject(transport);
    }
    [[nodiscard]] Status refuse(Status status)
    { control_.phase = Phase::failed; status_ = status; sync(); return status; }
    [[nodiscard]] Status reject(Status status)
    {
        if (!rejection_binding())
            return refuse(Status::fault("allocated empty data preview original rejection binding changed"));
        // The agreed original is captured once for rejection and final
        // observation; a single anchor drift must not leave an in-flight
        // report after the original request has already been drained.
        if (const auto actual = anchor_.abort_observe(status); actual) observed_ = actual.value();
        return refuse(status);
    }
    reclaim_history::SessionAnchor<Session> anchor_{};
    Session *original_{}, *original_seal_{};
    block::SessionReport initial_{}, initial_seal_{}, progress_{}, progress_seal_{}, flight_{}, flight_seal_{}, observed_{};
    driver::BlockGeometry geometry_{}, geometry_seal_{};
    block::SessionBounds bounds_{}, bounds_seal_{};
    Ext4RecoveryInfo binding_{}, binding_seal_{};
    Ext4AllocatedEmptyDataReadCapacity reserved_{};
    Control control_{}, sealed_{};
    Status status_{};
    bool attempted_{};
    block::SessionBindingObservation original_binding_{}, original_binding_seal_{};
    void *owner_context_{}, *owner_context_seal_{};
    Status (*owner_check_)(void *){}, (*owner_check_seal_)(void *){};
    bool owner_guard_retired_{};
    Reader reader_{*this};
    Dispatch dispatch_{*this};
    Ext4AllocatedEmptyDataPreviewWorkspace<Reader> workspace_{};
    Ext4AllocatedEmptyDataPreview<Reader> preview_{};
};

enum class Ext4ZeroTruncateTransactionPhase : u8
{ unused, preparing, prepared, publishing, published, inode, allocations, cleanup, final, complete, failed };
struct Ext4ZeroTruncateTransactionReport
{
    Ext4ZeroTruncateTransactionPhase phase{};
    Status status{};
    Ext4ZeroTruncateTransactionCapacity reserved{};
    block::SessionReport initial{}, observed{};
    block::SessionCounter reads{}, writes{}, flushes{}, completed_reads{}, completed_writes{}, completed_flushes{};
    u32 inode{}, generation{}, released_count{}, final_sequence{};
    u8 home_after_mask{};
    bool noop{}, dispatch_pending{};
};

// A private original-Session transaction, not a mounted filesystem entry point.
// The canonical caller excludes all other Session users/raw writers and retains
// the service, device, Session and this off-stack storage through the attempt.
template <typename Session>
class Ext4ZeroTruncateTransaction final
{
    template <usize, usize> friend class Ext4OwnedTransaction;
    using Phase = Ext4ZeroTruncateTransactionPhase;
    using Route = Ext4ZeroTruncateRoute;
    using Block = Ext4ZeroTruncateBlock;
    enum class Operation : u8 { none, read, write, flush };
    struct Reader
    {
        Ext4ZeroTruncateTransaction &owner;
        [[nodiscard]] Status read_block(u32 number, std::span<u8> bytes)
        { return owner.read(number, bytes); }
    };
    struct GeneratedReader
    {
        Ext4ZeroTruncateTransaction &owner;
        [[nodiscard]] Status read_block(u32 number, std::span<u8> bytes)
        {
            if (auto status = owner.idle(); !status) return status;
            if (bytes.size() != 1024) return owner.reject(Status::invalid_argument("zero truncate generated read size"));
            const u64 first = owner.binding_.layout.journal_first_block;
            if (number >= first && u64(number) - first < 8)
            {
                const auto &image = owner.formatted(u32(u64(number) - first));
                for (usize at = 0; at < 1024; ++at) bytes[at] = image[at];
                return {};
            }
            return owner.read(number, bytes);
        }
    };
    struct Dispatch
    {
        Ext4ZeroTruncateTransaction &owner;
        [[nodiscard]] Status in_flight_exact(Session &session)
        { return owner.in_flight_exact(session); }
    };
    struct Images
    {
        Ext4ZeroTruncatePlan plan{};
        Ext4ZeroTruncateJournalFormat journal{};
        std::array<Block, 8> original_journal{};
        std::array<Block, 4> actual_homes{};
        Block inode_bitmap{};
        u32 inode_bitmap_target{};
        u8 initial_home_mask{};
        bool ready{};
    };
    struct Control
    {
        Phase phase{};
        Operation operation{};
        block::SessionCounter reads{}, writes{}, flushes{}, completed_reads{}, completed_writes{}, completed_flushes{};
        u32 journal_cursor{}, selected{};
        block::SessionCounter publication_flushes{}, checkpoint_flushes{};
        u64 sector{};
        u8 journal_mask{}, home_mask{};
        bool pending{}, pending_writes{}, cleanup{}, generated{}, final_view{};
        friend bool operator==(const Control &, const Control &) = default;
    };
  public:
    Ext4ZeroTruncateTransaction() = default;
    ~Ext4ZeroTruncateTransaction()
    {
        if (minted_ && control_.phase != Phase::complete)
            static_cast<void>(anchor_.abort(Status::interrupted("zero truncate private transaction unfinished")));
    }
    Ext4ZeroTruncateTransaction(const Ext4ZeroTruncateTransaction &) = delete;
    Ext4ZeroTruncateTransaction &operator=(const Ext4ZeroTruncateTransaction &) = delete;
    Ext4ZeroTruncateTransaction(Ext4ZeroTruncateTransaction &&) = delete;
    Ext4ZeroTruncateTransaction &operator=(Ext4ZeroTruncateTransaction &&) = delete;
    [[nodiscard]] Ext4ZeroTruncateTransactionReport report() const
    {
        return {control_.phase, status_, reserved_, initial_, observed_, control_.reads, control_.writes,
            control_.flushes, control_.completed_reads, control_.completed_writes, control_.completed_flushes,
            images_.plan.inode, images_.plan.generation, images_.plan.released_count, reserved_.final_sequence,
            control_.home_mask, route_ == Route::noop, control_.pending};
    }
  private:
    static bool overlap(const void *a, usize as, const void *b, usize bs)
    {
        const auto one = reinterpret_cast<usize>(a), two = reinterpret_cast<usize>(b);
        return one <= two ? two - one < as : one - two < bs;
    }
    static bool same_geometry(driver::BlockGeometry a, driver::BlockGeometry b)
    { return a.block_size == b.block_size && a.block_count == b.block_count && a.writable == b.writable; }
    static bool same_bounds(block::SessionBounds a, block::SessionBounds b)
    { return a.first == b.first && a.count == b.count; }
    static bool same_update(const Ext4ZeroTruncateUpdate &a, const Ext4ZeroTruncateUpdate &b)
    { return a.inode == b.inode && a.generation == b.generation && a.old_size == b.old_size && a.seconds == b.seconds && a.nanoseconds == b.nanoseconds; }
    static bool same_plan(const Ext4ZeroTruncatePlan &a, const Ext4ZeroTruncatePlan &b)
    {
        return a.inode == b.inode && a.generation == b.generation && a.old_size == b.old_size && a.group == b.group &&
            a.released_count == b.released_count && a.released_mapping == b.released_mapping && a.targets == b.targets &&
            a.before == b.before && a.after == b.after && a.count == b.count && a.noop == b.noop;
    }
    static bool same_journal(const Ext4ZeroTruncateJournalFormat &a, const Ext4ZeroTruncateJournalFormat &b)
    {
        return a.head == b.head && a.descriptor == b.descriptor && a.commit == b.commit && a.end == b.end &&
            a.empty_head == b.empty_head && a.payloads == b.payloads && a.active_sequence == b.active_sequence &&
            a.next_sequence == b.next_sequence && a.ready == b.ready;
    }
    static bool same_images(const Images &a, const Images &b)
    {
        return same_plan(a.plan, b.plan) && same_journal(a.journal, b.journal) &&
            a.original_journal == b.original_journal && a.actual_homes == b.actual_homes &&
            a.inode_bitmap == b.inode_bitmap && a.inode_bitmap_target == b.inode_bitmap_target &&
            a.initial_home_mask == b.initial_home_mask && a.ready == b.ready;
    }
    static u32 population(u8 mask)
    { u32 count{}; for (u32 bit = 0; bit < 4; ++bit) if (mask & (1U << bit)) ++count; return count; }
    void sync() { sealed_ = control_; }
    void seal_images() { images_.ready = true; images_seal_ = images_; }
    [[nodiscard]] Status refuse(Status status)
    { control_.phase = Phase::failed; status_ = status; sync(); return status; }
    [[nodiscard]] Status reject(Status status)
    {
        if (minted_ && !rejection_binding())
            return refuse(Status::fault("zero truncate original rejection binding changed"));
        if (const auto actual = anchor_.abort_observe(status); actual) observed_ = actual.value();
        return refuse(status);
    }
    [[nodiscard]] Status mint(Session &session, const Ext4RecoveryInfo &binding,
                              Route route, Ext4ZeroTruncateUpdate update = {})
    {
        if (attempted_) return Status::already_exists("zero truncate private transaction is one-shot");
        if (overlap(this, sizeof(*this), &session, sizeof(session)) || overlap(this, sizeof(*this), &binding, sizeof(binding)))
            return Status::invalid_argument("zero truncate private transaction input overlap");
        attempted_ = true;
        if (auto status = anchor_.bind(session); !status) return refuse(status);
        original_ = original_seal_ = &session;
        if constexpr (requires { session.observe_binding(); })
        {
            const auto actual_binding = session.observe_binding();
            if (!actual_binding) return refuse(actual_binding.status());
            authoritative_binding_ = authoritative_binding_seal_ = actual_binding.value();
        }
        const auto actual = session.report(); if (!actual) return refuse(actual.status());
        observed_ = actual.value();
        if (!binding.metadata_count || binding.metadata_count % 4 || binding.metadata_count > binding.metadata.size() ||
            !binding.structural_metadata().valid() || !binding.needs_recovery ||
            binding.empty_log != (route != Route::recovery))
            return refuse(Status::invalid_argument("zero truncate private transaction binding profile"));
        const auto capacity = Ext4ZeroTruncateTransactionBudget::capacity(actual.value(), u32(binding.metadata_count / 4), route, binding.journal_sequence);
        if (!capacity) return refuse(capacity.status());
        const auto geometry = session.geometry(); const auto bounds = session.bounds();
        const auto limit = session.transfer_limit(); if (!limit) return refuse(limit.status());
        if (geometry.block_size != 512 || !geometry.block_count || bounds.count != geometry.block_count ||
            bounds.first > ~u64{0} - bounds.count || limit.value() < 1024 ||
            (route != Route::noop && !geometry.writable) || binding.layout.block_size != 1024 ||
            !binding.layout.filesystem_blocks || binding.layout.filesystem_blocks > 0xffffffffULL ||
            binding.layout.first_sector >= geometry.block_count ||
            binding.layout.filesystem_blocks > (geometry.block_count - binding.layout.first_sector) / 2)
            return refuse(Status::unsupported("zero truncate private transaction volume geometry"));
        binding_ = binding_seal_ = binding; request_ = request_seal_ = update;
        route_ = route_seal_ = route; reserved_ = capacity.value();
        initial_ = initial_seal_ = progress_ = progress_seal_ = actual.value();
        geometry_ = geometry_seal_ = geometry; bounds_ = bounds_seal_ = bounds;
        control_.phase = Phase::preparing; sync(); minted_ = true;
        return idle();
    }
    // Real BlockSessions can observe continuing registered backing without
    // callbacks or transport. Generic model proxies retain their old contract.
    [[nodiscard]] bool continuing_binding() const
    {
        if constexpr (requires { original_->observe_binding(); })
        {
            const auto actual = original_->observe_binding();
            return actual && authoritative_binding_ == authoritative_binding_seal_ &&
                actual.value() == authoritative_binding_;
        }
        else return true;
    }
    [[nodiscard]] bool terminal_binding() const
    {
        if constexpr (requires { original_->observe_terminal_binding(); })
        {
            const auto actual = anchor_.observe_terminal_binding();
            return actual && authoritative_binding_ == authoritative_binding_seal_ &&
                actual.value() == authoritative_binding_;
        }
        else if constexpr (requires { original_->observe_binding(); }) return false;
        else return true;
    }
    [[nodiscard]] bool rejection_binding() const
    {
        if constexpr (requires { original_->observe_binding(); })
        {
            const auto actual = anchor_.observe_binding();
            if (actual && authoritative_binding_ == authoritative_binding_seal_ &&
                actual.value() == authoritative_binding_) return true;
            return terminal_binding();
        }
        else return true;
    }
    [[nodiscard]] Status bind_owner_guard(void *context, Status (*check)(void *))
    {
        if (attempted_ || owner_guard_retired_ || owner_context_ || owner_check_ || !context || !check)
            return Status::already_exists("typed child owner attachment is one-shot");
        owner_context_ = owner_context_seal_ = context;
        owner_check_ = owner_check_seal_ = check;
        return {};
    }
    // The actual owner calls this on every terminal exit before its local
    // context dies. Retired callbacks cannot be rebound or dereferenced later.
    void retire_owner_guard()
    {
        recovery_preview_.retire_owner_guard();
        owner_context_ = owner_context_seal_ = nullptr;
        owner_check_ = owner_check_seal_ = nullptr;
        owner_guard_retired_ = true;
    }
    [[nodiscard]] Status retire_aborted_owner_session(Session &session)
    {
        if (!owner_guard_retired_ || owner_context_ || owner_context_seal_ || owner_check_ || owner_check_seal_ ||
            !recovery_preview_.owner_guard_retired_ || recovery_preview_.owner_context_ ||
            recovery_preview_.owner_context_seal_ || recovery_preview_.owner_check_ || recovery_preview_.owner_check_seal_)
            return Status::busy("zero failed Session retirement requires retired Owner callbacks");
        if (recovery_preview_.anchor_.exact(&session))
            if (auto status = recovery_preview_.anchor_.retire_aborted_original(session); !status) return status;
        if (anchor_.exact(&session)) return anchor_.retire_aborted_original(session);
        return {};
    }
    [[nodiscard]] bool owner_retirement_idle() const
    {
        return status_ && owner_guard_retired_ && !owner_context_ && !owner_context_seal_ &&
            !owner_check_ && !owner_check_seal_ && !control_.pending && !sealed_.pending &&
            !control_.pending_writes && !sealed_.pending_writes && control_ == sealed_ &&
            (minted_ ? control_.phase == Phase::complete :
                (!attempted_ && !original_ && !original_seal_ && control_ == Control{})) &&
            recovery_preview_.owner_retirement_idle();
    }
    [[nodiscard]] bool continuing_owner() const
    {
        if (owner_context_ != owner_context_seal_ || owner_check_ != owner_check_seal_ ||
            bool(owner_context_) != bool(owner_check_)) return false;
        return !owner_check_ || bool(owner_check_(owner_context_));
    }
    [[nodiscard]] bool completed_owner_callbacks_retired() const
    {
        const auto &preview = recovery_preview_;
        return owner_guard_retired_ && !owner_context_ && !owner_context_seal_ && !owner_check_ && !owner_check_seal_ &&
            preview.owner_guard_retired_ && !preview.owner_context_ && !preview.owner_context_seal_ &&
            !preview.owner_check_ && !preview.owner_check_seal_;
    }
    [[nodiscard]] bool retained(bool validate_live_boundary = true) const
    {
        if (!minted_ || !original_ || original_ != original_seal_ || !anchor_.exact(original_) ||
            control_.phase == Phase::failed || control_.phase == Phase::unused || control_ != sealed_ ||
            authoritative_binding_ != authoritative_binding_seal_ ||
            owner_context_ != owner_context_seal_ || owner_check_ != owner_check_seal_ ||
            bool(owner_context_) != bool(owner_check_) ||
            !completed_ownership_birth_valid() ||
            (validate_live_boundary && (!continuing_binding() || !continuing_owner())) ||
            route_ != route_seal_ || !same_update(request_, request_seal_) ||
            !reclaim_history::same_info(binding_, binding_seal_) ||
            !reclaim_history::same_session(initial_, initial_seal_) || !reclaim_history::same_session(progress_, progress_seal_) ||
            !same_geometry(geometry_, geometry_seal_) || !same_bounds(bounds_, bounds_seal_) ||
            (images_.ready && !same_images(images_, images_seal_)) || images_.ready != images_seal_.ready ||
            (control_.generated && !reclaim_history::same_info(generated_binding_, generated_binding_seal_)) ||
            (control_.final_view && !reclaim_history::same_info(final_binding_, final_binding_seal_))) return false;
        const auto capacity = Ext4ZeroTruncateTransactionBudget::capacity(initial_, u32(binding_.metadata_count / 4), route_, binding_.journal_sequence);
        if (!capacity || capacity.value() != reserved_ || control_.reads > reserved_.reads || control_.writes > reserved_.writes ||
            control_.flushes > reserved_.flushes || control_.reads != control_.completed_reads ||
            control_.writes != control_.completed_writes || control_.flushes != control_.completed_flushes ||
            control_.journal_cursor > 8 || control_.publication_flushes > 3 || control_.checkpoint_flushes > 3 ||
            (control_.home_mask & ~u8{15}) || (control_.home_mask & images_.initial_home_mask) != images_.initial_home_mask)
            return false;
        constexpr std::array<u32, 8> order{1, 2, 3, 4, 5, 7, 6, 0};
        u8 journal_mask = route_ == Route::recovery && images_.ready ? 255 : 0;
        if (route_ == Route::fresh)
            for (u32 at = 0; at < control_.journal_cursor; ++at) journal_mask |= u8(1U << order[at]);
        else if (control_.journal_cursor || control_.publication_flushes) return false;
        if (control_.journal_mask != journal_mask ||
            control_.writes != (route_ == Route::fresh ? control_.journal_cursor : 0) +
                population(u8(control_.home_mask & ~images_.initial_home_mask)) + (control_.cleanup ? 1 : 0) ||
            control_.flushes != control_.publication_flushes + control_.checkpoint_flushes ||
            (control_.checkpoint_flushes && !(control_.home_mask & 1)) ||
            (control_.checkpoint_flushes >= 2 && control_.home_mask != 15) ||
            (control_.cleanup && control_.checkpoint_flushes < 2)) return false;
        auto expected = initial_;
        expected.reads += control_.reads; expected.completed_reads += control_.completed_reads;
        expected.writes += control_.writes; expected.completed_writes += control_.completed_writes;
        expected.flushes += control_.flushes; expected.completed_flushes += control_.completed_flushes;
        expected.pending_writes = control_.pending_writes;
        return reclaim_history::same_session(expected, progress_);
    }
    [[nodiscard]] bool completed_ownership_birth_valid(bool require_complete = false) const
    {
        using Validator = Ext4ZeroTruncateOwnershipValidator;
        const bool complete = require_complete || control_.phase == Phase::complete;
        if (!Validator::completed_birth_progress_exact(ownership_birth_, ownership_birth_progress_, this)) return false;
        if (Validator::completed_birth_uncaptured(ownership_birth_, ownership_birth_progress_, this)) return !complete;
        if (&reader_.owner != this || (complete && !Validator::completed_birth_complete(ownership_birth_))) return false;
        return route_ == Route::noop ?
            Validator::completed_birth_exact(old_view_, reader_, binding_, old_proof_, ownership_birth_, this, complete) :
            Validator::completed_birth_exact(final_view_, reader_, final_binding_, final_proof_, ownership_birth_, this, complete);
    }
    [[nodiscard]] Status idle()
    {
        if (!retained() || control_.pending) return reject(Status::fault("zero truncate private original idle seals changed"));
        const auto actual = original_->report(); if (!actual) return reject(actual.status());
        observed_ = actual.value();
        if (!same_geometry(original_->geometry(), geometry_) || !same_bounds(original_->bounds(), bounds_) ||
            !reclaim_history::same_session(actual.value(), progress_))
            return reject(Status::fault("zero truncate private original idle Session changed"));
        return {};
    }
    [[nodiscard]] Status in_flight_exact(Session &session)
    {
        if (!retained() || !control_.pending || control_.operation == Operation::none ||
            !anchor_.exact(&session) || original_ != &session || !reclaim_history::same_session(flight_, flight_seal_) ||
            !reclaim_history::same_session(flight_, progress_))
            return reject(Status::fault("zero truncate private dispatch seals changed"));
        auto expected = flight_; expected.in_flight = true;
        if (control_.operation == Operation::read) ++expected.reads;
        if (control_.operation == Operation::write) { ++expected.writes; expected.pending_writes = true; }
        if (control_.operation == Operation::flush) ++expected.flushes;
        const auto actual = session.report(); if (!actual) return reject(actual.status());
        observed_ = actual.value();
        if (!same_geometry(session.geometry(), geometry_) || !same_bounds(session.bounds(), bounds_) ||
            !reclaim_history::same_session(actual.value(), expected))
            return reject(Status::fault("zero truncate private in-flight Session changed"));
        return {};
    }
    [[nodiscard]] Status finish_io(Status transport)
    {
        // Prove the opening backing before any post-failure report or abort.
        // A same-number foreign quarantine must never supply completion credit.
        if (!transport && !terminal_binding())
            return refuse(Status::fault("zero truncate original terminal binding changed"));
        // Failed guarded transport has already quarantined the Session and
        // removed its live owner. Retain every fixed seal, account the exact
        // failed prefix and stop; no further live admission or I/O is attempted.
        if (!retained(bool(transport)) || !control_.pending || !reclaim_history::same_session(flight_, flight_seal_))
            return reject(Status::fault("zero truncate private post-return seals changed"));
        const auto actual = original_->report(); if (!actual) return reject(actual.status());
        observed_ = actual.value(); auto expected = flight_;
        if (control_.operation == Operation::read) { ++expected.reads; if (transport) ++expected.completed_reads; }
        if (control_.operation == Operation::write)
        { ++expected.writes; expected.pending_writes = true; if (transport) ++expected.completed_writes; }
        if (control_.operation == Operation::flush)
        { ++expected.flushes; if (transport) { ++expected.completed_flushes; expected.pending_writes = false; } }
        if (!transport)
        {
            expected.state = block::DeviceState::quarantined; expected.first_error = actual.value().first_error;
            if (expected.first_error == StatusCode::ok) return reject(Status::fault("zero truncate failed I/O lacks quarantine"));
        }
        if (!same_geometry(original_->geometry(), geometry_) || !same_bounds(original_->bounds(), bounds_) ||
            !reclaim_history::same_session(actual.value(), expected))
            return reject(Status::fault("zero truncate exact attempted/completed I/O prefix changed"));
        if (!transport)
        {
            if (control_.operation == Operation::read) ++control_.reads;
            if (control_.operation == Operation::write) ++control_.writes;
            if (control_.operation == Operation::flush) ++control_.flushes;
            progress_ = progress_seal_ = actual.value(); control_.pending_writes = actual.value().pending_writes;
            control_.pending = false; control_.operation = Operation::none; control_.sector = 0; control_.selected = 0;
            return reject(transport);
        }
        if (control_.operation == Operation::read) { ++control_.reads; ++control_.completed_reads; }
        if (control_.operation == Operation::write)
        {
            ++control_.writes; ++control_.completed_writes;
            if (control_.phase == Phase::publishing)
            { control_.journal_mask |= u8(1U << control_.selected); ++control_.journal_cursor; }
            else if (control_.phase == Phase::cleanup) control_.cleanup = true;
            else control_.home_mask |= u8(1U << control_.selected);
        }
        if (control_.operation == Operation::flush)
        {
            ++control_.flushes; ++control_.completed_flushes;
            if (control_.phase == Phase::publishing) ++control_.publication_flushes;
            else ++control_.checkpoint_flushes;
        }
        progress_ = progress_seal_ = actual.value(); control_.pending_writes = actual.value().pending_writes;
        control_.pending = false; control_.operation = Operation::none; control_.sector = 0; control_.selected = 0; sync();
        return idle();
    }
    [[nodiscard]] Status begin_io(Operation kind, u32 selected, u32 physical = 0)
    {
        if (auto status = idle(); !status) return status;
        if ((kind == Operation::read && (control_.reads == reserved_.reads || control_.pending_writes)) ||
            (kind == Operation::write && control_.writes == reserved_.writes) ||
            (kind == Operation::flush && control_.flushes == reserved_.flushes) ||
            (kind != Operation::flush && physical >= binding_.layout.filesystem_blocks))
            return reject(Status::overflow("zero truncate selected I/O bound or whole credit"));
        flight_ = flight_seal_ = progress_; control_.operation = kind; control_.selected = selected;
        control_.sector = kind == Operation::flush ? 0 : binding_.layout.first_sector + u64(physical) * 2;
        control_.pending = true; sync(); return {};
    }
    [[nodiscard]] Status read(u32 number, std::span<u8> bytes)
    {
        if (bytes.size() != 1024) return reject(Status::invalid_argument("zero truncate selected READ size"));
        if (auto status = begin_io(Operation::read, number, number); !status) return status;
        return finish_io(original_->read_blocks_guarded(control_.sector, std::as_writable_bytes(bytes), dispatch_));
    }
    const Block &formatted(u32 record) const
    {
        if (!record) return images_.journal.head;
        if (record == 1) return images_.journal.descriptor;
        if (record < 6) return images_.journal.payloads[record - 2];
        return record == 6 ? images_.journal.commit : images_.journal.end;
    }
    [[nodiscard]] Status write_selected()
    {
        if (auto status = idle(); !status) return status;
        u32 selected{}, physical{}; const Block *image{};
        if (control_.phase == Phase::publishing && route_ == Route::fresh)
        {
            constexpr std::array<u32, 8> order{1, 2, 3, 4, 5, 7, 6, 0};
            if (control_.journal_cursor >= 8 || control_.publication_flushes !=
                (control_.journal_cursor < 6 ? 0U : control_.journal_cursor - 5))
                return reject(Status::denied("zero truncate WAL write is not the selected stage"));
            selected = order[control_.journal_cursor]; physical = u32(binding_.layout.journal_first_block) + selected;
            image = &formatted(selected);
        }
        else if (control_.phase == Phase::inode && !control_.checkpoint_flushes && !(control_.home_mask & 1))
        { selected = 0; physical = images_.plan.targets[0]; image = &images_.plan.after[0]; }
        else if (control_.phase == Phase::allocations && control_.checkpoint_flushes == 1 && (control_.home_mask & 1))
        {
            selected = 1; while (selected < 4 && (control_.home_mask & (1U << selected))) ++selected;
            if (selected == 4) return reject(Status::denied("zero truncate allocation stage is complete"));
            physical = images_.plan.targets[selected]; image = &images_.plan.after[selected];
        }
        else if (control_.phase == Phase::cleanup && control_.checkpoint_flushes == 2 &&
                 control_.home_mask == 15 && !control_.cleanup)
        { physical = u32(binding_.layout.journal_first_block); image = &images_.journal.empty_head; }
        else return reject(Status::denied("zero truncate WRITE lacks private selected stage"));
        if (!images_.ready || images_.plan.count != 4 || !images_.journal.ready)
            return reject(Status::fault("zero truncate WRITE lacks complete retained images"));
        if (auto status = begin_io(Operation::write, selected, physical); !status) return status;
        return finish_io(original_->write_blocks_guarded(control_.sector, std::as_bytes(std::span{*image}), dispatch_));
    }
    [[nodiscard]] Status flush_selected()
    {
        if (auto status = idle(); !status) return status;
        const bool publication = control_.phase == Phase::publishing && route_ == Route::fresh &&
            ((control_.journal_cursor == 6 && !control_.publication_flushes) ||
             (control_.journal_cursor == 7 && control_.publication_flushes == 1) ||
             (control_.journal_cursor == 8 && control_.publication_flushes == 2));
        const bool checkpoint = (control_.phase == Phase::inode && !control_.checkpoint_flushes && (control_.home_mask & 1)) ||
            (control_.phase == Phase::allocations && control_.checkpoint_flushes == 1 && control_.home_mask == 15) ||
            (control_.phase == Phase::cleanup && control_.checkpoint_flushes == 2 && control_.cleanup);
        if (!publication && !checkpoint) return reject(Status::denied("zero truncate FLUSH lacks private selected barrier"));
        if (auto status = begin_io(Operation::flush, 0); !status) return status;
        return finish_io(original_->flush_guarded(dispatch_));
    }
    [[nodiscard]] Status physical()
    {
        if (auto status = idle(); !status) return status;
        if (!images_.ready || control_.pending_writes) return reject(Status::fault("zero truncate physical readback lacks stable history"));
        const auto check = [&](u32 number, const Block &expected) -> Status {
            if (auto status = read(number, checking_); !status) return status;
            return checking_ == expected ? Status{} : reject(Status::io_error("zero truncate exact physical prefix changed"));
        };
        for (u32 record = 0; record < 8; ++record)
        {
            const auto &expected = !record && control_.cleanup ? images_.journal.empty_head :
                control_.journal_mask & (1U << record) ? formatted(record) : images_.original_journal[record];
            if (auto status = check(u32(binding_.layout.journal_first_block) + record, expected); !status) return status;
        }
        if (auto status = check(images_.inode_bitmap_target, images_.inode_bitmap); !status) return status;
        for (u32 home = 0; home < 4; ++home)
            if (auto status = check(images_.plan.targets[home], control_.home_mask & (1U << home) ?
                    images_.plan.after[home] : images_.plan.before[home]); !status) return status;
        return idle();
    }
    [[nodiscard]] Status prepare(Session &session, const Ext4RecoveryInfo &binding, Ext4ZeroTruncateUpdate update)
    {
        if (!update.inode || update.old_size > 16384 ||
            (update.old_size && (update.seconds > 0x7fffffffU || update.nanoseconds >= 1000000000U)))
            return Status::invalid_argument("zero truncate private update profile");
        if (auto status = mint(session, binding, update.old_size ? Route::fresh : Route::noop, update); !status) return status;
        if (auto status = old_view_.open(reader_, binding_, opening_); !status) return reject(status);
        const auto ownership = route_ == Route::noop ?
            Ext4ZeroTruncateOwnershipValidator::prove_completed(binding_, old_view_, reader_, request_, ownership_, old_proof_, ownership_birth_, ownership_birth_progress_, this) :
            Ext4ZeroTruncateOwnershipValidator::prove(binding_, old_view_, request_, ownership_, old_proof_);
        if (!ownership)
            return reject(ownership);
        const u32 group = old_proof_.count ? old_proof_.group : 0;
        const u32 per_group = ext4_checked::le32(ownership_.superblock, 40);
        const u32 inode_group = (request_.inode - 1) / per_group, inode_local = (request_.inode - 1) % per_group;
        const u32 table = ext4_checked::le32(ownership_.descriptors, inode_group * 32 + 8) + inode_local / 4;
        const u32 bitmap = ext4_checked::le32(ownership_.descriptors, group * 32);
        images_.inode_bitmap_target = ext4_checked::le32(ownership_.descriptors, inode_group * 32 + 4);
        const std::array<u32, 4> homes{table, bitmap, 2, 1};
        for (u32 record = 0; record < 8; ++record)
        {
            if (auto status = read(u32(binding_.layout.journal_first_block) + record, checking_); !status) return status;
            images_.original_journal[record] = checking_;
        }
        for (u32 home = 0; home < 4; ++home)
        {
            if (auto status = read(homes[home], checking_); !status) return status;
            images_.plan.before[home] = checking_;
        }
        if (auto status = read(images_.inode_bitmap_target, checking_); !status) return status;
        images_.inode_bitmap = checking_;
        if (images_.plan.before[3] != ownership_.superblock || images_.plan.before[2] != ownership_.descriptors ||
            !std::equal(ownership_.inode.begin(), ownership_.inode.end(), images_.plan.before[0].begin() + (inode_local % 4) * 256))
            return reject(Status::fault("zero truncate captured old identity differs from complete proof"));
        if (route_ == Route::noop)
        {
            const auto head = jbd2::Codec::open(images_.original_journal[0], u32(binding_.layout.journal_blocks), binding_.uuid,
                jbd2::DescriptorUuidPolicy::linux_internal_zero);
            if (!head) return reject(head.status());
            if (head.value().superblock().start_block || head.value().superblock().sequence != binding_.journal_sequence ||
                head.value().superblock().block_size != 1024 || head.value().superblock().first_block != 1)
                return reject(Status::unsupported("zero truncate no-op dirty empty journal differs"));
            images_.plan.inode = request_.inode; images_.plan.generation = request_.generation;
            images_.plan.group = group; images_.plan.targets = homes; images_.plan.after = images_.plan.before;
            images_.plan.noop = true; seal_images();
            if (auto status = physical(); !status) return status;
            control_.phase = Phase::complete; sync(); return idle();
        }
        const Ext4ZeroTruncateSource source{group, table, images_.plan.before[3], images_.plan.before[2],
            images_.plan.before[1], images_.inode_bitmap, images_.plan.before[0]};
        if (auto status = Ext4ZeroTruncatePlanner::plan_off_stack(binding_, request_, source, planning_, planned_); !status)
            return reject(status);
        if (planned_.released_mapping != old_proof_.mapping || planned_.released_count != old_proof_.count ||
            planned_.group != old_proof_.group || planned_.targets != homes)
            return reject(Status::fault("zero truncate plan differs from original complete ownership"));
        images_.plan = planned_;
        if (auto status = Ext4ZeroTruncateJournalFormatter::format(binding_, images_.plan, images_.original_journal[0],
                images_.inode_bitmap, formatter_, images_.journal); !status) return reject(status);
        seal_images(); generated_binding_ = generated_binding_seal_ = binding_;
        generated_binding_.empty_log = generated_binding_seal_.empty_log = false;
        generated_binding_.journal_sequence = generated_binding_seal_.journal_sequence = images_.journal.active_sequence;
        control_.generated = true; sync();
        if (auto status = generated_preview_.prepare(generated_reader_, generated_binding_, generated_workspace_); !status)
            return reject(status);
        if (generated_workspace_.claimant_ != &generated_preview_ || generated_preview_.workspace_ != &generated_workspace_ ||
            !same_plan(generated_workspace_.journal_.decoded, images_.plan) || generated_workspace_.result_.after_mask ||
            generated_preview_.report_.phase != Ext4ZeroTruncatePreviewPhase::ready)
            return reject(Status::fault("zero truncate generated complete preview differs"));
        if (auto status = physical(); !status) return status;
        control_.phase = Phase::prepared; sync(); return idle();
    }
    [[nodiscard]] Status prepare_recovery(Session &session, const Ext4RecoveryInfo &binding)
    {
        if (auto status = mint(session, binding, Route::recovery); !status) return status;
        // The delegated reader retains the actual parent, whose fixed checks
        // include the outer canonical Owner/cache context on every dispatch.
        if (auto status = recovery_preview_.bind_owner_guard(this, recovery_owner_guard); !status)
        { recovery_preview_.retire_owner_guard(); return reject(status); }
        const auto preview_status = recovery_preview_.prepare(session, binding_);
        recovery_preview_.retire_owner_guard();
        if (!preview_status) return reject(preview_status);
        if (!retained()) return reject(Status::fault("zero truncate recovery parent changed during readonly preview"));
        const auto &child = recovery_preview_; const auto &workspace = child.workspace_;
        const auto actual = original_->report(); if (!actual) return reject(actual.status());
        const block::SessionCounter reads = child.control_.attempts;
        if (reads > reserved_.reads)
            return reject(Status::fault("zero truncate recovery preview exceeds whole reserved reads"));
        auto expected = progress_; expected.reads += reads; expected.completed_reads += reads;
        if (reads > reserved_.reads || child.control_.completions != reads || child.control_.phase != Ext4ZeroTruncateSessionPreviewPhase::ready ||
            !child.anchor_.exact(original_) || child.original_ != original_ || child.original_seal_ != original_ ||
            !reclaim_history::same_info(child.binding_, binding_) || !reclaim_history::same_info(child.binding_seal_, binding_) ||
            !reclaim_history::same_session(child.initial_, progress_) || !reclaim_history::same_session(child.initial_seal_, progress_) ||
            !reclaim_history::same_session(child.progress_, expected) || !reclaim_history::same_session(child.progress_seal_, expected) ||
            !reclaim_history::same_session(actual.value(), expected) || !reclaim_history::same_session(child.observed_, expected) ||
            !child.same_control(child.control_, child.sealed_) || child.control_.pending || !child.status_ ||
            child.original_binding_ != authoritative_binding_ || child.original_binding_seal_ != authoritative_binding_ ||
            !child.owner_guard_retired_ || child.owner_context_ || child.owner_context_seal_ ||
            child.owner_check_ || child.owner_check_seal_ ||
            !same_geometry(child.geometry_, geometry_) || !same_geometry(child.geometry_seal_, geometry_) ||
            !same_bounds(child.bounds_, bounds_) || !same_bounds(child.bounds_seal_, bounds_) ||
            workspace.claimant_ != &child.preview_ ||
            child.preview_.workspace_ != &workspace || workspace.reader_ != &child.reader_ || !workspace.journal_ready_ ||
            child.preview_.report_.phase != Ext4ZeroTruncatePreviewPhase::ready)
            return reject(Status::fault("zero truncate recovery exact private preview handoff differs"));
        control_.reads = control_.completed_reads = reads; progress_ = progress_seal_ = observed_ = actual.value(); sync();
        images_.plan = workspace.journal_.decoded; images_.inode_bitmap = workspace.inode_bitmap_;
        images_.inode_bitmap_target = workspace.inode_bitmap_target_; images_.actual_homes = workspace.homes_;
        images_.initial_home_mask = control_.home_mask = workspace.result_.after_mask;
        images_.original_journal[0] = workspace.head_; images_.original_journal[1] = workspace.descriptor_;
        for (u32 at = 0; at < 4; ++at) images_.original_journal[at + 2] = workspace.payloads_[at];
        images_.original_journal[6] = workspace.commit_; images_.original_journal[7] = workspace.end_;
        format_binding_ = binding_; format_binding_.empty_log = true; --format_binding_.journal_sequence;
        format_head_ = workspace.head_;
        be32(format_head_, 24, format_binding_.journal_sequence); be32(format_head_, 28, 0); stamp_head(format_head_);
        if (auto status = Ext4ZeroTruncateJournalFormatter::format(format_binding_, images_.plan, format_head_,
                images_.inode_bitmap, formatter_, images_.journal); !status) return reject(status);
        for (u32 record = 0; record < 8; ++record)
            if (formatted(record) != images_.original_journal[record])
                return reject(Status::fault("zero truncate recovery regenerated active WAL differs"));
        control_.journal_mask = 255; seal_images(); control_.phase = Phase::published; sync(); return idle();
    }
    [[nodiscard]] static Status recovery_owner_guard(void *raw)
    {
        auto &parent = *static_cast<Ext4ZeroTruncateTransaction *>(raw);
        return parent.retained() ? Status{} :
            Status::fault("zero truncate delegated original parent/Owner changed");
    }
    [[nodiscard]] Status publish()
    {
        if (control_.phase != Phase::prepared || route_ != Route::fresh)
            return Status::bad_descriptor("zero truncate publication lacks private fresh preparation");
        if (auto status = physical(); !status) return status;
        control_.phase = Phase::publishing; sync();
        while (control_.journal_cursor < 6) if (auto status = write_selected(); !status) return status;
        if (auto status = flush_selected(); !status) return status;
        if (auto status = physical(); !status) return status;
        if (auto status = write_selected(); !status) return status;
        if (auto status = flush_selected(); !status) return status;
        if (auto status = physical(); !status) return status;
        if (auto status = write_selected(); !status) return status;
        if (auto status = flush_selected(); !status) return status;
        if (auto status = physical(); !status) return status;
        control_.phase = Phase::published; sync(); return idle();
    }
    [[nodiscard]] Status checkpoint()
    {
        if (control_.phase != Phase::published || route_ == Route::noop)
            return Status::bad_descriptor("zero truncate checkpoint lacks private complete publication");
        if (auto status = physical(); !status) return status;
        control_.phase = Phase::inode; sync();
        if (!(control_.home_mask & 1)) if (auto status = write_selected(); !status) return status;
        // Obtain this barrier even when an inconsistent legacy mixture already
        // contains allocation-after homes and an inode-after image.
        if (auto status = flush_selected(); !status) return status;
        if (auto status = physical(); !status) return status;
        control_.phase = Phase::allocations; sync();
        while (control_.home_mask != 15) if (auto status = write_selected(); !status) return status;
        if (auto status = flush_selected(); !status) return status;
        if (auto status = physical(); !status) return status;
        control_.phase = Phase::cleanup; sync();
        if (auto status = write_selected(); !status) return status;
        if (auto status = flush_selected(); !status) return status;
        if (auto status = physical(); !status) return status;
        final_binding_ = final_binding_seal_ = binding_;
        final_binding_.empty_log = final_binding_seal_.empty_log = true;
        final_binding_.journal_sequence = final_binding_seal_.journal_sequence = reserved_.final_sequence;
        control_.phase = Phase::final; control_.final_view = true; sync();
        if (auto status = final_view_.open(reader_, final_binding_, opening_); !status) return reject(status);
        const Ext4ZeroTruncateUpdate empty{images_.plan.inode, images_.plan.generation, 0, 0, 0};
        if (auto status = Ext4ZeroTruncateOwnershipValidator::prove_completed(final_binding_, final_view_, reader_,
                empty, ownership_, final_proof_, ownership_birth_, ownership_birth_progress_, this); !status)
            return reject(status);
        if (final_proof_.count || final_proof_.inode != images_.plan.inode || final_proof_.generation != images_.plan.generation)
            return reject(Status::fault("zero truncate final physical ownership identity differs"));
        if (auto status = physical(); !status) return status;
        control_.phase = Phase::complete; sync(); return idle();
    }
    static void be32(Block &bytes, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - index * 8)); }
    static void stamp_head(Block &bytes)
    { be32(bytes, 252, 0); be32(bytes, 252, jbd2::detail::crc32c(~u32{0}, bytes)); }

    reclaim_history::SessionAnchor<Session> anchor_{};
    Session *original_{}, *original_seal_{};
    Route route_{}, route_seal_{};
    Ext4RecoveryInfo binding_{}, binding_seal_{}, generated_binding_{}, generated_binding_seal_{},
        final_binding_{}, final_binding_seal_{}, format_binding_{};
    Ext4ZeroTruncateUpdate request_{}, request_seal_{};
    block::SessionReport initial_{}, initial_seal_{}, progress_{}, progress_seal_{}, flight_{}, flight_seal_{}, observed_{};
    bool owner_guard_retired_{};
    void *owner_context_{}, *owner_context_seal_{};
    Status (*owner_check_)(void *){}, (*owner_check_seal_)(void *){};
    block::SessionBindingObservation authoritative_binding_{}, authoritative_binding_seal_{};
    driver::BlockGeometry geometry_{}, geometry_seal_{};
    block::SessionBounds bounds_{}, bounds_seal_{};
    Ext4ZeroTruncateTransactionCapacity reserved_{};
    Control control_{}, sealed_{};
    Images images_{}, images_seal_{};
    Status status_{};
    bool attempted_{}, minted_{};
    Block checking_{}, format_head_{};
    Reader reader_{*this}; GeneratedReader generated_reader_{*this}; Dispatch dispatch_{*this};
    Ext4ReadOnlyOpenScratch opening_{};
    Ext4ReadOnlyView<Reader> old_view_{}, final_view_{};
    Ext4ZeroTruncateOwnershipScratch ownership_{};
    Ext4ZeroTruncateOwnership old_proof_{}, final_proof_{};
    Ext4ZeroTruncateOwnershipValidator::CompletedBirthSeal ownership_birth_{};
    Ext4ZeroTruncateOwnershipValidator::CompletedBirthProgress ownership_birth_progress_{};
    Ext4ZeroTruncatePlan planning_{}, planned_{};
    Ext4ZeroTruncateJournalFormatScratch formatter_{};
    Ext4ZeroTruncatePreviewWorkspace<GeneratedReader> generated_workspace_{};
    Ext4ZeroTruncatePreview<GeneratedReader> generated_preview_{};
    Ext4ZeroTruncateSessionPreview<Session> recovery_preview_{};
};

enum class Ext4AllocatedEmptyDataTransactionPhase : u8
{ unused, preparing, prepared, data, publishing, published, allocations, inode, cleanup, final, complete, failed };
struct Ext4AllocatedEmptyDataTransactionReport
{
    Ext4AllocatedEmptyDataTransactionPhase phase{};
    Status status{};
    Ext4AllocatedEmptyDataTransactionCapacity reserved{};
    block::SessionReport initial{}, observed{};
    block::SessionCounter reads{}, writes{}, flushes{}, completed_reads{}, completed_writes{}, completed_flushes{};
    u32 inode{}, generation{}, physical{}, size{}, final_sequence{};
    u8 home_after_mask{};
    bool data_written{}, data_flushed{}, dispatch_pending{};
};

// A private original-Session transaction, not a mounted filesystem entry point.
// The canonical caller excludes all other Session users/raw writers and retains
// the service, device, Session and this off-stack storage through the attempt.
template <typename Session>
class Ext4AllocatedEmptyDataTransaction final
{
    template <usize, usize> friend class Ext4OwnedTransaction;
    using Phase = Ext4AllocatedEmptyDataTransactionPhase;
    using Route = Ext4AllocatedEmptyDataRoute;
    using Block = Ext4FirstDataBlock;
    enum class Operation : u8 { none, read, write, flush };
    struct Reader
    {
        Ext4AllocatedEmptyDataTransaction &owner;
        [[nodiscard]] Status read_block(u32 number, std::span<u8> bytes)
        { return owner.read(number, bytes); }
    };
    struct GeneratedReader
    {
        Ext4AllocatedEmptyDataTransaction &owner;
        [[nodiscard]] Status read_block(u32 number, std::span<u8> bytes)
        {
            if (auto status = owner.idle(); !status) return status;
            if (bytes.size() != 1024) return owner.reject(Status::invalid_argument("allocated empty data generated read size"));
            const u64 first = owner.binding_.layout.journal_first_block;
            if (number >= first && u64(number) - first < 8)
            {
                const auto &image = owner.formatted(u32(u64(number) - first));
                for (usize at = 0; at < 1024; ++at) bytes[at] = image[at];
                return {};
            }
            return owner.read(number, bytes);
        }
    };
    struct Dispatch
    {
        Ext4AllocatedEmptyDataTransaction &owner;
        [[nodiscard]] Status in_flight_exact(Session &session)
        { return owner.in_flight_exact(session); }
    };
    struct Images
    {
        Ext4AllocatedEmptyDataPlan plan{};
        Ext4AllocatedEmptyDataJournalFormat journal{};
        std::array<Block, 8> original_journal{};
        std::array<Block, 4> actual_homes{};
        Block inode_bitmap{}, original_data{}, data_image{};
        u32 inode_bitmap_target{};
        u8 initial_home_mask{};
        bool ready{};
    };
    struct Control
    {
        Phase phase{};
        Operation operation{};
        block::SessionCounter reads{}, writes{}, flushes{}, completed_reads{}, completed_writes{}, completed_flushes{};
        u32 journal_cursor{}, selected{};
        block::SessionCounter publication_flushes{}, checkpoint_flushes{};
        u64 sector{};
        u8 journal_mask{}, home_mask{};
        bool pending{}, pending_writes{}, cleanup{}, generated{}, final_view{}, data_written{}, data_flushed{};
        friend bool operator==(const Control &, const Control &) = default;
    };
  public:
    Ext4AllocatedEmptyDataTransaction() = default;
    ~Ext4AllocatedEmptyDataTransaction()
    {
        if (minted_ && control_.phase != Phase::complete)
            static_cast<void>(anchor_.abort(Status::interrupted("allocated empty data private transaction unfinished")));
    }
    Ext4AllocatedEmptyDataTransaction(const Ext4AllocatedEmptyDataTransaction &) = delete;
    Ext4AllocatedEmptyDataTransaction &operator=(const Ext4AllocatedEmptyDataTransaction &) = delete;
    Ext4AllocatedEmptyDataTransaction(Ext4AllocatedEmptyDataTransaction &&) = delete;
    Ext4AllocatedEmptyDataTransaction &operator=(Ext4AllocatedEmptyDataTransaction &&) = delete;
    [[nodiscard]] Ext4AllocatedEmptyDataTransactionReport report() const
    {
        return {control_.phase, status_, reserved_, initial_, observed_, control_.reads, control_.writes,
            control_.flushes, control_.completed_reads, control_.completed_writes, control_.completed_flushes,
            images_.plan.update.inode, images_.plan.update.generation, images_.plan.physical, images_.plan.update.size, reserved_.final_sequence,
            control_.home_mask, control_.data_written, control_.data_flushed, control_.pending};
    }
  private:
    static bool overlap(const void *a, usize as, const void *b, usize bs)
    {
        const auto one = reinterpret_cast<usize>(a), two = reinterpret_cast<usize>(b);
        return one <= two ? two - one < as : one - two < bs;
    }
    static bool same_geometry(driver::BlockGeometry a, driver::BlockGeometry b)
    { return a.block_size == b.block_size && a.block_count == b.block_count && a.writable == b.writable; }
    static bool same_bounds(block::SessionBounds a, block::SessionBounds b)
    { return a.first == b.first && a.count == b.count; }
    static bool same_update(const Ext4AllocatedEmptyDataUpdate &a, const Ext4AllocatedEmptyDataUpdate &b)
    { return a.inode == b.inode && a.generation == b.generation && a.size == b.size && a.seconds == b.seconds && a.nanoseconds == b.nanoseconds; }
    static bool same_plan(const Ext4AllocatedEmptyDataPlan &a, const Ext4AllocatedEmptyDataPlan &b)
    {
        return same_update(a.update, b.update) && a.group == b.group && a.physical == b.physical &&
            a.old_version == b.old_version && a.new_version == b.new_version && a.targets == b.targets &&
            a.before == b.before && a.after == b.after && a.count == b.count;
    }
    static bool same_journal(const Ext4AllocatedEmptyDataJournalFormat &a, const Ext4AllocatedEmptyDataJournalFormat &b)
    {
        return a.head == b.head && a.descriptor == b.descriptor && a.commit == b.commit && a.end == b.end &&
            a.empty_head == b.empty_head && a.payloads == b.payloads && a.active_sequence == b.active_sequence &&
            a.next_sequence == b.next_sequence && a.ready == b.ready;
    }
    static bool same_images(const Images &a, const Images &b)
    {
        return same_plan(a.plan, b.plan) && same_journal(a.journal, b.journal) &&
            a.original_journal == b.original_journal && a.actual_homes == b.actual_homes &&
            a.inode_bitmap == b.inode_bitmap && a.original_data == b.original_data && a.data_image == b.data_image && a.inode_bitmap_target == b.inode_bitmap_target &&
            a.initial_home_mask == b.initial_home_mask && a.ready == b.ready;
    }
    static u32 population(u8 mask)
    { u32 count{}; for (u32 bit = 0; bit < 4; ++bit) if (mask & (1U << bit)) ++count; return count; }
    void sync() { sealed_ = control_; }
    void seal_images() { images_.ready = true; images_seal_ = images_; }
    [[nodiscard]] Status refuse(Status status)
    { control_.phase = Phase::failed; status_ = status; sync(); return status; }
    [[nodiscard]] Status reject(Status status)
    {
        if (minted_ && !rejection_binding())
            return refuse(Status::fault("allocated empty data original rejection binding changed"));
        if (const auto actual = anchor_.abort_observe(status); actual) observed_ = actual.value();
        return refuse(status);
    }
    [[nodiscard]] Status mint(Session &session, const Ext4RecoveryInfo &binding,
                              Route route, Ext4AllocatedEmptyDataUpdate update = {})
    {
        if (attempted_) return Status::already_exists("allocated empty data private transaction is one-shot");
        if (overlap(this, sizeof(*this), &session, sizeof(session)) || overlap(this, sizeof(*this), &binding, sizeof(binding)))
            return Status::invalid_argument("allocated empty data private transaction input overlap");
        attempted_ = true;
        if (auto status = anchor_.bind(session); !status) return refuse(status);
        original_ = original_seal_ = &session;
        if constexpr (requires { session.observe_binding(); })
        {
            const auto actual_binding = session.observe_binding();
            if (!actual_binding) return refuse(actual_binding.status());
            authoritative_binding_ = authoritative_binding_seal_ = actual_binding.value();
        }
        const auto actual = session.report(); if (!actual) return refuse(actual.status());
        observed_ = actual.value();
        if (!binding.metadata_count || binding.metadata_count % 4 || binding.metadata_count > binding.metadata.size() ||
            !binding.structural_metadata().valid() || !binding.needs_recovery ||
            binding.empty_log != (route != Route::recovery))
            return refuse(Status::invalid_argument("allocated empty data private transaction binding profile"));
        const auto capacity = Ext4AllocatedEmptyDataTransactionBudget::capacity(actual.value(), u32(binding.metadata_count / 4), route, binding.journal_sequence);
        if (!capacity) return refuse(capacity.status());
        const auto geometry = session.geometry(); const auto bounds = session.bounds();
        const auto limit = session.transfer_limit(); if (!limit) return refuse(limit.status());
        if (geometry.block_size != 512 || !geometry.block_count || bounds.count != geometry.block_count ||
            bounds.first > ~u64{0} - bounds.count || limit.value() < 1024 ||
            !geometry.writable || binding.layout.block_size != 1024 ||
            !binding.layout.filesystem_blocks || binding.layout.filesystem_blocks > 0xffffffffULL ||
            binding.layout.first_sector >= geometry.block_count ||
            binding.layout.filesystem_blocks > (geometry.block_count - binding.layout.first_sector) / 2)
            return refuse(Status::unsupported("allocated empty data private transaction volume geometry"));
        binding_ = binding_seal_ = binding; request_ = request_seal_ = update;
        route_ = route_seal_ = route; reserved_ = capacity.value();
        initial_ = initial_seal_ = progress_ = progress_seal_ = actual.value();
        geometry_ = geometry_seal_ = geometry; bounds_ = bounds_seal_ = bounds;
        control_.phase = Phase::preparing; sync(); minted_ = true;
        return idle();
    }
    // Real BlockSessions can observe continuing registered backing without
    // callbacks or transport. Generic model proxies retain their old contract.
    [[nodiscard]] bool continuing_binding() const
    {
        if constexpr (requires { original_->observe_binding(); })
        {
            const auto actual = original_->observe_binding();
            return actual && authoritative_binding_ == authoritative_binding_seal_ &&
                actual.value() == authoritative_binding_;
        }
        else return true;
    }
    [[nodiscard]] bool terminal_binding() const
    {
        if constexpr (requires { original_->observe_terminal_binding(); })
        {
            const auto actual = anchor_.observe_terminal_binding();
            return actual && authoritative_binding_ == authoritative_binding_seal_ &&
                actual.value() == authoritative_binding_;
        }
        else if constexpr (requires { original_->observe_binding(); }) return false;
        else return true;
    }
    [[nodiscard]] bool rejection_binding() const
    {
        if constexpr (requires { original_->observe_binding(); })
        {
            const auto actual = anchor_.observe_binding();
            if (actual && authoritative_binding_ == authoritative_binding_seal_ &&
                actual.value() == authoritative_binding_) return true;
            return terminal_binding();
        }
        else return true;
    }
    [[nodiscard]] Status bind_owner_guard(void *context, Status (*check)(void *))
    {
        if (attempted_ || owner_guard_retired_ || owner_context_ || owner_check_ || !context || !check)
            return Status::already_exists("typed child owner attachment is one-shot");
        owner_context_ = owner_context_seal_ = context;
        owner_check_ = owner_check_seal_ = check;
        return {};
    }
    // The actual owner calls this on every terminal exit before its local
    // context dies. Retired callbacks cannot be rebound or dereferenced later.
    void retire_owner_guard()
    {
        recovery_preview_.retire_owner_guard();
        owner_context_ = owner_context_seal_ = nullptr;
        owner_check_ = owner_check_seal_ = nullptr;
        owner_guard_retired_ = true;
    }
    [[nodiscard]] Status retire_aborted_owner_session(Session &session)
    {
        if (!owner_guard_retired_ || owner_context_ || owner_context_seal_ || owner_check_ || owner_check_seal_ ||
            !recovery_preview_.owner_guard_retired_ || recovery_preview_.owner_context_ ||
            recovery_preview_.owner_context_seal_ || recovery_preview_.owner_check_ || recovery_preview_.owner_check_seal_)
            return Status::busy("allocated failed Session retirement requires retired Owner callbacks");
        if (recovery_preview_.anchor_.exact(&session))
            if (auto status = recovery_preview_.anchor_.retire_aborted_original(session); !status) return status;
        if (anchor_.exact(&session)) return anchor_.retire_aborted_original(session);
        return {};
    }
    [[nodiscard]] bool owner_retirement_idle() const
    {
        return status_ && owner_guard_retired_ && !owner_context_ && !owner_context_seal_ &&
            !owner_check_ && !owner_check_seal_ && !control_.pending && !sealed_.pending &&
            !control_.pending_writes && !sealed_.pending_writes && control_ == sealed_ &&
            (minted_ ? control_.phase == Phase::complete :
                (!attempted_ && !original_ && !original_seal_ && control_ == Control{})) &&
            recovery_preview_.owner_retirement_idle();
    }
    [[nodiscard]] bool continuing_owner() const
    {
        if (owner_context_ != owner_context_seal_ || owner_check_ != owner_check_seal_ ||
            bool(owner_context_) != bool(owner_check_)) return false;
        return !owner_check_ || bool(owner_check_(owner_context_));
    }
    [[nodiscard]] bool completed_owner_callbacks_retired() const
    {
        const auto &preview = recovery_preview_;
        return owner_guard_retired_ && !owner_context_ && !owner_context_seal_ && !owner_check_ && !owner_check_seal_ &&
            preview.owner_guard_retired_ && !preview.owner_context_ && !preview.owner_context_seal_ &&
            !preview.owner_check_ && !preview.owner_check_seal_;
    }
    [[nodiscard]] bool retained(bool validate_live_boundary = true) const
    {
        if (!minted_ || !original_ || original_ != original_seal_ || !anchor_.exact(original_) ||
            control_.phase == Phase::failed || control_.phase == Phase::unused || control_ != sealed_ ||
            authoritative_binding_ != authoritative_binding_seal_ ||
            owner_context_ != owner_context_seal_ || owner_check_ != owner_check_seal_ ||
            bool(owner_context_) != bool(owner_check_) ||
            !completed_ownership_birth_valid() ||
            (validate_live_boundary && (!continuing_binding() || !continuing_owner())) ||
            route_ != route_seal_ || !same_update(request_, request_seal_) || data_request_ != data_request_seal_ ||
            !reclaim_history::same_info(binding_, binding_seal_) ||
            !reclaim_history::same_session(initial_, initial_seal_) || !reclaim_history::same_session(progress_, progress_seal_) ||
            !same_geometry(geometry_, geometry_seal_) || !same_bounds(bounds_, bounds_seal_) ||
            (images_.ready && !same_images(images_, images_seal_)) || images_.ready != images_seal_.ready ||
            (control_.generated && !reclaim_history::same_info(generated_binding_, generated_binding_seal_)) ||
            (control_.final_view && !reclaim_history::same_info(final_binding_, final_binding_seal_))) return false;
        const auto capacity = Ext4AllocatedEmptyDataTransactionBudget::capacity(initial_, u32(binding_.metadata_count / 4), route_, binding_.journal_sequence);
        if (!capacity || capacity.value() != reserved_ || control_.reads > reserved_.reads || control_.writes > reserved_.writes ||
            control_.flushes > reserved_.flushes || control_.reads != control_.completed_reads ||
            control_.writes != control_.completed_writes || control_.flushes != control_.completed_flushes ||
            control_.journal_cursor > 8 || control_.publication_flushes > 3 || control_.checkpoint_flushes > 3 ||
            (control_.home_mask & ~u8{15}) || (control_.home_mask & images_.initial_home_mask) != images_.initial_home_mask)
            return false;
        constexpr std::array<u32, 8> order{1, 2, 3, 4, 5, 7, 6, 0};
        u8 journal_mask = route_ == Route::recovery && images_.ready ? 255 : 0;
        if (route_ == Route::fresh)
            for (u32 at = 0; at < control_.journal_cursor; ++at) journal_mask |= u8(1U << order[at]);
        else if (control_.journal_cursor || control_.publication_flushes) return false;
        if (control_.journal_mask != journal_mask ||
            control_.writes != (route_ == Route::fresh ? control_.journal_cursor : 0) +
                population(u8(control_.home_mask & ~images_.initial_home_mask)) + (control_.cleanup ? 1 : 0) + (control_.data_written ? 1 : 0) ||
            control_.flushes != control_.publication_flushes + control_.checkpoint_flushes + (control_.data_flushed ? 1 : 0) ||
            (control_.data_flushed && !control_.data_written) ||
            (route_ == Route::recovery && (control_.data_written || control_.data_flushed)) ||
            (route_ == Route::fresh && control_.journal_cursor && !control_.data_flushed) ||
            (control_.checkpoint_flushes && (control_.home_mask & 7) != 7) ||
            (control_.checkpoint_flushes >= 2 && control_.home_mask != 15) ||
            (control_.cleanup && control_.checkpoint_flushes < 2)) return false;
        auto expected = initial_;
        expected.reads += control_.reads; expected.completed_reads += control_.completed_reads;
        expected.writes += control_.writes; expected.completed_writes += control_.completed_writes;
        expected.flushes += control_.flushes; expected.completed_flushes += control_.completed_flushes;
        expected.pending_writes = control_.pending_writes;
        return reclaim_history::same_session(expected, progress_);
    }
    [[nodiscard]] bool completed_ownership_birth_valid(bool require_complete = false) const
    {
        using Validator = Ext4ZeroTruncateOwnershipValidator;
        const bool complete = require_complete || control_.phase == Phase::complete;
        if (!Validator::completed_birth_progress_exact(ownership_birth_, ownership_birth_progress_, this)) return false;
        if (Validator::completed_birth_uncaptured(ownership_birth_, ownership_birth_progress_, this)) return !complete;
        if (&reader_.owner != this || (complete && !Validator::completed_birth_complete(ownership_birth_))) return false;
        return Validator::completed_birth_exact(final_view_, reader_, final_binding_, final_proof_, ownership_birth_, this, complete);
    }
    [[nodiscard]] Status idle()
    {
        if (!retained() || control_.pending) return reject(Status::fault("allocated empty data private original idle seals changed"));
        const auto actual = original_->report(); if (!actual) return reject(actual.status());
        observed_ = actual.value();
        if (!same_geometry(original_->geometry(), geometry_) || !same_bounds(original_->bounds(), bounds_) ||
            !reclaim_history::same_session(actual.value(), progress_))
            return reject(Status::fault("allocated empty data private original idle Session changed"));
        return {};
    }
    [[nodiscard]] Status in_flight_exact(Session &session)
    {
        if (!retained() || !control_.pending || control_.operation == Operation::none ||
            !anchor_.exact(&session) || original_ != &session || !reclaim_history::same_session(flight_, flight_seal_) ||
            !reclaim_history::same_session(flight_, progress_))
            return reject(Status::fault("allocated empty data private dispatch seals changed"));
        auto expected = flight_; expected.in_flight = true;
        if (control_.operation == Operation::read) ++expected.reads;
        if (control_.operation == Operation::write) { ++expected.writes; expected.pending_writes = true; }
        if (control_.operation == Operation::flush) ++expected.flushes;
        const auto actual = session.report(); if (!actual) return reject(actual.status());
        observed_ = actual.value();
        if (!same_geometry(session.geometry(), geometry_) || !same_bounds(session.bounds(), bounds_) ||
            !reclaim_history::same_session(actual.value(), expected))
            return reject(Status::fault("allocated empty data private in-flight Session changed"));
        return {};
    }
    [[nodiscard]] Status finish_io(Status transport)
    {
        // Prove the opening backing before any post-failure report or abort.
        // A same-number foreign quarantine must never supply completion credit.
        if (!transport && !terminal_binding())
            return refuse(Status::fault("allocated empty data original terminal binding changed"));
        // Failed guarded transport has already quarantined the Session and
        // removed its live owner. Retain every fixed seal, account the exact
        // failed prefix and stop; no further live admission or I/O is attempted.
        if (!retained(bool(transport)) || !control_.pending || !reclaim_history::same_session(flight_, flight_seal_))
            return reject(Status::fault("allocated empty data private post-return seals changed"));
        const auto actual = original_->report(); if (!actual) return reject(actual.status());
        observed_ = actual.value(); auto expected = flight_;
        if (control_.operation == Operation::read) { ++expected.reads; if (transport) ++expected.completed_reads; }
        if (control_.operation == Operation::write)
        { ++expected.writes; expected.pending_writes = true; if (transport) ++expected.completed_writes; }
        if (control_.operation == Operation::flush)
        { ++expected.flushes; if (transport) { ++expected.completed_flushes; expected.pending_writes = false; } }
        if (!transport)
        {
            expected.state = block::DeviceState::quarantined; expected.first_error = actual.value().first_error;
            if (expected.first_error == StatusCode::ok) return reject(Status::fault("allocated empty data failed I/O lacks quarantine"));
        }
        if (!same_geometry(original_->geometry(), geometry_) || !same_bounds(original_->bounds(), bounds_) ||
            !reclaim_history::same_session(actual.value(), expected))
            return reject(Status::fault("allocated empty data exact attempted/completed I/O prefix changed"));
        if (!transport)
        {
            if (control_.operation == Operation::read) ++control_.reads;
            if (control_.operation == Operation::write) ++control_.writes;
            if (control_.operation == Operation::flush) ++control_.flushes;
            progress_ = progress_seal_ = actual.value(); control_.pending_writes = actual.value().pending_writes;
            control_.pending = false; control_.operation = Operation::none; control_.sector = 0; control_.selected = 0;
            return reject(transport);
        }
        if (control_.operation == Operation::read) { ++control_.reads; ++control_.completed_reads; }
        if (control_.operation == Operation::write)
        {
            ++control_.writes; ++control_.completed_writes;
            if (control_.phase == Phase::data) control_.data_written = true;
            else if (control_.phase == Phase::publishing)
            { control_.journal_mask |= u8(1U << control_.selected); ++control_.journal_cursor; }
            else if (control_.phase == Phase::cleanup) control_.cleanup = true;
            else control_.home_mask |= u8(1U << control_.selected);
        }
        if (control_.operation == Operation::flush)
        {
            ++control_.flushes; ++control_.completed_flushes;
            if (control_.phase == Phase::data) control_.data_flushed = true;
            else if (control_.phase == Phase::publishing) ++control_.publication_flushes;
            else ++control_.checkpoint_flushes;
        }
        progress_ = progress_seal_ = actual.value(); control_.pending_writes = actual.value().pending_writes;
        control_.pending = false; control_.operation = Operation::none; control_.sector = 0; control_.selected = 0; sync();
        return idle();
    }
    [[nodiscard]] Status begin_io(Operation kind, u32 selected, u32 physical = 0)
    {
        if (auto status = idle(); !status) return status;
        if ((kind == Operation::read && (control_.reads == reserved_.reads || control_.pending_writes)) ||
            (kind == Operation::write && control_.writes == reserved_.writes) ||
            (kind == Operation::flush && control_.flushes == reserved_.flushes) ||
            (kind != Operation::flush && physical >= binding_.layout.filesystem_blocks))
            return reject(Status::overflow("allocated empty data selected I/O bound or whole credit"));
        flight_ = flight_seal_ = progress_; control_.operation = kind; control_.selected = selected;
        control_.sector = kind == Operation::flush ? 0 : binding_.layout.first_sector + u64(physical) * 2;
        control_.pending = true; sync(); return {};
    }
    [[nodiscard]] Status read(u32 number, std::span<u8> bytes)
    {
        if (bytes.size() != 1024) return reject(Status::invalid_argument("allocated empty data selected READ size"));
        if (auto status = begin_io(Operation::read, number, number); !status) return status;
        return finish_io(original_->read_blocks_guarded(control_.sector, std::as_writable_bytes(bytes), dispatch_));
    }
    const Block &formatted(u32 record) const
    {
        if (!record) return images_.journal.head;
        if (record == 1) return images_.journal.descriptor;
        if (record < 6) return images_.journal.payloads[record - 2];
        return record == 6 ? images_.journal.commit : images_.journal.end;
    }
    [[nodiscard]] Status write_selected()
    {
        if (auto status = idle(); !status) return status;
        u32 selected{}, physical{}; const Block *image{};
        if (control_.phase == Phase::data && route_ == Route::fresh && !control_.data_written &&
            !control_.data_flushed && !control_.journal_cursor && !control_.publication_flushes)
        { physical = images_.plan.physical; image = &images_.data_image; }
        else if (control_.phase == Phase::publishing && route_ == Route::fresh && control_.data_flushed)
        {
            constexpr std::array<u32, 8> order{1, 2, 3, 4, 5, 7, 6, 0};
            if (control_.journal_cursor >= 8 || control_.publication_flushes !=
                (control_.journal_cursor < 6 ? 0U : control_.journal_cursor - 5))
                return reject(Status::denied("allocated empty WAL write is not the selected stage"));
            selected = order[control_.journal_cursor]; physical = u32(binding_.layout.journal_first_block) + selected;
            image = &formatted(selected);
        }
        else if (control_.phase == Phase::allocations && !control_.checkpoint_flushes)
        {
            selected = 0; while (selected < 3 && (control_.home_mask & (1U << selected))) ++selected;
            if (selected == 3) return reject(Status::denied("allocated empty allocation stage is complete"));
            physical = images_.plan.targets[selected]; image = &images_.plan.after[selected];
        }
        else if (control_.phase == Phase::inode && control_.checkpoint_flushes == 1 &&
                 (control_.home_mask & 7) == 7 && !(control_.home_mask & 8))
        { selected = 3; physical = images_.plan.targets[3]; image = &images_.plan.after[3]; }
        else if (control_.phase == Phase::cleanup && control_.checkpoint_flushes == 2 &&
                 control_.home_mask == 15 && !control_.cleanup)
        { physical = u32(binding_.layout.journal_first_block); image = &images_.journal.empty_head; }
        else return reject(Status::denied("allocated empty data WRITE lacks private selected stage"));
        if (!images_.ready || images_.plan.count != 4 || !images_.journal.ready)
            return reject(Status::fault("allocated empty data WRITE lacks complete retained images"));
        if (auto status = begin_io(Operation::write, selected, physical); !status) return status;
        return finish_io(original_->write_blocks_guarded(control_.sector, std::as_bytes(std::span{*image}), dispatch_));
    }
    [[nodiscard]] Status flush_selected()
    {
        if (auto status = idle(); !status) return status;
        const bool data = control_.phase == Phase::data && route_ == Route::fresh &&
            control_.data_written && !control_.data_flushed && !control_.journal_cursor;
        const bool publication = control_.phase == Phase::publishing && route_ == Route::fresh && control_.data_flushed &&
            ((control_.journal_cursor == 6 && !control_.publication_flushes) ||
             (control_.journal_cursor == 7 && control_.publication_flushes == 1) ||
             (control_.journal_cursor == 8 && control_.publication_flushes == 2));
        const bool checkpoint = (control_.phase == Phase::allocations && !control_.checkpoint_flushes && (control_.home_mask & 7) == 7) ||
            (control_.phase == Phase::inode && control_.checkpoint_flushes == 1 && control_.home_mask == 15) ||
            (control_.phase == Phase::cleanup && control_.checkpoint_flushes == 2 && control_.cleanup);
        if (!data && !publication && !checkpoint)
            return reject(Status::denied("allocated empty FLUSH lacks private selected barrier"));
        if (auto status = begin_io(Operation::flush, 0); !status) return status;
        return finish_io(original_->flush_guarded(dispatch_));
    }
    [[nodiscard]] Status physical()
    {
        if (auto status = idle(); !status) return status;
        if (!images_.ready || control_.pending_writes) return reject(Status::fault("allocated empty data physical readback lacks stable history"));
        const auto check = [&](u32 number, const Block &expected) -> Status {
            if (auto status = read(number, checking_); !status) return status;
            return checking_ == expected ? Status{} : reject(Status::io_error("allocated empty data exact physical prefix changed"));
        };
        for (u32 record = 0; record < 8; ++record)
        {
            const auto &expected = !record && control_.cleanup ? images_.journal.empty_head :
                control_.journal_mask & (1U << record) ? formatted(record) : images_.original_journal[record];
            if (auto status = check(u32(binding_.layout.journal_first_block) + record, expected); !status) return status;
        }
        if (auto status = check(images_.inode_bitmap_target, images_.inode_bitmap); !status) return status;
        for (u32 home = 0; home < 4; ++home)
            if (auto status = check(images_.plan.targets[home], control_.home_mask & (1U << home) ?
                    images_.plan.after[home] : images_.plan.before[home]); !status) return status;
        if (auto status = check(images_.plan.physical, control_.data_written ? images_.data_image : images_.original_data); !status)
            return status;
        return idle();
    }
    [[nodiscard]] Status prepare(Session &session, const Ext4RecoveryInfo &binding,
        Ext4AllocatedEmptyDataUpdate update, u32 offset, const Block &data)
    {
        if (attempted_) return Status::already_exists("allocated empty private transaction is one-shot");
        if (overlap(this, sizeof(*this), &session, sizeof(session)) || overlap(this, sizeof(*this), &binding, sizeof(binding)) ||
            overlap(this, sizeof(*this), &data, sizeof(data)))
            return Status::invalid_argument("allocated empty private input/image overlap");
        if (!update.inode || !update.size || update.size > 1024 || offset >= update.size ||
            update.seconds > 0x7fffffffU || update.nanoseconds >= 1000000000U)
            return Status::invalid_argument("allocated empty private update profile");
        for (usize at = 0; at < data.size(); ++at)
            if ((at < offset || at >= update.size) && data[at])
                return Status::invalid_argument("allocated empty private data image is not zero-padded");
        data_request_ = data_request_seal_ = data;
        if (auto status = mint(session, binding, Route::fresh, update); !status) return status;
        if (auto status = old_view_.open(reader_, binding_, opening_); !status) return reject(status);
        const Ext4ZeroTruncateUpdate empty{request_.inode, request_.generation, 0, 0, 0};
        if (auto status = Ext4ZeroTruncateOwnershipValidator::prove(binding_, old_view_, empty, ownership_, old_proof_); !status)
            return reject(status);
        if (old_proof_.count || old_proof_.old_size)
            return reject(Status::io_error("allocated empty original target still owns data"));
        u32 group{}, selected_physical{}; bool unsupported_space{};
        for (u32 candidate_group = 0; candidate_group < reserved_.groups; ++candidate_group)
        {
            const auto descriptor = std::span<const u8>{ownership_.descriptors}.subspan(candidate_group * 32, 32);
            if (!ext4_checked::le16(descriptor, 12)) continue;
            if (ext4_checked::le16(descriptor, 18) & 2) { unsupported_space = true; continue; }
            if (auto status = read(ext4_checked::le32(descriptor, 0), selection_bitmap_); !status) return status;
            const auto selected = Ext4FirstDataPlanner::select_block(binding_, ownership_.superblock,
                ownership_.descriptors, selection_bitmap_, candidate_group);
            if (!selected) return reject(selected.status());
            group = candidate_group; selected_physical = selected.value(); break;
        }
        if (!selected_physical)
            return reject(unsupported_space ? Status::unsupported("allocated empty uninitialized free space") :
                Status::no_space("allocated empty initialized free space exhausted"));
        const u32 per_group = ext4_checked::le32(ownership_.superblock, 40);
        if (!per_group) return reject(Status::fault("allocated empty original inode group size"));
        const u32 inode_group = (request_.inode - 1) / per_group, local = (request_.inode - 1) % per_group;
        if (inode_group >= reserved_.groups) return reject(Status::fault("allocated empty original inode group"));
        const u32 table = ext4_checked::le32(ownership_.descriptors, inode_group * 32 + 8) + local / 4;
        images_.inode_bitmap_target = ext4_checked::le32(ownership_.descriptors, inode_group * 32 + 4);
        const std::array<u32, 4> homes{ext4_checked::le32(ownership_.descriptors, group * 32), 2, 1, table};
        for (u32 record = 0; record < 8; ++record)
        {
            if (auto status = read(u32(binding_.layout.journal_first_block) + record, checking_); !status) return status;
            images_.original_journal[record] = checking_;
        }
        for (u32 home = 0; home < 4; ++home)
        {
            if (auto status = read(homes[home], checking_); !status) return status;
            images_.plan.before[home] = checking_;
        }
        if (auto status = read(images_.inode_bitmap_target, checking_); !status) return status;
        images_.inode_bitmap = checking_;
        if (auto status = read(selected_physical, checking_); !status) return status;
        images_.original_data = checking_; images_.data_image = data_request_;
        if (images_.plan.before[2] != ownership_.superblock || images_.plan.before[1] != ownership_.descriptors ||
            images_.plan.before[0] != selection_bitmap_ ||
            !std::equal(ownership_.inode.begin(), ownership_.inode.end(), images_.plan.before[3].begin() + (local % 4) * 256))
            return reject(Status::fault("allocated empty captured old identity differs from full proof"));
        const Ext4AllocatedEmptyDataSource source{group, selected_physical, table, images_.plan.before[2], images_.plan.before[1],
            images_.plan.before[0], images_.inode_bitmap, images_.plan.before[3]};
        if (auto status = Ext4AllocatedEmptyDataPlanner::plan_off_stack(binding_, request_, source, planning_, planned_); !status)
            return reject(status);
        if (planned_.targets != homes || planned_.physical != selected_physical || planned_.group != group)
            return reject(Status::fault("allocated empty canonical selection differs"));
        images_.plan = planned_;
        if (auto status = Ext4AllocatedEmptyDataJournalFormatter::format(binding_, images_.plan, images_.original_journal[0],
                images_.inode_bitmap, formatter_, images_.journal); !status) return reject(status);
        seal_images(); generated_binding_ = generated_binding_seal_ = binding_;
        generated_binding_.empty_log = generated_binding_seal_.empty_log = false;
        generated_binding_.journal_sequence = generated_binding_seal_.journal_sequence = images_.journal.active_sequence;
        control_.generated = true; sync();
        if (auto status = generated_preview_.prepare(generated_reader_, generated_binding_, generated_workspace_); !status)
            return reject(status);
        if (generated_workspace_.claimant_ != &generated_preview_ || generated_preview_.workspace_ != &generated_workspace_ ||
            !same_plan(generated_workspace_.journal_.decoded, images_.plan) || generated_workspace_.result_.after_mask ||
            generated_workspace_.data_ != images_.original_data ||
            generated_preview_.report_.phase != Ext4AllocatedEmptyDataPreviewPhase::ready)
            return reject(Status::fault("allocated empty generated full preview differs"));
        if (auto status = physical(); !status) return status;
        control_.phase = Phase::prepared; sync(); return idle();
    }
    [[nodiscard]] Status prepare_recovery(Session &session, const Ext4RecoveryInfo &binding)
    {
        if (auto status = mint(session, binding, Route::recovery); !status) return status;
        // The delegated reader retains the actual parent, whose fixed checks
        // include the outer canonical Owner/cache context on every dispatch.
        if (auto status = recovery_preview_.bind_owner_guard(this, recovery_owner_guard); !status)
        { recovery_preview_.retire_owner_guard(); return reject(status); }
        const auto preview_status = recovery_preview_.prepare(session, binding_);
        recovery_preview_.retire_owner_guard();
        if (!preview_status) return reject(preview_status);
        if (!retained()) return reject(Status::fault("allocated empty data recovery parent changed during readonly preview"));
        const auto &child = recovery_preview_; const auto &workspace = child.workspace_;
        const auto actual = original_->report(); if (!actual) return reject(actual.status());
        const block::SessionCounter reads = child.control_.attempts;
        if (reads > reserved_.reads)
            return reject(Status::fault("allocated empty data recovery preview exceeds whole reserved reads"));
        auto expected = progress_; expected.reads += reads; expected.completed_reads += reads;
        if (reads > reserved_.reads || child.control_.completions != reads || child.control_.phase != Ext4AllocatedEmptyDataSessionPreviewPhase::ready ||
            !child.anchor_.exact(original_) || child.original_ != original_ || child.original_seal_ != original_ ||
            !reclaim_history::same_info(child.binding_, binding_) || !reclaim_history::same_info(child.binding_seal_, binding_) ||
            !reclaim_history::same_session(child.initial_, progress_) || !reclaim_history::same_session(child.initial_seal_, progress_) ||
            !reclaim_history::same_session(child.progress_, expected) || !reclaim_history::same_session(child.progress_seal_, expected) ||
            !reclaim_history::same_session(actual.value(), expected) || !reclaim_history::same_session(child.observed_, expected) ||
            !child.same_control(child.control_, child.sealed_) || child.control_.pending || !child.status_ ||
            child.original_binding_ != authoritative_binding_ || child.original_binding_seal_ != authoritative_binding_ ||
            !child.owner_guard_retired_ || child.owner_context_ || child.owner_context_seal_ ||
            child.owner_check_ || child.owner_check_seal_ ||
            !same_geometry(child.geometry_, geometry_) || !same_geometry(child.geometry_seal_, geometry_) ||
            !same_bounds(child.bounds_, bounds_) || !same_bounds(child.bounds_seal_, bounds_) ||
            workspace.claimant_ != &child.preview_ ||
            child.preview_.workspace_ != &workspace || workspace.reader_ != &child.reader_ || !workspace.journal_ready_ ||
            child.preview_.report_.phase != Ext4AllocatedEmptyDataPreviewPhase::ready)
            return reject(Status::fault("allocated empty data recovery exact private preview handoff differs"));
        control_.reads = control_.completed_reads = reads; progress_ = progress_seal_ = observed_ = actual.value(); sync();
        images_.plan = workspace.journal_.decoded; images_.inode_bitmap = workspace.inode_bitmap_;
        images_.inode_bitmap_target = workspace.inode_bitmap_target_; images_.actual_homes = workspace.homes_;
        images_.initial_home_mask = control_.home_mask = workspace.result_.after_mask;
        images_.original_data = images_.data_image = workspace.data_;
        images_.original_journal[0] = workspace.head_; images_.original_journal[1] = workspace.descriptor_;
        for (u32 at = 0; at < 4; ++at) images_.original_journal[at + 2] = workspace.payloads_[at];
        images_.original_journal[6] = workspace.commit_; images_.original_journal[7] = workspace.end_;
        format_binding_ = binding_; format_binding_.empty_log = true; --format_binding_.journal_sequence;
        format_head_ = workspace.head_;
        be32(format_head_, 24, format_binding_.journal_sequence); be32(format_head_, 28, 0); stamp_head(format_head_);
        if (auto status = Ext4AllocatedEmptyDataJournalFormatter::format(format_binding_, images_.plan, format_head_,
                images_.inode_bitmap, formatter_, images_.journal); !status) return reject(status);
        for (u32 record = 0; record < 8; ++record)
            if (formatted(record) != images_.original_journal[record])
                return reject(Status::fault("allocated empty data recovery regenerated active WAL differs"));
        control_.journal_mask = 255; seal_images(); control_.phase = Phase::published; sync(); return idle();
    }
    [[nodiscard]] static Status recovery_owner_guard(void *raw)
    {
        auto &parent = *static_cast<Ext4AllocatedEmptyDataTransaction *>(raw);
        return parent.retained() ? Status{} :
            Status::fault("allocated empty data delegated original parent/Owner changed");
    }
    [[nodiscard]] Status publish()
    {
        if (control_.phase != Phase::prepared || route_ != Route::fresh)
            return Status::bad_descriptor("allocated empty data publication lacks private fresh preparation");
        if (auto status = physical(); !status) return status;
        control_.phase = Phase::data; sync();
        if (auto status = write_selected(); !status) return status;
        if (auto status = flush_selected(); !status) return status;
        if (auto status = physical(); !status) return status;
        control_.phase = Phase::publishing; sync();
        while (control_.journal_cursor < 6) if (auto status = write_selected(); !status) return status;
        if (auto status = flush_selected(); !status) return status;
        if (auto status = physical(); !status) return status;
        if (auto status = write_selected(); !status) return status;
        if (auto status = flush_selected(); !status) return status;
        if (auto status = physical(); !status) return status;
        if (auto status = write_selected(); !status) return status;
        if (auto status = flush_selected(); !status) return status;
        if (auto status = physical(); !status) return status;
        control_.phase = Phase::published; sync(); return idle();
    }
    [[nodiscard]] Status checkpoint()
    {
        if (control_.phase != Phase::published)
            return Status::bad_descriptor("allocated empty data checkpoint lacks private complete publication");
        if (auto status = physical(); !status) return status;
        control_.phase = Phase::allocations; sync();
        while ((control_.home_mask & 7) != 7) if (auto status = write_selected(); !status) return status;
        // The allocation barrier is required even when the recovered inode is already final.
        if (auto status = flush_selected(); !status) return status;
        if (auto status = physical(); !status) return status;
        control_.phase = Phase::inode; sync();
        if (!(control_.home_mask & 8)) if (auto status = write_selected(); !status) return status;
        if (auto status = flush_selected(); !status) return status;
        if (auto status = physical(); !status) return status;
        control_.phase = Phase::cleanup; sync();
        if (auto status = write_selected(); !status) return status;
        if (auto status = flush_selected(); !status) return status;
        if (auto status = physical(); !status) return status;
        final_binding_ = final_binding_seal_ = binding_;
        final_binding_.empty_log = final_binding_seal_.empty_log = true;
        final_binding_.journal_sequence = final_binding_seal_.journal_sequence = reserved_.final_sequence;
        control_.phase = Phase::final; control_.final_view = true; sync();
        if (auto status = final_view_.open(reader_, final_binding_, opening_); !status) return reject(status);
        const auto &plan = images_.plan;
        const Ext4ZeroTruncateUpdate filled{plan.update.inode, plan.update.generation, plan.update.size, 0, 0};
        if (auto status = Ext4ZeroTruncateOwnershipValidator::prove_completed(final_binding_, final_view_, reader_,
                filled, ownership_, final_proof_, ownership_birth_, ownership_birth_progress_, this); !status)
            return reject(status);
        const u64 version = u64{ext4_checked::le32(ownership_.inode, 36)} | (u64{ext4_checked::le32(ownership_.inode, 152)} << 32);
        if (final_proof_.count != 1 || final_proof_.mapping[0] != plan.physical || final_proof_.group != plan.group ||
            final_proof_.inode != plan.update.inode || final_proof_.generation != plan.update.generation ||
            final_proof_.old_size != plan.update.size || version != plan.new_version)
            return reject(Status::fault("allocated empty final physical ownership identity differs"));
        if (auto status = physical(); !status) return status;
        control_.phase = Phase::complete; sync(); return idle();
    }
    static void be32(Block &bytes, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - index * 8)); }
    static void stamp_head(Block &bytes)
    { be32(bytes, 252, 0); be32(bytes, 252, jbd2::detail::crc32c(~u32{0}, bytes)); }

    reclaim_history::SessionAnchor<Session> anchor_{};
    Session *original_{}, *original_seal_{};
    Route route_{}, route_seal_{};
    Ext4RecoveryInfo binding_{}, binding_seal_{}, generated_binding_{}, generated_binding_seal_{},
        final_binding_{}, final_binding_seal_{}, format_binding_{};
    Ext4AllocatedEmptyDataUpdate request_{}, request_seal_{};
    block::SessionReport initial_{}, initial_seal_{}, progress_{}, progress_seal_{}, flight_{}, flight_seal_{}, observed_{};
    bool owner_guard_retired_{};
    void *owner_context_{}, *owner_context_seal_{};
    Status (*owner_check_)(void *){}, (*owner_check_seal_)(void *){};
    block::SessionBindingObservation authoritative_binding_{}, authoritative_binding_seal_{};
    driver::BlockGeometry geometry_{}, geometry_seal_{};
    block::SessionBounds bounds_{}, bounds_seal_{};
    Ext4AllocatedEmptyDataTransactionCapacity reserved_{};
    Control control_{}, sealed_{};
    Images images_{}, images_seal_{};
    Status status_{};
    bool attempted_{}, minted_{};
    Block checking_{}, format_head_{}, selection_bitmap_{}, data_request_{}, data_request_seal_{};
    Reader reader_{*this}; GeneratedReader generated_reader_{*this}; Dispatch dispatch_{*this};
    Ext4ReadOnlyOpenScratch opening_{};
    Ext4ReadOnlyView<Reader> old_view_{}, final_view_{};
    Ext4ZeroTruncateOwnershipScratch ownership_{};
    Ext4ZeroTruncateOwnership old_proof_{}, final_proof_{};
    Ext4ZeroTruncateOwnershipValidator::CompletedBirthSeal ownership_birth_{};
    Ext4ZeroTruncateOwnershipValidator::CompletedBirthProgress ownership_birth_progress_{};
    Ext4AllocatedEmptyDataScratch planning_{}; Ext4AllocatedEmptyDataPlan planned_{};
    Ext4AllocatedEmptyDataJournalFormatScratch formatter_{};
    Ext4AllocatedEmptyDataPreviewWorkspace<GeneratedReader> generated_workspace_{};
    Ext4AllocatedEmptyDataPreview<GeneratedReader> generated_preview_{};
    Ext4AllocatedEmptyDataSessionPreview<Session> recovery_preview_{};
};
} // namespace ok::fs
