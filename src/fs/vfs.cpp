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
    if (data_ != nullptr)
    {
        *data_ = {};
    }
    child_count_ = 0;
    return Status::success();
}

Status RamNode::attach_child(RamNode &child)
{
    smp::ScopedSpinLock guard(lock_);
    if (metadata_.type != NodeType::directory)
    {
        return Status::invalid_argument("parent is not a directory");
    }
    if (child_count_ >= children_.size())
    {
        return Status::overflow("directory child capacity exceeded");
    }
    children_[child_count_++] = &child;
    if (child.metadata_.type == NodeType::directory)
    {
        ++metadata_.link_count;
    }
    return Status::success();
}

Status RamNode::detach_child(std::string_view child)
{
    smp::ScopedSpinLock guard(lock_);
    if (metadata_.type != NodeType::directory)
    {
        return Status::invalid_argument("parent is not a directory");
    }
    for (usize i = 0; i < child_count_; ++i)
    {
        if (children_[i]->name() == child)
        {
            if (children_[i]->metadata_.type == NodeType::directory && metadata_.link_count > 2)
            {
                --metadata_.link_count;
            }
            for (usize j = i; j + 1 < child_count_; ++j)
            {
                children_[j] = children_[j + 1];
            }
            children_[child_count_ - 1] = nullptr;
            --child_count_;
            return Status::success();
        }
    }
    return Status::not_found("directory child not found");
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
    if (offset > metadata_.size)
    {
        return Status::invalid_argument("read offset beyond file size");
    }
    if (data_ == nullptr)
    {
        return Status::invalid_argument("node has no file storage");
    }
    const usize available = metadata_.size - offset;
    const usize length = count < available ? count : available;
    FileBuffer out{};
    out.size = length;
    for (usize i = 0; i < length; ++i)
    {
        out.data[i] = data_->data[offset + i];
    }
    return out;
}

Status RamNode::write(usize offset, std::span<const std::byte> data)
{
    smp::ScopedSpinLock guard(lock_);
    if (metadata_.type != NodeType::regular && metadata_.type != NodeType::device &&
        metadata_.type != NodeType::symlink)
    {
        return Status::invalid_argument("node is not writable");
    }
    if (data_ == nullptr)
    {
        return Status::invalid_argument("node has no file storage");
    }
    if (offset + data.size() > data_->data.size())
    {
        return Status::overflow("file data capacity exceeded");
    }
    if (offset == 0)
    {
        metadata_.size = data.size();
        metadata_.blocks = blocks_for_size(metadata_.size);
        data_->size = metadata_.size;
    }
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

Node *RamNode::lookup(std::string_view child)
{
    smp::ScopedSpinLock guard(lock_);
    if (metadata_.type != NodeType::directory)
    {
        return nullptr;
    }
    for (usize i = 0; i < child_count_; ++i)
    {
        if (children_[i]->name() == child)
        {
            return children_[i];
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
        if (auto status = entry.name.assign(children_[i]->name()); !status.ok())
        {
            return status;
        }
        entry.metadata = children_[i]->metadata();
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
    auto *node = allocate_node(leaf.view(), type);
    if (node == nullptr)
    {
        return Status::overflow("RAM VFS node pool exhausted");
    }
    return parent->attach_child(*node);
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
    return parent->detach_child(leaf.view());
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
    auto *ram_node = static_cast<RamNode *>(node);
    auto listing = ram_node->list();
    if (!listing)
    {
        return listing.status();
    }
    if (listing.value().count != 0)
    {
        return Status::busy("directory is not empty");
    }
    return parent->detach_child(leaf.view());
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
    auto *ram_node = static_cast<RamNode *>(node);
    return ram_node->list();
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

Node *VirtualFileSystem::lookup(std::string_view path)
{
    smp::ScopedSpinLock guard(lock_);
    return lookup_unlocked(path);
}

Node *VirtualFileSystem::lookup_unlocked(std::string_view path)
{
    if (path.empty() || path == "/")
    {
        return root_;
    }
    RamNode *current = root_;
    std::array<RamNode *, max_child_nodes + 1> stack{};
    usize depth = 0;
    if (current != nullptr)
    {
        stack[depth++] = current;
    }
    usize cursor = 0;
    std::string_view segment;
    while (next_segment(path, cursor, segment))
    {
        if (segment == ".")
        {
            continue;
        }
        if (segment == "..")
        {
            if (depth > 1)
            {
                --depth;
                current = stack[depth - 1];
            }
            continue;
        }
        auto *next = current == nullptr ? nullptr : current->lookup(segment);
        current = static_cast<RamNode *>(next);
        if (current == nullptr)
        {
            return nullptr;
        }
        if (depth < stack.size())
        {
            stack[depth++] = current;
        }
    }
    return current;
}

RamNode *VirtualFileSystem::allocate_node(std::string_view name, NodeType type)
{
    if (used_nodes_ >= nodes_.size())
    {
        return nullptr;
    }
    auto *buffer = allocate_file_buffer(type);
    if ((type == NodeType::regular || type == NodeType::device || type == NodeType::symlink) && buffer == nullptr)
    {
        return nullptr;
    }
    auto &node = nodes_[used_nodes_++];
    if (!node.configure(name, type, buffer).ok())
    {
        release_file_buffer(buffer);
        --used_nodes_;
        return nullptr;
    }
    return &node;
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

RamNode *VirtualFileSystem::parent_for(std::string_view path, FixedString<max_path_segment> &leaf)
{
    if (path.empty() || path == "/")
    {
        return nullptr;
    }

    RamNode *current = root_;
    usize cursor = 0;
    std::string_view segment;
    std::string_view previous;
    while (next_segment(path, cursor, segment))
    {
        previous = segment;
        const usize saved = cursor;
        std::string_view probe;
        if (!next_segment(path, cursor, probe))
        {
            static_cast<void>(leaf.assign(previous));
            return current;
        }
        cursor = saved;
        auto *next = current == nullptr ? nullptr : current->lookup(segment);
        current = static_cast<RamNode *>(next);
        if (current == nullptr)
        {
            return nullptr;
        }
    }
    return nullptr;
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
