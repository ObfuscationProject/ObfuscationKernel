#pragma once

#include "ok/core/fixed.hpp"
#include "ok/core/types.hpp"
#include "ok/fs/vfs.hpp"

#include <array>
#include <span>
#include <string_view>

namespace ok::fs
{

inline constexpr usize max_mounts = 8;
inline constexpr usize pipe_capacity = 256;

struct Inode
{
    u64 number{0};
    Metadata metadata{};
};

struct Vnode
{
    FixedString<96> path{};
    Inode inode{};
};

struct SuperBlock
{
    FixedString<32> filesystem{"ramfs"};
    u64 block_size{metadata_block_size};
};

class FileOperations
{
  public:
    virtual ~FileOperations() = default;
    virtual Result<usize> read(std::span<std::byte> out) = 0;
    virtual Result<usize> write(std::span<const std::byte> in) = 0;
    virtual Result<u32> poll() = 0;
};

class DirectoryOperations
{
  public:
    virtual ~DirectoryOperations() = default;
    virtual Result<DirectoryListing> list() const = 0;
};

struct File
{
    Vnode *node{nullptr};
    FileOperations *operations{nullptr};
    usize offset{0};
    u32 flags{0};
};

struct Mount
{
    FixedString<96> path{};
    SuperBlock superblock{};
};

class MountNamespace final
{
  public:
    Status mount(std::string_view path, SuperBlock superblock);
    [[nodiscard]] const Mount *lookup_mount(std::string_view path) const;
    [[nodiscard]] usize mount_count() const
    {
        return mounts_.size();
    }

  private:
    StaticVector<Mount, max_mounts> mounts_;
};

enum class DeviceKind : u8
{
    null,
    zero,
    random,
    console,
    tty,
    block,
};

class DeviceNode final : public FileOperations
{
  public:
    DeviceNode() = default;
    explicit DeviceNode(DeviceKind kind) : kind_(kind)
    {
    }

    Result<usize> read(std::span<std::byte> out) override;
    Result<usize> write(std::span<const std::byte> in) override;
    Result<u32> poll() override;
    [[nodiscard]] DeviceKind kind() const
    {
        return kind_;
    }
    [[nodiscard]] usize bytes_written() const
    {
        return bytes_written_;
    }

  private:
    DeviceKind kind_{DeviceKind::null};
    usize bytes_written_{0};
    u64 random_state_{0x123456789abcdef0ull};
};

class Pipe final : public FileOperations
{
  public:
    void set_non_blocking(bool enabled)
    {
        non_blocking_ = enabled;
    }
    Result<usize> read(std::span<std::byte> out) override;
    Result<usize> write(std::span<const std::byte> in) override;
    Result<u32> poll() override;
    [[nodiscard]] usize size() const
    {
        return size_;
    }

  private:
    std::array<std::byte, pipe_capacity> data_{};
    usize head_{0};
    usize tail_{0};
    usize size_{0};
    bool non_blocking_{false};
};

class TtyDevice final : public FileOperations
{
  public:
    Result<usize> read(std::span<std::byte> out) override;
    Result<usize> write(std::span<const std::byte> in) override;
    Result<u32> poll() override;
    Result<i64> ioctl(u64 request, u64 argument);
    [[nodiscard]] bool echo() const
    {
        return echo_;
    }
    [[nodiscard]] usize bytes_written() const
    {
        return bytes_written_;
    }

  private:
    bool echo_{true};
    usize bytes_written_{0};
};

class UnixVfsModel final
{
  public:
    Status initialize(VirtualFileSystem &vfs);
    Status validate_mounts();
    Status validate_files();
    Status validate_directories();
    Status validate_symlink();
    Status validate_devices();
    Status validate_pipe();
    Status validate_tty();

  private:
    MountNamespace mounts_;
    DeviceNode null_{DeviceKind::null};
    DeviceNode zero_{DeviceKind::zero};
    DeviceNode console_{DeviceKind::console};
    Pipe pipe_;
    TtyDevice tty_;
    VirtualFileSystem *vfs_{nullptr};
};

} // namespace ok::fs
