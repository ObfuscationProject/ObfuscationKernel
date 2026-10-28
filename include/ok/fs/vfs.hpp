#pragma once

#include "ok/sched/wait_registration.hpp"

#include "ok/user/user.hpp"

#include "ok/fs/attributes.hpp"
#include "ok/fs/mode_change.hpp"
#include "ok/fs/mount_retention.hpp"
#include "ok/fs/mount_snapshot.hpp"
#include "ok/fs/persistent_sync_profile.hpp"

#include "ok/core/fixed.hpp"
#include "ok/core/types.hpp"
#include "ok/smp/smp.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace ok::fs
{

inline constexpr usize max_path_segment = 32;
inline constexpr usize max_ram_nodes = 128;
inline constexpr usize max_ram_file_buffers = 32;
inline constexpr usize max_child_nodes = 64;
inline constexpr usize max_file_data = 4096;

enum class NodeType : u8
{
    directory,
    regular,
    device,
    symlink,
    socket,
};

enum class FileSystemMode : u8
{
    ram_only,
    ext4_read_only,
    ext4_journaled,
    ext4_read_write,
};

inline constexpr u32 mode_type_mask = 0170000u;
inline constexpr u32 mode_socket = 0140000u;
inline constexpr u32 mode_symlink = 0120000u;
inline constexpr u32 mode_regular = 0100000u;
inline constexpr u32 mode_block_device = 0060000u;
inline constexpr u32 mode_directory = 0040000u;
inline constexpr u32 mode_character_device = 0020000u;
inline constexpr u32 mode_fifo = 0010000u;
inline constexpr u32 mode_permission_mask = 07777u;
inline constexpr u32 default_uid = 0;
inline constexpr u32 default_gid = 0;
inline constexpr u32 metadata_block_size = 512;
inline constexpr u32 access_execute = 1u;
inline constexpr u32 access_write = 2u;
inline constexpr u32 access_read = 4u;

[[nodiscard]] constexpr u32 node_type_mode(NodeType type)
{
    switch (type)
    {
    case NodeType::directory:
        return mode_directory;
    case NodeType::regular:
        return mode_regular;
    case NodeType::device:
        return mode_character_device;
    case NodeType::symlink:
        return mode_symlink;
    case NodeType::socket:
        return mode_socket;
    }
    return mode_regular;
}

[[nodiscard]] constexpr u32 mode_for(NodeType type, u32 permissions)
{
    return node_type_mode(type) | (permissions & mode_permission_mask);
}

[[nodiscard]] constexpr u64 blocks_for_size(usize size)
{
    return static_cast<u64>((size + metadata_block_size - 1) / metadata_block_size);
}

struct Metadata
{
    NodeType type{NodeType::regular};
    usize size{0};
    u32 mode{mode_for(NodeType::regular, 0644u)};
    u32 uid{default_uid};
    u32 gid{default_gid};
    u32 link_count{1};
    u32 block_size{metadata_block_size};
    u64 blocks{0};
    FileAttributes attributes{};
};

struct Credentials
{
    u32 uid{default_uid};
    u32 gid{default_gid};
};

// Final attributes of a new node, before publication.
struct InitialNodeAttributes
{
    NodeType type{NodeType::regular};
    u32 mode{0644u};
    u32 uid{default_uid};
    u32 gid{default_gid};
};

[[nodiscard]] bool has_access(const Metadata &metadata, Credentials credentials, u32 access);
[[nodiscard]] Status require_access(const Metadata &metadata, Credentials credentials, u32 access);

struct FileBuffer
{
    std::array<std::byte, max_file_data> data{};
    usize size{0};
};

struct DirectoryEntry
{
    FixedString<max_path_segment> name{};
    Metadata metadata{};
};

struct DirectoryListing
{
    std::array<DirectoryEntry, max_child_nodes> entries{};
    usize count{0};
};

class VirtualFileSystem;
class FilePageCache;

// Internal topology checks must not be reported as directory access events.
enum class DirectoryReadIntent : u8 { access, inspection };

class Node
{
  public:
    virtual ~Node() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual Metadata metadata() const = 0;
    virtual Result<FileBuffer> read(usize offset, usize count) const = 0;
    virtual Status write(usize offset, std::span<const std::byte> data) = 0;
    virtual Status truncate(usize)
    {
        return Status::unsupported("truncate unavailable");
    }
    virtual Status sync()
    {
        return Status::success();
    }
    // A stronger opt-in storage boundary. RAM and unqualified providers must
    // never turn an ordinary no-op sync into persistent-storage admission.
    virtual Status sync_persistent()
    {
        return Status::unsupported("node has no persistent synchronization contract");
    }
    virtual Status retain()
    {
        return Status::success();
    }
    // Release consumes one successful retain even if deferred cleanup fails.
    virtual Status release()
    {
        return Status::success();
    }
    virtual Result<DirectoryListing> list(DirectoryReadIntent = DirectoryReadIntent::access) const
    {
        return Status::unsupported("directory listing unavailable");
    }
    // At most one listing of entries from position `start` in directory
    // order; a full listing may be followed by more. Providers whose
    // directories can exceed one listing override this; the default slices list().
    virtual Result<DirectoryListing> list_from(usize start, DirectoryReadIntent intent = DirectoryReadIntent::access) const
    {
        auto listing = list(intent);
        if (!listing || !start) return listing;
        auto &value = listing.value();
        const usize kept = start < value.count ? value.count - start : 0;
        for (usize i = 0; i < kept; ++i) value.entries[i] = value.entries[start + i];
        value.count = kept;
        return listing;
    }
    virtual Status remove(std::string_view, bool)
    {
        return Status::unsupported("remove unavailable");
    }
    [[nodiscard]] virtual const void *filesystem_identity() const
    {
        return nullptr;
    }
    // A provider that caches file contents exposes the node-owned page cache;
    // null means uncached I/O through read/write.
    [[nodiscard]] virtual FilePageCache *page_cache()
    {
        return nullptr;
    }
    // Legacy raw pointers carry no releasable lifetime token. Success promises
    // that this object's identity cannot be recycled for the owner's lifetime.
    // Recycling providers must record this exposure atomically while a lookup
    // retain or provider operation still excludes retirement. This callback
    // only records identity state; it must not perform device I/O.
    virtual Status expose_borrowed_identity()
    {
        return Status::unsupported("provider has no stable borrowed identity contract");
    }
    virtual Status link(std::string_view, Node &)
    {
        return Status::unsupported("hard links unavailable");
    }
    virtual Status rename(std::string_view, Node &, std::string_view)
    {
        return Status::unsupported("rename unavailable");
    }
    // Path relative to a filesystem directory root, rebuilt from live entries.
    virtual Result<FixedString<96>> directory_path(const Node &) const
    {
        return Status::unsupported("directory path reconstruction unavailable");
    }
    // Directories expose their actual parent through lookup(".."); filesystem
    // roots return themselves. VFS handles mount crossing and confined roots.
    virtual Node *lookup(std::string_view child) = 0;
    virtual Status create(std::string_view name, NodeType type) = 0;
    // Success returns a stable, provider-owned borrowed node with final initial
    // attributes. Unsupported providers must reject before changing storage;
    // callers must not emulate this with post-publication chmod/chown.
    virtual Result<Node *> create_child(std::string_view, const InitialNodeAttributes &)
    {
        return Status::unsupported("initial child attributes are unavailable");
    }
    // Success transfers one retain with the final initial attributes. A provider
    // that recycles storage must override this creation-to-retain handoff.
    virtual Result<Node *> create_child_retained(std::string_view name,
                                                const InitialNodeAttributes &attributes)
    {
        auto created = create_child(name, attributes);
        if (!created) return created.status();
        if (!created.value()) return Status::io_error("creation provider returned no child");
        if (auto status = created.value()->retain(); !status) return status;
        return created.value();
    }
    // Publish the target and ownership together; callers must not initialize an
    // already visible symlink through a later write/chown operation.
    virtual Status create_symlink(std::string_view, std::string_view, u32, u32)
    {
        return Status::unsupported("filesystem symlink creation is unavailable");
    }
    virtual Status chmod(u32 mode) = 0;
    // Public checked metadata mutation; unsupported providers refuse by default.
    virtual ModeChangeResult change_mode(u32, user::Credentials)
    {
        return ModeChangeResult::unavailable();
    }
    virtual ModeChangeResult change_owner(u32, u32, user::Credentials)
    {
        return ModeChangeResult::unavailable();
    }
    virtual Status chown(u32 uid, u32 gid) = 0;
    // Checked filesystem service operations. Infallible in-memory nodes may use
    // the defaults; I/O-backed nodes must override without discarding errors.
    [[nodiscard]] virtual Result<Metadata> query_metadata() const
    {
        return metadata();
    }
    virtual Result<Node *> lookup_child(std::string_view child)
    {
        auto *node = lookup(child);
        return node ? Result<Node *>{node} : Result<Node *>{Status::not_found("directory entry not found")};
    }
    // Success transfers one retain to the caller. Providers that can recycle
    // node storage concurrently must make lookup and retain one operation.
    virtual Result<Node *> lookup_child_retained(std::string_view child)
    {
        auto found = lookup_child(child);
        if (!found) return found.status();
        if (!found.value()) return Status::io_error("lookup provider returned no child");
        if (auto status = found.value()->retain(); !status) return status;
        return found.value();
    }
};

struct PathHandle
{
    Node *node{nullptr};
    usize mount_id{0};
    bool operator==(const PathHandle &) const = default;
};

struct ResolveContext
{
    PathHandle root{};
    PathHandle start{};
    Credentials credentials{};
    bool check_search{false};
};

enum class FinalComponent : u8
{
    follow,
    no_follow,
    parent,
    create_target,
    no_symlinks,
    create_directory,
};

class PathReference final
{
  public:
    ~PathReference()
    {
        static_cast<void>(release());
    }
    // Consume this reference even when filesystem cleanup fails. Successful
    // service boundaries call this explicitly before publishing their result.
    [[nodiscard]] Status release()
    {
        auto *node = handle_.node;
        handle_ = {};
        return node ? node->release() : Status::success();
    }
    PathReference(const PathReference &) = delete;
    PathReference &operator=(const PathReference &) = delete;
    PathReference(PathReference &&other) noexcept : handle_(other.handle_)
    {
        other.handle_ = {};
    }
    [[nodiscard]] PathHandle handle() const
    {
        return handle_;
    }
    [[nodiscard]] Node *node() const
    {
        return handle_.node;
    }

  private:
    friend class VirtualFileSystem;
    explicit PathReference(PathHandle handle) : handle_(handle) {}
    PathHandle handle_{};
};

// Owns a RAM socket inode and its namespace attachment. The VFS must outlive
// every token; a retained node alone does not keep a mount attached.
class SocketPathReference final
{
  public:
    ~SocketPathReference();
    SocketPathReference(const SocketPathReference &) = delete;
    SocketPathReference &operator=(const SocketPathReference &) = delete;
    SocketPathReference &operator=(SocketPathReference &&) = delete;
    SocketPathReference(SocketPathReference &&other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)), handle_(std::exchange(other.handle_, PathHandle{})) {}
    [[nodiscard]] Node *get() const { return handle_.node; }
    [[nodiscard]] PathHandle handle() const { return handle_; }
    [[nodiscard]] bool valid() const { return owner_ && handle_.node; }
    [[nodiscard]] bool same_identity(const SocketPathReference &other) const
    { return valid() && other.valid() && owner_ == other.owner_ && handle_ == other.handle_; }
    [[nodiscard]] Status release();
  private:
    friend class VirtualFileSystem;
    SocketPathReference(VirtualFileSystem &owner, PathHandle handle) : owner_(&owner), handle_(handle) {}
    VirtualFileSystem *owner_{};
    PathHandle handle_{};
};

// A namespace attachment identity, without ownership of provider storage.
class VfsMountHandle final
{
  public:
    VfsMountHandle() = default;
    [[nodiscard]] bool valid() const { return owner_ && id_; }
    [[nodiscard]] usize id() const { return id_; }
  private:
    friend class VirtualFileSystem;
    VfsMountHandle(const VirtualFileSystem &owner, usize id) : owner_(&owner), id_(id) {}
    const VirtualFileSystem *owner_{};
    usize id_{};
};

enum class OwnedMountKind : u8 { provider, ram };

struct OwnedMountExpectation
{
    VfsMountHandle attachment{};
    std::string_view path{};
    OwnedMountKind kind{OwnedMountKind::provider};
    // The default handle names the underlying RAM namespace.
    VfsMountHandle covered_mount{};
};

class RamNode final : public Node
{
  public:
    RamNode() = default;

    Status configure(std::string_view name, NodeType type, FileBuffer *data);
    Status configure_readonly(std::string_view name, std::span<const std::byte> data, u32 mode);
    Status attach_child(RamNode &child, std::string_view name = {});
    Status detach_child(std::string_view child);

    [[nodiscard]] std::string_view name() const override
    {
        return name_.view();
    }
    [[nodiscard]] Metadata metadata() const override;
    Result<FileBuffer> read(usize offset, usize count) const override;
    Status write(usize offset, std::span<const std::byte> data) override;
    Status truncate(usize size) override;
    Status retain() override;
    Status release() override;
    Status expose_borrowed_identity() override;
    const void *filesystem_identity() const override
    {
        return owner_;
    }
    Status link(std::string_view name, Node &target) override;
    Status rename(std::string_view old_name, Node &new_parent, std::string_view new_name) override;
    Result<FixedString<96>> directory_path(const Node &root) const override;
    Node *lookup(std::string_view child) override;
    Result<Node *> lookup_child_retained(std::string_view child) override;
    Status create(std::string_view name, NodeType type) override;
    Status chmod(u32 mode) override;
    ModeChangeResult change_mode(u32 mode, user::Credentials caller) override;
    Status chown(u32 uid, u32 gid) override;
    [[nodiscard]] Result<DirectoryListing> list(DirectoryReadIntent = DirectoryReadIntent::access) const override;
    [[nodiscard]] FileBuffer *writable_storage() const
    {
        return data_;
    }
    [[nodiscard]] bool used() const
    {
        return used_;
    }
    [[nodiscard]] NodeType type() const
    {
        return metadata_.type;
    }

  private:
    friend class VirtualFileSystem;
    [[nodiscard]] bool reusable() const;
    Status configure_locked(std::string_view name, NodeType type, FileBuffer *data);
    template <typename Operation>
    Result<Node *> with_child_identity(std::string_view child, Operation &&operation);
    [[nodiscard]] usize child_index(std::string_view name) const;
    [[nodiscard]] Node *find_child(std::string_view child);
    void erase_child(usize index);
    struct Child
    {
        FixedString<max_path_segment> name{};
        RamNode *node{nullptr};
    };
    VirtualFileSystem *owner_{nullptr};
    RamNode *parent_{nullptr};
    u32 references_{0};
    bool used_{false};
    bool borrowed_identity_exposed_{false};
    FixedString<max_path_segment> name_{};
    Metadata metadata_{};
    FileBuffer *data_{nullptr};
    std::span<const std::byte> readonly_data_{};
    bool readonly_{false};
    std::array<Child, max_child_nodes> children_{};
    usize child_count_{0};
    mutable smp::SpinLock lock_{};
};

class VirtualFileSystem final
{
  public:
    VirtualFileSystem();
    ~VirtualFileSystem();
    VirtualFileSystem(const VirtualFileSystem &) = delete;
    VirtualFileSystem &operator=(const VirtualFileSystem &) = delete;
    VirtualFileSystem(VirtualFileSystem &&) = delete;
    VirtualFileSystem &operator=(VirtualFileSystem &&) = delete;

    void set_mode(FileSystemMode mode)
    {
        smp::ScopedSpinLock guard(lock_);
        mode_ = mode;
    }
    [[nodiscard]] FileSystemMode mode() const
    {
        smp::ScopedSpinLock guard(lock_);
        return mode_;
    }
    // Borrowed compatibility requires the provider to outlive this VFS.
    Status mount_borrowed(std::string_view path, Node &root);
    Status mount(std::string_view path, Node &root) { return mount_borrowed(path, root); }
    Result<VfsMountHandle> mount_owned(std::string_view path, MountedRootReference root);
    // Publish an empty RAM directory from this VFS's pool over an existing
    // directory. All admission and initial attributes precede publication.
    Result<VfsMountHandle> mount_ram_owned(std::string_view path, u32 mode = 0755u,
                                          u32 uid = default_uid, u32 gid = default_gid);
    // Check the exact attached owner inventory without callbacks or effects.
    // This snapshot grants no admission barrier, owner drain or seal proof;
    // callers must recheck after closing producers and before consuming mounts.
    [[nodiscard]] Status preflight_owned_mounts(std::span<const OwnedMountExpectation> expected) const;
    // Copy one of nine fixed inventory rows without provider callbacks. Slot 0
    // is the underlying RAM namespace; rows 1 through 8 include tombstones.
    // Rejection leaves output unchanged. Observation grants no lifetime or seal.
    [[nodiscard]] Status mount_snapshot(u32 slot, MountSnapshot &output) const;
    // After the producer barrier and scheduler drain: RAM references must be
    // exactly the VFS's own mount holds, including detached namespace history.
    // Provider reference ownership is checked by its original clean seal.
    [[nodiscard]] Status shutdown_reference_status() const;
    Status detach(VfsMountHandle mount);
    Result<PathReference> resolve(const ResolveContext &context, std::string_view path,
                                  FinalComponent policy = FinalComponent::follow);
    Status create(const ResolveContext &context, std::string_view path, NodeType type,
                  u32 mode, u32 uid, u32 gid, std::string_view target = {}, bool follow_final = false);
    Result<PathReference> create_file(const ResolveContext &context, std::string_view path, u32 mode,
                                      u32 uid, u32 gid, bool follow_final);
    Result<SocketPathReference> create_socket(const ResolveContext &context, std::string_view path, u32 mode);
    // Traversal checks and final socket write permission use one captured ID pair.
    Result<SocketPathReference> resolve_socket(const ResolveContext &context, std::string_view path);
    [[nodiscard]] usize socket_path_reference_count() const
    { smp::ScopedSpinLock guard(lock_); return socket_path_references_; }
    [[nodiscard]] usize ram_mount_count() const
    { smp::ScopedSpinLock guard(lock_); return ram_mounts_; }
    [[nodiscard]] usize ram_detach_count() const
    { smp::ScopedSpinLock guard(lock_); return ram_detaches_; }
    Status link(const ResolveContext &source_context, std::string_view source,
                const ResolveContext &target_context, std::string_view target);
    Status rename(const ResolveContext &source_context, std::string_view source,
                  const ResolveContext &target_context, std::string_view target);
    Status remove(const ResolveContext &context, std::string_view path, bool directory);
    Status create(std::string_view path, NodeType type);
    // Exact root-owned boot metadata, before publication. Existing directories
    // must already match; mounted providers and symlink traversal are refused.
    Status import_archive_directory(std::string_view path, u32 mode);
    Status create_readonly_file(std::string_view path, std::span<const std::byte> data, u32 mode = 0555u,
                                bool replace = false);
    Status link(std::string_view oldpath, std::string_view newpath);
    Status rename(std::string_view oldpath, std::string_view newpath);
    Status unlink(std::string_view path);
    Status rmdir(std::string_view path);
    Status chmod(std::string_view path, u32 mode);
    Status chown(std::string_view path, u32 uid, u32 gid);
    Status write_file(std::string_view path, std::span<const std::byte> data);
    Result<FileBuffer> read_file(std::string_view path);
    Result<DirectoryListing> list(std::string_view path);
    Result<Metadata> stat(std::string_view path);
    // Caller retains node identity through this checked snapshot.
    Result<Metadata> query_metadata(Node &node) const;
    [[nodiscard]] Node *lookup(std::string_view path, usize *mount_id = nullptr);
    // Zero is the underlying RAM namespace; mounted roots have stable nonzero IDs.
    // Automatic selection is for inspection only. Directory handles retain an ID.
    static constexpr usize automatic_mount_id = ~usize{0};
    Result<FixedString<96>> directory_path(Node &directory, usize mount_id = automatic_mount_id);

    // A certificate proves only that admission failed before provider effects.
    // Moving transfers the one enqueue attempt; it never grants an operation.
    class OperationContention final
    {
      public:
        OperationContention() = default;
        OperationContention(const OperationContention &) = delete;
        OperationContention &operator=(const OperationContention &) = delete;
        OperationContention(OperationContention &&other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), serial_(std::exchange(other.serial_, 0)) {}
        OperationContention &operator=(OperationContention &&) = delete;
        [[nodiscard]] bool valid() const { return owner_ && serial_; }
      private:
        friend class VirtualFileSystem;
        OperationContention(const VirtualFileSystem &owner, u64 serial) : owner_(&owner), serial_(serial) {}
        const VirtualFileSystem *owner_{};
        u64 serial_{};
    };
    struct MetadataAdmission
    {
        Result<Metadata> result;
        OperationContention contention;
        explicit MetadataAdmission(Result<Metadata> value) : result(std::move(value)) {}
        MetadataAdmission(Result<Metadata> value, OperationContention pending)
            : result(std::move(value)), contention(std::move(pending)) {}
    };
    // One read-only request spans resolve/query/checked release. No ambient
    // permit lets a provider's reentrant call borrow this operation.
    [[nodiscard]] MetadataAdmission try_stat_absolute(std::string_view path, Credentials credentials,
                                                      FinalComponent policy = FinalComponent::follow);
    [[nodiscard]] Result<bool> queue_certified_operation_wait(OperationContention &&contention,
                                                             sched::WaitTicket ticket, const void *consumer);
    [[nodiscard]] Result<sched::WaitTicket> take_ready_certified_operation_wait(const void *consumer);
    [[nodiscard]] Status cancel_certified_operation_wait(sched::WaitTicket ticket, const void *consumer);

    // Optional restart adapter: arm the scheduler's exact wait first, enqueue,
    // and complete immediately when false is returned. Drain ready tokens via
    // SchedulerState::complete_wait after dropping every VFS memory lock.
    // This notification queue does not park a kernel stack or restart a syscall.
    static constexpr usize operation_wait_capacity = 16;
    [[nodiscard]] Result<bool> queue_operation_wait(sched::WaitTicket ticket);
    [[nodiscard]] Result<sched::WaitTicket> take_ready_operation_wait();
    [[nodiscard]] Status cancel_operation_wait(sched::WaitTicket ticket);

  private:
    friend class SocketPathReference;
    // One namespace operation spans callbacks, but its admission memory lock
    // never does. Synchronous contention returns busy, including reentrancy.
    class OperationGuard final
    {
      public:
        explicit OperationGuard(const VirtualFileSystem &owner, OperationContention *contention = nullptr)
            : owner_(owner), admission_(owner.enter_operation(contention)) {}
        OperationGuard(const OperationGuard &) = delete;
        OperationGuard &operator=(const OperationGuard &) = delete;
        ~OperationGuard() { if (admission_) owner_.leave_operation(admission_.value()); }
        [[nodiscard]] explicit operator bool() const { return bool(admission_); }
        [[nodiscard]] Status status() const { return admission_.status(); }
      private:
        const VirtualFileSystem &owner_;
        Result<u64> admission_;
    };
    [[nodiscard]] Result<u64> enter_operation(OperationContention *contention = nullptr) const;
    void leave_operation(u64 serial) const;
    enum class OperationWaitPhase : u8 { unused, waiting, ready };
    struct OperationWait
    {
        sched::WaitTicket ticket{};
        OperationWaitPhase phase{OperationWaitPhase::unused};
        const void *consumer{};
    };
    struct Resolution
    {
        PathHandle object{};
        FixedString<max_path_segment> leaf{};
        bool trailing_slash{false};
        std::optional<PathReference> retained{};
    };
    Result<PathReference> create_unlocked(const ResolveContext &context, std::string_view path, NodeType type,
                                          u32 mode, u32 uid, u32 gid, std::string_view target, bool follow_final,
                                          bool socket_reference = false);
    [[nodiscard]] Status admit_socket_reference(PathHandle handle) const;
    void add_socket_reference(PathHandle handle);
    [[nodiscard]] Status release_socket_reference(PathHandle handle);
    [[nodiscard]] Status retire_ram_tree(RamNode &root);
    void complete_metadata(const Node &node, Metadata &metadata) const;
    [[nodiscard]] Status validate_mount_id(usize mount_id) const;
    [[nodiscard]] PathHandle root_handle() const;
    [[nodiscard]] Result<PathReference> parent_reference(PathHandle directory, PathHandle root);
    [[nodiscard]] Result<Resolution> resolve_unlocked(const ResolveContext &context, std::string_view path,
                                                     FinalComponent policy);
    [[nodiscard]] Status parent_access(const ResolveContext &context, PathHandle parent, Node *victim = nullptr);
    [[nodiscard]] Result<bool> protects_mount(Node &node);
    [[nodiscard]] Node *lookup_unlocked(std::string_view path, usize *mount_id = nullptr);
    [[nodiscard]] RamNode *allocate_node(std::string_view name, NodeType type, bool readonly = false);
    [[nodiscard]] FileBuffer *allocate_file_buffer(NodeType type);
    void release_file_buffer(FileBuffer *buffer);
    [[nodiscard]] Node *parent_for(std::string_view path, FixedString<max_path_segment> &leaf,
                                   usize *mount_id = nullptr);
    [[nodiscard]] static bool next_segment(std::string_view path, usize &cursor, std::string_view &segment);

    [[nodiscard]] RamNode *ram_node(Node *node);
    enum class MountPhase : u8 { unused, borrowed, owned, owned_ram, detached };
    struct Mount
    {
        [[nodiscard]] bool attached() const
        { return phase == MountPhase::borrowed || phase == MountPhase::owned || phase == MountPhase::owned_ram; }
        MountPhase phase{MountPhase::unused};
        FixedString<256> path{};
        Node *root{nullptr};
        Node *covered{nullptr};
        usize covered_mount_id{0};
        std::optional<PathReference> covered_reference{};
        std::optional<MountedRootReference> root_reference{};
        std::optional<PathReference> ram_root_reference{};
        usize socket_references{};
    };
    FilesystemIdentities<9> identities_{};
    IdentitySequence<> inode_sequence_{};
    FileSystemMode mode_{FileSystemMode::ram_only};
    std::array<RamNode, max_ram_nodes> nodes_{};
    std::array<FileBuffer, max_ram_file_buffers> file_buffers_{};
    std::array<bool, max_ram_file_buffers> file_buffer_used_{};
    RamNode *root_{nullptr};
    usize socket_path_references_{}, ram_mounts_{}, ram_detaches_{};
    mutable u64 operation_serial_{};
    mutable bool operation_active_{};
    Status mount_cleanup_failure_{};
    mutable std::array<OperationWait, operation_wait_capacity> operation_waits_{};
    mutable smp::SpinLock lock_{};
    // Mount fallbacks run while RAM nodes, provider-facing locks and admission
    // bookkeeping still exist. External provider storage must outlive the VFS.
    std::array<Mount, 8> mounts_{};
};

} // namespace ok::fs
