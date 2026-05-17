#pragma once

#include "ok/core/types.hpp"

#include <cstddef>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ok::fs {

enum class NodeType : u8 {
    directory,
    regular,
    device,
    symlink,
};

struct Metadata {
    NodeType type {NodeType::regular};
    usize size {0};
    u32 mode {0644};
};

class Node {
public:
    virtual ~Node() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual Metadata metadata() const = 0;
    virtual Result<std::vector<std::byte>> read(usize offset, usize count) const = 0;
    virtual Status write(usize offset, std::span<const std::byte> data) = 0;
    virtual Node* lookup(std::string_view child) = 0;
    virtual Status create(std::string name, NodeType type) = 0;
};

class RamNode final : public Node {
public:
    RamNode(std::string name, NodeType type);

    [[nodiscard]] std::string_view name() const override { return name_; }
    [[nodiscard]] Metadata metadata() const override;
    Result<std::vector<std::byte>> read(usize offset, usize count) const override;
    Status write(usize offset, std::span<const std::byte> data) override;
    Node* lookup(std::string_view child) override;
    Status create(std::string name, NodeType type) override;

private:
    std::string name_;
    NodeType type_;
    std::vector<std::byte> data_;
    std::map<std::string, std::unique_ptr<RamNode>> children_;
};

class VirtualFileSystem final {
public:
    VirtualFileSystem();

    Status create(std::string_view path, NodeType type);
    Status write_file(std::string_view path, std::span<const std::byte> data);
    Result<std::vector<std::byte>> read_file(std::string_view path);
    [[nodiscard]] Node* lookup(std::string_view path);

private:
    [[nodiscard]] std::vector<std::string> split_path(std::string_view path) const;
    [[nodiscard]] RamNode* parent_for(std::string_view path, std::string& leaf);

    std::unique_ptr<RamNode> root_;
};

} // namespace ok::fs

