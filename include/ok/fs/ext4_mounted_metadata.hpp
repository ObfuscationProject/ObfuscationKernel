#pragma once

#include "ok/fs/ext4_owned_transaction.hpp"
#include "ok/fs/ext4_inode_lifetime.hpp"
#include "ok/fs/mount_retention.hpp"
#include "ok/fs/mode_policy.hpp"
#include "ok/fs/ext4_mounted_typed_cursor.hpp"
#include "ok/arch/realtime.hpp"

#include <optional>
#include <utility>

namespace ok::fs
{
enum class Ext4MountedPolicy : u8
{ maintenance_only, maintained_noatime, ordered_data_noatime, ordered_data_dirty_noatime };

enum class Ext4MountPhase : u8 { unused, opening, readable, updating, draining, freezing, clean_sealed, failed, closed };
struct Ext4MountReport
{
    Ext4MountPhase phase{Ext4MountPhase::unused};
    Status status{};
    u64 epoch{};
    bool published{}, reading{};
};

// Kernel-owned exclusive off-stack storage. It is bound once to one mount
// lifetime; ordinary user programs never receive these internal addresses.
struct Ext4MountedReclaimWorkspace final
{
    template <usize> friend class Ext4MountedMetadata;
    Ext4MountedReclaimWorkspace() = default;
    Ext4MountedReclaimWorkspace(const Ext4MountedReclaimWorkspace &) = delete;
    Ext4MountedReclaimWorkspace &operator=(const Ext4MountedReclaimWorkspace &) = delete;
    Ext4MountedReclaimWorkspace(Ext4MountedReclaimWorkspace &&) = delete;
    Ext4MountedReclaimWorkspace &operator=(Ext4MountedReclaimWorkspace &&) = delete;
  private:
    Status claim(const void *mount)
    {
        smp::ScopedSpinLock guard(lock);
        if (!mount || claimant) return Status::busy("reclaim workspace already belongs to a mount lifetime");
        claimant = mount; return {};
    }
    Ext4OrphanChainCaptureStorage original{};
    Ext4OrphanDirectoryQualifiedReclaimScratch preparation{};
    Ext4OrphanReclaimCaptureStorage observed{};
    Ext4OrphanReclaimColdScratch verification{};
    const void *claimant{};
    smp::SpinLock lock{};
};

// A bounded mounted metadata and ordered-data writer. Transport,
// registry and provider outlive every token, Node retain and synchronous call.
// Detachment/drain are one-way; external storage destruction is serialized.
template <usize DeviceCapacity = 4>
class Ext4MountedMetadata final
{
    // Single-block mkdir needs eight home images plus the journal head. The
    // earlier regular-file transaction still admits its seven-buffer policy.
    using Owner = Ext4OwnedTransaction<DeviceCapacity, ext4_directory_create_owner_buffers>;
    using Lifetime = Ext4InodeLifetimeTable<128, 512>;
    using Lease = Lifetime::Lease;
    struct Reader
    {
        Ext4MountedMetadata *mount;
        Status read_block(u32 block, std::span<u8> output)
        {
            typename Owner::Pin pin;
            const auto pinned = mount->owner_.common_root_domain_active() ?
                mount->owner_.common_root_pin(block, pin) : mount->owner_.startup_domain_active() ?
                mount->owner_.startup_pin(block, pin) : mount->owner_.pin(block, pin);
            if (!pinned) return pinned;
            auto status = pin.read(0, std::as_writable_bytes(output));
            const auto released = pin.release();
            return status ? released : status;
        }
    };
    using View = Ext4ReadOnlyView<Reader>;
    // Off-stack observations copied only from the original Owner's actual
    // selected completed child. This private record grants no dispatch right.
    struct TypedOwnershipSnapshot final
    {
        struct Group { u32 block_bitmap{}, inode_bitmap{}, table{}; u16 block_checksum{}, inode_checksum{}, flags{};
            friend bool operator==(const Group &, const Group &) = default; };
        struct Range { u32 first{}, count{}; friend bool operator==(const Range &, const Range &) = default; };
        TypedOwnershipSnapshot() = default;
        TypedOwnershipSnapshot(const TypedOwnershipSnapshot &) = delete;
        TypedOwnershipSnapshot &operator=(const TypedOwnershipSnapshot &) = delete;
        TypedOwnershipSnapshot(TypedOwnershipSnapshot &&) = delete;
        TypedOwnershipSnapshot &operator=(TypedOwnershipSnapshot &&) = delete;
        const Ext4MountedMetadata *mount{}; const Owner *owner{};
        const typename Owner::TypedOwnerWorkspace *workspace{};
        const void *child{}, *view{}, *reader{}, *proof{};
        Ext4RecoveryInfo binding{}; block::SessionReport terminal{};
        std::array<u8, 16> uuid{};
        std::array<Group, Ext4RecoveryInfo::group_capacity> groups{};
        std::array<Range, 128 * 4> ranges{};
        usize range_count{};
        u32 seed{}, first_data{}, blocks_per_group{}, inodes_per_group{}, group_count{}, inodes{}, first_inode{}, table_blocks{};
        u32 retained_orphan{}, retained_orphan_generation{}, retained_orphan_next{};
        Ext4ZeroTruncateOwnership target{};
        bool attempted{}, ready{}, failed{}, allocated{}, noop{}, minted{};
    };
    template <typename ChildReader>
    [[nodiscard]] static bool typed_view_snapshot_exact(const Ext4ReadOnlyView<ChildReader> &view,
        const Ext4ZeroTruncateOwnership &proof, const TypedOwnershipSnapshot &snapshot)
    {
        const auto &layout = view.layout_; const auto &saved = snapshot.binding.layout;
        if (snapshot.view != &view || snapshot.reader != view.reader_ || snapshot.proof != &proof ||
            view.attempted_ != snapshot.attempted || view.ready_ != snapshot.ready || view.failed_ != snapshot.failed ||
            layout.first_sector != saved.first_sector || layout.filesystem_blocks != saved.filesystem_blocks ||
            layout.journal_first_block != saved.journal_first_block || layout.journal_blocks != saved.journal_blocks ||
            layout.block_size != saved.block_size || view.uuid_ != snapshot.uuid ||
            view.seed_ != snapshot.seed || view.first_data_ != snapshot.first_data ||
            view.blocks_per_group_ != snapshot.blocks_per_group || view.inodes_per_group_ != snapshot.inodes_per_group ||
            view.group_count_ != snapshot.group_count || view.inodes_ != snapshot.inodes ||
            view.first_inode_ != snapshot.first_inode || view.table_blocks_ != snapshot.table_blocks ||
            view.ownership_range_count_ != snapshot.range_count ||
            view.retained_orphan_ != snapshot.retained_orphan ||
            view.retained_orphan_generation_ != snapshot.retained_orphan_generation ||
            view.retained_orphan_next_ != snapshot.retained_orphan_next ||
            proof.inode != snapshot.target.inode || proof.generation != snapshot.target.generation ||
            proof.old_size != snapshot.target.old_size || proof.count != snapshot.target.count ||
            proof.group != snapshot.target.group || proof.mapping != snapshot.target.mapping) return false;
        for (usize at = 0; at < snapshot.groups.size(); ++at)
        {
            const auto &actual = view.groups_[at];
            if (snapshot.groups[at] != typename TypedOwnershipSnapshot::Group{actual.block_bitmap,
                actual.inode_bitmap, actual.table, actual.block_checksum, actual.inode_checksum, actual.flags}) return false;
        }
        for (usize at = 0; at < snapshot.ranges.size(); ++at)
            if (snapshot.ranges[at] != typename TypedOwnershipSnapshot::Range{
                view.ownership_ranges_[at].first, view.ownership_ranges_[at].count}) return false;
        return true;
    }
    [[nodiscard]] Status capture_typed_ownership_snapshot(typename Owner::TypedOwnerWorkspace &workspace,
                                                        TypedOwnershipSnapshot &snapshot)
    {
        const auto overlaps = [](const void *a, usize as, const void *b, usize bs) {
            const auto one = reinterpret_cast<usize>(a), two = reinterpret_cast<usize>(b);
            return one <= two ? two - one < as : one - two < bs;
        };
        if (overlaps(&snapshot, sizeof(snapshot), this, sizeof(*this)) ||
            overlaps(&snapshot, sizeof(snapshot), &workspace, sizeof(workspace)))
            return Status::invalid_argument("typed completed ownership snapshot aliases retained storage");
        if (snapshot.minted) return Status::already_exists("typed completed ownership snapshot is one-shot");
        auto capture = [&](const auto &view, const Ext4ZeroTruncateOwnership &proof, const Ext4RecoveryInfo &binding,
            const block::SessionReport &terminal, const void *child, bool allocated, bool noop) -> Status {
            if (!view.reader_ || !view.attempted_ || !view.ready_ || view.failed_ ||
                view.ownership_range_count_ > view.ownership_ranges_.size())
                return Status::fault("typed completed ownership actual view is unavailable");
            snapshot.mount = this; snapshot.owner = &owner_; snapshot.workspace = &workspace;
            snapshot.child = child; snapshot.view = &view; snapshot.reader = view.reader_; snapshot.proof = &proof;
            snapshot.binding = binding; snapshot.terminal = terminal; snapshot.uuid = view.uuid_;
            snapshot.seed = view.seed_; snapshot.first_data = view.first_data_;
            snapshot.blocks_per_group = view.blocks_per_group_; snapshot.inodes_per_group = view.inodes_per_group_;
            snapshot.group_count = view.group_count_; snapshot.inodes = view.inodes_;
            snapshot.first_inode = view.first_inode_; snapshot.table_blocks = view.table_blocks_;
            snapshot.retained_orphan = view.retained_orphan_;
            snapshot.retained_orphan_generation = view.retained_orphan_generation_;
            snapshot.retained_orphan_next = view.retained_orphan_next_;
            snapshot.attempted = view.attempted_; snapshot.ready = view.ready_; snapshot.failed = view.failed_;
            snapshot.range_count = view.ownership_range_count_; snapshot.target = proof;
            snapshot.allocated = allocated; snapshot.noop = noop;
            for (usize at = 0; at < snapshot.groups.size(); ++at)
            {
                const auto &group = view.groups_[at];
                snapshot.groups[at] = {group.block_bitmap, group.inode_bitmap, group.table,
                    group.block_checksum, group.inode_checksum, group.flags};
            }
            for (usize at = 0; at < snapshot.ranges.size(); ++at)
                snapshot.ranges[at] = {view.ownership_ranges_[at].first, view.ownership_ranges_[at].count};
            return typed_view_snapshot_exact(view, proof, snapshot) ? Status{} :
                Status::fault("typed completed ownership initial declared fields differ");
        };
        const auto status = owner_.typed_visit_completed_view(workspace, capture);
        if (status) snapshot.minted = true;
        return status;
    }
    [[nodiscard]] Status validate_typed_ownership_snapshot(typename Owner::TypedOwnerWorkspace &workspace,
                                                         const TypedOwnershipSnapshot &snapshot)
    {
        if (!snapshot.minted || snapshot.mount != this || snapshot.owner != &owner_ || snapshot.workspace != &workspace)
            return Status::fault("typed completed ownership snapshot original attachment differs");
        auto validate = [&](const auto &view, const Ext4ZeroTruncateOwnership &proof, const Ext4RecoveryInfo &binding,
            const block::SessionReport &terminal, const void *child, bool allocated, bool noop) -> Status {
            if (snapshot.child != child || snapshot.allocated != allocated || snapshot.noop != noop ||
                !reclaim_history::same_info(snapshot.binding, binding) ||
                !reclaim_history::same_session(snapshot.terminal, terminal) ||
                !typed_view_snapshot_exact(view, proof, snapshot))
                return Status::fault("typed completed ownership selected actual fields changed");
            return {};
        };
        return owner_.typed_visit_completed_view(workspace, validate);
    }
    class MountedNode final : public Node
    {
        friend class Ext4MountedMetadata;
      public:
        std::string_view name() const override
        {
            smp::ScopedSpinLock guard(mount_->lock_);
            return rename_count_ ? rename_names_[rename_count_ - 1].view() : name_.view();
        }
        // Legacy diagnostic accessor cannot convey an error. Qualified callers
        // must use query_metadata; fallback has no permissions or valid identity.
        Metadata metadata() const override
        {
            auto result = query_metadata();
            return result ? result.value() : Metadata{.mode = 0, .link_count = 0, .block_size = 0};
        }
        Result<Metadata> query_metadata() const override
        { return mount_->read_operation([&] { return mount_->metadata(inode_); }, false, &lifetime_key_); }
        const void *filesystem_identity() const override { return mount_; }
        Result<Node *> lookup_child(std::string_view name) override
        {
            return mount_->read_operation([&]() -> Result<Node *> {
                auto child = mount_->view_.lookup(inode_, name);
                if (!child) return child.status();
                auto found = mount_->node(child.value(), name);
                if (!found) return found.status();
                if (auto status = found.value()->expose_borrowed_identity(); !status) return status;
                return found.value();
            }, false, &lifetime_key_);
        }
        Result<Node *> lookup_child_retained(std::string_view name) override
        {
            Node *retained{};
            auto found = mount_->read_operation([&]() -> Result<Node *> {
                auto child = mount_->view_.lookup(inode_, name);
                if (!child) return child.status();
                auto node = mount_->node(child.value(), name);
                if (!node) return node.status();
                if (auto status = node.value()->retain(); !status) return status;
                retained = node.value();
                return retained;
            }, false, &lifetime_key_);
            if (!found && retained)
            {
                if (auto status = retained->release(); !status) return status;
            }
            return found;
        }
        Node *lookup(std::string_view name) override
        { auto result = lookup_child(name); return result ? result.value() : nullptr; }
        Result<FileBuffer> read(usize offset, usize count) const override
        {
            return mount_->read_operation([&]() -> Result<FileBuffer> {
                FileBuffer bytes{};
                auto result = mount_->view_.read(inode_, offset,
                    {reinterpret_cast<u8 *>(bytes.data.data()), count < bytes.data.size() ? count : bytes.data.size()});
                if (!result) return result.status();
                bytes.size = result.value(); return bytes;
            }, false, &lifetime_key_);
        }
        Result<DirectoryListing> list(DirectoryReadIntent = DirectoryReadIntent::access) const override
        {
            Result<DirectoryListing> result{std::in_place};
            auto &listing = result.value();
            const auto status = mount_->read_operation([&]() -> Status {
                return mount_->view_.directory(inode_, [&](const Ext4ReadOnlyDirent &entry) -> Status {
                    if (entry.name == "." || entry.name == "..") return {};
                    if (listing.count == listing.entries.size()) return Status::overflow("mounted directory capacity");
                    auto metadata = mount_->metadata(entry.inode);
                    if (!metadata) return metadata.status();
                    auto &item = listing.entries[listing.count];
                    if (auto copied = item.name.assign(entry.name); !copied) return copied;
                    item.metadata = metadata.value(); ++listing.count; return {};
                });
            }, false, &lifetime_key_);
            if (!status) return status;
            return result;
        }
        Result<FixedString<96>> directory_path(const Node &root) const override
        {
            if (root.filesystem_identity() != mount_) return Status::not_found("foreign mounted directory root");
            // Verify identity by address, without trusting an unrelated Node cast.
            u32 root_inode{};
            {
                smp::ScopedSpinLock guard(mount_->lock_);
                for (const auto &node : mount_->nodes_) if (&node == &root) root_inode = node.inode_;
            }
            if (!root_inode) return Status::not_found("unknown mounted root");
            return mount_->read_operation([&]() -> Result<FixedString<96>> {
                u32 current = inode_; FixedString<96> suffix;
                for (usize depth = 0; depth < 48; ++depth)
                {
                    auto metadata = mount_->metadata(current);
                    if (!metadata) return metadata.status();
                    if (metadata.value().type != NodeType::directory) return Status::not_directory("mounted path node");
                    if (current == root_inode) return suffix;
                    if (current == 2) return Status::not_found("mounted directory outside root");
                    auto parent = mount_->view_.lookup(current, "..");
                    if (!parent) return parent.status();
                    if (parent.value() == current) return Status::io_error("mounted parent cycle");
                    FixedString<max_path_segment> name; bool found{};
                    auto status = mount_->view_.directory(parent.value(), [&](const Ext4ReadOnlyDirent &entry) -> Status {
                        if (entry.inode != current || entry.name == "." || entry.name == "..") return {};
                        if (found) return Status::io_error("ambiguous mounted directory parent");
                        found = true; return name.assign(entry.name);
                    });
                    if (!status) return status;
                    if (!found) return Status::io_error("mounted parent lacks child");
                    FixedString<96> path;
                    if (!path.append('/') || !path.append(name.view()) || !path.append(suffix.view()))
                        return Status::overflow("mounted directory path capacity");
                    suffix = path; current = parent.value();
                }
                return Status::overflow("mounted directory ancestry capacity");
            }, false, &lifetime_key_);
        }
        Status retain() override
        {
            smp::ScopedSpinLock guard(mount_->lock_);
            if (auto status = mount_->admission(); !status) return status;
            return retain_locked();
        }
        Status expose_borrowed_identity() override
        {
            smp::ScopedSpinLock guard(mount_->lock_);
            if (auto status = mount_->admission(); !status) return status;
            if (!inode_) return Status::not_found("mounted borrowed identity is unavailable");
            borrowed_identity_exposed_ = true; mount_->legacy_exposure_ = true;
            return {};
        }
        Status release() override
        {
            bool consumed{};
            Status integrity{};
            {
                smp::ScopedSpinLock guard(mount_->lock_);
                integrity = mount_->retention_integrity_locked();
                if (!references_)
                {
                    for (const auto &held : mount_->reference_holds_)
                        if (held.node == this)
                            return mount_->fail_locked(Status::fault("mounted zero count has a Node lease"));
                    const auto state = mount_->lifetime_.snapshot(lifetime_key_);
                    if (state && state.value().holds[static_cast<usize>(Ext4InodeHold::descriptor)])
                        return mount_->fail_locked(Status::fault("mounted zero count has an inode lease"));
                    return Status::bad_descriptor("mounted node has no reference");
                }
                for (auto &held : mount_->reference_holds_)
                    if (held.node == this)
                    {
                        if (!held.lease || !held.lease->valid())
                            return mount_->fail_locked(Status::fault("mounted release lease is absent"));
                        if (auto status = mount_->lifetime_.validate_lease(*held.lease, lifetime_key_,
                                                                         Ext4InodeHold::descriptor); !status)
                            return mount_->fail_locked(status);
                        const auto status = mount_->lifetime_.release(*held.lease);
                        if (!status) return mount_->fail_locked(status);
                        held.lease.reset(); held.node = nullptr; --references_; consumed = true; break;
                    }
            }
            if (!consumed) return mount_->fail(Status::fault("mounted inode reference count differs from leases"));
            if (!integrity) return integrity;
            // I/O may happen only after dropping the mount/lifetime locks.
            return mount_->drain_pending_orphan();
        }
        Status sync() override { return mount_->sync(); }
        Status sync_persistent() override { return mount_->sync_persistent(*this); }
        Status write(usize offset, std::span<const std::byte> data) override
        { return mount_->write_regular(*this, offset, data); }
        Status truncate(usize size) override { return mount_->truncate_regular(*this, size); }
        Status create(std::string_view, NodeType) override { return mount_->unsupported(); }
        Result<Node *> create_child(std::string_view name, const InitialNodeAttributes &attributes) override
        { return mount_->create_node(*this, name, attributes, false); }
        Result<Node *> create_child_retained(std::string_view name, const InitialNodeAttributes &attributes) override
        { return mount_->create_node(*this, name, attributes, true); }
        Status chmod(u32) override { return mount_->unsupported(); }
        ModeChangeResult change_mode(u32 mode, user::Credentials caller) override
        { return mount_->change_metadata<false>(*this, mode, 0, caller); }
        ModeChangeResult change_owner(u32 uid, u32 gid, user::Credentials caller) override
        { return mount_->change_owner(*this, uid, gid, caller); }
        Status chown(u32, u32) override { return mount_->unsupported(); }
        Status remove(std::string_view name, bool directory) override
        { return mount_->namespace_regular(*this, name, {}, directory, false); }
        Status link(std::string_view, Node &) override { return mount_->unsupported(); }
        Status rename(std::string_view old_name, Node &destination, std::string_view new_name) override
        { return mount_->rename_dispatch(*this, old_name, destination, new_name); }
        Status create_symlink(std::string_view, std::string_view, u32, u32) override { return mount_->unsupported(); }
      private:
        // Mount lock held; lifetime acquisition performs no I/O.
        Status retain_locked()
        {
            if (references_ == ~usize{0}) return Status::overflow("mounted node references");
            for (auto &held : mount_->reference_holds_)
                if (!held.node)
                {
                    auto lease = mount_->retain_existing(lifetime_key_, Ext4InodeHold::descriptor);
                    if (!lease) return lease.status();
                    held.lease.emplace(std::move(lease).value());
                    held.node = this;
                    ++references_; return {};
                }
            return Status::no_memory("mounted inode reference capacity");
        }
        Ext4MountedMetadata *mount_{};
        u32 inode_{};
        usize references_{};
        bool borrowed_identity_exposed_{};
        Ext4InodeLifetimeKey lifetime_key_{};
        FixedString<max_path_segment> name_{};
        // Previously returned views remain immutable until identity retirement.
        // The caller must retain the identity for its borrowed string lifetime.
        std::array<FixedString<max_path_segment>, 15> rename_names_{};
        usize rename_count_{};
    };

    struct ReferenceHold
    {
        MountedNode *node{};
        std::optional<Lease> lease{};
    };
    // Synchronous stack-owned original authority. Mutable mount fields are
    // checked against this object, and cannot erase the operation's existence.
    struct NodeMutationContext final
    {
        NodeMutationContext(MountedNode &value, Ext4InodeLifetimeKey identity, Lease &held, u64 admitted)
            : node(value), key(identity), lease(held), epoch(admitted) {}
        NodeMutationContext(const NodeMutationContext &) = delete;
        NodeMutationContext &operator=(const NodeMutationContext &) = delete;
        NodeMutationContext(NodeMutationContext &&) = delete;
        NodeMutationContext &operator=(NodeMutationContext &&) = delete;
        MountedNode &node;
        const Ext4InodeLifetimeKey key;
        Lease &lease;
        const u64 epoch;
    };

#include "ok/fs/ext4_mounted_original_node_authority.inc"
#include "ok/fs/ext4_mounted_typed_suffix.inc"
#include "ok/fs/ext4_mounted_typed_publication.inc"
#include "ok/fs/ext4_mounted_typed_request.inc"
#include "ok/fs/ext4_mounted_typed_cold_startup.inc"
#include "ok/fs/ext4_mounted_common_opening.inc"

  public:
    using Registry = typename Owner::Registry;
    // Caller-owned permanent storage; allocate outside synchronous call stacks.
    // Registry/provider and this storage outlive Mount and all retained Nodes.
    class CommonStorage final
    {
        friend class Ext4MountedMetadata;
      public:
        CommonStorage() = default;
        ~CommonStorage() = default;
        CommonStorage(const CommonStorage &) = delete;
        CommonStorage &operator=(const CommonStorage &) = delete;
        CommonStorage(CommonStorage &&) = delete;
        CommonStorage &operator=(CommonStorage &&) = delete;
      private:
        Ext4MountedStartupWorkspace startup_{};
        Ext4MountedReclaimWorkspace reclaim_{};
        typename Owner::TypedOwnerWorkspace child_{};
        std::optional<TypedColdWorkspace> operation_{};
        std::optional<TypedColdPublication> publication_{};
        std::optional<TypedMutationStorage> requests_{};
        const Ext4MountedMetadata *claimant_{};
        smp::SpinLock lock_{};
    };
    explicit Ext4MountedMetadata(CommonStorage &storage)
        : Ext4MountedMetadata(Ext4MountedPolicy::ordered_data_dirty_noatime,
                              &storage.reclaim_, &storage.startup_)
    { common_storage_ = &storage; }
    explicit Ext4MountedMetadata(Ext4MountedPolicy policy = Ext4MountedPolicy::maintenance_only,
                                 Ext4MountedReclaimWorkspace *workspace = nullptr,
                                 Ext4MountedStartupWorkspace *startup = nullptr)
        : policy_(policy), reclaim_workspace_(workspace), startup_workspace_(startup), owner_(policy == Ext4MountedPolicy::ordered_data_dirty_noatime),
          inspection_(policy == Ext4MountedPolicy::ordered_data_dirty_noatime,
                      policy == Ext4MountedPolicy::ordered_data_dirty_noatime) {}
    [[nodiscard]] Ext4MountedPolicy policy() const { return policy_; }
    Ext4MountedMetadata(const Ext4MountedMetadata &) = delete;
    Ext4MountedMetadata &operator=(const Ext4MountedMetadata &) = delete;
    Ext4MountedMetadata(Ext4MountedMetadata &&) = delete;
    Ext4MountedMetadata &operator=(Ext4MountedMetadata &&) = delete;
    [[nodiscard]] Status mount(Registry &registry, typename Registry::Registration key, block::SessionBounds bounds)
    {
        if (common_storage_) return mount_common_storage(registry, key, bounds);
        {
            smp::ScopedSpinLock guard(lock_);
            if (report_.phase != Ext4MountPhase::unused) return Status::already_exists("metadata mount is one-shot");
            if (policy_ != Ext4MountedPolicy::maintenance_only && policy_ != Ext4MountedPolicy::maintained_noatime &&
                policy_ != Ext4MountedPolicy::ordered_data_noatime &&
                policy_ != Ext4MountedPolicy::ordered_data_dirty_noatime)
                return Status::invalid_argument("unknown mounted metadata policy");
            report_.phase = Ext4MountPhase::opening;
        }
        if (startup_workspace_)
        {
            const auto overlaps = [](const void *a, usize na, const void *b, usize nb) {
                const auto x = reinterpret_cast<usize>(a), y = reinterpret_cast<usize>(b);
                return x <= y ? y - x < na : x - y < nb;
            };
            Status status;
            if (policy_ != Ext4MountedPolicy::ordered_data_dirty_noatime ||
                overlaps(startup_workspace_, sizeof(*startup_workspace_), this, sizeof(*this)) ||
                (reclaim_workspace_ && overlaps(startup_workspace_, sizeof(*startup_workspace_),
                                               reclaim_workspace_, sizeof(*reclaim_workspace_))))
                status = Status::invalid_argument("startup workspace policy or overlap");
            else status = startup_workspace_->claim(this, &owner_);
            if (!status)
            { smp::ScopedSpinLock guard(lock_); report_.phase = Ext4MountPhase::closed; report_.status = status; return status; }
        }
        if (reclaim_workspace_)
            if (auto status = reclaim_workspace_->claim(this); !status)
            { smp::ScopedSpinLock guard(lock_); report_.phase = Ext4MountPhase::closed; report_.status = status; return status; }
        if (auto status = owner_.open(registry, key, bounds); !status) return fail(status);
        identity_ = owner_.buffers();
        if (startup_workspace_)
            if (auto status = owner_.recover_startup(*startup_workspace_, this, 1, startup_reader_, startup_ordinary_reader_); !status) return fail(status);
        return mount_after_startup();
    }
    [[nodiscard]] Result<MountedRootReference> retain_root()
    {
        smp::ScopedSpinLock guard(lock_);
        if (auto status = admission(); !status) return status;
        if (auto status = identity(); !status) return fail_locked(status);
        auto reference = root_retention_.issue();
        if (!reference && reference.status().code() == StatusCode::fault)
            return fail_locked(reference.status());
        if (reference) report_.published = true;
        return reference;
    }
    [[nodiscard]] Node *root()
    {
        Status authority{};
        {
            smp::ScopedSpinLock guard(lock_);
            if (!admission()) return nullptr;
            // Publication is the final authority boundary. Keep the mount
            // gate held while checking the original Session so a completed
            // external abort cannot publish a stale root.
            authority = identity();
            if (authority)
            { report_.published = true; legacy_exposure_ = true; nodes_[0].borrowed_identity_exposed_ = true; return &nodes_[0]; }
            report_.phase = Ext4MountPhase::failed;
            report_.status = authority;
            root_retention_.stop_admission();
        }
        // Quarantine after releasing the mount lock; downstream completion
        // and cleanup paths may also need that lock.
        static_cast<void>(owner_.fail(authority));
        return nullptr;
    }
    [[nodiscard]] Ext4MountReport report() const { smp::ScopedSpinLock guard(lock_); return report_; }
    [[nodiscard]] MetadataOwnerReport buffers() const { return owner_.buffers(); }
    [[nodiscard]] Ext4StartupRecoveryReport startup_report() const { return owner_.startup_report(); }
    // Kernel diagnostics for the checked mounted identity; no device I/O.
    [[nodiscard]] Result<Ext4InodeLifetimeSnapshot> lifetime_snapshot(u32 inode) const
    {
        smp::ScopedSpinLock guard(lock_);
        for (const auto &node : nodes_)
            if (node.inode_ == inode) return lifetime_.snapshot(node.lifetime_key_);
        return Status::not_found("mounted inode has no runtime record");
    }
    // Trusted caller-supplied metadata is confined to the unqualified policy.
    [[nodiscard]] Status update_metadata(Ext4InodeMetadataUpdate update)
    {
        auto epoch = begin_update(Ext4MountedPolicy::maintenance_only);
        return epoch ? commit_update(update, epoch.value(), mode_regular) : epoch.status();
    }
    [[nodiscard]] Status sync()
    {
        return read_operation([&] { return owner_.bound_device().flush(); }, true);
    }
    [[nodiscard]] Status sync_persistent(const MountedNode &node)
    {
        if (policy_ != Ext4MountedPolicy::ordered_data_dirty_noatime)
            return Status::unsupported("persistent sync requires ordered-data journalled mounted storage");
        // Authenticate the original Node and inode generation while retaining
        // its I/O lease. The provider's serialized mutations already complete
        // their journal protocol; success here also requires device FLUSH.
        return read_operation([&] { return owner_.bound_device().flush(); }, true, nullptr, &node, true);
    }
    [[nodiscard]] Status close()
    {
        if (auto status = begin_drain(); !status) return status;
        {
            smp::ScopedSpinLock guard(lock_);
            if (auto status = drained_locked(); !status) return status;
            report_.phase = Ext4MountPhase::updating;
        }
        auto status = owner_.close(); if (!status) return fail(status);
        smp::ScopedSpinLock guard(lock_); report_.phase = Ext4MountPhase::closed; return {};
    }
    [[nodiscard]] Status begin_drain()
    {
        smp::ScopedSpinLock guard(lock_);
        if (auto status = retention_integrity_locked(); !status) return status;
        if (auto status = mutation_admission_locked(); !status) return status;
        if (report_.phase == Ext4MountPhase::draining) return {};
        if (auto status = admission(); !status) return status;
        if (legacy_exposure_) return Status::busy("mounted provider has permanent borrowed exposure");
        if (report_.reading) return Status::busy("mounted reader in flight");
        if (auto status = orphan_work_drained_locked(); !status) return status;
        if (auto status = root_retention_.empty(); !status) return status;
        const auto buffers = owner_.buffers();
        if (buffers.read_pins || buffers.in_flight || buffers.transaction_active)
            return Status::busy("mounted buffers have retained work");
        if (auto status = identity(); !status) return fail_locked(status);
        if (auto status = root_retention_.begin_drain(); !status) return status;
        report_.phase = Ext4MountPhase::draining; return {};
    }
    // One-way shutdown after the VFS has drained every retained node. A normal
    // file sync deliberately leaves needs_recovery set while mounted.
    [[nodiscard]] Status freeze_clean()
    {
        if (policy_ != Ext4MountedPolicy::ordered_data_dirty_noatime)
            return Status::unsupported("clean freeze requires mounted-dirty policy");
        if (auto status = begin_drain(); !status) return status;
        {
            smp::ScopedSpinLock guard(lock_);
            if (auto status = drained_locked(); !status) return status;
            report_.phase = Ext4MountPhase::freezing;
        }
        if (auto status = identity(); !status) return fail(status);
        inspection_.~Ext4RecoveryCompletion<8, 2>();
        ::new (static_cast<void *>(&inspection_)) Ext4RecoveryCompletion<8, 2>(true, true);
        if (auto status = inspection_.prepare(owner_.bound_device(), owner_.bounds_); !status) return fail(status);
        if (!inspection_.binding()->needs_recovery || !inspection_.report().empty_log)
            return fail(Status::fault("clean freeze requires dirty empty journal"));
        auto &device = owner_.bound_device();
        if (auto status = device.flush(); !status) return fail(status);
        if (auto status = device.write_blocks(u64{binding_.layout.journal_first_block} * 2,
                std::as_bytes(std::span{inspection_.journal_})); !status) return fail(status);
        if (auto status = device.flush(); !status) return fail(status);
        auto super = inspection_.filesystem_;
        Ext4RecoveryCompletion<8, 2>::le32(super, 96, ext4_checked::le32(super, 96) & ~u32{4});
        Ext4RecoveryCompletion<8, 2>::le32(super, 1020,
            jbd2::detail::crc32c(~u32{0}, std::span<const u8>{super}.first(1020)));
        if (auto status = device.write_blocks(2, std::as_bytes(std::span{super})); !status) return fail(status);
        if (auto status = device.flush(); !status) return fail(status);
        reset(view_);
        if (auto status = owner_.close(); !status) return fail(status);
        smp::ScopedSpinLock guard(lock_);
        report_.phase = Ext4MountPhase::clean_sealed;
        return {};
    }

  private:
    [[nodiscard]] Status mount_common_storage(Registry &registry, typename Registry::Registration key,
                                               block::SessionBounds bounds)
    {
        auto &storage = *common_storage_;
        const auto one = reinterpret_cast<usize>(&storage), two = reinterpret_cast<usize>(this);
        if ((one <= two ? two - one < sizeof(storage) : one - two < sizeof(*this)) ||
            startup_workspace_ != &storage.startup_ || reclaim_workspace_ != &storage.reclaim_)
            return Status::invalid_argument("common storage overlaps Mount or differs from its original domains");
        {
            smp::ScopedSpinLock guard(storage.lock_);
            if (storage.claimant_ || storage.operation_ || storage.publication_ || storage.requests_)
                return Status::busy("common storage already belongs to a Mount lifetime");
            storage.claimant_ = this;
        }
        // Permanent binding precedes construction and transport. This lock is
        // never held across Owner/cache/Mount locks or an operation callback.
        storage.requests_.emplace(*this);
        return common_mount_scope(registry, key, bounds, storage.child_,
            [&](const TypedColdOpening &original) {
                storage.operation_.emplace(*this, original, storage.child_);
                storage.publication_.emplace(*storage.operation_);
                const auto status = typed_run_common_operation(*storage.operation_, *storage.publication_);
                storage.publication_.reset();
                storage.operation_.reset();
                return status;
            });
    }
    [[nodiscard]] Status mount_after_startup()
    {
        if (policy_ == Ext4MountedPolicy::ordered_data_dirty_noatime &&
            (!startup_workspace_ || owner_.startup_ordinary_selected()))
            if (auto status = recover_and_mark_dirty(); !status) return fail(status);
        auto status = inspect(false);
        if (status) status = view_.open(reader_, binding_);
        if (status && policy_ != Ext4MountedPolicy::maintenance_only) status = view_.validate_stored_metadata();
        if (!status)
        {
            // The retained common opening owns its original failure/quarantine
            // path through scope termination, including decoder refusals.
            if (owner_.common_opening_) return fail(status);
            if (status.code() != StatusCode::unsupported && status.code() != StatusCode::invalid_argument &&
                status.code() != StatusCode::no_memory) return fail(status);
            if (auto closed = owner_.close(); !closed) return fail(closed);
            smp::ScopedSpinLock guard(lock_); report_.phase = Ext4MountPhase::closed; report_.status = status; return status;
        }
        if (startup_workspace_)
            if (auto admitted = owner_.startup_admit(); !admitted) return fail(admitted);
        if (auto root = node(2, "/"); !root) return fail(root.status());
        if (auto status = root_retention_.bind(&nodes_[0], this, identity_.session_serial,
                                              identity_.registration_generation); !status) return fail(status);
        smp::ScopedSpinLock guard(lock_); report_.epoch = 1; report_.phase = Ext4MountPhase::readable; return {};
    }
    friend Owner;
    [[nodiscard]] Status startup_opening(const Owner *owner, u64 epoch) const
    {
        smp::ScopedSpinLock guard(lock_);
        if (owner != &owner_ || epoch != 1 || report_.phase != Ext4MountPhase::opening ||
            report_.epoch || report_.published || report_.reading)
            return Status::fault("startup mount opening publication domain differs");
        for (const auto &node : nodes_)
            if (node.inode_) return Status::fault("startup runtime Node was populated before admission");
        return {};
    }
    [[nodiscard]] Status reserve_name_publication(u32 inode) const
    {
        smp::ScopedSpinLock guard(lock_);
        for (const auto &node : nodes_)
            if (node.inode_ == inode && node.rename_count_ == node.rename_names_.size())
                return Status::no_memory("mounted immutable name history capacity");
        return {};
    }
    // Mount lock held; every previous slot remains byte-exact for retained or
    // raw callers. Capacity is checked before the namespace writer issues I/O.
    [[nodiscard]] Status publish_name(u32 inode, const FixedString<max_path_segment> &name)
    {
        for (auto &node : nodes_)
            if (node.inode_ == inode)
            {
                if (node.rename_count_ == node.rename_names_.size())
                    return Status::fault("mounted reserved name publication capacity changed");
                node.rename_names_[node.rename_count_] = name; ++node.rename_count_;
            }
        return {};
    }
    // Mount lock held. A detached identity may acquire another hold only from
    // a real retained descriptor; public keys and diagnostic snapshots confer
    // no authority to resurrect a final inode.
    [[nodiscard]] Result<Lease> retain_existing(Ext4InodeLifetimeKey key, Ext4InodeHold kind)
    {
        if (key.mount != this || key.session != identity_.session_serial)
            return Status::bad_descriptor("mounted hold identity is foreign");
        auto fresh = lifetime_.retain(key, kind);
        if (fresh || fresh.status().code() != StatusCode::busy) return fresh;
        for (const auto &held : reference_holds_)
            if (held.node && held.lease && held.node->lifetime_key_ == key)
                return lifetime_.clone(*held.lease, kind);
        return fresh.status();
    }

    [[nodiscard]] Status namespace_regular(MountedNode &parent, std::string_view old_name,
                                           std::string_view new_name, bool directory, bool replace)
    {
        if (!reclaim_workspace_) return unsupported();
        auto admitted = begin_update(Ext4MountedPolicy::ordered_data_dirty_noatime);
        if (!admitted) return admitted.status();
        const u64 epoch = admitted.value();
        const auto reject = [&](Status status) { return reject_unprepared(status, epoch); };
        if (directory) return reject(Status::unsupported("mounted namespace removal requires regular file"));
        if (parent.mount_ != this) return reject(Status::bad_descriptor("mounted namespace parent is foreign"));
        const auto valid_name = [](std::string_view name) {
            return !name.empty() && name != "." && name != ".." &&
                   name.find('/') == std::string_view::npos && name.find('\0') == std::string_view::npos;
        };
        if (!valid_name(old_name) || (replace && !valid_name(new_name)))
            return reject(Status::invalid_argument("invalid mounted namespace name"));
        auto checked_parent = view_.inode(parent.inode_);
        if (!checked_parent) return reject(checked_parent.status());
        if (parent.lifetime_key_ != Ext4InodeLifetimeKey{this, identity_.session_serial,
                                                       parent.inode_, checked_parent.value().generation})
            return fail(Status::fault("mounted namespace parent generation changed"));
        auto source = view_.lookup(parent.inode_, old_name);
        if (!source) return reject(source.status());
        auto source_inode = view_.inode(source.value());
        if (!source_inode) return reject(source_inode.status());
        auto source_metadata = metadata(source.value());
        if (!source_metadata) return reject(source_metadata.status());
        if (source_metadata.value().type != NodeType::regular || source.value() < view_.first_ordinary_inode())
            return reject(Status::unsupported("mounted namespace source requires ordinary regular inode"));
        u32 removed = source.value(), removed_generation = source_inode.value().generation;
        if (replace)
        {
            auto target = view_.lookup(parent.inode_, new_name);
            if (!target) return reject(target.status());
            if (target.value() == source.value())
            {
                auto directory_metadata = metadata(parent.inode_);
                if (!directory_metadata) return reject(directory_metadata.status());
                if (directory_metadata.value().type != NodeType::directory ||
                    !directory_metadata.value().link_count || (directory_metadata.value().mode & 0300) != 0300)
                    return reject(Status::unsupported("mounted rename parent lacks owner write/search bits"));
                if (old_name != new_name && source_metadata.value().link_count < 2)
                    return reject(Status::io_error("mounted same-inode rename links disagree"));
                return reject_unprepared({}, epoch);
            }
            auto target_inode = view_.inode(target.value());
            if (!target_inode) return reject(target_inode.status());
            removed = target.value(); removed_generation = target_inode.value().generation;
        }
        auto victim_metadata = metadata(removed);
        if (!victim_metadata) return reject(victim_metadata.status());
        if (victim_metadata.value().type != NodeType::regular || !victim_metadata.value().link_count ||
            removed < view_.first_ordinary_inode())
            return reject(Status::unsupported("mounted namespace victim requires live ordinary regular inode"));
        // Reserve/observe the actual victim address before durable publication.
        // It is never raw-exposed by this internal lookup.
        auto victim_result = node(removed, replace ? new_name : old_name);
        if (!victim_result) return reject(victim_result.status());
        auto *victim = static_cast<MountedNode *>(victim_result.value());
        const Ext4InodeLifetimeKey key{this, identity_.session_serial, removed, removed_generation};
        if (victim->lifetime_key_ != key) return fail(Status::fault("mounted namespace victim generation changed"));
        FixedString<max_path_segment> replacement;
        if (replace)
            if (auto status = replacement.assign(new_name); !status) return reject(status);
        if (replace)
            if (auto status = reserve_name_publication(source.value()); !status) return reject(status);
        u32 seconds{}, nanoseconds{};
        if (auto status = ordered_clock(seconds, nanoseconds); !status) return reject(status);
        const Ext4NamespaceUpdate update{
            replace ? Ext4NamespaceOperation::replace : Ext4NamespaceOperation::unlink,
            parent.inode_, source.value(), replace ? removed : 0,
            checked_parent.value().generation, source_inode.value().generation,
            replace ? removed_generation : 0, old_name, new_name, seconds, nanoseconds};
        auto status = owner_.prepare_namespace(update);
        if (!status)
        {
            if (owner_.report().phase != Ext4OwnedPhase::rejected) return fail(status);
            if (auto drained = owner_.rearm(); !drained) return fail(drained);
            return reject(status);
        }
        if (status = identity(epoch); !status) return fail(status);
        reset(view_);
        if (status = owner_.commit(); !status) return fail(status);
        if (status = identity(epoch); !status) return fail(status);
        if (status = owner_.checkpoint(); !status) return fail(status);
        if (status = identity(epoch); !status) return fail(status);
        const bool orphan = owner_.report().orphan_cleanup_pending;
        if (orphan != (victim_metadata.value().link_count == 1))
            return fail(Status::fault("mounted namespace orphan decision disagrees with checked links"));
        if (orphan)
        {
            auto retained = owner_.retained_orphan_binding(key);
            if (!retained) return fail(retained.status());
            binding_ = retained.value();
            if (status = view_.open_retained_orphan(reader_, binding_, removed, removed_generation, 0); !status)
                return fail(status);
        }
        else
        {
            if (status = inspect(true); !status) return fail(status);
            if (status = view_.open(reader_, binding_); !status) return fail(status);
        }
        if (view_.lookup(parent.inode_, old_name).status().code() != StatusCode::not_found)
            return fail(Status::fault("mounted namespace old name remains visible"));
        if (replace)
        {
            auto target = view_.lookup(parent.inode_, new_name);
            if (!target || target.value() != source.value())
                return fail(Status::fault("mounted replacement namespace disagrees"));
        }
        auto changed = metadata(removed);
        if (!changed || changed.value().link_count != victim_metadata.value().link_count - 1)
            return fail(changed ? Status::fault("mounted namespace victim links disagree") : changed.status());
        if (!orphan)
            if (status = owner_.rearm(); !status) return fail(status);
        {
            smp::ScopedSpinLock guard(lock_);
            if (report_.epoch != epoch || report_.phase != Ext4MountPhase::updating || pending_orphan_)
                status = Status::fault("mounted namespace publication epoch changed");
            else
            {
                if (orphan)
                {
                    status = lifetime_.mark_orphan(key);
                    if (status)
                    { pending_orphan_ = victim; orphan_seconds_ = seconds; orphan_nanoseconds_ = nanoseconds; }
                }
                if (status)
                {
                    if (replace) status = publish_name(source.value(), replacement);
                    if (status) { ++report_.epoch; report_.phase = Ext4MountPhase::readable; }
                }
            }
        }
        if (!status) return fail(status);
        return drain_pending_orphan();
    }

    [[nodiscard]] Status drain_pending_orphan()
    {
        MountedNode *victim{}; Ext4InodeLifetimeKey key{}; u64 epoch{};
        {
            smp::ScopedSpinLock guard(lock_);
            if (auto status = retention_integrity_locked(); !status) return status;
            const auto mutation = lifetime_.node_mutation_active(this, identity_.session_serial);
            if (!mutation) return fail_locked(mutation.status());
            // Descriptor release is legal during a mutation. Deferred orphan
            // transport cannot start until the original I/O slot is consumed.
            if (mutation.value()) return {};
            if (!pending_orphan_ || report_.phase != Ext4MountPhase::readable || report_.reading) return {};
            victim = pending_orphan_; key = victim->lifetime_key_;
            if (victim->references_ || victim->borrowed_identity_exposed_) return {};
            if (!reclaim_workspace_ || reclaim_workspace_->claimant != this ||
                key.mount != this || key.session != identity_.session_serial || key.inode != victim->inode_)
                return Status::fault("mounted orphan workspace or identity changed");
            auto state = lifetime_.snapshot(key);
            if (!state) return state.status();
            if (state.value().total()) return {};
            if (state.value().phase != Ext4InodeLifetimePhase::ready)
                return Status::fault("mounted orphan is not ready for exact dispatch");
            if (report_.epoch == ~u64{0}) return Status::overflow("mounted reclaim epoch exhausted");
            epoch = report_.epoch; report_.phase = Ext4MountPhase::updating;
        }
        owner_.publication_domain_ = epoch;
        auto status = identity(epoch);
        if (!status) return fail(status);
        // Scheduling grants a checked dispatch, never an acknowledgement. The
        // same Owner privately claims it immediately before journal publication.
        status = lifetime_.schedule_exact(key, [&](const typename Lifetime::Ticket &ticket) -> Status {
            if (auto checked = validate_node_retirement(ticket); !checked) return checked;
            auto &workspace = *reclaim_workspace_;
            reset(view_);
            auto prepared = owner_.prepare_orphan_reclaim(lifetime_, ticket, key,
                workspace.original, workspace.preparation, orphan_seconds_, orphan_nanoseconds_);
            if (!prepared) return prepared;
            if (auto published = owner_.publish_orphan_reclaim(workspace.observed, workspace.verification); !published)
                return published;
            return owner_.checkpoint_orphan_reclaim(workspace.observed, workspace.verification);
        });
        if (!status) return fail(status);
        if (!owner_.report().orphan_reclaimed || owner_.report().orphan_cleanup_pending)
            return fail(Status::fault("mounted reclaim did not durably complete"));
        if (status = identity(epoch); !status) return fail(status);
        if (status = inspect(true); !status) return fail(status);
        if (status = view_.open(reader_, binding_); !status) return fail(status);
        if (status = owner_.rearm(); !status) return fail(status);
        {
            smp::ScopedSpinLock guard(lock_);
            if (report_.epoch != epoch || report_.phase != Ext4MountPhase::updating ||
                pending_orphan_ != victim || victim->lifetime_key_ != key ||
                victim->references_ || victim->borrowed_identity_exposed_)
                status = Status::fault("mounted reclaim retirement identity changed");
            else
            {
                victim->inode_ = 0; victim->lifetime_key_ = {}; victim->name_ = {};
                victim->rename_names_ = {}; victim->rename_count_ = 0;
                pending_orphan_ = nullptr; orphan_seconds_ = orphan_nanoseconds_ = 0;
                ++report_.epoch; report_.phase = Ext4MountPhase::readable;
            }
        }
        return status ? status : fail(status);
    }

    [[nodiscard]] Result<Metadata> metadata(u32 inode)
    {
        if (policy_ == Ext4MountedPolicy::maintenance_only) return view_.query_metadata(inode);
        auto result = view_.query_stored_metadata(inode);
        if (result)
        {
            auto &a = result.value().attributes;
            a.access.source = a.modification.source = a.change.source = TimestampSource::maintained_ext4;
        }
        return result;
    }
    // Mount lock held. The caller closes admission only after obtaining any
    // required lifetime hold; this check itself performs no I/O or mutation.
    [[nodiscard]] Result<u64> update_admission_locked(Ext4MountedPolicy required)
    {
        if (auto status = admission(); !status) return status;
        if (policy_ != required) return Status::unsupported("mounted mutation policy unavailable");
        if (pending_orphan_) return Status::busy("mounted orphan cleanup precedes another mutation");
        if (report_.reading) return Status::busy("mounted reader in flight");
        if (report_.epoch == ~u64{0}) return Status::overflow("mounted admission epoch exhausted");
        const auto buffers = owner_.buffers();
        if (buffers.read_pins || buffers.in_flight) return Status::busy("mounted canonical reader in flight");
        return report_.epoch;
    }
    [[nodiscard]] Result<u64> begin_update(Ext4MountedPolicy required)
    {
        u64 epoch;
        {
            smp::ScopedSpinLock guard(lock_);
            auto admitted = update_admission_locked(required);
            if (!admitted) return admitted.status();
            epoch = admitted.value(); report_.phase = Ext4MountPhase::updating;
        }
        owner_.publication_domain_ = epoch;
        if (auto status = identity(epoch); !status) return fail(status);
        return epoch;
    }
    // The owner has not entered prepare: rearm is invalid in its open phase.
    [[nodiscard]] Status reject_unprepared(Status status, u64 epoch,
                                           const NodeMutationContext *original = nullptr)
    {
        if (!view_.valid()) return fail(status);
        if (auto authority = identity(epoch, original); !authority) return fail(authority);
        {
            smp::ScopedSpinLock guard(lock_);
            if (report_.phase != Ext4MountPhase::updating || report_.epoch != epoch)
                status = Status::fault("mounted rejection epoch changed");
            else { if (!original && !mutation_context_) report_.phase = Ext4MountPhase::readable; return status; }
        }
        return fail(status);
    }
    [[nodiscard]] Status ordered_clock(u32 &seconds, u32 &nanoseconds)
    {
        auto sample = arch::realtime_sample();
        if (!sample) return Status::io_error("ordered data requires active realtime");
        const auto &value = sample.value();
        if (value.value.nanoseconds >= time::nanoseconds_per_second || !value.wall_generation ||
            !value.monotonic.generation ||
            (value.origin != time::RealtimeOrigin::boot_rtc &&
             value.origin != time::RealtimeOrigin::administrator_software) ||
            value.value.seconds < 0 || value.value.seconds > 0x7fffffffLL)
            return Status::unsupported("ordered data realtime sample outside profile");
        seconds = static_cast<u32>(value.value.seconds);
        nanoseconds = value.value.nanoseconds;
        return {};
    }
    [[nodiscard]] Status data_image(u32 inode, u32 logical, u32 &physical, std::array<u8, 1024> &image)
    {
        auto block = view_.unique_initialized_data_block(inode, logical);
        if (!block) return block.status();
        physical = block.value();
        typename Owner::Pin pin;
        if (auto status = owner_.pin(physical, pin); !status) return fail(status);
        auto status = pin.read(0, std::as_writable_bytes(std::span{image}));
        const auto released = pin.release();
        return status ? released : status;
    }
    // Mount lock held. A public inode/key or a copied Node is never authority
    // for a synchronous mutation. Validate its original slot before its fields.
    [[nodiscard]] Status authentic_node_locked(const MountedNode &node) const
    {
        bool member{};
        for (const auto &slot : nodes_) if (&slot == &node) member = true;
        if (!member) return Status::bad_descriptor("mounted mutation Node is not an original slot");
        const auto &key = node.lifetime_key_;
        if (node.mount_ != this || !node.inode_ || key.mount != this ||
            key.session != identity_.session_serial || key.inode != node.inode_)
            return Status::bad_descriptor("mounted mutation Node identity differs");
        for (const auto &slot : nodes_)
            if (&slot != &node && slot.inode_ == node.inode_)
                return Status::bad_descriptor("mounted mutation Node identity is duplicated");
        auto state = lifetime_.snapshot(key);
        return state ? Status{} : Status::bad_descriptor("mounted mutation Node lifetime is absent");
    }
    // Strong fd admission requires an original retained live namespace node.
    // A raw pointer, diagnostic count or detached inode is not this contract.
    [[nodiscard]] Status persistent_node_locked(const MountedNode &node) const
    {
        if (auto status = authentic_node_locked(node); !status) return status;
        const auto state = lifetime_.snapshot(node.lifetime_key_);
        if (!state || state.value().phase != Ext4InodeLifetimePhase::linked)
            return Status::bad_descriptor("persistent sync node is detached");
        for (const auto &held : reference_holds_)
            if (held.node == &node && held.lease && held.lease->valid() &&
                lifetime_.validate_lease(*held.lease, node.lifetime_key_, Ext4InodeHold::descriptor))
                return {};
        return Status::bad_descriptor("persistent sync node has no live retained descriptor");
    }
    // Mount lock held. The operation's original address-bound Lease remains
    // authoritative even if its diagnostic count or Node fields are changed.
    [[nodiscard]] Status mutation_integrity_locked(const NodeMutationContext *original = nullptr) const
    {
        if (original)
        {
            if (mutation_context_ != original || mutation_node_ != &original->node ||
                mutation_key_ != original->key || mutation_lease_ != &original->lease ||
                original->epoch != owner_.publication_domain_)
                return Status::fault("mounted original mutation binding changed");
            if (auto status = authentic_node_locked(original->node); !status) return status;
            if (original->node.lifetime_key_ != original->key)
                return Status::fault("mounted original mutation Node changed");
            if (auto status = lifetime_.validate_node_mutation(original->lease, original->key,
                                                              original, original->epoch); !status) return status;
            return lifetime_.validate_lease(original->lease, original->key, Ext4InodeHold::io);
        }
        if (mutation_context_) return mutation_integrity_locked(mutation_context_);
        if (!mutation_node_)
            return mutation_lease_ || mutation_key_ != Ext4InodeLifetimeKey{} ?
                Status::fault("mounted mutation binding is incomplete") : Status{};
        if (auto status = authentic_node_locked(*mutation_node_); !status) return status;
        if (mutation_node_->lifetime_key_ != mutation_key_ || !mutation_lease_)
            return Status::fault("mounted original mutation identity changed");
        return lifetime_.validate_lease(*mutation_lease_, mutation_key_, Ext4InodeHold::io);
    }
    // The verified view supplies the table location, but a resident canonical
    // table is not a new physical observation. Read through the original owned
    // Session and check the complete inode checksum before its generation.
    [[nodiscard]] Status physical_node_generation(Ext4InodeLifetimeKey key)
    {
        if (!view_.valid() || view_.layout_.block_size != 1024 || !view_.inodes_per_group_ ||
            !key.inode || key.inode > view_.inodes_)
            return Status::fault("mounted physical inode view is unavailable");
        const auto local = (key.inode - 1) % view_.inodes_per_group_;
        const auto table = view_.groups_[(key.inode - 1) / view_.inodes_per_group_].table + local / 4;
        std::array<u8, 1024> block{};
        if (auto status = owner_.bound_device().read_blocks(u64{table} * 2,
                std::as_writable_bytes(std::span{block})); !status) return status;
        const auto inode = std::span<const u8>{block}.subspan((local % 4) * 256, 256);
        if (auto status = ext4_checked::inode_checksum(view_.checksum_seed(), key.inode, inode); !status)
            return status;
        return ext4_checked::le32(inode, 100) == key.generation ? Status{} :
            Status::io_error("mounted physical inode generation changed");
    }
    enum class MutationTerminalVerification { wrapper, operation };
    template <MutationTerminalVerification Verification = MutationTerminalVerification::wrapper,
              bool CheckedMetadata = false, typename Operation>
    [[nodiscard]] Status node_mutation_operation(MountedNode &node, Operation operation)
    {
        std::optional<Lease> in_flight;
        std::optional<NodeMutationContext> original;
        Ext4InodeLifetimeKey key{};
        u64 epoch{};
        {
            smp::ScopedSpinLock guard(lock_);
            if constexpr (CheckedMetadata)
                if (policy_ == Ext4MountedPolicy::maintenance_only)
                    return Status::unsupported("checked mutation requires maintained metadata");
            auto admitted = update_admission_locked(CheckedMetadata ? policy_ :
                policy_ == Ext4MountedPolicy::ordered_data_dirty_noatime ?
                Ext4MountedPolicy::ordered_data_dirty_noatime : Ext4MountedPolicy::ordered_data_noatime);
            if (!admitted) return admitted.status();
            if (auto status = authentic_node_locked(node); !status) return status;
            key = node.lifetime_key_;
            auto held = retain_existing(key, Ext4InodeHold::io);
            if (!held) return held.status();
            in_flight.emplace(std::move(held).value());
            mutation_node_ = &node; mutation_key_ = key; mutation_lease_ = &*in_flight;
            epoch = admitted.value(); report_.phase = Ext4MountPhase::updating;
            original.emplace(node, key, *in_flight, epoch); mutation_context_ = &*original;
            if (auto status = lifetime_.claim_node_mutation(*in_flight, key, &*original, epoch); !status)
            {
                const auto released = lifetime_.release(*in_flight);
                mutation_node_ = nullptr; mutation_key_ = {}; mutation_lease_ = nullptr; mutation_context_ = nullptr;
                return fail_locked(released ? status : released);
            }
        }
        owner_.publication_domain_ = epoch;
        auto status = identity(epoch, &*original);
        if (!status) status = fail(status);
        else
        {
            auto checked = view_.inode(key.inode);
            if (!checked)
                status = checked.status().code() == StatusCode::no_memory ||
                         checked.status().code() == StatusCode::overflow ?
                    reject_unprepared(checked.status(), epoch, &*original) : fail(checked.status());
            else if (checked.value().generation != key.generation)
                status = fail(Status::io_error("mounted inode generation changed during mutation"));
            else if (auto observed = physical_node_generation(key); !observed) status = fail(observed);
            else if (auto authority = identity(epoch, &*original); !authority) status = fail(authority);
            else status = operation(*original);
        }
        // A continuation that publishes names performs this physical check
        // inside its complete participant-lease scope before publication.
        if constexpr (Verification == MutationTerminalVerification::wrapper)
            if (status)
                if (auto observed = physical_node_generation(key); !observed) status = fail(observed);
        {
            smp::ScopedSpinLock guard(lock_);
            auto integrity = mutation_integrity_locked(&*original);
            if (report_.reading) integrity = Status::fault("mounted reader entered original mutation");
            if (mutation_node_ != &node || mutation_key_ != key || mutation_lease_ != &*in_flight)
                integrity = Status::fault("mounted original mutation binding changed");
            // Always consume the actual local Lease, including quarantined exits.
            const auto released = lifetime_.release_node_mutation(*in_flight, original->key,
                                                                  &*original, original->epoch);
            mutation_node_ = nullptr; mutation_key_ = {}; mutation_lease_ = nullptr;
            mutation_context_ = nullptr;
            if (!integrity || !released) status = fail_locked(!integrity ? integrity : released);
            else if (report_.phase != Ext4MountPhase::failed)
            {
                if (report_.phase != Ext4MountPhase::updating ||
                    (report_.epoch != epoch && report_.epoch != epoch + 1))
                    status = fail_locked(Status::fault("mounted mutation terminal epoch changed"));
                else report_.phase = Ext4MountPhase::readable;
            }
        }
        const auto cleanup = drain_pending_orphan();
        return cleanup ? status : cleanup;
    }
    [[nodiscard]] ModeChangeResult change_owner(MountedNode &node, u32 uid, u32 gid, user::Credentials caller)
    { return change_metadata<true>(node, uid, gid, caller); }
    template <bool Ownership>
    [[nodiscard]] ModeChangeResult change_metadata(MountedNode &node, u32 first, u32 second, user::Credentials caller)
    {
        if (Ownership && caller.euid != user::root_uid)
            return ModeChangeResult::failure(Status::permission("mounted ownership change requires root"));
        const auto status = node_mutation_operation<MutationTerminalVerification::wrapper, true>(node,
            [&](const NodeMutationContext &original) -> Status {
                const auto rejected = [&](Status value) { return reject_unprepared(value, original.epoch, &original); };
                auto current = metadata(original.key.inode);
                if (!current) return rejected(current.status());
                const auto &value = current.value();
                if ((value.type != NodeType::regular && value.type != NodeType::directory) ||
                    original.key.inode < view_.first_ordinary_inode())
                    return rejected(Status::unsupported("mounted metadata mutation requires ordinary regular inode or directory"));
                u32 uid = value.uid, gid = value.gid, permissions{};
                if constexpr (Ownership)
                {
                    const auto decision = owner_change_policy(value.uid, value.gid, value.mode, first, second, caller);
                    if (!decision.authorized) return rejected(Status::permission("mounted ownership change requires root"));
                    uid = decision.uid; gid = decision.gid; permissions = decision.permissions;
                }
                else
                {
                    const auto decision = mode_change_policy(value.uid, value.gid, first, caller);
                    if (!decision.authorized) return rejected(Status::permission("mounted mode requires owner or root"));
                    permissions = decision.permissions;
                }
                auto sampled = arch::realtime_sample();
                if (!sampled) return rejected(Status::io_error("maintained metadata requires active realtime"));
                const auto &clock = sampled.value();
                if (clock.value.nanoseconds >= time::nanoseconds_per_second || !clock.wall_generation ||
                    !clock.monotonic.generation || (clock.origin != time::RealtimeOrigin::boot_rtc &&
                    clock.origin != time::RealtimeOrigin::administrator_software))
                    return rejected(Status::io_error("invalid maintained metadata realtime sample"));
                if (clock.value.seconds < 0 || clock.value.seconds > 0x7fffffffLL)
                    return rejected(Status::overflow("journalled metadata ctime outside bounded writer range"));
                Ext4InodeMetadataUpdate update{original.key.inode, uid, gid,
                    static_cast<u32>(clock.value.seconds), clock.value.nanoseconds,
                    static_cast<u16>(permissions)};
                return commit_update<Ownership, !Ownership>(update, original.epoch, value.mode & mode_type_mask,
                    0, nullptr, 0, false, 0, &original);
            });
        return status.code() == StatusCode::unsupported && report().phase != Ext4MountPhase::failed ?
            ModeChangeResult::unavailable() : ModeChangeResult::failure(status);
    }
    [[nodiscard]] Status write_regular(MountedNode &node, usize offset, std::span<const std::byte> input)
    {
        if (input.empty()) return read_operation([&] { return Status{}; }, false, nullptr, &node);
        if (common_storage_)
        {
            const auto current = node.query_metadata();
            if (!current) return current.status();
            // Observation selects a route; the typed operation authenticates
            // the actual original Node/Lease and current media before writing.
            if (current.value().type == NodeType::regular && !current.value().size && !current.value().blocks)
                return typed_private_refill(node, common_storage_->child_, *common_storage_->requests_, offset, input);
        }
        return node_mutation_operation(node, [&](const NodeMutationContext &original) {
            return write_regular_admitted(original, offset, input);
        });
    }
    [[nodiscard]] Status write_regular_admitted(const NodeMutationContext &original, usize offset,
                                                std::span<const std::byte> input)
    {
        const auto inode = original.key.inode; const auto epoch = original.epoch;
        const auto reject = [&](Status status) { return reject_unprepared(status, epoch, &original); };
        auto current = metadata(inode);
        if (!current) return reject(current.status());
        const auto &m = current.value();
        if constexpr (sizeof(usize) > sizeof(u32))
            if (offset > 0xffffffffULL)
                return reject(Status::unsupported("ordered data write offset outside 32-bit inode size"));
        if (m.type != NodeType::regular || inode < view_.first_ordinary_inode() ||
            (m.mode & 06000) || input.size() > 1024 ||
            input.size() > 0xffffffffULL - offset)
            return reject(Status::unsupported("ordered data write target or range"));
        const u64 end = u64{offset} + input.size();
        if (!m.size)
        {
            if (policy_ != Ext4MountedPolicy::ordered_data_dirty_noatime || end > 1024 || m.blocks)
                return reject(Status::unsupported("first data write requires dirty empty single-block file"));
            std::array<u8, 1024> image{};
            for (usize index = 0; index < input.size(); ++index)
                image[offset + index] = std::to_integer<u8>(input[index]);
            u32 seconds{}, nanoseconds{};
            if (auto status = ordered_clock(seconds, nanoseconds); !status) return reject(status);
            const Ext4InodeMetadataUpdate update{inode, m.uid, m.gid, seconds, nanoseconds,
                static_cast<u16>(m.mode & mode_permission_mask), true,
                static_cast<u32>(end), seconds, nanoseconds};
            return commit_update(update, epoch, mode_regular, 0, &image, u32(offset), true, 0, &original);
        }
        if (policy_ == Ext4MountedPolicy::ordered_data_dirty_noatime &&
            m.size >= 1024 && m.size <= 15360 && !(m.size % 1024) &&
            offset >= m.size && end <= u64{m.size} + 1024)
        {
            std::array<u8, 1024> image{};
            for (usize index = 0; index < input.size(); ++index)
                image[offset - m.size + index] = std::to_integer<u8>(input[index]);
            u32 seconds{}, nanoseconds{};
            if (auto status = ordered_clock(seconds, nanoseconds); !status) return reject(status);
            const Ext4InodeMetadataUpdate update{inode, m.uid, m.gid, seconds, nanoseconds,
                static_cast<u16>(m.mode & mode_permission_mask), true,
                static_cast<u32>(end), seconds, nanoseconds};
            return commit_update(update, epoch, mode_regular, 0, &image, u32(offset), false, m.size, &original);
        }
        const u32 logical = static_cast<u32>(offset / 1024);
        if (logical != (end - 1) / 1024 || end > ((u64{m.size} + 1023) / 1024) * 1024)
            return reject(Status::unsupported("ordered data write crosses an allocated block"));
        std::array<u8, 1024> image{}; u32 physical{};
        if (auto status = data_image(inode, logical, physical, image); !status) return fail(status);
        if (end > m.size)
            for (u64 at = m.size; at < offset; ++at)
                image[at % 1024] = 0;
        for (usize index = 0; index < input.size(); ++index)
            image[offset % 1024 + index] = std::to_integer<u8>(input[index]);
        u32 seconds{}, nanoseconds{};
        if (auto status = ordered_clock(seconds, nanoseconds); !status) return reject(status);
        Ext4InodeMetadataUpdate update{inode, m.uid, m.gid, seconds, nanoseconds,
            static_cast<u16>(m.mode & mode_permission_mask), true,
            static_cast<u32>(end > m.size ? end : m.size), seconds, nanoseconds};
        return commit_update(update, epoch, mode_regular, physical, &image, 0, false, 0, &original);
    }
    [[nodiscard]] Status truncate_regular(MountedNode &node, usize size)
    {
        if (common_storage_ && !size)
            return typed_private_zero(node, common_storage_->child_, *common_storage_->requests_, size);
        return node_mutation_operation(node, [&](const NodeMutationContext &original) {
            return truncate_regular_admitted(original, size);
        });
    }
    [[nodiscard]] Status truncate_regular_admitted(const NodeMutationContext &original, usize size)
    {
        const auto inode = original.key.inode; const auto epoch = original.epoch;
        const auto reject = [&](Status status) { return reject_unprepared(status, epoch, &original); };
        auto current = metadata(inode);
        if (!current) return reject(current.status());
        const auto &m = current.value();
        if constexpr (sizeof(usize) > sizeof(u32))
            if (size > 0xffffffffULL)
                return reject(Status::unsupported("ordered data size outside 32-bit inode size"));
        if (m.type != NodeType::regular || !m.size || !size ||
            inode < view_.first_ordinary_inode() || (m.mode & 06000) ||
            (u64{size} + 1023) / 1024 != (u64{m.size} + 1023) / 1024)
            return reject(Status::unsupported("ordered data resize changes allocated block count"));
        if (size == m.size) return reject({});
        const u32 logical = static_cast<u32>((m.size - 1) / 1024);
        std::array<u8, 1024> image{}; u32 physical{};
        if (auto status = data_image(inode, logical, physical, image); !status) return fail(status);
        const usize zero_start = (size < m.size ? size : m.size) % 1024;
        for (usize at = zero_start; at < image.size(); ++at) image[at] = 0;
        u32 seconds{}, nanoseconds{};
        if (auto status = ordered_clock(seconds, nanoseconds); !status) return reject(status);
        Ext4InodeMetadataUpdate update{inode, m.uid, m.gid, seconds, nanoseconds,
            static_cast<u16>(m.mode & mode_permission_mask), true,
            static_cast<u32>(size), seconds, nanoseconds};
        return commit_update(update, epoch, mode_regular, physical, &image, 0, false, 0, &original);
    }
    [[nodiscard]] Result<Node *> create_node(MountedNode &parent, std::string_view name,
                                                 const InitialNodeAttributes &attributes, bool retained_result)
    {
        auto admitted = begin_update(Ext4MountedPolicy::ordered_data_dirty_noatime);
        if (!admitted) return admitted.status();
        const u64 epoch = admitted.value();
        const auto reject = [&](Status status) -> Result<Node *> { return reject_unprepared(status, epoch); };
        if (attributes.type != NodeType::regular && attributes.type != NodeType::directory)
            return reject(Status::unsupported("mounted creation requires a regular file or bounded directory"));
        if (attributes.mode & ~mode_permission_mask)
            return reject(Status::invalid_argument("mounted creation permissions outside POSIX mode"));
        if (name.empty() || name == "." || name == ".." || name.find('/') != std::string_view::npos ||
            name.find('\0') != std::string_view::npos)
            return reject(Status::invalid_argument("invalid mounted creation name"));
        auto current = metadata(parent.inode_);
        if (!current) return reject(current.status());
        if (current.value().type != NodeType::directory || !current.value().link_count)
            return reject(Status::not_directory("mounted creation parent is not a live directory"));
        auto prior = view_.lookup(parent.inode_, name);
        if (prior) return reject(Status::already_exists("mounted creation name exists"));
        if (prior.status().code() != StatusCode::not_found) return reject(prior.status());

        // Reserve a stable result address before the writer can issue a write or
        // FLUSH. An unpublished slot has inode zero and cannot be found by node().
        MountedNode *reserved = nullptr;
        for (auto &candidate : nodes_)
            if (!candidate.inode_ && !candidate.borrowed_identity_exposed_) { reserved = &candidate; break; }
        if (!reserved) return reject(Status::no_memory("mounted metadata node capacity"));
        if (auto status = reserved->name_.assign(name); !status) return reject(status);
        const auto release = [&] { reserved->name_ = {}; };
        u32 seconds{}, nanoseconds{};
        if (auto status = ordered_clock(seconds, nanoseconds); !status)
        { release(); return reject(status); }
        const Ext4CreateUpdate update{parent.inode_, seconds, nanoseconds, name, attributes};
        auto status = owner_.prepare_create(update);
        if (!status)
        {
            release();
            if (owner_.report().phase != Ext4OwnedPhase::rejected) return fail(status);
            if (auto drained = owner_.rearm(); !drained) return fail(drained);
            return reject(status);
        }
        const u32 inode = owner_.created_inode();
        if (!inode || inode < view_.first_ordinary_inode())
            return fail(Status::fault("mounted creation did not reserve an ordinary inode"));
        if (status = identity(epoch); !status) return fail(status);
        reset(view_);
        if (status = owner_.commit(); !status) return fail(status);
        if (status = identity(epoch); !status) return fail(status);
        if (status = owner_.checkpoint(); !status) return fail(status);
        if (status = identity(epoch); !status) return fail(status);
        for (usize index = 0; index < owner_.participant_count_; ++index)
        {
            std::array<std::byte, 1024> disk{}, cached{};
            if (status = owner_.bound_device().read_blocks(owner_.blocks_[index] * 2, disk); !status)
                return fail(status);
            typename Owner::Pin pin;
            if (status = owner_.pin(owner_.blocks_[index], pin); !status) return fail(status);
            status = pin.read(0, cached); const auto released = pin.release();
            if (!status || !released) return fail(status ? released : status);
            for (usize byte = 0; byte < disk.size(); ++byte)
                if (disk[byte] != cached[byte] || disk[byte] != std::byte(owner_.sealed_[index][byte]))
                    return fail(Status::fault("mounted create checkpoint image disagreement"));
        }
        if (status = inspect(true); !status) return fail(status);
        if (status = view_.open(reader_, binding_); !status) return fail(status);
        if (status = view_.validate_stored_metadata(); !status) return fail(status);
        auto found = view_.lookup(parent.inode_, name);
        if (!found || found.value() != inode)
            return fail(Status::fault("mounted create namespace disagreement"));
        auto created = metadata(inode);
        const bool directory_created = attributes.type == NodeType::directory;
        if (!created || created.value().type != attributes.type ||
            created.value().size != (directory_created ? 1024U : 0U) ||
            created.value().blocks != (directory_created ? 2U : 0U) ||
            created.value().mode != ((directory_created ? mode_directory : mode_regular) | attributes.mode) ||
            created.value().uid != attributes.uid || created.value().gid != attributes.gid ||
            created.value().link_count != (directory_created ? 2U : 1U) ||
            created.value().attributes.change.seconds != seconds ||
            created.value().attributes.change.nanoseconds != nanoseconds ||
            created.value().attributes.modification.seconds != seconds ||
            created.value().attributes.modification.nanoseconds != nanoseconds)
            return fail(created ? Status::fault("mounted create inode disagreement") : created.status());
        auto changed_parent = metadata(parent.inode_);
        if (!changed_parent || changed_parent.value().link_count != current.value().link_count +
                (directory_created ? 1U : 0U) ||
            changed_parent.value().attributes.change.seconds != seconds ||
            changed_parent.value().attributes.change.nanoseconds != nanoseconds ||
            changed_parent.value().attributes.modification.seconds != seconds ||
            changed_parent.value().attributes.modification.nanoseconds != nanoseconds)
            return fail(changed_parent ? Status::fault("mounted create parent timestamp disagreement") :
                                          changed_parent.status());
        if (directory_created)
        {
            const auto dot = view_.lookup(inode, "."), dotdot = view_.lookup(inode, "..");
            if (!dot || !dotdot || dot.value() != inode || dotdot.value() != parent.inode_)
                return fail(Status::fault("mounted mkdir dot identity disagreement"));
        }
        auto checked_inode = view_.inode(inode);
        if (!checked_inode) return fail(checked_inode.status());
        if (status = owner_.directory_continuation_end(); !status) return fail(status);
        if (status = owner_.rearm(); !status) return fail(status);
        const Ext4InodeLifetimeKey lifetime_key{this, identity_.session_serial,
                                                inode, checked_inode.value().generation};
        {
            smp::ScopedSpinLock guard(lock_);
            if (report_.epoch != epoch || report_.phase != Ext4MountPhase::updating || reserved->inode_)
                status = Status::fault("mounted create publication epoch changed");
            else
            {
                status = lifetime_.observe(lifetime_key);
                if (status)
                {
                    reserved->mount_ = this; reserved->inode_ = inode;
                    reserved->lifetime_key_ = lifetime_key;
                    if (retained_result) status = reserved->retain_locked();
                    else reserved->borrowed_identity_exposed_ = true;
                    if (status) { ++report_.epoch; report_.phase = Ext4MountPhase::readable; }
                }
            }
        }
        return status ? Result<Node *>{reserved} : Result<Node *>{fail(status)};
    }
#include "ok/fs/ext4_mounted_regular_move.inc"
    [[nodiscard]] Status rename_same_parent(MountedNode &parent, std::string_view old_name,
                                        Node &destination, std::string_view new_name)
    {
        if (&parent != &destination || parent.mount_ != this)
            return unsupported();
        // The namespace family supplies last-link replacement/orphan semantics.
        // Its no-target case retains the already qualified ordinary rename.
        if (reclaim_workspace_)
        {
            auto found = parent.lookup_child_retained(new_name);
            if (found)
            {
                auto released = found.value()->release();
                if (!released) return released;
                return namespace_regular(parent, old_name, new_name, false, true);
            }
            if (found.status().code() != StatusCode::not_found) return found.status();
        }
        FixedString<max_path_segment> replacement;
        if (auto status = replacement.assign(new_name); !status) return status;
        auto admitted = begin_update(Ext4MountedPolicy::ordered_data_dirty_noatime);
        if (!admitted) return admitted.status();
        const u64 epoch = admitted.value();
        const auto reject = [&](Status status) { return reject_unprepared(status, epoch); };
        auto source = view_.lookup(parent.inode_, old_name);
        if (!source) return reject(source.status());
        auto current = metadata(source.value());
        if (!current) return reject(current.status());
        if ((current.value().type != NodeType::regular && current.value().type != NodeType::directory) ||
            source.value() < view_.first_ordinary_inode())
            return reject(Status::unsupported("mounted rename requires ordinary file or directory source"));
        auto target = view_.lookup(parent.inode_, new_name);
        if (!target && target.status().code() != StatusCode::not_found)
            return reject(target.status());
        if (target && target.value() == source.value())
        {
            auto directory = metadata(parent.inode_);
            if (!directory) return reject(directory.status());
            if (directory.value().type != NodeType::directory || !directory.value().link_count ||
                (directory.value().mode & 0300) != 0300)
                return reject(Status::unsupported("mounted rename parent lacks owner write/search bits"));
            if (old_name != new_name && current.value().link_count < 2)
                return reject(Status::io_error("mounted same-inode rename links disagree"));
            if (old_name != new_name && current.value().type == NodeType::directory)
                return reject(Status::io_error("mounted source directory has a namespace alias"));
            // Namespace identity is unchanged; keep the owner and epoch open.
            return reject_unprepared({}, epoch);
        }
        if (auto status = reserve_name_publication(source.value()); !status) return reject(status);
        u32 seconds{}, nanoseconds{};
        if (auto status = ordered_clock(seconds, nanoseconds); !status) return reject(status);
        const Ext4RenameUpdate update{parent.inode_, source.value(), seconds, nanoseconds,
                                       old_name, new_name, current.value().type == NodeType::directory};
        auto status = owner_.prepare_rename(update);
        if (!status)
        {
            if (owner_.report().phase != Ext4OwnedPhase::rejected) return fail(status);
            if (auto drained = owner_.rearm(); !drained) return fail(drained);
            return reject(status);
        }
        if (status = identity(epoch); !status) return fail(status);
        reset(view_);
        if (status = owner_.commit(); !status) return fail(status);
        if (status = identity(epoch); !status) return fail(status);
        if (status = owner_.checkpoint(); !status) return fail(status);
        if (status = identity(epoch); !status) return fail(status);
        for (usize index = 0; index < owner_.participant_count_; ++index)
        {
            std::array<std::byte, 1024> disk{}, cached{};
            if (status = owner_.bound_device().read_blocks(owner_.blocks_[index] * 2, disk); !status)
                return fail(status);
            typename Owner::Pin pin;
            if (status = owner_.pin(owner_.blocks_[index], pin); !status) return fail(status);
            status = pin.read(0, cached); const auto released = pin.release();
            if (!status || !released) return fail(status ? released : status);
            for (usize byte = 0; byte < disk.size(); ++byte)
                if (disk[byte] != cached[byte] || disk[byte] != std::byte(owner_.sealed_[index][byte]))
                    return fail(Status::fault("mounted rename checkpoint image disagreement"));
        }
        if (status = inspect(true); !status) return fail(status);
        if (status = view_.open(reader_, binding_); !status) return fail(status);
        auto found = view_.lookup(parent.inode_, new_name);
        if (!found || found.value() != source.value() ||
            view_.lookup(parent.inode_, old_name).status().code() != StatusCode::not_found)
            return fail(Status::fault("mounted rename namespace disagreement"));
        auto changed = metadata(source.value());
        if (!changed || changed.value().attributes.change.seconds != seconds ||
            changed.value().attributes.change.nanoseconds != nanoseconds)
            return fail(Status::fault("mounted rename inode timestamp disagreement"));
        if (status = owner_.rearm(); !status) return fail(status);
        {
            smp::ScopedSpinLock guard(lock_);
            if (report_.epoch != epoch || report_.phase != Ext4MountPhase::updating)
                status = Status::fault("mounted rename epoch changed");
            else
            {
                status = publish_name(source.value(), replacement);
                if (status) { ++report_.epoch; report_.phase = Ext4MountPhase::readable; }
            }
        }
        return status ? status : fail(status);
    }
    template <bool CheckedOwnership = false, bool CheckedMode = false>
    [[nodiscard]] Status commit_update(Ext4InodeMetadataUpdate update, u64 epoch, u32 expected_type,
                                       u32 data_block = 0, const std::array<u8, 1024> *data = nullptr,
                                       u32 first_offset = 0, bool allocate_first = false,
                                       u32 next_old_size = 0, const NodeMutationContext *original = nullptr)
    {
        auto status = identity(epoch, original);
        if (!status) return fail(status);
        if (original && update.inode != original->key.inode)
            return fail(Status::fault("mounted mutation update inode is foreign"));
        status = CheckedOwnership ? owner_.prepare_ownership(update) : CheckedMode ? owner_.prepare_mode(update) :
            allocate_first && data ? owner_.prepare_first_data(
            {update.inode, update.size, update.ctime_seconds, update.ctime_nanoseconds}, first_offset, *data) :
            next_old_size && data ? owner_.prepare_next_data(
            {update.inode, next_old_size, update.size, update.ctime_seconds, update.ctime_nanoseconds},
            first_offset, *data) :
            data ? owner_.prepare_ordered_data(update, data_block, *data) :
            policy_ == Ext4MountedPolicy::maintenance_only ? owner_.template prepare_for<false>(update)
                                                          : owner_.prepare(update);
        if (!status)
        {
            if (owner_.report().phase != Ext4OwnedPhase::rejected) return fail(status);
            if (auto drained = owner_.rearm(); !drained) return fail(drained);
            return reject_unprepared(status, epoch, original);
        }
        if (original)
        {
            const auto local = (update.inode - 1) % view_.inodes_per_group_;
            const auto table = view_.groups_[(update.inode - 1) / view_.inodes_per_group_].table + local / 4;
            bool found{};
            for (usize index = 0; index < owner_.participant_count_; ++index)
                if (owner_.blocks_[index] == table)
                {
                    found = true;
                    const auto inode = std::span<const u8>{owner_.sealed_[index]}.subspan((local % 4) * 256, 256);
                    if (auto checked = ext4_checked::inode_checksum(view_.checksum_seed(), update.inode, inode); !checked)
                        return fail(checked);
                    if (ext4_checked::le32(inode, 100) != original->key.generation)
                        return fail(Status::io_error("mounted prepared inode generation changed"));
                }
            if (!found) return fail(Status::fault("mounted prepared inode image is absent"));
        }
        // No public read is admitted between retiring this immutable view and
        // validating its replacement after checkpoint.
        if (status = identity(epoch, original); !status) return fail(status);
        reset(view_);
        if (status = owner_.commit(); !status) return fail(status);
        if (status = identity(epoch, original); !status) return fail(status);
        if (status = owner_.checkpoint(); !status) return fail(status);
        if (status = identity(epoch, original); !status) return fail(status);
        // A reopened reader is insufficient: compare every changed home/control
        // block against both the sealed clean image and its canonical cache bytes.
        for (usize index = 0; index < owner_.participant_count_; ++index)
        {
            std::array<std::byte, 1024> disk{}, cached{};
            if (status = owner_.bound_device().read_blocks(owner_.blocks_[index] * 2, disk); !status) return fail(status);
            typename Owner::Pin pin;
            if (status = owner_.pin(owner_.blocks_[index], pin); !status) return fail(status);
            status = pin.read(0, cached); const auto released = pin.release();
            if (!status || !released) return fail(status ? released : status);
            for (usize byte = 0; byte < disk.size(); ++byte)
                if (disk[byte] != cached[byte] || disk[byte] != std::byte(owner_.sealed_[index][byte]))
                    return fail(Status::fault("mounted checkpoint canonical image disagreement"));
        }
        if (status = inspect(true); !status) return fail(status);
        if (status = view_.open(reader_, binding_); !status) return fail(status);
        if (original)
        {
            auto changed = view_.inode(update.inode);
            if (!changed) return fail(changed.status());
            if (changed.value().generation != original->key.generation)
                return fail(Status::io_error("mounted renewed inode generation changed"));
        }
        auto metadata = this->metadata(update.inode);
        if (!metadata || metadata.value().mode != (expected_type | update.permissions) ||
            metadata.value().uid != update.uid || metadata.value().gid != update.gid)
            return fail(metadata ? Status::fault("mounted updated inode mismatch") : metadata.status());
        if (update.ordered_data && (metadata.value().size != update.size ||
            metadata.value().attributes.modification.seconds != update.mtime_seconds ||
            metadata.value().attributes.modification.nanoseconds != update.mtime_nanoseconds))
            return fail(Status::fault("mounted ordered-data size or mtime mismatch"));
        if (allocate_first && (metadata.value().blocks != 2 ||
            metadata.value().attributes.change.seconds != update.ctime_seconds ||
            metadata.value().attributes.change.nanoseconds != update.ctime_nanoseconds))
            return fail(Status::fault("mounted first data block or ctime mismatch"));
        if (next_old_size && (metadata.value().blocks != (next_old_size / 1024 + 1) * 2 ||
            metadata.value().attributes.change.seconds != update.ctime_seconds ||
            metadata.value().attributes.change.nanoseconds != update.ctime_nanoseconds))
            return fail(Status::fault("mounted next data block or ctime mismatch"));
        if ((CheckedOwnership || CheckedMode || policy_ == Ext4MountedPolicy::maintained_noatime) &&
            (metadata.value().attributes.change.seconds != update.ctime_seconds ||
             metadata.value().attributes.change.nanoseconds != update.ctime_nanoseconds))
            return fail(Status::fault("mounted updated ctime mismatch"));
        if (status = owner_.rearm(); !status) return fail(status);
        {
            smp::ScopedSpinLock guard(lock_);
            if (report_.epoch != epoch || report_.phase != Ext4MountPhase::updating)
                status = Status::fault("mounted admission epoch changed");
            else { ++report_.epoch; if (!original && !mutation_context_) report_.phase = Ext4MountPhase::readable; }
        }
        return status ? status : fail(status);
    }
    template <typename T> static void reset(T &object)
    { object.~T(); ::new (static_cast<void *>(&object)) T; }
    // Mount lock held. Root-domain failure quarantines the original Owner;
    // bookkeeping never follows a mutable token pointer to a decoy provider.
    [[nodiscard]] Status retention_integrity_locked()
    {
        if (!report_.epoch) return {};
        const auto status = root_retention_.binding(&nodes_[0], this, identity_.session_serial,
                                                   identity_.registration_generation);
        return status ? status : fail_locked(status);
    }
    [[nodiscard]] Status fail_locked(Status status)
    {
        // Owner failure only aborts its internal Cache/Session; no device I/O
        // or caller callbacks occur while the mount admission lock is held.
        if (startup_workspace_) owner_.startup_failed(status);
        root_retention_.stop_admission();
        static_cast<void>(owner_.fail(status));
        report_.phase = Ext4MountPhase::failed; report_.status = status; return status;
    }
    // Mount lock held. Neither an absent deferred pointer nor summary counters
    // erase actual Owner/Cache records. This private bounded check performs no I/O.
    [[nodiscard]] Status orphan_work_drained_locked()
    {
        if (pending_orphan_)
            return Status::busy("mounted orphan cleanup precedes drain");
        const auto status = owner_.mounted_teardown_preflight();
        return status || status.code() == StatusCode::busy ? status : fail_locked(status);
    }
    [[nodiscard]] Status drained_locked()
    {
        if (auto status = retention_integrity_locked(); !status) return status;
        if (auto status = mutation_admission_locked(); !status) return status;
        if (report_.phase != Ext4MountPhase::draining)
            return Status::not_initialized("mounted provider is not draining");
        if (legacy_exposure_) return Status::busy("mounted provider has permanent borrowed exposure");
        if (auto status = root_retention_.empty(); !status) return status;
        if (report_.reading) return Status::busy("mounted work remains while draining");
        if (auto status = orphan_work_drained_locked(); !status) return status;
        if (auto status = lifetime_.audit_lease_records(this, identity_.session_serial); !status)
            return fail_locked(status);
        usize records{}, references{};
        for (const auto &node : nodes_)
        {
            usize held_count{};
            for (const auto &held : reference_holds_)
                if (held.node == &node)
                {
                    if (!held.lease || !held.lease->valid())
                        return fail_locked(Status::fault("mounted node lease record differs"));
                    if (auto status = lifetime_.validate_lease(*held.lease, node.lifetime_key_,
                                                              Ext4InodeHold::descriptor); !status)
                        return fail_locked(status);
                    ++held_count;
                }
            if (held_count != node.references_)
                return fail_locked(Status::fault("mounted node count differs from exact leases"));
            if (node.inode_)
            {
                const auto state = lifetime_.snapshot(node.lifetime_key_);
                if (!state || state.value().holds[static_cast<usize>(Ext4InodeHold::descriptor)] != held_count)
                    return fail_locked(Status::fault("mounted inode table differs from Node leases"));
                if (state.value().holds[static_cast<usize>(Ext4InodeHold::mapping)] ||
                    state.value().holds[static_cast<usize>(Ext4InodeHold::io)] ||
                    state.value().holds[static_cast<usize>(Ext4InodeHold::pin)])
                    return Status::busy("mounted inode has retained non-Node work");
            }
            references += held_count;
        }
        for (const auto &held : reference_holds_)
        {
            if (bool(held.node) != bool(held.lease))
                return fail_locked(Status::fault("mounted reference record differs"));
            if (held.node) ++records;
        }
        for (usize first = 0; first < reference_holds_.size(); ++first)
            if (reference_holds_[first].lease)
                for (usize second = first + 1; second < reference_holds_.size(); ++second)
                    if (reference_holds_[second].lease && Lifetime::same_lease(*reference_holds_[first].lease,
                                                                             *reference_holds_[second].lease))
                        return fail_locked(Status::fault("mounted Node lease attachment is duplicated"));
        if (records != references) return fail_locked(Status::fault("mounted reference names foreign Node"));
        if (references) return Status::busy("mounted node reference retained");
        const auto buffers = owner_.buffers();
        if (buffers.read_pins || buffers.in_flight || buffers.transaction_active)
            return Status::busy("mounted buffers have retained work");
        return {};
    }
    // Mount lock held. The occupied original I/O role closes public admission
    // even if every mutable mount binding and diagnostic phase are erased.
    [[nodiscard]] Status mutation_admission_locked()
    {
        const auto active = lifetime_.node_mutation_active(this, identity_.session_serial);
        if (!active) return fail_locked(active.status());
        return active.value() ? Status::busy("mounted original mutation I/O remains admitted") : Status{};
    }
    [[nodiscard]] Status admission()
    {
        if (auto status = retention_integrity_locked(); !status) return status;
        if (report_.phase == Ext4MountPhase::failed) return Status::denied("mounted metadata is quarantined");
        if (auto status = mutation_admission_locked(); !status) return status;
        if (report_.phase == Ext4MountPhase::readable) return {};
        if (report_.phase == Ext4MountPhase::opening) return Status::busy("mounted metadata startup in flight");
        if (report_.phase == Ext4MountPhase::updating) return Status::busy("mounted metadata mutation in flight");
        if (report_.phase == Ext4MountPhase::draining) return Status::busy("mounted metadata is draining");
        return Status::not_initialized("mounted metadata unavailable");
    }
    [[nodiscard]] Status unsupported()
    { smp::ScopedSpinLock guard(lock_); auto status = admission(); return status ? Status::unsupported("metadata mount rejects ordinary mutation") : status; }
    [[nodiscard]] Status fail(Status status)
    {
        if (startup_workspace_) owner_.startup_failed(status);
        root_retention_.stop_admission();
        static_cast<void>(owner_.fail(status));
        smp::ScopedSpinLock guard(lock_); report_.phase = Ext4MountPhase::failed; report_.status = status; return status;
    }
    [[nodiscard]] Status identity(u64 epoch = 0, const NodeMutationContext *original = nullptr) const
    {
        if (epoch)
        {
            smp::ScopedSpinLock guard(lock_);
            if (report_.epoch != epoch || owner_.publication_domain_ != epoch ||
                report_.phase != Ext4MountPhase::updating || report_.reading)
                return Status::fault("mounted publication epoch changed");
            if (auto status = mutation_integrity_locked(original); !status) return status;
        }
        const auto now = owner_.buffers();
        const auto session = owner_.session_report();
        if (!session || session.value().state != block::DeviceState::owned ||
            session.value().identity != identity_.device_identity ||
            session.value().registration_generation != identity_.registration_generation ||
            session.value().session_serial != identity_.session_serial)
            return Status::fault("mounted session is not authoritative");
        if (now.state != MetadataOwnerState::ready || now.device_identity != identity_.device_identity ||
            now.registration_generation != identity_.registration_generation || now.session_serial != identity_.session_serial ||
            now.first_sector != identity_.first_sector || now.sectors != identity_.sectors || now.blocks != identity_.blocks)
            return Status::fault("mounted session authority changed");
        return {};
    }
    [[nodiscard]] Status inspect(bool renewed)
    {
        inspection_.~Ext4RecoveryCompletion<8, 2>();
        ::new (static_cast<void *>(&inspection_)) Ext4RecoveryCompletion<8, 2>(
            policy_ == Ext4MountedPolicy::ordered_data_dirty_noatime,
            policy_ == Ext4MountedPolicy::ordered_data_dirty_noatime);
        inspection_.require_one_kib();
        auto &device = owner_.startup_domain_active() ? static_cast<driver::BlockDevice &>(*owner_.startup_reader_) : owner_.bound_device();
        if (auto status = inspection_.prepare(device, owner_.bounds_); !status) return status;
        const auto &value = *inspection_.binding();
        if (value.needs_recovery != (policy_ == Ext4MountedPolicy::ordered_data_dirty_noatime) ||
            !value.empty_log || value.layout.block_size != 1024)
            return Status::unsupported("metadata mount requires selected empty 1 KiB journal state");
        if (renewed)
        {
            const auto &a = binding_.layout; const auto &b = value.layout;
            if (a.first_sector != b.first_sector || a.filesystem_blocks != b.filesystem_blocks ||
                a.journal_first_block != b.journal_first_block || a.journal_blocks != b.journal_blocks ||
                a.block_size != b.block_size || binding_.uuid != value.uuid ||
                binding_.journal_inode != value.journal_inode || binding_.metadata_count != value.metadata_count ||
                value.journal_sequence != owner_.report().completion.journal_sequence)
                return Status::fault("mounted binding changed at renewal");
            for (usize index = 0; index < value.metadata_count; ++index)
                if (value.metadata[index].first != binding_.metadata[index].first ||
                    value.metadata[index].count != binding_.metadata[index].count)
                    return Status::fault("mounted metadata domain changed at renewal");
        }
        binding_ = value; return identity();
    }
    [[nodiscard]] Status recover_and_mark_dirty()
    {
        auto &startup_device = owner_.startup_domain_active() ?
            static_cast<driver::BlockDevice &>(*owner_.startup_ordinary_reader_) : owner_.bound_device();
        if (owner_.startup_legacy_selected_)
        {
            if (auto status = owner_.startup_domain_guard(); !status) return status;
            const auto actions = owner_.startup_legacy_actions_;
            const auto replay = actions == 1 ? Ext4ReplayPolicy::mounted_inode :
                actions < 4 ? Ext4ReplayPolicy::directory_rename :
                actions == 4 ? Ext4ReplayPolicy::bounded_data_allocation :
                actions <= 6 ? Ext4ReplayPolicy::directory_create : Ext4ReplayPolicy::bounded_directory_create;
            inspection_.~Ext4RecoveryCompletion<8, 2>();
            ::new (static_cast<void *>(&inspection_)) Ext4RecoveryCompletion<8, 2>(
                replay == Ext4ReplayPolicy::mounted_inode, true, replay);
            // The private original selection and complete strict proof precede
            // every recovery WRITE/FLUSH. Never retry another policy on failure.
            return inspection_.run_legacy_startup(startup_device, owner_.bounds_,
                owner_.startup_workspace_->legacy_head_seal_,
                owner_.startup_workspace_->legacy_descriptor_seal_, actions);
        }
        // Recognize the union of exact four-action allocation proofs before the legacy
        // structural gate, including an all-after checkpoint. A recognized
        // allocation proof failure can never fall through to a weaker policy.
        inspection_.~Ext4RecoveryCompletion<8, 2>();
        ::new (static_cast<void *>(&inspection_)) Ext4RecoveryCompletion<8, 2>(
            false, true, Ext4ReplayPolicy::bounded_data_allocation);
        Ext4ReplayPolicy replay = Ext4ReplayPolicy::bounded_data_allocation;
        auto status = inspection_.run(startup_device, owner_.bounds_);
        if (!status)
        {
            const auto &failed = inspection_.report();
            const bool other_shape = status.code() == StatusCode::unsupported &&
                status.message() == "ext4 first data replay transaction shape";
            const bool create_split = status.code() == StatusCode::io_error &&
                (status.message() == "ext4 recovery inode bitmap checksum" ||
                 status.message() == "ext4 recovery block bitmap checksum");
            if ((!other_shape && !create_split) || failed.write_attempts || failed.flush_attempts)
                return status;
            inspection_.~Ext4RecoveryCompletion<8, 2>();
            ::new (static_cast<void *>(&inspection_)) Ext4RecoveryCompletion<8, 2>(true, true, Ext4ReplayPolicy::mounted_inode);
            replay = Ext4ReplayPolicy::mounted_inode;
            status = inspection_.run(startup_device, owner_.bounds_);
        }
        if (!status)
        {
            const auto &failed = inspection_.report();
            const bool target_denied = status.code() == StatusCode::denied ||
                (status.code() == StatusCode::unsupported &&
                 status.message() == "checked metadata replay transaction shape");
            const bool provisional_bitmap = status.code() == StatusCode::io_error &&
                (status.message() == "ext4 recovery inode bitmap checksum" ||
                 status.message() == "ext4 recovery block bitmap checksum");
            if ((!target_denied && !provisional_bitmap) || failed.write_attempts || failed.flush_attempts)
                return status;
            // A different action shape refused before I/O. A create can leave
            // its bitmap and descriptor home checksums split at a crash cut;
            // only the exact create replay proof may admit that state.
            inspection_.~Ext4RecoveryCompletion<8, 2>();
            ::new (static_cast<void *>(&inspection_)) Ext4RecoveryCompletion<8, 2>(
                false, true, Ext4ReplayPolicy::bounded_directory_create);
            replay = Ext4ReplayPolicy::bounded_directory_create;
            status = inspection_.run(startup_device, owner_.bounds_);
            if (!status)
            {
                const auto &mkdir_failed = inspection_.report();
                if (status.code() != StatusCode::unsupported ||
                    status.message() != "ext4 create replay transaction shape" ||
                    mkdir_failed.write_attempts || mkdir_failed.flush_attempts)
                    return status;
                inspection_.~Ext4RecoveryCompletion<8, 2>();
                ::new (static_cast<void *>(&inspection_)) Ext4RecoveryCompletion<8, 2>(
                    false, true, Ext4ReplayPolicy::directory_create);
                replay = Ext4ReplayPolicy::directory_create;
                status = inspection_.run(startup_device, owner_.bounds_);
            }
            if (!status)
            {
                const auto &create_failed = inspection_.report();
                if (!target_denied || status.code() != StatusCode::unsupported ||
                    status.message() != "ext4 create replay transaction shape" ||
                    create_failed.write_attempts || create_failed.flush_attempts)
                    return status;
                inspection_.~Ext4RecoveryCompletion<8, 2>();
                ::new (static_cast<void *>(&inspection_)) Ext4RecoveryCompletion<8, 2>(
                    false, true, Ext4ReplayPolicy::directory_rename);
                replay = Ext4ReplayPolicy::directory_rename;
                status = inspection_.run(startup_device, owner_.bounds_);
                if (!status) return status;
            }
        }
        if ((ext4_create_replay_policy(replay) || replay == Ext4ReplayPolicy::directory_rename) &&
            inspection_.report().empty_log)
            return Status::fault("mounted directory recovery has no committed transaction");
        if (!inspection_.report().empty_log &&
            (replay == Ext4ReplayPolicy::directory_rename ?
                (inspection_.report().validated_actions != 2 && inspection_.report().validated_actions != 3) :
             replay == Ext4ReplayPolicy::bounded_directory_create ?
                (inspection_.report().validated_actions != 7 && inspection_.report().validated_actions != 8) :
             replay == Ext4ReplayPolicy::directory_create ?
                (inspection_.report().validated_actions != 5 && inspection_.report().validated_actions != 6) :
             ext4_data_allocation_policy(replay) ? inspection_.report().validated_actions != 4 :
                inspection_.report().validated_actions != 1))
            return Status::fault("mounted recovery action count");
        if (!inspection_.report().already_clean) return {};
        auto super = inspection_.filesystem_;
        Ext4RecoveryCompletion<8, 2>::le32(super, 96, ext4_checked::le32(super, 96) | u32{4});
        Ext4RecoveryCompletion<8, 2>::le32(super, 1020,
            jbd2::detail::crc32c(~u32{0}, std::span<const u8>{super}.first(1020)));
        auto &device = startup_device;
        if (auto status = device.write_blocks(2, std::as_bytes(std::span{super})); !status) return status;
        return device.flush();
    }
    template <typename Operation> auto read_operation(Operation operation, bool transport = false,
                                                      const Ext4InodeLifetimeKey *key = nullptr,
                                                      const MountedNode *node = nullptr,
                                                      bool persistent = false) -> decltype(operation())
    {
        using ResultType = decltype(operation());
        std::optional<Lease> in_flight;
        Ext4InodeLifetimeKey original_key{};
        Status acquire_status{};
        {
            smp::ScopedSpinLock guard(lock_);
            if (auto status = admission(); !status) return ResultType{status};
            if (report_.reading) return ResultType{Status::busy("mounted reader already admitted")};
            if (node)
            {
                if (auto status = authentic_node_locked(*node); !status) return ResultType{status};
                if (persistent)
                    if (auto status = persistent_node_locked(*node); !status) return ResultType{status};
                original_key = node->lifetime_key_; key = &original_key;
            }
            if (key)
            {
                auto held = retain_existing(*key, Ext4InodeHold::io);
                if (!held) acquire_status = held.status();
                else in_flight.emplace(std::move(held).value());
            }
            if (acquire_status) report_.reading = true;
        }
        if (!acquire_status)
            return ResultType{acquire_status.code() == StatusCode::no_memory ||
                              acquire_status.code() == StatusCode::overflow ||
                              acquire_status.code() == StatusCode::busy ?
                                  acquire_status : fail(acquire_status)};
        auto identity_status = identity();
        const bool authority_failed = !identity_status;
        bool checked_fatal = false;
        if (identity_status && key)
        {
            auto checked = view_.inode(key->inode);
            if (!checked)
            {
                identity_status = checked.status();
                checked_fatal = checked.status().code() != StatusCode::no_memory &&
                                checked.status().code() != StatusCode::overflow;
            }
            else if (checked.value().generation != key->generation)
            {
                identity_status = Status::io_error("mounted inode generation changed during read");
                checked_fatal = true;
            }
            if (identity_status && node)
            {
                identity_status = physical_node_generation(*key);
                checked_fatal = !identity_status;
            }
        }
        if (identity_status && node)
        {
            smp::ScopedSpinLock guard(lock_);
            identity_status = authentic_node_locked(*node);
            if (identity_status && node->lifetime_key_ != original_key)
                identity_status = Status::fault("mounted original zero-write Node changed");
            if (identity_status) identity_status = lifetime_.validate_lease(*in_flight, original_key, Ext4InodeHold::io);
            checked_fatal = !identity_status;
        }
        auto result = identity_status ? operation() : ResultType{identity_status};
        Status status = [&]() { if constexpr (std::is_same_v<ResultType, Status>) return result; else return result.status(); }();
        if (persistent && status)
        {
            auto checked = identity();
            if (checked)
            {
                smp::ScopedSpinLock guard(lock_);
                checked = persistent_node_locked(*node);
                if (checked && node->lifetime_key_ != original_key)
                    checked = Status::fault("persistent sync original node changed during I/O");
                if (checked) checked = lifetime_.validate_lease(*in_flight, original_key, Ext4InodeHold::io);
            }
            if (!checked) { status = checked; checked_fatal = true; }
        }
        const Status released = in_flight ? lifetime_.release(*in_flight) : Status{};
        // Reader I/O errors retire the view. Benign lookup/capacity errors do not.
        const bool fatal = authority_failed || checked_fatal || !released || !view_.valid() || (!status && (transport || (status.code() != StatusCode::not_found &&
            status.code() != StatusCode::unsupported && status.code() != StatusCode::overflow &&
            status.code() != StatusCode::not_directory && status.code() != StatusCode::invalid_argument &&
            status.code() != StatusCode::is_directory && status.code() != StatusCode::no_memory)));
        if (fatal)
        {
            const auto failure = fail(!released ? released :
                                      status ? Status::fault("mounted view retired") : status);
            { smp::ScopedSpinLock guard(lock_); report_.reading = false; }
            return ResultType{failure};
        }
        { smp::ScopedSpinLock guard(lock_); report_.reading = false; }
        const auto cleanup = drain_pending_orphan();
        if (status && !cleanup) return ResultType{cleanup};
        return result;
    }
    // Required before a future worker retires or clears a Node identity. Raw
    // legacy exposure has no revocable token and therefore permanently bars
    // that slot's retirement/reuse for this provider lifetime. No freeing here.
    [[nodiscard]] Status validate_node_retirement(const typename Lifetime::Ticket &ticket) const
    {
        smp::ScopedSpinLock guard(lock_);
        if (report_.reading || report_.phase != Ext4MountPhase::updating)
            return Status::busy("mounted node retirement requires exclusive update");
        const auto key = ticket.key();
        if (key.mount != this || key.session != identity_.session_serial)
            return Status::bad_descriptor("mounted node retirement identity is foreign");
        if (key.inode < 11)
            return Status::unsupported("mounted reserved inode retirement is forbidden");
        if (auto status = lifetime_.validate_dispatched(ticket, key); !status) return status;
        for (const auto &node : nodes_)
            if (node.inode_ == key.inode && node.lifetime_key_ == key)
                return node.references_ || node.borrowed_identity_exposed_ ?
                    Status::busy("mounted node identity remains exposed") : Status{};
        return Status::not_found("mounted node retirement identity is absent");
    }

    [[nodiscard]] Result<Node *> node(u32 inode, std::string_view name)
    {
        auto checked = view_.inode(inode);
        if (!checked) return checked.status();
        const Ext4InodeLifetimeKey key{this, identity_.session_serial, inode,
                                       checked.value().generation};
        smp::ScopedSpinLock guard(lock_);
        for (auto &node : nodes_) if (node.inode_ == inode)
            return node.lifetime_key_ == key ? Result<Node *>{static_cast<Node *>(&node)} :
                Result<Node *>{Status::io_error("mounted inode generation changed under a live node")};
        for (auto &node : nodes_) if (!node.inode_ && !node.borrowed_identity_exposed_)
        {
            if (auto status = node.name_.assign(name); !status) return status;
            if (auto status = lifetime_.observe(key); !status)
            { node.name_ = {}; return status; }
            node.mount_ = this; node.inode_ = inode; node.lifetime_key_ = key;
            return static_cast<Node *>(&node);
        }
        return Status::no_memory("mounted metadata node capacity");
    }
    const Ext4MountedPolicy policy_;
    Ext4MountedReclaimWorkspace *reclaim_workspace_{};
    Ext4MountedStartupWorkspace *startup_workspace_{};
    CommonStorage *common_storage_{};
    MountedNode *pending_orphan_{};
    u32 orphan_seconds_{}, orphan_nanoseconds_{};
    Owner owner_{};
    typename Owner::StartupReadView startup_reader_{owner_};
    typename Owner::StartupOrdinaryView startup_ordinary_reader_{owner_};
    Reader reader_{this};
    View view_{};
    Ext4RecoveryCompletion<8, 2> inspection_{};
    Ext4RecoveryInfo binding_{};
    MetadataOwnerReport identity_{};
    std::array<MountedNode, 128> nodes_{};
    Lifetime lifetime_{};
    std::array<ReferenceHold, 512> reference_holds_{};
    MountedNode *mutation_node_{};
    Ext4InodeLifetimeKey mutation_key_{};
    Lease *mutation_lease_{};
    const NodeMutationContext *mutation_context_{};
    MountedRootRetentionDomain root_retention_{};
    bool legacy_exposure_{};
    Ext4MountReport report_{};
    mutable smp::SpinLock lock_{};
};
} // namespace ok::fs
