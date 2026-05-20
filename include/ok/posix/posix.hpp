#pragma once

#include "ok/core/fixed.hpp"
#include "ok/driver/driver.hpp"
#include "ok/fs/vfs.hpp"
#include "ok/sched/scheduler.hpp"

#include <array>
#include <span>
#include <string_view>

namespace ok::posix
{

using Fd = i32;

inline constexpr usize max_open_files = 32;
inline constexpr u32 o_RDONLY = 0x0000;
inline constexpr u32 o_WRONLY = 0x0001;
inline constexpr u32 o_RDWR = 0x0002;
inline constexpr u32 o_CREAT = 0x0040;
inline constexpr u32 o_TRUNC = 0x0200;

enum class SeekWhence : u8
{
    set,
    current,
    end,
};

struct FileStatus
{
    fs::NodeType type{fs::NodeType::regular};
    usize size{0};
    u32 mode{fs::mode_for(fs::NodeType::regular, 0644u)};
    u32 uid{fs::default_uid};
    u32 gid{fs::default_gid};
    u32 link_count{1};
    u32 block_size{fs::metadata_block_size};
    u64 blocks{0};
};

struct ClockTime
{
    i64 seconds{0};
    i64 nanoseconds{0};
};

struct UnameInfo
{
    FixedString<32> sysname{"ObfuscationKernel"};
    FixedString<32> nodename{"okernel"};
    FixedString<32> release{"0.1.0"};
    FixedString<32> version{"debug"};
    FixedString<32> machine{"unknown"};
};

class PosixService final
{
  public:
    Status initialize(fs::VirtualFileSystem &vfs, driver::ConsoleDriver &console, sched::Scheduler &scheduler);

    [[nodiscard]] bool initialized() const
    {
        return initialized_;
    }
    [[nodiscard]] sched::ProcessId getpid() const;
    Result<Fd> open(std::string_view path, u32 flags, u32 mode = 0644);
    Status close(Fd fd);
    Result<usize> read(Fd fd, std::span<std::byte> out);
    Result<usize> write(Fd fd, std::span<const std::byte> in);
    Result<usize> seek(Fd fd, i64 offset, SeekWhence whence);
    Status mkdir(std::string_view path, u32 mode = 0755);
    Status unlink(std::string_view path);
    Status chdir(std::string_view path);
    [[nodiscard]] std::string_view getcwd() const
    {
        return cwd_.view();
    }
    Result<FileStatus> stat(std::string_view path);
    [[nodiscard]] ClockTime clock_gettime() const;
    [[nodiscard]] UnameInfo uname() const;
    [[nodiscard]] usize open_file_count() const;

  private:
    struct FileDescriptor
    {
        bool used{false};
        bool readable{false};
        bool writable{false};
        bool console{false};
        usize offset{0};
        fs::Node *node{nullptr};
        FixedString<96> path{};
    };

    [[nodiscard]] FileDescriptor *descriptor(Fd fd);
    [[nodiscard]] const FileDescriptor *descriptor(Fd fd) const;
    [[nodiscard]] Result<Fd> allocate_fd();
    [[nodiscard]] Result<std::string_view> normalize_path(std::string_view path);

    fs::VirtualFileSystem *vfs_{nullptr};
    driver::ConsoleDriver *console_{nullptr};
    sched::Scheduler *scheduler_{nullptr};
    std::array<FileDescriptor, max_open_files> files_{};
    FixedString<96> cwd_{"/"};
    FixedString<96> normalized_path_{};
    bool initialized_{false};
    u64 monotonic_ticks_{0};
};

} // namespace ok::posix
