#pragma once

#include "ok/core/fixed.hpp"
#include "ok/core/types.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace ok::fs
{

inline constexpr usize max_path_segment = 32;
inline constexpr usize max_ram_nodes = 128;
inline constexpr usize max_child_nodes = 16;
inline constexpr usize max_file_data = 4096;

enum class NodeType : u8
{
    directory,
    regular,
    device,
    symlink,
};

enum class FileSystemMode : u8
{
    ram_only,
    ext4_read_only,
    ext4_journaled,
};

struct Metadata
{
    NodeType type{NodeType::regular};
    usize size{0};
    u32 mode{0644};
};

struct FileBuffer
{
    std::array<std::byte, max_file_data> data{};
    usize size{0};
};

class Node
{
  public:
    virtual ~Node() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual Metadata metadata() const = 0;
    virtual Result<FileBuffer> read(usize offset, usize count) const = 0;
    virtual Status write(usize offset, std::span<const std::byte> data) = 0;
    virtual Node *lookup(std::string_view child) = 0;
    virtual Status create(std::string_view name, NodeType type) = 0;
};

class RamNode final : public Node
{
  public:
    RamNode() = default;

    Status configure(std::string_view name, NodeType type);
    Status attach_child(RamNode &child);

    [[nodiscard]] std::string_view name() const override
    {
        return name_.view();
    }
    [[nodiscard]] Metadata metadata() const override;
    Result<FileBuffer> read(usize offset, usize count) const override;
    Status write(usize offset, std::span<const std::byte> data) override;
    Node *lookup(std::string_view child) override;
    Status create(std::string_view name, NodeType type) override;
    [[nodiscard]] bool used() const
    {
        return used_;
    }
    [[nodiscard]] NodeType type() const
    {
        return type_;
    }

  private:
    bool used_{false};
    FixedString<max_path_segment> name_{};
    NodeType type_{NodeType::regular};
    std::array<std::byte, max_file_data> data_{};
    usize data_size_{0};
    std::array<RamNode *, max_child_nodes> children_{};
    usize child_count_{0};
};

class VirtualFileSystem final
{
  public:
    VirtualFileSystem();

    void set_mode(FileSystemMode mode)
    {
        mode_ = mode;
    }
    [[nodiscard]] FileSystemMode mode() const
    {
        return mode_;
    }
    Status create(std::string_view path, NodeType type);
    Status write_file(std::string_view path, std::span<const std::byte> data);
    Result<FileBuffer> read_file(std::string_view path);
    [[nodiscard]] Node *lookup(std::string_view path);

  private:
    [[nodiscard]] RamNode *allocate_node(std::string_view name, NodeType type);
    [[nodiscard]] RamNode *parent_for(std::string_view path, FixedString<max_path_segment> &leaf);
    [[nodiscard]] static bool next_segment(std::string_view path, usize &cursor, std::string_view &segment);

    FileSystemMode mode_{FileSystemMode::ram_only};
    std::array<RamNode, max_ram_nodes> nodes_{};
    usize used_nodes_{0};
    RamNode *root_{nullptr};
};

} // namespace ok::fs
