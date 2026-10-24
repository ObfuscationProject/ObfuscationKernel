#pragma once

#include "ok/core/fixed.hpp"
#include "ok/driver/driver.hpp"
#include "ok/driver/entropy.hpp"
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
inline constexpr u32 o_NONBLOCK = 0x0800;
inline constexpr u32 o_CREAT = 0x0040;
inline constexpr u32 o_EXCL = 0x0080;
inline constexpr u32 o_NOFOLLOW = 0x020000;
inline constexpr u32 o_TRUNC = 0x0200;
inline constexpr u32 o_CLOEXEC = 0x080000;
inline constexpr u32 o_DIRECTORY = 0x0200000;
inline constexpr Fd at_FDCWD = -100;
inline constexpr u32 at_SYMLINK_NOFOLLOW = 0x0100;
inline constexpr u32 at_EACCESS = 0x0200;
inline constexpr u32 at_EMPTY_PATH = 0x1000;

inline constexpr u32 r_OK = 4;
inline constexpr u32 w_OK = 2;
inline constexpr u32 x_OK = 1;
inline constexpr u32 f_OK = 0;

inline constexpr u32 prot_NONE = 0x0;
inline constexpr u32 prot_READ = 0x1;
inline constexpr u32 prot_WRITE = 0x2;
inline constexpr u32 map_SHARED = 0x01;
inline constexpr u32 map_PRIVATE = 0x02;
inline constexpr u32 map_FIXED = 0x10;
inline constexpr u32 map_ANONYMOUS = 0x20;

inline constexpr u32 f_DUPFD = 0;
inline constexpr u32 f_GETFD = 1;
inline constexpr u32 f_SETFD = 2;
inline constexpr u32 f_GETFL = 3;
inline constexpr u32 f_SETFL = 4;
inline constexpr u32 f_ADD_SEALS = 1033;
inline constexpr u32 f_GET_SEALS = 1034;
inline constexpr u32 mfd_CLOEXEC = 1;
inline constexpr u32 mfd_ALLOW_SEALING = 2;
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

// Provider provenance is internal metadata, separate from serialized Linux layouts.
enum class FileStatusProvider : u8 { filesystem, volatile_shared_file };

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
    fs::FileAttributes attributes{};
    FileStatusProvider provider{FileStatusProvider::filesystem};
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

enum class ReadinessPolicy : u8 { native, linux };
enum class PositionedWritePolicy : u8 { native, linux_append };
enum class DirectoryPolicy : u8 { native, linux };

enum class ReadyEvents : u16
{
    none = 0,
    readable = 0x01,
    writable = 0x04,
    error = 0x08,
    hangup = 0x10,
    invalid = 0x20,
};
[[nodiscard]] constexpr ReadyEvents operator|(ReadyEvents left, ReadyEvents right)
{
    return static_cast<ReadyEvents>(static_cast<u16>(left) | static_cast<u16>(right));
}
[[nodiscard]] constexpr ReadyEvents operator&(ReadyEvents left, ReadyEvents right)
{
    return static_cast<ReadyEvents>(static_cast<u16>(left) & static_cast<u16>(right));
}
constexpr ReadyEvents &operator|=(ReadyEvents &left, ReadyEvents right)
{
    return left = left | right;
}
struct DescriptorReadiness
{
    ReadyEvents events{ReadyEvents::none};
    // The native console input owner must merge its actual buffered state.
    bool console_input{false};
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
    FixedString<32> nodename{"obfuscationos"};
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
    [[nodiscard]] sched::ThreadId gettid() const;
    [[nodiscard]] sched::ProcessId getppid() const;
    Result<sched::ProcessId> getpgid(i32 pid = 0) const;
    Result<sched::ProcessId> getsid(i32 pid = 0) const;
    Result<sched::ProcessId> setsid();
    Status setpgid(i32 pid, i32 group);
    [[nodiscard]] u32 uid() const
    {
        return user_credentials().uid;
    }
    [[nodiscard]] u32 euid() const
    {
        return user_credentials().euid;
    }
    [[nodiscard]] u32 gid() const
    {
        return user_credentials().gid;
    }
    [[nodiscard]] u32 egid() const
    {
        return user_credentials().egid;
    }
    [[nodiscard]] user::UserId getuid() const
    {
        return uid();
    }
    [[nodiscard]] user::UserId geteuid() const
    {
        return euid();
    }
    [[nodiscard]] user::GroupId getgid() const
    {
        return gid();
    }
    [[nodiscard]] user::GroupId getegid() const
    {
        return egid();
    }
    [[nodiscard]] fs::Credentials credentials() const
    {
        return fs::Credentials{.uid = euid(), .gid = egid()};
    }
    [[nodiscard]] user::Credentials user_credentials() const;
    Status activate(sched::ProcessId pid);
    Result<sched::ProcessId> fork();
    Result<sched::ProcessId> fork(sched::ProcessId parent, bool publish = true);
    Status exec(sched::ProcessId pid, arch::CpuContext context, const sched::MemoryMap &memory);
    Status exit(sched::ProcessId pid, i32 code);
    Result<i32> wait(sched::ProcessId parent, sched::ProcessId child);
    Status set_identity(u32 uid, u32 gid);
    Status set_credentials(user::Credentials credentials);
    Result<Fd> open(std::string_view path, u32 flags, u32 mode = 0644);
    Result<Fd> openat(Fd dirfd, std::string_view path, u32 flags, u32 mode = 0644);
    // Admission policy is checked before truncation/publication. Native opens
    // have no extra size bound; a selected Linux ABI may impose MAX_NON_LFS.
    Result<Fd> openat_limited(Fd dirfd, std::string_view path, u32 flags, u32 mode, u64 max_regular_size);
    Result<std::array<Fd, 2>> pipe(u32 flags = 0);
    Result<std::array<Fd, 2>> unix_socketpair(u32 flags = 0);
    Result<Fd> unix_socket(u32 flags = 0);
    // The syscall owner captures and retains this canonical description before
    // copying any pathname or output capacity from userspace.
    Status bind_unix_description(u16 description, std::string_view path);
    Status listen_unix_description(u16 description, usize backlog);
    // false reports a retained stale socket node, distinct from a missing pathname.
    Result<bool> connect_unix_description(u16 description, std::string_view path);
    Result<sched::UnixAccept> prepare_unix_accept(u16 description, u32 flags = 0);
    Result<sched::UnixPeerCredentials> unix_peer_credentials(u16 description) const;
    Result<usize> send_unix_rights(Fd fd, std::span<const std::byte> payload,
        std::span<const Fd> descriptors, bool nonblocking = false);
    Result<sched::UnixReceive> prepare_unix_receive(Fd fd, std::span<std::byte> preview,
        usize descriptor_capacity, bool peek = false, bool close_on_exec = false, bool nonblocking = false);
    Result<Fd> memfd_create(std::string_view name, u32 flags);
    // The caller owns the endpoint until this successfully publishes its OFD.
    Result<Fd> install_native_udp(u32 handle, bool close_on_exec = false, bool nonblocking = true);
    // Snapshots and description pins require the serialized boot-CPU service
    // context. A retained ID remains stable until its matching release.
    [[nodiscard]] Result<DescriptorReadiness> ready_events(Fd fd, ReadyEvents requested) const;
    [[nodiscard]] Result<DescriptorReadiness> ready_description(u16 id, ReadyEvents requested) const;
    template <ReadinessPolicy Policy>
    [[nodiscard]] Result<DescriptorReadiness> poll_description(u16 id, ReadyEvents requested) const;
    Status close(Fd fd);
    Status close_range(Fd first, Fd last);
    Result<Fd> duplicate(Fd fd, Fd minimum = 0, u32 flags = 0);
    Result<Fd> duplicate_to(Fd fd, Fd target, u32 flags = 0);
    Result<usize> read(Fd fd, std::span<std::byte> out);
    Result<usize> write(Fd fd, std::span<const std::byte> in);
    Status fsync(Fd fd);
    Status sync_persistent(Fd fd);
    Status ftruncate(Fd fd, usize size);
    Status truncate(std::string_view path, usize size);
    Result<usize> pread(Fd fd, std::span<std::byte> out, usize offset);
    Result<usize> pwrite(Fd fd, std::span<const std::byte> in, usize offset);
    template <PositionedWritePolicy Policy>
    Result<usize> pwrite_with_policy(Fd fd, std::span<const std::byte> in, usize offset,
                                    u64 maximum_offset = static_cast<u64>(__INTPTR_MAX__));
    Result<usize> readv(Fd fd, std::span<const IoVector> vectors);
    Result<usize> writev(Fd fd, std::span<const IoVector> vectors);
    Result<usize> seek(Fd fd, i64 offset, SeekWhence whence);
    Status mkdir(std::string_view path, u32 mode = 0755);
    Status mkdirat(Fd dirfd, std::string_view path, u32 mode = 0755);
    Status symlinkat(std::string_view target, Fd dirfd, std::string_view path);
    Result<usize> readlinkat(Fd dirfd, std::string_view path, std::span<std::byte> out);
    Result<fs::PathReference> resolve_node(Fd dirfd, std::string_view path,
                                         fs::FinalComponent policy = fs::FinalComponent::follow);
    Status link(std::string_view oldpath, std::string_view newpath);
    Status linkat(Fd olddirfd, std::string_view oldpath, Fd newdirfd, std::string_view newpath, u32 flags = 0);
    Status rename(std::string_view oldpath, std::string_view newpath);
    Status renameat(Fd olddirfd, std::string_view oldpath, Fd newdirfd, std::string_view newpath);
    Status unlink(std::string_view path);
    Status unlinkat(Fd dirfd, std::string_view path, u32 flags = 0);
    Status rmdir(std::string_view path);
    Status chdir(std::string_view path);
    Status fchdir(Fd fd);
    Status access(std::string_view path, u32 mode);
    Status faccessat(Fd dirfd, std::string_view path, u32 mode, u32 flags = 0);
    Status chmod(std::string_view path, u32 mode);
    fs::ModeChangeResult change_mode_at(Fd dirfd, std::string_view path, u32 mode, u32 flags = 0);
    fs::ModeChangeResult change_mode_fd(Fd fd, u32 mode);
    fs::ModeChangeResult change_owner_at(Fd dirfd, std::string_view path, u32 uid, u32 gid, u32 flags = 0);
    fs::ModeChangeResult change_owner_fd(Fd fd, u32 uid, u32 gid);
    Status chown(std::string_view path, u32 uid, u32 gid);
    Result<i64> fcntl(Fd fd, u32 command, u64 argument);
    Result<i64> ioctl(Fd fd, u64 request, u64 argument);
    [[nodiscard]] Status console_control_descriptor(Fd fd) const;
    Result<i64> console_control(Fd fd, sched::ConsoleControlOperation operation, i32 value = 0);
    [[nodiscard]] Result<std::string_view> current_directory();
    [[nodiscard]] std::string_view getcwd()
    {
        auto path = current_directory();
        return path ? path.value() : std::string_view{};
    }
    Result<FileStatus> stat(std::string_view path);
    Result<FileStatus> statat(Fd dirfd, std::string_view path, u32 flags = 0);
    Result<FileStatus> fstat(Fd fd) const;
    Result<usize> getdents64(Fd fd, std::span<std::byte> out);
    // The serialized consumer copies one complete record; it cannot yield or
    // reenter descriptor operations. Cursor commit follows successful copying.
    template <DirectoryPolicy Policy, typename Consumer>
        requires requires(Consumer consumer, const fs::DirectoryEntry &entry, u64 cookie) {
            { consumer(entry, cookie) } -> std::same_as<Result<usize>>;
        }
    Result<usize> enumerate_directory(Fd fd, Consumer &&consumer);
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
    // Source ownership remains with the boot distribution. A null source
    // removes access; an absent source fails closed with would_block.
    [[nodiscard]] driver::EntropySource *entropy_source() const { return entropy_source_; }
    void set_entropy_source(driver::EntropySource *source) { entropy_source_ = source; }
    Result<usize> getrandom(std::span<std::byte> out, u32 flags = 0);
    Status set_tid_address(uptr address);
    Status set_robust_list(uptr head, usize length);
    Status rseq(uptr control_block, u32 length, i32 flags, u32 signature);
    Status signal_noop();
    Status sched_yield();
    Status exit(i32 code);
    void advance_clock(u64 milliseconds)
    {
        if (milliseconds <= (~u64{0} - monotonic_ticks_) / 1000000)
            monotonic_ticks_ += milliseconds * 1000000;
    }
    void set_monotonic_time(u64 nanoseconds, u64 resolution)
    {
        monotonic_ticks_ = nanoseconds;
        clock_resolution_ = resolution;
    }
    [[nodiscard]] u32 umask(u32 mask);
    [[nodiscard]] usize open_file_count() const;

  private:
    using FileDescriptor = sched::OpenFileDescription;
    using MemoryMapping = sched::MemoryMapping;
    [[nodiscard]] sched::ProcessResources &resources();
    [[nodiscard]] const sched::ProcessResources &resources() const;
    [[nodiscard]] sched::ThreadLocalState &thread_local_state();
    [[nodiscard]] FileDescriptor *descriptor(Fd fd);
    [[nodiscard]] const FileDescriptor *descriptor(Fd fd) const;
    [[nodiscard]] Result<Fd> allocate_fd(Fd minimum = 0);
    [[nodiscard]] Result<fs::ResolveContext> path_context(Fd dirfd, std::string_view path);
    [[nodiscard]] Status copy_descriptor(Fd source, Fd target, u32 flags);

    fs::VirtualFileSystem *vfs_{nullptr};
    driver::ConsoleDriver *console_{nullptr};
    sched::Scheduler *scheduler_{nullptr};
    sched::ProcessResources unbound_resources_{};
    sched::ThreadLocalState boot_tls_{};
    user::Credentials boot_credentials_{user::kernel_credentials()};
    bool initialized_{false};
    u64 monotonic_ticks_{0};
    u64 clock_resolution_{1000000};
    driver::EntropySource *entropy_source_{nullptr};
};

template <DirectoryPolicy Policy, typename Consumer>
    requires requires(Consumer consumer, const fs::DirectoryEntry &entry, u64 cookie) {
        { consumer(entry, cookie) } -> std::same_as<Result<usize>>;
    }
Result<usize> PosixService::enumerate_directory(Fd fd, Consumer &&consumer)
{
    auto *description = descriptor(fd);
    if (!description || !description->used)
        return Status::bad_descriptor("bad directory descriptor");
    if (!description->node)
    {
        if constexpr (Policy == DirectoryPolicy::linux)
            return Status::not_directory("descriptor has no directory provider");
        return Status::bad_descriptor("bad directory file descriptor");
    }
    auto metadata = description->node->query_metadata();
    if (!metadata) return metadata.status();
    if (metadata.value().type != fs::NodeType::directory)
        return Status::not_directory("descriptor is not a directory");
    // Open-time read authority survives later inode permission changes.
    if (!description->readable)
        return Status::bad_descriptor("directory descriptor is not readable");
    auto listing = description->node->list();
    if (!listing) return listing.status();
    if (listing.value().count > listing.value().entries.size())
        return Status::io_error("directory provider exceeded listing capacity");
    constexpr usize dots = Policy == DirectoryPolicy::linux ? 2 : 0;
    std::array<fs::DirectoryEntry, dots> dot_entries{};
    if constexpr (Policy == DirectoryPolicy::linux)
    {
        dot_entries[0].metadata = metadata.value();
        static_cast<void>(dot_entries[0].name.assign("."));
        auto parent = description->node->lookup_child_retained("..");
        if (!parent) return parent.status();
        if (!parent.value()) return Status::io_error("directory parent is null");
        auto parent_metadata = parent.value()->query_metadata();
        const auto released_parent = parent.value()->release();
        if (!released_parent) return released_parent;
        if (!parent_metadata) return parent_metadata.status();
        if (parent_metadata.value().type != fs::NodeType::directory)
            return Status::io_error("directory parent is not a directory");
        dot_entries[1].metadata = parent_metadata.value();
        static_cast<void>(dot_entries[1].name.assign(".."));
        for (const auto &dot : dot_entries)
            if (!dot.metadata.attributes.has(fs::Attribute::inode) || !dot.metadata.attributes.inode)
                return Status::unsupported("directory provider has no qualified inode identity");
    }
    usize written = 0;
    while (description->offset < listing.value().count + dots)
    {
        const auto position = description->offset;
        const auto &source = position < dots ? dot_entries[position] : listing.value().entries[position - dots];
        const auto name = source.name.view();
        if constexpr (Policy == DirectoryPolicy::linux)
        {
            if (name.empty() || name.find('/') != std::string_view::npos || name.find('\0') != std::string_view::npos ||
                (position >= dots && (name == "." || name == "..")))
                return written ? Result<usize>{written} : Result<usize>{Status::io_error("invalid directory entry name")};
            if (!source.metadata.attributes.has(fs::Attribute::inode) || !source.metadata.attributes.inode)
                return written ? Result<usize>{written} : Result<usize>{Status::unsupported("entry has no qualified inode identity")};
        }
        auto emitted = consumer(source, static_cast<u64>(position + 1));
        if (!emitted)
            return written ? Result<usize>{written} : Result<usize>{emitted.status()};
        if (!emitted.value()) return written;
        written += emitted.value();
        description->offset = position + 1;
    }
    return written;
}

} // namespace ok::posix
