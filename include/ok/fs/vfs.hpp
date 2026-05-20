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
inline constexpr usize max_ram_file_buffers = 32;
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
};

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

    Status configure(std::string_view name, NodeType type, FileBuffer *data);
    Status attach_child(RamNode &child);
    Status detach_child(std::string_view child);

    [[nodiscard]] std::string_view name() const override
    {
        return name_.view();
    }
    [[nodiscard]] Metadata metadata() const override;
    Result<FileBuffer> read(usize offset, usize count) const override;
    Status write(usize offset, std::span<const std::byte> data) override;
    Node *lookup(std::string_view child) override;
    Status create(std::string_view name, NodeType type) override;
    [[nodiscard]] Result<DirectoryListing> list() const;
    [[nodiscard]] bool used() const
    {
        return used_;
    }
    [[nodiscard]] NodeType type() const
    {
        return metadata_.type;
    }

  private:
    bool used_{false};
    FixedString<max_path_segment> name_{};
    Metadata metadata_{};
    FileBuffer *data_{nullptr};
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
    Status unlink(std::string_view path);
    Status write_file(std::string_view path, std::span<const std::byte> data);
    Result<FileBuffer> read_file(std::string_view path);
    Result<DirectoryListing> list(std::string_view path);
    Result<Metadata> stat(std::string_view path);
    [[nodiscard]] Node *lookup(std::string_view path);

  private:
    [[nodiscard]] RamNode *allocate_node(std::string_view name, NodeType type);
    [[nodiscard]] FileBuffer *allocate_file_buffer(NodeType type);
    void release_file_buffer(FileBuffer *buffer);
    [[nodiscard]] RamNode *parent_for(std::string_view path, FixedString<max_path_segment> &leaf);
    [[nodiscard]] static bool next_segment(std::string_view path, usize &cursor, std::string_view &segment);

    FileSystemMode mode_{FileSystemMode::ram_only};
    std::array<RamNode, max_ram_nodes> nodes_{};
    std::array<FileBuffer, max_ram_file_buffers> file_buffers_{};
    std::array<bool, max_ram_file_buffers> file_buffer_used_{};
    usize used_nodes_{0};
    RamNode *root_{nullptr};
};

} // namespace ok::fs
