#include "ok/fs/vfs.hpp"

namespace ok::fs
{

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

Status RamNode::configure(std::string_view name, NodeType type, FileBuffer *data)
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
    if (auto status = configure(name, NodeType::regular, nullptr); !status.ok())
        return status;
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
    return !used_ || (metadata_.link_count == 0 && references_ == 0);
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
    if (data_ == nullptr || metadata_.type == NodeType::directory)
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

Node *RamNode::lookup(std::string_view child)
{
    smp::ScopedSpinLock guard(lock_);
    if (metadata_.type != NodeType::directory)
    {
        return nullptr;
    }
    for (usize i = 0; i < child_count_; ++i)
    {
        if (children_[i].name.view() == child)
        {
            return children_[i].node;
        }
    }
    return nullptr;
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

Status RamNode::chown(u32 uid, u32 gid)
{
    smp::ScopedSpinLock guard(lock_);
    metadata_.uid = uid;
    metadata_.gid = gid;
    return Status::success();
}

Result<DirectoryListing> RamNode::list() const
{
    smp::ScopedSpinLock guard(lock_);
    if (metadata_.type != NodeType::directory)
    {
        return Status::invalid_argument("node is not a directory");
    }
    DirectoryListing listing{};
    for (usize i = 0; i < child_count_; ++i)
    {
        auto &entry = listing.entries[listing.count++];
        if (auto status = entry.name.assign(children_[i].name.view()); !status.ok())
        {
            return status;
        }
        entry.metadata = children_[i].node->metadata();
    }
    return listing;
}

VirtualFileSystem::VirtualFileSystem()
{
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

RamNode *VirtualFileSystem::ram_node(Node *node)
{
    for (auto &candidate : nodes_)
        if (&candidate == node)
            return &candidate;
    return nullptr;
}

Status VirtualFileSystem::mount(std::string_view path, Node &root)
{
    smp::ScopedSpinLock guard(lock_);
    if (path.empty() || path.front() != '/' || (path.size() > 1 && path.back() == '/') ||
        root.metadata().type != NodeType::directory)
        return Status::invalid_argument("invalid mount root");
    usize cursor = 0;
    std::string_view segment;
    while (next_segment(path, cursor, segment))
        if (segment == "." || segment == "..")
            return Status::invalid_argument("mount path must be canonical");
    if (path.find("//") != std::string_view::npos)
        return Status::invalid_argument("mount path must be canonical");
    auto *covered = lookup_unlocked(path);
    if (!covered || covered->metadata().type != NodeType::directory)
        return Status::not_found("mount point is not a directory");
    for (auto &entry : mounts_)
        if (entry.root && entry.path.view() == path)
            return Status::busy("mount point is already mounted");
    for (auto &entry : mounts_)
        if (!entry.root)
        {
            if (auto status = entry.path.assign(path); !status.ok())
                return status;
            entry.root = &root;
            entry.covered = covered;
            return Status::success();
        }
    return Status::no_memory("mount table full");
}

Status VirtualFileSystem::create(std::string_view path, NodeType type)
{
    smp::ScopedSpinLock guard(lock_);
    FixedString<max_path_segment> leaf;
    auto *parent = parent_for(path, leaf);
    if (parent == nullptr || leaf.empty())
    {
        return Status::not_found("parent path not found");
    }
    if (parent->lookup(leaf.view()) != nullptr)
    {
        return Status::already_exists("node already exists");
    }
    auto *ram_parent = ram_node(parent);
    if (!ram_parent)
        return parent->create(leaf.view(), type);
    auto *node = allocate_node(leaf.view(), type);
    if (node == nullptr)
    {
        return Status::overflow("RAM VFS node pool exhausted");
    }
    auto status = ram_parent->attach_child(*node);
    if (!status.ok())
        node->metadata_.link_count = 0;
    return status;
}

Status VirtualFileSystem::create_readonly_file(std::string_view path, std::span<const std::byte> data, u32 mode,
                                               bool replace)
{
    smp::ScopedSpinLock guard(lock_);
    FixedString<max_path_segment> leaf;
    auto *parent = parent_for(path, leaf);
    if (parent == nullptr || leaf.empty())
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
}

Status VirtualFileSystem::link(std::string_view oldpath, std::string_view newpath)
{
    smp::ScopedSpinLock guard(lock_);
    usize source_mount{}, destination_mount{};
    auto *source = lookup_unlocked(oldpath, &source_mount);
    if (!source)
        return Status::not_found("hard link source is missing");
    if (source->metadata().type == NodeType::directory)
        return Status::permission("directories cannot be hard linked");
    FixedString<max_path_segment> leaf;
    auto *parent = parent_for(newpath, leaf, &destination_mount);
    if (!parent)
        return Status::not_found("hard link parent is missing");
    if (parent->metadata().type != NodeType::directory)
        return Status::not_directory("hard link parent is not a directory");
    if (newpath.size() > 1 && newpath.back() == '/')
        return Status::not_directory("hard link destination has a trailing slash");
    if (source_mount != destination_mount || source->filesystem_identity() != parent->filesystem_identity())
        return Status::cross_device("hard link crosses mounts");
    return parent->link(leaf.view(), *source);
}
Status VirtualFileSystem::rename(std::string_view oldpath, std::string_view newpath)
{
    smp::ScopedSpinLock guard(lock_);
    FixedString<max_path_segment> old_name, new_name;
    usize source_mount{}, destination_mount{};
    auto *old_parent = parent_for(oldpath, old_name, &source_mount);
    auto *new_parent = parent_for(newpath, new_name, &destination_mount);
    if (!old_parent || !new_parent)
        return Status::not_found("rename parent is missing or root has no parent");
    if (old_parent->metadata().type != NodeType::directory || new_parent->metadata().type != NodeType::directory)
        return Status::not_directory("rename parent is not a directory");
    auto *source = old_parent->lookup(old_name.view());
    if (!source)
        return Status::not_found("rename source is missing");
    auto *target = new_parent->lookup(new_name.view());
    const bool directory = source->metadata().type == NodeType::directory;
    if (!directory && ((oldpath.size() > 1 && oldpath.back() == '/') || (newpath.size() > 1 && newpath.back() == '/')))
        return Status::not_directory("file rename path has a trailing slash");
    if (source_mount != destination_mount || old_parent->filesystem_identity() != new_parent->filesystem_identity())
        return Status::cross_device("rename crosses mounts");
    FixedString<256> source_path, target_path;
    static_cast<void>(lookup_unlocked(oldpath, nullptr, &source_path));
    if (target)
        static_cast<void>(lookup_unlocked(newpath, nullptr, &target_path));
    for (const auto &mount : mounts_)
    {
        if (!mount.root)
            continue;
        const auto path = mount.path.view();
        const auto within = [&](std::string_view parent) {
            return !parent.empty() && path.starts_with(parent) && path.size() > parent.size() &&
                   path[parent.size()] == '/';
        };
        if (mount.covered == source || mount.covered == target || mount.root == source || mount.root == target ||
            (directory && within(source_path.view())) || (target && within(target_path.view())))
            return Status::busy("rename of a mount point or its ancestor is unavailable");
    }
    return old_parent->rename(old_name.view(), *new_parent, new_name.view());
}

Status VirtualFileSystem::unlink(std::string_view path)
{
    smp::ScopedSpinLock guard(lock_);
    FixedString<max_path_segment> leaf;
    auto *parent = parent_for(path, leaf);
    if (parent == nullptr || leaf.empty())
    {
        return Status::not_found("parent path not found");
    }
    auto *node = parent->lookup(leaf.view());
    if (node == nullptr)
    {
        return Status::not_found("path not found");
    }
    if (node->metadata().type == NodeType::directory)
    {
        return Status::invalid_argument("cannot unlink directory");
    }
    auto *ram_parent = ram_node(parent);
    return ram_parent ? ram_parent->detach_child(leaf.view()) : parent->remove(leaf.view(), false);
}

Status VirtualFileSystem::rmdir(std::string_view path)
{
    smp::ScopedSpinLock guard(lock_);
    FixedString<max_path_segment> leaf;
    auto *parent = parent_for(path, leaf);
    if (parent == nullptr || leaf.empty())
    {
        return Status::not_found("parent path not found");
    }
    auto *node = parent->lookup(leaf.view());
    if (node == nullptr)
    {
        return Status::not_found("path not found");
    }
    if (node->metadata().type != NodeType::directory)
    {
        return Status::invalid_argument("path is not a directory");
    }
    for (const auto &mount : mounts_)
        if (mount.root && mount.covered == node)
            return Status::busy("directory is a mount point");
    auto listing = node->list();
    if (!listing)
    {
        return listing.status();
    }
    if (listing.value().count != 0)
    {
        return Status::busy("directory is not empty");
    }
    auto *ram_parent = ram_node(parent);
    return ram_parent ? ram_parent->detach_child(leaf.view()) : parent->remove(leaf.view(), true);
}

Status VirtualFileSystem::chmod(std::string_view path, u32 mode)
{
    smp::ScopedSpinLock guard(lock_);
    auto *node = lookup_unlocked(path);
    if (node == nullptr)
    {
        return Status::not_found("path not found");
    }
    return node->chmod(mode);
}

Status VirtualFileSystem::chown(std::string_view path, u32 uid, u32 gid)
{
    smp::ScopedSpinLock guard(lock_);
    auto *node = lookup_unlocked(path);
    if (node == nullptr)
    {
        return Status::not_found("path not found");
    }
    return node->chown(uid, gid);
}

Status VirtualFileSystem::write_file(std::string_view path, std::span<const std::byte> data)
{
    smp::ScopedSpinLock guard(lock_);
    auto *node = lookup_unlocked(path);
    if (node == nullptr)
    {
        return Status::not_found("path not found");
    }
    if (auto status = node->truncate(0); !status.ok())
        return status;
    return node->write(0, data);
}

Result<FileBuffer> VirtualFileSystem::read_file(std::string_view path)
{
    smp::ScopedSpinLock guard(lock_);
    auto *node = lookup_unlocked(path);
    if (node == nullptr)
    {
        return Status::not_found("path not found");
    }
    const auto metadata = node->metadata();
    return node->read(0, metadata.size);
}

Result<DirectoryListing> VirtualFileSystem::list(std::string_view path)
{
    smp::ScopedSpinLock guard(lock_);
    auto *node = lookup_unlocked(path);
    if (node == nullptr)
    {
        return Status::not_found("path not found");
    }
    return node->list();
}

Result<Metadata> VirtualFileSystem::stat(std::string_view path)
{
    smp::ScopedSpinLock guard(lock_);
    auto *node = lookup_unlocked(path);
    if (node == nullptr)
    {
        return Status::not_found("path not found");
    }
    return node->metadata();
}

Node *VirtualFileSystem::lookup(std::string_view path, usize *mount_id)
{
    smp::ScopedSpinLock guard(lock_);
    return lookup_unlocked(path, mount_id);
}

Result<FixedString<96>> VirtualFileSystem::directory_path(Node &directory, usize mount_id)
{
    smp::ScopedSpinLock guard(lock_);
    if (directory.metadata().type != NodeType::directory)
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
        usize actual_mount = automatic_mount_id;
        if (lookup_unlocked(path.view(), &actual_mount) != &directory || actual_mount != expected_mount)
            return Status::not_found("directory is covered or unreachable");
        return path;
    };
    if (mount_id != automatic_mount_id)
    {
        if (mount_id == 0)
            return from_root(*root_, "/", 0);
        if (mount_id > mounts_.size() || !mounts_[mount_id - 1].root)
            return Status::not_found("directory mount is unavailable");
        const auto &mount = mounts_[mount_id - 1];
        return from_root(*mount.root, mount.path.view(), mount_id);
    }
    auto result = from_root(*root_, "/", 0);
    if (result)
        return result;
    for (usize i = 0; i < mounts_.size(); ++i)
        if (const auto &mount = mounts_[i]; mount.root)
        {
            auto candidate = from_root(*mount.root, mount.path.view(), i + 1);
            if (candidate)
                return candidate;
            if (candidate.status().code() != StatusCode::not_found &&
                candidate.status().code() != StatusCode::unsupported)
                result = candidate;
        }
    return result.status().code() == StatusCode::unsupported
               ? Status::not_found("directory is not reachable in the namespace")
               : result;
}

Node *VirtualFileSystem::lookup_unlocked(std::string_view path, usize *mount_id, FixedString<256> *resolved_path)
{
    Node *current = root_;
    usize current_mount = 0;
    for (usize i = 0; i < mounts_.size(); ++i)
        if (mounts_[i].root && mounts_[i].path.view() == "/")
        {
            current = mounts_[i].root;
            current_mount = i + 1;
        }
    std::array<usize, max_child_nodes + 1> mount_stack{};
    mount_stack[0] = current_mount;
    std::array<Node *, max_child_nodes + 1> stack{};
    std::array<usize, max_child_nodes> starts{};
    std::array<char, 256> canonical{};
    stack[0] = current;
    usize length = 0, depth = 0, cursor = 0;
    std::string_view segment;
    while (next_segment(path, cursor, segment))
    {
        // Resolve every component before '..'; missing files and regular files
        // cannot be skipped by lexical normalization, including across mounts.
        if (!current || current->metadata().type != NodeType::directory)
            return nullptr;
        if (segment == ".")
            continue;
        if (segment == "..")
        {
            if (depth)
            {
                length = starts[--depth];
                current = stack[depth];
                current_mount = mount_stack[depth];
            }
            continue;
        }
        if (depth == starts.size() || segment.size() + 1 > canonical.size() - length)
            return nullptr;
        current = current->lookup(segment);
        if (!current)
            return nullptr;
        starts[depth] = length;
        canonical[length++] = '/';
        for (char c : segment)
            canonical[length++] = c;
        const std::string_view resolved(canonical.data(), length);
        for (usize i = 0; i < mounts_.size(); ++i)
            if (mounts_[i].root && mounts_[i].path.view() == resolved)
            {
                current = mounts_[i].root;
                current_mount = i + 1;
            }
        stack[++depth] = current;
        mount_stack[depth] = current_mount;
    }
    if (path.size() > 1 && path.back() == '/' && (!current || current->metadata().type != NodeType::directory))
        return nullptr;
    if (mount_id)
        *mount_id = current_mount;
    if (resolved_path)
        static_cast<void>(
            resolved_path->assign(length ? std::string_view(canonical.data(), length) : std::string_view("/")));
    return current;
}

RamNode *VirtualFileSystem::allocate_node(std::string_view name, NodeType type, bool readonly)
{
    for (auto &node : nodes_)
    {
        if (!node.reusable())
            continue;
        release_file_buffer(node.data_);
        node.data_ = nullptr;
        auto *buffer = readonly ? nullptr : allocate_file_buffer(type);
        if (!readonly && type != NodeType::directory && !buffer)
            return nullptr;
        if (!node.configure(name, type, buffer).ok())
        {
            release_file_buffer(buffer);
            return nullptr;
        }
        node.owner_ = this;
        return &node;
    }
    return nullptr;
}

FileBuffer *VirtualFileSystem::allocate_file_buffer(NodeType type)
{
    if (type == NodeType::directory)
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
    while (path.size() > 1 && path.back() == '/')
        path.remove_suffix(1);
    const auto slash = path.find_last_of('/');
    const auto name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    if (name.empty() || name == "." || name == ".." || !leaf.assign(name).ok())
        return nullptr;
    return lookup_unlocked(slash == std::string_view::npos || slash == 0 ? "/" : path.substr(0, slash), mount_id);
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
