#pragma once

#include "ok/core/fixed.hpp"
#include "ok/driver/driver.hpp"
#include "ok/fs/vfs.hpp"
#include "ok/sched/scheduler.hpp"
#include "ok/user/user.hpp"

#include <array>
#include <span>
#include <string_view>

namespace ok::posix
{

using Fd = i32;

inline constexpr usize max_open_files = 32;
inline constexpr usize max_memory_mappings = 32;
inline constexpr u32 o_RDONLY = 0x0000;
inline constexpr u32 o_WRONLY = 0x0001;
inline constexpr u32 o_RDWR = 0x0002;
inline constexpr u32 o_APPEND = 0x0008;
inline constexpr u32 o_CREAT = 0x0040;
inline constexpr u32 o_TRUNC = 0x0200;
inline constexpr u32 o_CLOEXEC = 0x080000;
inline constexpr u32 o_DIRECTORY = 0x0200000;
inline constexpr Fd at_FDCWD = -100;
inline constexpr u32 at_EMPTY_PATH = 0x1000;

inline constexpr u32 r_OK = 4;
inline constexpr u32 w_OK = 2;
inline constexpr u32 x_OK = 1;
inline constexpr u32 f_OK = 0;

inline constexpr u32 prot_NONE = 0x0;
inline constexpr u32 prot_READ = 0x1;
inline constexpr u32 prot_WRITE = 0x2;
inline constexpr u32 map_PRIVATE = 0x02;
inline constexpr u32 map_ANONYMOUS = 0x20;

inline constexpr u32 f_DUPFD = 0;
inline constexpr u32 f_GETFD = 1;
inline constexpr u32 f_SETFD = 2;
inline constexpr u32 f_GETFL = 3;
inline constexpr u32 f_SETFL = 4;
inline constexpr u32 fd_CLOEXEC = 1;

inline constexpr u32 arch_SET_GS = 0x1001;
inline constexpr u32 arch_SET_FS = 0x1002;
inline constexpr u32 arch_GET_FS = 0x1003;
inline constexpr u32 arch_GET_GS = 0x1004;

inline constexpr u32 futex_WAIT = 0;
inline constexpr u32 futex_WAKE = 1;

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

struct IoVector
{
    uptr base{0};
    usize length{0};
};

struct ResourceLimit
{
    u64 current{0};
    u64 maximum{0};
};

struct SystemInfo
{
    i64 uptime{0};
    u64 total_ram{0};
    u64 free_ram{0};
    u16 process_count{0};
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
    [[nodiscard]] sched::ProcessId getppid() const
    {
        return 0;
    }
    [[nodiscard]] u32 uid() const
    {
        return current_uid_;
    }
    [[nodiscard]] u32 euid() const
    {
        return current_euid_;
    }
    [[nodiscard]] u32 gid() const
    {
        return current_gid_;
    }
    [[nodiscard]] u32 egid() const
    {
        return current_egid_;
    }
    [[nodiscard]] user::UserId getuid() const
    {
        return current_uid_;
    }
    [[nodiscard]] user::UserId geteuid() const
    {
        return current_euid_;
    }
    [[nodiscard]] user::GroupId getgid() const
    {
        return current_gid_;
    }
    [[nodiscard]] user::GroupId getegid() const
    {
        return current_egid_;
    }
    [[nodiscard]] fs::Credentials credentials() const
    {
        return fs::Credentials{.uid = current_euid_, .gid = current_egid_};
    }
    [[nodiscard]] user::Credentials user_credentials() const
    {
        return user::Credentials{.uid = current_uid_,
                                 .gid = current_gid_,
                                 .euid = current_euid_,
                                 .egid = current_egid_,
                                 .kernel_space = current_kernel_space_};
    }
    Status set_identity(u32 uid, u32 gid);
    Status set_credentials(user::Credentials credentials);
    Result<Fd> open(std::string_view path, u32 flags, u32 mode = 0644);
    Result<Fd> openat(Fd dirfd, std::string_view path, u32 flags, u32 mode = 0644);
    Status close(Fd fd);
    Status close_range(Fd first, Fd last);
    Result<Fd> duplicate(Fd fd, Fd minimum = 0);
    Result<Fd> duplicate_to(Fd fd, Fd target, u32 flags = 0);
    Result<usize> read(Fd fd, std::span<std::byte> out);
    Result<usize> write(Fd fd, std::span<const std::byte> in);
    Result<usize> pread(Fd fd, std::span<std::byte> out, usize offset);
    Result<usize> pwrite(Fd fd, std::span<const std::byte> in, usize offset);
    Result<usize> readv(Fd fd, std::span<const IoVector> vectors);
    Result<usize> writev(Fd fd, std::span<const IoVector> vectors);
    Result<usize> seek(Fd fd, i64 offset, SeekWhence whence);
    Status mkdir(std::string_view path, u32 mode = 0755);
    Status mkdirat(Fd dirfd, std::string_view path, u32 mode = 0755);
    Status unlink(std::string_view path);
    Status unlinkat(Fd dirfd, std::string_view path, u32 flags = 0);
    Status rmdir(std::string_view path);
    Status chdir(std::string_view path);
    Status fchdir(Fd fd);
    Status access(std::string_view path, u32 mode);
    Status faccessat(Fd dirfd, std::string_view path, u32 mode, u32 flags = 0);
    Status chmod(std::string_view path, u32 mode);
    Status chown(std::string_view path, u32 uid, u32 gid);
    Result<i64> fcntl(Fd fd, u32 command, u64 argument);
    Result<i64> ioctl(Fd fd, u64 request, u64 argument);
    [[nodiscard]] std::string_view getcwd() const
    {
        return cwd_.view();
    }
    Result<FileStatus> stat(std::string_view path);
    Result<FileStatus> statat(Fd dirfd, std::string_view path, u32 flags = 0);
    Result<FileStatus> fstat(Fd fd) const;
    Result<usize> getdents64(Fd fd, std::span<std::byte> out);
    [[nodiscard]] ClockTime clock_gettime() const;
    [[nodiscard]] ClockTime clock_getres() const;
    Status nanosleep(ClockTime requested, ClockTime *remaining = nullptr);
    [[nodiscard]] UnameInfo uname() const;
    [[nodiscard]] ResourceLimit resource_limit(u32 resource) const;
    [[nodiscard]] SystemInfo system_info() const;
    Result<uptr> brk(uptr address);
    Result<uptr> mmap(uptr address, usize length, u32 protection, u32 flags, Fd fd, usize offset);
    Status mprotect(uptr address, usize length, u32 protection);
    Status munmap(uptr address, usize length);
    Result<i64> arch_prctl(u32 code, uptr address);
    Result<i64> futex(uptr user_address, u32 operation, u32 value);
    Result<usize> getrandom(std::span<std::byte> out);
    Status set_tid_address(uptr address);
    Status set_robust_list(uptr head, usize length);
    Status rseq(uptr control_block, u32 length, i32 flags, u32 signature);
    Status signal_noop();
    Status sched_yield();
    Status exit(i32 code);
    [[nodiscard]] u32 umask(u32 mask);
    [[nodiscard]] usize open_file_count() const;

  private:
    struct FileDescriptor
    {
        bool used{false};
        bool readable{false};
        bool writable{false};
        bool console{false};
        bool close_on_exec{false};
        u32 flags{0};
        usize offset{0};
        fs::Node *node{nullptr};
        FixedString<96> path{};
    };

    struct MemoryMapping
    {
        bool used{false};
        uptr address{0};
        usize length{0};
        u32 protection{prot_NONE};
        u32 flags{0};
        Fd fd{-1};
        usize offset{0};
    };

    [[nodiscard]] FileDescriptor *descriptor(Fd fd);
    [[nodiscard]] const FileDescriptor *descriptor(Fd fd) const;
    [[nodiscard]] Result<Fd> allocate_fd(Fd minimum = 3);
    [[nodiscard]] Result<std::string_view> normalize_path(std::string_view path);
    [[nodiscard]] Result<std::string_view> resolve_path(Fd dirfd, std::string_view path);
    [[nodiscard]] Status copy_descriptor(Fd source, Fd target, u32 flags);

    fs::VirtualFileSystem *vfs_{nullptr};
    driver::ConsoleDriver *console_{nullptr};
    sched::Scheduler *scheduler_{nullptr};
    std::array<FileDescriptor, max_open_files> files_{};
    std::array<MemoryMapping, max_memory_mappings> mappings_{};
    FixedString<96> cwd_{"/"};
    FixedString<96> normalized_path_{};
    bool initialized_{false};
    u64 monotonic_ticks_{0};
    uptr program_break_base_{0x1000'0000};
    uptr program_break_{0x1000'0000};
    uptr program_break_limit_{0x1010'0000};
    uptr next_mapping_address_{0x2000'0000};
    uptr fs_base_{0};
    uptr gs_base_{0};
    uptr clear_tid_address_{0};
    u32 file_mode_mask_{0022};
    u32 current_uid_{fs::default_uid};
    u32 current_gid_{fs::default_gid};
    u32 current_euid_{fs::default_uid};
    u32 current_egid_{fs::default_gid};
    bool current_kernel_space_{true};
    i32 last_exit_code_{0};
};

} // namespace ok::posix
