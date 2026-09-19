#pragma once

#include "ok/core/fixed.hpp"
#include "ok/core/types.hpp"
#include "ok/smp/smp.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

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

struct Credentials
{
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
    virtual Status retain()
    {
        return Status::success();
    }
    virtual Status release()
    {
        return Status::success();
    }
    virtual Result<DirectoryListing> list() const
    {
        return Status::unsupported("directory listing unavailable");
    }
    virtual Status remove(std::string_view, bool)
    {
        return Status::unsupported("remove unavailable");
    }
    [[nodiscard]] virtual const void *filesystem_identity() const
    {
        return nullptr;
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
    virtual Node *lookup(std::string_view child) = 0;
    virtual Status create(std::string_view name, NodeType type) = 0;
    virtual Status chmod(u32 mode) = 0;
    virtual Status chown(u32 uid, u32 gid) = 0;
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
    const void *filesystem_identity() const override
    {
        return owner_;
    }
    Status link(std::string_view name, Node &target) override;
    Status rename(std::string_view old_name, Node &new_parent, std::string_view new_name) override;
    Result<FixedString<96>> directory_path(const Node &root) const override;
    Node *lookup(std::string_view child) override;
    Status create(std::string_view name, NodeType type) override;
    Status chmod(u32 mode) override;
    Status chown(u32 uid, u32 gid) override;
    [[nodiscard]] Result<DirectoryListing> list() const override;
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
    [[nodiscard]] usize child_index(std::string_view name) const;
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

    void set_mode(FileSystemMode mode)
    {
        mode_ = mode;
    }
    [[nodiscard]] FileSystemMode mode() const
    {
        return mode_;
    }
    Status mount(std::string_view path, Node &root);
    Status create(std::string_view path, NodeType type);
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
    [[nodiscard]] Node *lookup(std::string_view path, usize *mount_id = nullptr);
    // Zero is the underlying RAM namespace; mounted roots have stable nonzero IDs.
    // Automatic selection is for inspection only. Directory handles retain an ID.
    static constexpr usize automatic_mount_id = ~usize{0};
    Result<FixedString<96>> directory_path(Node &directory, usize mount_id = automatic_mount_id);

  private:
    [[nodiscard]] Node *lookup_unlocked(std::string_view path, usize *mount_id = nullptr,
                                        FixedString<256> *canonical = nullptr);
    [[nodiscard]] RamNode *allocate_node(std::string_view name, NodeType type, bool readonly = false);
    [[nodiscard]] FileBuffer *allocate_file_buffer(NodeType type);
    void release_file_buffer(FileBuffer *buffer);
    [[nodiscard]] Node *parent_for(std::string_view path, FixedString<max_path_segment> &leaf,
                                   usize *mount_id = nullptr);
    [[nodiscard]] static bool next_segment(std::string_view path, usize &cursor, std::string_view &segment);

    [[nodiscard]] RamNode *ram_node(Node *node);
    struct Mount
    {
        FixedString<256> path{};
        Node *root{nullptr};
        Node *covered{nullptr};
    };
    std::array<Mount, 8> mounts_{};
    FileSystemMode mode_{FileSystemMode::ram_only};
    std::array<RamNode, max_ram_nodes> nodes_{};
    std::array<FileBuffer, max_ram_file_buffers> file_buffers_{};
    std::array<bool, max_ram_file_buffers> file_buffer_used_{};
    RamNode *root_{nullptr};
    mutable smp::SpinLock lock_{};
};

} // namespace ok::fs
