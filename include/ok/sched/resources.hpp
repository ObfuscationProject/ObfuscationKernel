#pragma once
#include "ok/arch/execution.hpp"
#include "ok/fs/control_node.hpp"
#include "ok/fs/terminal_node.hpp"
#include "ok/sched/raw_pty_transport.hpp"
#include "ok/net/tcp_stream_handle.hpp"
#include "ok/core/fixed.hpp"
#include "ok/memory/memory.hpp"
#include "ok/user/native_mapping_pages.hpp"
#include "ok/user/native_shared_file_handle.hpp"
#include "ok/smp/gate.hpp"
#include <array>
#include <atomic>
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
    // The device node a path open converted (fstat reports it); none for raw
    // pairs and the boot stdio. Console I/O reads it only to exempt a
    // /dev/console open from job control (Linux redirected_tty_write).
    fs::TerminalNodeKind terminal_origin{};
    // A kernel control attribute this description opened; writes to a
    // cpu_online one go to the CPU hotplug service (docs/modules/sysfs.md).
    fs::ControlNode control{};
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

// One process's address-space gate (rank mm, class B: its holder may wait
// for quiesce_root, a frame copy or a lower gate). It covers the VM records
// (native_pages, memory, mappings), native_space publication, the program
// break, memory_limits, space_sequence and dead. Translation writers
// (remap, destroy_space) of a live space run holding it.
//
// native_space is published under a seqcount: space_sequence is odd while
// the holder rewrites the handle, so a reader without the gate (a user entry)
// retries a torn copy. translation_sequence moves after the holder changed a
// translation of the live space in place (copy-on-write resolution); a page
// fault taken while the change ran finds it moved once it holds the gate.
// `dead` is set once teardown retired the space; a later holder finds
// nothing to change.
//
// A process record is copied when the table compacts (reap), never while its
// gate is held: a copy takes the sequence and `dead`, and a fresh gate.
class AddressSpaceGate final
{
  public:
    constexpr AddressSpaceGate() noexcept = default;
    AddressSpaceGate(const AddressSpaceGate &other) noexcept
        : space_sequence(other.space_sequence.load(std::memory_order_relaxed)),
          translation_sequence(other.translation_sequence.load(std::memory_order_relaxed)), dead(other.dead)
    {
    }
    AddressSpaceGate &operator=(const AddressSpaceGate &other) noexcept
    {
        if (this != &other)
        {
            space_sequence.store(other.space_sequence.load(std::memory_order_relaxed), std::memory_order_relaxed);
            translation_sequence.store(other.translation_sequence.load(std::memory_order_relaxed),
                                       std::memory_order_relaxed);
            dead = other.dead;
        }
        return *this;
    }

    [[nodiscard]] Status enter() noexcept { return gate_.enter(); }
    void leave() noexcept { gate_.leave(); }
    [[nodiscard]] bool owned_by_current_cpu() const noexcept { return gate_.owned_by_current_cpu(); }
    [[nodiscard]] bool held() const noexcept { return gate_.held(); }

    // Holds the gate for a scope unless this CPU holds it already (a nested
    // user-access resolution inside a held stretch).
    class Hold final
    {
      public:
        explicit Hold(AddressSpaceGate &gate) noexcept
            : gate_(gate), entered_(!gate.owned_by_current_cpu() && gate.enter().ok())
        {
        }
        ~Hold()
        {
            if (entered_)
                gate_.leave();
        }
        Hold(const Hold &) = delete;
        Hold &operator=(const Hold &) = delete;

      private:
        AddressSpaceGate &gate_;
        bool entered_;
    };

    // After the in-place change is complete (its translations installed);
    // only the holder writes, with a load and a store.
    void translations_changed() noexcept
    {
        translation_sequence.store(translation_sequence.load(std::memory_order_relaxed) + 1,
                                   std::memory_order_release);
    }

    std::atomic<u32> space_sequence{};
    std::atomic<u32> translation_sequence{};
    bool dead{};

  private:
    smp::ServingGate<smp::LockRank::mm> gate_{};
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
    AddressSpaceGate mm{};
};

// The mm holder publishes a new handle (seqcount writer).
inline void publish_native_space(ProcessResources &resources, const arch::execution::AddressSpace &space) noexcept
{
    smp::check_held(resources.mm, "publish_native_space");
    auto &sequence = resources.mm.space_sequence;
    const auto start = sequence.load(std::memory_order_relaxed);
    sequence.store(start + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    std::atomic_ref<uptr>{resources.native_space.root}.store(space.root, std::memory_order_relaxed);
    std::atomic_ref<u32>{resources.native_space.slot}.store(space.slot, std::memory_order_relaxed);
    std::atomic_ref<u32>{resources.native_space.generation}.store(space.generation, std::memory_order_relaxed);
    sequence.store(start + 2, std::memory_order_release);
}

// A consistent copy of the handle without the gate (seqcount reader).
[[nodiscard]] inline arch::execution::AddressSpace read_native_space(const ProcessResources &resources) noexcept
{
    auto &space = const_cast<arch::execution::AddressSpace &>(resources.native_space);
    for (;;)
    {
        const auto before = resources.mm.space_sequence.load(std::memory_order_acquire);
        const arch::execution::AddressSpace copy{
            std::atomic_ref<uptr>{space.root}.load(std::memory_order_relaxed),
            std::atomic_ref<u32>{space.slot}.load(std::memory_order_relaxed),
            std::atomic_ref<u32>{space.generation}.load(std::memory_order_relaxed)};
        std::atomic_thread_fence(std::memory_order_acquire);
        if ((before & 1) == 0 && resources.mm.space_sequence.load(std::memory_order_relaxed) == before)
            return copy;
        arch::ArchOperations::spin_pause();
    }
}

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
    pty_write = 14,
    // A terminal call parked by job control, keyed by terminal (0 console,
    // 1 + PTY slot); it re-dispatches when the wait completes.
    job_control = 15,
    // A write to /sys/devices/system/cpu/cpuN/online waiting for CPU 0's
    // hotplug service; uninterruptible, keyed 0 (docs/modules/sysfs.md).
    cpu_hotplug = 16
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
