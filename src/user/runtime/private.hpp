#pragma once
// Private to the native runtime units (src/user/runtime.cpp and src/user/runtime/).
// The runtime is one serialized boot-CPU owner; every declaration here has
// exactly one definition in one unit, and all state is constant-initialized.
#include "ok/user/native_signal_config.hpp"
#include "ok/uapi/signal.h"
#include "ok/uapi/job_control.h"
#include "ok/user/job_control_signals.hpp"
#include "../native_lifecycle.hpp"
#include "../signals/private_signal_scheduler_relocation.hpp"
#include "ok/user/native_signal_action.hpp"
#include "ok/user/runtime.hpp"
#include "ok/user/native_sessions_profile.hpp"
#include "ok/user/native_process_group_signal.hpp"
#include "ok/user/native_group_signal_profile.hpp"
#include "ok/user/native_console_control_profile.hpp"
#include "ok/core/native_shutdown.hpp"
#include "ok/sched/group_quiesce.hpp"
#include "ok/smp/bringup.hpp"
#include "ok/smp/cpu.hpp"
#include "ok/smp/kernel_section.hpp"
#include "ok/user/uaccess.hpp"
#include "futex_buckets.hpp"
#include "ok/user/native_sleep.hpp"
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64) || defined(__mips__) || defined(__powerpc__) || defined(__PPC__))
#define OK_PRIVATE_POLL_MASK 1
#include "../native_poll_mask.hpp"
#include "../signals/process_pending.hpp"
#include "../signals/wait_state.hpp"
#include "../signals/child_exit_lifecycle.hpp"
#include "ok/user/native_signal_target.hpp"
#include "ok/user/native_sleep_interruption.hpp"
#if defined(__i386__)
#include "ok/arch/i386/signal.hpp"
#elif defined(__arm__)
#include "ok/arch/arm32/signal.hpp"
#elif defined(__aarch64__)
#include "ok/arch/aarch64/signal.hpp"
#elif defined(__powerpc__) || defined(__PPC__)
#include "ok/arch/ppc/signal.hpp"
#define OK_PRIVATE_SIGNAL_HAS_TRAMPOLINE 1
#define OK_PRIVATE_PPC32_SIGNAL 1
#elif defined(__mips64)
#include "ok/arch/mips64/signal.hpp"
#include "ok/user/native_mips64_n64_elf_admission.hpp"
#include "ok/user/native_signal_trampoline.hpp"
#define OK_PRIVATE_SIGNAL_HAS_TRAMPOLINE 1
#define OK_PRIVATE_MIPS64_N64_SIGNAL 1
#elif defined(__mips__) && !defined(__mips64)
#include "ok/arch/mips/signal.hpp"
#include "ok/user/native_mips_o32_elf_admission.hpp"
#include "ok/user/native_signal_trampoline.hpp"
#define OK_PRIVATE_SIGNAL_HAS_TRAMPOLINE 1
#define OK_PRIVATE_MIPS_O32_SIGNAL 1
#elif defined(__loongarch64)
#include "ok/arch/loongarch64/signal.hpp"
#include "ok/user/native_signal_trampoline.hpp"
#define OK_PRIVATE_SIGNAL_HAS_TRAMPOLINE 1
#elif defined(__riscv)
#include "ok/arch/riscv/signal.hpp"
#include "ok/user/native_signal_trampoline.hpp"
#define OK_PRIVATE_SIGNAL_HAS_TRAMPOLINE 1
#else
#include "ok/arch/x86_64/signal.hpp"
#endif
#endif
#include "ok/user/native_poll.hpp"
#include "ok/user/native_futex.hpp"
#include "ok/user/native_futex_requeue_sdk_profile.hpp"
#if defined(OK_ENABLE_TEST_POINTS) && defined(OK_PRIVATE_FUTEX_PEER_FIXTURE)
#include "../../../tests/native/private_futex_peer_shared.hpp"
#endif
#include "ok/user/native_select.hpp"
#include "ok/user/native_pty_pair_dispatch.hpp"
#include "ok/user/native_copy_on_write.hpp"
#include "ok/user/native_shared_files.hpp"
#include "ok/abi/linux_futex_wake_operation.hpp"
#include "ok/user/native_shared_files_sdk_profile.hpp"
#include "ok/user/native_entry_stacks.hpp"
#ifdef OK_KERNEL_FREESTANDING
#include "ok/arch/service_stack.hpp"
#endif
#include "../native_syscall_origin.hpp"
#include <ok/net/tcp_accept_copied.hpp>
#include "../native_shared_capture.hpp"
#if defined(OK_PRIVATE_MIPS64_N64_SIGNAL) || defined(OK_PRIVATE_PPC32_SIGNAL)
#include "../native_syscall_restart_publication.hpp"
#endif
#include "ok/memory/object_pool.hpp"
#include "ok/boot/boot_memory.hpp"
#include "ok/abi/linux_exec.hpp"
#if defined(__x86_64__)
#include "ok/abi/linux_dynamic_exec_x86_64.hpp"
#endif
#include "ok/user/linux_bootstrap.hpp"
#include "ok/user/native_credentials.hpp"
#include "ok/user/native_mount_info.hpp"
#include "ok/user/console_control_dispatch.hpp"
#include "ok/user/native_metadata.hpp"
#include "ok/arch/execution.hpp"
#include "ok/arch/clock.hpp"
#include "ok/interrupt/platform.hpp"
#include "ok/time/public_clock.hpp"
#include "ok/user/clock_set.hpp"
#include "ok/core/kernel.hpp"
#include "ok/net/native_udp_poll.hpp"
#include "../linux_udp_immediate_access.hpp"
#include "../linux_udp_captured_decode.hpp"
#include "../linux_udp_operation_body.hpp"
#include "../linux_udp_receive_owner_store.hpp"
#include "../tcp_immediate_access.hpp"
#include "../tcp_captured_accept.hpp"
#include "ok/sched/process.hpp"
#include "ok/syscall/linux.hpp"
#include "ok/uapi/syscall.h"

namespace ok::user::native
{
inline namespace runtime_detail
{
namespace hw = arch::execution;
namespace lu = abi::linux_user;
using LinuxExec = lu::ExecBootstrap<lu::configured_native_profile()>;
using Personality = sched::UserPersonality;
// Job-control numbering of a personality; native numbers are Linux generic.
[[nodiscard]] constexpr const job::Numbering &private_job_numbering(Personality personality)
{
    return personality == Personality::linux ? job::linux_numbering<lu::configured_native_profile()> : job::native;
}
void copy_bytes(void *dst, const void *src, usize size);
using Pages = MappingPages;
extern NativePagePool page_pool;
extern SharedMemoryFiles shared_files;
#ifdef OK_PRIVATE_SIGNAL_HAS_TRAMPOLINE
using PrivateSignalCode = SignalTrampoline<lu::configured_native_profile()>;
constexpr uptr private_signal_code_address = hw::user_end - hw::page_size;
#endif
// Part of the linker-reserved kernel image, separate from user/object frames.
extern std::array<std::byte, memory::KernelEntryStackPool::arena_bytes> entry_stack_arena;
extern memory::KernelEntryStackPool entry_stacks;
using SyscallOrigins = SyscallOriginTable<lu::configured_native_profile()>;
extern SyscallOrigins syscall_origins;
extern VfsStatContinuation<lu::configured_native_profile()> vfs_continuations;
// User entries by outcome, per CPU: written only by that CPU (a load and a
// store), summed by entry_total for reports and admission digests.
struct alignas(64) EntryCpu
{
    std::atomic<uptr> syscalls{}, timers{}, interrupts{}, faults{}, rejections{};
};
extern std::array<EntryCpu, smp::max_cpus> entry_cpus;
void count_entry(std::atomic<uptr> EntryCpu::*counter);
[[nodiscard]] u64 entry_total(std::atomic<uptr> EntryCpu::*counter);
// Every outcome on every CPU.
[[nodiscard]] u64 entry_total();
struct QuarantinedSpace
{
    enum class Ownership : u8 { borrowed, pending, discharged };
    hw::AddressSpace space{};
    Pages pages;
    // Per-record state lives in the directory's retirement bytes.
    RetirementField<Ownership, 3, 0> ownership(usize index) { return {&pages.retirement(index)}; }
    RetirementField<bool, 4, 2> pin_held(usize index) { return {&pages.retirement(index)}; }
    void reset(Ownership value) { for (usize index = 0; index < pages.size(); ++index) pages.retirement(index) = u8(value); }
    usize pinned_count{};
    bool used{};
    bool reserved{};
    // Claimed by one CPU that works on it with mm_retire_lock released.
    bool busy{};
    bool table_retired{};
    bool pin_complete{};
};
// A process holds at most one live space and transactions build one
// replacement at a time: one slot per process plus one candidate.
//
// mm_retire_lock (rank mm_retire) covers each slot's state words (used,
// reserved, busy, space, table_retired, pin_complete) and quarantine_live.
// A slot's pages, ownership bits and pin count belong to the CPU that
// reserved it or claimed it busy; that CPU destroys tables and releases
// frames after unlocking (entries are popped under the lock, destroyed
// outside it).
extern std::array<QuarantinedSpace, sched::max_processes + 1> quarantined_spaces;
extern smp::FineLock<smp::LockRank::mm_retire> mm_retire_lock;
// Slots in use, read without the lock to skip an empty retry pass.
extern std::atomic<u32> quarantine_live;
// A free slot reserved for the caller, or null when every slot is taken.
[[nodiscard]] QuarantinedSpace *reserve_quarantine();
void cancel_quarantine(QuarantinedSpace &saved);
// The caller's reserved or free-claimed slot becomes used and stays busy.
void publish_quarantine(QuarantinedSpace &saved, const hw::AddressSpace &space, bool table_retired,
                        bool pin_complete);
// Marks a used slot whose space matches (any, for root 0) busy for the caller.
[[nodiscard]] QuarantinedSpace *claim_quarantine(const hw::AddressSpace &space, bool retired_only);
void unclaim_quarantine(QuarantinedSpace &saved);
void set_quarantine_state(QuarantinedSpace &saved, bool table_retired, bool pin_complete);
[[nodiscard]] bool space_quarantined(const hw::AddressSpace &space);
// The runtime's failure and started state: written only by the kernel lock's
// holder (a kernel section reads them at admission, under the lock). The
// wrappers keep the Status and bool interfaces; every write is a
// require_exclusive site. A failure is stored under runtime_status_lock
// (rank runtime_status, a leaf); `started` is an atomic flag. The one
// exception is bkl_sink(): a plain Status reference for the UDP and TCP
// operation bodies (their original-description guards record release
// failures into it), whose writes bypass runtime_status_lock and are safe
// only while those families dispatch under the kernel lock. It is named, not
// an implicit conversion, so no other write can bypass the lock unnoticed;
// the package that takes UDP or TCP off the lock must replace it.
extern smp::RankedSpinLock<smp::LockRank::runtime_status> runtime_status_lock;
class RuntimeFailure final
{
  public:
    constexpr RuntimeFailure() = default;
    RuntimeFailure(const RuntimeFailure &) = default;
    RuntimeFailure &operator=(const RuntimeFailure &other)
    {
        store(other.value_);
        return *this;
    }
    template <typename T>
        requires std::convertible_to<const T &, Status>
    RuntimeFailure &operator=(const T &value)
    {
        store(value);
        return *this;
    }
    void store(const Status &value)
    {
        const decltype(runtime_status_lock)::Guard guard(runtime_status_lock);
        store_locked(value);
    }
    // The caller holds runtime_status_lock.
    void store_locked(const Status &value)
    {
        checked() = value;
    }
    // Writes through this reference skip runtime_status_lock (see above).
    [[nodiscard]] Status &bkl_sink()
    {
        return checked();
    }
    operator const Status &() const
    {
        return value_;
    }
    // `return runtime_failure;` from a Result-returning function.
    template <typename T> operator Result<T>() const
    {
        return Result<T>{value_};
    }
    [[nodiscard]] bool ok() const
    {
        return value_.ok();
    }
    [[nodiscard]] StatusCode code() const
    {
        return value_.code();
    }
    [[nodiscard]] std::string_view message() const
    {
        return value_.message();
    }
    [[nodiscard]] explicit operator bool() const
    {
        return value_.ok();
    }

  private:
    Status &checked()
    {
        static_cast<void>(smp::require_exclusive("runtime_failure"));
        return value_;
    }
    Status value_{};
};
class RuntimeFlag final
{
  public:
    constexpr RuntimeFlag() = default;
    RuntimeFlag(const RuntimeFlag &other) noexcept : value_(static_cast<bool>(other)) {}
    RuntimeFlag &operator=(const RuntimeFlag &other)
    {
        return *this = static_cast<bool>(other);
    }
    RuntimeFlag &operator=(bool value)
    {
        static_cast<void>(smp::require_exclusive("runtime started"));
        value_.store(value, std::memory_order_release);
        return *this;
    }
    operator bool() const
    {
        return value_.load(std::memory_order_acquire);
    }

  private:
    std::atomic<bool> value_{};
};
extern RuntimeFlag started;
// Records the runtime's failure and stops it, both under runtime_status_lock.
void fail_runtime(const Status &status);
// Kernel sections (src/user/runtime/section.hpp) are compiled in: x86_64,
// aarch64 and rv64 with option smp_sections.
#if defined(OK_SMP_SECTIONS)
inline constexpr bool smp_sections = true;
#else
inline constexpr bool smp_sections = false;
#endif
// Holding only the kernel lock: commits and waits for `pid`'s kernel
// sections when any is pinned (src/user/runtime/section.cpp). The check
// comes first, so a caller holding fine locks with nothing pinned passes.
void drain_if_pinned(sched::ProcessId pid);
extern Kernel *runtime_kernel;
extern RuntimeFailure runtime_failure;
extern ShutdownOwner original_shutdown_owner;
extern bool shutdown_admission_closed, shutdown_retirement_started, shutdown_retirement_completed;
// Per CPU: inside step(). Whether its last step found nothing to run is the
// scheduler's idle flag (SchedulerState::enter_idle).
extern std::array<bool, smp::max_cpus> runtime_step_active;
bool any_step_active();
// A CPU leaving (hotplug): the legacy scan's cursor starts fresh on return.
void forget_cpu_cursor(smp::CpuId cpu);
// The calling CPU's service stack: the linked boot stack on CPU 0, the stack
// bring-up allocated on a secondary.
Status require_cpu_service_stack();
extern std::atomic<bool> secondary_admission;
// The backend entry for the shared capture: leaves the kernel lock while this
// CPU runs user code (src/user/runtime/smp.cpp).
Result<hw::Trap> enter_unlocked(hw::AddressSpace &space, hw::UserFrame &frame, const hw::KernelEntryStackView &view,
                                memory::KernelEntryStackHandle lease, u64 pid, u64 tid);
// Group transitions (spec 4.8, smp.cpp). The frozen check of a kernel lock
// acquisition made for a thread of `pid` (smp::bkl_frozen_check).
bool group_frozen(u64 pid) noexcept;
// Claims exec or exit for `claimer` and waits, the kernel lock released,
// until no other thread of the group runs on a CPU.
sched::QuiesceResult quiesce_group(Kernel &kernel, sched::ProcessId pid, sched::ThreadId claimer,
                                   sched::GroupTransition kind, sched::ExitRequest request = {},
                                   std::optional<sched::ExitRequest> *owed = nullptr);
// A kill or fatal decision that never waits: pending_exit, probes, queue
// entries for parked threads and kicks for running ones (spec 4.8).
void request_group_exit(Kernel &kernel, sched::ProcessId pid, sched::ExitRequest request);
// finish's quiesce: false when the exit is left to the group (another exit
// claims it, an exec runs, or the caller is a thread of another process and
// the group exits at its next pick).
// A kill accepted before the exit claim replaces `request` with its status.
bool quiesce_for_exit(Kernel &kernel, sched::ProcessId pid, sched::ExitRequest &request);
// On a CPU that runs no thread of `pid` and holds the kernel lock: waits,
// releasing it between checks, until the group another CPU exits completed.
// False on one CPU or without the kernel lock (nothing would end the wait).
bool wait_group_completed(Kernel &kernel, sched::ProcessId pid);
// The status a recorded kill or fatal decision owes: the signal state names
// its signal (SIGKILL wins) and is consumed.
sched::ExitRequest owed_exit(sched::ProcessId pid, sched::ExitRequest request);
// Every lease of a quiesced group is inactive.
Status validate_quiesced_leases(std::span<const sched::ThreadControlBlock> threads);
// The delivery point (spec 4.8): a picked thread of a process with a pending
// exit exits the group with the recorded status. False when none is pending.
bool deliver_pending_exit(Kernel &kernel, sched::ProcessId pid);
// User CPU hotplug and affinity (hotplug.cpp). A thread's finished hotplug
// request: the stored result, consumed once by the step's resume chain when
// the woken writer is dispatched again (never for a newly trapped syscall).
std::optional<i64> cpu_control_resume(const sched::ThreadControlBlock &thread);
// A thread retires, or exec replaces its image: its armed or done record
// goes, a running one is abandoned to the service.
void cpu_control_retire(const sched::ThreadControlBlock &thread);
void cpu_control_retire(memory::KernelEntryStackHandle lease, sched::ProcessId pid, sched::ThreadId tid);
i64 sched_affinity_get(Kernel &kernel, sched::ProcessId pid, u64 target, u64 length, uptr output);
i64 sched_affinity_set(Kernel &kernel, sched::ProcessId pid, u64 target, u64 length, uptr input);
i64 cpu_number(Kernel &kernel, sched::ProcessId pid, uptr cpu, uptr node);
void report_affinity_broken(Kernel &kernel);
void report_cpu_hotplug();
#ifdef OK_PRIVATE_MIPS_O32_SIGNAL
// The backend owns capability/capture tokens; this table owns per-lease state.
using PrivateMipsReadiness = MipsO32SignalReadiness;
extern std::array<PrivateMipsReadiness, memory::KernelEntryStackPool::capacity> private_mips_states;
bool private_mips_reset(const sched::ThreadControlBlock &thread);
bool private_mips_initial(const sched::ThreadControlBlock &thread);
bool private_mips_inherit(const sched::ThreadControlBlock &parent, const sched::ThreadControlBlock &child);
bool private_mips_begin(const sched::ThreadControlBlock &thread);
bool private_mips_capture(const sched::ThreadControlBlock &thread);
void private_mips_revoke(const sched::ThreadControlBlock &thread);
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
using PrivateN64Readiness = Mips64N64SignalReadiness;
namespace private_n64_backend = hw::mips64_n64_private;
struct PrivateN64State
{
    PrivateN64Readiness readiness{};
    u64 image_generation{};
};
extern std::array<PrivateN64State, memory::KernelEntryStackPool::capacity> private_n64_states;
PrivateN64Readiness::Identity private_n64_identity(const sched::ProcessControlBlock &process,
                                                 const sched::ThreadControlBlock &thread);
PrivateN64Readiness *private_n64_state(const sched::ProcessControlBlock &process,
                                     const sched::ThreadControlBlock &thread);
void private_n64_revoke_slot(u32 slot);
void private_n64_revoke(const sched::ThreadControlBlock &thread);
bool private_n64_initial(const sched::ProcessControlBlock &process, const sched::ThreadControlBlock &thread);
bool private_n64_inherit(const sched::ProcessControlBlock &parent_process,
                         const sched::ThreadControlBlock &parent,
                         const sched::ProcessControlBlock &child_process,
                         const sched::ThreadControlBlock &child);
bool private_n64_begin(const sched::ProcessControlBlock &process, const sched::ThreadControlBlock &thread,
                       const hw::KernelEntryStackView &view);
bool private_n64_capture(const sched::ProcessControlBlock &process, const sched::ThreadControlBlock &thread,
                         const hw::KernelEntryStackView &view);
Status private_n64_completion(const sched::ProcessControlBlock &process, const hw::UserFrame &frame, i64 result);
bool private_n64_space_update(sched::ProcessResources &resources, hw::AddressSpace previous,
                              hw::AddressSpace next, bool commit);
#endif
#ifdef OK_PRIVATE_PPC32_SIGNAL
using PrivatePpcReadiness = Ppc32PersistentReadiness;
namespace private_ppc_backend = hw::ppc32_private;
struct PrivatePpcState
{
    PrivatePpcReadiness readiness{};
    Ppc32ImageAdmission image{};
    Ppc32OwnedLease lease{};
    hw::UserFrame initial_frame{};
    hw::AddressSpace staged_space{};
    u64 image_generation{};
    bool initial{}, initial_relocation{}, initial_space{};
};
extern std::array<PrivatePpcState, memory::KernelEntryStackPool::capacity> private_ppc_states;

PrivatePpcReadiness::Identity private_ppc_identity(const sched::ProcessControlBlock &process,
                                                 const sched::ThreadControlBlock &thread);
PrivatePpcReadiness *private_ppc_state(const sched::ProcessControlBlock &process,
                                     const sched::ThreadControlBlock &thread);
void private_ppc_revoke_slot(u32 slot);
void private_ppc_revoke(const sched::ThreadControlBlock &thread);
bool private_ppc_initial(const sched::ProcessControlBlock &process, const sched::ThreadControlBlock &thread,
                         Ppc32ImageAdmission image);
bool private_ppc_inherit(const sched::ProcessControlBlock &parent_process, const sched::ThreadControlBlock &parent,
                         const sched::ProcessControlBlock &child_process, const sched::ThreadControlBlock &child,
                         bool clone);
bool private_ppc_begin(const sched::ProcessControlBlock &process, const sched::ThreadControlBlock &thread,
                       const hw::KernelEntryStackView &view);
bool private_ppc_capture(const sched::ProcessControlBlock &process, const sched::ThreadControlBlock &thread,
                         const hw::KernelEntryStackView &view);
Status private_ppc_completion(const sched::ProcessControlBlock &process, const hw::UserFrame &frame, i64 result);

void private_ppc_cancel_transactions();
bool private_ppc_space_update(sched::ProcessResources &resources, hw::AddressSpace previous,
                              hw::AddressSpace next, bool commit);
#endif
struct QuarantinedEntryStack
{
    memory::KernelEntryStackOwner owner{};
    memory::KernelEntryStackHandle handle{};
};
extern std::array<QuarantinedEntryStack, memory::KernelEntryStackPool::capacity> stranded_entry_stacks;
// For markers emitted when a preemption interrupted user output: starts a new
// line first, so the marker never joins a partial user line.
void emit_line(std::string_view text);
#ifdef OK_PRIVATE_POLL_MASK
extern PollSignalMasks poll_signal_masks;
extern SignalWaitState<lu::configured_native_profile()> signal_waits;

using PrivateSignalWaitState = SignalWaitState<lu::configured_native_profile()>;
void emit(std::string_view text);
void emit_number(u64 value);
void private_signal_wait_trace_retire(const PrivateSignalWaitState::Record &r, std::string_view cause);
void private_signal_thread_retire(sched::ProcessId pid, sched::ThreadId tid, memory::KernelEntryStackHandle lease);
#endif
Status cancel_native_vfs_thread(const sched::ThreadControlBlock &thread);
memory::ObjectHandle linux_udp_receive_object(sched::ProcessId pid, sched::ThreadId tid);
Status retire_native_thread(sched::ThreadControlBlock &thread
#ifdef OK_PRIVATE_POLL_MASK
                            , std::string_view signal_wait_cause = "thread"
#endif
);
Status prepare_native_thread(sched::ThreadControlBlock &thread);
// Registrations retain open descriptions and a copied request until completion.
// Only the serialized boot-CPU runtime owns these records; no user pointers are
// dereferenced after blocking except for the validated completion destination.
struct PollWait
{
    u64 syscall{};
    uptr resume{};
    sched::ProcessId pid{};
    sched::ThreadId tid{};
    sched::WaitTicket ticket{};
    u32 console_epoch{};
    uptr output{};
    usize count{};
    std::array<ok_pollfd, sched::max_process_fds> fds{};
    std::array<u16, sched::max_process_fds> descriptions{};
    sched::DeadlineWait timeout{};
    PollTimeoutOutput timeout_output{};
    bool big_endian{std::endian::native == std::endian::big};
    u16 read_normal{}, write_normal{};
    bool linux_events{};
    bool select_sets{};
    SelectSets sets{};
    std::array<u32, 3> ready_sets{};
    // Set by poll_ready(): the device source of a ready console or socket.
    sched::WakeSource ready_source{sched::WakeSource::thread};
    // Set by its last arm: a watched source has no channel entry (no channel
    // covers it, or the pool was exhausted) and a timer re-evaluates it.
    bool unwatched{};
};
using PollObjectsLayout = memory::ObjectPool<PollWait, NativePagePool, 1>;
constexpr usize poll_object_pages = (sched::max_processes + PollObjectsLayout::slots_per_page - 1) /
                                   PollObjectsLayout::slots_per_page;
using PollObjects = memory::ObjectPool<PollWait, NativePagePool, poll_object_pages>;
extern PollObjects poll_objects;
struct PollWaitIndex
{
    sched::ProcessId pid{};
    sched::ThreadId tid{};
    memory::ObjectHandle object{};
};
// This bounded index contains identities only; the records live in owned pages.
extern std::array<PollWaitIndex, memory::KernelEntryStackPool::capacity> poll_waits;
Status activate_pool(Kernel *kernel = nullptr);
void emit(std::string_view text);
void emit_number(u64 value);

struct FutexWait
{
    sched::ProcessId pid{};
    sched::ThreadId tid{};
    uptr address{};
    FutexKeyDomain domain{FutexKeyDomain::private_address_space};
    struct Key
    {
        FutexKeyDomain domain{FutexKeyDomain::private_address_space};
        sched::ProcessId pid{};
        uptr address{};
        memory::PhysicalFrame frame{};
        usize word_offset{};
        bool shared{};
    } key{};
    bool pin_owned{};
    memory::PhysicalFrame pending_transfer_frame{};
    bool pending_transfer_pin_owned{};
    // Queue position (futex_buckets.hpp): guarded by the bucket that lists
    // the wait; `bucket` (futex_no_bucket when unlisted) is also loaded by
    // the owner without a lock. A waker never retires the record.
    FutexWait *bucket_prev{}, *bucket_next{};
    u32 bucket{futex_no_bucket};
    u64 queue_sequence{};
    sched::WaitTicket ticket{};
    sched::DeadlineWait timeout{};
    u32 expected{};
    u64 syscall{}, origin{};
    uptr timeout_address{};
};
SyscallOrigins::FutexBinding futex_origin_binding(memory::ObjectHandle object, const FutexWait &wait);
using FutexObjectsLayout = memory::ObjectPool<FutexWait, NativePagePool, 1>;
constexpr usize futex_object_pages =
    (sched::max_processes + FutexObjectsLayout::slots_per_page - 1) / FutexObjectsLayout::slots_per_page;
using FutexObjects = memory::ObjectPool<FutexWait, NativePagePool, futex_object_pages>;
extern FutexObjects futex_objects;
extern std::array<PollWaitIndex, memory::KernelEntryStackPool::capacity> futex_waits;
// The 64 futex hash buckets (spec 4.5): FineLock<futex_bucket> each.
using FutexBucketTable = FutexBuckets<FutexWait, smp::FineLock<smp::LockRank::futex_bucket>>;
extern FutexBucketTable futex_buckets;
[[nodiscard]] u32 futex_bucket_of(const FutexWait::Key &key);

// A blocking read owns the original request and open description. A
// descriptor number is only a name in a mutable shared table, not ownership.
struct PipeReadWait
{
    sched::ProcessId pid{};
    sched::ThreadId tid{};
    sched::WaitTicket ticket{};
    u64 request{};
    uptr output{};
    usize count{};
    i32 fd{}; // Diagnostic only; never resolve this name after blocking.
    u16 description{sched::invalid_description};
    u16 pipe{sched::invalid_description};
    PipeVectors vectors{};
};
SyscallOrigins::PipeBinding pipe_origin_binding(memory::ObjectHandle object, const PipeReadWait &wait);
constexpr usize pipe_read_capacity = memory::KernelEntryStackPool::capacity;
using PipeReadObjectsLayout = memory::ObjectPool<PipeReadWait, NativePagePool, 1>;
constexpr usize pipe_read_object_pages =
    (pipe_read_capacity + PipeReadObjectsLayout::slots_per_page - 1) / PipeReadObjectsLayout::slots_per_page;
using PipeReadObjects = memory::ObjectPool<PipeReadWait, NativePagePool, pipe_read_object_pages>;
extern PipeReadObjects pipe_read_objects;
extern std::array<PollWaitIndex, pipe_read_capacity> pipe_read_waits;
// Writes retain the copied request and original open description across waits.
struct PipeWriteWait
{
    sched::ProcessId pid{};
    sched::ThreadId tid{};
    sched::WaitTicket ticket{};
    u64 request{};
    u64 origin_capture{}; // Admission authority survives a later origin revocation.
    uptr input{};
    usize count{};
    usize committed{};
    i32 fd{}; // Diagnostic only; never resolve this name after blocking.
    u16 description{sched::invalid_description};
    u16 pipe{sched::invalid_description};
    PipeVectors vectors{};
};
SyscallOrigins::PipeWriteBinding pipe_write_origin_binding(memory::ObjectHandle object, const PipeWriteWait &wait);
constexpr usize pipe_write_capacity = memory::KernelEntryStackPool::capacity;
using PipeWriteObjectsLayout = memory::ObjectPool<PipeWriteWait, NativePagePool, 1>;
constexpr usize pipe_write_object_pages =
    (pipe_write_capacity + PipeWriteObjectsLayout::slots_per_page - 1) / PipeWriteObjectsLayout::slots_per_page;
using PipeWriteObjects = memory::ObjectPool<PipeWriteWait, NativePagePool, pipe_write_object_pages>;
extern PipeWriteObjects pipe_write_objects;
extern std::array<PollWaitIndex, pipe_write_capacity> pipe_write_waits;
struct PtyWait
{
    sched::ProcessId pid{};
    sched::ThreadId tid{};
    sched::WaitTicket ticket{};
    u64 request{};
    hw::AddressSpace space{};
    memory::KernelEntryStackHandle lease{};
    hw::UserFrame frame{};
    uptr address{};
    usize count{}, committed{};
    i32 fd{}; // Diagnostic only after admission.
    u16 description{sched::invalid_description};
    sched::private_pty::Handle endpoint{};
    PipeVectors vectors{};
    bool writing{};
    Personality personality{Personality::native}; // Captured at admission; the owner must keep it.
    u64 origin{};          // Linux syscall origin capture that bound this wait (0: none).
    u64 deadline{};        // VTIME expiry (monotonic ns), 0 when no timer runs.
    u64 received_mark{};   // Slave bytes received when the interbyte timer was last armed.
    u8 timer{};            // Armed VTIME length in deciseconds.
    bool interbyte{};      // The timer restarts on new input (VMIN > 0).
    sched::private_pty::ReadTimer vtime{}; // VTIME captured when the read first waited.
};
// A blocked PTY write would move a byte now or fail at once (signal readiness).
bool pty_write_ready(Kernel &kernel, const PtyWait &wait);
SyscallOrigins::PtyReadBinding pty_read_origin_binding(memory::ObjectHandle object, const PtyWait &wait);
SyscallOrigins::PtyWriteBinding pty_write_origin_binding(memory::ObjectHandle object, const PtyWait &wait);
constexpr usize pty_wait_capacity = memory::KernelEntryStackPool::capacity;
using PtyWaitObjectsLayout = memory::ObjectPool<PtyWait, NativePagePool, 1>;
constexpr usize pty_wait_object_pages =
    (pty_wait_capacity + PtyWaitObjectsLayout::slots_per_page - 1) / PtyWaitObjectsLayout::slots_per_page;
using PtyWaitObjects = memory::ObjectPool<PtyWait, NativePagePool, pty_wait_object_pages>;
extern PtyWaitObjects pty_wait_objects;
extern std::array<PollWaitIndex, pty_wait_capacity> pty_waits;
struct ChildWait
{
    sched::ProcessId pid{};
    sched::ThreadId tid{};
    sched::WaitTicket ticket{};
    u64 request{};
    ChildWaitRequest operation{};
};
SyscallOrigins::ChildBinding child_origin_binding(memory::ObjectHandle object, const ChildWait &wait);
constexpr usize child_wait_capacity = memory::KernelEntryStackPool::capacity;
using ChildWaitObjectsLayout = memory::ObjectPool<ChildWait, NativePagePool, 1>;
constexpr usize child_wait_object_pages =
    (child_wait_capacity + ChildWaitObjectsLayout::slots_per_page - 1) / ChildWaitObjectsLayout::slots_per_page;
using ChildWaitObjects = memory::ObjectPool<ChildWait, NativePagePool, child_wait_object_pages>;
extern ChildWaitObjects child_wait_objects;
extern std::array<PollWaitIndex, child_wait_capacity> child_waits;
enum class NativeUdpOwnerPhase : u8 { staged, published, taken };
struct NativeUdpReceiveWait
{
    sched::ProcessId pid{};
    sched::ThreadId tid{};
    NativeUdpOwnerPhase phase{NativeUdpOwnerPhase::staged};
    SyscallOrigins::NativeUdpCapture capture;
    SyscallOrigins::NativeUdpReceiveRequest operation{};
    u32 endpoint{};
    u16 retained_description{sched::invalid_description};
    sched::WaitTicket ticket{};
    u64 request{}, observed_receive{}, observed_executor{};
    std::optional<SyscallOrigins::NativeUdpReceiveBinding> origin_binding;

    NativeUdpReceiveWait(sched::ProcessId owner, sched::ThreadId thread,
        SyscallOrigins::NativeUdpCapture captured,
        SyscallOrigins::NativeUdpReceiveRequest copied, u32 handle) noexcept
        : pid(owner), tid(thread), capture(captured), operation(copied), endpoint(handle) {}
};
constexpr usize native_udp_receive_capacity = memory::KernelEntryStackPool::capacity;
using NativeUdpReceiveLayout = memory::ObjectPool<NativeUdpReceiveWait, NativePagePool, 1>;
constexpr usize native_udp_receive_pages =
    (native_udp_receive_capacity + NativeUdpReceiveLayout::slots_per_page - 1) /
    NativeUdpReceiveLayout::slots_per_page;
using NativeUdpReceiveObjects = memory::ObjectPool<NativeUdpReceiveWait, NativePagePool, native_udp_receive_pages>;
extern NativeUdpReceiveObjects native_udp_receive_objects;
extern std::array<PollWaitIndex, native_udp_receive_capacity> native_udp_receive_waits;
using LinuxUdpReceiveWait = LinuxUdpReceiveOwner<lu::configured_native_profile()>;
using LinuxUdpReceiveStore = LinuxUdpReceiveOwnerStore<lu::configured_native_profile(), NativePagePool>;
constexpr usize linux_udp_receive_pages = LinuxUdpReceiveStore::pages;
extern LinuxUdpReceiveStore linux_udp_receive_store;
inline constinit auto &linux_udp_receive_objects=linux_udp_receive_store.objects();
inline constinit auto &linux_udp_receive_waits=linux_udp_receive_store.members();
Status retire_linux_udp_receives(Kernel &kernel, sched::ProcessId pid,
    std::optional<sched::ThreadId> tid, LinuxUdpReceiveRetirement cause);

[[maybe_unused]] memory::ObjectHandle futex_object(sched::ProcessId pid, sched::ThreadId tid);
Status cancel_futex(Kernel &kernel, memory::ObjectHandle object, std::string_view reason = "cancelled", i64 result = -4);
Status cancel_futex(Kernel &kernel, sched::ProcessId pid);
// Exec's check, before it evicts siblings, that cancel_futex(pid) will succeed.
Status prepare_cancel_futex(sched::ProcessId pid);

[[maybe_unused]] memory::ObjectHandle poll_object(sched::ProcessId pid, sched::ThreadId tid);
u64 poll_syscall_number(const hw::UserFrame &frame, bool select_sets);
void cancel_poll(Kernel &kernel, memory::ObjectHandle object, std::string_view reason = "cancelled", i64 result = -4);

void cancel_poll(Kernel &kernel, sched::ProcessId pid);
void prune_poll_waits(Kernel &kernel);

Result<usize> poll_ready(Kernel &kernel, PollWait &wait);

bool poll_output(const hw::AddressSpace &space, const PollWait &wait);
extern "C" int ok_platform_console_poll() __attribute__((weak));
void emit(std::string_view text);
void emit_number(u64 value);
[[maybe_unused]] void emit_hex(u64 value);
void emit_entry_stack_stats();
void emit_pool_stats();
Status activate_pool(Kernel *kernel);
#ifdef OK_ENABLE_TEST_POINTS
Status check_runtime_objects();
#endif
bool same_space(const hw::AddressSpace &a, const hw::AddressSpace &b);
// Counts translation_writer_asserts when a translation writer on a published
// space runs without its owner's mm.
void check_translation_writer(const sched::ProcessResources &resources, const char *site);
// A user page fault of the process that owns `resources`, under its mm: a
// write to a withheld page, or a fault on a translation replaced meanwhile.
// True when the thread may resume.
bool resolve_user_fault(sched::ProcessResources &resources, const hw::Trap &trap, u32 translations);
// Process teardown under its mm: retires the space and every frame, then
// marks the address space dead.
void release_address_space(sched::ProcessResources &resources);
const memory::PhysicalFrame *owned_frame(const Pages &pages, const hw::PageMapping &page);
Status discharge_quarantine(QuarantinedSpace &saved, bool all = false, uptr begin = 0, usize size = 0);
Status finish_quarantine(QuarantinedSpace &saved, bool retry_owners = true);
Status retry_quarantined_retirement();
void release(hw::AddressSpace &space, Pages &pages);
Status allocate(Pages &pages, uptr address, u8 flags, bool shared = false);
// Native VM retains allocation handles in the canonical registry. Backends only
// receive accessible mappings; PROT_NONE retains backing without a user PTE.
Result<hw::AddressSpace> create_page_space(const Pages &pages);
bool shared_pool_page(const Pages &pages, const hw::PageMapping &page);

Status store(Pages &pages, uptr address, const void *data, usize count);

Result<uptr> native_mmap(sched::ProcessResources &resources, uptr hint, usize length,
                         uptr protection, uptr flags, uptr fd, uptr offset,
                         fs::Node *source = nullptr, usize file_size = 0, SharedFileHandle shared_file = {});

Status native_mprotect(sched::ProcessResources &resources, uptr address, usize length, uptr protection);

Status native_munmap(sched::ProcessResources &resources, uptr address, usize length);

Result<uptr> native_brk(sched::ProcessResources &resources, uptr address);

Status store(Pages &pages, uptr address, const void *data, usize count);
Status string_from_user(const hw::AddressSpace &space, uptr address, FixedString<256> &result);
i64 error(Status status);
Status encode_result(const sched::ProcessControlBlock &process, hw::UserFrame &frame, i64 result);
Status set_result(const sched::ProcessControlBlock &process, hw::UserFrame &frame, i64 result);
[[maybe_unused]] void emit_pipe_vectors(const PipeVectors &vectors);
bool pipe_ranges(const hw::AddressSpace &space, uptr address, usize count, const PipeVectors &vectors, bool output);
Status pipe_scatter(const hw::AddressSpace &space, uptr address, std::span<const std::byte> bytes, const PipeVectors &vectors);
memory::ObjectHandle pipe_read_object(sched::ProcessId pid, sched::ThreadId tid);
void cancel_pipe_read(Kernel &kernel, memory::ObjectHandle object,
                      std::string_view outcome = "cancelled", i64 result = -125);
void cancel_pipe_read(Kernel &kernel, sched::ProcessId pid);
void prune_pipe_reads(Kernel &kernel);
i64 resume_pipe_read(Kernel &kernel, sched::ProcessControlBlock &process,
                     memory::ObjectHandle object, bool &complete);
i64 begin_pipe_read(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadId tid,
                    i32 fd, u16 description_id, uptr output, usize count, bool &complete, const PipeVectors &vectors = {});

// Signal policy uses the ordinary serialized native signal service.
void queue_pipe_sigpipe(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadId tid);

memory::ObjectHandle pipe_write_object(sched::ProcessId pid, sched::ThreadId tid);
void cancel_pipe_write(Kernel &kernel, memory::ObjectHandle object,
                      std::string_view outcome = "cancelled", i64 result = -125);
void cancel_pipe_write(Kernel &kernel, sched::ProcessId pid);
void prune_pipe_writes(Kernel &kernel);
i64 pipe_write_terminal_result(const PipeWriteWait &wait, i64 failure);
i64 resume_pipe_write(Kernel &kernel, sched::ProcessControlBlock &process,
                     memory::ObjectHandle object, bool &complete);
i64 begin_pipe_write(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadId tid,
                    i32 fd, u16 description_id, uptr input, usize count, bool &complete, const PipeVectors &vectors = {});
// PTY reads and writes of both personalities. A Linux wait binds its syscall
// origin when armed, so a caught signal can interrupt or restart it.
memory::ObjectHandle pty_wait_object(sched::ProcessId pid, sched::ThreadId tid);
void cancel_pty_waits(Kernel &kernel, sched::ProcessId pid, sched::ThreadId tid = 0);
// Retires one PTY wait object (the signal family's interruption).
void cancel_pty_wait(Kernel &kernel, memory::ObjectHandle object, std::string_view outcome = "cancelled", i64 result = -125);
void prune_pty_waits(Kernel &kernel);
i64 resume_pty_io(Kernel &kernel, sched::ProcessControlBlock &process,
                  memory::ObjectHandle object, bool &complete);
i64 begin_pty_io(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadId tid,
                 i32 fd, u16 description_id, uptr address, usize count, bool writing,
                 bool &complete, const PipeVectors &vectors = {});
// Terminals (tty.cpp). Console ingress runs the line discipline and emits its
// echo; writes go through OPOST and return the full count (at most 64 KiB per
// call); reads copy out before they consume. Every path drains the terminal
// outbox, as do the top of step() and the exit commit.
void tty_console_input(Kernel &kernel, char value, sched::WakeSource source);
// Writes check TOSTOP first; `fd` and `complete` serve a job-control park.
// A description opened through /dev/console skips the job-control check of
// reads and writes (Linux redirected_tty_write).
i64 tty_console_write(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadControlBlock &caller,
                      const sched::OpenFileDescription &description, std::span<const PipeVector> vectors, i32 fd,
                      bool &complete);
// `arguments`/`vectored`: the read or readv call a Linux wait binds.
i64 tty_console_read(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadControlBlock &caller,
                     const sched::OpenFileDescription &description, std::span<const PipeVector> vectors,
                     bool &complete, const std::array<uptr, 6> &arguments, bool vectored);
// Native terminal ioctls (struct ok_termios); Linux uses TtyIoctlCodec in linux_bootstrap.hpp.
i64 tty_terminal_control(Kernel &kernel, sched::ProcessControlBlock &process, const std::array<uptr, 6> &arguments,
                         bool &complete);
// The job-control gate (docs/modules/tty.md section 9.6, Linux
// __tty_check_change). It acts only when `ref` is the caller's controlling
// terminal, has a foreground group and the caller is not in it. A write is
// checked only under TOSTOP. `park`: the caller's group got SIGTTIN/SIGTTOU
// and the call waits in ThreadWaitReason::job_control until a continue, a
// foreground change or a signal ends the park; it then re-dispatches.
enum class TtyAccess : u8 { read, write, change };
enum class TtyGate : u8 { go, io_error, park };
TtyGate tty_check(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread,
                  sched::TerminalRef ref, TtyAccess access);
// Parks the running `thread` (would_block with `parked` set).
Status tty_park(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread,
                sched::TerminalRef ref, i32 fd, bool &parked);
// tty_check then tty_park: {} to go on, io_error, or the park.
Status tty_gate(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread,
                sched::TerminalRef ref, TtyAccess access, i32 fd, bool &parked);
// Sends queued terminal signals; true when any outbox held entries.
bool tty_drain_outbox(Kernel &kernel);

memory::ObjectHandle child_wait_object(sched::ProcessId pid, sched::ThreadId tid);
// Return a ready child PID, ECHILD, EFAULT, zero for WNOHANG, or EAGAIN
// exclusively for a matching live child. Never retain a canonical PCB pointer.
i64 child_wait_probe(Kernel &kernel, sched::ProcessId pid, const ChildWaitRequest &request,
                     ChildWaitReport *report = nullptr);
void cancel_child_wait(Kernel &kernel, memory::ObjectHandle object,
                       std::string_view outcome = "cancelled", i64 result = -125);
void cancel_child_wait(Kernel &kernel, sched::ProcessId pid);
void prune_child_waits(Kernel &kernel);
i64 resume_child_wait(Kernel &kernel, sched::ProcessControlBlock &process, memory::ObjectHandle object, bool &complete);
memory::ObjectHandle native_udp_receive_object(sched::ProcessId pid, sched::ThreadId tid);
void retire_native_udp_receives(Kernel &kernel, sched::ProcessId pid,
                               std::optional<sched::ThreadId> tid = {});
Status retire_native_udp_receives_for_exec(Kernel &kernel, sched::ProcessId pid);
// Exec's check, before it evicts siblings, that the retirement above will succeed.
Status prepare_native_udp_receives_for_exec(sched::ProcessId pid);
i64 resume_native_udp_receive(Kernel &kernel, sched::ProcessControlBlock &process,
                             memory::ObjectHandle object, bool &complete);
Status scan_native_udp_receives(Kernel &kernel);
Status notify_udp_receives(Kernel &kernel);
// Socket receive waits enlisted in their socket's queue, readiness
// evaluations, and waits that found their queue full (forcing rescans).
struct UdpWaitCounters { u64 queued{}, evaluated{}, unqueued{}; };
UdpWaitCounters udp_wait_statistics();
// The kernel's TCP wait counters, bound when the runtime starts.
extern const net::TcpWaitStats *tcp_wait_stats;
// The kernel's datagram transmit backlog (OK_NET_TX), bound when the runtime starts.
extern const net::TransmitBacklog<> *transmit_backlog_stats;
void report_network_waits();
// A UDP socket's poll channel (udp_receive, its slot): the receive-change
// pass wakes it for each socket whose readiness moved, and every socket when
// the executor's did (transmit credit, link failure).
[[nodiscard]] constexpr u64 udp_poll_channel(u32 endpoint) { return endpoint & 0xffu; }
// The owned result of a thread's completed poll, futex WAIT or sleep,
// settled at its dispatch (spec 4.4); empty when it has none and dispatch
// goes on.
std::optional<i64> resume_owned_wait(Kernel &kernel, sched::ProcessControlBlock &process,
                                     sched::ThreadControlBlock &thread, bool &complete);
// A completed futex WAIT's result (spec 4.4): ready 0, timeout -110, any
// other completion -4. The wait leaves its queue if it is still listed (a
// timeout or a cancel left it there) and its record is retired. Empty when
// the record is not this continuation's (it is retired) or the runtime
// stopped.
std::optional<i64> resume_futex(Kernel &kernel, sched::ProcessControlBlock &process,
                                sched::ThreadControlBlock &thread, memory::ObjectHandle object);
// The poll audit's probed waiter (spec 4.10): readiness no wake reported
// completes its wait and counts poll_audit_ready.
void audit_probed_poll(Kernel &kernel, sched::ThreadRef chosen);
// Installs the lock-violation recorder and arms require_exclusive; once,
// just before secondary CPUs are admitted (src/user/runtime/interrupts.cpp).
void arm_kernel_checks();
RobustWalkReport retire_robust(Kernel &kernel, sched::ProcessControlBlock &process,
                              sched::ThreadLocalState &tls, sched::ThreadId owner);
void clear_child_tid(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadLocalState &tls, bool peers);
void clear_group_tids(Kernel &kernel, sched::ProcessControlBlock &process);
void finish(Kernel &kernel, sched::ProcessId pid, i32 code, u8 signal = 0);
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64) || defined(__mips__) || defined(__powerpc__) || defined(__PPC__))
// Ordinary bounded boot-CPU service; diagnostics have a separate Debug gate.
inline constexpr auto private_signal_profile = lu::configured_native_profile();
using PrivateSignalQueue = sched::PendingSignals<SignalActionCodec<private_signal_profile>::max_signal>;
using PrivateProcessQueue = ProcessPendingSignals<SignalActionCodec<private_signal_profile>::max_signal>;
struct PrivateSignalActions
{
    sched::ProcessId pid{};
    u64 epoch{};
    sched::SignalActionTable table{};
    PrivateProcessQueue shared{};
    bool group_kill{};
    // A fatal default decided at generation (native recipients, and Linux
    // recipients that cannot take it in a thread); applied at the next step.
    u8 deferred_fatal{};
    ChildExitLineage lineage{};
};
extern std::array<PrivateSignalActions, sched::max_processes> private_signal_actions;
extern std::array<PrivateSignalQueue, memory::KernelEntryStackPool::capacity> private_signal_queues;
extern std::array<sched::SignalAltstack, memory::KernelEntryStackPool::capacity> private_signal_altstacks;

using PrivateChildExit = ChildExitLifecycle<private_signal_profile>;
PrivateSignalActions *private_signal_find(sched::ProcessId pid);

PrivateSignalActions *private_signal_process(Kernel &kernel, sched::ProcessId pid);
void private_signal_retire(sched::ProcessId pid);
PrivateSignalQueue *private_signal_queue(const sched::ThreadControlBlock &thread);
void private_signal_thread_retire(sched::ProcessId pid, sched::ThreadId tid, memory::KernelEntryStackHandle lease);
sched::SignalAltstack *private_signal_altstack(const sched::ThreadControlBlock &thread);
Status private_signal_fork(Kernel &kernel, sched::ProcessId parent, sched::ThreadId caller, sched::ProcessId child);
void private_signal_exec(Kernel &kernel, sched::ProcessControlBlock &process, PrivateSignalQueue *previous,
                         sched::SignalAltstack old_stack);
bool private_signal_deliver(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread);
bool private_sleep_signal_ready(Kernel &kernel, const sched::ProcessControlBlock &process,
                                const sched::ThreadControlBlock &thread);
bool private_sleep_signal_complete(Kernel &kernel, sched::ProcessControlBlock &process,
                                   sched::ThreadControlBlock &thread);

bool private_pipe_signal_ready(Kernel &kernel, const sched::ProcessControlBlock &process,
                               const sched::ThreadControlBlock &thread);

bool private_pipe_signal_complete(Kernel &kernel, sched::ProcessControlBlock &process,
                                  sched::ThreadControlBlock &thread);

bool private_pipe_write_signal_ready(Kernel &kernel, const sched::ProcessControlBlock &process,
                               const sched::ThreadControlBlock &thread);

bool private_pipe_write_signal_complete(Kernel &kernel, sched::ProcessControlBlock &process,
                                  sched::ThreadControlBlock &thread);

bool private_child_signal_ready(Kernel &kernel, const sched::ProcessControlBlock &process,
                               const sched::ThreadControlBlock &thread);

bool private_child_signal_complete(Kernel &kernel, sched::ProcessControlBlock &process,
                                  sched::ThreadControlBlock &thread);

bool private_futex_signal_ready(Kernel &kernel, const sched::ProcessControlBlock &process,
                                const sched::ThreadControlBlock &thread);

bool private_futex_signal_complete(Kernel &kernel, sched::ProcessControlBlock &process,
                                   sched::ThreadControlBlock &thread);

bool private_poll_signal_ready(Kernel &kernel, const sched::ProcessControlBlock &process,
                               const sched::ThreadControlBlock &thread);

bool private_poll_signal_complete(Kernel &kernel, sched::ProcessControlBlock &process,
                                  sched::ThreadControlBlock &thread);
bool private_signal_wait_ready(Kernel &kernel, const sched::ProcessControlBlock &process,
                                const sched::ThreadControlBlock &thread, u64 now);
bool private_signal_wait_complete(Kernel &kernel, sched::ProcessControlBlock &process,
                                   sched::ThreadControlBlock &thread, u64 now);
bool private_signal_wait_pending(Kernel &kernel);
i64 native_process_kill(Kernel &kernel, const sched::ProcessControlBlock &sender, u64 raw_pid, u64 raw_signal);
bool private_signal_call(Kernel &kernel, sched::ProcessControlBlock &process,
                         sched::ThreadControlBlock &thread, bool &complete, i64 &result);
// Interval timers (timers.cpp). An expiry signal is sent, dropped as ignored
// or by a departed target, or stays owed to the timer until a later step.
enum class PrivateTimerSend : u8 { sent, ignored, departed, retry };
// `replay`: a retry whose CONT/stop side effects already ran.
PrivateTimerSend private_timer_signal(Kernel &kernel, sched::ProcessControlBlock &process,
                                      sched::ThreadId tid, sched::PendingSignalInfo info, bool replay = false);
bool private_timer_call(Kernel &kernel, sched::ProcessControlBlock &process,
                        sched::ThreadControlBlock &thread, i64 &result);
// Every process's timers (models, and any pass that must see all of them).
bool private_timer_service(Kernel &kernel, u64 now);
// The timers of the process slots whose deadline key expired (spec 4.6);
// each one's key is set to its next expiry again.
void private_timer_service_due(Kernel &kernel, u64 now, std::span<const u16> slots);
void private_timer_exec(sched::ProcessId pid);
void private_timer_discard(Kernel &kernel, sched::ProcessId pid, u32 signal);
// A CONT or stop discarded `signal`: an expiry still owed to its timer goes
// with the queued records (Linux flushed the record it queued at expiry).
void private_timer_discard_owed(sched::ProcessId pid, u32 signal);
void private_timer_thread_exit(sched::ProcessId pid, sched::ThreadId tid, const PrivateSignalQueue &queue);
// The signal's action is ignored now (SIG_IGN, or a default that ignores it).
bool private_signal_ignored(Kernel &kernel, sched::ProcessId pid, u32 signal);
void private_timer_retire(sched::ProcessId pid);
struct PrivateTimerCounters { u64 armed{}, expired{}, signalled{}, dequeued{}, retried{}; };
PrivateTimerCounters private_timer_counters();
Status private_child_exit_commit(Kernel &kernel, PrivateChildExit::Completion &completion);

bool private_child_reap(Kernel &kernel);
// Job control (jobctl.cpp). Delivery consumed a default stop: an orphaned
// group (or init) discards TSTP/TTIN/TTOU, otherwise the process stops and its
// parent is notified. The settle pass applies a pending default stop that only
// threads parked in waits can take, then ends each job-control park of a
// running Linux process whose interrupting signal is gone.
bool private_jobctl_discard_stop(Kernel &kernel, const sched::ProcessControlBlock &process, u32 signal);
void private_jobctl_stop(Kernel &kernel, sched::ProcessControlBlock &process, u32 signal);
void private_jobctl_settle(Kernel &kernel);
void private_terminal_park_settle(Kernel &kernel);
i64 private_signal_wait_enqueue(Kernel &kernel, sched::ProcessControlBlock &recipient, sched::ThreadControlBlock &target,
                                sched::PendingSignalInfo info);
bool private_signal_admit_blocked(Kernel &kernel, const sched::ProcessControlBlock &recipient,
                                  const sched::ThreadControlBlock &target);
// Whether a signal can interrupt a waiting thread's wait (spec 4.7): the
// interruptible class its owner recorded at park; for a parked thread the
// old predicate (private_signal_admit_blocked) runs as its shadow and a
// disagreement counts admit_shadow_mismatch. False for a thread not waiting.
bool private_wait_interruptible(Kernel &kernel, const sched::ProcessControlBlock &process,
                                const sched::ThreadControlBlock &thread);
// Generation's probe (spec 4.7): after a signal was queued for `process`,
// every waiting thread whose wait it may interrupt is probed; a parked one
// is queued so that its resume re-evaluates the wait.
void private_signal_probe(Kernel &kernel, sched::ProcessControlBlock &process);
// Whether a pending signal settles `thread`'s wait at its next dispatch: the
// readiness of the per-family signal completions the resume runs (a signal
// that interrupts the wait, or an ignored one a signal wait discards).
bool private_wait_signal_ready(Kernel &kernel, const sched::ProcessControlBlock &process,
                               const sched::ThreadControlBlock &thread, u64 now);
// The first pending signal `thread` would take that interrupts a wait (not
// ignored, not a default stop), when its action is supported.
bool private_signal_interrupting(Kernel &kernel, const sched::ProcessControlBlock &process,
                                 const sched::ThreadControlBlock &thread, u32 &signal, sched::SignalAction &action);
// Terminal waits of Linux processes (jobctl.cpp): console reads and PTY reads
// and writes whose syscall origin is bound. Data, EOF or room wins over a
// signal; otherwise a caught signal gives EINTR (a partial write its count) or
// a restart under SA_RESTART, and a terminating default ends the process.
bool private_terminal_signal_owned(Kernel &kernel, const sched::ProcessControlBlock &process,
                                   const sched::ThreadControlBlock &thread);
bool private_terminal_signal_ready(Kernel &kernel, const sched::ProcessControlBlock &process,
                                   const sched::ThreadControlBlock &thread);
bool private_terminal_signal_complete(Kernel &kernel, sched::ProcessControlBlock &process,
                                      sched::ThreadControlBlock &thread);
// The generation core (Linux prepare_signal/complete_signal order) shared by
// kill, tkill/tgkill, group kill, process timers and kernel senders. The
// signal in `info` is in the recipient's numbering; callers have done lookup,
// range, permission and the zero probe.
enum class JobSignalOutcome : u8 { accepted, ignored, departed, retry, refused, exhausted, full, invalid, unavailable };
struct JobSignalRequest
{
    sched::PendingSignalInfo info{};
    sched::ThreadId named{};   // Its mask decides ignored generation; 0 names the leader.
    bool directed{};           // Only `named` may take the signal (tkill, tgkill, SIGEV_THREAD_ID).
    bool kernel{};             // A caught signal no thread can take now is retried, not refused.
    bool replay{};             // Owed retry: the CONT/stop side effects already ran.
};
JobSignalOutcome private_generate_signal(Kernel &kernel, sched::ProcessControlBlock &recipient,
                                         const JobSignalRequest &request);
// kill/tkill/tgkill errno of an outcome.
[[nodiscard]] constexpr i64 private_job_signal_result(JobSignalOutcome outcome)
{
    switch (outcome)
    {
    case JobSignalOutcome::accepted: case JobSignalOutcome::ignored: case JobSignalOutcome::departed: return 0;
    case JobSignalOutcome::full: return -11;
    case JobSignalOutcome::invalid: return -22;
    case JobSignalOutcome::exhausted: case JobSignalOutcome::unavailable: return -12;
    default: return -95;
    }
}
// tkill/tgkill admission of a blocked target: job-control signals and stopped
// targets reach the generation core without interrupting the wait.
// Before fork publication: a pending fatal default `thread` can take becomes
// the group's pending exit (spec 4.12; Linux copy_process with a signal
// pending).
void private_fork_condemn(Kernel &kernel, sched::ProcessControlBlock &process, const sched::ThreadControlBlock &thread);
bool private_jobctl_admit_blocked(Kernel &kernel, const sched::ProcessControlBlock &recipient,
                                  const sched::ThreadControlBlock &target, i32 signal);
// Kernel-originated signals (Linux SEND_SIG_PRIV): SI_KERNEL, no permission
// check, `signal` in native numbering and translated per recipient. A caught
// signal that only threads in uninterruptible waits could take is owed and
// retried by private_jobctl_service at every step until a wait completes.
// Exit, pid-slot reuse (epoch change), an exec into another personality,
// SIG_IGN or the opposite CONT/stop drops it; a same-personality exec keeps it.
enum class KernelSignalSend : u8 { sent, ignored, departed, owed };
KernelSignalSend send_kernel_process_signal(Kernel &kernel, sched::ProcessId pid, u32 signal);
struct KernelGroupSignal { u8 matched{}, sent{}, ignored{}, departed{}, owed{}; };
KernelGroupSignal send_kernel_group_signal(Kernel &kernel, sched::ProcessId group, u32 signal);
bool private_jobctl_service(Kernel &kernel);
void private_jobctl_retire(sched::ProcessId pid);
void private_jobctl_discard_owed(sched::ProcessId pid, u32 signal);
// Linux is_ignored(): SIG_IGN or blocked by `thread`; native processes have
// SIG_IGN only. `signal` is in native numbering.
bool private_signal_ignored_or_blocked(Kernel &kernel, const sched::ProcessControlBlock &process,
                                       const sched::ThreadControlBlock &thread, u32 signal);
bool private_group_orphaned(Kernel &kernel, sched::ProcessId group);
// Linux __tty_check_change for a background member of the terminal's session
// (`signal` is native SIGTTIN or SIGTTOU). `restart` means the caller's group
// got the signal and the caller stopped or has a signal that interrupts a
// park; otherwise the result is io_error (init drops its stop).
enum class BackgroundAccess : u8 { allowed, io_error, restart };
BackgroundAccess private_terminal_background(Kernel &kernel, sched::ProcessControlBlock &caller,
                                             sched::ThreadControlBlock &thread, u32 signal);
// Parks the running `thread` in ThreadWaitReason::job_control under `key` and,
// for Linux, binds the park to its syscall origin; -11 with `complete` false.
i64 private_job_control_park(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread,
                             u64 key, i32 fd, bool &complete);
// Native rt_sigaction: SIG_DFL and SIG_IGN only (ok/uapi/job_control.h).
i64 native_signal_action(Kernel &kernel, sched::ProcessControlBlock &process, const std::array<uptr, 6> &arguments);
// Exec: SIG_IGN kept under the new numbering (MIPS), except SIGCHLD in a
// native image; owed kernel signals dropped on a personality change.
void private_jobctl_exec(Kernel &kernel, sched::ProcessControlBlock &process, Personality previous);
// The exit commit: init adoption, then the terminal outbox, then the
// orphaned-group pass sending SIGHUP and SIGCONT (Linux kill_orphaned_pgrp).
Status private_exit_commit(Kernel &kernel, sched::ProcessId pid, const PrivateChildExit::Completion &completion,
                           const PrivateChildExit::Adoption &adoption);
#endif
void queue_pipe_sigpipe(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadId tid);
// Completes one queued futex wait as a WAKE would (the peer fixture's
// control completion); its owner produces the result when it resumes.
bool wake_futex_object(Kernel &kernel, memory::ObjectHandle object);
enum class FutexInterrupt : u8
{
    interrupted, // Published and interrupted.
    completed,   // A waker, the timeout or a cancel completed it first.
    failed,      // The publication or the interrupt failed.
};
// The signal side of spec 4.5, at the owner's probed dispatch. Under the
// bucket that lists the wait, it leaves the queue, and while it is still
// pending `publish` writes the interrupted call's outcome into the owner's
// frame and the ticket is interrupted. A wait some other completion reached
// first is left to the resume, which produces that result; the signal stays
// pending. `failed` means a lost publication (the runtime stops).
//
// Every completion of an untimed wait other than this one holds its bucket
// (WAKE, WAKE_OP, REQUEUE) or runs while the owner is off its CPU (the
// quiescer's cancel), so the pending check, the publication and the
// interrupt cannot be split; only an untimed wait restarts, and its restart
// permit validates only while the wait is pending. The expiry of a timed
// wait holds `task` alone, so a timed wait is interrupted first and its
// EINTR published after: an expiry that lands in between finds it complete.
template <typename Publish>
FutexInterrupt interrupt_futex_wait(Kernel &kernel, FutexWait &wait, Publish &&publish)
{
    const auto index = futex_buckets.lock_linked(wait);
    if (index == FutexBucketTable::none) return FutexInterrupt::completed;
    futex_buckets.unlink(index, wait);
    auto outcome = FutexInterrupt::completed;
    if (wait.timeout.armed())
    {
        if (kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::interrupted))
            outcome = std::forward<Publish>(publish)() ? FutexInterrupt::interrupted : FutexInterrupt::failed;
    }
    else if (kernel.scheduler().wait_pending(wait.ticket))
        outcome = std::forward<Publish>(publish)() &&
                          kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::interrupted)
                      ? FutexInterrupt::interrupted
                      : FutexInterrupt::failed;
    futex_buckets[index].lock.unlock();
    return outcome;
}

void finish(Kernel &kernel, sched::ProcessId pid, i32 code, u8 signal);

Status set_linux_udp_result(const sched::ProcessControlBlock &process,
                            hw::UserFrame &frame, i64 result);

template <lu::Profile Target>
bool linux_udp_delay_slot_excluded(const hw::Trap &trap)
{
    if constexpr (Target == lu::Profile::mips || Target == lu::Profile::mips64)
        return trap.kind == hw::TrapKind::syscall && trap.vector == 8 &&
            ((trap.error_code >> 2) & 0x1f) == 8 && (trap.error_code & 0x80000000U);
    else
        return false;
}

bool linux_udp_frame(const hw::UserFrame &frame);

Status dispatch_linux_udp(Kernel &kernel, sched::ProcessControlBlock &process,
                          sched::ThreadControlBlock &thread);
class TcpImmediateRuntime final
{
    using Access = TcpImmediateAccess<lu::configured_native_profile()>;
    using Call = lu::TcpCall;
    using Constants = lu::UdpConstants<lu::configured_native_profile()>;
    using AcceptCall = lu::TcpAcceptCall<lu::configured_native_profile()>;
    static constexpr usize profile = static_cast<usize>(lu::configured_native_profile());
    struct Request { Call operation{}; bool linux{}; std::array<u64, 6> arguments{}; };
  public:
    Status dispatch(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread)
    {
        if (busy_) return Status::busy("TCP immediate runtime is not reentrant");
        struct Scope { bool &busy; explicit Scope(bool &value) : busy(value) { busy = true; } ~Scope() { busy = false; } } scope{busy_};
        auto admitted = Access::acquire(syscall_origins, process, thread);
        if (!admitted) return admitted.status();
        auto &access = admitted.value();
        // Only owned frame values and captured IDs survive copied callbacks.
        const auto saved_frame = thread.native_frame;
        const u64 pid = access.pid();
        // The effective call: a legacy socketcall selector (TcpSocketcall), the
        // copied accept route, or the direct call.
        const auto route = [&]() -> lu::TcpCallRoute {
            if (!access.linux_personality()) return {access.operation(), false, 0, 0};
            const auto snapshot = lu::NativeBridge<lu::configured_native_profile()>::snapshot(saved_frame);
            if (const auto socketcall = Socketcall::route(snapshot); socketcall.call != Call::none)
                return {socketcall.call, true, socketcall.array, socketcall.bytes()};
            return AcceptCall::route(snapshot);
        }();
        TcpOriginalDescription original{kernel.scheduler(), kernel.native_tcp(), runtime_failure.bkl_sink()};
        const auto operation = [&]() -> Status
        {
            if (!access.live()) return Status::denied("TCP dispatch lost captured image");
            if (route.operation != access.operation()) return Status::denied("TCP effective route differs from original capture");
            if (access.linux_personality())
            {
                using Codec = lu::Codec<lu::configured_native_profile()>;
                const auto registers = lu::NativeBridge<lu::configured_native_profile()>::snapshot(saved_frame);
                if (registers.integer[Codec::profile.number_register] != access.raw_syscall())
                    return Status::denied("TCP outer raw syscall differs from captured frame");
            }
            if (access.operation() == Call::accept || access.operation() == Call::accept4)
            {
                auto captured = TcpCapturedAccept<lu::configured_native_profile()>::capture(
                    kernel.scheduler(), saved_frame, access, original);
                if (!captured)
                {
                    auto encoded = mapped_error(access, captured.status());
                    return encoded ? publish(kernel, access, {0, encoded.value()}) : encoded.status();
                }
                if (captured.value().original != TcpDescriptionKind::retained_tcp)
                {
                    const bool bad_fd = captured.value().original == TcpDescriptionKind::bad_fd;
                    const auto number = access.linux_personality()
                        ? *Constants::errno_number(bad_fd ? lu::UdpErrno::e_badf : lu::UdpErrno::e_notsock)
                        : bad_fd ? 9u : 88u;
                    return publish(kernel, access, {0, number});
                }
                const Request request{captured.value().operation, captured.value().linux, captured.value().arguments};
                auto result = execute(kernel, pid, access, original, request);
                return result ? publish(kernel, access, result.value()) : result.status();
            }
            i32 original_fd{};
            if (access.operation() != Call::socket)
            {
                // Direct signatures own fd in a saved register, including
                // O32. Accept socketcall selection belongs to the shared path.
                const auto first = first_argument(saved_frame, access);
                if (!first)
                {
                    auto encoded = mapped_error(access, first.status());
                    return encoded ? publish(kernel, access, {0, encoded.value()}) : encoded.status();
                }
                original_fd = static_cast<i32>(static_cast<u32>(first.value()));
                if (!access.live()) return Status::denied("TCP original capture lost image after fd head copy");
                const auto *current = kernel.scheduler().find(pid);
                if (!current) return Status::denied("TCP captured process disappeared before original selection");
                auto selected = original.acquire(*current, original_fd);
                if (!selected)
                {
                    auto encoded = mapped_error(access, selected.status());
                    return encoded ? publish(kernel, access, {0, encoded.value()}) : encoded.status();
                }
                if (selected.value() != TcpDescriptionKind::retained_tcp)
                {
                    if (selected.value() == TcpDescriptionKind::not_socket && access.operation() == Call::getsockopt)
                        if (const u32 udp = udp_socket(kernel, *current, original_fd))
                        {
                            auto request = decode(saved_frame, access);
                            if (!request)
                            {
                                auto encoded = mapped_error(access, request.status());
                                return encoded ? publish(kernel, access, {0, encoded.value()}) : encoded.status();
                            }
                            auto result = udp_option(kernel, access, request.value(), udp);
                            return result ? publish(kernel, access, result.value()) : result.status();
                        }
                    const auto number = access.linux_personality()
                        ? *Constants::errno_number(selected.value() == TcpDescriptionKind::bad_fd ? lu::UdpErrno::e_badf : lu::UdpErrno::e_notsock)
                        : selected.value() == TcpDescriptionKind::bad_fd ? 9u : 88u;
                    return publish(kernel, access, {0, number});
                }
                if (auto status = access.bind_original(original, original_fd); !status) return status;
            }
            auto request = decode(saved_frame, access);
            if (!request)
            {
                auto encoded = mapped_error(access, request.status());
                return encoded ? publish(kernel, access, {0, encoded.value()}) : encoded.status();
            }
            auto result = execute(kernel, pid, access, original, request.value());
            const bool raises = request.value().operation == Call::write ||
                (request.value().operation == Call::sendto &&
                 !(static_cast<u32>(request.value().arguments[3]) & *Constants::message(lu::UdpMessage::nosignal)));
            if (result && raises && request.value().linux && result.value().error == lu::tcp_errno_epipe[profile])
            {
                if (!access.live()) return Status::denied("TCP SIGPIPE lost captured image/descriptor");
                auto *current = kernel.scheduler().find(pid);
                if (!current) return Status::denied("TCP SIGPIPE captured process disappeared");
                queue_pipe_sigpipe(kernel, *current, access.tid());
                if (!runtime_failure) return runtime_failure;
            }
            return result ? publish(kernel, access, result.value()) : result.status();
        }();
        // Publication happens above while the original OFD is still held.
        // Consume capture authority before the only fallible final release.
        access.reset();
        const auto accept_retired = prepared_accept_ ? prepared_accept_->retire() : Status{};
        prepared_accept_.reset();
        const auto released = original.release();
        if (!accept_retired) { started = false; return accept_retired; }
        if (!released) { started = false; return released; }
        return operation;
    }
  private:
    using Socketcall = TcpSocketcall<lu::configured_native_profile()>;
    // The descriptor argument: a register, or the first word of a legacy
    // socketcall's argument array.
    static Result<u64> first_argument(const hw::UserFrame &frame, Access &access)
    {
        if (!access.linux_personality()) return hw::syscall(frame).arguments[0];
        using Codec = lu::Codec<lu::configured_native_profile()>;
        const auto snapshot = lu::NativeBridge<lu::configured_native_profile()>::snapshot(frame);
        if (const auto route = Socketcall::route(snapshot); route.call != Call::none)
        {
            if (auto status = Socketcall::interval(route); !status) return status;
            std::array<std::byte, 4> head{};
            if (auto status = access.read(route.array, head); !status) return status;
            return Socketcall::word(head);
        }
        return snapshot.integer[Codec::profile.argument_registers[0]] & Codec::word_mask;
    }
    static usize argument_count(Call call)
    {
        if (call == Call::getsockopt || call == Call::setsockopt) return 5;
        if (call == Call::sendto || call == Call::recvfrom) return 6;
        if (call == Call::accept4) return 4;
        if (call == Call::listen || call == Call::shutdown) return 2;
        return 3;
    }
    static Result<Request> decode(const hw::UserFrame &frame, Access &access)
    {
        if (!access.live()) return Status::denied("TCP decode lost captured image");
        Request request{access.operation(), access.linux_personality(), {}};
        if (!request.linux)
        {
            const auto copied = hw::syscall(frame);
            if (copied.number != access.raw_syscall()) return Status::denied("native TCP syscall changed after capture");
            for (usize at = 0; at < request.arguments.size(); ++at) request.arguments[at] = copied.arguments[at];
            if (request.operation == Call::accept) request.arguments[3] = 0;
            return request;
        }
        using Codec = lu::Codec<lu::configured_native_profile()>;
        const auto snapshot = lu::NativeBridge<lu::configured_native_profile()>::snapshot(frame);
        const u64 sp = static_cast<u64>(frame.sp) & Codec::word_mask;
        const u64 number = snapshot.integer[Codec::profile.number_register];
        // A legacy socketcall: every argument comes from the copied array.
        if (const auto socketcall = Socketcall::route(snapshot); socketcall.call != Call::none)
        {
            if (number != access.raw_syscall() || socketcall.call != request.operation)
                return Status::denied("Linux TCP socketcall changed after capture");
            if (auto status = Socketcall::interval(socketcall); !status) return status;
            std::array<std::byte, 24> words{};
            const auto copied = std::span{words}.first(socketcall.bytes());
            if (auto status = access.read(socketcall.array, copied); !status) return status;
            auto arguments = Socketcall::arguments(socketcall, copied);
            if (!arguments || !access.live()) return arguments ? Status::denied("TCP copied argument authority expired") : arguments.status();
            request.arguments = arguments.value();
            return request;
        }
        const auto route = AcceptCall::route(snapshot);
        if (number != access.raw_syscall() || route.socketcall || route.operation != request.operation)
            return Status::denied("Linux TCP operation changed after capture");
        const usize count = argument_count(request.operation);
        std::array<u8, 24> stack{};
        if constexpr (lu::configured_native_profile() == lu::Profile::mips)
        {
            if (count > 4)
            {
                const usize size = (count - 4) * 4;
                if (sp > Codec::word_mask - 16 || size - 1 > Codec::word_mask - (sp + 16))
                    return Status::fault("TCP O32 stack argument interval wraps");
                if (auto status = access.read(sp + 16, {reinterpret_cast<std::byte *>(stack.data() + 16), size}); !status)
                    return status;
            }
        }
        auto decoded = Codec::decode_call(snapshot, count, stack);
        if (!decoded || !access.live()) return decoded ? Status::denied("TCP copied argument authority expired") : decoded.status();
        request.arguments = decoded.value().arguments;
        return request;
    }
    static Result<u32> mapped_error(const Access &access, Status status)
    {
        if (access.linux_personality())
        {
            auto value = net::LinuxTcpStreamAdapter<lu::configured_native_profile()>::error_number(status.code());
            if (value) return *value;
            return Status::unsupported("TCP status has no exact target errno mapping");
        }
        if (status.code() == StatusCode::unsupported) return u32{95};
        return static_cast<u32>(syscall::ErrnoMapper::errno_for(status));
    }
    static u32 error(const Request &request, lu::UdpErrno code, u32 native)
    { return request.linux ? *Constants::errno_number(code) : native; }
    static u32 tcp_error(const Request &request, const std::array<u32, static_cast<usize>(lu::Profile::count)> &values, u32 native)
    { return request.linux ? values[profile] : native; }
    static Result<lu::Return> translated(Access &access, Result<usize> result)
    {
        if (result) return lu::Return{result.value(), 0};
        auto number = mapped_error(access, result.status());
        return number ? Result<lu::Return>{lu::Return{0, number.value()}} : Result<lu::Return>{number.status()};
    }
    // The record a TCP name call reports: the profile's sockaddr_in or
    // sockaddr_in6, or the native ok_sockaddr_in or ok_sockaddr_in6.
    static Result<lu::SocketName> socket_name(const Request &request, const net::tcp::Endpoint &endpoint)
    {
        if (request.linux) return lu::encode_socket_name<lu::configured_native_profile()>(endpoint.address, endpoint.port);
        lu::SocketName name{};
        const std::array<u8, 2> port{static_cast<u8>(endpoint.port >> 8), static_cast<u8>(endpoint.port)};
        if (endpoint.address.v4())
        {
            ok_sockaddr_in record{};
            record.family = OK_AF_INET; record.port_be[0] = port[0]; record.port_be[1] = port[1];
            for (usize i = 0; i < 4; ++i) record.address[i] = endpoint.address[i];
            const auto bytes = std::bit_cast<std::array<u8, sizeof(record)>>(record);
            for (usize i = 0; i < bytes.size(); ++i) name.bytes[i] = bytes[i];
            name.size = bytes.size();
            return name;
        }
        ok_sockaddr_in6 record{};
        record.family = OK_AF_INET6; record.port_be[0] = port[0]; record.port_be[1] = port[1];
        for (usize i = 0; i < 16; ++i) record.address[i] = endpoint.address[i];
        record.scope_id = net::wire::link_local(endpoint.address.ipv6()) ? 1 : 0;
        const auto bytes = std::bit_cast<std::array<u8, sizeof(record)>>(record);
        for (usize i = 0; i < bytes.size(); ++i) name.bytes[i] = bytes[i];
        name.size = bytes.size();
        return name;
    }
    // The source address the interface selects for a peer.
    static Result<net::IpAddress> connect_source(Kernel &kernel, const net::IpAddress &peer)
    {
        if (peer.v4())
        {
            auto source = kernel.network_interface().source(peer.ipv4()); if (!source) return source.status();
            return net::IpAddress{source.value()};
        }
        auto source = kernel.network_interface().source(peer.ipv6()); if (!source) return source.status();
        return net::IpAddress{source.value()};
    }
    // Binds the next free port of the ephemeral range 49152-65535.
    static Result<net::tcp::Endpoint> autobind(net::NativeTcpStreamService &service, net::TcpStreamHandle handle,
                                               net::IpAddress address)
    {
        for (u32 attempt = 0; attempt < 16384; ++attempt)
        {
            const u16 port = next_ephemeral_;
            next_ephemeral_ = port == 65535 ? u16{49152} : static_cast<u16>(port + 1);
            const net::tcp::Endpoint local{address, port};
            const auto status = service.bind(handle, local);
            if (status) return local;
            if (status.code() != StatusCode::already_exists) return status;
        }
        return Status::no_space("no free ephemeral TCP port");
    }
    static inline constinit u16 next_ephemeral_ = 49152;
    // The errno a failed connection reports (Linux tcp_v4_err, tcp_reset,
    // tcp_write_err); `none` is 0.
    static u32 tcp_failure_errno(const Request &request, net::tcp::Failure failure)
    {
        using F = net::tcp::Failure;
        switch (failure)
        {
        case F::none: return 0;
        case F::refused: return tcp_error(request, lu::tcp_errno_econnrefused, 111);
        case F::host_unreachable: return tcp_error(request, lu::tcp_errno_ehostunreach, 113);
        case F::network_unreachable: return tcp_error(request, lu::tcp_errno_enetunreach, 101);
        case F::prohibited: return tcp_error(request, lu::tcp_errno_eacces, 13);
        case F::protocol: return tcp_error(request, lu::tcp_errno_eproto, 71);
        case F::timed_out: return tcp_error(request, lu::tcp_errno_etimedout, 110);
        case F::reset: return tcp_error(request, lu::tcp_errno_econnreset, 104);
        case F::aborted: break;
        }
        return tcp_error(request, lu::tcp_errno_econnaborted, 103);
    }
    // Linux order (__sys_getsockname, move_addr_to_user): the name is taken
    // first, then the length is read; a negative length is EINVAL, at most
    // the length is copied and the full size is reported.
    static Result<usize> copy_socket_name(Access &access, const Request &request, const lu::SocketName &name, u32 &semantic)
    {
        std::array<std::byte, 4> length_bytes{};
        if (auto status = access.read(request.arguments[2], length_bytes); !status) return status;
        std::array<u8, 4> raw{};
        for (usize i = 0; i < raw.size(); ++i) raw[i] = std::to_integer<u8>(length_bytes[i]);
        i32 length{};
        if (request.linux)
        {
            auto decoded = lu::UdpWire<lu::configured_native_profile()>::decode_socklen(raw); if (!decoded) return decoded.status();
            length = decoded.value();
        }
        else length = std::bit_cast<i32>(std::bit_cast<u32>(raw));
        if (length < 0) { semantic = error(request, lu::UdpErrno::e_inval, 22); return Status::fault(); }
        const usize copied = static_cast<usize>(length) < name.size ? static_cast<usize>(length) : name.size;
        std::array<std::byte, 28> record{};
        for (usize i = 0; i < copied; ++i) record[i] = std::byte{name.bytes[i]};
        if (copied) if (auto status = access.write(request.arguments[1], std::span{record}.first(copied)); !status) return status;
        if (request.linux)
        {
            if (auto status = lu::UdpWire<lu::configured_native_profile()>::encode_socklen(raw, static_cast<u32>(name.size)); !status)
                return status;
        }
        else raw = std::bit_cast<std::array<u8, 4>>(static_cast<u32>(name.size));
        for (usize i = 0; i < raw.size(); ++i) length_bytes[i] = std::byte{raw[i]};
        if (auto status = access.write(request.arguments[2], length_bytes); !status) return status;
        return usize{0};
    }
    // An integer socket option (sk_getsockopt): the length is read first and a
    // negative one is EINVAL, before the value is taken (SO_ERROR consumes
    // the pending error only once the length is known to be valid).
    static Result<i32> socket_option_length(Access &access, const Request &request, u32 &semantic)
    {
        std::array<std::byte, 4> length_bytes{};
        if (auto status = access.read(request.arguments[4], length_bytes); !status) return status;
        std::array<u8, 4> raw{};
        for (usize i = 0; i < raw.size(); ++i) raw[i] = std::to_integer<u8>(length_bytes[i]);
        i32 length{};
        if (request.linux)
        {
            auto decoded = lu::UdpWire<lu::configured_native_profile()>::decode_socklen(raw); if (!decoded) return decoded.status();
            length = decoded.value();
        }
        else length = std::bit_cast<i32>(std::bit_cast<u32>(raw));
        if (length < 0) { semantic = error(request, lu::UdpErrno::e_inval, 22); return Status::fault(); }
        return length;
    }
    // Then at most four bytes of the value are copied in the profile's byte
    // order and the copied size is written back.
    static Result<usize> copy_socket_option(Access &access, const Request &request, i32 length, u32 value)
    {
        const usize copied = static_cast<usize>(length) < 4 ? static_cast<usize>(length) : 4;
        std::array<u8, 4> encoded{}, raw{};
        if (request.linux)
        {
            if (auto status = lu::UdpWire<lu::configured_native_profile()>::encode_socklen(encoded, value); !status) return status;
        }
        else encoded = std::bit_cast<std::array<u8, 4>>(value);
        std::array<std::byte, 4> bytes{};
        for (usize i = 0; i < bytes.size(); ++i) bytes[i] = std::byte{encoded[i]};
        if (copied) if (auto status = access.write(request.arguments[3], std::span{bytes}.first(copied)); !status) return status;
        if (request.linux)
        {
            if (auto status = lu::UdpWire<lu::configured_native_profile()>::encode_socklen(raw, static_cast<u32>(copied)); !status)
                return status;
        }
        else raw = std::bit_cast<std::array<u8, 4>>(static_cast<u32>(copied));
        std::array<std::byte, 4> length_bytes{};
        for (usize i = 0; i < raw.size(); ++i) length_bytes[i] = std::byte{raw[i]};
        if (auto status = access.write(request.arguments[4], length_bytes); !status) return status;
        return usize{0};
    }
    // The UDP endpoint of a descriptor, or 0.
    static u32 udp_socket(Kernel &kernel, const sched::ProcessControlBlock &process, i32 fd)
    {
        const auto *slot = process.fd_table().entry(fd);
        if (!slot || !slot->used) return 0;
        const auto *description = kernel.scheduler().open_description(slot->description);
        return description && description->references ? description->native_udp_handle : 0;
    }
    // getsockopt on a UDP socket: SO_ERROR takes a pending ICMP refusal
    // (reported once, like a receive or send would), SO_TYPE is SOCK_DGRAM.
    static Result<lu::Return> udp_option(Kernel &kernel, Access &access, const Request &request, u32 udp)
    {
        u32 semantic{};
        auto result = [&]() -> Result<usize> {
            constexpr auto profile = static_cast<usize>(lu::configured_native_profile());
            const u32 level = static_cast<u32>(request.arguments[1]), option = static_cast<u32>(request.arguments[2]);
            const u32 sol_socket = request.linux ? lu::tcp_sol_socket[profile] : OK_SOL_SOCKET;
            const u32 so_error = request.linux ? lu::tcp_so_error[profile] : OK_SO_ERROR;
            const u32 so_type = request.linux ? lu::tcp_so_type[profile] : OK_SO_TYPE;
            if (level != sol_socket || (option != so_error && option != so_type))
                return Status::unsupported("UDP socket option is outside SO_ERROR and SO_TYPE");
            auto length = socket_option_length(access, request, semantic); if (!length) return length.status();
            u32 value = request.linux ? Constants::sock_dgram : OK_SOCK_DGRAM;
            if (option == so_error)
            {
                const auto taken = kernel.native_udp().take_error(udp);
                if (taken.code() == StatusCode::connection_refused) value = tcp_error(request, lu::tcp_errno_econnrefused, 111);
                else if (!taken) return taken;
                else value = 0;
            }
            return copy_socket_option(access, request, length.value(), value);
        }();
        if (!access.live()) return Status::denied("UDP option lost captured image before publication");
        return semantic ? Result<lu::Return>{lu::Return{0, semantic}} : translated(access, result);
    }
    // An IPv6 stream is v6-only: an AF_INET record is EAFNOSUPPORT and a
    // mapped address is `mapped` (EINVAL for bind, ENETUNREACH for connect).
    static Result<net::tcp::Endpoint> address(Access &access, const Request &request, net::Family family,
                                              u32 mapped, u32 &semantic)
    {
        const i32 length = static_cast<i32>(static_cast<u32>(request.arguments[2]));
        if (family == net::Family::ipv6)
        {
            if (length < 24 || length > 128 || (!request.linux && length != 28))
                return Status::invalid_argument("TCP sockaddr_in6 length is outside this profile");
            std::array<std::byte, 128> bytes{};
            if (auto status = access.read(request.arguments[1], std::span{bytes}.first(static_cast<usize>(length))); !status) return status;
            std::array<u8, 128> copied{};
            for (usize at = 0; at < static_cast<usize>(length); ++at) copied[at] = std::to_integer<u8>(bytes[at]);
            auto endpoint = lu::UdpWire<lu::configured_native_profile()>::decode_ipv6_address(std::span{copied}.first(static_cast<usize>(length)));
            if (!endpoint) return endpoint.status();
            if (endpoint.value().family != Constants::af_inet6) semantic = error(request, lu::UdpErrno::e_afnosupport, 97);
            else if (net::wire::ipv4_mapped(endpoint.value().address)) semantic = mapped;
            if (semantic) return Status::fault();
            return net::tcp::Endpoint{endpoint.value().address, endpoint.value().port};
        }
        if (length < 16 || length > 128 || (!request.linux && length != 16))
            return Status::invalid_argument("TCP sockaddr length is outside this profile");
        std::array<std::byte, 128> bytes{};
        if (auto status = access.read(request.arguments[1], std::span{bytes}.first(static_cast<usize>(length))); !status) return status;
        std::array<u8, 128> copied{};
        for (usize at = 0; at < static_cast<usize>(length); ++at) copied[at] = std::to_integer<u8>(bytes[at]);
        auto endpoint = lu::UdpWire<lu::configured_native_profile()>::decode_address(std::span{copied}.first(static_cast<usize>(length)));
        if (!endpoint) return endpoint.status();
        if (endpoint.value().family != 2) return Status::unsupported("TCP sockaddr family is not IPv4");
        return net::tcp::Endpoint{endpoint.value().address, endpoint.value().port};
    }
    Result<lu::Return> execute(Kernel &kernel, u64 pid, Access &access,
        TcpOriginalDescription &original, const Request &request)
    {
        const auto &a = request.arguments;
        if (request.operation == Call::socket)
        {
            const u32 type = static_cast<u32>(a[1]);
            const u32 stream = request.linux ? Constants::sock_stream : OK_SOCK_STREAM;
            const u32 nonblock = request.linux ? Constants::sock_nonblock : OK_SOCK_NONBLOCK;
            const u32 cloexec = request.linux ? Constants::sock_cloexec : OK_SOCK_CLOEXEC;
            const u32 mask = request.linux ? Constants::sock_type_mask : 0xf;
            const i32 domain = static_cast<i32>(static_cast<u32>(a[0]));
            if (domain != 2 && domain != 10)
                return lu::Return{0, error(request, lu::UdpErrno::e_afnosupport, 97)};
            if ((type & mask) != stream || (type & ~(mask | nonblock | cloexec)) ||
                (static_cast<i32>(static_cast<u32>(a[2])) != 0 && static_cast<i32>(static_cast<u32>(a[2])) != 6))
                return lu::Return{0, error(request, lu::UdpErrno::e_opnotsupp, 95)};
            if (auto status = kernel.activate_native_tcp(); !status) return translated(access, status);
            if (!access.live()) return Status::denied("TCP socket lost image during link activation");
            auto opened = kernel.native_tcp_operations().open(pid, type & nonblock, type & cloexec,
                domain == 10 ? net::Family::ipv6 : net::Family::ipv4);
            return translated(access, opened ? Result<usize>{static_cast<usize>(opened.value())} : Result<usize>{opened.status()});
        }
        const i32 fd = static_cast<i32>(static_cast<u32>(a[0]));
        if (!access.live() || !original.matches(pid, fd))
            return Status::denied("TCP decoded operation lost its original descriptor");
        const bool nonblocking = original.flags() & net::TcpStreamOperations::nonblock;
        u32 semantic_error{};
        const auto result = [&]() -> Result<usize> {
            const auto handle = original.handle();
            auto &service = kernel.native_tcp();
            if (request.operation == Call::accept || request.operation == Call::accept4)
            {
                if (prepared_accept_) return Status::busy("TCP runtime already owns a prepared accept");
                net::TcpAcceptCopied<lu::configured_native_profile()> copied{kernel.native_tcp_operations()};
                auto prepared = copied.prepare(pid, fd, a[1], a[2], static_cast<u32>(a[3]),
                    request.operation == Call::accept4, request.linux, access, original);
                if (!prepared) return prepared.status();
                if (!access.live()) return Status::denied("TCP accept lost captured authority before commit");
                prepared_accept_.emplace(std::move(prepared.value()));
                auto accepted = prepared_accept_->commit();
                return accepted ? Result<usize>{static_cast<usize>(accepted.value())} : Result<usize>{accepted.status()};
            }
            if (request.operation == Call::bind)
            {
                const auto stream = service.snapshot(handle);
                const auto family = stream ? stream.value().family : net::Family::ipv4;
                auto local = address(access, request, family, error(request, lu::UdpErrno::e_inval, 22), semantic_error);
                if (!local) return local.status();
                if (!kernel.native_udp().local_address(local.value().address))
                { semantic_error = error(request, lu::UdpErrno::e_addrnotavail, 99); return Status::fault(); }
                const auto status = service.bind(handle, local.value()); return status ? Result<usize>{0} : Result<usize>{status};
            }
            if (request.operation == Call::listen)
            {
                const i32 requested = static_cast<i32>(static_cast<u32>(a[1]));
                const usize backlog = requested <= 0 ? 1 : static_cast<usize>(requested);
                const auto status = service.listen(handle, backlog); return status ? Result<usize>{0} : Result<usize>{status};
            }
            if (request.operation == Call::connect)
            {
                if (!nonblocking) return Status::unsupported("blocking TCP connect requires its captured continuation");
                auto state = service.snapshot(handle); if (!state) return state.status();
                if (state.value().endpoint)
                {
                    if (state.value().endpoint->kind != net::tcp::HandleKind::connection)
                        return Status::invalid_argument("TCP listener cannot connect");
                    auto protocol = service.endpoints().state(*state.value().endpoint); if (!protocol) return protocol.status();
                    // inet_stream_connect: a failed attempt reports its error once.
                    if (protocol.value().transport.error_pending)
                    {
                        auto taken = service.take_error(handle); if (!taken) return taken.status();
                        semantic_error = tcp_failure_errno(request, taken.value()); return Status::fault();
                    }
                    semantic_error = protocol.value().transport.state == net::tcp::State::syn_sent
                        ? tcp_error(request, lu::tcp_errno_ealready, 114) : tcp_error(request, lu::tcp_errno_eisconn, 106);
                    return Status::fault();
                }
                auto peer = address(access, request, state.value().family,
                    error(request, lu::UdpErrno::e_netunreach, 101), semantic_error);
                if (!peer) return peer.status();
                if (!kernel.native_udp().reachable(peer.value().address))
                { semantic_error = error(request, lu::UdpErrno::e_netunreach, 101); return Status::fault(); }
                // An unbound stream binds first (inet_autobind, inet_hash_connect):
                // the source address the route selects and a free ephemeral port.
                net::tcp::Endpoint local = state.value().local;
                if (!local.port)
                {
                    auto source = connect_source(kernel, peer.value().address);
                    if (!source) { semantic_error = error(request, lu::UdpErrno::e_netunreach, 101); return Status::fault(); }
                    auto bound = autobind(service, handle, source.value());
                    if (!bound && bound.status().code() == StatusCode::no_space)
                    { semantic_error = error(request, lu::UdpErrno::e_addrnotavail, 99); return Status::fault(); }
                    if (!bound) return bound.status();
                    local = bound.value();
                }
                auto now = kernel.native_tcp_now(); if (!now) return now.status();
                if (!access.live()) return Status::denied("TCP connect lost image during clock acquisition");
                auto iss = kernel.native_tcp_iss(local, peer.value()); if (!iss) return iss.status();
                if (!access.live()) return Status::denied("TCP connect lost image during entropy acquisition");
                const auto status = service.connect(handle, peer.value(), iss.value(), now.value());
                if (status) semantic_error = tcp_error(request, lu::tcp_errno_einprogress, 115);
                return status ? Result<usize>{0} : Result<usize>{status};
            }
            if (request.operation == Call::getsockname || request.operation == Call::getpeername)
            {
                auto stream = service.snapshot(handle); if (!stream) return stream.status();
                // Unbound: the unspecified address and port 0 of the socket's family.
                net::tcp::Endpoint name = stream.value().local;
                if (!name.port && name.address.unspecified())
                    name.address = stream.value().family == net::Family::ipv6 ? net::IpAddress{net::wire::Ipv6{}} : net::IpAddress{};
                const bool connection = stream.value().endpoint && stream.value().endpoint->kind == net::tcp::HandleKind::connection;
                if (connection)
                {
                    auto protocol = service.endpoints().state(*stream.value().endpoint); if (!protocol) return protocol.status();
                    const auto state = protocol.value().transport.state;
                    if (request.operation == Call::getsockname) name = protocol.value().local;
                    // inet_getname: no peer before the handshake or after close.
                    else if (state == net::tcp::State::closed || state == net::tcp::State::syn_sent ||
                             state == net::tcp::State::listen)
                    { semantic_error = tcp_error(request, lu::tcp_errno_enotconn, 107); return Status::fault(); }
                    else name = protocol.value().peer;
                }
                else if (request.operation == Call::getpeername)
                { semantic_error = tcp_error(request, lu::tcp_errno_enotconn, 107); return Status::fault(); }
                auto record = socket_name(request, name); if (!record) return record.status();
                return copy_socket_name(access, request, record.value(), semantic_error);
            }
            if (request.operation == Call::getsockopt)
            {
                // SOL_SOCKET only: SO_ERROR takes the pending error, else a
                // remembered soft ICMP error; SO_TYPE is SOCK_STREAM. Other
                // levels and options stay EOPNOTSUPP.
                constexpr auto profile = static_cast<usize>(lu::configured_native_profile());
                const u32 level = static_cast<u32>(a[1]), option = static_cast<u32>(a[2]);
                const u32 sol_socket = request.linux ? lu::tcp_sol_socket[profile] : OK_SOL_SOCKET;
                const u32 so_error = request.linux ? lu::tcp_so_error[profile] : OK_SO_ERROR;
                const u32 so_type = request.linux ? lu::tcp_so_type[profile] : OK_SO_TYPE;
                if (level != sol_socket || (option != so_error && option != so_type))
                    return Status::unsupported("TCP socket option is outside SO_ERROR and SO_TYPE");
                auto length = socket_option_length(access, request, semantic_error); if (!length) return length.status();
                u32 value = request.linux ? Constants::sock_stream : OK_SOCK_STREAM;
                if (option == so_error)
                {
                    auto taken = service.take_error(handle, true); if (!taken) return taken.status();
                    value = tcp_failure_errno(request, taken.value());
                }
                return copy_socket_option(access, request, length.value(), value);
            }
            // Defer unsupported operations before touching connection-only state;
            // a listener is a valid stream but not a connection handle.
            const bool sendto = request.operation == Call::sendto, recvfrom = request.operation == Call::recvfrom;
            if (request.operation != Call::shutdown && request.operation != Call::read && request.operation != Call::write &&
                !sendto && !recvfrom)
                return Status::unsupported("TCP operation requires the next checked continuation slice");
            // sendto/recvfrom are write/read with flags (tcp_sendmsg, tcp_recvmsg).
            // MSG_DONTWAIT is already the immediate behaviour; MSG_NOSIGNAL
            // suppresses SIGPIPE and MSG_PEEK leaves the bytes queued.
            const u32 flags = sendto || recvfrom ? static_cast<u32>(a[3]) : 0;
            const u32 dontwait = request.linux ? *Constants::message(lu::UdpMessage::dontwait) : OK_MSG_DONTWAIT;
            const u32 peek = request.linux ? *Constants::message(lu::UdpMessage::peek) : OK_MSG_PEEK;
            const u32 nosignal = request.linux ? *Constants::message(lu::UdpMessage::nosignal) : OK_MSG_NOSIGNAL;
            if ((sendto && (flags & ~(dontwait | nosignal))) || (recvfrom && (flags & ~(dontwait | peek))))
                return Status::unsupported("TCP message flag is outside DONTWAIT, NOSIGNAL and PEEK");
            // __sys_sendto copies a destination before sending; TCP then ignores it.
            if (sendto && a[4])
            {
                const i32 length = static_cast<i32>(static_cast<u32>(a[5]));
                if (length < 0 || length > 128) { semantic_error = error(request, lu::UdpErrno::e_inval, 22); return Status::fault(); }
                std::array<std::byte, 128> ignored{};
                if (length) if (auto status = access.read(a[4], std::span{ignored}.first(static_cast<usize>(length))); !status) return status;
            }
            auto stream = service.snapshot(handle); if (!stream) return stream.status();
            if (!stream.value().endpoint || stream.value().endpoint->kind != net::tcp::HandleKind::connection)
            { semantic_error = tcp_error(request, lu::tcp_errno_enotconn, 107); return Status::fault(); }
            auto protocol = service.endpoints().state(*stream.value().endpoint); if (!protocol) return protocol.status();
            const auto &state = protocol.value().transport;
            // The failure is reported once (Linux sock_error); afterwards the
            // closed connection reads EOF and writes EPIPE.
            if (state.error_pending)
            {
                auto taken = service.take_error(handle); if (!taken) return taken.status();
                semantic_error = tcp_failure_errno(request, taken.value());
                return Status::fault();
            }
            if (request.operation == Call::shutdown)
            {
                if (static_cast<i32>(static_cast<u32>(a[1])) != OK_SHUT_WR) return Status::unsupported("only TCP write shutdown is supported");
                const auto status = service.shutdown_write(handle); return status ? Result<usize>{0} : Result<usize>{status};
            }
            const u64 ssize_max = request.linux ? (lu::Codec<lu::configured_native_profile()>::word_mask >> 1) : (~uptr{0} >> 1);
            if (a[2] > ssize_max) return Status::invalid_argument("TCP I/O count exceeds target ssize range");
            if (!a[2]) return usize{0};
            const usize size = a[2] < payload_.size() ? static_cast<usize>(a[2]) : payload_.size();
            if (state.state == net::tcp::State::syn_sent || state.state == net::tcp::State::syn_received)
                return Status::would_block("TCP handshake is not complete");
            if (request.operation == Call::read || recvfrom)
            {
                auto received = [&]() -> Result<usize> {
                    // A failed connection whose error was reported reads EOF (tcp_recvmsg, SOCK_DONE).
                    if (state.failure != net::tcp::Failure::none) return usize{0};
                    auto preview = service.peek(handle, std::span{payload_}.first(size)); if (!preview) return preview.status();
                    if (preview.value())
                        if (auto status = access.write(a[1], {payload_.data(), preview.value()}); !status) return status;
                    if (flags & peek) return preview;
                    auto consumed = service.read(handle, std::span{payload_}.first(preview.value()));
                    if (!consumed || consumed.value() != preview.value()) return Status::fault("TCP bytes changed after checked output");
                    return consumed;
                }();
                if (!received || !recvfrom || !a[4]) return received;
                // TCP names no source (msg_namelen 0): __sys_recvfrom writes a
                // zero length after the data, so a bad length fails after it.
                // socket_option_length reads the length word named by argument 4.
                auto length = socket_option_length(access, Request{request.operation, request.linux,
                    {a[0], a[1], a[2], a[3], a[5], a[5]}}, semantic_error);
                if (!length) return length.status();
                std::array<u8, 4> raw{};
                if (request.linux)
                {
                    if (auto status = lu::UdpWire<lu::configured_native_profile()>::encode_socklen(raw, 0); !status) return status;
                }
                std::array<std::byte, 4> zero{};
                for (usize i = 0; i < raw.size(); ++i) zero[i] = std::byte{raw[i]};
                if (auto status = access.write(a[5], zero); !status) return status;
                return received;
            }
            if (state.write_shutdown || (state.state != net::tcp::State::established && state.state != net::tcp::State::close_wait))
            { semantic_error = tcp_error(request, lu::tcp_errno_epipe, 32); return Status::fault(); }
            if (auto status = access.read(a[1], std::span{payload_}.first(size)); !status) return status;
            return service.write(handle, {payload_.data(), size});
        }();
        if (!access.live()) return Status::denied("TCP operation lost captured image/original descriptor before publication");
        return semantic_error ? Result<lu::Return>{lu::Return{0, semantic_error}} : translated(access, result);
    }
    static Status publish(Kernel &kernel, Access &access, lu::Return value)
    {
        if (!access.live()) return Status::denied("TCP result lost captured image authority");
        auto *process = kernel.scheduler().find(access.pid());
        auto *thread = kernel.scheduler().find_thread(access.pid(), access.tid());
        if (!process || !thread || kernel.scheduler().current_thread() != thread)
            return Status::denied("TCP result lost its original captured process/thread/frame");
        if (value.error > 4095) return Status::invalid_argument("TCP result error exceeds target range");
        thread->syscall_pending = false;
        const i64 result = value.error ? -static_cast<i64>(value.error) : static_cast<i64>(value.value);
        return access.linux_personality() ? set_linux_udp_result(*process, thread->native_frame, result)
                                          : set_result(*process, thread->native_frame, result);
    }
    std::optional<net::PreparedTcpAccept> prepared_accept_{};
    std::array<std::byte, 1024> payload_{};
    bool busy_{};
};
extern TcpImmediateRuntime tcp_immediate_runtime;

bool tcp_immediate_frame(Kernel &kernel, const sched::ProcessControlBlock &process, const sched::ThreadControlBlock &thread);
// Private captured receive ownership, integrated by the serialized runtime.
[[maybe_unused]] Status linux_udp_receive_invariant(Status status);
[[maybe_unused]] memory::ObjectHandle linux_udp_receive_object(sched::ProcessId pid, sched::ThreadId tid);
LinuxUdpReceiveStore::Member *linux_udp_receive_member(memory::ObjectHandle object);
SyscallOrigins::LinuxUdpReceiveBinding linux_udp_receive_binding(memory::ObjectHandle object,
                                                               const LinuxUdpReceiveWait &wait);

[[maybe_unused]] Status retire_linux_udp_receive(Kernel &kernel, memory::ObjectHandle object,
                                               LinuxUdpReceiveRetirement cause);
[[maybe_unused]] Status retire_linux_udp_receives(Kernel &kernel, sched::ProcessId pid,
    std::optional<sched::ThreadId> tid, LinuxUdpReceiveRetirement cause);
[[maybe_unused]] Status retire_linux_udp_receives_for_exec(Kernel &kernel, sched::ProcessId pid);
[[maybe_unused]] Status prepare_linux_udp_receives_for_exec(sched::ProcessId pid);

Status linux_udp_receive_start_status();

// Orphan erasure must follow checked receive retirement while the canonical PCB
// still exists. Missing owners indicate a violated lifecycle boundary; preserve
// their evidence and contain instead of treating disappearance as completion.
Status prune_departed_linux_udp_receives(Kernel &kernel);
[[maybe_unused]] Status resume_linux_udp_receive(Kernel &kernel, sched::ProcessControlBlock &process,
    sched::ThreadControlBlock &thread, memory::ObjectHandle object);
[[maybe_unused]] Status scan_linux_udp_receives(Kernel &kernel);
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64) || defined(__mips__) || defined(__powerpc__) || defined(__PPC__))

bool private_linux_udp_signal_ready(Kernel &kernel, const sched::ProcessControlBlock &process,
                                    const sched::ThreadControlBlock &thread);

bool private_linux_udp_signal_complete(Kernel &kernel, sched::ProcessControlBlock &process,
                                       sched::ThreadControlBlock &thread);
#endif

i64 native_udp_call(Kernel &kernel, sched::ProcessControlBlock *process, sched::ThreadControlBlock *caller,
                    hw::AddressSpace &space, syscall::Number number, const std::array<uptr, 6> &a,
                    bool &complete);
i64 fork_process(Kernel &kernel, sched::ProcessId pid, sched::ProcessControlBlock *process,
                 sched::ThreadId calling_tid, bool &complete);
i64 exec_image(Kernel &kernel, sched::ProcessId pid, sched::ProcessControlBlock *process,
               sched::ThreadId calling_tid, hw::AddressSpace &space, const std::array<uptr, 6> &a,
               bool &complete);
// Admit /init under its initial identity and install it as the first user process.
Result<sched::ProcessId> install_init(Kernel &kernel, const fs::BootArchive &archive);
} // namespace runtime_detail
} // namespace ok::user::native
