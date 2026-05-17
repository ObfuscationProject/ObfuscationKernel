#include "ok/fs/vfs.hpp"

#include <algorithm>

namespace ok::fs {

RamNode::RamNode(std::string name, NodeType type)
    : name_(std::move(name)), type_(type)
{
}

Metadata RamNode::metadata() const
{
    return Metadata {.type = type_, .size = data_.size(), .mode = type_ == NodeType::directory ? 0755u : 0644u};
}

Result<std::vector<std::byte>> RamNode::read(usize offset, usize count) const
{
    if (type_ != NodeType::regular && type_ != NodeType::device) {
        return Status::invalid_argument("node is not readable");
    }
    if (offset > data_.size()) {
        return Status::invalid_argument("read offset beyond file size");
    }
    const usize available = data_.size() - offset;
    const usize length = std::min(count, available);
    return std::vector<std::byte>(data_.begin() + static_cast<std::ptrdiff_t>(offset),
                                  data_.begin() + static_cast<std::ptrdiff_t>(offset + length));
}

Status RamNode::write(usize offset, std::span<const std::byte> data)
{
    if (type_ != NodeType::regular && type_ != NodeType::device) {
        return Status::invalid_argument("node is not writable");
    }
    if (offset > data_.size()) {
        data_.resize(offset);
    }
    if (offset + data.size() > data_.size()) {
        data_.resize(offset + data.size());
    }
    std::copy(data.begin(), data.end(), data_.begin() + static_cast<std::ptrdiff_t>(offset));
    return Status::success();
}

Node* RamNode::lookup(std::string_view child)
{
    if (type_ != NodeType::directory) {
        return nullptr;
    }
    auto it = children_.find(std::string(child));
    return it == children_.end() ? nullptr : it->second.get();
}

Status RamNode::create(std::string name, NodeType type)
{
    if (type_ != NodeType::directory) {
        return Status::invalid_argument("parent is not a directory");
    }
    if (children_.contains(name)) {
        return Status::already_exists("node already exists");
    }
    children_.emplace(name, std::make_unique<RamNode>(name, type));
    return Status::success();
}

VirtualFileSystem::VirtualFileSystem()
    : root_(std::make_unique<RamNode>("/", NodeType::directory))
{
    static_cast<void>(root_->create("dev", NodeType::directory));
    static_cast<void>(root_->create("tmp", NodeType::directory));
}

Status VirtualFileSystem::create(std::string_view path, NodeType type)
{
    std::string leaf;
    auto* parent = parent_for(path, leaf);
    if (!parent || leaf.empty()) {
        return Status::not_found("parent path not found");
    }
    return parent->create(std::move(leaf), type);
}

Status VirtualFileSystem::write_file(std::string_view path, std::span<const std::byte> data)
{
    auto* node = lookup(path);
    if (!node) {
        return Status::not_found("path not found");
    }
    return node->write(0, data);
}

Result<std::vector<std::byte>> VirtualFileSystem::read_file(std::string_view path)
{
    auto* node = lookup(path);
    if (!node) {
        return Status::not_found("path not found");
    }
    const auto metadata = node->metadata();
    return node->read(0, metadata.size);
}

Node* VirtualFileSystem::lookup(std::string_view path)
{
    if (path.empty() || path == "/") {
        return root_.get();
    }
    Node* current = root_.get();
    for (const auto& segment : split_path(path)) {
        current = current->lookup(segment);
        if (!current) {
            return nullptr;
        }
    }
    return current;
}

std::vector<std::string> VirtualFileSystem::split_path(std::string_view path) const
{
    std::vector<std::string> segments;
    usize start = 0;
    while (start < path.size()) {
        while (start < path.size() && path[start] == '/') {
            ++start;
        }
        usize end = start;
        while (end < path.size() && path[end] != '/') {
            ++end;
        }
        if (end > start) {
            segments.emplace_back(path.substr(start, end - start));
        }
        start = end;
    }
    return segments;
}

RamNode* VirtualFileSystem::parent_for(std::string_view path, std::string& leaf)
{
    const auto segments = split_path(path);
    if (segments.empty()) {
        return nullptr;
    }

    leaf = segments.back();
    Node* current = root_.get();
    for (usize i = 0; i + 1 < segments.size(); ++i) {
        current = current->lookup(segments[i]);
        if (!current) {
            return nullptr;
        }
    }
    return dynamic_cast<RamNode*>(current);
}

} // namespace ok::fs

