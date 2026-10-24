#include "ok/fs/vfs.hpp"
#include "ok/fs/archive_directory_profile.hpp"
#include "ok/fs/mode_policy.hpp"

#include <utility>

namespace ok::fs
{

namespace
{
Status preserve_operation(Status operation, Status cleanup)
{
    return operation.ok() ? cleanup : operation;
}

template <typename T> Result<T> preserve_operation(Result<T> operation, Status cleanup)
{
    if (!operation) return operation.status();
    if (!cleanup) return cleanup;
    return operation;
}

Status operation_status(Status operation)
{
    return operation;
}

template <typename T> Status operation_status(const Result<T> &operation)
{
    return operation.status();
}

template <typename Resolved, typename Operation>
auto with_retained(Resolved &resolved, Operation &&operation)
{
    auto result = std::forward<Operation>(operation)();
    auto cleanup = resolved.retained->release();
    if constexpr (std::same_as<decltype(result), Status>)
        return preserve_operation(result, cleanup);
    if (auto status = preserve_operation(operation_status(result), cleanup); !status)
        return decltype(result)(status);
    return result;
}

template <typename Operation>
auto with_reference(PathReference &reference, Operation &&operation)
{
    auto result = std::forward<Operation>(operation)();
    auto cleanup = reference.release();
    if constexpr (std::same_as<decltype(result), Status>)
        return preserve_operation(result, cleanup);
    if (auto status = preserve_operation(operation_status(result), cleanup); !status)
        return decltype(result)(status);
    return result;
}

Status transfer_retained(std::optional<PathReference> &current, PathReference incoming)
{
    auto status = current->release();
    if (!status)
    {
        static_cast<void>(incoming.release());
        return status;
    }
    current.emplace(std::move(incoming));
    return {};
}
} // namespace

bool has_access(const Metadata &metadata, Credentials credentials, u32 access)
{
    if ((access & ~(access_read | access_write | access_execute)) != 0)
    {
        return false;
    }
    if (access == 0)
    {
        return true;
    }
    if (credentials.uid == 0)
    {
        return true;
    }

    u32 shift = 0;
    if (credentials.uid == metadata.uid)
    {
        shift = 6;
    }
    else if (credentials.gid == metadata.gid)
    {
        shift = 3;
    }

    const auto permissions = (metadata.mode >> shift) & 07u;
    return (permissions & access) == access;
}

Status require_access(const Metadata &metadata, Credentials credentials, u32 access)
{
    return has_access(metadata, credentials, access) ? Status::success()
                                                     : Status::denied("filesystem permission denied");
}

SocketPathReference::~SocketPathReference()
{
    static_cast<void>(release());
}

Status SocketPathReference::release()
{
    auto *owner = std::exchange(owner_, nullptr);
    const auto handle = std::exchange(handle_, PathHandle{});
    return owner ? owner->release_socket_reference(handle) : Status::success();
}

Status RamNode::configure(std::string_view name, NodeType type, FileBuffer *data)
{
    smp::ScopedSpinLock guard(lock_);
    if (borrowed_identity_exposed_ || references_ || (used_ && metadata_.link_count))
        return Status::busy("RAM node identity remains linked, exposed or held");
    return configure_locked(name, type, data);
}

Status RamNode::configure_locked(std::string_view name, NodeType type, FileBuffer *data)
{
    if (auto status = name_.assign(name); !status.ok())
    {
        return status;
    }
    metadata_ = Metadata{
        .type = type,
        .size = 0,
        .mode = mode_for(type, type == NodeType::directory ? 0755u : 0644u),
        .uid = default_uid,
        .gid = default_gid,
        .link_count = type == NodeType::directory ? 2u : 1u,
        .block_size = metadata_block_size,
        .blocks = 0,
    };
    used_ = true;
    data_ = data;
    readonly_ = false;
    readonly_data_ = {};
    if (data_ != nullptr)
    {
        *data_ = {};
    }
    child_count_ = 0;
    references_ = 0;
    parent_ = nullptr;
    children_ = {};
    return Status::success();
}

Status RamNode::configure_readonly(std::string_view name, std::span<const std::byte> data, u32 mode)
{
    smp::ScopedSpinLock guard(lock_);
    if (used_ && (references_ || metadata_.type != NodeType::regular || metadata_.link_count != 1))
        return Status::busy("RAM readonly replacement requires an unshared closed live file");
    // Replacing this live file's backing preserves its published inode identity.
    // Reinitializing an unlinked exposed slot is forbidden by the guard above.
    const auto attributes = metadata_.attributes;
    if (auto status = configure_locked(name, NodeType::regular, nullptr); !status.ok())
        return status;
    metadata_.attributes = attributes;
    readonly_ = true;
    readonly_data_ = data;
    metadata_.size = data.size();
    metadata_.mode = mode_for(NodeType::regular, mode);
    return Status::success();
}

namespace
{
bool valid_component(std::string_view name)
{
    return !name.empty() && name != "." && name != ".." && name.size() < max_path_segment &&
           name.find('/') == std::string_view::npos && name.find('\0') == std::string_view::npos;
}
} // namespace

usize RamNode::child_index(std::string_view name) const
{
    for (usize i = 0; i < child_count_; ++i)
        if (children_[i].name.view() == name)
            return i;
    return child_count_;
}
void RamNode::erase_child(usize index)
{
    for (usize i = index; i + 1 < child_count_; ++i)
        children_[i] = children_[i + 1];
    children_[--child_count_] = {};
}
bool RamNode::reusable() const
{
    smp::ScopedSpinLock guard(lock_);
    return !borrowed_identity_exposed_ && (!used_ || (metadata_.link_count == 0 && references_ == 0));
}
Status RamNode::retain()
{
    smp::ScopedSpinLock guard(lock_);
    if (!used_ || (!metadata_.link_count && !references_))
        return Status::not_found("RAM node is unlinked and closed");
    if (references_ == ~u32{0})
        return Status::overflow("RAM node reference count exceeded");
    ++references_;
    return Status::success();
}
Status RamNode::release()
{
    smp::ScopedSpinLock guard(lock_);
    if (!references_)
        return Status::invalid_argument("RAM node reference count is zero");
    --references_;
    // Allocation reclaims unlinked, closed inodes while holding the VFS lock.
    return Status::success();
}
Status RamNode::expose_borrowed_identity()
{
    smp::ScopedSpinLock guard(lock_);
    if (!used_) return Status::not_found("RAM node identity is unavailable");
    borrowed_identity_exposed_ = true;
    return {};
}
Status RamNode::attach_child(RamNode &child, std::string_view name)
{
    smp::ScopedSpinLock guard(lock_);
    if (metadata_.type != NodeType::directory)
        return Status::not_directory("parent is not a directory");
    if (name.empty())
        name = child.name();
    if (!valid_component(name) || &child == this)
        return Status::invalid_argument("invalid directory entry");
    if (child_index(name) != child_count_)
        return Status::already_exists("directory entry exists");
    if (child_count_ == children_.size())
        return Status::no_memory("directory child capacity exceeded");
    static_cast<void>(children_[child_count_].name.assign(name));
    children_[child_count_++].node = &child;
    if (child.metadata_.type == NodeType::directory)
    {
        ++metadata_.link_count;
        child.parent_ = this;
    }
    return Status::success();
}
Status RamNode::detach_child(std::string_view name)
{
    smp::ScopedSpinLock guard(lock_);
    if (metadata_.type != NodeType::directory)
        return Status::not_directory("parent is not a directory");
    const auto index = child_index(name);
    if (index == child_count_)
        return Status::not_found("directory child not found");
    auto &child = *children_[index].node;
    {
        smp::ScopedSpinLock child_guard(child.lock_);
        if (child.metadata_.type == NodeType::directory)
        {
            if (child.child_count_)
                return Status::not_empty("directory is not empty");
            --metadata_.link_count;
            child.metadata_.link_count = 0;
            child.parent_ = nullptr;
        }
        else
            --child.metadata_.link_count;
    }
    erase_child(index);
    return Status::success();
}
Status RamNode::link(std::string_view name, Node &target)
{
    if (!owner_ || target.filesystem_identity() != owner_)
        return Status::cross_device("hard link crosses filesystems");
    auto &inode = static_cast<RamNode &>(target);
    if (&inode == this || inode.type() == NodeType::directory)
        return Status::permission("directories cannot be hard linked");
    if (!valid_component(name))
        return Status::invalid_argument("invalid hard link name");
    smp::ScopedSpinLock guard(lock_);
    smp::ScopedSpinLock inode_guard(inode.lock_);
    if (metadata_.type != NodeType::directory)
        return Status::not_directory("destination is not a directory");
    if (!metadata_.link_count || !inode.metadata_.link_count)
        return Status::not_found("hard link references an unlinked node");
    if (child_index(name) != child_count_)
        return Status::already_exists("hard link name exists");
    if (inode.metadata_.link_count == ~u32{0})
        return Status::link_limit("RAM link count exceeded");
    if (child_count_ == children_.size())
        return Status::no_memory("directory child capacity exceeded");
    static_cast<void>(children_[child_count_].name.assign(name));
    children_[child_count_++].node = &inode;
    ++inode.metadata_.link_count;
    return Status::success();
}
Status RamNode::rename(std::string_view old_name, Node &destination, std::string_view new_name)
{
    if (!owner_ || destination.filesystem_identity() != owner_)
        return Status::cross_device("rename crosses filesystems");
    if (!valid_component(old_name) || !valid_component(new_name))
        return Status::invalid_argument("invalid rename component");
    auto &new_parent = static_cast<RamNode &>(destination);
    // Namespace calls are serialized by VirtualFileSystem. Keep both directory
    // locks ordered for readers while the directory entries change together.
    RamNode *first = reinterpret_cast<uptr>(this) < reinterpret_cast<uptr>(&new_parent) ? this : &new_parent;
    RamNode *second = first == this ? &new_parent : this;
    first->lock_.lock();
    if (second != first)
        second->lock_.lock();
    struct Unlock
    {
        RamNode *first, *second;
        ~Unlock()
        {
            if (first != second)
                second->lock_.unlock();
            first->lock_.unlock();
        }
    } unlock{first, second};
    if (metadata_.type != NodeType::directory || new_parent.metadata_.type != NodeType::directory)
        return Status::not_directory("rename parent is not a directory");
    if (!metadata_.link_count || !new_parent.metadata_.link_count)
        return Status::not_found("rename parent was removed");
    auto source_index = child_index(old_name);
    if (source_index == child_count_)
        return Status::not_found("rename source is missing");
    auto *source = children_[source_index].node;
    auto target_index = new_parent.child_index(new_name);
    auto *target = target_index == new_parent.child_count_ ? nullptr : new_parent.children_[target_index].node;
    if (source == target)
        return Status::success(); // POSIX leaves both names when they name one inode.
    const bool directory = source->type() == NodeType::directory;
    if (directory)
        for (auto *ancestor = &new_parent; ancestor; ancestor = ancestor->parent_)
            if (ancestor == source)
                return Status::invalid_argument("cannot move a directory into its descendant");
    if (target)
    {
        if (directory && target->type() != NodeType::directory)
            return Status::not_directory("directory replacement target is not a directory");
        if (!directory && target->type() == NodeType::directory)
            return Status::is_directory("file cannot replace a directory");
        if (directory && target->child_count_)
            return Status::not_empty("replacement directory is not empty");
    }
    else if (&new_parent != this && new_parent.child_count_ == new_parent.children_.size())
        return Status::no_memory("destination directory is full");
    if (target)
    {
        smp::ScopedSpinLock target_guard(target->lock_);
        if (directory)
        {
            target->metadata_.link_count = 0;
            target->parent_ = nullptr;
            --new_parent.metadata_.link_count;
        }
        else
            --target->metadata_.link_count;
        new_parent.erase_child(target_index);
        if (&new_parent == this)
            source_index = child_index(old_name);
    }
    if (&new_parent == this)
        static_cast<void>(children_[source_index].name.assign(new_name));
    else
    {
        auto &entry = new_parent.children_[new_parent.child_count_++];
        static_cast<void>(entry.name.assign(new_name));
        entry.node = source;
        erase_child(source_index);
        if (directory)
        {
            --metadata_.link_count;
            ++new_parent.metadata_.link_count;
            source->parent_ = &new_parent;
        }
    }
    return Status::success();
}

Metadata RamNode::metadata() const
{
    smp::ScopedSpinLock guard(lock_);
    auto out = metadata_;
    out.blocks = blocks_for_size(out.size);
    return out;
}

Result<FileBuffer> RamNode::read(usize offset, usize count) const
{
    smp::ScopedSpinLock guard(lock_);
    if (metadata_.type != NodeType::regular && metadata_.type != NodeType::device &&
        metadata_.type != NodeType::symlink)
    {
        return Status::invalid_argument("node is not readable");
    }
    if (offset >= metadata_.size)
        return FileBuffer{};
    if (data_ == nullptr && !readonly_)
    {
        return Status::invalid_argument("node has no file storage");
    }
    const usize available = metadata_.size - offset;
    const usize requested = count < available ? count : available;
    const usize length = requested < max_file_data ? requested : max_file_data;
    FileBuffer out{};
    out.size = length;
    for (usize i = 0; i < length; ++i)
    {
        out.data[i] = readonly_ ? readonly_data_[offset + i] : data_->data[offset + i];
    }
    return out;
}

Status RamNode::write(usize offset, std::span<const std::byte> data)
{
    smp::ScopedSpinLock guard(lock_);
    if (readonly_)
        return Status::denied("file has read-only archive backing");
    if (metadata_.type != NodeType::regular && metadata_.type != NodeType::device &&
        metadata_.type != NodeType::symlink)
    {
        return Status::invalid_argument("node is not writable");
    }
    if (data_ == nullptr)
    {
        return Status::invalid_argument("node has no file storage");
    }
    if (offset > data_->data.size() || data.size() > data_->data.size() - offset)
    {
        return Status::overflow("file data capacity exceeded");
    }
    if (data.empty())
        return Status::success();
    for (usize i = metadata_.size; i < offset; ++i)
        data_->data[i] = std::byte{0};
    for (usize i = 0; i < data.size(); ++i)
    {
        data_->data[offset + i] = data[i];
    }
    if (offset + data.size() > metadata_.size)
    {
        metadata_.size = offset + data.size();
        metadata_.blocks = blocks_for_size(metadata_.size);
        data_->size = metadata_.size;
    }
    return Status::success();
}

Status RamNode::truncate(usize size)
{
    smp::ScopedSpinLock guard(lock_);
    if (readonly_)
        return Status::denied("read-only file");
    if (data_ == nullptr || (metadata_.type != NodeType::regular && metadata_.type != NodeType::device &&
                             metadata_.type != NodeType::symlink))
        return Status::invalid_argument("node is not a file");
    if (size > max_file_data)
        return Status::overflow("RAM file capacity exceeded");
    for (usize i = metadata_.size; i < size; ++i)
        data_->data[i] = std::byte{0};
    data_->size = metadata_.size = size;
    metadata_.blocks = blocks_for_size(size);
    return Status::success();
}

Result<FixedString<96>> RamNode::directory_path(const Node &root) const
{
    if (root.filesystem_identity() != owner_)
        return Status::not_found("directory belongs to another filesystem");
    if (metadata_.type != NodeType::directory)
        return Status::not_directory("node is not a directory");
    FixedString<96> suffix;
    auto *current = this;
    for (usize depth = 0; depth < max_ram_nodes; ++depth)
    {
        if (!current->metadata_.link_count)
            return Status::not_found("directory has been removed");
        if (current == &root)
            return suffix;
        const auto *parent = current->parent_;
        if (!parent)
            return Status::not_found("directory is outside this root");
        std::string_view name;
        for (usize i = 0; i < parent->child_count_; ++i)
            if (parent->children_[i].node == current)
            {
                name = parent->children_[i].name.view();
                break;
            }
        if (name.empty())
            return Status::not_found("directory has no live parent entry");
        FixedString<96> path;
        if (!path.append('/').ok() || !path.append(name).ok() || !path.append(suffix.view()).ok())
            return Status::overflow("directory path exceeds capacity");
        suffix = path;
        current = parent;
    }
    return Status::invalid_argument("directory ancestry is cyclic");
}

Node *RamNode::find_child(std::string_view child)
{
    if (metadata_.type != NodeType::directory)
    {
        return nullptr;
    }
    if (child == ".")
        return this;
    if (child == "..")
        return parent_ ? parent_ : (name_.view() == "/" ? this : nullptr);
    for (usize i = 0; i < child_count_; ++i)
    {
        if (children_[i].name.view() == child)
        {
            return children_[i].node;
        }
    }
    return nullptr;
}

template <typename Operation>
Result<Node *> RamNode::with_child_identity(std::string_view child, Operation &&operation)
{
    for (;;)
    {
        lock_.lock();
        auto *node = static_cast<RamNode *>(find_child(child));
        if (!node)
        {
            lock_.unlock();
            return Status::not_found("directory entry not found");
        }
        // A child lookup and its '..' lookup may acquire opposite node pairs.
        // Do not wait for the second lock while holding the first; retry the
        // topology lookup after releasing it, so removal can also progress.
        if (node != this && !node->lock_.try_lock())
        {
            lock_.unlock();
            std::atomic_signal_fence(std::memory_order_seq_cst);
            continue;
        }
        struct Unlock
        {
            RamNode *parent, *node;
            ~Unlock()
            {
                if (node != parent) node->lock_.unlock();
                parent->lock_.unlock();
            }
        } unlock{this, node};
        return operation(*node);
    }
}
Node *RamNode::lookup(std::string_view child)
{
    auto found = with_child_identity(child, [](RamNode &node) -> Result<Node *> {
        node.borrowed_identity_exposed_ = true;
        return static_cast<Node *>(&node);
    });
    return found ? found.value() : nullptr;
}
Result<Node *> RamNode::lookup_child_retained(std::string_view child)
{
    return with_child_identity(child, [](RamNode &node) -> Result<Node *> {
        if (!node.used_ || (!node.metadata_.link_count && !node.references_))
            return Status::not_found("RAM node is unlinked and closed");
        if (node.references_ == ~u32{0})
            return Status::overflow("RAM node reference count exceeded");
        ++node.references_; return static_cast<Node *>(&node);
    });
}

Status RamNode::create(std::string_view, NodeType)
{
    return Status::unsupported("create must be routed through VirtualFileSystem");
}

Status RamNode::chmod(u32 mode)
{
    smp::ScopedSpinLock guard(lock_);
    metadata_.mode = node_type_mode(metadata_.type) | (mode & mode_permission_mask);
    return Status::success();
}

ModeChangeResult RamNode::change_mode(u32 mode, user::Credentials caller)
{
    smp::ScopedSpinLock guard(lock_);
    if (!used_ || (!metadata_.link_count && !references_))
        return ModeChangeResult::failure(Status::not_found("RAM node is unlinked and closed"));
    const auto decision = mode_change_policy(metadata_.uid, metadata_.gid, mode, caller);
    if (!decision.authorized)
        return ModeChangeResult::failure(Status::permission("only the owner can chmod a RAM node"));
    metadata_.mode = node_type_mode(metadata_.type) | decision.permissions;
    return ModeChangeResult::applied();
}

Status RamNode::chown(u32 uid, u32 gid)
{
    smp::ScopedSpinLock guard(lock_);
    metadata_.uid = uid;
    metadata_.gid = gid;
    return Status::success();
}

Result<DirectoryListing> RamNode::list(DirectoryReadIntent) const
{
    smp::ScopedSpinLock guard(lock_);
    if (metadata_.type != NodeType::directory)
    {
        return Status::invalid_argument("node is not a directory");
    }
    Result<DirectoryListing> result{std::in_place};
    auto &listing = result.value();
    for (usize i = 0; i < child_count_; ++i)
    {
        auto &entry = listing.entries[listing.count++];
        if (auto status = entry.name.assign(children_[i].name.view()); !status.ok())
        {
            return status;
        }
        entry.metadata = children_[i].node->metadata();
    }
    return result;
}

Result<u64> VirtualFileSystem::enter_operation(OperationContention *contention) const
{
    smp::ScopedSpinLock guard(lock_);
    if (operation_active_)
    {
        if (contention)
        {
            contention->owner_ = this;
            contention->serial_ = operation_serial_;
        }
        return Status::busy("VFS operation requires deferred retry");
    }
    if (operation_serial_ == ~u64{0})
        return Status::overflow("VFS operation serial exhausted");
    operation_active_ = true;
    return ++operation_serial_;
}

void VirtualFileSystem::leave_operation(u64 serial) const
{
    smp::ScopedSpinLock guard(lock_);
    if (!operation_active_ || serial != operation_serial_) return;
    operation_active_ = false;
    for (auto &wait : operation_waits_)
        if (wait.phase == OperationWaitPhase::waiting) wait.phase = OperationWaitPhase::ready;
}

Result<bool> VirtualFileSystem::queue_operation_wait(sched::WaitTicket ticket)
{
    if (!ticket.owner || !ticket.thread || !ticket.generation)
        return Status::invalid_argument("VFS operation wait ticket");
    smp::ScopedSpinLock guard(lock_);
    if (!operation_active_) return false;
    OperationWait *available{};
    for (auto &wait : operation_waits_)
    {
        if (wait.phase == OperationWaitPhase::unused)
        { if (!available) available = &wait; continue; }
        if (wait.ticket.owner == ticket.owner && wait.ticket.thread == ticket.thread)
            return Status::already_exists("VFS thread wait must be drained before rearming");
    }
    if (!available) return Status::no_memory("VFS operation wait capacity");
    *available = {ticket, OperationWaitPhase::waiting};
    return true;
}

Result<sched::WaitTicket> VirtualFileSystem::take_ready_operation_wait()
{
    smp::ScopedSpinLock guard(lock_);
    for (auto &wait : operation_waits_)
        if (wait.phase == OperationWaitPhase::ready && !wait.consumer)
        {
            const auto ticket = wait.ticket; wait = {}; return ticket;
        }
    return Status::not_found("VFS has no ready operation wait");
}

Status VirtualFileSystem::cancel_operation_wait(sched::WaitTicket ticket)
{
    smp::ScopedSpinLock guard(lock_);
    for (auto &wait : operation_waits_)
        if (wait.phase != OperationWaitPhase::unused && wait.ticket == ticket && !wait.consumer)
        { wait = {}; return {}; }
    return Status::not_found("VFS operation wait ticket is stale");
}

Result<bool> VirtualFileSystem::queue_certified_operation_wait(OperationContention &&contention,
                                                               sched::WaitTicket ticket, const void *consumer)
{
    const auto *owner = std::exchange(contention.owner_, nullptr);
    const u64 serial = std::exchange(contention.serial_, 0);
    if (owner != this || !serial || !consumer || !ticket.owner || !ticket.thread || !ticket.generation)
        return Status::denied("certified VFS enqueue has no exact contention owner");
    smp::ScopedSpinLock guard(lock_);
    // A successor operation is not the operation this failed attempt observed.
    if (!operation_active_ || operation_serial_ != serial) return false;
    OperationWait *available{};
    for (auto &wait : operation_waits_)
    {
        if (wait.phase == OperationWaitPhase::unused)
        { if (!available) available = &wait; continue; }
        if (wait.ticket.owner == ticket.owner && wait.ticket.thread == ticket.thread)
            return Status::already_exists("VFS thread wait must be drained before rearming");
    }
    if (!available) return Status::no_memory("VFS operation wait capacity");
    *available = {ticket, OperationWaitPhase::waiting, consumer};
    return true;
}

Result<sched::WaitTicket> VirtualFileSystem::take_ready_certified_operation_wait(const void *consumer)
{
    if (!consumer) return Status::denied("certified VFS drain has no consumer");
    smp::ScopedSpinLock guard(lock_);
    for (auto &wait : operation_waits_)
        if (wait.phase == OperationWaitPhase::ready && wait.consumer == consumer)
        { const auto ticket = wait.ticket; wait = {}; return ticket; }
    return Status::not_found("VFS consumer has no ready operation wait");
}

Status VirtualFileSystem::cancel_certified_operation_wait(sched::WaitTicket ticket, const void *consumer)
{
    if (!consumer) return Status::denied("certified VFS cancel has no consumer");
    smp::ScopedSpinLock guard(lock_);
    for (auto &wait : operation_waits_)
        if (wait.phase != OperationWaitPhase::unused && wait.ticket == ticket && wait.consumer == consumer)
        { wait = {}; return {}; }
    return Status::not_found("certified VFS operation wait ticket is stale");
}

VirtualFileSystem::MetadataAdmission VirtualFileSystem::try_stat_absolute(std::string_view path,
                                                                        Credentials credentials,
                                                                        FinalComponent policy)
{
    if (path.empty() || path.front() != '/' ||
        (policy != FinalComponent::follow && policy != FinalComponent::no_follow))
        return MetadataAdmission(Status::unsupported("private admission stat requires an absolute path"));
    OperationContention contention;
    OperationGuard operation(*this, &contention);
    if (!operation) return MetadataAdmission(operation.status(), std::move(contention));
    const ResolveContext context{.credentials = credentials, .check_search = true};
    auto resolved = resolve_unlocked(context, path, policy);
    if (!resolved) return MetadataAdmission(resolved.status());
    return MetadataAdmission(with_retained(resolved.value(), [&]() -> Result<Metadata> {
        auto *node = resolved.value().object.node;
        auto value = node->query_metadata();
        if (!value) return value.status();
        complete_metadata(*node, value.value());
        return value;
    }));
}

VirtualFileSystem::VirtualFileSystem()
{
    static_cast<void>(identities_.admit(this));
    root_ = allocate_node("/", NodeType::directory);
    auto *dev = allocate_node("dev", NodeType::directory);
    auto *tmp = allocate_node("tmp", NodeType::directory);
    if (root_ != nullptr && dev != nullptr)
    {
        static_cast<void>(root_->attach_child(*dev));
    }
    if (root_ != nullptr && tmp != nullptr)
    {
        static_cast<void>(root_->attach_child(*tmp));
        static_cast<void>(tmp->chmod(01777));
    }
}

VirtualFileSystem::~VirtualFileSystem()
{
    OperationGuard operation(*this);
    if (!operation) return;
    for (auto &entry : mounts_)
        if (entry.phase == MountPhase::owned_ram)
            static_cast<void>(retire_ram_tree(*static_cast<RamNode *>(entry.root)));
    // Destruction is externally serialized. Remove the entire namespace before
    // callbacks, then retire later attachments before their covered providers.
    {
        smp::ScopedSpinLock guard(lock_);
        for (auto &entry : mounts_)
        {
            if (entry.attached()) entry.phase = MountPhase::detached;
            entry.root = entry.covered = nullptr;
        }
    }
    for (usize i = mounts_.size(); i; --i)
    {
        auto &entry = mounts_[i - 1];
        if (entry.root_reference)
        {
            static_cast<void>(entry.root_reference->release());
            entry.root_reference.reset();
        }
        if (entry.ram_root_reference)
        {
            static_cast<void>(entry.ram_root_reference->release());
            entry.ram_root_reference.reset();
        }
        if (entry.covered_reference)
        {
            static_cast<void>(entry.covered_reference->release());
            entry.covered_reference.reset();
        }
    }
}

RamNode *VirtualFileSystem::ram_node(Node *node)
{
    for (auto &candidate : nodes_)
        if (&candidate == node)
            return &candidate;
    return nullptr;
}

Status VirtualFileSystem::mount_borrowed(std::string_view path, Node &root)
{
    OperationGuard operation(*this);
    if (!operation) return operation.status();
    auto root_metadata = root.query_metadata();
    if (!root_metadata)
        return root_metadata.status();
    if (path.empty() || path.front() != '/' || (path.size() > 1 && path.back() == '/') ||
        root_metadata.value().type != NodeType::directory)
        return Status::invalid_argument("invalid mount root");
    usize cursor = 0;
    std::string_view segment;
    while (next_segment(path, cursor, segment))
        if (segment == "." || segment == "..")
            return Status::invalid_argument("mount path must be canonical");
    if (path.find("//") != std::string_view::npos)
        return Status::invalid_argument("mount path must be canonical");
    auto resolved = resolve_unlocked({}, path, FinalComponent::no_symlinks);
    if (!resolved)
        return resolved.status();
    return with_retained(resolved.value(), [&]() -> Status {
    const auto covered_mount_id = resolved.value().object.mount_id;
    auto *covered = resolved.value().object.node;
    auto covered_metadata = covered->query_metadata();
    if (!covered_metadata)
        return covered_metadata.status();
    if (covered_metadata.value().type != NodeType::directory)
        return Status::not_found("mount point is not a directory");
    for (auto &entry : mounts_)
        if (entry.attached() && entry.path.view() == path)
            return Status::busy("mount point is already mounted");
    for (auto &entry : mounts_)
        if (entry.phase == MountPhase::unused)
        {
            if (auto status = entry.path.assign(path); !status.ok())
                return status;
            // Capacity and nonwrapping identity admission precede namespace publication.
            if (const auto *provider = root.filesystem_identity())
                if (auto identity = identities_.admit(provider); !identity) return identity.status();
            // A supported provider records permanent raw publication even when
            // this Node came from a retained path. Unsupported providers keep
            // their explicit external-lifetime compatibility contract.
            if (auto status = root.retain(); !status) return status;
            PathReference root_hold({&root, 0});
            if (auto status = with_reference(root_hold, [&]() -> Status {
                    auto exposed = root.expose_borrowed_identity();
                    return exposed.code() == StatusCode::unsupported ? Status::success() : exposed;
                }); !status) return status;
            {
                smp::ScopedSpinLock guard(lock_);
                entry.root = &root;
                entry.covered = covered;
                entry.covered_mount_id = covered_mount_id;
                entry.phase = MountPhase::borrowed;
            }
            return Status::success();
        }
    return Status::no_memory("mount table full");
    });
}

Result<VfsMountHandle> VirtualFileSystem::mount_owned(std::string_view path, MountedRootReference root)
{
    OperationGuard operation(*this);
    // Parameter destruction follows local guard destruction. Consume explicitly
    // so every fallible cleanup still belongs to this operation's admission.
    auto finish_root = [&](Result<VfsMountHandle> result) -> Result<VfsMountHandle> {
        if (root.original_anchor_) return preserve_operation(std::move(result), root.release());
        return result;
    };
    if (!operation) return finish_root(operation.status());
    auto node = root.node_for_mount();
    if (!node) return finish_root(node.status());
    auto root_metadata = node.value()->query_metadata();
    if (!root_metadata) return finish_root(root_metadata.status());
    if (path.empty() || path.front() != '/' || (path.size() > 1 && path.back() == '/') ||
        root_metadata.value().type != NodeType::directory)
        return finish_root(Status::invalid_argument("invalid mount root"));
    usize cursor = 0;
    std::string_view segment;
    while (next_segment(path, cursor, segment))
        if (segment == "." || segment == "..")
            return finish_root(Status::invalid_argument("mount path must be canonical"));
    if (path.find("//") != std::string_view::npos)
        return finish_root(Status::invalid_argument("mount path must be canonical"));
    auto resolved = resolve_unlocked({}, path, FinalComponent::no_symlinks);
    if (!resolved) return finish_root(resolved.status());
    auto attach = [&]() -> Result<VfsMountHandle> {
        auto *covered = resolved.value().object.node;
        auto covered_metadata = covered->query_metadata();
        if (!covered_metadata) return covered_metadata.status();
        if (covered_metadata.value().type != NodeType::directory)
            return Status::not_found("mount point is not a directory");
        for (const auto &entry : mounts_)
            if (entry.attached() && entry.path.view() == path)
                return Status::busy("mount point is already mounted");
        usize index = 0;
        while (index < mounts_.size() && mounts_[index].phase != MountPhase::unused) ++index;
        if (index == mounts_.size()) return Status::no_memory("mount table full");
        FixedString<256> mount_path;
        if (auto status = mount_path.assign(path); !status) return status;
        if (const auto *provider = node.value()->filesystem_identity())
            if (auto identity = identities_.admit(provider); !identity) return identity.status();
        if (auto status = root.validate_for_mount(); !status) return status;
        {
            smp::ScopedSpinLock guard(lock_);
            auto &entry = mounts_[index];
            entry.path = mount_path;
            entry.root = node.value();
            entry.covered = covered;
            entry.covered_mount_id = resolved.value().object.mount_id;
            entry.root_reference.emplace(std::move(root));
            entry.covered_reference.emplace(std::move(*resolved.value().retained));
            entry.phase = MountPhase::owned;
        }
        return VfsMountHandle(*this, index + 1);
    };
    auto result = attach();
    auto cleanup = resolved.value().retained->release();
    return finish_root(preserve_operation(std::move(result), cleanup));
}

Result<VfsMountHandle> VirtualFileSystem::mount_ram_owned(std::string_view path, u32 mode, u32 uid, u32 gid)
{
    OperationGuard operation(*this);
    if (!operation) return operation.status();
    if (path.empty() || path.front() != '/' || (path.size() > 1 && path.back() == '/') ||
        path.find("//") != std::string_view::npos || (mode & ~mode_permission_mask))
        return Status::invalid_argument("invalid RAM mount path or permissions");
    usize cursor = 0;
    std::string_view segment;
    while (next_segment(path, cursor, segment))
        if (segment == "." || segment == "..")
            return Status::invalid_argument("RAM mount path must be canonical");
    auto resolved = resolve_unlocked({}, path, FinalComponent::no_symlinks);
    if (!resolved) return resolved.status();
    return with_retained(resolved.value(), [&]() -> Result<VfsMountHandle> {
        auto *covered = resolved.value().object.node;
        auto metadata = covered->query_metadata();
        if (!metadata) return metadata.status();
        if (metadata.value().type != NodeType::directory)
            return Status::not_directory("RAM mount point is not a directory");
        if (!metadata.value().link_count)
            return Status::not_found("RAM mount point was removed");
        for (const auto &entry : mounts_)
            if (entry.attached() && entry.path.view() == path)
                return Status::busy("RAM mount point is already mounted");
        usize index = 0;
        while (index < mounts_.size() && mounts_[index].phase != MountPhase::unused) ++index;
        if (index == mounts_.size()) return Status::no_memory("mount table full");
        FixedString<256> mount_path;
        if (auto status = mount_path.assign(path); !status) return status;
        auto *root = allocate_node("/", NodeType::directory);
        if (!root) return Status::no_memory("RAM mount node pool exhausted");
        static_cast<void>(root->chmod(mode));
        static_cast<void>(root->chown(uid, gid));
        if (auto status = root->retain(); !status)
        {
            root->metadata_.link_count = 0;
            return status;
        }
        PathReference root_hold({root, index + 1});
        // The root retain and all fallible provider work precede publication.
        {
            smp::ScopedSpinLock guard(lock_);
            auto &entry = mounts_[index];
            entry.path = mount_path;
            entry.root = root;
            entry.covered = covered;
            entry.covered_mount_id = resolved.value().object.mount_id;
            entry.ram_root_reference.emplace(std::move(root_hold));
            entry.covered_reference.emplace(std::move(*resolved.value().retained));
            entry.phase = MountPhase::owned_ram;
            ++ram_mounts_;
        }
        return VfsMountHandle(*this, index + 1);
    });
}

Status VirtualFileSystem::retire_ram_tree(RamNode &root)
{
    std::array<RamNode *, max_ram_nodes> directories{};
    usize count = 1;
    directories[0] = &root;
    // Validate the bounded tree before removing any entry. Namespace admission
    // excludes topology mutation; no external provider callback is involved.
    for (usize index = 0; index < count; ++index)
    {
        auto &directory = *directories[index];
        smp::ScopedSpinLock guard(directory.lock_);
        if (directory.metadata_.type != NodeType::directory)
            return Status::fault("RAM mount root is not a directory");
        for (usize child = 0; child < directory.child_count_; ++child)
        {
            auto *node = ram_node(directory.children_[child].node);
            if (!node) return Status::unsupported("RAM mount contains a foreign node");
            if (node->metadata_.type != NodeType::directory) continue;
            for (usize prior = 0; prior < count; ++prior)
                if (directories[prior] == node)
                    return Status::fault("RAM mount directory topology is cyclic or aliased");
            if (count == directories.size()) return Status::overflow("RAM mount directory capacity exceeded");
            directories[count++] = node;
        }
    }
    while (count)
    {
        auto &directory = *directories[--count];
        while (directory.child_count_)
            if (auto status = directory.detach_child(directory.children_[0].name.view()); !status) return status;
    }
    smp::ScopedSpinLock guard(root.lock_);
    root.metadata_.link_count = 0;
    root.parent_ = nullptr;
    return {};
}

Status VirtualFileSystem::preflight_owned_mounts(std::span<const OwnedMountExpectation> expected) const
{
    smp::ScopedSpinLock guard(lock_);
    if (operation_active_)
        return Status::busy("mount inventory has an active namespace operation");
    if (expected.size() > mounts_.size())
        return Status::invalid_argument("mount inventory exceeds attachment capacity");
    for (usize index = 0; index < expected.size(); ++index)
    {
        const auto &requirement = expected[index];
        const auto mount = requirement.attachment;
        if (mount.owner_ != this || !mount.id_ || mount.id_ > mounts_.size())
            return Status::bad_descriptor("mount inventory has a foreign or invalid attachment");
        for (usize prior = 0; prior < index; ++prior)
            if (expected[prior].attachment.id_ == mount.id_)
                return Status::invalid_argument("mount inventory repeats an attachment");
        const auto parent = requirement.covered_mount;
        if ((parent.owner_ || parent.id_) &&
            (parent.owner_ != this || !parent.id_ || parent.id_ > mounts_.size()))
            return Status::bad_descriptor("mount inventory has a foreign or invalid parent");
        const auto &entry = mounts_[mount.id_ - 1];
        if (!entry.attached())
            return Status::bad_descriptor("mount inventory attachment is detached or unavailable");
        if (entry.phase == MountPhase::borrowed)
            return Status::unsupported("mount inventory contains a borrowed attachment");
        if (requirement.kind != OwnedMountKind::provider && requirement.kind != OwnedMountKind::ram)
            return Status::invalid_argument("mount inventory kind is invalid");
        const auto phase = requirement.kind == OwnedMountKind::provider ? MountPhase::owned : MountPhase::owned_ram;
        if (entry.phase != phase || entry.path.view() != requirement.path || entry.covered_mount_id != parent.id_)
            return Status::invalid_argument("mount inventory attachment kind, path or parent differs");
        if (!entry.root || !entry.covered || !entry.covered_reference ||
            entry.covered_reference->handle() != PathHandle{entry.covered, entry.covered_mount_id})
            return Status::fault("mount inventory covered ownership differs");
        if (entry.phase == MountPhase::owned)
        {
            if (!entry.root_reference || !entry.root_reference->valid() || entry.ram_root_reference ||
                entry.root_reference->root_ != entry.root)
                return Status::fault("mount inventory provider ownership differs");
        }
        else if (!entry.ram_root_reference || entry.root_reference ||
                 entry.ram_root_reference->handle() != PathHandle{entry.root, mount.id_})
            return Status::fault("mount inventory RAM ownership differs");
    }
    for (usize index = 0; index < mounts_.size(); ++index)
    {
        if (!mounts_[index].attached()) continue;
        bool declared = false;
        for (const auto &requirement : expected)
            if (requirement.attachment.id_ == index + 1) declared = true;
        if (!declared)
            return Status::unsupported("mount inventory has an undeclared attachment");
    }
    return {};
}

Status VirtualFileSystem::mount_snapshot(u32 slot, MountSnapshot &output) const
{
    static_assert(mount_snapshot_capacity == std::tuple_size<decltype(mounts_)>::value + 1);
    if (slot >= mount_snapshot_capacity)
        return Status::invalid_argument("mount snapshot slot exceeds capacity");
    smp::ScopedSpinLock guard(lock_);
    if (operation_active_)
        return Status::busy("mount snapshot has an active namespace operation");
    MountSnapshot snapshot{};
    snapshot.generation = 1;
    snapshot.slot = slot;
    for (const auto &entry : mounts_)
    {
        switch (entry.phase)
        {
        case MountPhase::unused: break;
        case MountPhase::borrowed:
        case MountPhase::owned:
        case MountPhase::owned_ram: ++snapshot.generation; break;
        case MountPhase::detached: snapshot.generation += 2; break;
        default: return Status::fault("mount snapshot phase is invalid");
        }
    }
    if (!slot)
    {
        snapshot.state = MountSnapshotState::underlying_ram;
        snapshot.path_length = 1;
        snapshot.path[0] = '/';
    }
    else
    {
        const auto &entry = mounts_[slot - 1];
        switch (entry.phase)
        {
        case MountPhase::unused: break;
        case MountPhase::borrowed: snapshot.state = MountSnapshotState::borrowed_provider; break;
        case MountPhase::owned: snapshot.state = MountSnapshotState::owned_provider; break;
        case MountPhase::owned_ram: snapshot.state = MountSnapshotState::owned_ram; break;
        case MountPhase::detached: snapshot.state = MountSnapshotState::detached; break;
        default: return Status::fault("mount snapshot phase is invalid");
        }
        // A failed borrowed attachment can leave a draft path in an unused slot.
        if (entry.phase != MountPhase::unused)
        {
            if (entry.covered_mount_id >= mount_snapshot_capacity)
                return Status::fault("mount snapshot parent is invalid");
            snapshot.parent_id = static_cast<u32>(entry.covered_mount_id);
            const auto path = entry.path.view();
            if (path.empty() || path.size() >= snapshot.path.size())
                return Status::fault("mount snapshot path is invalid");
            snapshot.path_length = static_cast<u32>(path.size());
            for (usize index = 0; index < path.size(); ++index)
                snapshot.path[index] = path[index];
        }
    }
    output = snapshot;
    return {};
}

Status VirtualFileSystem::detach(VfsMountHandle mount)
{
    OperationGuard operation(*this);
    if (!operation) return operation.status();
    if (mount.owner_ != this || !mount.id_ || mount.id_ > mounts_.size())
        return Status::bad_descriptor("mount handle is unavailable");
    auto &entry = mounts_[mount.id_ - 1];
    if (entry.phase != MountPhase::owned && entry.phase != MountPhase::owned_ram)
        return Status::bad_descriptor("mount attachment is not owned or is detached");
    {
        smp::ScopedSpinLock guard(lock_);
        if (entry.socket_references)
            return Status::busy("mount attachment has retained socket paths");
    }
    for (const auto &child : mounts_)
        if (child.attached() && child.covered_mount_id == mount.id_)
            return Status::busy("mount attachment has an attached child");
    const bool ram = entry.phase == MountPhase::owned_ram;
    if (ram)
        if (auto status = retire_ram_tree(*static_cast<RamNode *>(entry.root)); !status) return status;
    std::optional<MountedRootReference> root;
    std::optional<PathReference> ram_root;
    std::optional<PathReference> covered;
    {
        smp::ScopedSpinLock guard(lock_);
        if (ram) ram_root.emplace(std::move(*entry.ram_root_reference));
        else root.emplace(std::move(*entry.root_reference));
        covered.emplace(std::move(*entry.covered_reference));
        entry.root_reference.reset();
        entry.ram_root_reference.reset();
        entry.covered_reference.reset();
        entry.phase = MountPhase::detached;
        entry.root = entry.covered = nullptr;
        if (ram)
        {
            --ram_mounts_;
            if (ram_detaches_ != ~usize{0}) ++ram_detaches_;
        }
    }
    // Removal is permanent even when cleanup fails. Both holds are consumed.
    auto result = ram ? ram_root->release() : root->release();
    const auto released = preserve_operation(result, covered->release());
    if (!released)
    {
        smp::ScopedSpinLock guard(lock_);
        if (mount_cleanup_failure_) mount_cleanup_failure_ = released;
    }
    return released;
}

Status VirtualFileSystem::shutdown_reference_status() const
{
    smp::ScopedSpinLock guard(lock_);
    if (!mount_cleanup_failure_) return mount_cleanup_failure_;
    if (operation_active_ || socket_path_references_)
        return Status::busy("VFS shutdown retains a namespace operation or socket path");
    for (const auto &wait : operation_waits_)
        if (wait.phase != OperationWaitPhase::unused)
            return Status::busy("VFS shutdown retains an operation waiter");
    for (const auto &node : nodes_)
    {
        usize owned{};
        for (const auto &mount : mounts_)
        {
            if (mount.covered_reference && mount.covered_reference->handle().node == &node) ++owned;
            if (mount.ram_root_reference && mount.ram_root_reference->handle().node == &node) ++owned;
        }
        smp::ScopedSpinLock node_guard(node.lock_);
        if (node.references_ != owned)
            return Status::busy("VFS shutdown retains an external RAM path or node owner");
    }
    return {};
}

Status VirtualFileSystem::create(std::string_view path, NodeType type)
{
    return create({}, path, type, type == NodeType::directory ? 0755u : 0644u, default_uid, default_gid);
}

Status VirtualFileSystem::create(const ResolveContext &context, std::string_view path, NodeType type,
                                u32 mode, u32 uid, u32 gid, std::string_view target, bool follow_final)
{
    OperationGuard operation(*this);
    if (!operation) return operation.status();
    auto created = create_unlocked(context, path, type, mode, uid, gid, target, follow_final);
    return created ? created.value().release() : created.status();
}

Result<PathReference> VirtualFileSystem::create_file(const ResolveContext &context, std::string_view path,
                                                   u32 mode, u32 uid, u32 gid, bool follow_final)
{
    OperationGuard operation(*this);
    if (!operation) return operation.status();
    auto created = create_unlocked(context, path, NodeType::regular, mode, uid, gid, {}, follow_final);
    if (!created)
        return created.status();
    return std::move(created).value();
}

Status VirtualFileSystem::admit_socket_reference(PathHandle handle) const
{
    smp::ScopedSpinLock guard(lock_);
    if (socket_path_references_ == ~usize{0})
        return Status::overflow("socket path reference count exceeded");
    if (handle.mount_id)
    {
        if (handle.mount_id > mounts_.size() || !mounts_[handle.mount_id - 1].attached())
            return Status::not_found("socket path mount is unavailable");
        if (mounts_[handle.mount_id - 1].socket_references == ~usize{0})
            return Status::overflow("socket mount reference count exceeded");
    }
    return {};
}

void VirtualFileSystem::add_socket_reference(PathHandle handle)
{
    smp::ScopedSpinLock guard(lock_);
    ++socket_path_references_;
    if (handle.mount_id) ++mounts_[handle.mount_id - 1].socket_references;
}

Status VirtualFileSystem::release_socket_reference(PathHandle handle)
{
    auto *node = ram_node(handle.node);
    if (!node) return Status::fault("socket path does not name this VFS's RAM node");
    smp::ScopedSpinLock guard(lock_);
    if (!socket_path_references_ ||
        (handle.mount_id && (handle.mount_id > mounts_.size() ||
                            !mounts_[handle.mount_id - 1].attached() ||
                            !mounts_[handle.mount_id - 1].socket_references)))
        return Status::fault("socket path attachment reference is unavailable");
    --socket_path_references_;
    if (handle.mount_id) --mounts_[handle.mount_id - 1].socket_references;
    // This token admits only pool-owned RAM nodes. Release is memory-only and
    // cannot reenter namespace operations through an external provider.
    return node->RamNode::release();
}

Result<SocketPathReference> VirtualFileSystem::create_socket(
    const ResolveContext &context, std::string_view path, u32 mode)
{
    OperationGuard operation(*this);
    if (!operation) return operation.status();
    auto created = create_unlocked(context, path, NodeType::socket, mode,
                                   context.credentials.uid, context.credentials.gid, {}, false, true);
    if (!created) return created.status();
    const auto handle = created.value().handle();
    created.value().handle_ = {};
    return SocketPathReference(*this, handle);
}

Result<SocketPathReference> VirtualFileSystem::resolve_socket(
    const ResolveContext &context, std::string_view path)
{
    OperationGuard operation(*this);
    if (!operation) return operation.status();
    auto resolved = resolve_unlocked(context, path, FinalComponent::follow);
    if (!resolved) return resolved.status();
    auto &reference = *resolved.value().retained;
    const auto handle = reference.handle();
    auto metadata = handle.node->query_metadata();
    if (!metadata) return preserve_operation(Result<SocketPathReference>{metadata.status()}, reference.release());
    auto status = [&]() -> Status {
        if (metadata.value().type != NodeType::socket)
            return Status::invalid_argument("pathname is not a socket");
        if (!ram_node(handle.node))
            return Status::unsupported("socket paths require this VFS's RAM namespace");
        if (auto allowed = require_access(metadata.value(), context.credentials, access_write); !allowed)
            return allowed;
        return admit_socket_reference(handle);
    }();
    if (!status) return preserve_operation(Result<SocketPathReference>{status}, reference.release());
    add_socket_reference(handle);
    reference.handle_ = {};
    return SocketPathReference(*this, handle);
}

Result<PathReference> VirtualFileSystem::create_unlocked(const ResolveContext &context, std::string_view path,
                                                        NodeType type, u32 mode, u32 uid, u32 gid,
                                                        std::string_view target, bool follow_final,
                                                        bool socket_reference)
{
    const auto policy = follow_final ? FinalComponent::create_target :
                        type == NodeType::directory ? FinalComponent::create_directory : FinalComponent::parent;
    auto result = resolve_unlocked(context, path, policy);
    if (!result)
        return result.status();
    return with_retained(result.value(), [&]() -> Result<PathReference> {
    const auto &resolved = result.value();
    auto retain_child = [&](Node *node) -> Result<PathReference> {
        if (auto status = node->retain(); !status) return status;
        return PathReference({node, resolved.object.mount_id});
    };
    if (resolved.leaf.empty())
        return Status::already_exists("node already exists");
    auto *parent = resolved.object.node;
    if (resolved.trailing_slash && type != NodeType::directory)
        return Status::not_directory("trailing slash requires a directory");
    auto parent_metadata = parent->query_metadata();
    if (!parent_metadata)
        return parent_metadata.status();
    if (parent_metadata.value().type != NodeType::directory)
        return Status::not_directory("creation parent is not a directory");
    if (!parent_metadata.value().link_count)
        return Status::not_found("creation parent was removed");
    auto existing = parent->lookup_child_retained(resolved.leaf.view());
    if (existing)
    {
        PathReference held({existing.value(), resolved.object.mount_id});
        return Status::already_exists("node already exists");
    }
    if (existing.status().code() != StatusCode::not_found)
        return existing.status();
    if (context.check_search)
        if (auto status = require_access(parent_metadata.value(), context.credentials,
                                         access_write | access_execute); !status.ok())
            return status;
    if (type == NodeType::symlink && (target.empty() || target.size() >= 256 ||
                                     target.find('\0') != std::string_view::npos))
        return Status::invalid_argument("invalid symlink target");
    mode &= mode_permission_mask;
    if (parent_metadata.value().mode & 02000u)
    {
        gid = parent_metadata.value().gid;
        if (type == NodeType::directory)
            mode |= 02000u;
    }
    auto *ram_parent = ram_node(parent);
    if (type == NodeType::socket && !ram_parent)
        return Status::unsupported("socket creation requires this VFS's RAM namespace");
    if (socket_reference)
        if (auto status = admit_socket_reference(resolved.object); !status) return status;
    if (!ram_parent)
    {
        if (type == NodeType::symlink)
        {
            if (auto status = parent->create_symlink(resolved.leaf.view(), target, uid, gid); !status.ok())
                return status;
            auto node = parent->lookup_child_retained(resolved.leaf.view());
            if (!node)
                return node.status();
            return PathReference({node.value(), resolved.object.mount_id});
        }
        auto found = parent->create_child_retained(resolved.leaf.view(), {type, mode, uid, gid});
        if (!found)
            return found.status();
        if (!found.value())
            return Status::io_error("creation provider returned no child");
        return PathReference({found.value(), resolved.object.mount_id});
    }
    auto *node = allocate_node(resolved.leaf.view(), type);
    if (!node)
        return Status::no_memory("RAM VFS node pool exhausted");
    static_cast<void>(node->chmod(mode));
    static_cast<void>(node->chown(uid, gid));
    if (type == NodeType::symlink)
        if (auto status = node->write(0, {reinterpret_cast<const std::byte *>(target.data()), target.size()}); !status.ok())
        {
            node->metadata_.link_count = 0;
            return status;
        }
    if (socket_reference)
    {
        if (auto status = node->retain(); !status)
        {
            node->metadata_.link_count = 0;
            return status;
        }
        PathReference held({node, resolved.object.mount_id});
        if (auto status = ram_parent->attach_child(*node); !status)
        {
            node->metadata_.link_count = 0;
            return preserve_operation(Result<PathReference>{status}, held.release());
        }
        add_socket_reference(held.handle());
        return held;
    }
    auto status = ram_parent->attach_child(*node);
    if (!status.ok())
    {
        node->metadata_.link_count = 0;
        return status;
    }
    return retain_child(node);
    });
}

Status VirtualFileSystem::import_archive_directory(std::string_view path, u32 mode)
{
    OperationGuard operation(*this);
    if (!operation) return operation.status();
    if ((mode & ~boot_archive_directory_mode_mask) || path.size() < 2 || path.size() >= 128 ||
        path.front() != '/' || path.back() == '/' || path.find("//") != std::string_view::npos ||
        path.find('\0') != std::string_view::npos)
        return Status::invalid_argument("invalid archive directory path or permissions");
    usize validation_cursor = 0;
    std::string_view segment;
    while (next_segment(path, validation_cursor, segment))
        if (!valid_component(segment))
            return Status::invalid_argument("invalid archive directory component");

    auto inspect = [&](RamNode *node) -> Result<Metadata> {
        if (!node || node->owner_ != this)
            return Status::denied("archive directory requires owned RAM");
        for (const auto &mount : mounts_)
            if (mount.attached() && mount.covered_mount_id == 0 && mount.covered == node)
                return Status::denied("archive directory path is mounted");
        const auto metadata = node->metadata();
        if (metadata.type != NodeType::directory)
            return Status::not_directory("archive directory collides with another node type");
        if (!metadata.link_count || metadata.uid != default_uid || metadata.gid != default_gid)
            return Status::denied("archive directory requires live root ownership");
        return metadata;
    };
    if (auto root = inspect(root_); !root) return root.status();
    if (auto status = root_->retain(); !status) return status;
    std::optional<PathReference> held;
    held.emplace(PathReference({root_, 0}));
    auto *parent = root_;
    auto import = [&]() -> Status {
        usize cursor = 0;
        while (next_segment(path, cursor, segment))
        {
            const bool final = cursor == path.size();
            // Walk only this pool, so no mount/provider/symlink callback can
            // turn the trusted import into a general namespace mutation.
            auto found = parent->lookup_child_retained(segment);
            if (!found)
            {
                if (found.status().code() != StatusCode::not_found) return found.status();
                if (!final) return Status::not_found("archive directory parent is absent");
                auto *node = allocate_node(segment, NodeType::directory);
                if (!node) return Status::no_memory("RAM VFS node pool exhausted");
                auto status = node->chmod(mode);
                if (status.ok()) status = node->chown(default_uid, default_gid);
                if (status.ok()) status = parent->attach_child(*node);
                if (!status.ok()) node->metadata_.link_count = 0;
                return status;
            }
            PathReference child({found.value(), 0});
            auto *node = ram_node(found.value());
            auto metadata = inspect(node);
            if (!metadata) return preserve_operation(metadata.status(), child.release());
            if (final)
            {
                const auto status = metadata.value().mode == mode_for(NodeType::directory, mode)
                    ? Status::success() : Status::already_exists("archive directory mode collision");
                return preserve_operation(status, child.release());
            }
            if (auto status = transfer_retained(held, std::move(child)); !status) return status;
            parent = node;
        }
        return Status::invalid_argument("archive directory has no final component");
    };
    auto result = import();
    auto cleanup = held->release();
    return preserve_operation(result, cleanup);
}

Status VirtualFileSystem::create_readonly_file(std::string_view path, std::span<const std::byte> data, u32 mode,
                                               bool replace)
{
    OperationGuard operation(*this);
    if (!operation) return operation.status();
    auto resolved = resolve_unlocked({}, path, FinalComponent::parent);
    if (!resolved)
        return resolved.status();
    return with_retained(resolved.value(), [&]() -> Status {
    const auto &leaf = resolved.value().leaf;
    auto *parent = resolved.value().object.node;
    if (leaf.empty())
        return Status::not_found("archive parent path not found");
    auto *ram_parent = ram_node(parent);
    if (!ram_parent)
        return Status::denied("archive requires RAM backing");
    if (auto *existing = parent->lookup(leaf.view()); existing != nullptr)
    {
        if (!replace || existing->metadata().type != NodeType::regular)
            return Status::already_exists("archive path already exists");
        auto *node = static_cast<RamNode *>(existing);
        if (node->references_ || node->metadata_.link_count != 1)
            return Status::busy("archive replacement requires an unshared closed inode");
        auto *old_buffer = node->writable_storage();
        auto status = node->configure_readonly(leaf.view(), data, mode);
        if (status.ok())
            release_file_buffer(old_buffer);
        return status;
    }
    auto *node = allocate_node(leaf.view(), NodeType::regular, true);
    if (!node)
        return Status::no_memory("RAM VFS node pool exhausted");
    auto status = node->configure_readonly(leaf.view(), data, mode);
    if (status.ok())
        status = ram_parent->attach_child(*node);
    if (!status.ok())
        node->metadata_.link_count = 0;
    return status;
    });
}

Status VirtualFileSystem::link(std::string_view oldpath, std::string_view newpath)
{
    return link({}, oldpath, {}, newpath);
}

Status VirtualFileSystem::link(const ResolveContext &source_context, std::string_view oldpath,
                              const ResolveContext &target_context, std::string_view newpath)
{
    OperationGuard operation(*this);
    if (!operation) return operation.status();
    auto source = resolve_unlocked(source_context, oldpath, FinalComponent::no_follow);
    if (!source)
        return source.status();
    return with_retained(source.value(), [&]() -> Status {
    auto metadata = source.value().object.node->query_metadata();
    if (!metadata)
        return metadata.status();
    if (metadata.value().type == NodeType::directory)
        return Status::permission("directories cannot be hard linked");
    auto target = resolve_unlocked(target_context, newpath, FinalComponent::parent);
    if (!target)
        return target.status();
    return with_retained(target.value(), [&]() -> Status {
    const auto destination = target.value().object;
    if (target.value().trailing_slash)
        return Status::not_directory("hard link destination has a trailing slash");
    if (source.value().object.mount_id != destination.mount_id ||
        source.value().object.node->filesystem_identity() != destination.node->filesystem_identity())
        return Status::cross_device("hard link crosses mounts");
    if (auto status = parent_access(target_context, destination); !status.ok())
        return status;
    auto existing = destination.node->lookup_child_retained(target.value().leaf.view());
    if (existing)
    {
        PathReference held({existing.value(), destination.mount_id});
        return Status::already_exists("hard link destination already exists");
    }
    if (existing.status().code() != StatusCode::not_found)
        return existing.status();
    return destination.node->link(target.value().leaf.view(), *source.value().object.node);
    });
    });
}

Status VirtualFileSystem::rename(std::string_view oldpath, std::string_view newpath)
{
    return rename({}, oldpath, {}, newpath);
}

Status VirtualFileSystem::rename(const ResolveContext &source_context, std::string_view oldpath,
                                const ResolveContext &target_context, std::string_view newpath)
{
    OperationGuard operation(*this);
    if (!operation) return operation.status();
    auto from = resolve_unlocked(source_context, oldpath, FinalComponent::parent);
    if (!from)
        return from.status();
    return with_retained(from.value(), [&]() -> Status {
    auto to = resolve_unlocked(target_context, newpath, FinalComponent::parent);
    if (!to)
        return to.status();
    return with_retained(to.value(), [&]() -> Status {
    const auto old_parent = from.value().object, new_parent = to.value().object;
    auto source_found = old_parent.node->lookup_child_retained(from.value().leaf.view());
    if (!source_found)
        return source_found.status();
    PathReference source_hold({source_found.value(), old_parent.mount_id});
    return with_reference(source_hold, [&]() -> Status {
    auto target_found = new_parent.node->lookup_child_retained(to.value().leaf.view());
    if (!target_found && target_found.status().code() != StatusCode::not_found)
        return target_found.status();
    std::optional<PathReference> target_hold;
    if (target_found) target_hold.emplace(PathReference({target_found.value(), new_parent.mount_id}));
    auto operation = [&]() -> Status {
    auto *source = source_found.value();
    auto *target = target_found ? target_found.value() : nullptr;
    auto metadata = source->query_metadata();
    if (!metadata)
        return metadata.status();
    if (metadata.value().type != NodeType::directory &&
        (from.value().trailing_slash || to.value().trailing_slash))
        return Status::not_directory("file rename path has a trailing slash");
    if (old_parent.mount_id != new_parent.mount_id ||
        old_parent.node->filesystem_identity() != new_parent.node->filesystem_identity())
        return Status::cross_device("rename crosses mounts");
    if (auto status = parent_access(source_context, old_parent, source); !status.ok())
        return status;
    if (auto status = parent_access(target_context, new_parent, target); !status.ok())
        return status;
    auto source_protected = protects_mount(*source);
    if (!source_protected) return source_protected.status();
    if (source_protected.value())
        return Status::busy("rename of a mount point or its ancestor is unavailable");
    if (target)
    {
        auto target_protected = protects_mount(*target);
        if (!target_protected) return target_protected.status();
        if (target_protected.value())
            return Status::busy("rename of a mount point or its ancestor is unavailable");
    }
    return old_parent.node->rename(from.value().leaf.view(), *new_parent.node, to.value().leaf.view());
    };
    auto status = operation();
    if (target_hold) status = preserve_operation(status, target_hold->release());
    return status;
    });
    });
    });
}

Status VirtualFileSystem::unlink(std::string_view path)
{
    return remove({}, path, false);
}

Status VirtualFileSystem::rmdir(std::string_view path)
{
    return remove({}, path, true);
}

Status VirtualFileSystem::remove(const ResolveContext &context, std::string_view path, bool directory)
{
    OperationGuard operation(*this);
    if (!operation) return operation.status();
    auto result = resolve_unlocked(context, path, FinalComponent::parent);
    if (!result)
        return result.status();
    return with_retained(result.value(), [&]() -> Status {
    auto &resolved = result.value();
    auto *parent = resolved.object.node;
    auto found = parent->lookup_child_retained(resolved.leaf.view());
    if (!found)
        return found.status();
    PathReference victim_hold({found.value(), resolved.object.mount_id});
    return with_reference(victim_hold, [&]() -> Status {
    auto *node = found.value();
    auto metadata = node->query_metadata();
    if (!metadata)
        return metadata.status();
    if (auto status = parent_access(context, resolved.object, node); !status.ok())
        return status;
    if (resolved.trailing_slash && metadata.value().type != NodeType::directory)
        return Status::not_directory("trailing slash requires a directory");
    if (directory != (metadata.value().type == NodeType::directory))
        return directory ? Status::not_directory("path is not a directory") : Status::is_directory("cannot unlink directory");
    auto protected_mount = protects_mount(*node);
    if (!protected_mount) return protected_mount.status();
    if (protected_mount.value())
        return Status::busy("directory is a mount point");
    if (directory)
    {
        auto listing = node->list(DirectoryReadIntent::inspection);
        if (!listing)
            return listing.status();
        if (listing.value().count)
            return Status::not_empty("directory is not empty");
    }
    auto *ram_parent = ram_node(parent);
    return ram_parent ? ram_parent->detach_child(resolved.leaf.view()) : parent->remove(resolved.leaf.view(), directory);
    });
    });
}

Status VirtualFileSystem::chmod(std::string_view path, u32 mode)
{
    OperationGuard operation(*this);
    if (!operation) return operation.status();
    auto resolved = resolve_unlocked({}, path, FinalComponent::follow);
    if (!resolved)
        return resolved.status();
    return with_retained(resolved.value(), [&]() -> Status {
    auto *node = resolved.value().object.node;
    return node->chmod(mode);
    });
}

Status VirtualFileSystem::chown(std::string_view path, u32 uid, u32 gid)
{
    OperationGuard operation(*this);
    if (!operation) return operation.status();
    auto resolved = resolve_unlocked({}, path, FinalComponent::follow);
    if (!resolved)
        return resolved.status();
    return with_retained(resolved.value(), [&]() -> Status {
    auto *node = resolved.value().object.node;
    return node->chown(uid, gid);
    });
}

Status VirtualFileSystem::write_file(std::string_view path, std::span<const std::byte> data)
{
    OperationGuard operation(*this);
    if (!operation) return operation.status();
    auto resolved = resolve_unlocked({}, path, FinalComponent::follow);
    if (!resolved)
        return resolved.status();
    return with_retained(resolved.value(), [&]() -> Status {
    auto *node = resolved.value().object.node;
    if (auto status = node->truncate(0); !status.ok())
        return status;
    return node->write(0, data);
    });
}

Result<FileBuffer> VirtualFileSystem::read_file(std::string_view path)
{
    OperationGuard operation(*this);
    if (!operation) return operation.status();
    auto resolved = resolve_unlocked({}, path, FinalComponent::follow);
    if (!resolved)
        return resolved.status();
    return with_retained(resolved.value(), [&]() -> Result<FileBuffer> {
    auto *node = resolved.value().object.node;
    auto metadata = node->query_metadata();
    if (!metadata)
        return metadata.status();
    return node->read(0, metadata.value().size);
    });
}

Result<DirectoryListing> VirtualFileSystem::list(std::string_view path)
{
    OperationGuard operation(*this);
    if (!operation) return operation.status();
    auto resolved = resolve_unlocked({}, path, FinalComponent::follow);
    if (!resolved)
        return resolved.status();
    return with_retained(resolved.value(), [&]() -> Result<DirectoryListing> {
    auto *node = resolved.value().object.node;
    return node->list();
    });
}

Result<Metadata> VirtualFileSystem::stat(std::string_view path)
{
    OperationGuard operation(*this);
    if (!operation) return operation.status();
    auto resolved = resolve_unlocked({}, path, FinalComponent::follow);
    if (!resolved)
        return resolved.status();
    return with_retained(resolved.value(), [&]() -> Result<Metadata> {
    auto *node = resolved.value().object.node;
    auto metadata = node->query_metadata();
    if (!metadata) return metadata.status();
    complete_metadata(*node, metadata.value());
    return metadata;
    });
}

void VirtualFileSystem::complete_metadata(const Node &node, Metadata &metadata) const
{
    metadata.attributes.device = {};
    metadata.attributes.valid &= ~static_cast<u32>(Attribute::device);
    if (auto device = identities_.find(node.filesystem_identity()); device)
    {
        metadata.attributes.device = device.value();
        metadata.attributes.mark(Attribute::device);
    }
}

Result<Metadata> VirtualFileSystem::query_metadata(Node &node) const
{
    OperationGuard operation(*this);
    if (!operation) return operation.status();
    auto metadata = node.query_metadata();
    if (!metadata) return metadata.status();
    complete_metadata(node, metadata.value());
    return metadata;
}

Node *VirtualFileSystem::lookup(std::string_view path, usize *mount_id)
{
    OperationGuard operation(*this);
    if (!operation) return nullptr;
    return lookup_unlocked(path, mount_id);
}

Result<FixedString<96>> VirtualFileSystem::directory_path(Node &directory, usize mount_id)
{
    OperationGuard operation(*this);
    if (!operation) return operation.status();
    if (mount_id != automatic_mount_id)
        if (auto status = validate_mount_id(mount_id); !status) return status;
    auto metadata = directory.query_metadata();
    if (!metadata)
        return metadata.status();
    if (metadata.value().type != NodeType::directory)
        return Status::not_directory("node is not a directory");
    auto from_root = [&](Node &root, std::string_view prefix, usize expected_mount) -> Result<FixedString<96>> {
        auto relative = directory.directory_path(root);
        if (!relative)
            return relative.status();
        FixedString<96> path;
        if (prefix != "/" && !path.assign(prefix).ok())
            return Status::overflow("directory path exceeds capacity");
        if (!path.append(relative.value().view()).ok())
            return Status::overflow("directory path exceeds capacity");
        if (path.empty())
            static_cast<void>(path.assign("/"));
        // A newer mount can cover this root or one of its descendants.
        auto actual = resolve_unlocked({}, path.view(), FinalComponent::follow);
        if (!actual)
            return actual.status();
        return with_retained(actual.value(), [&]() -> Result<FixedString<96>> {
        if (actual.value().object.node != &directory || actual.value().object.mount_id != expected_mount)
            return Status::not_found("directory is covered or unreachable");
        return path;
        });
    };
    if (mount_id != automatic_mount_id)
    {
        if (mount_id == 0)
            return from_root(*root_, "/", 0);
        const auto &mount = mounts_[mount_id - 1];
        return from_root(*mount.root, mount.path.view(), mount_id);
    }
    auto result = from_root(*root_, "/", 0);
    if (result)
        return result;
    if (result.status().code() != StatusCode::not_found &&
        result.status().code() != StatusCode::unsupported)
        return result.status();
    for (usize i = 0; i < mounts_.size(); ++i)
        if (const auto &mount = mounts_[i]; mount.attached())
        {
            auto candidate = from_root(*mount.root, mount.path.view(), i + 1);
            if (candidate)
                return candidate;
            if (candidate.status().code() != StatusCode::not_found &&
                candidate.status().code() != StatusCode::unsupported)
                return candidate.status();
        }
    return result.status().code() == StatusCode::unsupported
               ? Status::not_found("directory is not reachable in the namespace")
               : result;
}

PathHandle VirtualFileSystem::root_handle() const
{
    for (usize i = 0; i < mounts_.size(); ++i)
        if (mounts_[i].attached() && mounts_[i].path.view() == "/")
            return {mounts_[i].root, i + 1};
    return {root_, 0};
}

Status VirtualFileSystem::validate_mount_id(usize mount_id) const
{
    if (mount_id && (mount_id > mounts_.size() || !mounts_[mount_id - 1].attached()))
        return Status::not_found("directory mount is unavailable");
    return {};
}

Result<PathReference> VirtualFileSystem::parent_reference(PathHandle directory, PathHandle root)
{
    if (auto status = validate_mount_id(directory.mount_id); !status) return status;
    if (auto status = validate_mount_id(root.mount_id); !status) return status;
    if (!directory.node || !root.node) return Status::not_found("directory root is unavailable");
    if (directory == root)
    {
        if (auto status = directory.node->retain(); !status) return status;
        return PathReference(directory);
    }
    if (directory.mount_id)
    {
        const auto &mount = mounts_[directory.mount_id - 1];
        if (directory.node == mount.root)
            return parent_reference({mount.covered, mount.covered_mount_id}, root);
    }
    auto parent = directory.node->lookup_child_retained("..");
    if (!parent) return parent.status();
    return PathReference({parent.value(), directory.mount_id});
}

Result<VirtualFileSystem::Resolution> VirtualFileSystem::resolve_unlocked(
    const ResolveContext &context, std::string_view path, FinalComponent policy)
{
    if (path.empty())
        return Status::not_found("empty pathname");
    if (path.find('\0') != std::string_view::npos)
        return Status::invalid_argument("pathname contains a null byte");
    // Validate even an unused context side before touching any supplied pointer.
    if (auto status = validate_mount_id(context.root.mount_id); !status) return status;
    if (auto status = validate_mount_id(context.start.mount_id); !status) return status;
    FixedString<256> pending;
    if (auto status = pending.assign(path); !status.ok())
        return Status::name_too_long("pathname exceeds capacity");
    const auto root = context.root.node ? context.root : root_handle();
    auto current = path.front() == '/' || !context.start.node ? root : context.start;
    if (!current.node) return Status::not_found("pathname root unavailable");
    if (auto status = current.node->retain(); !status) return status;
    std::optional<PathReference> current_hold;
    current_hold.emplace(PathReference(current));
    usize cursor = 0, expansions = 0;
    std::string_view segment;
    while (next_segment(pending.view(), cursor, segment))
    {
        if (!current.node)
            return Status::not_directory("path component has no node");
        auto parent_metadata = current.node->query_metadata();
        if (!parent_metadata)
            return parent_metadata.status();
        if (parent_metadata.value().type != NodeType::directory)
            return Status::not_directory("path component is not a directory");
        if (context.check_search)
            if (auto status = require_access(parent_metadata.value(), context.credentials, access_execute); !status.ok())
                return status;
        usize next = cursor;
        while (next < pending.size() && pending.view()[next] == '/')
            ++next;
        const bool final = next == pending.size();
        const bool trailing = final && cursor < pending.size();
        if (final && (policy == FinalComponent::parent ||
                      (policy == FinalComponent::create_directory && segment != "." && segment != "..")))
        {
            if (segment.size() >= max_path_segment)
                return Status::name_too_long("pathname component exceeds capacity");
            if (!valid_component(segment))
                return Status::invalid_argument("invalid final pathname component");
            Resolution result{.object = current, .trailing_slash = trailing,
                              .retained = std::move(current_hold)};
            if (auto status = result.leaf.assign(segment); !status.ok())
                return status;
            return result;
        }
        if (segment == ".")
            continue;
        if (segment == "..")
        {
            auto parent = parent_reference(current, root);
            if (!parent)
                return parent.status();
            auto next = parent.value().handle();
            if (auto status = transfer_retained(current_hold, std::move(parent).value()); !status) return status;
            current = next;
            continue;
        }
        if (segment.size() >= max_path_segment)
            return Status::name_too_long("pathname component exceeds capacity");
        auto found = current.node->lookup_child_retained(segment);
        if (!found)
        {
            if (found.status().code() != StatusCode::not_found)
                return found.status();
            if (final && policy == FinalComponent::create_target)
            {
                Resolution result{.object = current, .trailing_slash = trailing,
                                  .retained = std::move(current_hold)};
                if (auto status = result.leaf.assign(segment); !status.ok())
                    return status;
                return result;
            }
            return Status::not_found("pathname component not found");
        }
        auto *node = found.value();
        PathReference found_hold({node, current.mount_id});
        auto metadata = node->query_metadata();
        if (!metadata)
            return metadata.status();
        if (metadata.value().type == NodeType::symlink && policy == FinalComponent::no_symlinks)
            return Status::too_many_symlinks("mount path must not contain a symlink");
        if (metadata.value().type == NodeType::symlink &&
            (!final || trailing || policy == FinalComponent::follow || policy == FinalComponent::create_target))
        {
            if (++expansions > 40)
                return Status::too_many_symlinks("symlink traversal limit exceeded");
            const auto size = metadata.value().size;
            if (!size)
                return Status::not_found("empty symlink target");
            if (size >= 256)
                return Status::name_too_long("symlink target exceeds pathname capacity");
            auto data = node->read(0, size);
            if (!data)
                return data.status();
            if (data.value().size != size)
                return Status::io_error("incomplete symlink target");
            FixedString<256> expanded;
            const std::string_view target(reinterpret_cast<const char *>(data.value().data.data()), size);
            if (target.find('\0') != std::string_view::npos)
                return Status::invalid_argument("symlink target contains a null byte");
            if (!expanded.assign(target).ok() || !expanded.append(pending.view().substr(cursor)).ok())
                return Status::name_too_long("expanded pathname exceeds capacity");
            if (auto status = found_hold.release(); !status) return status;
            if (target.front() == '/')
            {
                if (auto status = root.node->retain(); !status) return status;
                if (auto status = transfer_retained(current_hold, PathReference(root)); !status) return status;
                current = root;
            }
            pending = expanded;
            cursor = 0;
            continue;
        }
        if (auto status = transfer_retained(current_hold, std::move(found_hold)); !status) return status;
        current.node = node;
        for (usize i = 0; i < mounts_.size(); ++i)
            if (mounts_[i].attached() && mounts_[i].covered == current.node &&
                mounts_[i].covered_mount_id == current.mount_id)
            {
                if (auto status = mounts_[i].root->retain(); !status) return status;
                auto mounted = PathHandle{mounts_[i].root, i + 1};
                if (auto status = transfer_retained(current_hold, PathReference(mounted)); !status) return status;
                current = mounted;
                break;
            }
    }
    if (policy == FinalComponent::parent)
        return Status::invalid_argument("root has no final pathname component");
    if (pending.view().back() == '/')
    {
        auto metadata = current.node->query_metadata();
        if (!metadata)
            return metadata.status();
        if (metadata.value().type != NodeType::directory)
            return Status::not_directory("trailing slash requires a directory");
    }
    return Resolution{.object = current, .retained = std::move(current_hold)};
}

Result<PathReference> VirtualFileSystem::resolve(const ResolveContext &context, std::string_view path,
                                                FinalComponent policy)
{
    OperationGuard operation(*this);
    if (!operation) return operation.status();
    auto resolved = resolve_unlocked(context, path, policy);
    if (!resolved)
        return resolved.status();
    if (!resolved.value().retained)
        return Status::fault("resolved pathname has no retained node");
    return std::move(*resolved.value().retained);
}

Node *VirtualFileSystem::lookup_unlocked(std::string_view path, usize *mount_id)
{
    auto result = resolve_unlocked({}, path, FinalComponent::follow);
    if (!result)
        return nullptr;
    auto *node = result.value().object.node;
    auto exposure = node->expose_borrowed_identity();
    auto cleanup = result.value().retained->release();
    if (!exposure || !cleanup) return nullptr;
    if (mount_id)
        *mount_id = result.value().object.mount_id;
    return node;
}

Status VirtualFileSystem::parent_access(const ResolveContext &context, PathHandle parent, Node *victim)
{
    auto queried = parent.node->query_metadata();
    if (!queried)
        return queried.status();
    const auto metadata = queried.value();
    if (!metadata.link_count)
        return Status::not_found("parent directory was removed");
    if (!context.check_search)
        return Status::success();
    if (auto status = require_access(metadata, context.credentials, access_write | access_execute); !status.ok())
        return status;
    if (victim && (metadata.mode & 01000u) && context.credentials.uid &&
        context.credentials.uid != metadata.uid)
    {
        auto victim_metadata = victim->query_metadata();
        if (!victim_metadata)
            return victim_metadata.status();
        if (context.credentials.uid != victim_metadata.value().uid)
            return Status::permission("sticky directory protects another owner's file");
    }
    return Status::success();
}

Result<bool> VirtualFileSystem::protects_mount(Node &node)
{
    for (const auto &mount : mounts_)
    {
        if (!mount.attached())
            continue;
        auto current = PathHandle{mount.covered, mount.covered_mount_id};
        if (auto status = current.node->retain(); !status) return status;
        std::optional<PathReference> current_hold;
        current_hold.emplace(PathReference(current));
        auto operation = [&]() -> Result<bool> {
        for (usize depth = 0; depth < max_ram_nodes; ++depth)
        {
            if (current.node == &node || mount.root == &node)
                return true;
            auto parent = parent_reference(current, root_handle());
            if (!parent) return parent.status();
            if (parent.value().handle() == current)
            {
                if (auto status = parent.value().release(); !status) return status;
                break;
            }
            auto next = parent.value().handle();
            if (auto status = transfer_retained(current_hold, std::move(parent).value()); !status) return status;
            current = next;
        }
        return false;
        };
        auto operation_result = operation();
        auto cleanup = current_hold->release();
        auto checked = preserve_operation(std::move(operation_result), cleanup);
        if (!checked) return checked.status();
        if (checked.value()) return true;
    }
    return false;
}

RamNode *VirtualFileSystem::allocate_node(std::string_view name, NodeType type, bool readonly)
{
    for (auto &node : nodes_)
    {
        if (!node.reusable())
            continue;
        auto identity = inode_sequence_.next();
        if (!identity) return nullptr;
        release_file_buffer(node.data_);
        node.data_ = nullptr;
        auto *buffer = readonly ? nullptr : allocate_file_buffer(type);
        if (!readonly && type != NodeType::directory && type != NodeType::socket && !buffer)
            return nullptr;
        if (!node.configure(name, type, buffer).ok())
        {
            release_file_buffer(buffer);
            return nullptr;
        }
        node.owner_ = this;
        node.metadata_.attributes.inode = identity.value();
        node.metadata_.attributes.mark(Attribute::inode);
        return &node;
    }
    return nullptr;
}

FileBuffer *VirtualFileSystem::allocate_file_buffer(NodeType type)
{
    if (type == NodeType::directory || type == NodeType::socket)
    {
        return nullptr;
    }
    for (usize i = 0; i < file_buffers_.size(); ++i)
    {
        if (!file_buffer_used_[i])
        {
            file_buffer_used_[i] = true;
            file_buffers_[i] = {};
            return &file_buffers_[i];
        }
    }
    // A reusable directory slot may precede an unlinked file slot. Reclaim
    // closed file storage independently so inode order cannot cause false ENOSPC.
    for (auto &node : nodes_)
        if (node.data_ && node.reusable())
        {
            auto *buffer = node.data_;
            node.data_ = nullptr;
            *buffer = {};
            return buffer; // Its pool slot remains allocated to the new inode.
        }
    return nullptr;
}

void VirtualFileSystem::release_file_buffer(FileBuffer *buffer)
{
    if (buffer == nullptr)
    {
        return;
    }
    for (usize i = 0; i < file_buffers_.size(); ++i)
    {
        if (&file_buffers_[i] == buffer)
        {
            file_buffers_[i] = {};
            file_buffer_used_[i] = false;
            return;
        }
    }
}

Node *VirtualFileSystem::parent_for(std::string_view path, FixedString<max_path_segment> &leaf, usize *mount_id)
{
    auto result = resolve_unlocked({}, path, FinalComponent::parent);
    if (!result)
        return nullptr;
    leaf = result.value().leaf;
    auto *parent = result.value().object.node;
    auto exposure = parent->expose_borrowed_identity();
    auto cleanup = result.value().retained->release();
    if (!exposure || !cleanup) return nullptr;
    if (mount_id)
        *mount_id = result.value().object.mount_id;
    return parent;
}

bool VirtualFileSystem::next_segment(std::string_view path, usize &cursor, std::string_view &segment)
{
    while (cursor < path.size() && path[cursor] == '/')
    {
        ++cursor;
    }
    if (cursor >= path.size())
    {
        return false;
    }
    const usize start = cursor;
    while (cursor < path.size() && path[cursor] != '/')
    {
        ++cursor;
    }
    segment = path.substr(start, cursor - start);
    return true;
}

} // namespace ok::fs
