#pragma once
#include "ok/arch/execution.hpp"
#include "ok/sched/raw_pty_transport.hpp"
#include "ok/net/tcp_stream_handle.hpp"
#include "ok/core/fixed.hpp"
#include "ok/memory/memory.hpp"
#include "ok/user/native_mapping_pages.hpp"
#include "ok/user/native_shared_file_handle.hpp"
#include <array>
namespace ok::fs
{
class Node;
}
namespace ok::sched
{
// A single surviving init may own every other process in the bounded table.
inline constexpr usize max_process_children = 64;
inline constexpr usize max_memory_areas = 16;
inline constexpr usize max_process_fds = 32;
inline constexpr usize max_elf_load_segments = 8;

struct SignalState
{
    u64 pending{0};
    u64 blocked{0};
};

struct ProcessGroup
{
    u64 id{0};
};

struct Session
{
    u64 id{0};
};

inline constexpr usize max_open_descriptions = 256;
inline constexpr u16 invalid_description = 0xffffu;

inline constexpr usize max_unix_endpoints = 32;
inline constexpr usize max_unix_backlog = 4;
struct UnixEndpointHandle
{
    u16 slot{invalid_description};
    u64 generation{};
    [[nodiscard]] explicit operator bool() const { return slot < max_unix_endpoints && generation; }
    bool operator==(const UnixEndpointHandle &) const = default;
};
struct UnixPeerCredentials
{
    u64 pid{};
    u32 uid{}, gid{};
    bool operator==(const UnixPeerCredentials &) const = default;
};
struct UnixSocketReadiness
{
    bool listening{}, readable{}, writable{}, hangup{}, error{};
};

struct FileDescriptor
{
    bool used{false};
    u64 reservation{0}; // Private receive admission; zero is an available or published slot.
    bool close_on_exec{false};
    u16 description{invalid_description};
    u64 installation{0}; // Shared scheduler token; close/reinstall never inherits authority.
};

struct OpenFileDescription
{
    bool used{false};
    u64 reservation{0}; // Unpublished canonical description, owned by one prepared operation.
    bool readable{false};
    bool writable{false};
    bool console{false};
    bool input_console{false};
    bool large_file{false}; // Size policy admitted when opening the description.
    u32 flags{0};
    usize offset{0};
    usize references{0};
    usize unix_queue_references{0}; // Included in references, but never an external collection root.
    u16 pipe{invalid_description};
    // AF_UNIX stream endpoints receive from pipe and transmit to unix_write_pipe.
    u16 unix_write_pipe{invalid_description};
    private_pty::Handle pty_endpoint{}; // One canonical typed PTY endpoint; never console/pipe/UNIX backing.
    UnixEndpointHandle unix_endpoint{}; // One pathname endpoint owner per canonical OFD.
    u32 native_udp_handle{0}; // Zero is not a UDP socket; final OFD release retires the handle.
    net::TcpStreamHandle native_tcp_handle{}; // One stream owner per shared OFD.
    user::native::SharedFileHandle native_shared_file{}; // One volatile file owner per canonical OFD.
    fs::Node *node{nullptr};
    usize mount_id{0};
    FixedString<96> path{};
};

// Diagnostic counters saturate; they never grant or discharge a provider hold.
struct ResourceDrainReport
{
    Status first_failure{};
    usize operations{}, descriptors_consumed{}, cwd_consumed{}, failed_releases{};
    usize descriptions_remaining{}, reservations_remaining{};
    i32 first_failed_fd{-1};
    u16 first_failed_description{invalid_description};
    usize first_failed_mount_id{};
    bool first_failed_cwd{}, active{};
};

class FileDescriptorTable final
{
  public:
    using FdLike = i32;
    // Legacy metadata helpers cannot release or copy scheduler-owned descriptions.
    Status open_slot(FdLike fd);
    Status close_slot(FdLike fd);
    Status clone_from(const FileDescriptorTable &source);
    [[nodiscard]] bool contains(FdLike fd) const;
    [[nodiscard]] usize open_count() const;
    [[nodiscard]] FileDescriptor *entry(FdLike fd)
    {
        return fd >= 0 && static_cast<usize>(fd) < entries_.size() ? &entries_[static_cast<usize>(fd)] : nullptr;
    }
    [[nodiscard]] const FileDescriptor *entry(FdLike fd) const
    {
        return fd >= 0 && static_cast<usize>(fd) < entries_.size() ? &entries_[static_cast<usize>(fd)] : nullptr;
    }
    [[nodiscard]] auto &entries()
    {
        return entries_;
    }
    [[nodiscard]] const auto &entries() const
    {
        return entries_;
    }

  private:
    std::array<FileDescriptor, max_process_fds> entries_{};
};

class MemoryMap final
{
  public:
    Status add_area(memory::VmArea area);
    Status clone_from(const MemoryMap &source);
    Status replace_with(memory::VmArea area);
    [[nodiscard]] bool contains(uptr address, usize length) const;
    [[nodiscard]] bool allows_user_access(uptr address, usize length, usize permissions) const;
    [[nodiscard]] Status require_user_access(uptr address, usize length, usize permissions) const;
    [[nodiscard]] usize area_count() const
    {
        return areas_.size();
    }
    [[nodiscard]] const memory::VmArea *area(usize index) const;

  private:
    StaticVector<memory::VmArea, max_memory_areas> areas_;
};

struct MemoryMapping
{
    bool used{false};
    uptr address{0};
    usize length{0};
    u32 protection{0};
    u32 flags{0};
    i32 fd{-1};
    usize offset{0};
};

enum class UserPersonality : u8
{
    native,
    linux
};

// One RLIMIT value pair in bytes, laid out like the UAPI `ok_rlimit`.
struct MemoryLimit
{
    u64 current{~u64{0}};
    u64 maximum{~u64{0}};
};

// Per-process memory quotas (RLIMIT_AS, RLIMIT_DATA). Native mappings are
// eager, so every mapped user page is the process's charge against the address
// space limit; the data limit bounds the brk heap. Inherited by fork, kept by exec.
struct MemoryLimits
{
    MemoryLimit address_space{};
    MemoryLimit data{};

    // Common UAPI resource numbers (x86_64 Linux): RLIMIT_DATA 2, RLIMIT_AS 9.
    [[nodiscard]] MemoryLimit *find(uptr resource)
    {
        return resource == 9 ? &address_space : resource == 2 ? &data : nullptr;
    }
    [[nodiscard]] Status admit(usize pages, usize page_size) const
    {
        return pages > address_space.current / page_size ? Status::no_memory("process address-space quota exceeded")
                                                         : Status::success();
    }
    // Errno-style result. Lowering below current usage is allowed (growth then
    // fails); only a privileged caller may raise a maximum.
    static i64 update(MemoryLimit &limit, MemoryLimit requested, bool privileged)
    {
        if (requested.current > requested.maximum)
            return -22;
        if (requested.maximum > limit.maximum && !privileged)
            return -1;
        limit = requested;
        return 0;
    }
};

struct ProcessResources
{
    usize unix_receive_operations{}; // Prepared UNIX receives/accepts prevent resource retirement.
    usize prepared_descriptor_operations{}; // PID-resolved TCP admission shares descriptor tokens with UNIX receive.
    UserPersonality personality{UserPersonality::native};
    arch::execution::AddressSpace native_space{};
    user::native::MappingPages native_pages{};
    FileDescriptorTable files{};
    MemoryMap memory{};
    std::array<MemoryMapping, 32> mappings{};
    FixedString<96> cwd{"/"};
    fs::Node *cwd_node{nullptr};
    usize cwd_mount_id{0};
    uptr program_break_base{0x1000'0000};
    uptr program_break{0x1000'0000};
    uptr program_break_limit{0x1010'0000};
    uptr next_mapping_address{0x2000'0000};
    u32 file_mode_mask{0022};
    MemoryLimits memory_limits{};
};

inline constexpr usize max_process_pipes = 16;
inline constexpr usize pipe_capacity = 4096;
inline constexpr usize max_unix_rights = 8;
inline constexpr usize max_unix_records = 8;
struct UnixRightsRecord
{
    bool used{};
    usize start{}, end{}, count{};
    std::array<u16, max_unix_rights> descriptions{};
};
struct PipeBuffer
{
    bool used{false};
    u64 revision{1};
    usize head{0};
    usize size{0};
    std::array<std::byte, pipe_capacity> data{};
    std::array<UnixRightsRecord, max_unix_records> rights{};
};
enum class ThreadWaitReason : u8
{
    none,
    child,
    pipe_read,
    pipe_write,
    deadline,
    console_read,
    poll,
    futex,
    signal_suspend = 8,
    signal_wait = 9,
    vfs_operation = 10,
    udp_receive = 11,
    tcp_stream = 12,
    pty_read = 13,
    pty_write = 14
};

struct ThreadLocalState
{
    uptr fs_base{0};
    uptr gs_base{0};
    uptr clear_tid_address{0};
    uptr set_tid_address{0};
    uptr robust_list{0};
    usize robust_list_length{0};
};

} // namespace ok::sched
