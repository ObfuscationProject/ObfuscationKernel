#pragma once

#include "ok/fs/ext4_readonly.hpp"
#include "ok/fs/ext4_rename_plan.hpp"
#include "ok/fs/ext4_create_plan.hpp"
#include "ok/fs/ext4_create_witness.hpp"
#include "ok/fs/ext4_directory_create_plan.hpp"
#include "ok/fs/ext4_directory_create_witness.hpp"
#include "ok/fs/ext4_first_data_plan.hpp"
#include "ok/fs/ext4_next_data_plan.hpp"

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
        usize inspected = 0;
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
                if (++inspected > 128) return Status::no_memory("ext4 directory replay inode scan budget");
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
        usize inspected{};
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
                if (++inspected > 128) return Status::no_memory("ext4 create replay inode scan budget");
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
} // namespace ok::fs
