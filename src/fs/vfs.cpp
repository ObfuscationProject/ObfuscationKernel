#include "ok/fs/vfs.hpp"

namespace ok::fs
{

Status RamNode::configure(std::string_view name, NodeType type)
{
    if (auto status = name_.assign(name); !status.ok())
    {
        return status;
    }
    type_ = type;
    used_ = true;
    data_size_ = 0;
    child_count_ = 0;
    return Status::success();
}

Status RamNode::attach_child(RamNode &child)
{
    if (type_ != NodeType::directory)
    {
        return Status::invalid_argument("parent is not a directory");
    }
    if (child_count_ >= children_.size())
    {
        return Status::overflow("directory child capacity exceeded");
    }
    children_[child_count_++] = &child;
    return Status::success();
}

Metadata RamNode::metadata() const
{
    return Metadata{.type = type_, .size = data_size_, .mode = type_ == NodeType::directory ? 0755u : 0644u};
}

Result<FileBuffer> RamNode::read(usize offset, usize count) const
{
    if (type_ != NodeType::regular && type_ != NodeType::device)
    {
        return Status::invalid_argument("node is not readable");
    }
    if (offset > data_size_)
    {
        return Status::invalid_argument("read offset beyond file size");
    }
    const usize available = data_size_ - offset;
    const usize length = count < available ? count : available;
    FileBuffer out{};
    out.size = length;
    for (usize i = 0; i < length; ++i)
    {
        out.data[i] = data_[offset + i];
    }
    return out;
}

Status RamNode::write(usize offset, std::span<const std::byte> data)
{
    if (type_ != NodeType::regular && type_ != NodeType::device)
    {
        return Status::invalid_argument("node is not writable");
    }
    if (offset + data.size() > data_.size())
    {
        return Status::overflow("file data capacity exceeded");
    }
    for (usize i = 0; i < data.size(); ++i)
    {
        data_[offset + i] = data[i];
    }
    if (offset + data.size() > data_size_)
    {
        data_size_ = offset + data.size();
    }
    return Status::success();
}

Node *RamNode::lookup(std::string_view child)
{
    if (type_ != NodeType::directory)
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
    }
}

Status VirtualFileSystem::create(std::string_view path, NodeType type)
{
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

Status VirtualFileSystem::write_file(std::string_view path, std::span<const std::byte> data)
{
    auto *node = lookup(path);
    if (node == nullptr)
    {
        return Status::not_found("path not found");
    }
    return node->write(0, data);
}

Result<FileBuffer> VirtualFileSystem::read_file(std::string_view path)
{
    auto *node = lookup(path);
    if (node == nullptr)
    {
        return Status::not_found("path not found");
    }
    const auto metadata = node->metadata();
    return node->read(0, metadata.size);
}

Node *VirtualFileSystem::lookup(std::string_view path)
{
    if (path.empty() || path == "/")
    {
        return root_;
    }
    RamNode *current = root_;
    usize cursor = 0;
    std::string_view segment;
    while (next_segment(path, cursor, segment))
    {
        auto *next = current == nullptr ? nullptr : current->lookup(segment);
        current = static_cast<RamNode *>(next);
        if (current == nullptr)
        {
            return nullptr;
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
    auto &node = nodes_[used_nodes_++];
    if (!node.configure(name, type).ok())
    {
        --used_nodes_;
        return nullptr;
    }
    return &node;
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
