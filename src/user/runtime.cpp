#include "ok/user/native_signal_config.hpp"
#include "ok/uapi/signal.h"
#include "native_lifecycle.hpp"
#include "ok/user/native_signal_action.hpp"
#include "ok/user/runtime.hpp"
#include "ok/user/native_sessions_profile.hpp"
#include "ok/user/native_process_group_signal.hpp"
#include "ok/user/native_group_signal_profile.hpp"
#include "ok/user/native_console_control_profile.hpp"
#include "ok/core/native_shutdown.hpp"
#include "ok/user/native_sleep.hpp"
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64) || defined(__mips__) || defined(__powerpc__) || defined(__PPC__))
#define OK_PRIVATE_POLL_MASK 1
#include "native_poll_mask.hpp"
#include "signals/process_pending.hpp"
#include "signals/wait_state.hpp"
#include "signals/child_exit_lifecycle.hpp"
#include "ok/user/native_signal_target.hpp"
#include "ok/user/native_sleep_interruption.hpp"
#if defined(__i386__)
#include "ok/user/native_signal_delivery_i386.hpp"
#include "ok/user/native_signal_return_i386.hpp"
#elif defined(__arm__)
#include "ok/user/native_signal_delivery_arm32.hpp"
#include "ok/user/native_signal_return_arm32.hpp"
#elif defined(__aarch64__)
#include "ok/user/native_signal_delivery_aarch64.hpp"
#elif defined(__powerpc__) || defined(__PPC__)
#include "ok/user/native_signal_delivery_ppc32.hpp"
#include "ok/user/native_signal_return_ppc32.hpp"
#include "ok/user/native_signal_persistent_ppc32.hpp"
#define OK_PRIVATE_SIGNAL_HAS_TRAMPOLINE 1
#define OK_PRIVATE_PPC32_SIGNAL 1
#elif defined(__mips64)
#include "ok/user/native_signal_delivery_mips64_n64.hpp"
#include "ok/user/native_signal_return_mips64_n64.hpp"
#include "ok/user/native_signal_gate_mips64_n64.hpp"
#include "ok/user/native_mips64_n64_elf_admission.hpp"
#include "ok/user/native_signal_trampoline.hpp"
#define OK_PRIVATE_SIGNAL_HAS_TRAMPOLINE 1
#define OK_PRIVATE_MIPS64_N64_SIGNAL 1
#elif defined(__mips__) && !defined(__mips64)
#include "ok/user/native_signal_delivery_mips_o32.hpp"
#include "ok/user/native_signal_return_mips_o32.hpp"
#include "ok/user/native_signal_gate_mips_o32.hpp"
#include "ok/user/native_mips_o32_elf_admission.hpp"
#include "ok/user/native_signal_trampoline.hpp"
#define OK_PRIVATE_SIGNAL_HAS_TRAMPOLINE 1
#define OK_PRIVATE_MIPS_O32_SIGNAL 1
#elif defined(__loongarch64)
#include "ok/user/native_signal_delivery_loongarch64.hpp"
#include "ok/user/native_signal_return_loongarch64.hpp"
#include "ok/user/native_signal_trampoline.hpp"
#define OK_PRIVATE_SIGNAL_HAS_TRAMPOLINE 1
#elif defined(__riscv)
#include "ok/user/native_signal_delivery_riscv.hpp"
#include "ok/user/native_signal_trampoline.hpp"
#define OK_PRIVATE_SIGNAL_HAS_TRAMPOLINE 1
#else
#include "ok/user/native_signal_delivery_x86_64.hpp"
#endif
#endif
#include "ok/user/native_poll.hpp"
#include "ok/user/native_futex.hpp"
#include "ok/user/native_futex_requeue_sdk_profile.hpp"
#if defined(OK_ENABLE_TEST_POINTS) && defined(OK_PRIVATE_FUTEX_PEER_FIXTURE)
#include "../../tests/native/private_futex_peer_shared.hpp"
#endif
#include "ok/user/native_select.hpp"
#include "ok/user/native_pty_pair_dispatch.hpp"
#include "ok/user/native_page_pool.hpp"
#include "ok/user/native_shared_files.hpp"
#include "ok/abi/linux_futex_wake_operation.hpp"
#include "ok/user/native_shared_files_sdk_profile.hpp"
#include "ok/user/native_entry_stacks.hpp"
#ifdef OK_KERNEL_FREESTANDING
#include "ok/arch/service_stack.hpp"
#endif
#include "native_syscall_origin.hpp"
#include <ok/net/tcp_accept_copied.hpp>
#include "native_shared_capture.hpp"
#if defined(OK_PRIVATE_MIPS64_N64_SIGNAL) || defined(OK_PRIVATE_PPC32_SIGNAL)
#include "native_syscall_restart_publication.hpp"
#endif
#include "ok/memory/object_pool.hpp"
#include "ok/boot/qemu_process_memory.hpp"
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
#include "ok/time/public_clock.hpp"
#include "ok/user/clock_set.hpp"
#include "ok/core/kernel.hpp"
#include "ok/net/native_udp_poll.hpp"
#include "linux_udp_immediate_access.hpp"
#include "linux_udp_captured_decode.hpp"
#include "linux_udp_operation_body.hpp"
#include "linux_udp_receive_owner_store.hpp"
#include "tcp_immediate_access.hpp"
#include "tcp_captured_accept.hpp"
#include "ok/sched/process.hpp"
#include "ok/syscall/linux.hpp"
#include "ok/uapi/syscall.h"

extern "C" ok::Status ok_boot_filesystem(ok::fs::VirtualFileSystem &) __attribute__((weak));
extern "C" ok::Status ok_boot_entropy(ok::posix::PosixService &) __attribute__((weak));

namespace ok::user::native
{
namespace
{
namespace hw = arch::execution;
namespace lu = abi::linux_user;
using LinuxExec = lu::ExecBootstrap<lu::configured_native_profile()>;
using Personality = sched::UserPersonality;
static_assert(native_sessions_module_abi == kernel_module_abi_version);
static_assert(native_sessions_version == OK_NATIVE_SESSIONS_VERSION);
static_assert(native_sessions_process_capacity == sched::max_processes);
static_assert(native_console_control_module_abi == kernel_module_abi_version);
static_assert(native_console_control_version == OK_NATIVE_CONSOLE_CONTROL_VERSION);
static_assert(native_console_control_pid_bytes == 4 && native_sessions_controlling_tty == 1 &&
              native_group_signal_controlling_tty == 1 && native_console_control_pty == 0 &&
              native_console_control_termios == 0 && native_console_control_job_control == 0 &&
              native_console_control_smp == 0);
static_assert(native_group_signal_module_abi == kernel_module_abi_version);
static_assert(native_group_signal_process_capacity == sched::max_processes);
static_assert(native_group_signal_profile == "bootcpu-signed32-current-negative-pgid-probe-term-kill-deferred-partial-v2");
static_assert(native_group_signal_current_group == 1 && native_group_signal_negative_group == 1 &&
              native_group_signal_probe == 1 && native_group_signal_term == OK_SIGTERM &&
              native_group_signal_kill == OK_SIGKILL && native_group_signal_deferred_fatal == 1 &&
              native_group_signal_partial_success == 1 && native_group_signal_broadcast == 0 &&
              native_group_signal_stop_continue == 0 && native_group_signal_job_control == 0);
void copy_bytes(void *dst, const void *src, usize size)
{
    auto *out = static_cast<std::byte *>(dst);
    auto *in = static_cast<const std::byte *>(src);
    for (usize i = 0; i < size; ++i)
        out[i] = in[i];
}
using Pages = MappingPages;
constexpr usize page_count = NativePagePool::capacity;
constexpr auto runtime_arch = arch::configured_architecture();
#ifdef OK_KERNEL_FREESTANDING
constexpr bool linker_pool_required = !boot::qemu_firmware_process_pool<runtime_arch>;
#else
constexpr bool linker_pool_required = true; // Hosted backing, never firmware evidence.
#endif
alignas(4096) std::array<std::array<std::byte, hw::page_size>, linker_pool_required ? page_count : 0> physical_pages;
memory::FrameAllocator native_frames;
NativePagePool page_pool;
SharedMemoryFiles shared_files;
#ifdef OK_PRIVATE_SIGNAL_HAS_TRAMPOLINE
using PrivateSignalCode = SignalTrampoline<lu::configured_native_profile()>;
constexpr uptr private_signal_code_address = hw::user_end - hw::page_size;
static_assert(PrivateSignalCode::code_bytes <= hw::page_size);
#endif
// Part of the linker-reserved kernel image, separate from user/object frames.
alignas(4096) std::array<std::byte, memory::KernelEntryStackPool::arena_bytes> entry_stack_arena;
memory::KernelEntryStackPool entry_stacks;
using SyscallOrigins = SyscallOriginTable<lu::configured_native_profile()>;
SyscallOrigins syscall_origins{entry_stacks};
VfsStatContinuation<lu::configured_native_profile()> vfs_continuations{entry_stacks};
SharedNativeCapture<lu::configured_native_profile()> shared_capture{entry_stacks};
u64 entry_syscalls{}, entry_timers{}, entry_faults{}, entry_rejections{};
#ifdef OK_KERNEL_FREESTANDING
boot::FirmwareMemory firmware_workspace, firmware_memory;
boot::QualifiedBootMemory qualified_workspace, qualified_memory;
Status memory_preparation = Status::not_initialized("native memory not prepared at entry");
bool memory_preparation_attempted{};
extern "C" char _kernel_start[], _kernel_end[];
#endif
struct QuarantinedSpace
{
    enum class Ownership : u8 { borrowed, pending, discharged };
    hw::AddressSpace space{};
    Pages pages;
    std::array<Ownership, Pages::capacity> ownership{};
    std::array<bool, Pages::capacity> pin_held{};
    usize pinned_count{};
    bool used{};
    bool reserved{};
    bool table_retired{};
    bool pin_complete{};
};
// The final slot retains a candidate even if a backend incorrectly reports a
// failed retirement after consuming all of its advertised space slots.
std::array<QuarantinedSpace, hw::max_address_spaces + 1> quarantined_spaces{};
#ifdef OK_PRIVATE_PPC32_SIGNAL
// Pinned PPC musl debug files retain section tables after their debug data.
// This bounded private prefix is metadata storage, not executable image memory.
std::array<std::byte, 512 * 1024> elf_buffer{};
#else
std::array<std::byte, 128 * 1024> elf_buffer{};
#endif
sched::ProcessId last_pid{};
sched::ThreadId last_tid{};
bool started{};
Kernel *runtime_kernel{};
Status runtime_failure{};
ShutdownOwner original_shutdown_owner{};
bool runtime_step_active{}, shutdown_admission_closed{}, shutdown_retirement_started{}, shutdown_retirement_completed{};
Status shutdown_failure{};
struct RuntimeStepGuard
{
    RuntimeStepGuard() { runtime_step_active = true; }
    ~RuntimeStepGuard() { runtime_step_active = false; }
};
#ifdef OK_PRIVATE_MIPS_O32_SIGNAL
#include "signals/private_signal_mips_o32_state.hpp"
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
#include "signals/private_signal_mips64_n64_state.hpp"
#endif
#ifdef OK_PRIVATE_PPC32_SIGNAL
#include "signals/private_signal_ppc32_state.hpp"
#endif
struct QuarantinedEntryStack
{
    memory::KernelEntryStackOwner owner{};
    memory::KernelEntryStackHandle handle{};
};
std::array<QuarantinedEntryStack, memory::KernelEntryStackPool::capacity> stranded_entry_stacks{};
#ifdef OK_PRIVATE_POLL_MASK
PollSignalMasks poll_signal_masks;
SignalWaitState<lu::configured_native_profile()> signal_waits;
#include "signals/private_signal_wait_trace.hpp"
void private_signal_thread_retire(sched::ProcessId pid, sched::ThreadId tid, memory::KernelEntryStackHandle lease);
#endif
Status cancel_native_vfs_thread(const sched::ThreadControlBlock &thread)
{
    const auto pending = vfs_continuations.pending(thread.owner, thread.tid);
    if (!pending.request) return {};
    if (!runtime_kernel)
        return Status::not_initialized("VFS retirement requires its scheduler service owner");
    if (runtime_kernel->scheduler().find_thread(thread.owner, thread.tid) != &thread)
        return Status::denied("VFS retirement requires its canonical native thread");
    return vfs_continuations.cancel(runtime_kernel->scheduler(), pending);
}
memory::ObjectHandle linux_udp_receive_object(sched::ProcessId pid, sched::ThreadId tid);
Status retire_native_thread(sched::ThreadControlBlock &thread
#ifdef OK_PRIVATE_POLL_MASK
                            , std::string_view signal_wait_cause = "thread"
#endif
)
{
    if (linux_udp_receive_object(thread.owner, thread.tid).pool)
        return Status::busy("native thread retirement must first retire its Linux UDP receive owner");
    if (auto status = cancel_native_vfs_thread(thread); !status) return status;
#ifdef OK_PRIVATE_POLL_MASK
    const auto signal_lease = thread.entry_stack;
#endif
    if (auto status = syscall_origins.retire_checked(thread); !status) return status;
    if (auto status = vfs_continuations.retire(thread); !status) return status;
#ifdef OK_PRIVATE_POLL_MASK
    poll_signal_masks.restore(thread);
    if (const auto *wait = signal_waits.find(thread)) private_signal_wait_trace_retire(*wait, signal_wait_cause);
    signal_waits.restore(thread);
#endif
#ifdef OK_PRIVATE_MIPS_O32_SIGNAL
    private_mips_revoke(thread);
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
    private_n64_revoke(thread);
#endif
#ifdef OK_PRIVATE_PPC32_SIGNAL
    private_ppc_revoke(thread);
#endif
    const auto retired = retire_entry_stack(entry_stacks, thread);
#ifdef OK_PRIVATE_POLL_MASK
    if (retired) private_signal_thread_retire(thread.owner, thread.tid, signal_lease);
#endif
    return retired;
}
Status prepare_native_thread(sched::ThreadControlBlock &thread)
{
    if (linux_udp_receive_object(thread.owner, thread.tid).pool)
        return Status::busy("native thread preparation must not replace a Linux UDP receive owner");
    auto status = prepare_entry_stack(entry_stacks, thread);
    if (status)
    {
        status = syscall_origins.prepare(thread);
        if (status)
        {
            status = vfs_continuations.prepare(thread);
            if (!status) syscall_origins.retire(thread);
        }
        if (!status)
            if (auto retired = retire_entry_stack(entry_stacks, thread); !retired) status = retired;
    }
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
    if (thread.entry_stack.pool) private_n64_revoke(thread);
#endif
#ifdef OK_PRIVATE_PPC32_SIGNAL
    if (thread.entry_stack.pool) private_ppc_revoke(thread);
#endif
    if (!status && thread.entry_stack.pool)
    {
        // An unexpected post-reservation corruption must survive destruction of
        // an unpublished TCB. Keep the canonical charge and its recovery handle.
        stranded_entry_stacks[thread.entry_stack.slot] = {{thread.owner, thread.tid}, thread.entry_stack};
        thread.entry_stack = {};
        thread.kernel_stack_top = 0;
        runtime_failure = status;
        started = false;
    }
#ifdef OK_PRIVATE_MIPS_O32_SIGNAL
    if (status && !private_mips_reset(thread))
    {
        const auto retired = retire_native_thread(thread);
        if (!retired && thread.entry_stack.pool)
        {
            stranded_entry_stacks[thread.entry_stack.slot] = {{thread.owner, thread.tid}, thread.entry_stack};
            thread.entry_stack = {};
            thread.kernel_stack_top = 0;
            runtime_failure = retired;
            started = false;
        }
        return Status::fault("private MIPS signal lease preparation failed");
    }
#endif
    return status;
}
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
};
using PollObjectsLayout = memory::ObjectPool<PollWait, NativePagePool, 1>;
constexpr usize poll_object_pages = (sched::max_processes + PollObjectsLayout::slots_per_page - 1) /
                                   PollObjectsLayout::slots_per_page;
using PollObjects = memory::ObjectPool<PollWait, NativePagePool, poll_object_pages>;
constinit PollObjects poll_objects{page_pool, sched::max_processes};
struct PollWaitIndex
{
    sched::ProcessId pid{};
    sched::ThreadId tid{};
    memory::ObjectHandle object{};
};
// This bounded index contains identities only; the records live in owned pages.
std::array<PollWaitIndex, sched::max_processes> poll_waits{};
Status activate_pool();
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
    u64 queue_sequence{};
    sched::WaitTicket ticket{};
    sched::DeadlineWait timeout{};
    u32 expected{};
    u64 syscall{}, origin{};
    uptr timeout_address{};
};
bool same_futex_key(const FutexWait::Key &left, const FutexWait::Key &right)
{
    if (left.domain != right.domain || left.shared != right.shared) return false;
    if (!left.shared) return left.pid == right.pid && left.address == right.address;
    return left.frame.owner == right.frame.owner && left.frame.index == right.frame.index &&
           left.frame.generation == right.frame.generation && left.word_offset == right.word_offset;
}
bool exact_futex_key(const FutexWait::Key &left, const FutexWait::Key &right)
{
    return left.domain == right.domain && left.pid == right.pid && left.address == right.address &&
           left.frame.owner == right.frame.owner && left.frame.index == right.frame.index &&
           left.frame.generation == right.frame.generation && left.frame.address == right.frame.address &&
           left.word_offset == right.word_offset && left.shared == right.shared;
}
SyscallOrigins::FutexBinding futex_origin_binding(memory::ObjectHandle object, const FutexWait &wait)
{
    return {object, wait.ticket, wait.address,
        wait.domain == FutexKeyDomain::private_address_space ? 128U : 0U, wait.expected, wait.syscall,
        wait.timeout_address, wait.timeout.deadline(), wait.timeout.generation()};
}
using FutexObjectsLayout = memory::ObjectPool<FutexWait, NativePagePool, 1>;
constexpr usize futex_object_pages =
    (sched::max_processes + FutexObjectsLayout::slots_per_page - 1) / FutexObjectsLayout::slots_per_page;
using FutexObjects = memory::ObjectPool<FutexWait, NativePagePool, futex_object_pages>;
constinit FutexObjects futex_objects{page_pool, sched::max_processes};
std::array<PollWaitIndex, sched::max_processes> futex_waits{};
u64 futex_queue_sequence{};
Result<u64> reserve_futex_sequences(usize count)
{
    if (count > ~u64{0} - futex_queue_sequence)
        return Status::overflow("futex queue sequence exhausted");
    if (!count) return u64{0};
    const auto first = futex_queue_sequence + 1;
    futex_queue_sequence += count;
    return first;
}

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
SyscallOrigins::PipeBinding pipe_origin_binding(memory::ObjectHandle object, const PipeReadWait &wait)
{
    return {object, wait.ticket, wait.request, wait.fd, wait.output, wait.count, wait.description, wait.pipe, wait.vectors};
}
constexpr usize pipe_read_capacity = memory::KernelEntryStackPool::capacity;
using PipeReadObjectsLayout = memory::ObjectPool<PipeReadWait, NativePagePool, 1>;
constexpr usize pipe_read_object_pages =
    (pipe_read_capacity + PipeReadObjectsLayout::slots_per_page - 1) / PipeReadObjectsLayout::slots_per_page;
using PipeReadObjects = memory::ObjectPool<PipeReadWait, NativePagePool, pipe_read_object_pages>;
constinit PipeReadObjects pipe_read_objects{page_pool, pipe_read_capacity};
std::array<PollWaitIndex, pipe_read_capacity> pipe_read_waits{};
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
SyscallOrigins::PipeWriteBinding pipe_write_origin_binding(memory::ObjectHandle object, const PipeWriteWait &wait)
{
    return {object, wait.ticket, wait.request, wait.fd, wait.input, wait.count, wait.description, wait.pipe, wait.vectors, wait.committed};
}
constexpr usize pipe_write_capacity = memory::KernelEntryStackPool::capacity;
using PipeWriteObjectsLayout = memory::ObjectPool<PipeWriteWait, NativePagePool, 1>;
constexpr usize pipe_write_object_pages =
    (pipe_write_capacity + PipeWriteObjectsLayout::slots_per_page - 1) / PipeWriteObjectsLayout::slots_per_page;
using PipeWriteObjects = memory::ObjectPool<PipeWriteWait, NativePagePool, pipe_write_object_pages>;
constinit PipeWriteObjects pipe_write_objects{page_pool, pipe_write_capacity};
std::array<PollWaitIndex, pipe_write_capacity> pipe_write_waits{};
#include "native_pty_owners.hpp"
struct ChildWait
{
    sched::ProcessId pid{};
    sched::ThreadId tid{};
    sched::WaitTicket ticket{};
    u64 request{};
    ChildWaitRequest operation{};
};
SyscallOrigins::ChildBinding child_origin_binding(memory::ObjectHandle object, const ChildWait &wait)
{ return {object, wait.ticket, wait.request, wait.operation}; }
constexpr usize child_wait_capacity = memory::KernelEntryStackPool::capacity;
using ChildWaitObjectsLayout = memory::ObjectPool<ChildWait, NativePagePool, 1>;
constexpr usize child_wait_object_pages =
    (child_wait_capacity + ChildWaitObjectsLayout::slots_per_page - 1) / ChildWaitObjectsLayout::slots_per_page;
using ChildWaitObjects = memory::ObjectPool<ChildWait, NativePagePool, child_wait_object_pages>;
constinit ChildWaitObjects child_wait_objects{page_pool, child_wait_capacity};
std::array<PollWaitIndex, child_wait_capacity> child_waits{};
#include "native_udp_receive_owners.hpp"
#include "linux_udp_receive_owners.hpp"
Status retire_linux_udp_receives(Kernel &kernel, sched::ProcessId pid,
    std::optional<sched::ThreadId> tid, LinuxUdpReceiveRetirement cause);
constexpr usize kernel_object_pages = poll_object_pages + futex_object_pages + pipe_read_object_pages + pipe_write_object_pages + pty_wait_object_pages + child_wait_object_pages + native_udp_receive_pages + linux_udp_receive_pages;
// Aliases consume metadata records without another physical data frame. Bound
// every published directory and one candidate independently of the data quota.
static_assert(kernel_object_pages < NativePagePool::capacity);
constexpr usize mapping_metadata_pages =
    (hw::max_address_spaces + 1) * (Pages::max_blocks + Pages::max_frame_blocks);
constexpr usize kernel_reserve_pages = kernel_object_pages + mapping_metadata_pages;
static_assert(kernel_reserve_pages < NativePagePool::capacity);
#if __SIZEOF_POINTER__ == 8
static_assert(Pages::records_per_block == 170 && Pages::frames_per_block == 128 && mapping_metadata_pages == 255);
#else
static_assert(Pages::records_per_block == 341 && Pages::max_frame_blocks >= 6 && Pages::max_frame_blocks <= 7);
#endif

[[maybe_unused]] memory::ObjectHandle futex_object(sched::ProcessId pid, sched::ThreadId tid)
{
    for (const auto &entry : futex_waits)
        if (entry.pid == pid && entry.tid == tid) return entry.object;
    return {};
}
void emit_futex_arm(const FutexWait &wait, uptr pc)
{
#if defined(OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE) && defined(OK_ENABLE_TEST_POINTS)
    if (!wait.origin) return;
    using Codec = lu::Codec<lu::configured_native_profile()>;
    const bool time64 = Codec::profile.word_bytes == 8 ||
        Codec::syscall_number(lu::Operation::futex_time64) == wait.syscall;
    emit("OK_NATIVE_FUTEX_ARM pid="); emit_number(wait.pid); emit(" tid="); emit_number(wait.tid);
    emit(" wait="); emit_number(wait.ticket.generation); emit(" origin="); emit_number(wait.origin);
    emit(" pc="); emit_number(pc); emit(" syscall="); emit_number(wait.syscall);
    emit(" address="); emit_number(wait.address); emit(" operation=");
    emit_number(wait.domain == FutexKeyDomain::private_address_space ? 128 : 0);
    emit(" expected="); emit_number(wait.expected); emit(" timeout="); emit_number(wait.timeout_address);
    emit(" deadline="); emit_number(wait.timeout.deadline()); emit(time64 ? " format=time64" : " format=time32");
    emit(" live="); emit_number(futex_objects.accounting().live_objects); emit("\n");
#else
    static_cast<void>(wait); static_cast<void>(pc);
#endif
}
void emit_shared_futex_arm(const FutexWait &wait)
{
#ifdef OK_ENABLE_TEST_POINTS
    if (!wait.key.shared) return;
    emit("OK_SHARED_FUTEX_ARM pid="); emit_number(wait.pid);
    emit(" tid="); emit_number(wait.tid);
    emit(" owner="); emit_number(reinterpret_cast<uptr>(wait.key.frame.owner));
    emit(" index="); emit_number(wait.key.frame.index);
    emit(" generation="); emit_number(wait.key.frame.generation);
    emit(" offset="); emit_number(wait.key.word_offset);
    emit(" ticket="); emit_number(wait.ticket.generation);
    emit(" pinned="); emit_number(page_pool.pinned_count()); emit("\n");
#else
    static_cast<void>(wait);
#endif
}
void emit_shared_futex_wake(const FutexWait::Key &key, usize count)
{
#ifdef OK_ENABLE_TEST_POINTS
    if (!key.shared) return;
    emit("OK_SHARED_FUTEX_WAKE pid="); emit_number(key.pid);
    emit(" owner="); emit_number(reinterpret_cast<uptr>(key.frame.owner));
    emit(" index="); emit_number(key.frame.index);
    emit(" generation="); emit_number(key.frame.generation);
    emit(" offset="); emit_number(key.word_offset);
    emit(" count="); emit_number(count);
    emit(" pinned="); emit_number(page_pool.pinned_count()); emit("\n");
#else
    static_cast<void>(key); static_cast<void>(count);
#endif
}
Status cancel_futex(Kernel &kernel, memory::ObjectHandle object, std::string_view reason = "cancelled", i64 result = -4)
{
    auto resolved = futex_objects.resolve(object);
    if (!resolved)
    {
        runtime_failure = resolved.status();
        started = false;
        return resolved.status();
    }
    const auto wait = *resolved.value();
    const auto ticket = wait.ticket;
    syscall_origins.retire_futex(wait.pid, wait.tid, futex_origin_binding(object, wait));
    if (ticket.generation)
        static_cast<void>(kernel.scheduler().complete_wait(ticket, sched::WaitCompletion::cancelled));
    if (wait.pin_owned)
    {
        // Keep the object and index discoverable if the allocator rejects the
        // retained generation. Native execution stops with that exact owner.
        if (auto status = page_pool.unpin(wait.key.frame); !status)
        {
            runtime_failure = status;
            started = false;
            return status;
        }
        resolved.value()->pin_owned = false;
    }
    if (resolved.value()->pending_transfer_pin_owned)
    {
        if (auto status = page_pool.unpin(resolved.value()->pending_transfer_frame); !status)
        {
            runtime_failure = status;
            started = false;
            return status;
        }
        resolved.value()->pending_transfer_pin_owned = false;
        resolved.value()->pending_transfer_frame = {};
    }
    for (auto &entry : futex_waits)
        if (entry.object == object)
            entry = {};
    if (auto status = futex_objects.destroy(object); !status)
    { runtime_failure = status; started = false; return status; }
    if (auto status = futex_objects.trim_empty_pages(); !status)
    { runtime_failure = status; started = false; return status; }
#if defined(OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE) && defined(OK_ENABLE_TEST_POINTS)
    if (wait.origin)
    {
        emit("OK_NATIVE_FUTEX_RETIRE pid="); emit_number(wait.pid); emit(" tid="); emit_number(wait.tid);
        emit(" wait="); emit_number(ticket.generation); emit(" origin="); emit_number(wait.origin);
        emit(" reason="); emit(reason); emit(" result=");
        if (result < 0) { emit("-"); emit_number(static_cast<u64>(-(result + 1)) + 1); }
        else emit_number(static_cast<u64>(result));
        emit(" live="); emit_number(futex_objects.accounting().live_objects); emit("\n");
    }
#else
    static_cast<void>(reason); static_cast<void>(result);
#endif
    return Status::success();
}
Status cancel_futex(Kernel &kernel, sched::ProcessId pid)
{
    for (const auto &entry : futex_waits)
        if (entry.pid == pid)
            if (auto status = cancel_futex(kernel, entry.object); !status) return status;
    return Status::success();
}

[[maybe_unused]] memory::ObjectHandle poll_object(sched::ProcessId pid, sched::ThreadId tid)
{
    for (const auto &entry : poll_waits)
        if (entry.pid == pid && entry.tid == tid) return entry.object;
    return {};
}
u64 poll_syscall_number(const hw::UserFrame &frame, bool select_sets)
{
    using Bridge = lu::NativeBridge<lu::configured_native_profile()>;
    using Codec = lu::Codec<lu::configured_native_profile()>;
    const auto call = Bridge::decode_call(frame, 0);
    if (!call) return 0;
    auto is = [&](lu::Operation operation) {
        const auto number = Codec::syscall_number(operation);
        return number && *number == call.value().number;
    };
    const bool matches = select_sets
        ? is(lu::Operation::select) || is(lu::Operation::_newselect) ||
          is(lu::Operation::pselect6) || is(lu::Operation::pselect6_time64)
        : is(lu::Operation::poll) || is(lu::Operation::ppoll) || is(lu::Operation::ppoll_time64);
    return matches ? call.value().number : 0;
}
void emit_poll_arm(const PollWait &wait)
{
#if defined(OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE) && defined(OK_ENABLE_TEST_POINTS)
    if (!wait.syscall) return;
    emit("OK_NATIVE_POLL_ARM pid="); emit_number(wait.pid); emit(" tid="); emit_number(wait.tid);
    emit(" wait="); emit_number(wait.ticket.generation); emit(" pc="); emit_number(wait.resume);
    emit(" syscall="); emit_number(wait.syscall); emit(wait.select_sets ? " kind=select" : " kind=poll");
    emit(" count="); emit_number(wait.count); emit(" deadline="); emit_number(wait.timeout.deadline());
    emit(" live="); emit_number(poll_objects.accounting().live_objects); emit("\n");
#else
    static_cast<void>(wait);
#endif
}
void cancel_poll(Kernel &kernel, memory::ObjectHandle object, std::string_view reason = "cancelled", i64 result = -4)
{
    auto found = poll_objects.resolve(object);
    if (!found)
    {
        runtime_failure = found.status();
        return;
    }
    const auto wait = *found.value();
#ifdef OK_PRIVATE_POLL_MASK
    if (auto *owner = kernel.scheduler().find_thread(wait.pid, wait.tid))
        poll_signal_masks.complete(*owner, object, wait.ticket, false);
#endif
    // Remove membership before dropping any references or the object storage.
    for (auto &entry : poll_waits)
        if (entry.pid == wait.pid && entry.tid == wait.tid && entry.object == object)
            entry = {};
    if (wait.ticket.generation)
        static_cast<void>(kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::cancelled));
    for (usize i = 0; i < wait.count; ++i)
        if (wait.descriptions[i] != sched::invalid_description)
            if (auto status = kernel.scheduler().release_description(wait.descriptions[i]); !status)
                runtime_failure = status;
    if (auto status = poll_objects.destroy(object); !status) runtime_failure = status;
    if (auto status = poll_objects.trim_empty_pages(); !status) runtime_failure = status;
#if defined(OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE) && defined(OK_ENABLE_TEST_POINTS)
    if (wait.syscall)
    {
        emit("OK_NATIVE_POLL_RETIRE pid="); emit_number(wait.pid); emit(" tid="); emit_number(wait.tid);
        emit(" wait="); emit_number(wait.ticket.generation); emit(" reason="); emit(reason); emit(" result=");
        if (result < 0) { emit("-"); emit_number(static_cast<u64>(-(result + 1)) + 1); }
        else emit_number(static_cast<u64>(result));
        emit(" live="); emit_number(poll_objects.accounting().live_objects); emit("\n");
    }
#else
    static_cast<void>(reason); static_cast<void>(result);
#endif
}

void cancel_poll(Kernel &kernel, sched::ProcessId pid)
{
    for (const auto &entry : poll_waits)
        if (entry.pid == pid)
            cancel_poll(kernel, entry.object);
}

#include "native_udp_poll_runtime.hpp"

Result<usize> poll_ready(Kernel &kernel, PollWait &wait)
{
    usize count = 0;
    wait.ready_sets = {};
    for (usize i = 0; i < wait.count; ++i)
    {
        auto &entry = wait.fds[i];
        entry.revents = 0;
        if (entry.fd < 0)
            continue;
        if (wait.descriptions[i] == sched::invalid_description)
            entry.revents = OK_POLLNVAL;
        else
        {
            const auto requested = static_cast<u16>(entry.events);
            u16 canonical{};
            if (requested & (OK_POLLIN | wait.read_normal)) canonical |= OK_POLLIN;
            if (requested & (OK_POLLOUT | wait.write_normal)) canonical |= OK_POLLOUT;
            auto ready = wait.linux_events
                ? retained_description_readiness<posix::ReadinessPolicy::linux>(kernel, wait.descriptions[i], static_cast<posix::ReadyEvents>(canonical))
                : retained_description_readiness<posix::ReadinessPolicy::native>(kernel, wait.descriptions[i], static_cast<posix::ReadyEvents>(canonical));
            if (!ready)
                return ready.status();
            u16 events = static_cast<u16>(ready.value().events);
            if (ready.value().console_input)
            {
                if (!wait.console_epoch || !kernel.scheduler().console_authorized(wait.pid, wait.console_epoch))
                    events |= OK_POLLERR | OK_POLLHUP;
                else if (!kernel.scheduler().console_read_authority(wait.pid, wait.console_epoch))
                    events |= OK_POLLERR;
                else if ((canonical & OK_POLLIN) && kernel.scheduler().console_readable(wait.pid, wait.console_epoch))
                    events |= OK_POLLIN;
            }
            u16 result = events & (OK_POLLERR | OK_POLLHUP | OK_POLLNVAL);
            if (events & OK_POLLIN) result |= requested & (OK_POLLIN | wait.read_normal);
            if (events & OK_POLLOUT) result |= requested & (OK_POLLOUT | wait.write_normal);
            entry.revents = static_cast<i16>(result);
        }
        if (wait.select_sets)
        {
            const auto events = static_cast<u16>(entry.revents);
            if (events & OK_POLLNVAL) return Status::bad_descriptor("select description lost");
            const auto bit = u32{1} << entry.fd;
            // Current providers have no priority source. HUP/ERR never imply
            // exceptional data. Priority support also needs canonical event
            // admission and folding before a future provider can supply it.
            const std::array<bool, 3> ready{bool(events & (OK_POLLIN | OK_POLLHUP | OK_POLLERR)),
                bool(events & (OK_POLLOUT | OK_POLLERR)), bool(events & 0x2)};
            for (usize set = 0; set < ready.size(); ++set)
                if (ready[set] && (wait.sets.interests[set] & bit))
                { wait.ready_sets[set] |= bit; ++count; }
        }
        else count += entry.revents != 0;
    }
    return count;
}

bool poll_output(const hw::AddressSpace &space, const PollWait &wait)
{
    if (wait.select_sets)
    {
        if (!wait.sets.count) return true;
        for (usize set = 0; set < wait.sets.output.size(); ++set)
        {
            if (!wait.sets.output[set]) continue;
            std::array<u8, 8> bytes{};
            const auto width = wait.sets.word_bytes;
            for (usize i = 0; i < width; ++i)
                bytes[i] = static_cast<u8>(u64{wait.ready_sets[set]} >> (8 * (wait.big_endian ? width - 1 - i : i)));
            if (!hw::copy_to_user(space, wait.sets.output[set], bytes.data(), width)) return false;
        }
        return true;
    }
    if (!wait.linux_events && !hw::user_range(space, wait.output, wait.count * sizeof(ok_pollfd), true))
        return false;
    for (usize i = 0; i < wait.count; ++i)
    {
        const auto value = static_cast<u16>(wait.fds[i].revents);
        const std::array<u8, 2> bytes{static_cast<u8>(value >> (wait.big_endian ? 8 : 0)),
                                      static_cast<u8>(value >> (wait.big_endian ? 0 : 8))};
        if (!hw::copy_to_user(space, wait.output + i * sizeof(ok_pollfd) + offsetof(ok_pollfd, revents),
                              bytes.data(), bytes.size()))
            return false;
    }
    return true;
}
extern "C" void ok_platform_console_write_char(char) __attribute__((weak));
extern "C" int ok_platform_console_poll() __attribute__((weak));
Status drain_console_ingress()
{
    if (ok_platform_console_poll == nullptr)
    {
#if defined(OK_KERNEL_FREESTANDING)
        return Status::unsupported("console ingress flush requires its platform reader");
#else
        return {}; // Hosted models have no external UART ingress.
#endif
    }
    for (usize count = 0; count < 1024; ++count)
        if (ok_platform_console_poll() < 0) return {};
    return Status::io_error("console ingress did not become quiescent");
}
void emit(std::string_view text)
{
    if (ok_platform_console_write_char != nullptr)
        for (char c : text)
            ok_platform_console_write_char(c);
}
void emit_number(u64 value)
{
    char digits[20];
    usize count = 0;
    do { digits[count++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
    while (count) emit({&digits[--count], 1});
}
[[maybe_unused]] void emit_hex(u64 value)
{
    char digits[18] = {'0','x'};
    for (usize i=0;i<16;++i) digits[i+2] = "0123456789abcdef"[(value >> ((15-i)*4)) & 15];
    emit({digits,18});
}
void emit_entry_stack_stats()
{
    const auto stats = entry_stacks.accounting();
    if (!stats.backing_bytes) return; // Early scheduler diagnostics precede arena activation.
    static memory::KernelEntryStackAccounting previous{};
    static bool emitted{};
    if (emitted && stats.reservations == previous.reservations && stats.releases == previous.releases &&
        stats.activations == previous.activations && stats.quarantined_slots == previous.quarantined_slots &&
        stats.active_slots == previous.active_slots) return;
    previous = stats;
    emitted = true;
    emit("\nOK_ENTRY_STACKS version=1 scope=actual-user cpu=0 capacity=");
    emit_number(memory::KernelEntryStackPool::capacity);
    emit(" usable_bytes="); emit_number(memory::KernelEntryStackPool::usable_bytes);
    emit(" backing_bytes="); emit_number(stats.backing_bytes);
    emit(" leased="); emit_number(stats.leased_slots);
    emit(" active="); emit_number(stats.active_slots);
    emit(" quarantine="); emit_number(stats.quarantined_slots);
    emit(" peak="); emit_number(stats.peak_slots);
    emit(" reservations="); emit_number(stats.reservations);
    emit(" releases="); emit_number(stats.releases);
    emit(" activations="); emit_number(stats.activations);
    emit(" syscalls="); emit_number(entry_syscalls);
    emit(" timers="); emit_number(entry_timers);
    emit(" faults="); emit_number(entry_faults);
    emit(" rejected="); emit_number(entry_rejections);
    emit(" guards=mapped-canaries smp=0\n");
}
Status validate_group_stacks(const sched::ProcessControlBlock &process)
{
    for (const auto &thread : process.threads())
        if (auto status = validate_entry_stack(entry_stacks, thread); !status) return status;
    return Status::success();
}
Status retire_group_stacks(sched::ProcessControlBlock &process)
{
    if (auto status = validate_group_stacks(process); !status) return status;
    for (auto &thread : process.threads())
        if (auto status = retire_native_thread(thread
#ifdef OK_PRIVATE_POLL_MASK
                                               , "group"
#endif
            ); !status) return status;
    return Status::success();
}
void emit_pool_stats()
{
    emit("\nOK_NATIVE_FRAMES version=2 source=");
    emit(page_pool.source() == NativePagePool::Source::qualified_firmware ? "firmware" : "linker");
    emit(" used="); emit_number(page_pool.used_count());
    emit(" total="); emit_number(page_pool.total_count());
    emit(" allocations="); emit_number(page_pool.allocations());
    emit(" releases="); emit_number(page_pool.releases());
    emit(" pinned="); emit_number(page_pool.pinned_count());
    emit(" user="); emit_number(page_pool.user_count());
    emit(" objects="); emit_number(page_pool.object_count());
    emit(" free="); emit_number(page_pool.free_count());
    emit(" pending="); emit_number(page_pool.pending_count());
    emit(" reserve="); emit_number(page_pool.kernel_reserve_count());
    emit(" user_limit="); emit_number(page_pool.user_capacity_count());
    emit(" object_quarantine="); emit_number(page_pool.quarantined_kernel_page_count()); emit("\n");
    const auto files = shared_files.ledger();
    emit("OK_NATIVE_SHARED_FILES_LEDGER version=1 files="); emit_number(files.files);
    emit(" aliases="); emit_number(files.aliases);
    emit(" retiring_files="); emit_number(files.retiring_files);
    emit(" retained_frames="); emit_number(files.retained_frames);
    emit(" reserved_cells="); emit_number(files.reserved_cells); emit("\n");
}
Status activate_pool()
{
    if (page_pool.source() != NativePagePool::Source::inactive)
        return Status::success();
#ifdef OK_KERNEL_FREESTANDING
    if (!memory_preparation) return memory_preparation;
    if constexpr (!linker_pool_required)
    {
        if (auto status = boot::initialize_qualified_frames(qualified_memory, native_frames); !status)
            return status;
        return page_pool.activate(native_frames, NativePagePool::Source::qualified_firmware, kernel_reserve_pages);
    }
#endif
    const memory::MemoryRegion region{reinterpret_cast<uptr>(physical_pages.data()),sizeof(physical_pages),
                                      memory::RegionType::usable};
    if (auto status = native_frames.initialize({&region,1},hw::page_size); !status) return status;
    return page_pool.activate(native_frames, NativePagePool::Source::linker_reserved, kernel_reserve_pages);
}
#ifdef OK_ENABLE_TEST_POINTS
Status check_runtime_objects()
{
    std::array<memory::ObjectHandle, sched::max_processes> handles{};
    usize count{};
    Status result{};
    const auto users_before = page_pool.user_count();
    for (; count < handles.size(); ++count)
    {
        auto created = poll_objects.create();
        if (!created) { result = created.status(); break; }
        handles[count] = created.value();
        auto resolved = poll_objects.resolve(handles[count]);
        if (!resolved) { result = resolved.status(); ++count; break; }
        auto &record = *resolved.value();
        const auto backing = reinterpret_cast<uptr>(&record) & ~uptr{hw::page_size - 1};
        if (record.pid || record.count || page_pool.owns(backing) || page_pool.handle(backing))
        { result = Status::fault("kernel object initialization or purpose failed"); ++count; break; }
        record.pid = static_cast<sched::ProcessId>(count + 1);
        record.fds.back().fd = static_cast<i32>(count + 117);
    }
    if (result)
    {
        auto excess = poll_objects.create();
        if (excess)
        {
            static_cast<void>(poll_objects.destroy(excess.value()));
            result = Status::fault("kernel object limit was not enforced");
        }
        else if (excess.status().code() != StatusCode::no_memory)
            result = excess.status();
        if (page_pool.object_count() != poll_object_pages || page_pool.user_count() != users_before)
            result = Status::fault("kernel object page accounting failed");
        for (usize i = 0; i < count; ++i)
        {
            auto resolved = poll_objects.resolve(handles[i]);
            if (!resolved || resolved.value()->pid != i + 1 ||
                resolved.value()->fds.back().fd != static_cast<i32>(i + 117))
                result = Status::fault("kernel object writes did not survive allocation");
        }
    }
    for (usize i = 0; i < count; ++i)
    {
        if (auto status = poll_objects.destroy(handles[i]); !status && result) result = status;
        if (poll_objects.resolve(handles[i])) result = Status::fault("destroyed kernel object remains resolvable");
    }
    if (auto status = poll_objects.trim_empty_pages(); !status && result) result = status;
    if (page_pool.object_count()) result = Status::fault("kernel object test retained backing");
    emit("OK_NATIVE_OBJECTS version=1 scope=kernel status="); emit(result ? "pass" : "fail");
    emit(" records="); emit_number(count); emit(" pages="); emit_number(poll_object_pages);
    emit(" slots_per_page="); emit_number(PollObjects::slots_per_page);
    emit(" remaining="); emit_number(page_pool.object_count()); emit("\n");
    return result;
}
#endif
bool same_space(const hw::AddressSpace &a, const hw::AddressSpace &b)
{
    return a.root == b.root && a.slot == b.slot && a.generation == b.generation;
}
Status release_page_reference(const memory::PhysicalFrame &frame, bool &consumed)
{
    consumed = false;
    if (shared_files.owns(frame)) return shared_files.release_alias(frame, consumed);
    const auto status = page_pool.release_reference(frame);
    consumed = status.ok();
    return status;
}
Status release_page_reference(const memory::PhysicalFrame &frame)
{
    bool consumed{};
    return release_page_reference(frame, consumed);
}
Status retain_page_reference(const memory::PhysicalFrame &frame)
{
    return shared_files.owns(frame) ? shared_files.retain_alias(frame) : page_pool.retain_shared(frame);
}
const memory::PhysicalFrame *saved_frame(const Pages &pages, const hw::PageMapping &page)
{
    const auto *frame = pages.frame(page);
    return frame && frame->address == page.physical_address && page_pool.valid(*frame) ? frame : nullptr;
}
const memory::PhysicalFrame *owned_frame(const Pages &pages, const hw::PageMapping &page)
{
    const auto *frame = saved_frame(pages, page);
    return frame && page_pool.reference_count(frame->address) ? frame : nullptr;
}
Status discharge_quarantine(QuarantinedSpace &saved, bool all = false, uptr begin = 0, usize size = 0)
{
    using Ownership = QuarantinedSpace::Ownership;
    Status result{};
    for (usize index = 0; index < saved.pages.size(); ++index)
    {
        const auto &page = saved.pages[index];
        const bool selected = all || (size && page.virtual_address >= begin && page.virtual_address - begin < size);
        if (selected && saved.ownership[index] == Ownership::borrowed) saved.ownership[index] = Ownership::pending;
    }
    if (!saved.pin_complete && !saved.table_retired)
        return Status::busy("quarantine cannot discharge an incompletely pinned native table");
    for (usize index = 0; index < saved.pages.size(); ++index)
    {
        const auto &page = saved.pages[index];
        auto &ownership = saved.ownership[index];
        if (ownership != Ownership::pending) continue;
        const auto *frame = owned_frame(saved.pages, page);
        if (!frame)
        {
            if (result) result = Status::fault("quarantine lost exact owner discharge authority");
            continue;
        }
        bool consumed{};
        const auto status = release_page_reference(*frame, consumed);
        // Alias discharge can succeed before a fallible canonical-file
        // retirement. Such a failure must never retry the consumed alias.
        if (consumed) ownership = Ownership::discharged;
        if (!status && result) result = status;
    }
    return result;
}
Status finish_quarantine(QuarantinedSpace &saved, bool retry_owners = true)
{
    if (!saved.table_retired) return Status::busy("quarantined native table still borrows mapping metadata");
    Status result = retry_owners ? discharge_quarantine(saved) : Status{};
    for (usize index = 0; index < saved.pages.size(); ++index)
    {
        if (saved.ownership[index] == QuarantinedSpace::Ownership::pending) continue;
        if (!saved.pin_held[index]) continue;
        const auto *frame = saved_frame(saved.pages, saved.pages[index]);
        if (!frame)
        {
            if (result) result = Status::fault("quarantine lost exact pin retirement authority");
            continue;
        }
        const auto status = page_pool.unpin(*frame);
        if (status)
        {
            saved.pin_held[index] = false;
            --saved.pinned_count;
        }
        else if (result) result = status;
    }
    for (usize index = 0; index < saved.pages.size(); ++index)
        if (saved.ownership[index] == QuarantinedSpace::Ownership::pending || saved.pin_held[index])
            return result ? Status::busy("quarantine still owns failed frame retirement") : result;
    if (auto status = saved.pages.clear(); !status) return status;
    saved.space = {};
    saved.used = false;
    saved.reserved = false;
    saved.table_retired = false;
    saved.pin_complete = false;
    return result;
}
Status retry_quarantined_retirement()
{
    Status result{};
    for (auto &saved : quarantined_spaces)
        if (saved.used && saved.table_retired)
            if (auto status = finish_quarantine(saved); !status && result) result = status;
    if (!result) runtime_failure = result;
    return result;
}
// A range transaction reserves retry metadata before acquiring data ownership
// or publishing replacement tables. Unselected records remain borrowed.
class PageRetirementReservation final : private NonCopyable
{
  public:
    PageRetirementReservation(const PageRetirementReservation &) = delete;
    PageRetirementReservation &operator=(const PageRetirementReservation &) = delete;
    PageRetirementReservation &operator=(PageRetirementReservation &&) = delete;
    PageRetirementReservation(PageRetirementReservation &&other) noexcept
        : saved_(std::exchange(other.saved_, nullptr)) {}
    ~PageRetirementReservation() { if (saved_) saved_->reserved = false; }
    [[nodiscard]] static Result<PageRetirementReservation> reserve()
    {
        for (auto &saved : quarantined_spaces)
            if (!saved.used && !saved.reserved)
            {
                saved.reserved = true;
                return PageRetirementReservation{saved};
            }
        return Status::no_memory("native range retirement bookkeeping is full");
    }
    Status release_range(Pages &pages, uptr begin, usize size)
    {
        if (!saved_ || !saved_->reserved || saved_->used)
            return Status::fault("native range retirement reservation lost its owner");
        auto &saved = *saved_;
        saved.pages = std::move(pages);
        saved.space = {};
        saved.used = true;
        saved.reserved = false;
        saved.table_retired = true;
        saved.pin_complete = true;
        saved.pinned_count = 0;
        saved.pin_held.fill(false);
        saved.ownership.fill(QuarantinedSpace::Ownership::borrowed);
        saved_ = nullptr;
        const auto discharged = discharge_quarantine(saved, false, begin, size);
        const auto retired = finish_quarantine(saved, false);
        if (!discharged) return discharged;
        return retired;
    }
  private:
    explicit PageRetirementReservation(QuarantinedSpace &saved) : saved_(&saved) {}
    QuarantinedSpace *saved_{};
};
Status quarantine(const hw::AddressSpace &space, Pages &pages)
{
    for (const auto &saved : quarantined_spaces)
        if (saved.used && same_space(saved.space,space))
        {
            if (pages.empty()) return Status::success();
            runtime_failure = Status::already_exists("native space is already quarantined");
            __builtin_trap();
        }
    for (auto &saved : quarantined_spaces)
        if (!saved.used && !saved.reserved)
        {
            saved.pages = std::move(pages);
            saved.space = space;
            saved.used = true;
            saved.table_retired = false;
            saved.ownership.fill(QuarantinedSpace::Ownership::borrowed);
            saved.pin_held.fill(false);
            saved.pinned_count = 0;
            saved.pin_complete = false;
            for (usize index = 0; index < saved.pages.size(); ++index)
            {
                const auto *frame = owned_frame(saved.pages, saved.pages[index]);
                if (!frame) return runtime_failure = Status::fault("quarantine lost its exact mapping frame");
                if (auto status = page_pool.pin(*frame); !status)
                {
                    runtime_failure = status;
                    // Retain the metadata and every ownership edge. No alias
                    // may be discharged while this borrow is incompletely pinned.
                    return status;
                }
                saved.pin_held[index] = true;
                ++saved.pinned_count;
            }
            saved.pin_complete = true;
            return Status::success();
        }
    // Returning here would destroy the caller's metadata while an unretired
    // backend may still borrow its view. This is a backend slot-contract breach.
    runtime_failure = Status::overflow("native retirement quarantine exhausted");
    __builtin_trap();
}
Status forget_quarantine(const hw::AddressSpace &space)
{
    for (auto &saved : quarantined_spaces)
        if (saved.used && same_space(saved.space,space))
        {
            saved.table_retired = true;
            const auto status = finish_quarantine(saved);
            if (!status) runtime_failure = status;
            return status;
        }
    return {};
}
Status retire_space(hw::AddressSpace &space, Pages &pages, uptr staged_begin = 0, usize staged_size = 0)
{
    const auto previous = space;
    for (auto &saved : quarantined_spaces)
        if (saved.used && saved.table_retired && same_space(saved.space, previous))
        {
            space = {};
            return forget_quarantine(previous);
        }
    if (auto status = hw::destroy_space(space); !status)
    {
        if (auto retained = quarantine(previous, pages); !retained)
        {
            if (staged_size)
                for (auto &saved : quarantined_spaces)
                    if (saved.used && same_space(saved.space, previous))
                        static_cast<void>(discharge_quarantine(saved, false, staged_begin, staged_size));
            runtime_failure = retained;
            return retained;
        }
        // mmap owns freshly allocated backing only in the replacement. Once
        // the failed table is pinned, mark those frames pending so successful
        // later retirement returns them to the allocator.
        if (staged_size)
            for (auto &saved : quarantined_spaces)
                if (saved.used && saved.pin_complete && same_space(saved.space, previous))
                    if (auto released = discharge_quarantine(saved, false, staged_begin, staged_size); !released)
                        runtime_failure = released;
        runtime_failure = status;
        return status;
    }
    return forget_quarantine(previous);
}
void release(hw::AddressSpace &space, Pages &pages)
{
    for (const auto &page : pages)
        if (!owned_frame(pages, page))
        { runtime_failure = Status::fault("native teardown lost exact mapping frame authority"); return; }
    if (space.root != 0 && !retire_space(space,pages))
    {
        // A failed final retirement transferred the owner into quarantine.
        // Every user frame becomes pending while its borrowed view is pinned.
        if (pages.empty())
            for (auto &saved : quarantined_spaces)
                if (saved.used && same_space(saved.space, space))
                    if (auto status = discharge_quarantine(saved, true); !status) runtime_failure = status;
        return;
    }
    if (pages.empty())
    {
        if (auto status = pages.clear(); !status) runtime_failure = status;
        return;
    }
    // Reserve bounded retry bookkeeping before consuming any ownership edge,
    // including when no hardware table was ever created for these staged pages.
    for (auto &saved : quarantined_spaces)
        if (!saved.used && !saved.reserved)
        {
            saved.pages = std::move(pages);
            saved.space = space;
            saved.used = true;
            saved.table_retired = true;
            saved.pin_complete = true;
            saved.pinned_count = 0;
            saved.pin_held.fill(false);
            saved.ownership.fill(QuarantinedSpace::Ownership::borrowed);
            const auto discharged = discharge_quarantine(saved, true);
            const auto retired = finish_quarantine(saved, false);
            if (!discharged) runtime_failure = discharged;
            else if (!retired) runtime_failure = retired;
            return;
        }
    runtime_failure = Status::no_memory("native frame retirement bookkeeping is full");
}
Status allocate(Pages &pages, uptr address, u8 flags, bool shared = false)
{
    for (const auto &page : pages)
        if (page.virtual_address == address)
            return Status::invalid_argument("ELF pages overlap");
#ifndef OK_KERNEL_FREESTANDING
    if (auto status = activate_pool(); !status) return status;
#endif
    if (auto status = pages.reserve(page_pool, pages.size() + 1, true); !status) return status;
    auto frame = page_pool.allocate_frame();
    if (!frame) return frame.status();
    auto *bytes = reinterpret_cast<std::byte *>(frame.value().address);
    for (usize i=0;i<hw::page_size;++i) bytes[i] = std::byte{};
    if (shared)
    {
        if (auto status = page_pool.mark_shared(frame.value()); !status)
        {
            static_cast<void>(page_pool.release_reference(frame.value()));
            return status;
        }
    }
    auto status = pages.push_back({address,frame.value().address,flags}, frame.value());
    if (!status) static_cast<void>(page_pool.release_reference(frame.value()));
    return status;
}
// Native VM retains allocation handles in the canonical registry. Backends only
// receive accessible mappings; PROT_NONE retains backing without a user PTE.
Result<hw::AddressSpace> create_page_space(const Pages &pages)
{
    if (!pages.exact_frames())
        return Status::fault("native address space requires exact mapping frame tokens");
    bool accessible = false;
    for (const auto &page : pages)
    {
        if (!owned_frame(pages, page))
            return Status::fault("user mapping requires a retained user-purpose frame");
        accessible |= page.flags != 0;
    }
    if (!accessible)
        return Status::no_memory("native address space requires accessible image pages");
    return hw::create_space(pages.view());
}

Result<usize> native_page_length(usize length)
{
    if (length == 0 || length > ~usize{0} - (hw::page_size - 1))
        return Status::invalid_argument("invalid native VM length");
    return (length + hw::page_size - 1) & ~(hw::page_size - 1);
}

Result<u8> native_page_flags(uptr protection, bool write_implies_read = false)
{
    // Every current architecture backend requires read permission on a
    // writable PTE. The sealed-file adapter admits PROT_WRITE with that
    // existing read/write representation, rather than promising write-only PTEs.
    if (write_implies_read && protection == posix::prot_WRITE)
        return static_cast<u8>(posix::prot_READ | posix::prot_WRITE);
    if (protection != posix::prot_NONE && protection != posix::prot_READ &&
        protection != (posix::prot_READ | posix::prot_WRITE))
        return Status::invalid_argument("native anonymous VM supports none, read or read/write protection");
    return static_cast<u8>(protection);
}

bool valid_native_range(uptr address, usize length)
{
    return address >= hw::user_begin && address < hw::user_end &&
           (address & (hw::page_size - 1)) == 0 && length <= hw::user_end - address;
}

bool private_signal_reserved_range(const sched::ProcessResources &resources, uptr address, usize length)
{
#ifdef OK_PRIVATE_SIGNAL_HAS_TRAMPOLINE
    // The caller already checked this complete interval against the user window.
    return resources.personality == Personality::linux && length &&
        address < private_signal_code_address + hw::page_size &&
        (address >= private_signal_code_address || length > private_signal_code_address - address);
#else
    static_cast<void>(resources); static_cast<void>(address); static_cast<void>(length);
    return false;
#endif
}

bool owns_pool_page(const Pages &pages, const hw::PageMapping &page)
{
    return owned_frame(pages, page) != nullptr;
}
bool shared_pool_page(const Pages &pages, const hw::PageMapping &page)
{
    return owned_frame(pages, page) && page_pool.is_shared(page.physical_address);
}
enum class FutexKeyAccess : u8 { retained_write, read };
Result<FutexWait::Key> resolve_futex_key(const sched::ProcessControlBlock &process,
                                        uptr address, FutexKeyDomain domain,
                                        FutexKeyAccess access = FutexKeyAccess::retained_write)
{
    if (address % sizeof(u32)) return Status::invalid_argument("futex word is misaligned");
    if (address > hw::user_end - sizeof(u32)) return Status::fault("futex word exceeds user bounds");
    FutexWait::Key key{};
    key.domain = domain;
    key.pid = process.pid();
    key.address = address;
    if (domain == FutexKeyDomain::private_address_space)
        return key;
    if (!hw::user_range(process.resources().native_space, address, sizeof(u32),
                        access == FutexKeyAccess::retained_write))
        return Status::fault("ordinary futex word has invalid access");
    for (const auto &page : process.resources().native_pages)
    {
        if (address < page.virtual_address || address - page.virtual_address > hw::page_size - sizeof(u32))
            continue;
        const auto *frame = owned_frame(process.resources().native_pages, page);
        if (!frame || !frame->owner || !frame->generation || frame->address != page.physical_address)
            return Status::fault("ordinary futex backing lost canonical owner");
        if (shared_pool_page(process.resources().native_pages, page))
        {
            key.shared = true;
            key.frame = *frame;
            key.word_offset = address - page.virtual_address;
        }
        else if (access == FutexKeyAccess::read &&
                 !hw::user_range(process.resources().native_space, address, sizeof(u32), true))
            return Status::fault("ordinary private anonymous futex backing is read-only");
        return key;
    }
    return Status::fault("ordinary futex word has no native mapping record");
}
Status page_memory_map(const Pages &pages, sched::MemoryMap &map)
{
    for (usize first = 0; first < pages.size();)
    {
        usize last = first + 1;
        while (last < pages.size() && pages[last].flags == pages[first].flags &&
               shared_pool_page(pages, pages[last]) == shared_pool_page(pages, pages[first]) &&
               pages[last].virtual_address == pages[last - 1].virtual_address + hw::page_size)
            ++last;
        if (auto status = map.add_area({pages[first].virtual_address, (last - first) * hw::page_size,
                                      memory::page_user | pages[first].flags |
                                          (shared_pool_page(pages, pages[first]) ? static_cast<usize>(memory::page_shared) : usize{0})}); !status.ok())
            return Status::no_memory("native VM area capacity exceeded");
        first = last;
    }
    return Status::success();
}

Status replace_native_pages(sched::ProcessResources &resources, Pages &candidate,
                            bool &can_release_staged, uptr staged_begin = 0, usize staged_size = 0)
{
    // Called only after enter() restored the kernel address space, on the boot
    // CPU. Build everything before retiring the old tables or publishing state.
    sched::MemoryMap memory;
    if (auto status = page_memory_map(candidate, memory); !status.ok())
        return status;
    auto replacement = create_page_space(candidate);
    if (!replacement)
        return replacement.status();
    auto previous = resources.native_space;
    const auto previous_identity = previous;
    auto origin_update = syscall_origins.stage_space(runtime_kernel ? &runtime_kernel->scheduler() : nullptr,
        resources, previous_identity, replacement.value());
    if (!origin_update)
    {
        if (auto rollback = retire_space(replacement.value(), candidate, staged_begin, staged_size); !rollback)
        { can_release_staged = false; return rollback; }
        return origin_update.status();
    }
#ifdef OK_PRIVATE_PPC32_SIGNAL
    if (!private_ppc_space_update(resources, previous_identity, replacement.value(), false))
    {
        private_ppc_cancel_transactions();
        runtime_failure = Status::fault("private PPC mapping preflight lost readiness");
        started = false;
        if (auto rollback = retire_space(replacement.value(), candidate, staged_begin, staged_size); !rollback)
        { can_release_staged = false; return rollback; }
        return runtime_failure;
    }
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
    if (!private_n64_space_update(resources, previous_identity, replacement.value(), false))
    {
        if (auto rollback = retire_space(replacement.value(), candidate, staged_begin, staged_size); !rollback)
        { can_release_staged = false; return rollback; }
        runtime_failure = Status::fault("private n64 mapping publication preflight failed");
        return runtime_failure;
    }
#endif
    if (auto status = hw::destroy_space(previous); !status.ok())
    {
#ifdef OK_PRIVATE_PPC32_SIGNAL
        private_ppc_cancel_transactions();
        runtime_failure = Status::fault("private PPC mapping retirement failed after staging");
        started = false;
#endif
        if (auto rollback = retire_space(replacement.value(),candidate, staged_begin, staged_size); !rollback.ok())
        {
            // Quarantine staged pages if a backend cannot retire fresh tables.
            can_release_staged = false;
            return rollback;
        }
        return status;
    }
    if (auto status = forget_quarantine(resources.native_space); !status)
    {
        runtime_failure = status;
        can_release_staged = false;
        return status;
    }
    resources.native_space = replacement.value();
    resources.native_pages = std::move(candidate);
    resources.memory = memory;
    if (!syscall_origins.commit_space(origin_update.value(), runtime_kernel ? &runtime_kernel->scheduler() : nullptr,
                                     resources))
    {
        can_release_staged = false;
        runtime_failure = Status::fault("committed mapping replacement lost syscall origin owner");
        started = false;
        return runtime_failure;
    }
#ifdef OK_PRIVATE_PPC32_SIGNAL
    if (!private_ppc_space_update(resources, previous_identity, replacement.value(), true))
    {
        private_ppc_cancel_transactions();
        can_release_staged = false;
        runtime_failure = Status::fault("private PPC committed mapping publication failed");
        started = false;
        return runtime_failure;
    }
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
    if (!private_n64_space_update(resources, previous_identity, replacement.value(), true))
    {
        can_release_staged = false;
        runtime_failure = Status::fault("private n64 committed mapping publication failed");
        return runtime_failure;
    }
#endif
    return Status::success();
}

Status store(Pages &pages, uptr address, const void *data, usize count);

Result<uptr> native_mmap(sched::ProcessResources &resources, uptr hint, usize length,
                         uptr protection, uptr flags, uptr fd, uptr offset,
                         fs::Node *source = nullptr, usize file_size = 0, SharedFileHandle shared_file = {})
{
    const bool fixed = flags & posix::map_FIXED;
    const bool anonymous = flags & posix::map_ANONYMOUS;
    const bool shared = flags & posix::map_SHARED;
    const bool private_mapping = flags & posix::map_PRIVATE;
    if (shared == private_mapping ||
        (flags & ~uptr(posix::map_FIXED | posix::map_ANONYMOUS | posix::map_PRIVATE | posix::map_SHARED)))
        return Status::invalid_argument("native mmap requires exactly one of MAP_PRIVATE and MAP_SHARED");
    if (shared && !anonymous && !shared_file)
        return Status::unsupported("shared file mappings require a coherent page cache");
    if (anonymous)
    {
        if (static_cast<__INTPTR_TYPE__>(fd) != -1 || offset != 0 || source || shared_file)
            return Status::invalid_argument("anonymous native mapping requires fd -1 and offset zero");
    }
    else
    {
        if (shared_file && private_mapping)
            return Status::unsupported("shared file private snapshots are not admitted");
        if (static_cast<__INTPTR_TYPE__>(fd) < 0 || (!source && !shared_file))
            return Status::bad_descriptor("private file mapping requires a readable regular file");
        if (offset % hw::page_size || offset > ~usize{0} - length)
            return Status::invalid_argument("private file mapping offset is unaligned or wraps");
        if (shared_file && (length % hw::page_size || offset > file_size || length > file_size - offset))
            return Status::unsupported("shared file mapping requires complete pages inside sealed size");
        // Without a file-fault path, a complete page beyond EOF cannot deliver
        // SIGBUS. Refuse it instead of silently exposing anonymous zero pages.
        if (offset >= file_size || (length > hw::page_size &&
            (length - 1) / hw::page_size * hw::page_size >= file_size - offset))
            return Status::unsupported("private file mapping includes a page beyond EOF");
    }
    auto rounded = native_page_length(length);
    if (!rounded)
        return rounded.status();
    auto page_flags = native_page_flags(protection, static_cast<bool>(shared_file));
    if (!page_flags)
        return page_flags.status();
    if (shared_file && (protection & posix::prot_WRITE))
        if (auto status = shared_files.check_write(shared_file, 1); !status) return status;
    const usize size = rounded.value();
    auto available = [&](uptr address) {
        if (!valid_native_range(address, size))
            return false;
        for (const auto &page : resources.native_pages)
            if (page.virtual_address >= address && page.virtual_address - address < size)
                return false;
        return true;
    };
    uptr address = hint & ~(hw::page_size - 1);
    if (fixed && (address != hint || !valid_native_range(address, size)))
        return Status::invalid_argument("native fixed mapping range is invalid");
    if (fixed && private_signal_reserved_range(resources, address, size))
        return Status::denied("private signal return page is reserved");
    if (!fixed && (hint == 0 || !available(address)))
    {
        // Keep ordinary allocations away from the conventional ELF base. A
        // hint remains optional: occupied/invalid hints use the same first fit.
        address = hw::user_begin + 16 * 1024 * 1024;
        while (valid_native_range(address, size))
        {
            uptr next = address;
            for (const auto &page : resources.native_pages)
                if (page.virtual_address >= address && page.virtual_address - address < size &&
                    page.virtual_address + hw::page_size > next)
                    next = page.virtual_address + hw::page_size;
            if (next == address)
                break;
            address = next;
        }
        if (!available(address))
            return Status::no_memory("native virtual address window exhausted");
    }
    Pages candidate;
    usize removed_count = 0;
    for (const auto &page : resources.native_pages)
        if (fixed && page.virtual_address >= address && page.virtual_address - address < size)
        {
            if (!owns_pool_page(resources.native_pages, page))
                return Status::fault("native fixed mapping has no pool owner");
            ++removed_count;
        }
    if (size / hw::page_size > Pages::capacity - (resources.native_pages.size() - removed_count))
        return Status::no_memory("native process page capacity exceeded");
    auto retirement_result = PageRetirementReservation::reserve();
    if (!retirement_result) return retirement_result.status();
    auto retirement = std::move(retirement_result.value());
    if (auto status = candidate.reserve(page_pool,
            resources.native_pages.size() - removed_count + size / hw::page_size, true); !status) return status;
    for (const auto &page : resources.native_pages)
        if (fixed && page.virtual_address >= address && page.virtual_address - address < size)
            continue;
        else
        {
            const auto *frame = owned_frame(resources.native_pages, page);
            if (!frame) return Status::fault("native mapping copy lost exact frame authority");
            if (auto status = candidate.push_back(page, *frame); !status) return status;
        }
    auto rollback_pages = [&] {
        if (auto status = retirement.release_range(candidate, address, size); !status)
            runtime_failure = status;
    };
    for (usize mapping_offset = 0; mapping_offset < size; mapping_offset += hw::page_size)
    {
        Status status{};
        if (shared_file)
        {
            auto frame = shared_files.page(shared_file, (offset + mapping_offset) / hw::page_size);
            if (!frame) status = frame.status();
            else if (auto retained = shared_files.retain_alias(frame.value()); !retained) status = retained;
            else
            {
                status = candidate.push_back({address + mapping_offset, frame.value().address, page_flags.value()}, frame.value());
                if (!status) static_cast<void>(shared_files.release_alias(frame.value()));
            }
        }
        else status = allocate(candidate, address + mapping_offset, page_flags.value(), shared);
        if (!status)
        {
            rollback_pages();
            return status;
        }
    }
    if (source)
    {
        const usize remaining = file_size - offset;
        // The requested length reserves complete pages. Bytes between length
        // and the end of its final page still reflect the file up to EOF.
        const usize file_bytes = size < remaining ? size : remaining;
        usize copied = 0;
        while (copied < file_bytes)
        {
            const usize chunk = file_bytes - copied < fs::max_file_data ? file_bytes - copied : fs::max_file_data;
            auto read = source->read(offset + copied, chunk);
            if (!read)
            {
                rollback_pages();
                return read.status();
            }
            if (!read.value().size || read.value().size > chunk)
            {
                rollback_pages();
                return Status::io_error("private file mapping changed during staging");
            }
            if (auto status = store(candidate, address + copied,
                                    read.value().data.data(), read.value().size); !status)
            {
                rollback_pages();
                return status;
            }
            copied += read.value().size;
        }
    }
    bool can_release_staged = true;
    if (auto status = replace_native_pages(resources, candidate, can_release_staged, address, size); !status.ok())
    {
        if (can_release_staged)
            rollback_pages();
        else if (!candidate.empty())
            // A post-publication failure leaves the retired prior directory
            // here. Quarantine failures instead transfer and empty candidate.
            if (auto retired = retirement.release_range(candidate, address, size); !retired)
                runtime_failure = retired;
        return status;
    }
    // Fixed replacement keeps old backing live until old page tables retire.
    // Failed staging leaves both the prior bytes and permissions untouched.
    if (fixed)
        if (auto status = retirement.release_range(candidate, address, size); !status)
            runtime_failure = status;
    if (auto status = candidate.clear(); !status) runtime_failure = status;
    return address;
}

Status native_mprotect(sched::ProcessResources &resources, uptr address, usize length, uptr protection)
{
    auto rounded = native_page_length(length);
    if (!rounded)
        return rounded.status();
    if (!valid_native_range(address, rounded.value()))
        return Status::invalid_argument("native protection range is invalid");
    if (private_signal_reserved_range(resources, address, rounded.value()))
        return Status::denied("private signal return page is reserved");
    auto flags = native_page_flags(protection, protection == posix::prot_WRITE);
    if (!flags) return flags.status();
    // Complete authority/protection admission before staging any changed page
    // list or hardware table. A refusal preserves the entire original range.
    usize admitted = 0;
    for (const auto &page : resources.native_pages)
        if (page.virtual_address >= address && page.virtual_address - address < rounded.value())
        {
            const auto *frame = owned_frame(resources.native_pages, page);
            if (!frame) return Status::fault("native protection lost exact frame authority");
            const bool shared_file = shared_files.owns(*frame);
            if (protection == posix::prot_WRITE && !shared_file)
                return Status::unsupported("write-only requests require the sealed shared-file adapter");
            if ((protection & posix::prot_WRITE) && shared_file)
                if (auto status = shared_files.check_mapping_write(*frame); !status) return status;
            ++admitted;
        }
    if (admitted != rounded.value() / hw::page_size)
        return Status::no_memory("native protection range contains unmapped pages");
    auto copied = Pages::clone(resources.native_pages, page_pool);
    if (!copied) return copied.status();
    Pages candidate = std::move(copied.value());
    usize changed = 0;
    for (auto &page : candidate)
        if (page.virtual_address >= address && page.virtual_address - address < rounded.value())
        {
            page.flags = flags.value();
            ++changed;
        }
    if (changed != rounded.value() / hw::page_size)
        return Status::no_memory("native protection range contains unmapped pages");
    bool can_release_staged = true;
    auto status = replace_native_pages(resources, candidate, can_release_staged);
    if (status)
        if (auto retired = candidate.clear(); !retired) runtime_failure = retired;
    return status;
}

Status native_munmap(sched::ProcessResources &resources, uptr address, usize length)
{
    auto rounded = native_page_length(length);
    if (!rounded)
        return rounded.status();
    if (!valid_native_range(address, rounded.value()))
        return Status::invalid_argument("native unmap range is invalid");
    if (private_signal_reserved_range(resources, address, rounded.value()))
        return Status::denied("private signal return page is reserved");
    usize removed = 0;
    for (const auto &page : resources.native_pages)
        if (page.virtual_address >= address && page.virtual_address - address < rounded.value())
        {
            if (!owns_pool_page(resources.native_pages, page))
                return Status::fault("native unmap page has no pool owner");
            ++removed;
        }
    if (!removed)
        return Status::success();
    auto retirement_result = PageRetirementReservation::reserve();
    if (!retirement_result) return retirement_result.status();
    auto retirement = std::move(retirement_result.value());
    Pages candidate;
    if (auto status = candidate.reserve(page_pool, resources.native_pages.size() - removed, true); !status)
        return status;
    for (const auto &page : resources.native_pages)
        if (page.virtual_address < address || page.virtual_address - address >= rounded.value())
        {
            const auto *frame = owned_frame(resources.native_pages, page);
            if (!frame) return Status::fault("native unmap copy lost exact frame authority");
            if (auto status = candidate.push_back(page, *frame); !status) return status;
        }
    bool can_release_staged = true;
    if (auto status = replace_native_pages(resources, candidate, can_release_staged); !status.ok())
    {
        if (!can_release_staged && !candidate.empty())
            if (auto retired = retirement.release_range(candidate, address, rounded.value()); !retired)
                runtime_failure = retired;
        return status;
    }
    // No old user translations can run after successful table retirement.
    if (auto status = retirement.release_range(candidate, address, rounded.value()); !status)
        runtime_failure = status;
    if (auto status = candidate.clear(); !status) runtime_failure = status;
    return Status::success();
}

Result<uptr> native_brk(sched::ProcessResources &resources, uptr address)
{
    if (!address)
        return resources.program_break;
    if (resources.program_break_base < hw::user_begin ||
        resources.program_break_limit >= hw::user_end ||
        address < resources.program_break_base || address > resources.program_break_limit)
        return Status::no_memory("native program break outside heap window");
    const uptr old_end = (resources.program_break + hw::page_size - 1) & ~(hw::page_size - 1);
    const uptr new_end = (address + hw::page_size - 1) & ~(hw::page_size - 1);
    if (new_end > old_end)
    {
        // Unlike MAP_FIXED, brk must never replace an intervening mapping.
        for (const auto &page : resources.native_pages)
            if (page.virtual_address >= old_end && page.virtual_address < new_end)
                return Status::no_memory("native heap growth overlaps an existing mapping");
        auto mapped = native_mmap(resources, old_end, new_end - old_end,
                                  posix::prot_READ | posix::prot_WRITE,
                                  posix::map_PRIVATE | posix::map_ANONYMOUS | posix::map_FIXED,
                                  ~uptr{0}, 0);
        if (!mapped)
            return mapped.status();
    }
    else if (new_end < old_end)
        if (auto status = native_munmap(resources, new_end, old_end - new_end); !status.ok())
            return status;
    resources.program_break = address;
    return address;
}

Status store(Pages &pages, uptr address, const void *data, usize count)
{
    const auto *bytes = static_cast<const std::byte *>(data);
    while (count)
    {
        hw::PageMapping *mapping = nullptr;
        for (auto &page : pages)
            if (page.virtual_address == (address & ~(hw::page_size - 1)))
            {
                mapping = &page;
                break;
            }
        if (!mapping)
            return Status::fault("image destination is unmapped");
        usize offset = address & (hw::page_size - 1);
        usize chunk = hw::page_size - offset;
        if (chunk > count)
            chunk = count;
        copy_bytes(reinterpret_cast<void *>(mapping->physical_address + offset), bytes, chunk);
        address += chunk;
        bytes += chunk;
        count -= chunk;
    }
    return Status::success();
}
struct Arguments
{
    StaticVector<FixedString<256>, 16> argv;
    StaticVector<FixedString<256>, 16> envp;
};
// A local loaded image must reserve failure retention before it acquires any
// pages. Publishing its complete directory into this cell is infallible; a
// failed backend or frame retirement cannot leave a local metadata destructor
// holding the last record of a live translation or exact data-page owner.
class ImageRetirementReservation final : private NonCopyable
{
  public:
    ImageRetirementReservation(const ImageRetirementReservation &) = delete;
    ImageRetirementReservation &operator=(const ImageRetirementReservation &) = delete;
    ImageRetirementReservation &operator=(ImageRetirementReservation &&) = delete;
    ImageRetirementReservation(ImageRetirementReservation &&other) noexcept
        : saved_(std::exchange(other.saved_, nullptr)) {}
    ~ImageRetirementReservation() { cancel(); }
    [[nodiscard]] static Result<ImageRetirementReservation> reserve()
    {
        for (auto &saved : quarantined_spaces)
            if (!saved.used && !saved.reserved)
            {
                saved.reserved = true;
                return ImageRetirementReservation{saved};
            }
        return Status::no_memory("native image retirement bookkeeping is full");
    }
    [[nodiscard]] bool active() const { return saved_ != nullptr; }
    void cancel()
    {
        if (saved_) saved_->reserved = false;
        saved_ = nullptr;
    }
    Status retire(hw::AddressSpace &space, Pages &pages) { return transfer(space, pages, true); }
    Status retain(hw::AddressSpace &space, Pages &pages) { return transfer(space, pages, false); }
  private:
    explicit ImageRetirementReservation(QuarantinedSpace &saved) : saved_(&saved) {}
    Status transfer(hw::AddressSpace &space, Pages &pages, bool destroy_table)
    {
        if (!saved_ || !saved_->reserved || saved_->used)
        {
            // Continuing would let the caller destroy the only metadata for a
            // live image. Move-only, serialized reservation ownership excludes
            // this breach; fail stopped if the invariant is ever violated.
            runtime_failure = Status::fault("native image retirement lost its reservation");
            __builtin_trap();
        }
        auto &saved = *std::exchange(saved_, nullptr);
        saved.pages = std::move(pages);
        saved.space = std::exchange(space, {});
        saved.used = true;
        saved.reserved = false;
        saved.table_retired = saved.space.root == 0;
        saved.pin_complete = saved.table_retired;
        saved.pinned_count = 0;
        saved.pin_held.fill(false);
        saved.ownership.fill(QuarantinedSpace::Ownership::pending);
        for (const auto &page : saved.pages)
            if (!owned_frame(saved.pages, page))
                return Status::fault("native image retirement lost an exact owner token");
        Status result{};
        if (!saved.table_retired && destroy_table)
        {
            result = hw::destroy_space(saved.space);
            saved.table_retired = result.ok();
            saved.pin_complete = saved.table_retired;
        }
        if (!saved.table_retired)
        {
            for (usize index = 0; index < saved.pages.size(); ++index)
            {
                const auto *frame = owned_frame(saved.pages, saved.pages[index]);
                if (!frame) return Status::fault("native image pin lost exact owner token");
                if (auto status = page_pool.pin(*frame); !status) return status;
                saved.pin_held[index] = true;
                ++saved.pinned_count;
            }
            saved.pin_complete = true;
            if (!destroy_table) return Status::success();
        }
        const auto discharged = discharge_quarantine(saved);
        if (!saved.table_retired) return !result ? result : discharged;
        const auto retired = finish_quarantine(saved, false);
        if (!discharged) return discharged;
        return retired;
    }
    QuarantinedSpace *saved_{};
};
struct Image
{
    explicit Image(ImageRetirementReservation owner) : retirement(std::move(owner)) {}
    Image(const Image &) = delete;
    Image &operator=(const Image &) = delete;
    Image &operator=(Image &&) = delete;
    Image(Image &&other) noexcept
        : retirement(std::move(other.retirement)), pages(std::move(other.pages)),
          space(std::exchange(other.space, {}))
    {
#ifdef OK_PRIVATE_PPC32_SIGNAL
        signal_image = other.signal_image;
#endif
        personality = other.personality;
        frame = other.frame;
        memory = other.memory;
        program_break = other.program_break;
    }
    ~Image() { if (retirement.active()) static_cast<void>(retire()); }
    Status retire()
    {
        const auto status = retirement.retire(space, pages);
        if (!status) runtime_failure = status;
        return status;
    }
    Status retain_for_diagnosis()
    {
        const auto status = retirement.retain(space, pages);
        if (!status) runtime_failure = status;
        return status;
    }
    ImageRetirementReservation retirement;
#ifdef OK_PRIVATE_PPC32_SIGNAL
    Ppc32ImageAdmission signal_image{};
#endif
    Personality personality{Personality::native};
    Pages pages;
    hw::AddressSpace space;
    hw::UserFrame frame;
    sched::MemoryMap memory;
    uptr program_break{hw::user_begin};
};
Result<Image> load(Kernel &kernel, std::string_view path, const Arguments &args)
{
    auto resolved = kernel.posix().resolve_node(posix::at_FDCWD, path);
    if (!resolved)
        return resolved.status();
    auto *node = resolved.value().node();
    auto queried = node->query_metadata();
    if (!queried)
        return queried.status();
    const auto metadata = queried.value();
    if (metadata.type != fs::NodeType::regular || (metadata.mode & 0111u) == 0)
        return Status::denied("file is not executable");
    if (auto status = fs::require_access(metadata, {kernel.posix().euid(), kernel.posix().egid()}, fs::access_execute);
        !status.ok())
        return status;
    const usize prefix_size = metadata.size < elf_buffer.size() ? metadata.size : elf_buffer.size();
    usize offset = 0;
    while (offset < prefix_size)
    {
        auto bytes = node->read(offset, prefix_size - offset);
        if (!bytes)
            return bytes.status();
        if (bytes.value().size == 0 || bytes.value().size > prefix_size - offset)
            return Status::invalid_argument("truncated executable");
        copy_bytes(elf_buffer.data() + offset, bytes.value().data.data(), bytes.value().size);
        offset += bytes.value().size;
    }
    constexpr u8 native_encoding = __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__ ? 2 : 1;
    if (metadata.size < 16 || std::to_integer<u8>(elf_buffer[4]) != (sizeof(uptr) == 8 ? 2 : 1) ||
        std::to_integer<u8>(elf_buffer[5]) != native_encoding)
        return Status::invalid_argument("ELF class or byte order does not match the native ABI");
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
    // A required prefix beyond the explicit 128 KiB bound is unsupported. The
    // serialized file read above and ordinary loader still own mapping checks.
    if (!::ok::user::mips64_n64_elf_admission::admit({elf_buffer.data(), prefix_size}, metadata.size))
        return Status::unsupported("private n64 signal ELF profile is not admitted");
#endif
#ifdef OK_PRIVATE_MIPS_O32_SIGNAL
    if (!::ok::user::mips_o32_elf_admission::admit({elf_buffer.data(), prefix_size}, metadata.size).ok())
        return Status::unsupported("private MIPS signal ELF profile is not admitted");
#endif
    auto elf = sched::ElfLoader{}.load_prefix({elf_buffer.data(), prefix_size}, metadata.size, kernel.arch().architecture());
    if (!elf)
        return elf.status();
    auto retained = ImageRetirementReservation::reserve();
    if (!retained) return retained.status();
    Image image{std::move(retained.value())};
    lu::ExecMetadata linux_metadata{};
    bool dynamic_x86 = false;
    uptr interpreter_entry = 0;
#if defined(__x86_64__)
    constexpr std::string_view x86_interpreter_path = "/lib/ld-musl-x86_64.so.1";
    uptr entry_bias = 0;
#endif
    const auto osabi = std::to_integer<u8>(elf_buffer[7]);
    if (osabi == 3)
    {
        constexpr u8 abi_version = LinuxExec::architecture == arch::Architecture::mips ||
                                   LinuxExec::architecture == arch::Architecture::mips64 ? 1 : 0;
        if (std::to_integer<u8>(elf_buffer[8]) != abi_version)
            return Status::unsupported("Linux ELF ABI version differs from configured profile");
        const usize linux_prefix = prefix_size < LinuxExec::max_prefix_bytes ? prefix_size : LinuxExec::max_prefix_bytes;
        if (elf.value().static_executable)
        {
            auto inspected = LinuxExec::inspect({elf_buffer.data(), linux_prefix}, metadata.size);
            if (!inspected)
                return inspected.status();
            linux_metadata = inspected.value().metadata;
        }
        else
        {
#if defined(__x86_64__)
            auto inspected = lu::X86DynamicExec::inspect({elf_buffer.data(), linux_prefix}, metadata.size, false);
            if (!inspected)
                return inspected.status();
            const auto path_offset = inspected.value().interpreter_offset;
            const auto path_size = inspected.value().interpreter_size;
            if (path_size != x86_interpreter_path.size() + 1 ||
                path_offset > linux_prefix || path_size > linux_prefix - path_offset)
                return Status::unsupported("x86-64 dynamic ELF interpreter path differs from pinned musl loader");
            for (usize i = 0; i < x86_interpreter_path.size(); ++i)
                if (std::to_integer<u8>(elf_buffer[path_offset + i]) != static_cast<u8>(x86_interpreter_path[i]))
                    return Status::unsupported("x86-64 dynamic ELF interpreter path differs from pinned musl loader");
            if (elf_buffer[path_offset + x86_interpreter_path.size()] != std::byte{})
                return Status::invalid_argument("x86-64 dynamic ELF interpreter path is not terminated");
            dynamic_x86 = true;
            entry_bias = 0x42000000;
            linux_metadata = inspected.value().metadata;
            if (linux_metadata.program_headers > ~uptr{0} - entry_bias ||
                linux_metadata.entry > ~uptr{0} - entry_bias)
                return Status::overflow("x86-64 dynamic main metadata relocation wraps");
            linux_metadata.program_headers += entry_bias;
            linux_metadata.entry += entry_bias;
#else
            return Status::unsupported("dynamic Linux images require an architecture-qualified loader");
#endif
        }
        image.personality = Personality::linux;
    }
    else if (osabi != 0)
        return Status::unsupported("unsupported ELF syscall personality");
    else if (!elf.value().static_executable)
        return Status::unsupported("native dynamic ELF images are unsupported");
#ifdef OK_PRIVATE_PPC32_SIGNAL
    image.signal_image = Ppc32ImageAdmission::inspect(private_ppc_backend::capability(),
        {elf_buffer.data(), prefix_size}, metadata.size,
        image.personality == Personality::linux ? ppc32_elf_admission::Personality::linux_user :
            ppc32_elf_admission::Personality::native_os,
        ppc32_elf_admission::RuntimeProfile::e500v2_scalar_soft_float);
    if (!image.signal_image.valid(private_ppc_backend::capability()))
        return Status::unsupported("private PPC ELF profile is not admitted");
#endif
    auto fail = [&](Status status) -> Result<Image> {
        return status;
    };
    constexpr uptr stack_top = hw::user_end - 0x100000;
    constexpr usize stack_size = 16 * hw::page_size;
    const uptr main_bias = dynamic_x86 ? uptr{0x42000000} : uptr{0};
    auto map_loaded = [&](const sched::LoadedElf &loaded, fs::Node *source,
                          usize file_size, uptr bias, bool main_program) -> Status {
        bool entry_executable = false;
        constexpr uptr load_limit = stack_top - stack_size - hw::page_size;
        if (loaded.entry > ~uptr{0} - bias)
            return Status::overflow("ELF entry relocation wraps");
        const uptr entry = loaded.entry + bias;
        for (const auto &segment : loaded.segments)
        {
            if (segment.memory_size == 0)
                continue;
            if (segment.virtual_address > ~uptr{0} - bias)
                return Status::overflow("ELF segment relocation wraps");
            const uptr address = segment.virtual_address + bias;
            if (address < hw::user_begin || address >= load_limit ||
                segment.memory_size > load_limit - address ||
                segment.file_offset > file_size || segment.file_size > file_size - segment.file_offset)
                return Status::invalid_argument("ELF segment outside user window");
            u8 flags = hw::page_read;
            if (segment.permissions & memory::page_write)
                flags |= hw::page_write;
            if (segment.permissions & memory::page_execute)
                flags |= hw::page_execute;
            if ((flags & (hw::page_write | hw::page_execute)) == (hw::page_write | hw::page_execute))
                return Status::denied("writable executable pages are forbidden");
            if ((flags & hw::page_execute) && entry >= address && entry - address < segment.memory_size)
                entry_executable = true;
            const uptr begin = address & ~(hw::page_size - 1);
            const uptr end = (address + segment.memory_size + hw::page_size - 1) & ~(hw::page_size - 1);
            if (main_program && end > image.program_break)
                image.program_break = end;
            if (auto status = image.memory.add_area({begin, end - begin, segment.permissions}); !status.ok())
                return status;
            for (uptr page = begin; page < end; page += hw::page_size)
                if (auto status = allocate(image.pages, page, flags); !status.ok())
                    return status;
            // Source identity stays retained until all unpublished pages are populated.
            usize copied = 0;
            while (copied < segment.file_size)
            {
                auto bytes = source->read(segment.file_offset + copied, segment.file_size - copied);
                if (!bytes)
                    return bytes.status();
                if (bytes.value().size == 0 || bytes.value().size > segment.file_size - copied)
                    return Status::invalid_argument("truncated ELF load segment");
                if (auto status = store(image.pages, address + copied, bytes.value().data.data(),
                                        bytes.value().size); !status.ok())
                    return status;
                copied += bytes.value().size;
            }
        }
        return entry_executable ? Status::success() : Status::invalid_argument("ELF entry is not executable");
    };
    if (auto status = map_loaded(elf.value(), node, metadata.size, main_bias, true); !status.ok())
        return fail(status);
#if defined(__x86_64__)
    if (dynamic_x86)
    {
        auto interpreter = kernel.posix().resolve_node(posix::at_FDCWD, x86_interpreter_path);
        if (!interpreter)
            return fail(interpreter.status());
        auto *interpreter_node = interpreter.value().node();
        auto interpreter_query = interpreter_node->query_metadata();
        if (!interpreter_query)
            return fail(interpreter_query.status());
        const auto interpreter_metadata = interpreter_query.value();
        if (interpreter_metadata.type != fs::NodeType::regular || (interpreter_metadata.mode & 0111u) == 0)
            return fail(Status::denied("musl interpreter is not executable"));
        if (auto status = fs::require_access(interpreter_metadata,
                {kernel.posix().euid(), kernel.posix().egid()}, fs::access_execute); !status.ok())
            return fail(status);
        const usize interpreter_prefix = interpreter_metadata.size < lu::X86DynamicExec::max_prefix_bytes ?
            interpreter_metadata.size : lu::X86DynamicExec::max_prefix_bytes;
        usize interpreter_offset = 0;
        while (interpreter_offset < interpreter_prefix)
        {
            auto bytes = interpreter_node->read(interpreter_offset, interpreter_prefix - interpreter_offset);
            if (!bytes)
                return fail(bytes.status());
            if (bytes.value().size == 0 || bytes.value().size > interpreter_prefix - interpreter_offset)
                return fail(Status::invalid_argument("truncated musl interpreter prefix"));
            copy_bytes(elf_buffer.data() + interpreter_offset, bytes.value().data.data(), bytes.value().size);
            interpreter_offset += bytes.value().size;
        }
        auto inspected = lu::X86DynamicExec::inspect(
            {elf_buffer.data(), interpreter_prefix}, interpreter_metadata.size, true);
        if (!inspected)
            return fail(inspected.status());
        constexpr uptr interpreter_bias = 0x60000000;
        if (auto status = map_loaded(inspected.value().loaded, interpreter_node,
                                      interpreter_metadata.size, interpreter_bias, false); !status.ok())
            return fail(status);
        interpreter_entry = interpreter_bias + inspected.value().loaded.entry;
        linux_metadata.interpreter_base = interpreter_bias;
        if (auto status = interpreter.value().release(); !status.ok())
            return fail(status);
    }
#endif
    // Neither source identity is released until both unpublished images are complete.
    if (auto status = resolved.value().release(); !status.ok())
        return fail(status);
    for (uptr address = stack_top - stack_size; address < stack_top; address += hw::page_size)
        if (auto status = allocate(image.pages, address, hw::page_read | hw::page_write); !status.ok())
            return fail(status);
    static_cast<void>(image.memory.add_area(
        {stack_top - stack_size, stack_size, memory::page_user | memory::page_read | memory::page_write}));
    uptr sp = stack_top;
    std::array<uptr, 16> argv{}, envp{};
    auto strings = [&](const auto &values, auto &pointers) -> Status {
        for (usize i = 0; i < values.size(); ++i)
        {
            sp -= values[i].size() + 1;
            pointers[i] = sp;
            if (auto status = store(image.pages, sp, values[i].c_str(), values[i].size() + 1); !status.ok())
                return status;
        }
        return Status::success();
    };
    if (auto status = strings(args.argv, argv); !status.ok())
        return fail(status);
    if (auto status = strings(args.envp, envp); !status.ok())
        return fail(status);
    std::array<uptr, 64> words{};
    usize n = 0;
    words[n++] = args.argv.size();
    for (usize i = 0; i < args.argv.size(); ++i)
        words[n++] = argv[i];
    words[n++] = 0;
    for (usize i = 0; i < args.envp.size(); ++i)
        words[n++] = envp[i];
    words[n++] = 0;
    if (image.personality == Personality::linux)
    {
        sp -= path.size() + 1;
        if (auto status = store(image.pages, sp, path.data(), path.size()); !status.ok())
            return fail(status);
        const char terminator = 0;
        if (auto status = store(image.pages, sp + path.size(), &terminator, 1); !status.ok())
            return fail(status);
        std::array<std::byte, 16> random{};
        auto acquired = kernel.posix().getrandom(random);
        if (!acquired)
            return fail(acquired.status());
        if (acquired.value() != random.size())
            return fail(Status::would_block("incomplete AT_RANDOM entropy"));
        sp -= random.size();
        if (auto status = store(image.pages, sp, random.data(), random.size()); !status.ok())
            return fail(status);
        const uptr random_address = sp;
        std::array<u8, LinuxExec::max_auxv_bytes> auxv{};
        auto encoded = LinuxExec::encode_auxv(auxv, linux_metadata,
            {.page_size = hw::page_size, .uid = kernel.posix().uid(), .euid = kernel.posix().euid(),
             .gid = kernel.posix().gid(), .egid = kernel.posix().egid(),
             .secure = kernel.posix().uid() != kernel.posix().euid() || kernel.posix().gid() != kernel.posix().egid(),
             .execfn = sp + random.size(), .random = random_address});
        if (!encoded)
            return fail(encoded.status());
        if (encoded.value() > (words.size() - n) * sizeof(uptr))
            return fail(Status::overflow("initial auxiliary vector exceeds stack record"));
        copy_bytes(words.data() + n, auxv.data(), encoded.value());
        n += encoded.value() / sizeof(uptr);
    }
    else
    {
        words[n++] = 6;
        words[n++] = hw::page_size; // AT_PAGESZ
        words[n++] = 9;
        words[n++] = elf.value().entry; // AT_ENTRY
        words[n++] = 0;
        words[n++] = 0;
    }
    sp = (sp - n * sizeof(uptr)) & ~uptr(15);
    if (auto status = store(image.pages, sp, words.data(), n * sizeof(uptr)); !status.ok())
        return fail(status);
#ifdef OK_PRIVATE_SIGNAL_HAS_TRAMPOLINE
    if (image.personality == Personality::linux)
    {
        // ELF admission excludes this region above the initial stack. Fill an
        // owned page before publishing RX PTEs; there is no user-writable alias.
        constexpr auto code = PrivateSignalCode::image();
        if (auto status = allocate(image.pages, private_signal_code_address, hw::page_read | hw::page_execute); !status)
            return fail(status);
        if (auto status = store(image.pages, private_signal_code_address, code.data(), code.size()); !status)
            return fail(status);
        if (auto status = image.memory.add_area({private_signal_code_address, hw::page_size,
                memory::page_user | memory::page_read | memory::page_execute}); !status)
            return fail(status);
        // RISC-V entry executes fence.i; LoongArch entry executes dbar/ibar
        // before this new page executes on the serialized boot CPU.
    }
#endif
    auto space = create_page_space(image.pages);
    if (!space)
        return fail(space.status());
    image.space = space.value();
    image.frame = hw::initial_frame(dynamic_x86 ? interpreter_entry : elf.value().entry, sp);
    return image;
}
Status string_from_user(const hw::AddressSpace &space, uptr address, FixedString<256> &result)
{
    result.clear();
    for (usize i = 0; i < 256; ++i)
    {
        char c;
        if (address + i < address || !hw::copy_from_user(space, &c, address + i, 1))
            return Status::fault("invalid user string");
        if (c == 0)
            return Status::success();
        if (auto status = result.append(c); !status.ok())
            return status;
    }
    return Status::overflow("user string exceeds limit");
}
Status arguments_from_user(const hw::AddressSpace &space, uptr pointer, auto &out)
{
    if (pointer == 0)
        return Status::success();
    for (usize i = 0; i <= 16; ++i)
    {
        uptr value{};
        const uptr address = pointer + i * sizeof(uptr);
        if (address < pointer || !hw::copy_from_user(space, &value, address, sizeof(value)))
            return Status::fault("invalid argument vector");
        if (value == 0)
            return Status::success();
        if (i == 16)
            return Status::overflow("too many arguments");
        FixedString<256> text;
        if (auto status = string_from_user(space, value, text); !status.ok())
            return status;
        static_cast<void>(out.push_back(text));
    }
    return Status::overflow("argument vector exceeds limit");
}
Status install(sched::ProcessControlBlock &process, Image &image, sched::SignalMask mask = {})
{
    // Native bridges currently execute only on the boot CPU.
    bool previous_quarantined = false;
    for (const auto &saved : quarantined_spaces)
        previous_quarantined |= saved.space.root && same_space(saved.space, process.resources().native_space);
    if (!process.resources().native_pages.empty() ||
        (process.resources().native_space.root && !previous_quarantined))
        return Status::busy("previous native image must retire before installation");
    auto &resources = process.resources();
    auto &thread = process.threads()[0];
    if (auto status = cancel_native_vfs_thread(thread); !status) return status;
    const auto previous_thread = thread;
    const auto previous_memory = resources.memory;
    const auto previous_space = resources.native_space;
    const auto previous_personality = resources.personality;
    const auto previous_break_base = resources.program_break_base;
    const auto previous_break = resources.program_break;
    const auto previous_break_limit = resources.program_break_limit;
    const auto previous_affinity = process.cpu_affinity_mask();
    bool origin_prepared = false;
    auto fail_prepare = [&](Status status) {
        if (origin_prepared) syscall_origins.retire(thread);
        vfs_continuations.revoke(thread);
        thread = previous_thread;
        resources.memory = previous_memory;
        resources.native_space = previous_space;
        resources.personality = previous_personality;
        resources.program_break_base = previous_break_base;
        resources.program_break = previous_break;
        resources.program_break_limit = previous_break_limit;
        process.set_cpu_affinity_mask(previous_affinity);
        return status;
    };
    process.set_cpu_affinity_mask(1);
    if (image.personality == Personality::linux)
    {
        uptr first = ~uptr{0}, last = 0;
        for (const auto &page : image.pages)
        {
            if (!owned_frame(image.pages, page))
                runtime_failure = Status::fault("Linux image lost frame ownership");
            if (page.physical_address < first) first = page.physical_address;
            if (page.physical_address > last) last = page.physical_address;
        }
        emit("\nOK_NATIVE_IMAGE personality=linux source=");
        emit(page_pool.source() == NativePagePool::Source::qualified_firmware ? "firmware" : "linker");
        emit(" pages="); emit_number(image.pages.size());
        emit(" physical_first="); emit_hex(first);
        emit(" physical_last="); emit_hex(last); emit(" owner=retained\n");
    }
    resources.personality = image.personality;
    resources.native_space = image.space;
    resources.program_break_base = image.program_break;
    resources.program_break = image.program_break;
    resources.program_break_limit = hw::user_end - 0x100000 - 17 * hw::page_size;
    thread.tls = {};
    thread.native_signal_mask = mask;
    thread.native_frame = image.frame;
    thread.native_active = true;
    thread.syscall_pending = false;
    thread.native_deadline.cancel();
    thread.wait_registration.cancel();
    process.memory_map() = image.memory;
    if (auto status = syscall_origins.prepare(thread); !status) return fail_prepare(status);
    origin_prepared = true;
    if (auto status = vfs_continuations.prepare(thread); !status) return fail_prepare(status);
#ifdef OK_PRIVATE_PPC32_SIGNAL
    if (image.personality == Personality::linux && !private_ppc_initial(process, thread, image.signal_image))
        return fail_prepare(Status::fault("private PPC initial image admission failed"));
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
    if (!private_n64_initial(process, thread))
        return fail_prepare(Status::fault("private n64 initial signal context publication failed"));
#endif
#ifdef OK_PRIVATE_MIPS_O32_SIGNAL
    if (!private_mips_initial(thread))
        return fail_prepare(Status::fault("private MIPS initial signal context publication failed"));
#endif
    // Every fallible origin and signal preparation has completed while the
    // staged image still owns records borrowed by its backend. Transfer only
    // now; block addresses do not change when the directory moves.
    resources.native_pages = std::move(image.pages);
    image.space = {};
    image.retirement.cancel();
    return Status::success();
}
template <typename Install>
Status retire_then_install(sched::ProcessResources &resources, Image &image,
                           ImageRetirementReservation &previous, Install &&install_image)
{
    const auto retired = previous.retire(resources.native_space, resources.native_pages);
    if (!retired)
    {
        static_cast<void>(image.retire());
        runtime_failure = retired;
        return retired;
    }
    const auto installed = install_image();
    if (!installed)
    {
        static_cast<void>(image.retire());
        runtime_failure = installed;
    }
    return installed;
}
i64 error(Status status)
{
    return -syscall::ErrnoMapper::errno_for(status);
}
Status encode_result(const sched::ProcessControlBlock &process, hw::UserFrame &frame, i64 result)
{
    if (process.resources().personality == Personality::linux)
    {
        if (auto status = linux_bootstrap::complete<lu::configured_native_profile()>(frame, result); !status)
            return status;
    }
    else hw::set_return_value(frame, result);
    return Status::success();
}
Status set_result(const sched::ProcessControlBlock &process, hw::UserFrame &frame, i64 result)
{
    syscall_origins.revoke_frame(process, frame);
    if (auto status = encode_result(process, frame, result); !status) return status;
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
    return private_n64_completion(process, frame, result);
#elif defined(OK_PRIVATE_PPC32_SIGNAL)
    return private_ppc_completion(process, frame, result);
#else
    return Status::success();
#endif
}
[[maybe_unused]] void emit_pipe_vectors(const PipeVectors &vectors)
{
    if (!vectors.count) return;
    emit(" iovcnt="); emit_number(vectors.count); emit(" vectors=");
    for (usize i = 0; i < vectors.count; ++i)
    {
        if (i) emit(",");
        emit_number(vectors.entries[i].base); emit(":"); emit_number(vectors.entries[i].length);
    }
}
bool pipe_ranges(const hw::AddressSpace &space, uptr address, usize count, const PipeVectors &vectors, bool output)
{
    if (!vectors.count) return hw::user_range(space, address, count, output);
    if (!vectors.staged_count(sched::pipe_capacity + 1)) return false;
    for (usize i = 0; i < vectors.count; ++i)
        if (vectors.entries[i].length && !hw::user_range(space, vectors.entries[i].base, vectors.entries[i].length, output))
            return false;
    return true;
}
Status pipe_gather(const hw::AddressSpace &space, std::span<std::byte> bytes, uptr address, const PipeVectors &vectors)
{
    if (!vectors.count) return hw::copy_from_user(space, bytes.data(), address, bytes.size());
    usize offset = 0;
    for (usize i = 0; i < vectors.count && offset < bytes.size(); ++i)
    {
        const auto length = vectors.entries[i].length < bytes.size() - offset ? vectors.entries[i].length : bytes.size() - offset;
        if (length)
            if (auto status = hw::copy_from_user(space, bytes.data() + offset, vectors.entries[i].base, length); !status) return status;
        offset += length;
    }
    return offset == bytes.size() ? Status::success() : Status::fault("short copied pipe vector input");
}
Status pipe_scatter(const hw::AddressSpace &space, uptr address, std::span<const std::byte> bytes, const PipeVectors &vectors)
{
    if (!vectors.count) return hw::copy_to_user(space, address, bytes.data(), bytes.size());
    usize offset = 0;
    for (usize i = 0; i < vectors.count && offset < bytes.size(); ++i)
    {
        const auto length = vectors.entries[i].length < bytes.size() - offset ? vectors.entries[i].length : bytes.size() - offset;
        if (length)
            if (auto status = hw::copy_to_user(space, vectors.entries[i].base, bytes.data() + offset, length); !status) return status;
        offset += length;
    }
    return offset == bytes.size() ? Status::success() : Status::fault("short copied pipe vector output");
}
memory::ObjectHandle pipe_read_object(sched::ProcessId pid, sched::ThreadId tid)
{
    for (const auto &entry : pipe_read_waits)
        if (entry.pid == pid && entry.tid == tid) return entry.object;
    return {};
}
void emit_pipe_read(const PipeReadWait &wait, bool terminal, std::string_view state, i64 result = 0,
                    bool released = true)
{
#ifdef OK_ENABLE_TEST_POINTS
    emit(wait.vectors.count ? (terminal ? "\nOK_NATIVE_PIPE_READV_COMPLETE pid=" : "\nOK_NATIVE_PIPE_READV_WAIT pid=")
                            : (terminal ? "\nOK_NATIVE_PIPE_READ_COMPLETE pid=" : "\nOK_NATIVE_PIPE_READ_WAIT pid="));
    emit_number(wait.pid); emit(" tid="); emit_number(wait.tid);
    emit(" wait="); emit_number(wait.ticket.generation);
    emit(" request="); emit_number(wait.request);
    emit(wait.vectors.count ? " array=" : " output="); emit_number(wait.output);
    emit(" fd="); emit_number(static_cast<u32>(wait.fd));
    emit(" description="); emit_number(wait.description);
    emit(" pipe="); emit_number(wait.pipe); emit(" bytes="); emit_number(wait.count);
    emit_pipe_vectors(wait.vectors);
    if (terminal)
    {
        emit(" result=");
        if (result < 0) { emit("-"); emit_number(u64{0} - static_cast<u64>(result)); }
        else emit_number(static_cast<u64>(result));
        emit(" outcome="); emit(state); emit(released ? " pinned=0" : " pinned=1");
    }
    else { emit(" stage="); emit(state); }
    emit(" live="); emit_number(pipe_read_objects.accounting().live_objects); emit("\n");
#else
    static_cast<void>(wait); static_cast<void>(terminal); static_cast<void>(state);
    static_cast<void>(result); static_cast<void>(released);
#endif
}
void cancel_pipe_read(Kernel &kernel, memory::ObjectHandle object,
                      std::string_view outcome = "cancelled", i64 result = -125)
{
    auto resolved = pipe_read_objects.resolve(object);
    if (!resolved) { runtime_failure = resolved.status(); return; }
    const auto wait = *resolved.value();
    syscall_origins.retire_pipe(wait.pid, wait.tid, pipe_origin_binding(object, wait));
    for (auto &entry : pipe_read_waits)
        if (entry.object == object) entry = {};
    // An old object's teardown cannot complete or cancel a replacement wait.
    if (wait.ticket.generation)
        static_cast<void>(kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::cancelled));
    bool released = true;
    if (wait.description != sched::invalid_description)
        if (auto status = kernel.scheduler().release_description(wait.description); !status)
        { runtime_failure = status; released = false; }
    if (auto status = pipe_read_objects.destroy(object); !status) runtime_failure = status;
    if (auto status = pipe_read_objects.trim_empty_pages(); !status) runtime_failure = status;
    // Only published waits have a matching terminal record.
    if (wait.ticket.generation) emit_pipe_read(wait, true, outcome, result, released);
}
void cancel_pipe_read(Kernel &kernel, sched::ProcessId pid)
{
    for (const auto &entry : pipe_read_waits)
        if (entry.pid == pid) cancel_pipe_read(kernel, entry.object);
}
void prune_pipe_reads(Kernel &kernel)
{
    for (const auto &entry : pipe_read_waits)
    {
        if (!entry.pid) continue;
        const auto object = entry.object;
        auto resolved = pipe_read_objects.resolve(object);
        if (!resolved) { runtime_failure = resolved.status(); continue; }
        const auto &wait = *resolved.value();
        const auto *process = kernel.scheduler().find(wait.pid);
        const auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
        if (!process || !thread || !thread->native_active || !thread->syscall_pending ||
            process->state() == sched::ProcessState::zombie || process->state() == sched::ProcessState::exited ||
            thread->state == sched::ProcessState::exited || thread->wait_registration.ticket() != wait.ticket ||
            (thread->wait_registration.active() &&
             (thread->state != sched::ProcessState::blocked || thread->wait_reason != sched::ThreadWaitReason::pipe_read ||
              thread->wait_key != wait.pipe)))
            cancel_pipe_read(kernel, object);
    }
}
i64 resume_pipe_read(Kernel &kernel, sched::ProcessControlBlock &process,
                     memory::ObjectHandle object, bool &complete)
{
    auto resolved = pipe_read_objects.resolve(object);
    if (!resolved) { runtime_failure = resolved.status(); return error(resolved.status()); }
    auto &wait = *resolved.value();
    const auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
    if (!thread || thread->wait_registration.ticket() != wait.ticket || thread->wait_registration.active())
    { cancel_pipe_read(kernel, object); return -125; }
    if (thread->wait_registration.completion() != sched::WaitCompletion::ready)
    {
        // Recipient-side signal interruption retires the object before resume.
        // No restart/EINTR policy is synthesized from an unrelated completion.
        cancel_pipe_read(kernel, object);
        return -125;
    }
    const auto *description = kernel.scheduler().open_description(wait.description);
    if (!description || !description->readable || description->pipe != wait.pipe)
    { cancel_pipe_read(kernel, object, "error", -9); return -9; }
    auto &space = process.resources().native_space;
    // Mapping changes by a peer must be observed before consuming pipe bytes.
    if (!pipe_ranges(space, wait.output, wait.count, wait.vectors, true))
    { cancel_pipe_read(kernel, object, "efault", -14); return -14; }
    std::array<std::byte, sched::pipe_capacity> buffer{};
    auto read = kernel.scheduler().read_pipe(wait.pipe, {buffer.data(), wait.count}, true);
    if (!read && read.status().code() == StatusCode::would_block && !(description->flags & posix::o_NONBLOCK))
    {
        auto registration = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::pipe_read, wait.pipe);
        if (!registration)
        {
            const auto result = error(registration.status());
            cancel_pipe_read(kernel, object, "error", result);
            return result;
        }
        const auto previous_binding = pipe_origin_binding(object, wait);
        wait.ticket = registration.value();
        const auto origin = syscall_origins.identity(process, *thread);
        if (origin.capture &&
            !syscall_origins.rearm(origin, thread->native_frame, previous_binding, pipe_origin_binding(object, wait)))
            syscall_origins.revoke(*thread);
        emit_pipe_read(wait, false, "rearm");
        complete = false;
        return -11;
    }
    i64 result = read ? static_cast<i64>(read.value()) : error(read.status());
    if (read && !pipe_scatter(space, wait.output, {buffer.data(), read.value()}, wait.vectors)) result = -14;
    cancel_pipe_read(kernel, object, result > 0 ? "ready" : result == 0 ? "eof" : result == -14 ? "efault" : "error", result);
    return result;
}
i64 begin_pipe_read(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadId tid,
                    i32 fd, u16 description_id, uptr output, usize count, bool &complete, const PipeVectors &vectors = {})
{
    auto &space = process.resources().native_space;
    if (count > sched::pipe_capacity || (vectors.count && vectors.staged_count(sched::pipe_capacity) != count)) return -22;
    if (!pipe_ranges(space, output, count, vectors, true)) return -14;
    const auto *description = kernel.scheduler().open_description(description_id);
    if (!description || !description->readable) return -9;
    std::array<std::byte, sched::pipe_capacity> buffer{};
    auto read = kernel.scheduler().read_pipe(description->pipe, {buffer.data(), count}, true);
    if (read)
        return pipe_scatter(space, output, {buffer.data(), read.value()}, vectors) ? static_cast<i64>(read.value()) : -14;
    if (read.status().code() != StatusCode::would_block || (description->flags & posix::o_NONBLOCK))
        return error(read.status());
    PollWaitIndex *slot = nullptr;
    for (auto &entry : pipe_read_waits)
        if (!entry.pid) { slot = &entry; break; }
    if (!slot) return -12;
#ifndef OK_KERNEL_FREESTANDING
    if (auto status = activate_pool(); !status) return error(status);
#endif
    auto allocated = pipe_read_objects.create();
    if (!allocated) return error(allocated.status());
    const auto object = allocated.value();
    auto resolved = pipe_read_objects.resolve(object);
    if (!resolved) { runtime_failure = resolved.status(); return error(resolved.status()); }
    auto &wait = *resolved.value();
    if (auto status = kernel.scheduler().retain_description(description_id); !status)
    { cancel_pipe_read(kernel, object); return error(status); }
    wait.pid = process.pid(); wait.tid = tid; wait.output = output; wait.count = count; wait.vectors = vectors;
    wait.fd = fd; wait.description = description_id; wait.pipe = description->pipe;
    auto registration = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::pipe_read, wait.pipe);
    if (!registration)
    { cancel_pipe_read(kernel, object); return error(registration.status()); }
    wait.ticket = registration.value();
    wait.request = wait.ticket.generation;
    *slot = {wait.pid, wait.tid, object};
    if (auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid))
    {
        const auto origin = syscall_origins.identity(process, *thread);
        if (syscall_origins.live(origin) &&
            !syscall_origins.bind(origin, thread->native_frame, pipe_origin_binding(object, wait)))
            syscall_origins.revoke(*thread);
    }
    emit_pipe_read(wait, false, "arm");
#if defined(OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE) && defined(OK_ENABLE_TEST_POINTS)
    if (auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid))
    {
        const auto origin = syscall_origins.identity(process, *thread);
        const auto permit = syscall_origins.prepare_restart(origin, thread->native_frame, pipe_origin_binding(object, wait));
        if (permit)
        {
            emit(wait.vectors.count ? "OK_NATIVE_PIPE_READV_ORIGIN pid=" : "OK_NATIVE_PIPE_ORIGIN pid="); emit_number(wait.pid); emit(" tid="); emit_number(wait.tid);
            emit(" request="); emit_number(wait.request); emit(" wait="); emit_number(wait.ticket.generation);
            emit(" origin="); emit_number(origin.capture); emit(" pc="); emit_number(thread->native_frame.pc);
            emit(" syscall="); emit_number(*lu::Codec<lu::configured_native_profile()>::syscall_number(wait.vectors.count ? lu::Operation::readv : lu::Operation::read));
            emit(" fd="); emit_number(static_cast<u32>(wait.fd)); emit(wait.vectors.count ? " array=" : " output="); emit_number(wait.output);
            emit(" bytes="); emit_number(wait.count); emit_pipe_vectors(wait.vectors); emit("\n");
        }
    }
#endif
    complete = false;
    return -11;
}

// Signal policy uses the ordinary serialized native signal service.
void queue_pipe_sigpipe(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadId tid);

#include "native_pipe_write_runtime.hpp"
#include "native_pty_runtime.hpp"

#include "native_child_wait_runtime.hpp"

#include "native_udp_receive_runtime.hpp"

i64 wake_futex(Kernel &kernel, sched::ProcessId pid, uptr address, FutexKeyDomain domain, u32 limit);
struct RobustUserAccess
{
    static constexpr bool serialized_native_execution = true;
    Kernel &kernel;
    sched::ProcessControlBlock &process;
    static constexpr bool big_endian = lu::Codec<lu::configured_native_profile()>::profile.big_endian;

    Status read(u64 address, std::span<std::byte> bytes)
    {
        return hw::copy_from_user(process.resources().native_space, bytes.data(),
            static_cast<uptr>(address), bytes.size());
    }
    Result<u32> compare_exchange32(u64 address, u32 expected, u32 desired)
    {
        const auto pointer = static_cast<uptr>(address);
        auto &space = process.resources().native_space;
        if (pointer % sizeof(u32) || !hw::user_range(space, pointer, sizeof(u32), true))
            return Status::fault("robust owner word is not writable and aligned");
        std::array<std::byte, 4> bytes{};
        if (auto status = read(address, bytes); !status) return status;
        u32 actual{};
        for (usize i = 0; i < bytes.size(); ++i)
            actual |= static_cast<u32>(bytes[i]) << (8 * (big_endian ? 3 - i : i));
        if (actual != expected) return actual;
        for (usize i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<std::byte>(desired >> (8 * (big_endian ? 3 - i : i)));
        // Every native user is stopped at this CPU0 service boundary. Checked
        // copy is indivisible relative to those users, not an SMP atomic CAS.
        if (auto status = hw::copy_to_user(space, pointer, bytes.data(), bytes.size()); !status) return status;
        return actual;
    }
    void wake_one(u64 address)
    {
        const auto pointer = static_cast<uptr>(address);
        if (pointer % sizeof(u32) == 0 && hw::user_range(process.resources().native_space, pointer, sizeof(u32), true))
            static_cast<void>(wake_futex(kernel, process.pid(), pointer, FutexKeyDomain::anonymous_shared_operation, 1));
    }
};
static_assert(sched::max_task_id <= robust_tid_mask);
RobustWalkReport retire_robust(Kernel &kernel, sched::ProcessControlBlock &process,
                              sched::ThreadLocalState &tls, sched::ThreadId owner)
{
    if (process.resources().personality != Personality::linux) return {};
    const uptr head = tls.robust_list;
    tls.robust_list = 0;
    tls.robust_list_length = 0;
    RobustUserAccess access{kernel, process};
    return retire_robust_list<lu::configured_native_profile()>(head, static_cast<u32>(owner), access);
}
void clear_child_tid(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadLocalState &tls, bool peers)
{
    if (process.resources().personality != Personality::linux)
        return;
    const u32 zero = 0;
    const uptr address = tls.clear_tid_address;
    tls.clear_tid_address = 0;
    // Linux mm_release skips the store/wake for the last address-space user.
    // The store result is ignored. Shared anonymous key acquisition separately
    // requires a writable, aligned word, including when the store failed.
    if (peers && address)
    {
        static_cast<void>(hw::copy_to_user(process.resources().native_space, address, &zero, sizeof(zero)));
        if (address % sizeof(u32) == 0 && hw::user_range(process.resources().native_space, address, sizeof(u32), true))
            static_cast<void>(wake_futex(kernel, process.pid(), address, FutexKeyDomain::anonymous_shared_operation, 1));
    }
}
void clear_group_tids(Kernel &kernel, sched::ProcessControlBlock &process)
{
    usize live = 0;
    for (const auto &thread : process.threads()) live += thread.state != sched::ProcessState::exited;
    for (auto &thread : process.threads())
        if (thread.state != sched::ProcessState::exited)
        {
            static_cast<void>(retire_robust(kernel, process, thread.tls, thread.tid));
            clear_child_tid(kernel, process, thread.tls, live-- > 1);
        }
}
#include "native_vfs_retirement.hpp"
struct ExecRetirement
{
    struct Thread { sched::ThreadId tid{}; sched::ThreadLocalState tls{}; };
    StaticVector<Thread, sched::max_threads_per_process> threads;
    sched::ThreadId caller{};
    ExecRetirement(const sched::ProcessControlBlock &process, sched::ThreadId invoking) : caller(invoking)
    {
        for (const auto &thread : process.threads())
            if (thread.state != sched::ProcessState::exited)
                static_cast<void>(threads.push_back({thread.tid, thread.tls}));
    }
    void consume(Kernel &kernel, sched::ProcessControlBlock &process)
    {
        syscall_origins.revoke_process(process.pid());
        cancel_pipe_read(kernel, process.pid());
        cancel_pipe_write(kernel, process.pid());
        cancel_pty_waits(kernel, process.pid());
        cancel_child_wait(kernel, process.pid());
        // Scheduler collapse is committed, but the process still owns its old
        // VM. No user can observe this boundary until all retirement is done.
        for (auto &thread : threads)
            if (thread.tid != caller)
            {
                static_cast<void>(retire_robust(kernel, process, thread.tls, thread.tid));
                clear_child_tid(kernel, process, thread.tls, true);
            }
        for (auto &thread : threads)
            if (thread.tid == caller)
            {
                // Linux de_thread transfers the process TID before caller
                // futex_exec_release. The old TID must not be used here.
                static_cast<void>(retire_robust(kernel, process, thread.tls, process.pid()));
                clear_child_tid(kernel, process, thread.tls, false);
            }
        threads.clear();
    }
};
void finish(Kernel &kernel, sched::ProcessId pid, i32 code, u8 signal = 0);
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64) || defined(__mips__) || defined(__powerpc__) || defined(__PPC__))
bool private_linux_udp_signal_owned(Kernel &kernel, const sched::ProcessControlBlock &process,
                                    const sched::ThreadControlBlock &thread);
#include "signals/private_signal_delivery.hpp"
#include "signals/private_child_exit.hpp"
#endif
void queue_pipe_sigpipe(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadId tid)
{
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64) || defined(__mips__) || defined(__powerpc__) || defined(__PPC__))
    if (process.resources().personality != Personality::linux) return;
    auto *thread = kernel.scheduler().find_thread(process.pid(), tid);
    if (kernel.scheduler().find(process.pid()) != &process || !thread ||
        kernel.scheduler().current_thread() != thread || !thread->native_active || !thread->syscall_pending)
    {
        runtime_failure = Status::fault("pipe SIGPIPE lost calling thread ownership");
        started = false;
        return;
    }
    // Linux pipe_write uses send_sig(SIGPIPE, current, 0): SI_USER, group PID,
    // real UID. Existing standard pending work retains its original payload.
    const auto result = user::enqueue_directed_signal<private_signal_profile>(process, *thread,
        {13, 0, static_cast<u32>(process.pid()), process.credentials().uid},
        [&](sched::ProcessId pid) { return private_signal_table(kernel, pid); },
        [&](sched::ThreadControlBlock &owner) { return private_signal_queue(owner); });
    if (result != 0)
    {
        runtime_failure = Status::fault("pipe SIGPIPE could not retain pending signal");
        started = false;
    }
#else
    static_cast<void>(kernel); static_cast<void>(process); static_cast<void>(tid);
#endif
}
struct FutexQueueEntry
{
    memory::ObjectHandle object{};
    u64 sequence{};
    FutexWait::Key key{};
    sched::ProcessId pid{};
    sched::ThreadId tid{};
    SyscallOrigins::FutexBinding binding{};
    u64 origin{};
    SyscallOrigins::Identity origin_identity{};
};
struct FutexQueueSnapshot
{
    std::array<FutexQueueEntry, sched::max_processes> entries{};
    usize count{};
};
Status stop_futex_runtime(Status status)
{
    runtime_failure = status;
    started = false;
    return status;
}
bool pending_futex_wait(Kernel &kernel, const FutexWait &wait)
{
    const auto *process = kernel.scheduler().find(wait.pid);
    const auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
    return process && thread && thread->native_active && thread->syscall_pending &&
        process->state() != sched::ProcessState::zombie && process->state() != sched::ProcessState::exited &&
        thread->state == sched::ProcessState::blocked && thread->wait_reason == sched::ThreadWaitReason::futex &&
        thread->wait_key == wait.address && wait.ticket.owner == wait.pid && wait.ticket.thread == wait.tid &&
        thread->wait_registration.pending(wait.ticket);
}
Status validate_futex_origin(Kernel &kernel, memory::ObjectHandle object, const FutexWait &wait)
{
    if (!wait.origin) return Status::success();
    auto *process = kernel.scheduler().find(wait.pid);
    auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
    if (!process || !thread) return Status::fault("futex queue lost captured thread");
    const auto identity = syscall_origins.identity(*process, *thread);
    if (identity.capture != wait.origin ||
        !syscall_origins.validate_bound(identity, thread->native_frame, futex_origin_binding(object, wait)))
        return Status::fault("futex queue lost captured origin");
    return Status::success();
}
Result<FutexQueueSnapshot> snapshot_futex_queue(Kernel &kernel, const FutexWait::Key &key)
{
    if (!runtime_failure) return runtime_failure;
    FutexQueueSnapshot snapshot;
    for (const auto &entry : futex_waits)
    {
        if (!entry.pid) continue;
        auto resolved = futex_objects.resolve(entry.object);
        if (!resolved) return stop_futex_runtime(resolved.status());
        const auto &wait = *resolved.value();
        if (wait.pid != entry.pid || wait.tid != entry.tid)
            return stop_futex_runtime(Status::fault("futex queue index lost exact owner"));
        if (!same_futex_key(wait.key, key) || !pending_futex_wait(kernel, wait)) continue;
        if (!wait.queue_sequence || wait.pending_transfer_pin_owned || wait.pin_owned != wait.key.shared)
            return stop_futex_runtime(Status::fault("futex queue has invalid sequence or pending transfer"));
        if (auto status = validate_futex_origin(kernel, entry.object, wait); !status)
            return stop_futex_runtime(status);
        if (snapshot.count == snapshot.entries.size())
            return stop_futex_runtime(Status::overflow("futex queue snapshot exceeds capacity"));
        usize position = snapshot.count++;
        while (position && snapshot.entries[position - 1].sequence > wait.queue_sequence)
        {
            snapshot.entries[position] = snapshot.entries[position - 1];
            --position;
        }
        if ((position && snapshot.entries[position - 1].sequence == wait.queue_sequence) ||
            (position + 1 < snapshot.count && snapshot.entries[position + 1].sequence == wait.queue_sequence))
            return stop_futex_runtime(Status::fault("futex queue sequence was reused"));
        SyscallOrigins::Identity origin_identity{};
        if (wait.origin)
        {
            const auto *process = kernel.scheduler().find(wait.pid);
            const auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
            if (!process || !thread)
                return stop_futex_runtime(Status::fault("futex snapshot lost captured owner"));
            origin_identity = syscall_origins.identity(*process, *thread);
        }
        snapshot.entries[position] = {entry.object, wait.queue_sequence, wait.key, wait.pid, wait.tid,
            futex_origin_binding(entry.object, wait), wait.origin, origin_identity};
    }
    return snapshot;
}
Result<FutexWait *> selected_futex_wait(Kernel &kernel, const FutexQueueEntry &entry, const FutexWait::Key &key)
{
    auto resolved = futex_objects.resolve(entry.object);
    if (!resolved) return stop_futex_runtime(resolved.status());
    auto &wait = *resolved.value();
    if (wait.queue_sequence != entry.sequence || !same_futex_key(wait.key, key) ||
        !exact_futex_key(wait.key, entry.key) || wait.pid != entry.pid || wait.tid != entry.tid ||
        futex_origin_binding(entry.object, wait) != entry.binding || wait.origin != entry.origin ||
        !pending_futex_wait(kernel, wait))
        return stop_futex_runtime(Status::fault("selected futex lost exact pending continuation"));
    if (entry.origin)
    {
        const auto *process = kernel.scheduler().find(entry.pid);
        const auto *thread = kernel.scheduler().find_thread(entry.pid, entry.tid);
        if (!process || !thread || syscall_origins.identity(*process, *thread) != entry.origin_identity ||
            !syscall_origins.validate_bound(entry.origin_identity, thread->native_frame, entry.binding))
            return stop_futex_runtime(Status::fault("selected futex lost original captured origin"));
    }
    return &wait;
}
bool finish_futex(Kernel &kernel, memory::ObjectHandle object, sched::WaitCompletion cause, i64 result)
{
    if (!runtime_failure) return false;
    auto resolved = futex_objects.resolve(object);
    if (!resolved)
    {
        runtime_failure = resolved.status();
        return false;
    }
    const auto wait = *resolved.value();
    auto *process = kernel.scheduler().find(wait.pid);
    auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
    const bool completed = process && thread && thread->native_active &&
                           thread->wait_reason == sched::ThreadWaitReason::futex && thread->wait_key == wait.address &&
                           kernel.scheduler().complete_wait(wait.ticket, cause);
    if (completed)
    {
        thread->syscall_pending = false;
        if (auto status = set_result(*process, thread->native_frame, result); !status)
        {
            runtime_failure = status;
            finish(kernel, wait.pid, 127);
            return false;
        }
    }
    if (auto status = cancel_futex(kernel, object, cause == sched::WaitCompletion::ready ? "ready" :
        cause == sched::WaitCompletion::timeout ? "timeout" : "cancelled", result); !status) return false;
    return completed;
}
i64 wake_futex(Kernel &kernel, sched::ProcessId pid, uptr address, FutexKeyDomain domain, u32 limit)
{
    if (!runtime_failure) return -5;
    auto *caller = kernel.scheduler().find(pid);
    if (!caller) return -22;
    auto key = resolve_futex_key(*caller, address, domain);
    if (!key) return error(key.status());
    auto snapshot = snapshot_futex_queue(kernel, key.value());
    if (!snapshot) return -5;
    usize count = 0;
    for (usize index = 0; index < snapshot.value().count && count < limit; ++index)
    {
        const auto &entry = snapshot.value().entries[index];
        if (!selected_futex_wait(kernel, entry, key.value())) return -5;
        if (!finish_futex(kernel, entry.object, sched::WaitCompletion::ready, 0))
        {
            if (runtime_failure) stop_futex_runtime(Status::fault("selected futex wake failed"));
            return -5;
        }
        if (!runtime_failure) return -5;
        ++count;
    }
    emit_shared_futex_wake(key.value(), count);
    return static_cast<i64>(count);
}
bool expire_futex(Kernel &kernel, u64 now)
{
    if (!runtime_failure) return false;
    bool pending = false;
    for (const auto &entry : futex_waits)
    {
        if (!entry.pid)
            continue;
        const auto object = entry.object;
        auto resolved = futex_objects.resolve(object);
        if (!resolved)
        {
            runtime_failure = resolved.status();
            started = false;
            return false;
        }
        auto &wait = *resolved.value();
        auto *process = kernel.scheduler().find(wait.pid);
        auto *thread = kernel.scheduler().find_thread(wait.pid, wait.tid);
        // External scheduler cancellation owns the same completed ticket, but
        // has not encoded the saved syscall result. Never replay an old WAIT.
        // A replacement ticket belongs to another continuation and is untouched.
        if (process && thread && thread->native_active &&
            process->state() != sched::ProcessState::zombie && process->state() != sched::ProcessState::exited &&
            !thread->wait_registration.active() && thread->wait_registration.ticket() == wait.ticket &&
            (thread->state == sched::ProcessState::runnable || thread->state == sched::ProcessState::running))
        {
            const auto cause = thread->wait_registration.completion();
            const i64 result = cause == sched::WaitCompletion::ready ? 0 :
                               cause == sched::WaitCompletion::timeout ? -110 : -4;
            thread->syscall_pending = false;
            if (auto status = set_result(*process, thread->native_frame, result); !status)
            {
                runtime_failure = status;
                finish(kernel, wait.pid, 127);
            }
            else cancel_futex(kernel, object, cause == sched::WaitCompletion::ready ? "ready" :
                cause == sched::WaitCompletion::timeout ? "timeout" : "cancelled", result);
            if (!runtime_failure) return false;
            continue;
        }
        if (!process || !thread || !thread->native_active || process->state() == sched::ProcessState::zombie ||
            process->state() == sched::ProcessState::exited || !thread->wait_registration.pending(wait.ticket) ||
            thread->wait_reason != sched::ThreadWaitReason::futex || thread->wait_key != wait.address)
        {
            if (!cancel_futex(kernel, object)) return false;
            continue;
        }
        if (wait.timeout.complete(now, wait.timeout.generation()))
            static_cast<void>(finish_futex(kernel, object, sched::WaitCompletion::timeout, -110));
        else
            pending = true;
        if (!runtime_failure) return false;
    }
    return pending;
}

void finish(Kernel &kernel, sched::ProcessId pid, i32 code, u8 signal)
{
    const auto *initial = kernel.scheduler().find(pid);
    if (!initial) return;
    if (initial->state() == sched::ProcessState::zombie || initial->state() == sched::ProcessState::exited)
    { runtime_failure = Status::already_exists("native group already completed"); started = false; return; }
#ifdef OK_PRIVATE_POLL_MASK
    PrivateChildExit::Completion completion{};
    PrivateChildExit::Adoption adoption{};
    const auto cause = signal ? sched::ChildExitStatus::signaled(signal) : sched::ChildExitStatus::normal(code);
    if (auto status = PrivateChildExit::prepare(kernel.scheduler(), pid, cause, private_signal_find,
        [](const sched::ThreadControlBlock &thread) { return signal_waits.real_blocked(thread); }, completion); !status)
    { runtime_failure = status; started = false; return; }
    if (auto status = PrivateChildExit::prepare_adoption(kernel.scheduler(), pid, private_signal_find, adoption); !status)
    { runtime_failure = status; started = false; return; }
#endif
    auto cleanup = [&]() -> Status {
        cancel_poll(kernel, pid);
        if (auto status = cancel_futex(kernel, pid); !status) return status;
        cancel_pipe_read(kernel, pid);
        cancel_pipe_write(kernel, pid);
        cancel_pty_waits(kernel, pid);
        cancel_child_wait(kernel, pid);
        retire_native_udp_receives(kernel, pid);
        const auto linux_udp_retired = retire_linux_udp_receives(kernel, pid, {}, LinuxUdpReceiveRetirement::process_exit);
        if (!runtime_failure) return runtime_failure;
        if (!linux_udp_retired) return linux_udp_retired;
        auto *process = kernel.scheduler().find(pid);
        if (!process) return Status::fault("native completion lost process");
        if (auto status = retire_group_stacks(*process); !status) return status;
        if (!runtime_failure) return runtime_failure;
#ifdef OK_PRIVATE_POLL_MASK
        private_signal_retire(pid);
#endif
        clear_group_tids(kernel, *process);
        if (!runtime_failure) return runtime_failure;
        release(process->resources().native_space, process->resources().native_pages);
        if (!runtime_failure) return runtime_failure;
        emit_pool_stats();
        emit_entry_stack_stats();
        for (auto &thread : process->threads())
        {
            thread.native_deadline.cancel();
            thread.native_active = false;
        }
        return Status::success();
    };
    auto transition = [&]() {
        return signal ? kernel.scheduler().terminate_process(pid, signal) : kernel.posix().exit(pid, code);
    };
#ifdef OK_PRIVATE_POLL_MASK
    const auto status = child_exit_transaction(cleanup, transition,
        [&] {
            if (auto status = private_child_exit_commit(kernel, completion); !status) return status;
            return PrivateChildExit::commit_adoption(kernel.scheduler(), adoption, private_signal_find);
        });
#else
    auto status = cleanup();
    if (status) status = transition();
#endif
    if (!status) { runtime_failure = status; started = false; }
}
#include "linux_udp_runtime.hpp"
#include "tcp_immediate_runtime.hpp"
#include "linux_udp_receive_runtime.hpp"
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64) || defined(__mips__) || defined(__powerpc__) || defined(__PPC__))
#include "signals/private_linux_udp_signal.hpp"
#endif

// Only this boundary accepts untrusted user pointers. The existing syscall
// table receives kernel-owned buffers; calls without a complete marshaller are
// deliberately unavailable to native programs.
i64 dispatch(Kernel &kernel, sched::ProcessId pid, hw::Syscall call, bool &complete)
{
    auto *process = kernel.scheduler().find(pid);
    auto *caller = kernel.scheduler().current_thread();
    if (!process || !caller || caller->owner != pid || caller->state != sched::ProcessState::running)
        return -22;
    const auto calling_tid = caller->tid;
    if (const auto object = pty_wait_object(pid, calling_tid); object.pool)
        return resume_pty_io(kernel, *process, object, complete);
    if (const auto object = linux_udp_receive_object(pid, calling_tid); object.pool)
    {
        complete = false;
        if (auto status = resume_linux_udp_receive(kernel, *process, *caller, object); !status)
            static_cast<void>(linux_udp_receive_invariant(status));
        return 0; // The captured owner alone publishes or remains pending.
    }
    if (const auto object = native_udp_receive_object(pid, calling_tid); object.pool)
        return resume_native_udp_receive(kernel, *process, object, complete);
    if (const auto object = child_wait_object(pid, calling_tid); object.pool)
        return resume_child_wait(kernel, *process, object, complete);
    if (const auto object = pipe_read_object(pid, calling_tid); object.pool)
    {
        auto saved = pipe_read_objects.resolve(object);
        if (!saved) { runtime_failure = saved.status(); return error(saved.status()); }
        if (caller->wait_registration.ticket() == saved.value()->ticket)
            return resume_pipe_read(kernel, *process, object, complete);
        cancel_pipe_read(kernel, object);
    }
    if (const auto object = pipe_write_object(pid, calling_tid); object.pool)
    {
        auto saved = pipe_write_objects.resolve(object);
        if (!saved) { runtime_failure = saved.status(); return error(saved.status()); }
        if (caller->wait_registration.ticket() == saved.value()->ticket)
            return resume_pipe_write(kernel, *process, object, complete);
        cancel_pipe_write(kernel, object);
    }
    // Blocking may clear the current CPU slot; retain identity rather than
    // consulting the first TCB or the post-block current-thread accessor.
    auto blocked_caller = [&] {
        const auto *thread = kernel.scheduler().find_thread(pid, calling_tid);
        return thread && thread->state == sched::ProcessState::blocked;
    };
    auto &space = process->resources().native_space;
    using N = syscall::Number;
    const auto number = static_cast<N>(call.number);
    const auto &a = call.arguments;
    if (number == N::pty_pair)
    {
        if (process->resources().personality != Personality::native) return -38;
        auto result = native_pty::create<std::endian::native == std::endian::big>(a,
            [&](uptr output, usize size) { return hw::user_range(space, output, size, true); },
            [&](u32 flags) { return kernel.scheduler().prepare_pty_pair(pid, flags); },
            [&](uptr output, std::span<const u8> bytes) {
                return hw::copy_to_user(space, output, bytes.data(), bytes.size());
            },
            [](sched::PreparedPtyPair &operation) noexcept { operation.publish_checked(); },
            [&](Status failure) { if (runtime_failure) runtime_failure = failure; });
        return result ? static_cast<i64>(result.value()) : error(result.status());
    }
    if (number == N::ioctl)
        return console_control::dispatch<std::endian::native == std::endian::big>(
            static_cast<i32>(a[0]), console_control::native_operation(a[1]), a[2],
            [&](i32 fd) { return kernel.posix().console_control_descriptor(fd); },
            [&](i32 fd, sched::ConsoleControlOperation operation, i32 value) {
                return kernel.posix().console_control(fd, operation, value);
            },
            [&](u64 address, std::span<u8> bytes) {
                return hw::copy_from_user(space, bytes.data(), static_cast<uptr>(address), bytes.size());
            },
            [&](u64 address, usize bytes) { return hw::user_range(space, static_cast<uptr>(address), bytes, true); },
            [&](u64 address, std::span<const u8> bytes) {
                return hw::copy_to_user(space, static_cast<uptr>(address), bytes.data(), bytes.size());
            });
    if (caller->console_wait_epoch && !kernel.scheduler().console_authorized(pid, caller->console_wait_epoch))
    {
        caller->console_wait_epoch = 0;
        return -13;
    }
    if (const auto operation = native_metadata::select(number))
        return native_metadata::dispatch(process->resources().personality == Personality::native, *operation, a,
            [&](uptr address, FixedString<256> &path) {
                return native_metadata::copy_path(address, path, [&](uptr byte, char &value) {
                    return hw::copy_from_user(space, &value, byte, 1);
                });
            },
            [&](native_metadata::Request request, std::string_view path) -> fs::ModeChangeResult {
                if (request.ownership)
                    return request.direct_fd ? kernel.posix().change_owner_fd(request.fd, request.uid, request.gid)
                        : kernel.posix().change_owner_at(request.fd, path, request.uid, request.gid, request.flags);
                return request.direct_fd ? kernel.posix().change_mode_fd(request.fd, request.mode)
                    : kernel.posix().change_mode_at(request.fd, path, request.mode, request.flags);
            });
    if (number == N::mount_info)
        return native_mount_info::dispatch(process->resources().personality == Personality::native, a,
            [&](u32 slot, fs::MountSnapshot &snapshot) { return kernel.vfs().mount_snapshot(slot, snapshot); },
            [&](uptr address, usize bytes) { return hw::user_range(space, address, bytes, true); },
            [&](uptr address, std::span<const u8> bytes) {
                return hw::copy_to_user(space, address, bytes.data(), bytes.size());
            });
    if (number == N::power_prepare || number == N::power_commit)
    {
        if (process->resources().personality != Personality::native) return -38;
        const auto power_error = [](Status status) -> i64 {
            if (status.code() == StatusCode::denied || status.code() == StatusCode::permission) return -1;
            return status.code() == StatusCode::unsupported ? -95 : error(status);
        };
        auto owner = shutdown_caller(kernel);
        if (!owner) return power_error(owner.status());
        for (usize index = 1; index < a.size(); ++index)
            if (a[index]) return -22;
        // Check the complete native word before narrowing on 64-bit profiles.
        if (!a[0] || a[0] > OK_POWER_TOKEN_MAX) return -22;
        if (number == N::power_prepare)
        {
            auto issued = prepare_native_power(kernel, static_cast<u32>(a[0]));
            return issued ? static_cast<i64>(issued.value()) : power_error(issued.status());
        }
        const auto status = commit_native_power(kernel, static_cast<u32>(a[0]));
        return status ? 0 : power_error(status);
    }
    if (number == N::sync_persistent)
    {
        if (process->resources().personality != Personality::native) return -38;
        for (usize index = 1; index < a.size(); ++index)
            if (a[index]) return -22;
        if (a[0] > 0x7fffffffU) return -9;
        const auto status = kernel.posix().sync_persistent(static_cast<posix::Fd>(a[0]));
        return status ? 0 : status.code() == StatusCode::unsupported ? -95 : error(status);
    }
    if (number == N::console_claim || number == N::console_revoke || number == N::console_flush)
    {
        if (process->resources().personality != Personality::native) return -38;
        for (usize index = 1; index < a.size(); ++index)
            if (a[index]) return -22;
        if (!a[0] || a[0] > sched::native_console_epoch_limit) return -22;
        if (number == N::console_claim)
        {
            auto issued = kernel.scheduler().claim_console(a[0]);
            if (!issued) return error(issued.status());
            if (auto status = drain_console_ingress(); !status)
            { kernel.scheduler().abort_console_input(); return error(status); }
            return issued.value();
        }
        const auto epoch = static_cast<u32>(a[0]);
        auto status = number == N::console_revoke ? kernel.scheduler().revoke_console(epoch)
                                                 : kernel.scheduler().flush_console(epoch);
        if (!status) return error(status);
        if (auto drained = drain_console_ingress(); !drained)
        { kernel.scheduler().abort_console_input(); return error(drained); }
        return 0;
    }
    if (const auto credential_call = native_credentials::select(number))
    {
        if (process->resources().personality != Personality::native) return -38;
        return native_credentials::dispatch(*process, *credential_call, a,
            [&](uptr address, std::span<const u8> bytes) {
                return hw::copy_to_user(space, address, bytes.data(), bytes.size());
            });
    }
    const auto unix_operation = native_unix_path_call(call, kernel.scheduler(), *process);
    if (process->resources().personality == Personality::native && unix_operation != lu::UnixPathCall::none)
        return unix_pathname<lu::configured_native_profile(), true>(kernel.posix(), kernel.scheduler(), *process,
            unix_operation, {a[0], a[1], a[2], a[3], a[4], a[5]},
            [](std::array<u64, 6> arguments) -> Result<std::array<u64, 6>> { return arguments; },
            [&](u64 address, std::span<u8> bytes) { return hw::copy_from_user(space, bytes.data(), address, bytes.size()); },
            [&](u64 address, std::span<const u8> bytes) { return hw::copy_to_user(space, address, bytes.data(), bytes.size()); },
            [&](u64 address, usize length) { return hw::user_range(space, address, length, true); });
    if (number == N::sendmsg || number == N::recvmsg)
    {
        if (process->resources().personality != Personality::native) return -38;
        using C = lu::UnixMessageCodec<lu::configured_native_profile(), true>;
        static_assert(sizeof(ok_msghdr) == C::header_bytes && sizeof(ok_cmsghdr) == C::cmsg_header_bytes);
        static_assert(offsetof(ok_msghdr, vectors) == C::vectors_offset &&
            offsetof(ok_msghdr, flags) == C::flags_offset);
        return unix_message<lu::configured_native_profile(), true>(kernel.posix(), number == N::recvmsg,
            a[0], a[1], a[2],
            [&](u64 address, std::span<u8> bytes) { return hw::copy_from_user(space, bytes.data(), address, bytes.size()); },
            [&](u64 address, std::span<const u8> bytes) { return hw::copy_to_user(space, address, bytes.data(), bytes.size()); },
            [&](u64 address, usize length) { return hw::user_range(space, address, length, true); });
    }
    if (number == N::socketpair)
    {
        if (process->resources().personality != Personality::native) return -38;
        constexpr u64 allowed = OK_SOCK_STREAM | OK_SOCK_NONBLOCK | OK_SOCK_CLOEXEC;
        if (a[0] != OK_AF_UNIX || (a[1] & ~allowed) || (a[1] & 0xfu) != OK_SOCK_STREAM || a[2])
            return -22;
        auto fds = kernel.posix().unix_socketpair(static_cast<u32>(a[1]) &
                                                  (posix::o_NONBLOCK | posix::o_CLOEXEC));
        if (!fds) return error(fds.status());
        if (!hw::copy_to_user(space, a[3], fds.value().data(), sizeof(i32) * 2))
        {
            static_cast<void>(kernel.posix().close(fds.value()[0]));
            static_cast<void>(kernel.posix().close(fds.value()[1]));
            return -14;
        }
        return 0;
    }
    if (number == N::socket || number == N::bind || number == N::sendto || number == N::recvfrom)
    {
        // Native UAPI numbers and sockaddr layout are not Linux ABI values on
        // the ten supported targets. A Linux decoder must be added separately.
        if (process->resources().personality != Personality::native) return -38;
        static_assert(sizeof(ok_sockaddr_in) == 16);
        auto socket_description = [&](u64 descriptor) -> Result<u16> {
            if (descriptor > static_cast<u64>(__INT32_MAX__))
                return Status::bad_descriptor("native UDP descriptor is out of range");
            const auto *slot = process->fd_table().entry(static_cast<i32>(descriptor));
            const auto *open = slot && slot->used ? kernel.scheduler().open_description(slot->description) : nullptr;
            if (!open || !open->native_udp_handle)
                return Status::bad_descriptor("descriptor is not a native UDP socket");
            return slot->description;
        };
        auto address_from_user = [&](u64 pointer, u64 length) -> Result<ok_sockaddr_in> {
            if (length != sizeof(ok_sockaddr_in))
                return Status::invalid_argument("native UDP address must be sixteen bytes");
            ok_sockaddr_in address{};
            if (!hw::copy_from_user(space, &address, pointer, sizeof(address)))
                return Status::fault("native UDP address is unreadable");
            if (address.family != OK_AF_INET)
                return Status::invalid_argument("native UDP requires AF_INET");
            for (auto byte : address.reserved)
                if (byte) return Status::invalid_argument("native UDP address has nonzero reserved bytes");
            return address;
        };
        if (number == N::socket)
        {
            constexpr u64 allowed = OK_SOCK_DGRAM | OK_SOCK_NONBLOCK | OK_SOCK_CLOEXEC;
            if (a[0] != OK_AF_INET || (a[1] & ~allowed) ||
                (a[1] & OK_SOCK_DGRAM) != OK_SOCK_DGRAM ||
                (a[2] != 0 && a[2] != OK_IPPROTO_UDP)) return -22;
            if (auto status = kernel.native_udp_executor().status(); !status) return error(status);
            if (!kernel.native_udp().configured())
            {
                auto *extension = kernel.extension();
                if (!extension) return -38;
                auto link = extension->network_link(kernel);
                if (!link) return error(link.status());
                if (auto status = kernel.native_udp().configure(link.value()); !status)
                    return error(status);
            }
            auto opened = kernel.native_udp().open();
            if (!opened) return error(opened.status());
            auto fd = kernel.posix().install_native_udp(opened.value(), (a[1] & OK_SOCK_CLOEXEC) != 0, (a[1] & OK_SOCK_NONBLOCK) != 0);
            if (!fd)
            {
                static_cast<void>(kernel.native_udp().close(opened.value()));
                return error(fd.status());
            }
            return fd.value();
        }
        const auto description_id = socket_description(a[0]);
        if (!description_id) return error(description_id.status());
        const Result<u32> handle = kernel.scheduler().open_description(description_id.value())->native_udp_handle;
        if (number == N::bind)
        {
            auto address = address_from_user(a[1], a[2]);
            if (!address) return error(address.status());
            const auto port = static_cast<u16>((u16{address.value().port_be[0]} << 8) |
                                               address.value().port_be[1]);
            const net::wire::Ipv4 local{address.value().address[0], address.value().address[1],
                                        address.value().address[2], address.value().address[3]};
            return error(kernel.native_udp().bind(handle.value(), local, port));
        }
        if (number == N::sendto)
        {
            auto *extension = kernel.extension();
            if (!extension) return -38;
            if (a[3]) return -22;
            if (auto status = kernel.native_udp_executor().status(); !status) return error(status);
            if (a[2] > net::NativeUdpService::datagram_capacity) return -90;
            auto address = address_from_user(a[4], a[5]);
            if (!address) return error(address.status());
            std::array<std::byte, net::NativeUdpService::datagram_capacity> payload{};
            if (!hw::copy_from_user(space, payload.data(), a[1], static_cast<usize>(a[2]))) return -14;
            std::array<std::byte, net::NativeUdpService::frame_capacity> frame{};
            const auto port = static_cast<u16>((u16{address.value().port_be[0]} << 8) |
                                               address.value().port_be[1]);
            const net::wire::Ipv4 destination{address.value().address[0], address.value().address[1],
                                               address.value().address[2], address.value().address[3]};
            auto length = kernel.native_udp().build_datagram(handle.value(), destination,
                port, {payload.data(), static_cast<usize>(a[2])}, frame);
            if (!length) return error(length.status());
            if (auto status = extension->network_transmit(kernel, {frame.data(), length.value()}); !status)
                return error(status);
            return static_cast<i64>(a[2]);
        }
        if (a[3]) return -22; // MSG_TRUNC and all other receive flags are unsupported.
        if (!!a[4] != !!a[5]) return -22;
        u32 address_capacity{};
        if (a[4])
        {
            if (!hw::copy_from_user(space, &address_capacity, a[5], sizeof(address_capacity))) return -14;
            if (address_capacity < sizeof(ok_sockaddr_in)) return -22;
            if (!hw::user_range(space, a[4], sizeof(ok_sockaddr_in), true) ||
                !hw::user_range(space, a[5], sizeof(address_capacity), true)) return -14;
        }
        const SyscallOrigins::NativeUdpReceiveRequest request{
            static_cast<i32>(a[0]), a[1], a[2], a[3], a[4], a[5], address_capacity};
        return begin_native_udp_receive(kernel, *process, *caller, description_id.value(), request, complete);
    }
    if (number == N::poll)
    {
        if (a[1] > sched::max_process_fds)
            return -22;
        if (!hw::user_range(space, a[0], a[1] * sizeof(ok_pollfd), true))
            return -14;
        std::array<ok_pollfd, sched::max_process_fds> descriptors{};
        if (!hw::copy_from_user(space, descriptors.data(), a[0], a[1] * sizeof(ok_pollfd)))
            return -14;
        const i32 milliseconds = static_cast<i32>(a[2]);
        PollRequest request{};
        request.output = a[0];
        request.descriptors = {descriptors.data(), a[1]};
        request.immediate = milliseconds == 0;
        request.timed = milliseconds > 0;
        request.timeout = {milliseconds / 1000, (milliseconds % 1000) * 1000000};
        return begin_poll(kernel, pid, request, complete);
    }
    if (number == N::clock_settime)
    {
        if (a[0] != 0)
            return -22;
        ok_timespec requested{};
        if (!hw::copy_from_user(space, &requested, a[1], sizeof(requested)))
            return -14;
        return error(user::set_realtime_checked(
            {requested.seconds, requested.nanoseconds}, process->credentials(),
            [] { return arch::monotonic_sample(); },
            [](time::EpochTime value, time::MonotonicPoint point) { return arch::set_realtime(value, point); }));
    }
    if (number == N::clock_gettime || number == N::clock_getres)
    {
        if (a[0] > 1)
            return -22;
        if (number == N::clock_getres && a[1] == 0)
            return 0;
        auto now = time::public_clock(static_cast<u32>(a[0]), number == N::clock_getres);
        if (!now)
            return error(now.status());
        const posix::ClockTime result{now.value().seconds, now.value().nanoseconds};
        return hw::copy_to_user(space, a[1], &result, sizeof(result)) ? 0 : -14;
    }
    if (number == N::time || number == N::gettimeofday)
    {
        if (number == N::gettimeofday && a[0] == 0)
            return 0;
        auto now = time::public_clock(0);
        if (!now)
            return error(now.status());
        if (number == N::time)
        {
            auto seconds = time::native_time_seconds<sizeof(uptr)>(now.value());
            if (!seconds)
                return error(seconds.status());
            const i64 value = seconds.value();
            if (a[0] && !hw::copy_to_user(space, a[0], &value, sizeof(value)))
                return -14;
            return value;
        }
        const ok_timeval result{now.value().seconds, now.value().nanoseconds / 1000};
        return hw::copy_to_user(space, a[0], &result, sizeof(result)) ? 0 : -14;
    }
    if (number == N::nanosleep || number == N::clock_nanosleep)
    {
        const bool clock_sleep = number == N::clock_nanosleep;
        if (clock_sleep && (a[0] != 1 || (a[1] & ~uptr{1}) != 0))
            return -22;
        posix::ClockTime request{};
        if (!hw::copy_from_user(space, &request, a[clock_sleep ? 2 : 0], sizeof(request)))
            return -14;
        const bool absolute = clock_sleep && a[1] == 1;
        auto now = arch::monotonic_nanoseconds();
        if (!now)
            return error(now.status());
        return error(begin_sleep(kernel, pid, now.value(), request, absolute, complete));
    }
    if (number == N::brk)
    {
        auto adjusted = native_brk(process->resources(), a[0]);
        return adjusted ? static_cast<i64>(adjusted.value()) : error(adjusted.status());
    }
    if (number == N::memfd_create)
    {
        FixedString<256> name;
        if (auto status = string_from_user(space, a[0], name); !status) return error(status);
        auto created = kernel.posix().memfd_create(name.view(), static_cast<u32>(a[1]));
        return created ? static_cast<i64>(created.value()) : error(created.status());
    }
    if (number == N::mmap)
    {
        fs::Node *source = nullptr;
        usize file_size = 0;
        if ((a[3] & posix::map_ANONYMOUS) == 0)
        {
            const auto fd = static_cast<__INTPTR_TYPE__>(a[4]);
            if (fd < 0 || fd >= static_cast<__INTPTR_TYPE__>(sched::max_process_fds))
                return -9;
            const auto *slot = process->resources().files.entry(static_cast<i32>(fd));
            const auto *description = slot && slot->used ? kernel.scheduler().open_description(slot->description) : nullptr;
            if (!description || !description->used || (!description->node && !description->native_shared_file))
                return -9;
            if (!description->readable)
                return -13;
            if (description->native_shared_file)
            {
                if ((a[2] & posix::prot_WRITE) && !description->writable) return -13;
                const auto handle = description->native_shared_file;
                auto metadata = shared_files.info(handle);
                if (!metadata) return error(metadata.status());
                const auto id = slot->description;
                if (auto status = kernel.scheduler().retain_description(id); !status) return error(status);
                auto mapped = native_mmap(process->resources(), a[0], a[1], a[2], a[3], a[4], a[5],
                    nullptr, metadata.value().size, handle);
                if (auto status = kernel.scheduler().release_description(id); !status)
                { runtime_failure = status; return error(status); }
                return mapped ? static_cast<i64>(mapped.value()) : error(mapped.status());
            }
            if (a[3] & posix::map_SHARED)
                return error(Status::unsupported("shared VFS mappings require a coherent page cache"));
            auto metadata = description->node->query_metadata();
            if (!metadata) return error(metadata.status());
            if (metadata.value().type != fs::NodeType::regular)
                return -19;
            source = description->node;
            file_size = metadata.value().size;
        }
        auto mapped = native_mmap(process->resources(), a[0], a[1], a[2], a[3], a[4], a[5], source, file_size);
        return mapped ? static_cast<i64>(mapped.value()) : error(mapped.status());
    }
    if (number == N::mprotect)
        return error(native_mprotect(process->resources(), a[0], a[1], a[2]));
    if (number == N::munmap)
        return error(native_munmap(process->resources(), a[0], a[1]));
    if (number == N::exit || number == N::exit_group)
    {
        if (number == N::exit && process->resources().personality == Personality::linux)
        {
            retire_native_udp_receives(kernel, pid, calling_tid);
            const auto linux_udp_retired = retire_linux_udp_receives(
                kernel, pid, calling_tid, LinuxUdpReceiveRetirement::thread_exit);
            if (!runtime_failure) { started = false; complete = false; return error(runtime_failure); }
            if (!linux_udp_retired) { runtime_failure = linux_udp_retired; started = false; complete = false; return error(linux_udp_retired); }
            auto *thread = kernel.scheduler().find_thread(pid, calling_tid);
            for (const auto &entry : futex_waits)
                if (entry.pid == pid && entry.tid == calling_tid)
                    if (auto status = cancel_futex(kernel, entry.object); !status)
                    { complete = false; return error(status); }
            if (auto status = retire_native_thread(*thread); !status)
            { runtime_failure = status; started = false; complete = false; return error(status); }
            if (!runtime_failure) { started = false; complete = false; return error(runtime_failure); }
            for (const auto &entry : poll_waits)
                if (entry.pid == pid && entry.tid == calling_tid) cancel_poll(kernel, entry.object);
            for (const auto &entry : pipe_read_waits)
                if (entry.pid == pid && entry.tid == calling_tid) cancel_pipe_read(kernel, entry.object);
            for (const auto &entry : pipe_write_waits)
                if (entry.pid == pid && entry.tid == calling_tid) cancel_pipe_write(kernel, entry.object);
            cancel_pty_waits(kernel, pid, calling_tid);
            for (const auto &entry : child_waits)
                if (entry.pid == pid && entry.tid == calling_tid) cancel_child_wait(kernel, entry.object);
            usize live = 0;
            for (const auto &peer : process->threads()) live += peer.state != sched::ProcessState::exited;
            static_cast<void>(retire_robust(kernel, *process, thread->tls, thread->tid));
            clear_child_tid(kernel, *process, thread->tls, live > 1);
            if (!runtime_failure) { started = false; complete = false; return error(runtime_failure); }
            auto retired = kernel.scheduler().exit_thread(pid, calling_tid);
            if (!retired)
            { runtime_failure = retired.status(); started = false; complete = false; return error(retired.status()); }
#ifdef OK_PRIVATE_POLL_MASK
            if (auto status = PrivateChildExit::thread_exited(kernel.scheduler(), pid, calling_tid,
                sched::ChildExitStatus::normal(static_cast<i32>(a[0])), private_signal_find); !status)
            { runtime_failure = status; started = false; complete = false; return error(status); }
#endif
            if (retired.value() == 0) finish(kernel, pid, static_cast<i32>(a[0]));
        }
        else finish(kernel, pid, static_cast<i32>(a[0]));
        complete = false;
        return 0;
    }
    if (number == N::fork)
    {
        auto result = kernel.posix().fork(pid, false);
        if (!result)
            return error(result.status());
        const auto child_pid = result.value();
        auto *child = kernel.scheduler().find(child_pid);
        if (auto status = prepare_native_thread(child->threads()[0]); !status)
        {
            finish(kernel, child_pid, 127);
            static_cast<void>(kernel.posix().wait(pid, child_pid));
            return error(status);
        }
        auto &pages = child->resources().native_pages;
        if (auto status = pages.reserve(page_pool, process->resources().native_pages.size(), true); !status)
        {
            finish(kernel, child_pid, 127);
            static_cast<void>(kernel.posix().wait(pid, child_pid));
            return error(status);
        }
        for (const auto &page : process->resources().native_pages)
        {
            const auto *source_frame = owned_frame(process->resources().native_pages, page);
            if (!source_frame)
            {
                finish(kernel, child_pid, 127);
                static_cast<void>(kernel.posix().wait(pid, child_pid));
                return error(Status::fault("fork page lost exact frame authority"));
            }
            if (shared_pool_page(process->resources().native_pages, page))
            {
                const auto owner = *source_frame;
                if (auto status = retain_page_reference(owner); !status)
                {
                    finish(kernel, child_pid, 127);
                    static_cast<void>(kernel.posix().wait(pid, child_pid));
                    return error(status);
                }
                if (auto status = pages.push_back(page, owner); !status)
                {
                    static_cast<void>(release_page_reference(owner));
                    finish(kernel, child_pid, 127);
                    static_cast<void>(kernel.posix().wait(pid, child_pid));
                    return error(status);
                }
                continue;
            }
            if (auto status = allocate(pages, page.virtual_address, page.flags); !status.ok())
            {
                finish(kernel, child_pid, 127);
                static_cast<void>(kernel.posix().wait(pid, child_pid));
                return error(status);
            }
            copy_bytes(reinterpret_cast<void *>(pages[pages.size() - 1].physical_address),
                       reinterpret_cast<const void *>(source_frame->address), hw::page_size);
        }
        sched::MemoryMap child_memory;
        if (auto status = page_memory_map(pages, child_memory); !status)
        {
            finish(kernel, child_pid, 127);
            static_cast<void>(kernel.posix().wait(pid, child_pid));
            return error(status);
        }
        auto child_space = create_page_space(pages);
        if (!child_space)
        {
            finish(kernel, child_pid, 127);
            static_cast<void>(kernel.posix().wait(pid, child_pid));
            return error(child_space.status());
        }
        child->resources().native_space = child_space.value();
        child->resources().memory = child_memory;
        if (auto status = encode_result(*child, child->threads()[0].native_frame, 0); !status.ok())
        {
            finish(kernel, child_pid, 127);
            static_cast<void>(kernel.posix().wait(pid, child_pid));
            return error(status);
        }
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64) || defined(__mips__) || defined(__powerpc__) || defined(__PPC__))
        if (auto status = private_signal_fork(kernel, pid, calling_tid, child_pid); !status)
        {
            finish(kernel, child_pid, 127);
            static_cast<void>(kernel.posix().wait(pid, child_pid));
            return error(status);
        }
#endif
        child->threads()[0].native_active = true;
        child->threads()[0].syscall_pending = false;
        if (auto status = kernel.scheduler().set_runnable(child_pid); !status)
        {
            finish(kernel, child_pid, 127);
            static_cast<void>(kernel.posix().wait(pid, child_pid));
            return error(status);
        }
        return static_cast<i64>(child_pid);
    }
    if (number == N::execve)
    {
        FixedString<256> path;
        Arguments arguments;
        if (auto status = string_from_user(space, a[0], path); !status.ok())
            return error(status);
        if (auto status = arguments_from_user(space, a[1], arguments.argv); !status.ok())
            return error(status);
        if (auto status = arguments_from_user(space, a[2], arguments.envp); !status.ok())
            return error(status);
        auto previous_image_retirement = ImageRetirementReservation::reserve();
        if (!previous_image_retirement) return error(previous_image_retirement.status());
        auto image = load(kernel, path.view(), arguments);
        if (!image)
            return error(image.status());
        auto context = kernel.arch().make_user_context({image.value().frame.pc, image.value().frame.sp});
#ifdef OK_PRIVATE_POLL_MASK
        const auto &mask_owner = *kernel.scheduler().find_thread(pid, calling_tid);
        const auto inherited_mask = signal_waits.find(mask_owner) ? signal_waits.exec_mask(mask_owner)
                                                                : poll_signal_masks.exec_mask(mask_owner);
#else
        const auto inherited_mask = kernel.scheduler().find_thread(pid, calling_tid)->native_signal_mask;
#endif
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64) || defined(__mips__) || defined(__powerpc__) || defined(__PPC__))
        auto *pending_before_exec = private_signal_queue(*kernel.scheduler().find_thread(pid, calling_tid));
        const auto altstack_before_exec = *private_signal_altstack(*kernel.scheduler().find_thread(pid, calling_tid));
        std::array<RetainedEntryStack, sched::max_threads_per_process> old_signal_owners{};
        usize old_signal_owner_count{};
        for (const auto &old_thread : process->threads())
            if (old_thread.entry_stack.pool)
                old_signal_owners[old_signal_owner_count++] = {{pid, old_thread.tid}, old_thread.entry_stack, 0};
#endif
        ExecRetirement retirement{*process, calling_tid};
        VfsImageRetirement vfs_retirement;
        if (auto status = vfs_retirement.prepare(kernel.scheduler(), *process); !status)
        {
            return error(status);
        }
#if defined(OK_PRIVATE_MIPS64_N64_SIGNAL) || defined(OK_PRIVATE_PPC32_SIGNAL)
        std::array<u32, sched::max_threads_per_process> previous_signal_slots{};
        usize previous_signal_count{};
        for (const auto &old_thread : process->threads())
            if (old_thread.entry_stack.pool) previous_signal_slots[previous_signal_count++] = old_thread.entry_stack.slot;
#endif
        bool vfs_retired{};
        const auto replaced = replace_entry_stacks(entry_stacks, *process,
            [&] {
                return kernel.posix().exec(pid, context, image.value().memory);
            }, [&] {
                // Scheduler commit has escrowed the old entry stacks. A failed
                // futex discharge must retain them and both VM generations.
                if (auto status = cancel_futex(kernel, pid); !status) return status;
                const auto native_udp_retired = retire_native_udp_receives_for_exec(kernel, pid);
                const auto linux_udp_retired = retire_linux_udp_receives_for_exec(kernel, pid);
                if (!native_udp_retired) return native_udp_retired;
                if (!linux_udp_retired) return linux_udp_retired;
                const auto status = vfs_retirement.consume(kernel.scheduler(), *process);
                vfs_retired = status.ok();
                if (!status) return status;
#ifdef OK_PRIVATE_POLL_MASK
                {
                    for (const auto &wait : signal_waits.records())
                        if (wait.phase != PrivateSignalWaitState::Phase::none && wait.pid == pid)
                            private_signal_wait_trace_retire(wait, "exec");
                    poll_signal_masks.discard_process(pid); signal_waits.discard_process(pid);
                }
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
                for (usize i = 0; i < previous_signal_count; ++i) private_n64_revoke_slot(previous_signal_slots[i]);
#endif
#ifdef OK_PRIVATE_PPC32_SIGNAL
                for (usize i = 0; i < previous_signal_count; ++i) private_ppc_revoke_slot(previous_signal_slots[i]);
#endif
                return status;
            });
        if (replaced.committed && vfs_retired) retirement.consume(kernel, *process);
        if (!replaced)
        {
            if (!replaced.committed)
            {
                return error(replaced.status);
            }
            // Exec cannot return to the old image after scheduler commit. Keep
            // both VM generations and every unreleased lease for diagnosis.
            for (usize i = 0; i < replaced.retained_count; ++i)
            {
                const auto &saved = replaced.retained[i];
                stranded_entry_stacks[saved.handle.slot] = {saved.owner, saved.handle};
            }
            static_cast<void>(image.value().retain_for_diagnosis());
            runtime_failure = replaced.status;
            started = false;
            complete = false;
            return 0;
        }
        cancel_poll(kernel, pid);
        if (!runtime_failure)
        {
            static_cast<void>(image.value().retain_for_diagnosis());
            started = false;
            complete = false;
            return 0;
        }
        if (auto status = retire_then_install(process->resources(), image.value(), previous_image_retirement.value(),
                [&] { return install(*process, image.value(), inherited_mask); }); !status)
        {
            runtime_failure = status;
            started = false;
            complete = false;
            return 0;
        }
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64) || defined(__mips__) || defined(__powerpc__) || defined(__PPC__))
        private_signal_exec(kernel, *process, pending_before_exec, altstack_before_exec);
        for (usize index = 0; index < old_signal_owner_count; ++index)
            if (old_signal_owners[index].handle != process->threads()[0].entry_stack)
                private_signal_thread_retire(pid, old_signal_owners[index].owner.tid, old_signal_owners[index].handle);
#endif
        complete = false; // The new image has its own entry register state.
        return 0;
    }
    if (number == N::wait4)
    {
        if ((a[2] & ~uptr{1}) || a[3]) return -22;
        return begin_child_wait(kernel, pid, {static_cast<i32>(a[0]), a[1], (a[2] & 1) != 0}, complete);
    }

    if (number == N::pipe || number == N::pipe2)
    {
        if (!hw::user_range(space, a[0], sizeof(i32) * 2, true))
            return -14;
        auto fds = kernel.posix().pipe(number == N::pipe2 ? static_cast<u32>(a[1]) : 0);
        if (!fds)
            return error(fds.status());
        if (!hw::copy_to_user(space, a[0], fds.value().data(), sizeof(i32) * 2))
        {
            static_cast<void>(kernel.posix().close(fds.value()[0]));
            static_cast<void>(kernel.posix().close(fds.value()[1]));
            return -14;
        }
        return 0;
    }
    if (number == N::readv || number == N::writev)
    {
        using Vector = PipeVector;
        std::array<Vector, PipeVectors::capacity> vectors{};
        if (a[2] > vectors.size()) return -22;
        if (!hw::copy_from_user(space, vectors.data(), a[1], a[2] * sizeof(Vector))) return -14;
        const auto *slot = process->fd_table().entry(static_cast<i32>(a[0]));
        auto *description = slot && slot->used ? kernel.scheduler().open_description(slot->description) : nullptr;
        const bool writing = number == N::writev;
        if (!description || !(writing ? description->writable : description->readable)) return -9;
        usize total = 0;
        for (usize i = 0; i < a[2]; ++i)
        {
            if (vectors[i].length > static_cast<usize>(__INTPTR_MAX__) - total) return -22;
            total += vectors[i].length;
        }
        if (description->pty_endpoint)
        {
            const PipeVectors request{vectors, a[2]};
            const auto staged = writing || total < sched::private_pty::RawPtyTable<>::queue_capacity
                ? total : sched::private_pty::RawPtyTable<>::queue_capacity;
            return begin_pty_io(kernel, *process, calling_tid, static_cast<i32>(a[0]), slot->description,
                                a[1], staged, writing, complete, request);
        }
        if (!total) return 0;
        if (writing && description->native_shared_file)
            if (auto status = shared_files.check_write(description->native_shared_file, total); !status)
                return error(status);
        if (description->pipe != sched::invalid_description)
        {
            const PipeVectors request{vectors, a[2]};
            const usize limit = sched::pipe_capacity + (writing ? 1 : 0);
            const usize staged = total < limit ? total : limit;
            return writing ? begin_pipe_write(kernel, *process, calling_tid, static_cast<i32>(a[0]),
                slot->description, a[1], total, complete, request)
                : begin_pipe_read(kernel, *process, calling_tid, static_cast<i32>(a[0]),
                slot->description, a[1], staged, complete, request);
        }
        for (usize i = 0; i < a[2]; ++i)
            if (!hw::user_range(space, vectors[i].base, vectors[i].length, !writing)) return -14;
        std::array<std::byte, sched::pipe_capacity> data{};
        const usize limit = data.size();
        if (total > limit) total = limit;
        usize position = 0;
        if (writing)
            for (usize i = 0; i < a[2] && position < total; ++i)
            {
                auto length = vectors[i].length < total - position ? vectors[i].length : total - position;
                if (!hw::copy_from_user(space, data.data() + position, vectors[i].base, length)) return -14;
                position += length;
            }
        Result<usize> result = usize{0};
        if (writing && description->console)
        {
            emit({reinterpret_cast<const char *>(data.data()), total});
            result = kernel.posix().write(static_cast<i32>(a[0]), {data.data(), total});
        }
        else if (!writing && description->input_console)
        {
            const auto epoch = caller->console_wait_epoch ? caller->console_wait_epoch : process->console_epoch();
            result = kernel.scheduler().read_console(pid, epoch, {data.data(), total});
            if (!result && result.status().code() == StatusCode::would_block &&
                !(description->flags & posix::o_NONBLOCK))
            {
                caller->console_wait_epoch = epoch;
                if (auto status = kernel.scheduler().block_current(sched::ThreadWaitReason::console_read, epoch); !status)
                { caller->console_wait_epoch = 0; return error(status); }
            }
        }
        else if (writing)
            result = kernel.posix().write(static_cast<i32>(a[0]), {data.data(), total});
        else if (description->native_shared_file)
            result = kernel.posix().pread(static_cast<i32>(a[0]), {data.data(), total}, description->offset);
        else result = kernel.posix().read(static_cast<i32>(a[0]), {data.data(), total});
        if (!result)
        {
            if (result.status().code() == StatusCode::would_block &&
                blocked_caller()) complete = false;
            return error(result.status());
        }
        if (!writing)
        {
            position = 0;
            for (usize i = 0; i < a[2] && position < result.value(); ++i)
            {
                auto length = vectors[i].length < result.value() - position ? vectors[i].length : result.value() - position;
                if (!hw::copy_to_user(space, vectors[i].base, data.data() + position, length)) return -14;
                position += length;
            }
            if (description->native_shared_file) description->offset += result.value();
        }
        return static_cast<i64>(result.value());
    }
    if (number == N::rename || number == N::link || number == N::renameat || number == N::linkat)
    {
        const bool relative = number == N::renameat || number == N::linkat;
        FixedString<256> source, destination;
        if (auto status = string_from_user(space, a[relative ? 1 : 0], source); !status.ok())
            return error(status);
        if (auto status = string_from_user(space, a[relative ? 3 : 1], destination); !status.ok())
            return error(status);
        Status result;
        if (number == N::rename)
            result = kernel.posix().rename(source.view(), destination.view());
        else if (number == N::link)
            result = kernel.posix().link(source.view(), destination.view());
        else if (number == N::renameat)
            result = kernel.posix().renameat(static_cast<i32>(a[0]), source.view(), static_cast<i32>(a[2]),
                                            destination.view());
        else
            result = kernel.posix().linkat(static_cast<i32>(a[0]), source.view(), static_cast<i32>(a[2]),
                                          destination.view(), static_cast<u32>(a[4]));
        return result.ok() ? 0 : error(result);
    }
    if (number == N::symlink || number == N::symlinkat)
    {
        FixedString<256> target, path;
        if (auto status = string_from_user(space, a[0], target); !status.ok())
            return error(status);
        if (auto status = string_from_user(space, a[number == N::symlink ? 1 : 2], path); !status.ok())
            return error(status);
        return error(kernel.posix().symlinkat(target.view(),
            number == N::symlink ? posix::at_FDCWD : static_cast<i32>(a[1]), path.view()));
    }
    if ((number == N::pread64 || number == N::pwrite64) && static_cast<__INTPTR_TYPE__>(a[3]) < 0)
        return -22;
    if ((number == N::truncate || number == N::ftruncate) && static_cast<__INTPTR_TYPE__>(a[1]) < 0)
        return -22;
    syscall::Request request{.number = number, .caller = pid};
    const auto *fd = process->fd_table().entry(static_cast<i32>(a[0]));
    const auto *description = fd && fd->used ? kernel.scheduler().open_description(fd->description) : nullptr;
    if ((number == N::read || number == N::write) && description && description->pty_endpoint)
    {
        const bool writing = number == N::write;
        const auto count = writing || a[2] < sched::private_pty::RawPtyTable<>::queue_capacity
            ? a[2] : sched::private_pty::RawPtyTable<>::queue_capacity;
        return begin_pty_io(kernel, *process, calling_tid, static_cast<i32>(a[0]), fd->description,
                            a[1], count, writing, complete);
    }
    if (number == N::write && description && description->writable && description->pipe != sched::invalid_description)
        return begin_pipe_write(kernel, *process, calling_tid, static_cast<i32>(a[0]), fd->description, a[1],
            a[2], complete);
    if (number == N::read && description && description->readable && description->pipe != sched::invalid_description)
        return begin_pipe_read(kernel, *process, calling_tid, static_cast<i32>(a[0]), fd->description, a[1],
            a[2] < sched::pipe_capacity ? a[2] : sched::pipe_capacity, complete);
    for (usize i = 0; i < 6; ++i)
        request.args[i] = a[i];
    if (number == N::lseek)
        request.args[1] = static_cast<u64>(static_cast<__INTPTR_TYPE__>(a[1]));
    alignas(16) std::array<std::byte, sched::pipe_capacity + 1> buffer{};
    FixedString<256> path;
    int path_arg = -1, output_arg = -1, input_arg = -1;
    usize output_size = 0, input_size = 0;
    bool variable_output = false;
    switch (number)
    {
    case N::read:
    case N::pread64:
    case N::getdents:
    case N::getdents64:
        output_arg = 1;
        output_size = a[2] < sched::pipe_capacity ? a[2] : sched::pipe_capacity;
        request.args[2] = output_size;
        variable_output = true;
        break;
    case N::write:
    case N::pwrite64:
    {
        if (description && description->writable && description->native_shared_file)
            if (auto status = shared_files.check_write(description->native_shared_file, a[2]); !status)
                return error(status);
        input_arg = 1;
        const usize limit = number == N::write && description && description->pipe != sched::invalid_description
                                ? buffer.size() : sched::pipe_capacity;
        input_size = a[2] < limit ? a[2] : limit;
        request.args[2] = input_size;
        break;
    }
    case N::truncate:
    case N::open:
    case N::creat:
    case N::mkdir:
    case N::unlink:
    case N::rmdir:
    case N::chdir:
    case N::access:
        path_arg = 0;
        break;
    case N::openat:
    case N::mkdirat:
    case N::unlinkat:
    case N::faccessat:
    case N::faccessat2:
        path_arg = 1;
        break;
    case N::stat:
    case N::lstat:
        path_arg = 0;
        output_arg = 1;
        output_size = sizeof(ok_stat);
        break;
    case N::fstat:
        output_arg = 1;
        output_size = sizeof(ok_stat);
        break;
    case N::newfstatat:
        path_arg = 1;
        output_arg = 2;
        output_size = sizeof(ok_stat);
        break;
    case N::fstat_v2:
        output_arg = 1;
        output_size = sizeof(ok_stat_v2);
        break;
    case N::newfstatat_v2:
        path_arg = 1;
        output_arg = 2;
        output_size = sizeof(ok_stat_v2);
        break;
    case N::readlink:
    case N::readlinkat:
    {
        const bool relative = number == N::readlinkat;
        path_arg = relative ? 1 : 0;
        output_arg = relative ? 2 : 1;
        const usize size_arg = relative ? 3 : 2;
        if (a[size_arg] == 0)
            return -22;
        output_size = a[size_arg] < buffer.size() ? a[size_arg] : buffer.size();
        request.args[size_arg] = output_size;
        variable_output = true;
        break;
    }
    case N::getcwd:
        output_arg = 0;
        output_size = a[1] < buffer.size() ? a[1] : buffer.size();
        request.args[1] = output_size;
        break;
    case N::uname:
        output_arg = 0;
        output_size = sizeof(posix::UnameInfo);
        break;
    case N::getrlimit:
        output_arg = 1;
        output_size = sizeof(posix::ResourceLimit);
        break;
    case N::sysinfo:
        output_arg = 0;
        output_size = sizeof(posix::SystemInfo);
        break;
    case N::getrandom:
        output_arg = 0;
        output_size = a[1] < buffer.size() ? a[1] : buffer.size();
        request.args[1] = output_size;
        variable_output = true;
        break;
    case N::kill:
#ifdef OK_PRIVATE_POLL_MASK
        return native_process_kill(kernel, *process, a[0], a[1]);
#else
        return -95;
#endif
    case N::fsync:
    case N::fdatasync:
    case N::ftruncate:
    case N::close:
    case N::close_range:
    case N::dup:
    case N::dup2:
    case N::dup3:
    case N::lseek:
    case N::getpid:
    case N::getppid:
    case N::setpgid:
    case N::getpgid:
    case N::getpgrp:
    case N::getsid:
    case N::setsid:
    case N::gettid:
    case N::getuid:
    case N::getgid:
    case N::geteuid:
    case N::getegid:
    case N::umask:
    case N::fcntl:
    case N::fchdir:
    case N::sched_yield:
        break;
    default:
        return -38;
    }
    if (path_arg >= 0)
    {
        if (auto status = string_from_user(space, a[path_arg], path); !status.ok())
            return error(status);
        request.args[path_arg] = reinterpret_cast<uptr>(path.c_str());
    }
    if (input_arg >= 0)
    {
        if (!hw::copy_from_user(space, buffer.data(), a[input_arg], input_size))
            return -14;
        request.args[input_arg] = reinterpret_cast<uptr>(buffer.data());
    }
    if (output_arg >= 0)
    {
        // readlink has no side effects: validate only the bytes it returns,
        // so unused capacity may extend beyond the caller's mapped buffer.
        if (number != N::readlink && number != N::readlinkat &&
            !hw::user_range(space, a[output_arg], output_size, true))
            return -14;
        request.args[output_arg] = reinterpret_cast<uptr>(buffer.data());
    }
    if (number == N::write && description && description->console && description->writable)
    {
        emit({reinterpret_cast<const char *>(buffer.data()), input_size});
        static_cast<void>(kernel.console().write({reinterpret_cast<const char *>(buffer.data()), input_size}));
        return static_cast<i64>(input_size);
    }
    if (number == N::read && description && description->input_console)
    {
        if (!description->readable) return -9;
        const auto epoch = caller->console_wait_epoch ? caller->console_wait_epoch : process->console_epoch();
        auto read = kernel.scheduler().read_console(pid, epoch, {buffer.data(), output_size});
        if (!read && read.status().code() == StatusCode::would_block)
        {
            if (description->flags & posix::o_NONBLOCK)
                return -11;
            caller->console_wait_epoch = epoch;
            auto status = kernel.scheduler().block_current(sched::ThreadWaitReason::console_read, epoch);
            if (!status)
            { caller->console_wait_epoch = 0; return error(status); }
            complete = false;
            return -11;
        }
        if (!read) return error(read.status());
        const auto count = read.value();
        if (!hw::copy_to_user(space, a[1], buffer.data(), count))
            return -14;
        return static_cast<i64>(count);
    }
    if (description && description->native_shared_file && (number == N::read || number == N::pread64))
    {
        auto read = kernel.posix().pread(static_cast<i32>(a[0]), {buffer.data(), output_size},
            number == N::read ? description->offset : static_cast<usize>(a[3]));
        if (!read) return error(read.status());
        if (!hw::copy_to_user(space, a[1], buffer.data(), read.value())) return -14;
        if (number == N::read)
        {
            auto *open = kernel.scheduler().open_description(fd->description);
            if (!open || open->native_shared_file != description->native_shared_file)
                return error(Status::fault("shared file read publication lost its canonical description"));
            open->offset += read.value();
        }
        return static_cast<i64>(read.value());
    }
    auto result = kernel.syscalls().dispatch(request);
    if (!result.status.ok())
    {
        if (result.status.code() == StatusCode::would_block &&
            blocked_caller())
            complete = false;
        return error(result.status);
    }
    if (output_arg >= 0)
    {
        if (variable_output)
            output_size = static_cast<usize>(result.value);
        if (output_size > buffer.size() || !hw::copy_to_user(space, a[output_arg], buffer.data(), output_size))
            return -14;
    }
    return result.value;
}
} // namespace

i64 futex_wake_operation(Kernel &kernel, sched::ProcessId pid, const FutexWakeOperationRequest &request)
{
    if (!runtime_failure) return -5;
    auto *process = kernel.scheduler().find(pid);
    auto *current = kernel.scheduler().current_thread();
    if (!process || !current || current->owner != pid || current->state != sched::ProcessState::running ||
        kernel.scheduler().current_pid() != pid)
        return -22;
    if (request.domain != FutexKeyDomain::private_address_space &&
        request.domain != FutexKeyDomain::anonymous_shared_operation)
        return -38;
    if (request.first_address % sizeof(u32) || request.second_address % sizeof(u32)) return -22;
    if (request.first_address > hw::user_end - sizeof(u32) ||
        request.second_address > hw::user_end - sizeof(u32)) return -14;
    const auto first = resolve_futex_key(*process, request.first_address, request.domain);
    if (!first) return error(first.status());
    const auto second = resolve_futex_key(*process, request.second_address, request.domain);
    if (!second) return error(second.status());
    const auto &space = process->resources().native_space;
    const auto operation = lu::FutexWakeOperation::decode(request.encoded_operation);
    // Reject an undefined arithmetic operation without reading or modifying
    // user memory. Linux compares only after the modification has succeeded.
    if (operation.operation > 4) return -38;
    if (!hw::user_range(space, request.second_address, sizeof(u32), true)) return -14;
    std::array<u8, sizeof(u32)> bytes{};
    if (!hw::copy_from_user(space, bytes.data(), request.second_address, bytes.size())) return -14;
    u32 previous{};
    for (usize index = 0; index < bytes.size(); ++index)
        previous |= static_cast<u32>(bytes[index]) << (8 * (request.big_endian ? 3 - index : index));
    const auto modified = operation.modify(previous);
    if (!modified) return error(modified.status());
    for (usize index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<u8>(modified.value() >> (8 * (request.big_endian ? 3 - index : index)));
    // There is no user/IRQ queue producer in this serialized boot-CPU span.
    // SMP needs an atomic user-memory operation and futex queue locking.
    if (!hw::copy_to_user(space, request.second_address, bytes.data(), bytes.size())) return -14;
    const auto comparison = operation.compare(previous);
    if (!comparison) return error(comparison.status());
    const auto limit = [](u32 value) { return std::bit_cast<i32>(value) <= 0 ? 1U : value; };
    const auto woke_first = wake_futex(kernel, pid, request.first_address, request.domain, limit(request.first_limit));
    if (woke_first < 0 || !comparison.value()) return woke_first;
    const auto woke_second = wake_futex(kernel, pid, request.second_address, request.domain, limit(request.second_limit));
    return woke_second < 0 ? woke_second : woke_first + woke_second;
}

bool same_futex_frame(const memory::PhysicalFrame &left, const memory::PhysicalFrame &right)
{
    return left.owner == right.owner && left.index == right.index &&
        left.generation == right.generation && left.address == right.address;
}
Status rollback_futex_transfer_pins(const FutexQueueSnapshot &snapshot, usize begin, usize count)
{
    for (usize index = begin; index < begin + count; ++index)
    {
        auto resolved = futex_objects.resolve(snapshot.entries[index].object);
        if (!resolved) return stop_futex_runtime(resolved.status());
        auto &wait = *resolved.value();
        if (!wait.pending_transfer_pin_owned) continue;
        if (auto status = page_pool.unpin(wait.pending_transfer_frame); !status)
            return stop_futex_runtime(status);
        wait.pending_transfer_pin_owned = false;
        wait.pending_transfer_frame = {};
    }
    return Status::success();
}
i64 futex_requeue(Kernel &kernel, sched::ProcessId pid, const FutexRequeueRequest &request)
{
    if (!runtime_failure) return -5;
    auto *process = kernel.scheduler().find(pid);
    auto *current = kernel.scheduler().current_thread();
    if (!process || !current || current->owner != pid || current->state != sched::ProcessState::running ||
        kernel.scheduler().current_pid() != pid) return -22;
#if defined(OK_ENABLE_TEST_POINTS) && defined(OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE)
    emit("OK_NATIVE_FUTEX_REQUEUE_CALL pid="); emit_number(pid); emit(" tid="); emit_number(current->tid);
    emit(" source="); emit_number(request.source_address); emit(" destination="); emit_number(request.destination_address);
    emit(" wake_bits="); emit_number(request.wake_limit); emit(" requeue_bits="); emit_number(request.requeue_limit);
    emit(" compare="); emit_number(request.compare); emit(" expected="); emit_number(request.expected);
    emit(" private="); emit_number(request.domain == FutexKeyDomain::private_address_space); emit("\n");
#endif
    if (request.domain != FutexKeyDomain::private_address_space &&
        request.domain != FutexKeyDomain::anonymous_shared_operation) return -38;
    if (std::bit_cast<i32>(request.wake_limit) < 0 || std::bit_cast<i32>(request.requeue_limit) < 0) return -22;
    const auto source = resolve_futex_key(*process, request.source_address, request.domain, FutexKeyAccess::read);
    if (!source) return error(source.status());
    const auto destination = resolve_futex_key(*process, request.destination_address, request.domain, FutexKeyAccess::read);
    if (!destination) return error(destination.status());
    if (request.compare)
    {
        std::array<u8, sizeof(u32)> bytes{};
        if (!hw::copy_from_user(process->resources().native_space, bytes.data(), request.source_address, bytes.size()))
            return -14;
        u32 value{};
        for (usize index = 0; index < bytes.size(); ++index)
            value |= static_cast<u32>(bytes[index]) << (8 * (request.big_endian ? 3 - index : index));
        if (value != request.expected) return -11;
    }
    auto selected = snapshot_futex_queue(kernel, source.value());
    if (!selected) return -5;
    const auto &snapshot = selected.value();
    const usize wakes = request.wake_limit < snapshot.count ? request.wake_limit : snapshot.count;
    const usize remaining = snapshot.count - wakes;
    const usize transfers = request.requeue_limit < remaining ? request.requeue_limit : remaining;
    const bool same_key = same_futex_key(source.value(), destination.value());
    auto sequences = reserve_futex_sequences(same_key ? 0 : transfers);
    if (!sequences) return error(sequences.status());
    // All fallible new pins precede wakes/key publication. Each successful pin
    // has a discoverable waiter owner before the next fallible operation.
    for (usize index = wakes; index < wakes + transfers; ++index)
    {
        auto resolved = selected_futex_wait(kernel, snapshot.entries[index], source.value());
        if (!resolved) return -5;
        auto &wait = *resolved.value();
        if (destination.value().shared &&
            (!wait.pin_owned || !same_futex_frame(wait.key.frame, destination.value().frame)))
        {
            if (auto status = page_pool.pin(destination.value().frame); !status)
            {
                if (!rollback_futex_transfer_pins(snapshot, wakes, transfers)) return -5;
                return error(status);
            }
            wait.pending_transfer_frame = destination.value().frame;
            wait.pending_transfer_pin_owned = true;
        }
    }
    for (usize index = 0; index < wakes; ++index)
    {
        if (!selected_futex_wait(kernel, snapshot.entries[index], source.value()) ||
            !finish_futex(kernel, snapshot.entries[index].object, sched::WaitCompletion::ready, 0))
        {
            if (runtime_failure) stop_futex_runtime(Status::fault("requeue wake lost exact continuation"));
            return -5;
        }
        if (!runtime_failure) return -5;
    }
    for (usize index = wakes; index < wakes + transfers; ++index)
    {
        auto resolved = selected_futex_wait(kernel, snapshot.entries[index], source.value());
        if (!resolved) return -5;
        auto &wait = *resolved.value();
        const bool reuse = wait.pin_owned && destination.value().shared &&
            same_futex_frame(wait.key.frame, destination.value().frame);
        if (wait.pin_owned && !reuse)
        {
            if (auto status = page_pool.unpin(wait.key.frame); !status)
            {
                stop_futex_runtime(status);
                return -5;
            }
            wait.pin_owned = false;
        }
        wait.key = destination.value();
        if (!reuse)
        {
            wait.pin_owned = wait.pending_transfer_pin_owned;
            wait.pending_transfer_pin_owned = false;
            wait.pending_transfer_frame = {};
        }
        if (!same_key) wait.queue_sequence = sequences.value() + (index - wakes);
#if defined(OK_ENABLE_TEST_POINTS) && defined(OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE)
        emit("OK_NATIVE_FUTEX_REQUEUE_TRANSFER pid="); emit_number(wait.pid); emit(" tid="); emit_number(wait.tid);
        emit(" wait="); emit_number(wait.ticket.generation); emit(" origin="); emit_number(wait.origin);
        emit(" address="); emit_number(wait.address); emit(" destination="); emit_number(request.destination_address);
        emit(" sequence="); emit_number(wait.queue_sequence); emit(" key_shared="); emit_number(wait.key.shared);
        emit("\n");
#endif
    }
    return static_cast<i64>(wakes + transfers);
}

i64 futex(Kernel &kernel, sched::ProcessId pid, const FutexRequest &request, bool &complete)
{
    if (!runtime_failure) return -5;
    auto *process = kernel.scheduler().find(pid);
    auto *current = kernel.scheduler().current_thread();
    if (!process || !current || current->owner != pid || current->state != sched::ProcessState::running ||
        kernel.scheduler().current_pid() != pid)
        return -22;
    if (request.operation != FutexOperation::wait && request.operation != FutexOperation::wake)
        return -38;
    if (request.domain != FutexKeyDomain::private_address_space &&
        request.domain != FutexKeyDomain::anonymous_shared_operation)
        return -38;
    if (request.operation == FutexOperation::wait && request.timed &&
        (request.timeout.seconds < 0 || request.timeout.nanoseconds < 0 || request.timeout.nanoseconds >= 1000000000))
        return -22;
    sched::DeadlineWait deadline;
    if (request.operation == FutexOperation::wait && request.timed)
    {
        auto now = arch::monotonic_nanoseconds();
        if (!now)
            return error(now.status());
        const auto end =
            lu::linux_sleep_deadline(now.value(), {request.timeout.seconds, request.timeout.nanoseconds}, false);
        auto armed =
            deadline.arm(now.value(), static_cast<i64>(end / 1000000000), static_cast<i64>(end % 1000000000), true);
        if (!armed)
            return error(armed.status());
    }
    if (request.address % sizeof(u32))
        return -22;
    if (request.address > hw::user_end - sizeof(u32))
        return -14;
    auto key = resolve_futex_key(*process, request.address, request.domain);
    if (!key) return error(key.status());
    if (request.operation == FutexOperation::wake)
    {
        // The Linux legacy WAKE loop increments before comparing a signed
        // count. Zero and negative counts therefore wake one matching waiter.
        const u32 limit = static_cast<i32>(request.value) <= 0 ? 1 : request.value;
        return wake_futex(kernel, pid, request.address, request.domain, limit);
    }
    // No user execution or IRQ queue producer can run in this serialized span.
    // Do not generalize it to SMP without a lock and explicit memory ordering.
    std::array<u8, 4> bytes{};
    if (!hw::copy_from_user(process->resources().native_space, bytes.data(), request.address, bytes.size()))
        return -14;
    u32 value = 0;
    for (usize i = 0; i < bytes.size(); ++i)
        value |= static_cast<u32>(bytes[i]) << (8 * (request.big_endian ? 3 - i : i));
    if (value != request.value)
        return -11;
    if (request.timed)
    {
        auto now = arch::monotonic_nanoseconds();
        if (!now) return error(now.status());
        if (deadline.complete(now.value(), deadline.generation())) return -110;
    }
    PollWaitIndex *slot = nullptr;
    for (auto &entry : futex_waits)
    {
        if (entry.pid == pid && entry.tid == current->tid)
            return -22;
        if (!entry.pid && !slot)
            slot = &entry;
    }
    if (!slot)
        return -12;
    auto sequence = reserve_futex_sequences(1);
    if (!sequence) return error(sequence.status());
#ifndef OK_KERNEL_FREESTANDING
    if (auto status = activate_pool(); !status)
        return error(status);
#endif
    const bool pinned = key.value().shared;
    if (pinned)
        if (auto status = page_pool.pin(key.value().frame); !status)
            return error(status);
    auto rollback_pin = [&](Status failure) -> i64
    {
        if (pinned)
            if (auto status = page_pool.unpin(key.value().frame); !status)
            {
                runtime_failure = status;
                started = false;
                return -5;
            }
        return error(failure);
    };
    auto allocated = futex_objects.create();
    if (!allocated)
        return rollback_pin(allocated.status());
    const auto object = allocated.value();
    auto resolved = futex_objects.resolve(object);
    if (!resolved)
    {
        const auto failure = resolved.status();
        if (auto status = futex_objects.destroy(object); !status)
        { runtime_failure = status; started = false; return -5; }
        if (auto status = futex_objects.trim_empty_pages(); !status)
        { runtime_failure = status; started = false; return -5; }
        return rollback_pin(failure);
    }
    auto &wait = *resolved.value();
    wait.pid = pid;
    wait.tid = current->tid;
    wait.address = request.address;
    wait.domain = request.domain;
    wait.key = key.value();
    wait.pin_owned = pinned;
    wait.queue_sequence = sequence.value();
    wait.timeout = deadline;
    wait.expected = request.value;
    auto registration = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::futex, request.address);
    if (!registration)
    {
        cancel_futex(kernel, object);
        return error(registration.status());
    }
    wait.ticket = registration.value();
    *slot = {pid, current->tid, object};
    const auto origin = syscall_origins.identity(*process, *current);
    if (syscall_origins.live(origin))
    {
        // Only the first four WAIT inputs exist on every profile. In particular,
        // o32 arguments five/six must not be fetched from an unused user stack.
        const auto call = SyscallOrigins::Adapter::Bridge::decode_call(current->native_frame, 4);
        if (call && (call.value().arguments[3] != 0) == request.timed)
        {
            wait.syscall = call.value().number;
            wait.timeout_address = call.value().arguments[3];
            if (syscall_origins.bind(origin, current->native_frame, futex_origin_binding(object, wait)))
                wait.origin = origin.capture;
        }
        if (!wait.origin)
        {
            syscall_origins.revoke(*current);
            if (pinned)
            {
                cancel_futex(kernel, object);
                return -5;
            }
        }
    }
    emit_futex_arm(wait, current->native_frame.pc);
    emit_shared_futex_arm(wait);
    complete = false;
    return 0;
}

void finish_poll_timeout(const hw::AddressSpace &space, const PollTimeoutOutput &output, u64 now)
{
    if (!output.address || (output.field_bytes != 4 && output.field_bytes != 8))
        return;
    constexpr i64 second = 1000000000;
    i64 seconds = output.end.seconds - static_cast<i64>(now / second);
    i64 nanos = output.end.nanoseconds - static_cast<i64>(now % second);
    if (nanos < 0) { --seconds; nanos += second; }
    if (seconds < 0) seconds = nanos = 0;
    std::array<u8, 16> bytes{};
    for (usize field = 0; field < 2; ++field)
    {
        const u64 value = static_cast<u64>(field ? (output.format == PollTimeoutFormat::timeval ? nanos / 1000 : nanos) : seconds);
        for (usize i = 0; i < output.field_bytes; ++i)
            bytes[field * output.field_bytes + i] = static_cast<u8>(value >>
                (8 * (output.big_endian ? output.field_bytes - 1 - i : i)));
    }
    // Linux poll_select_finish ignores timeout copyout failure when there is no
    // signal restart. It must never replace the descriptor operation's result.
    static_cast<void>(hw::copy_to_user(space, output.address, bytes.data(), output.field_bytes * 2));
}

i64 begin_poll_signal_mask(Kernel &kernel, sched::ProcessId pid, sched::SignalMask requested)
{
#ifdef OK_PRIVATE_POLL_MASK
    auto *process = kernel.scheduler().find(pid);
    auto *thread = kernel.scheduler().current_thread();
    if (!process || !thread || thread->owner != pid || kernel.scheduler().current_pid() != pid ||
        thread->state != sched::ProcessState::running || !thread->native_active ||
        process->resources().personality != Personality::linux || !validate_entry_stack(entry_stacks, *thread)) return -22;
    if (signal_waits.find(*thread)) return -22;
    return poll_signal_masks.begin(*thread, requested) ? 0 : -22;
#else
    static_cast<void>(kernel); static_cast<void>(pid); static_cast<void>(requested);
    return -38;
#endif
}
void finish_poll_signal_mask(Kernel &kernel, sched::ProcessId pid, bool complete)
{
#ifdef OK_PRIVATE_POLL_MASK
    auto *thread = kernel.scheduler().current_thread();
    if (complete && thread && thread->owner == pid) poll_signal_masks.restore(*thread);
#else
    static_cast<void>(kernel); static_cast<void>(pid); static_cast<void>(complete);
#endif
}

static i64 begin_readiness_wait(Kernel &kernel, sched::ProcessId pid, const PollRequest &request,
                                bool &complete, const SelectSets *sets = nullptr)
{
    auto *process = kernel.scheduler().find(pid);
    auto *current = kernel.scheduler().current_thread();
    if (!process || !current || current->owner != pid || current->state != sched::ProcessState::running ||
        kernel.scheduler().current_pid() != pid)
        return -22;
    auto &space = process->resources().native_space;
    auto finish = [&](i64 result) {
        if (request.timeout_output.address)
            if (auto now = arch::monotonic_nanoseconds(); now)
                finish_poll_timeout(space, request.timeout_output, now.value());
        return result;
    };
    if (request.descriptors.size() > sched::max_process_fds)
        return finish(-22);
    for (const auto &candidate : poll_waits)
        if (candidate.pid == pid && candidate.tid == current->tid) return finish(-22);
    PollWaitIndex *entry = nullptr;
    for (auto &candidate : poll_waits)
        if (candidate.pid == 0) { entry = &candidate; break; }
    if (!entry) return finish(-12);
#ifndef OK_KERNEL_FREESTANDING
    if (auto status = activate_pool(); !status) return finish(error(status));
#endif
    auto allocated = poll_objects.create();
    if (!allocated) return finish(error(allocated.status()));
    const auto object = allocated.value();
    auto resolved = poll_objects.resolve(object);
    if (!resolved)
    {
        if (auto status = poll_objects.destroy(object); !status) runtime_failure = status;
        if (auto status = poll_objects.trim_empty_pages(); !status) runtime_failure = status;
        return finish(error(resolved.status()));
    }
    auto *wait = resolved.value();
    wait->pid = pid;
    wait->tid = current->tid;
    wait->console_epoch = process->console_epoch();
    wait->output = request.output;
    wait->count = request.descriptors.size();
    wait->descriptions.fill(sched::invalid_description);
    wait->timeout_output = request.timeout_output;
    wait->big_endian = request.big_endian;
    wait->read_normal = request.read_normal;
    wait->write_normal = request.write_normal;
    wait->linux_events = request.linux_events;
    wait->select_sets = sets != nullptr;
    if (sets) wait->sets = *sets;
    auto done = [&](i64 result) { cancel_poll(kernel, object); return finish(result); };
    for (usize i = 0; i < wait->count; ++i)
    {
        wait->fds[i] = request.descriptors[i];
        const auto *slot = process->fd_table().entry(wait->fds[i].fd);
        if (slot && slot->used)
        {
            if (auto status = kernel.scheduler().retain_description(slot->description); !status)
                return done(error(status));
            wait->descriptions[i] = slot->description;
        }
    }
    auto ready = poll_ready(kernel, *wait);
    if (!ready) return done(error(ready.status()));
    if (ready.value() || request.immediate)
        return done(poll_output(space, *wait) ? static_cast<i64>(ready.value()) : -14);
    if (request.timed)
    {
        auto now = arch::monotonic_nanoseconds();
        if (!now) return done(error(now.status()));
        auto armed = wait->timeout.arm(now.value(), request.timeout.seconds,
                                      request.timeout.nanoseconds, request.absolute);
        if (!armed) return done(error(armed.status()));
        if (wait->timeout.complete(now.value(), armed.value()))
            return done(poll_output(space, *wait) ? 0 : -14);
    }
    auto registration = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::poll, 0);
    if (!registration)
        return done(error(registration.status()));
    wait->ticket = registration.value();
#ifdef OK_PRIVATE_POLL_MASK
    if (!poll_signal_masks.bind(*current, object, wait->ticket)) return done(-22);
#endif
    *entry = {pid, current->tid, object};
    if (process->resources().personality == Personality::linux && (wait->select_sets || wait->linux_events))
    {
        wait->syscall = poll_syscall_number(current->native_frame, wait->select_sets);
        wait->resume = current->native_frame.pc;
    }
    emit_poll_arm(*wait);
    complete = false;
    return 0;
}

i64 begin_child_wait(Kernel &kernel, sched::ProcessId pid, const ChildWaitRequest &request, bool &complete)
{ return start_child_wait(kernel, pid, request, complete); }

i64 begin_poll(Kernel &kernel, sched::ProcessId pid, const PollRequest &request, bool &complete)
{
    return begin_readiness_wait(kernel, pid, request, complete);
}
i64 begin_select(Kernel &kernel, sched::ProcessId pid, const SelectRequest &request, bool &complete)
{
    static_assert(sched::max_process_fds == 32, "Select mask must follow descriptor table growth");
    auto *process = kernel.scheduler().find(pid);
    if (!process || process->threads().empty() || kernel.scheduler().current_pid() != pid ||
        request.sets.count > sched::max_process_fds ||
        (request.sets.word_bytes != 4 && request.sets.word_bytes != 8)) return -22;
    const auto allowed = request.sets.count == 32 ? ~u32{0} : request.sets.count ? (u32{1} << request.sets.count) - 1 : 0;
    for (auto mask : request.sets.interests) if (mask & ~allowed) return -22;
    auto finish = [&](i64 result) {
        if (request.readiness.timeout_output.address)
            if (auto now = arch::monotonic_nanoseconds(); now)
                finish_poll_timeout(process->resources().native_space, request.readiness.timeout_output, now.value());
        return result;
    };
    std::array<ok_pollfd, sched::max_process_fds> descriptors{};
    usize count = 0;
    const auto selected = request.sets.interests[0] | request.sets.interests[1] | request.sets.interests[2];
    for (usize fd = 0; fd < request.sets.count; ++fd)
    {
        const auto bit = u32{1} << fd;
        if (!(selected & bit)) continue;
        const auto *slot = process->fd_table().entry(static_cast<i32>(fd));
        if (!slot || !slot->used) return finish(-9);
        descriptors[count++] = {static_cast<i32>(fd), static_cast<i16>(
            (request.sets.interests[0] & bit ? OK_POLLIN : 0) |
            (request.sets.interests[1] & bit ? OK_POLLOUT : 0)), 0};
    }
    auto readiness = request.readiness;
    readiness.descriptors = {descriptors.data(), count};
    readiness.linux_events = true;
    readiness.read_normal = readiness.write_normal = 0;
    return begin_readiness_wait(kernel, pid, readiness, complete, &request.sets);
}
Status prepare_memory()
{
#ifdef OK_KERNEL_FREESTANDING
    if (memory_preparation_attempted) return memory_preparation;
    memory_preparation_attempted = true;
    if constexpr (linker_pool_required)
    {
        memory_preparation = Status::success();
        emit("OK_NATIVE_MEMORY stage=prepare source=linker reason=unsupported-firmware-profile limit=1024\n");
    }
    else
    {
        // The supported QEMU recipes have no separately loaded objects; the OS
        // archive, extension and all NOLOAD reservations share these linker bounds.
        const uptr first = reinterpret_cast<uptr>(_kernel_start), last = reinterpret_cast<uptr>(_kernel_end);
        if constexpr (runtime_arch == arch::Architecture::arm32)
        {
            const auto &entry = boot::arm32_entry_state();
            emit("OK_ARM32_ENTRY cpsr="); emit_hex(entry.cpsr);
            emit(" sctlr="); emit_hex(entry.sctlr); emit(" mpidr="); emit_hex(entry.mpidr);
            emit(" status="); emit(boot::valid_arm32_entry(entry) ? "pass\n" : "fail\n");
            if (!boot::valid_arm32_entry(entry))
            {
                memory_preparation = Status::fault("ARM loader entry state rejected");
                return memory_preparation;
            }
        }
        boot::QemuProcessMemoryReader<runtime_arch> reader{boot::raw_boot_handoff().arguments[2]};
        const auto discovery = boot::discover_firmware_memory<runtime_arch>(boot::raw_boot_handoff(),
            boot::qemu_process_boot_protocol<runtime_arch>(),reader,firmware_workspace,firmware_memory);
        memory_preparation = discovery.status;
        if constexpr (runtime_arch == arch::Architecture::arm32)
            if (memory_preparation)
            {
                emit("OK_NATIVE_MEMORY_SOURCE protocol=arm32-linux-image fdt=");
                emit_hex(firmware_memory.source_storage.address); emit(" size=");
                emit_hex(firmware_memory.source_storage.size); emit("\n");
            }
        boot::QemuEntryMemoryPlan plan;
        if (memory_preparation && last <= first) memory_preparation = Status::fault("invalid final linker bounds");
        if (memory_preparation) memory_preparation = boot::qemu_runtime_memory_plan<runtime_arch>(
            firmware_memory,{first,last-first},{},page_count,plan);
        if (memory_preparation) memory_preparation = boot::qualify_boot_memory(firmware_memory,
            {plan.reservations.begin(),plan.reservations.size()},plan.policy,qualified_workspace,qualified_memory);
        if (memory_preparation && qualified_memory.page_count() != page_count)
            memory_preparation = Status::no_memory("insufficient qualified runtime frames");
        emit("OK_NATIVE_MEMORY stage=prepare source=firmware status=");
        emit(memory_preparation.ok() ? "pass" : "fail");
        emit(" image_begin="); emit_hex(first); emit(" image_end="); emit_hex(last);
        emit(" limit=1024\n");
        if (memory_preparation)
            for (auto range : qualified_memory.managed())
            {
                emit("OK_NATIVE_MEMORY_RANGE base="); emit_hex(range.address);
                emit(" size="); emit_hex(range.size); emit("\n");
            }
    }
    return memory_preparation;
#else
    return Status::unsupported("hosted tests do not discover firmware");
#endif
}
#if defined(OK_ENABLE_TEST_POINTS) && defined(OK_PRIVATE_FUTEX_PEER_FIXTURE)
#include "../../tests/native/private_futex_peer_fixture.hpp"
#endif
Status start(Kernel &kernel, fs::BootArchive archive)
{
#ifdef OK_KERNEL_FREESTANDING
    // Root selection and native services run on the original boot stack.
    // Bounds are a current-SP check, not faulting guards or a usage budget.
    if (auto status = arch::require_service_stack(); !status) return status;
#endif
    if (shutdown_admission_closed || native_shutdown_pending())
        return Status::busy("native shutdown has closed runtime admission");
    if (started || original_shutdown_owner.runtime)
        return Status::already_exists("native runtime already bound its original init");
    if (!runtime_failure) return runtime_failure;
    if (auto status = linux_udp_receive_start_status(); !status) return status;
    if (!hw::supported())
        return Status::unsupported("native execution backend is unavailable");
    if (!archive.data || !archive.initial_program)
        return Status::invalid_argument("missing boot archive");
    if (auto status = fs::mount_archive(kernel.vfs(), {archive.data, archive.size}); !status.ok())
        return status;
    if (ok_boot_filesystem != nullptr)
        if (auto status = ok_boot_filesystem(kernel.vfs()); !status.ok())
            return status;
    if (ok_boot_entropy != nullptr)
        if (auto status = ok_boot_entropy(kernel.posix()); !status.ok())
            return status;
    // Root selection can cover the directory retained by early diagnostics.
    // Kernel GUI/services must start in the selected root as well as /init.
    if (auto status = kernel.posix().activate(0); !status.ok())
        return status;
    if (auto status = kernel.posix().chdir("/"); !status.ok())
        return status;
    if (auto status = hw::initialize(); !status.ok())
        return status;
    // Allocation becomes possible only after supervisor mappings are established.
    if (auto status = activate_pool(); !status) return status;
    if (auto status = shared_files.bind(page_pool); !status) return status;
    if (auto status = kernel.scheduler().set_shared_memory_files(shared_files); !status) return status;
    if (auto status = entry_stacks.activate(entry_stack_arena); !status) return status;
    emit_pool_stats();
    emit_entry_stack_stats();
#ifdef OK_ENABLE_TEST_POINTS
    if (auto status = check_runtime_objects(); !status) return status;
    emit_pool_stats();
#endif
    if (auto status = arch::initialize_clock(); !status.ok())
        return status;
    // A missing RTC does not prevent boot. The first anchor attempt is terminal
    // for this boot, and fork/exec never repeat it or substitute monotonic time.
    const auto realtime_status = arch::initialize_realtime();
    const auto realtime_info = arch::realtime_info();
    emit("OK_REALTIME scope=runtime state="); emit_number(static_cast<u32>(realtime_info.state));
    emit(" source="); emit_number(static_cast<u32>(realtime_info.source));
    emit(" source_resolution_ns="); emit_number(realtime_info.source_resolution_ns);
    emit(" acquisition_span_ns="); emit_number(realtime_info.acquisition_span_ns);
    emit(" accuracy=unknown available="); emit_number(realtime_status ? 1 : 0); emit("\n");
    Arguments arguments;
    static_cast<void>(arguments.argv.push_back(FixedString<256>(archive.initial_program)));
    static_cast<void>(arguments.envp.push_back(FixedString<256>("PATH=/bin")));
    const auto initial_credentials = archive.initial_credentials;
    if (initial_credentials.kernel_space || initial_credentials.uid != initial_credentials.euid ||
        initial_credentials.gid != initial_credentials.egid ||
        initial_credentials.suid != initial_credentials.euid ||
        initial_credentials.sgid != initial_credentials.egid)
        return Status::invalid_argument("initial user identity requires equal unprivileged IDs");
    if (auto status = kernel.posix().set_credentials(initial_credentials); !status)
        return status;
    auto image = load(kernel, archive.initial_program, arguments);
    const auto restored = kernel.posix().set_credentials(user::root_credentials());
    if (!restored)
    {
        return restored;
    }
    if (!image)
        return image.status();
    auto context = kernel.arch().make_user_context({image.value().frame.pc, image.value().frame.sp});
    auto pid = kernel.scheduler().create_init_process(context);
    if (!pid)
    {
        return pid.status();
    }
    auto *process = kernel.scheduler().find(pid.value());
    process->set_credentials(initial_credentials);
    if (auto status = prepare_native_thread(process->threads()[0]); !status)
    {
        finish(kernel, pid.value(), 127);
        return status;
    }
    if (auto status = install(*process, image.value()); !status)
    {
        finish(kernel, pid.value(), 127);
        return status;
    }
#ifdef OK_PRIVATE_POLL_MASK
    if (!private_signal_process(kernel, pid.value()))
    {
        finish(kernel, pid.value(), 127);
        return Status::no_memory("private process signal owner capacity");
    }
#endif
    static_cast<void>(kernel.scheduler().set_runnable(pid.value()));
#if defined(OK_ENABLE_TEST_POINTS) && defined(OK_PRIVATE_FUTEX_PEER_FIXTURE)
    if (auto status = setup_peer_fixture(kernel, pid.value()); !status) return status;
#endif
    started = true;
    runtime_kernel = &kernel;
    // This static singleton has one boot lifetime and no reconstruction/reset API.
    original_shutdown_owner = {&original_shutdown_owner, 1, pid.value(), process->threads()[0].tid};
    emit("OK_NATIVE scope=hardware init=loaded\n");
    return Status::success();
}
bool step(Kernel &kernel)
{
    if (shutdown_admission_closed || native_shutdown_pending()) return false;
    if (runtime_step_active)
    { runtime_failure = Status::busy("native step is not reentrant"); started = false; return false; }
    RuntimeStepGuard step_guard;
#ifdef OK_KERNEL_FREESTANDING
    if (auto status = arch::require_service_stack(); !status)
    { runtime_failure = status; started = false; return false; }
#endif
    // Retry retained owner/pin cleanup on the serialized boot-CPU service path.
    // A failed retry remains discoverable and cannot consume a discharged edge.
    if (auto retired = retry_quarantined_retirement(); !retired) runtime_failure = retired;
    if (auto retired = shared_files.retry_retirement(); !retired) runtime_failure = retired;
    // The outer loop also handles GUI/services when no native task is ready.
    if (!runtime_failure) { started = false; return false; }
    if (!kernel.posix().activate(0))
        return false;
    if (!started)
        return false;
#ifdef OK_PRIVATE_POLL_MASK
    private_process_signal_fatal(kernel);
    if (!runtime_failure) { started = false; return false; }
#endif
#if defined(OK_ENABLE_TEST_POINTS) && defined(OK_PRIVATE_FUTEX_PEER_FIXTURE)
    if (auto status = checkpoint_peer_fixture(kernel); !status)
    { runtime_failure = status; started = false; return false; }
    if (!started) return false;
#endif
    auto now = arch::monotonic_nanoseconds();
    if (!now)
    {
        runtime_failure = now.status();
        started = false;
        emit("OK_TEST_FAIL native-clock=unavailable\n");
        return false;
    }
    kernel.posix().set_monotonic_time(now.value(), arch::clock_resolution_ns());
    if (auto status = prune_departed_linux_udp_receives(kernel); !status)
    { runtime_failure = status; started = false; return false; }
#ifdef OK_PRIVATE_POLL_MASK
    if (!private_child_reap(kernel)) return false;
#else
    static_cast<void>(kernel.scheduler().reap_orphans());
#endif
    if (!runtime_failure) { started = false; return false; }
    if (auto status = scan_native_udp_receives(kernel); !status)
    { runtime_failure = status; started = false; return false; }
    if (auto status = scan_linux_udp_receives(kernel); !status)
    { runtime_failure = status; started = false; return false; }
    prune_pipe_reads(kernel);
    prune_pipe_writes(kernel);
    prune_pty_waits(kernel);
    prune_child_waits(kernel);
    if (!runtime_failure) { started = false; return false; }
    bool waiting_deadline = expire_futex(kernel, now.value());
#ifdef OK_PRIVATE_POLL_MASK
    waiting_deadline |= private_signal_wait_pending(kernel);
#endif
    for (const auto &entry : poll_waits)
    {
        if (entry.pid == 0)
            continue;
        const auto object = entry.object;
        auto resolved = poll_objects.resolve(object);
        if (!resolved)
        {
            runtime_failure = resolved.status();
            started = false;
            return false;
        }
        auto &wait = *resolved.value();
        auto *candidate = kernel.scheduler().find(wait.pid);
        auto *owner = kernel.scheduler().find_thread(wait.pid, wait.tid);
        if (candidate == nullptr || owner == nullptr || !owner->native_active ||
            !owner->wait_registration.pending(wait.ticket) ||
            owner->wait_reason != sched::ThreadWaitReason::poll ||
            candidate->state() == sched::ProcessState::zombie || candidate->state() == sched::ProcessState::exited)
        {
            cancel_poll(kernel, object);
            continue;
        }
        auto ready = poll_ready(kernel, wait);
        const bool expired = wait.timeout.complete(now.value(), wait.timeout.generation());
        if (!ready || ready.value() != 0 || expired)
        {
            const auto cause = ready && ready.value() == 0 && expired
                ? sched::WaitCompletion::timeout : sched::WaitCompletion::ready;
            if (!kernel.scheduler().complete_wait(wait.ticket, cause))
            {
                cancel_poll(kernel, object);
                continue;
            }
            i64 result = ready ? static_cast<i64>(ready.value()) : error(ready.status());
            if (ready && !poll_output(candidate->resources().native_space, wait))
                result = -14;
            finish_poll_timeout(candidate->resources().native_space, wait.timeout_output, now.value());
            auto &thread = *owner;
            thread.syscall_pending = false;
            if (auto status = set_result(*candidate, thread.native_frame, result); !status.ok())
            {
                runtime_failure = status;
                finish(kernel, candidate->pid(), 127);
                continue;
            }
            cancel_poll(kernel, object, cause == sched::WaitCompletion::timeout ? "timeout" : "ready", result);
        }
        else
            waiting_deadline = true;
    }
    for (auto &candidate : kernel.scheduler().processes())
        for (auto &thread : candidate.threads())
            if (thread.native_active && thread.native_deadline.armed())
            {
                if (thread.wait_reason == sched::ThreadWaitReason::deadline &&
                    thread.native_deadline.complete(now.value(), thread.wait_key) &&
                    kernel.scheduler().complete_wait(thread.wait_registration.ticket(), sched::WaitCompletion::timeout))
                {
                    thread.syscall_pending = false;
                    if (auto status = set_result(candidate, thread.native_frame, 0); !status.ok())
                    {
                        runtime_failure = status;
                        finish(kernel, candidate.pid(), 127);
                        continue;
                    }
                }
                else
                    waiting_deadline = true;
            }
    if (!runtime_failure || !started) { started = false; return false; }
    struct Candidate { sched::ProcessId pid{}; sched::ThreadId tid{}; };
    auto before = [](Candidate left, Candidate right) {
        return left.pid < right.pid || (left.pid == right.pid && left.tid < right.tid);
    };
    Candidate chosen{}, first{};
    for (const auto &candidate : kernel.scheduler().processes())
    {
        if (candidate.state() != sched::ProcessState::runnable && candidate.state() != sched::ProcessState::running &&
            candidate.state() != sched::ProcessState::blocked)
            continue;
        for (const auto &thread : candidate.threads())
        {
            bool interruptible = false;
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64) || defined(__mips__) || defined(__powerpc__) || defined(__PPC__))
            if (linux_udp_receive_object(candidate.pid(), thread.tid).pool)
                interruptible = private_linux_udp_signal_ready(kernel, candidate, thread);
            else
                interruptible = private_signal_wait_ready(kernel, candidate, thread, now.value()) ||
                            private_sleep_signal_ready(kernel, candidate, thread) ||
                            private_pipe_signal_ready(kernel, candidate, thread) ||
                            private_pipe_write_signal_ready(kernel, candidate, thread) ||
                            private_futex_signal_ready(kernel, candidate, thread) ||
                            private_poll_signal_ready(kernel, candidate, thread) ||
                            private_child_signal_ready(kernel, candidate, thread);
#endif
            if (!thread.native_active ||
                (thread.state != sched::ProcessState::runnable && thread.state != sched::ProcessState::running && !interruptible))
                continue;
            Candidate entry{candidate.pid(), thread.tid};
            if (!first.pid || before(entry, first)) first = entry;
            if (before({last_pid, last_tid}, entry) && (!chosen.pid || before(entry, chosen))) chosen = entry;
        }
    }
    if (!runtime_failure || !started) { started = false; return false; }
    if (!chosen.pid)
        chosen = first;
    if (!chosen.pid)
        return waiting_deadline;
    last_pid = chosen.pid;
    last_tid = chosen.tid;
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64) || defined(__mips__) || defined(__powerpc__) || defined(__PPC__))
    if (auto *selected = kernel.scheduler().find_thread(chosen.pid, chosen.tid); selected)
    {
        auto *owner = kernel.scheduler().find(chosen.pid);
        if (!owner) return false;
        // Also inspect runnable continuations whose readiness wake lost its
        // data to a peer. A wake alone has not completed the original read.
        if (linux_udp_receive_object(chosen.pid, chosen.tid).pool)
        {
            if (private_linux_udp_signal_ready(kernel, *owner, *selected) &&
                !private_linux_udp_signal_complete(kernel, *owner, *selected)) return runtime_failure.ok();
        }
        else if (private_signal_wait_ready(kernel, *owner, *selected, now.value()))
        {
            if (!private_signal_wait_complete(kernel, *owner, *selected, now.value())) return runtime_failure.ok();
        }
        else if (private_pipe_signal_ready(kernel, *owner, *selected))
        {
            if (!private_pipe_signal_complete(kernel, *owner, *selected)) return runtime_failure.ok();
        }
        else if (private_pipe_write_signal_ready(kernel, *owner, *selected))
        {
            if (!private_pipe_write_signal_complete(kernel, *owner, *selected)) return runtime_failure.ok();
        }
        else if (private_child_signal_ready(kernel, *owner, *selected))
        {
            if (!private_child_signal_complete(kernel, *owner, *selected)) return runtime_failure.ok();
        }
        else if (private_futex_signal_ready(kernel, *owner, *selected))
        {
            if (!private_futex_signal_complete(kernel, *owner, *selected)) return runtime_failure.ok();
        }
        else if (private_poll_signal_ready(kernel, *owner, *selected))
        {
            if (!private_poll_signal_complete(kernel, *owner, *selected)) return runtime_failure.ok();
        }
        else if (selected->state == sched::ProcessState::blocked &&
                 !private_sleep_signal_complete(kernel, *owner, *selected)) return runtime_failure.ok();
    }
#endif
    if (!kernel.scheduler().activate_thread(chosen.pid, chosen.tid))
        return false;
    auto *process = kernel.scheduler().find(chosen.pid);
    auto &thread = *kernel.scheduler().current_thread();
    if (linux_udp_receive_object(chosen.pid, chosen.tid).pool && !thread.syscall_pending)
    {
        static_cast<void>(linux_udp_receive_invariant(
            Status::fault("Linux UDP receive owner lost its pending syscall before entry")));
        return false;
    }
    bool linux_udp_delay_slot = false;
    if (!thread.syscall_pending)
    {
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64) || defined(__mips__) || defined(__powerpc__) || defined(__PPC__))
        if (!private_signal_deliver(kernel, *process, thread))
        {
            static_cast<void>(kernel.posix().activate(0));
            return true;
        }
#endif
        if (thread.tls.set_tid_address)
        {
            const uptr destination = thread.tls.set_tid_address;
            thread.tls.set_tid_address = 0;
            std::array<u8, 4> bytes{};
            constexpr bool big_endian = lu::Codec<lu::configured_native_profile()>::profile.big_endian;
            for (usize i = 0; i < bytes.size(); ++i)
                bytes[i] = static_cast<u8>(thread.tid >> (8 * (big_endian ? 3 - i : i)));
            static_cast<void>(hw::copy_to_user(process->resources().native_space, destination, bytes.data(), bytes.size()));
        }
        if (auto status = validate_entry_stack(entry_stacks, thread); !status)
        { runtime_failure = status; started = false; return false; }
        auto active = entry_stacks.activate_entry(thread.entry_stack, {chosen.pid, chosen.tid}, 0);
        if (!active) { runtime_failure = active.status(); started = false; return false; }
        const auto view = active.value();
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
        if (!private_n64_begin(*process, thread, view))
        {
            static_cast<void>(entry_stacks.deactivate_entry(thread.entry_stack, {chosen.pid, chosen.tid}, 0));
            runtime_failure = Status::fault("private n64 entry readiness missing");
            started = false;
            return false;
        }
#endif
#ifdef OK_PRIVATE_MIPS_O32_SIGNAL
        if (!private_mips_begin(thread))
        {
            static_cast<void>(entry_stacks.deactivate_entry(thread.entry_stack, {chosen.pid, chosen.tid}, 0));
            runtime_failure = Status::fault("private MIPS entry readiness missing");
            started = false;
            return false;
        }
#endif
#ifdef OK_PRIVATE_PPC32_SIGNAL
        if (process->resources().personality == Personality::linux && !private_ppc_begin(*process, thread, view))
        {
            static_cast<void>(entry_stacks.deactivate_entry(thread.entry_stack, {chosen.pid, chosen.tid}, 0));
            runtime_failure = Status::fault("private PPC entry readiness missing");
            started = false; return false;
        }
#endif
        auto trap = shared_capture.enter(syscall_origins, vfs_continuations, *process, thread, view, kernel.scheduler());
        const auto capture = trap ? trap.value().kernel_stack_pointer : 0;
        const bool captured = capture != 0 && view.contains(capture);
        const bool needs_capture = trap && (trap.value().kind == hw::TrapKind::syscall ||
                                           trap.value().kind == hw::TrapKind::timer);
        const bool bad_capture = (capture && !captured) || (needs_capture && !captured);
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
        // The observation expires with view deactivation. Consume it first.
        const bool n64_capture_valid = !needs_capture || (!bad_capture && private_n64_capture(*process, thread, view));
        if (!needs_capture || !n64_capture_valid) private_n64_revoke(thread);
#endif
#ifdef OK_PRIVATE_PPC32_SIGNAL
        const bool ppc_linux = process->resources().personality == Personality::linux;
        const bool ppc_capture_valid = !ppc_linux || (trap && !bad_capture && private_ppc_capture(*process, thread, view));
        if (ppc_linux && !ppc_capture_valid) private_ppc_revoke(thread);
#endif
        // Backends have restored the service stack and detached hardware/global
        // pointers before returning. Never dispatch or free an active lease.
        const auto deactivated = entry_stacks.deactivate_entry(thread.entry_stack, {chosen.pid, chosen.tid}, 0);
        if (!deactivated || bad_capture)
        {
            syscall_origins.revoke(thread);
            vfs_continuations.revoke(thread);
            runtime_failure = !deactivated ? deactivated : Status::fault("missing owned privileged capture");
            started = false;
            emit("OK_TEST_FAIL native-entry-stack=ownership\n");
            return false;
        }
#ifdef OK_PRIVATE_PPC32_SIGNAL
        if (!ppc_capture_valid)
        {
            syscall_origins.revoke(thread);
            vfs_continuations.revoke(thread);
            runtime_failure = Status::fault("private PPC capture readiness missing");
            started = false; return false;
        }
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
        if (!n64_capture_valid)
        {
            syscall_origins.revoke(thread);
            vfs_continuations.revoke(thread);
            runtime_failure = Status::fault("private n64 capture readiness missing");
            started = false;
            return false;
        }
#endif
#ifdef OK_PRIVATE_MIPS_O32_SIGNAL
        if (needs_capture)
        {
            if (!private_mips_capture(thread))
            {
                syscall_origins.revoke(thread);
            vfs_continuations.revoke(thread);
                runtime_failure = Status::fault("private MIPS capture readiness missing");
                started = false;
                return false;
            }
        }
        else private_mips_revoke(thread);
#endif
        if (captured)
        {
            if (trap.value().kind == hw::TrapKind::syscall) ++entry_syscalls;
            else if (trap.value().kind == hw::TrapKind::timer) ++entry_timers;
            else ++entry_faults;
        }
        else ++entry_rejections;
        if (!trap)
        {
            finish(kernel, chosen.pid, 127);
            emit("OK_NATIVE scope=hardware enter=failed\n");
        }
        else if (trap.value().kind == hw::TrapKind::syscall)
        {
#if defined(OK_ENABLE_TEST_POINTS) && defined(OK_PRIVATE_FUTEX_PEER_FIXTURE)
            if (auto status = record_peer_trap(kernel, chosen.pid, chosen.tid, thread.native_frame); !status)
            { runtime_failure = status; started = false; return false; }
#endif
            linux_udp_delay_slot = linux_udp_delay_slot_excluded<lu::configured_native_profile()>(trap.value());
            thread.syscall_pending = true;
        }
        else if (trap.value().kind == hw::TrapKind::timer)
        {
            ++thread.cpu_time_ticks;
            static bool reported = false;
            if (!reported)
            {
                emit("OK_NATIVE scope=hardware timer=preempted\n");
                reported = true;
            }
        }
        else
        {
            emit("OK_NATIVE scope=hardware fault=contained\n");
            finish(kernel, chosen.pid, 0, 11);
        }
    }
    if (thread.native_active && thread.syscall_pending)
    {
        bool complete = true;
        i64 result{};
#ifdef OK_PRIVATE_PPC32_SIGNAL
        sched::ThreadId ppc_prepared_clone{};
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
        sched::ThreadId n64_prepared_clone{};
#endif
        // Captured Linux ownership precedes signal-call/frame decoding and all
        // synchronous continuations. Resume never rereads syscall arguments.
        if (const auto object = linux_udp_receive_object(chosen.pid, chosen.tid); object.pool)
        {
            complete = false;
            if (auto status = resume_linux_udp_receive(kernel, *process, thread, object); !status)
                static_cast<void>(linux_udp_receive_invariant(status));
        }
        else
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64) || defined(__mips__) || defined(__powerpc__) || defined(__PPC__))
        if (!private_signal_call(kernel, *process, thread, complete, result))
#endif
        {
        if (const auto object = native_udp_receive_object(chosen.pid, chosen.tid); object.pool)
            result = resume_native_udp_receive(kernel, *process, object, complete);
        else if (const auto object = pty_wait_object(chosen.pid, chosen.tid); object.pool)
            result = resume_pty_io(kernel, *process, object, complete);
        else
        if (const auto object = child_wait_object(chosen.pid, chosen.tid); object.pool)
            result = resume_child_wait(kernel, *process, object, complete);
        else if (const auto object = pipe_read_object(chosen.pid, chosen.tid); object.pool)
            result = resume_pipe_read(kernel, *process, object, complete);
        else if (const auto object = pipe_write_object(chosen.pid, chosen.tid); object.pool)
            result = resume_pipe_write(kernel, *process, object, complete);
        else if ((process->resources().personality == Personality::native &&
                      native_unix_path_call(hw::syscall(thread.native_frame), kernel.scheduler(), *process) != lu::UnixPathCall::none) ||
                 (process->resources().personality == Personality::linux &&
                      linux_unix_path_call<lu::configured_native_profile()>(thread.native_frame, kernel.scheduler(), *process) != lu::UnixPathCall::none))
        {
            if (linux_udp_delay_slot) result = -95;
            else result = process->resources().personality == Personality::linux
                ? linux_bootstrap::dispatch<lu::configured_native_profile()>(kernel, chosen.pid, thread.native_frame, complete,
                    [&](hw::Syscall call, bool &done) { return dispatch(kernel, chosen.pid, call, done); })
                : dispatch(kernel, chosen.pid, hw::syscall(thread.native_frame), complete);
        }
        else if (syscall_origins.has_tcp_immediate(thread) || tcp_immediate_frame(kernel, *process, thread))
        {
            if (linux_udp_delay_slot) result = -95;
            else
            {
                complete = false; result = 0;
                if (const auto status = tcp_immediate_runtime.dispatch(kernel, *process, thread); !status)
                { runtime_failure = status; started = false; }
            }
        }
        else if (process->resources().personality == Personality::linux && linux_udp_frame(thread.native_frame))
        {
            if (linux_udp_delay_slot)
            {
                // BD has a recognized backend continuation but no immediate IO
                // capture. Complete only this unsupported call, with no UDP effects.
                result = -95; // Generic EOPNOTSUPP; bootstrap maps target errno.
            }
            else
            {
                // Publish once while live. Generic completion must not run
                // after access reset or final original-OFD release.
                complete = false;
                result = 0;
                if (const auto status = dispatch_linux_udp(kernel, *process, thread); !status)
                    static_cast<void>(linux_udp_receive_invariant(status));
            }
        }
        else
        result = process->resources().personality == Personality::linux
            ? linux_bootstrap::dispatch<lu::configured_native_profile()>(kernel, chosen.pid, thread.native_frame, complete,
                [&](hw::Syscall call, bool &done) { return dispatch(kernel, chosen.pid, call, done); },
                [&](sched::ThreadControlBlock &created) {
                    const auto prepared = prepare_native_thread(created);
                    if (!prepared) return prepared;
#ifdef OK_PRIVATE_PPC32_SIGNAL
                    if (!private_ppc_inherit(*process, thread, *process, created, true) ||
                        !private_ppc_state(*process, created)->stage_relocation(private_ppc_identity(*process, created),
                            created.native_frame, private_ppc_backend::capability()))
                    {
                        const auto retired = retire_native_thread(created);
                        if (!retired && created.entry_stack.pool)
                        {
                            stranded_entry_stacks[created.entry_stack.slot] = {{created.owner, created.tid}, created.entry_stack};
                            created.entry_stack = {};
                            created.kernel_stack_top = 0;
                            runtime_failure = retired;
                            started = false;
                        }
                        return Status::fault("private PPC clone signal context inheritance failed");
                    }
                    ppc_prepared_clone = created.tid;
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
                    if (!private_n64_inherit(*process, thread, *process, created) ||
                        !private_n64_state(*process, created)->stage_relocation(private_n64_identity(*process, created),
                            created.native_frame, private_n64_backend::capability()))
                    {
                        const auto retired = retire_native_thread(created);
                        if (!retired && created.entry_stack.pool)
                        {
                            stranded_entry_stacks[created.entry_stack.slot] = {{created.owner, created.tid}, created.entry_stack};
                            created.entry_stack = {};
                            created.kernel_stack_top = 0;
                            runtime_failure = retired;
                            started = false;
                        }
                        return Status::fault("private n64 clone signal context inheritance failed");
                    }
                    n64_prepared_clone = created.tid;
#endif
#ifdef OK_PRIVATE_MIPS_O32_SIGNAL
                    if (!private_mips_inherit(thread, created))
                    {
                        // The scheduler has not published this child yet.
                        const auto retired = retire_native_thread(created);
                        if (!retired && created.entry_stack.pool)
                        {
                            stranded_entry_stacks[created.entry_stack.slot] = {{created.owner, created.tid}, created.entry_stack};
                            created.entry_stack = {};
                            created.kernel_stack_top = 0;
                            runtime_failure = retired;
                            started = false;
                        }
                        return Status::fault("private MIPS clone signal context inheritance failed");
                    }
#endif
                    return Status::success();
                })
            : dispatch(kernel, chosen.pid, hw::syscall(thread.native_frame), complete);
        }
        process = kernel.scheduler().find(chosen.pid);
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
        if (n64_prepared_clone)
        {
            auto *child = kernel.scheduler().find_thread(chosen.pid, n64_prepared_clone);
            auto *state = process && child ? private_n64_state(*process, *child) : nullptr;
            if (result != static_cast<i64>(n64_prepared_clone) || !state ||
                !state->commit_relocation(private_n64_identity(*process, *child), child->native_frame,
                                          private_n64_backend::capability()))
            {
                runtime_failure = Status::fault("private n64 clone canonical publication failed");
                started = false; return false;
            }
        }
#endif
#ifdef OK_PRIVATE_PPC32_SIGNAL
        if (ppc_prepared_clone)
        {
            auto *child = kernel.scheduler().find_thread(chosen.pid, ppc_prepared_clone);
            auto *state = process && child ? private_ppc_state(*process, *child) : nullptr;
            if (result != static_cast<i64>(ppc_prepared_clone) || !state ||
                !state->commit_relocation(private_ppc_identity(*process, *child), child->native_frame,
                                          private_ppc_backend::capability()))
            {
                runtime_failure = Status::fault("private PPC clone canonical publication failed");
                started = false; return false;
            }
        }
#endif
        auto *resumed = kernel.scheduler().find_thread(chosen.pid, chosen.tid);
        if (process && resumed && resumed->native_active && complete)
        {
            auto &current = *resumed;
            current.syscall_pending = false;
            current.console_wait_epoch = 0;
            if (auto status = set_result(*process, current.native_frame, result); !status.ok())
            {
                runtime_failure = status;
                finish(kernel, chosen.pid, 127);
            }
        }
    }
    static_cast<void>(kernel.posix().activate(0));
    // The committing syscall's result is published before leaving all step locals.
    if (native_shutdown_pending()) return false;
    if (!runtime_failure || !started) { started = false; return false; }
    if (auto status = prune_departed_linux_udp_receives(kernel); !status)
    { runtime_failure = status; started = false; return false; }
#ifdef OK_PRIVATE_POLL_MASK
    if (!private_child_reap(kernel)) return false;
#else
    static_cast<void>(kernel.scheduler().reap_orphans());
#endif
    if (!runtime_failure) { started = false; return false; }
    return true;
}
#if defined(OK_NATIVE_SIGNAL_RUNTIME)
Status private_signal_before_process_compaction(sched::SchedulerState &scheduler)
{
    if (runtime_kernel && &runtime_kernel->scheduler() == &scheduler)
        if (auto status = prune_departed_linux_udp_receives(*runtime_kernel); !status) return status;
#ifdef OK_PRIVATE_PPC32_SIGNAL
    if (runtime_kernel && &runtime_kernel->scheduler() == &scheduler &&
        !private_ppc_stage_relocation(*runtime_kernel))
    {
        private_ppc_cancel_transactions();
        runtime_failure = Status::fault("private PPC process compaction stage failed");
        started = false;
        return runtime_failure;
    }
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
    if (runtime_kernel && &runtime_kernel->scheduler() == &scheduler &&
        !private_n64_stage_relocation(*runtime_kernel))
    {
        runtime_failure = Status::fault("private n64 process compaction preflight failed");
        return runtime_failure;
    }
#else
    static_cast<void>(scheduler);
#endif
    return Status::success();
}
Status private_signal_after_process_compaction(sched::SchedulerState &scheduler)
{
#ifdef OK_PRIVATE_PPC32_SIGNAL
    if (runtime_kernel && &runtime_kernel->scheduler() == &scheduler &&
        !private_ppc_commit_relocation(*runtime_kernel))
    {
        private_ppc_cancel_transactions();
        runtime_failure = Status::fault("private PPC process compaction commit failed");
        started = false;
        return runtime_failure;
    }
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
    if (runtime_kernel && &runtime_kernel->scheduler() == &scheduler &&
        !private_n64_commit_relocation(*runtime_kernel))
    {
        runtime_failure = Status::fault("private n64 process compaction commit failed");
        return runtime_failure;
    }
#else
    static_cast<void>(scheduler);
#endif
    return Status::success();
}
#endif
namespace
{
Status hold_shutdown_failure(Status status)
{
    if (!status && shutdown_failure) shutdown_failure = status;
    return shutdown_failure;
}
Status shutdown_runtime_owner(Kernel &kernel)
{
    if (runtime_kernel != &kernel || !original_shutdown_owner.runtime)
        return Status::not_initialized("native shutdown has no original runtime owner");
    if (!shutdown_failure) return shutdown_failure;
    if (!runtime_failure) return runtime_failure;
    return {};
}
Status original_init_state(Kernel &kernel)
{
    const auto *process = kernel.scheduler().find(original_shutdown_owner.process);
    const auto *leader = kernel.scheduler().find_thread(original_shutdown_owner.process, original_shutdown_owner.leader);
    if (!process || !leader || process->pid() != 1 || process->parent_pid() ||
        process->execution() != sched::ProcessExecution::user_process ||
        process->resources().personality != Personality::native ||
        process->state() == sched::ProcessState::created || process->state() == sched::ProcessState::zombie ||
        process->state() == sched::ProcessState::exited || leader->state == sched::ProcessState::exited ||
        leader->owner != process->pid() || !leader->native_active || !process->resources().native_space.root ||
        process->resources().native_pages.empty())
        return Status::denied("shutdown original init is not a live native process");
    const auto &ids = process->credentials();
    if (ids.kernel_space || ids.uid || ids.euid || ids.suid || ids.gid || ids.egid || ids.sgid)
        return Status::denied("shutdown original init requires all six root identities");
    usize live{};
    for (const auto &thread : process->threads())
        live += thread.state != sched::ProcessState::exited;
    if (live != 1 || !leader->entry_stack.pool || !leader->kernel_stack_top)
        return Status::denied("shutdown original init must retain its single leader");
    return validate_entry_stack(entry_stacks, *leader);
}
Status shutdown_quiesced_groups(Kernel &kernel, ShutdownOwner owner)
{
    const auto *init = kernel.scheduler().find(owner.process);
    if (!init || !init->children().empty())
        return Status::busy("native shutdown requires original init to reap every child");
    for (const auto &process : kernel.scheduler().processes())
    {
        if (process.execution() != sched::ProcessExecution::user_process || process.pid() == owner.process) continue;
        if (process.parent_pid() == owner.process)
            return Status::busy("native shutdown retains an unresolved original-init child");
        if (process.state() != sched::ProcessState::zombie && process.state() != sched::ProcessState::exited)
            return Status::busy("native shutdown requires quiesced user child groups");
        if (process.resources().native_space.root || !process.resources().native_pages.empty())
            return Status::busy("completed user child still retains native VM ownership");
        for (const auto &thread : process.threads())
            if (thread.native_active || thread.entry_stack.pool || thread.kernel_stack_top || thread.syscall_pending ||
                thread.wait_registration.active() || thread.native_deadline.armed())
                return Status::busy("completed user child still retains native continuation ownership");
    }
    return {};
}
Status native_retirement_empty(const NativeShutdownReport &report)
{
    if (!report.failure) return report.failure;
    if (report.step_active || report.live_user_groups || report.native_threads || report.address_spaces ||
        report.mapping_pages || report.backend_spaces || report.active_entries || report.leased_entries ||
        report.quarantined_entries || report.stranded_entries || report.quarantined_spaces ||
        report.quarantined_pins || report.wait_members || report.signal_owners || report.signal_waits ||
        report.origin_records || report.origin_borrows || report.origin_pending || report.vfs_records ||
        report.vfs_pending || report.vfs_captures || report.live_objects || report.object_pages || report.object_quarantine)
        return Status::busy("native shutdown retains an execution, continuation or VM owner");
    return {};
}
} // namespace
Result<ShutdownOwner> shutdown_owner(Kernel &kernel)
{
    if (runtime_kernel != &kernel || !original_shutdown_owner.runtime)
        return Status::not_initialized("native runtime has not bound original init");
    return original_shutdown_owner;
}
Result<ShutdownCaller> shutdown_caller(Kernel &kernel)
{
    if (auto status = shutdown_runtime_owner(kernel); !status) return status;
    if (!started || shutdown_admission_closed)
        return Status::busy("native shutdown caller admission is closed");
    if (auto status = original_init_state(kernel); !status) return status;
    const auto *thread = kernel.scheduler().current_thread();
    const auto *process = kernel.scheduler().find(kernel.scheduler().current_pid());
    if (!process || !thread || process->pid() != original_shutdown_owner.process ||
        thread != kernel.scheduler().find_thread(process->pid(), original_shutdown_owner.leader) ||
        thread->state != sched::ProcessState::running || process->state() != sched::ProcessState::running)
        return Status::denied("shutdown caller is not the canonical running original init leader");
    const auto &ids = process->credentials();
    return ShutdownCaller{original_shutdown_owner, thread->tid, 1, true,
                          {ids.uid, ids.euid, ids.suid, ids.gid, ids.egid, ids.sgid}};
}
Status shutdown_preflight(Kernel &kernel, ShutdownOwner owner)
{
    if (auto status = shutdown_runtime_owner(kernel); !status) return status;
    if (owner != original_shutdown_owner)
        return Status::denied("native shutdown owner is foreign or stale");
    if (!started || shutdown_admission_closed)
        return Status::busy("native shutdown runtime is not accepting a reservation");
    if (auto status = original_init_state(kernel); !status) return status;
    if (entry_stacks.accounting().active_slots)
        return Status::busy("native shutdown reservation has an active entry");
    // Service children remain admissible until the OS has quiesced them.
    const auto report = shutdown_report(kernel);
    if (!report.failure) return report.failure;
    if (report.backend_spaces != report.address_spaces)
        return Status::fault("native shutdown found an unpublished backend address-space owner");
    if (report.quarantined_entries || report.stranded_entries || report.quarantined_spaces ||
        report.quarantined_pins || report.object_quarantine || report.quarantined_pages)
        return Status::busy("native shutdown has unresolved quarantined ownership");
    if (report.origin_borrows || report.origin_pending)
        return Status::busy("native shutdown has borrowed or deferred origin retirement");
    return {};
}
Status shutdown_check_commit(Kernel &kernel, ShutdownOwner owner)
{
    if (auto status = shutdown_preflight(kernel, owner); !status) return status;
    if (auto status = shutdown_quiesced_groups(kernel, owner); !status) return status;
    if (auto status = kernel.scheduler().shutdown_console_status(); !status) return status;
    if (auto status = kernel.scheduler().shutdown_endpoint_status(); !status) return status;
    return kernel.vfs().socket_path_reference_count()
        ? Status::busy("native shutdown retains a socket path reference") : Status{};
}
Status shutdown_outer_boundary(Kernel &kernel)
{
    if (auto status = shutdown_runtime_owner(kernel); !status) return status;
#ifdef OK_KERNEL_FREESTANDING
    if (auto status = arch::require_service_stack(); !status) return status;
#endif
    if (runtime_step_active || entry_stacks.accounting().active_slots)
        return Status::busy("native shutdown requires return from the complete runtime step");
    const auto backend = hw::ownership_report();
    if (!backend.failure) return backend.failure;
    return backend.executing ? Status::busy("native backend still owns an active execution frame") : Status{};
}
Status shutdown_close_admission(Kernel &kernel)
{
    if (auto status = shutdown_outer_boundary(kernel); !status) return status;
    if (shutdown_admission_closed) return Status::already_exists("native shutdown admission was already consumed");
    shutdown_admission_closed = true;
    return {};
}
NativeShutdownReport shutdown_report(Kernel &kernel)
{
    NativeShutdownReport result{};
    auto retain_failure = [&](Status status) { if (result.failure && !status) result.failure = status; };
    retain_failure(shutdown_runtime_owner(kernel));
    result.admission_closed = shutdown_admission_closed;
    result.step_active = runtime_step_active;
    result.retirement_started = shutdown_retirement_started;
    result.retirement_completed = shutdown_retirement_completed;
    for (const auto &process : kernel.scheduler().processes())
    {
        if (process.execution() != sched::ProcessExecution::user_process) continue;
        result.live_user_groups += process.state() != sched::ProcessState::zombie && process.state() != sched::ProcessState::exited;
        result.address_spaces += process.resources().native_space.root != 0;
        result.mapping_pages += process.resources().native_pages.size();
        for (const auto &thread : process.threads())
        {
            result.native_threads += thread.native_active;
            result.wait_members += thread.wait_registration.active() || thread.native_deadline.armed() || thread.syscall_pending;
        }
    }
    const auto backend = hw::ownership_report();
    retain_failure(backend.failure);
    result.backend_spaces = backend.address_spaces;
    if (backend.executing) retain_failure(Status::busy("native backend still executing"));
    const auto entries = entry_stacks.accounting();
    result.active_entries = entries.active_slots;
    result.leased_entries = entries.leased_slots;
    result.quarantined_entries = entries.quarantined_slots;
    for (const auto &entry : stranded_entry_stacks) result.stranded_entries += entry.handle.pool != nullptr;
    for (const auto &space : quarantined_spaces)
    {
        result.quarantined_spaces += space.used || space.reserved || space.space.root || !space.pages.empty();
        result.quarantined_pins += space.pinned_count;
    }
    auto members = [&](const auto &waits) {
        for (const auto &entry : waits) result.wait_members += entry.pid || entry.tid || entry.object.pool;
    };
    members(poll_waits); members(futex_waits); members(pipe_read_waits); members(pipe_write_waits);
    members(pty_waits);
    members(child_waits); members(native_udp_receive_waits); members(linux_udp_receive_waits);
    for (const auto &entry : linux_udp_receive_waits) result.wait_members += entry.reservation != 0;
    retain_failure(linux_udp_receive_store.failure());
    if (linux_udp_receive_store.contained()) retain_failure(Status::fault("Linux UDP owner store is contained"));
    auto objects = [&](const auto &pool) {
        retain_failure(pool.ownership_status());
        const auto info = pool.accounting();
        result.live_objects += info.live_objects;
        result.object_pages += info.backing_pages;
        result.object_quarantine += info.quarantined_pages;
    };
    objects(poll_objects); objects(futex_objects); objects(pipe_read_objects); objects(pipe_write_objects);
    objects(pty_wait_objects);
    objects(child_wait_objects); objects(native_udp_receive_objects); objects(linux_udp_receive_objects);
    const auto origins = syscall_origins.ownership();
    result.origin_records = origins.prepared + origins.live + origins.bound;
    result.origin_borrows = origins.borrows;
    result.origin_pending = origins.pending;
    const auto continuations = vfs_continuations.ownership();
    result.vfs_records = continuations.prepared;
    result.vfs_pending = continuations.pending;
    result.vfs_captures = continuations.captured;
#ifdef OK_PRIVATE_POLL_MASK
    for (const auto &entry : private_signal_actions)
    {
        result.signal_owners += entry.pid || entry.shared.identity().process || entry.group_kill || entry.native_termination;
        for (auto word : entry.shared.pending_mask().words) result.signal_owners += word != 0;
    }
    for (usize i = 0; i < private_signal_queues.size(); ++i)
    {
        const auto &queue = private_signal_queues[i];
        result.signal_owners += queue.identity().process != 0;
        for (auto word : queue.pending_mask().words) result.signal_owners += word != 0;
        const auto &stack = private_signal_altstacks[i];
        result.signal_owners += stack.base || stack.size || stack.flags != sched::SignalAltstack::disable;
    }
    for (const auto &entry : poll_signal_masks.records()) result.signal_waits += entry.phase != PollSignalMasks::Phase::none;
    for (const auto &entry : signal_waits.records()) result.signal_waits += entry.phase != decltype(signal_waits)::Phase::none;
#endif
#ifdef OK_PRIVATE_MIPS_O32_SIGNAL
    for (const auto &entry : private_mips_states) result.signal_owners += entry.state() != PrivateMipsReadiness::State::unverified;
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
    for (const auto &entry : private_n64_states) result.signal_owners += entry.readiness.state() != PrivateN64Readiness::State::unverified;
#endif
#ifdef OK_PRIVATE_PPC32_SIGNAL
    for (const auto &entry : private_ppc_states)
        result.signal_owners += entry.initial || entry.initial_relocation || entry.initial_space || entry.staged_space.root ||
                                entry.readiness.state() != PrivatePpcReadiness::State::unverified ||
                                entry.readiness.transaction() != PrivatePpcReadiness::Transaction::none;
#endif
    result.pool_pages = page_pool.used_count();
    result.user_pages = page_pool.user_count();
    result.pool_pins = page_pool.pinned_count();
    result.pending_pages = page_pool.pending_count();
    result.quarantined_pages = page_pool.quarantined_kernel_page_count();
    const auto files = shared_files.ledger();
    retain_failure(files.first_cleanup_failure);
    result.shared_files = files.files;
    result.shared_aliases = files.aliases;
    result.shared_retiring = files.retiring_files;
    result.shared_frames = files.retained_frames;
    result.shared_reservations = files.reserved_cells;
    const auto resources = kernel.scheduler().resource_drain_report();
    retain_failure(resources.first_failure);
    if (resources.active) retain_failure(Status::busy("scheduler resource drain remains active"));
    return result;
}
Status shutdown_retire(Kernel &kernel)
{
    if (auto status = shutdown_outer_boundary(kernel); !status) return hold_shutdown_failure(status);
    if (!shutdown_admission_closed) return Status::denied("native retirement requires closed admission");
    if (shutdown_retirement_started) return Status::already_exists("native shutdown retirement cannot be retried");
    if (auto status = original_init_state(kernel); !status) return hold_shutdown_failure(status);
    if (auto status = shutdown_quiesced_groups(kernel, original_shutdown_owner); !status) return hold_shutdown_failure(status);
    shutdown_retirement_started = true;
    finish(kernel, original_shutdown_owner.process, 0, 0);
    if (!runtime_failure) return hold_shutdown_failure(runtime_failure);
    auto trim = [&](auto &pool) { return pool.trim_empty_pages(); };
    if (auto status = trim(poll_objects); !status) return hold_shutdown_failure(status);
    if (auto status = trim(futex_objects); !status) return hold_shutdown_failure(status);
    if (auto status = trim(pipe_read_objects); !status) return hold_shutdown_failure(status);
    if (auto status = trim(pipe_write_objects); !status) return hold_shutdown_failure(status);
    if (auto status = trim(pty_wait_objects); !status) return hold_shutdown_failure(status);
    if (auto status = trim(child_wait_objects); !status) return hold_shutdown_failure(status);
    if (auto status = trim(native_udp_receive_objects); !status) return hold_shutdown_failure(status);
    if (auto status = trim(linux_udp_receive_objects); !status) return hold_shutdown_failure(status);
    started = false;
    if (auto status = native_retirement_empty(shutdown_report(kernel)); !status) return hold_shutdown_failure(status);
    shutdown_retirement_completed = true;
    return {};
}
Status shutdown_verify_drained(Kernel &kernel)
{
    if (auto status = shutdown_outer_boundary(kernel); !status) return hold_shutdown_failure(status);
    if (!shutdown_admission_closed || !shutdown_retirement_completed)
        return Status::denied("native shutdown retirement has not completed");
    if (auto status = kernel.scheduler().shutdown_console_status(); !status) return hold_shutdown_failure(status);
    if (auto status = kernel.scheduler().shutdown_endpoint_status(); !status) return hold_shutdown_failure(status);
    if (kernel.vfs().socket_path_reference_count())
        return hold_shutdown_failure(Status::busy("native shutdown retains a socket path reference"));
    const auto report = shutdown_report(kernel);
    if (auto status = native_retirement_empty(report); !status) return hold_shutdown_failure(status);
    if (report.pool_pages || report.user_pages || report.pool_pins || report.pending_pages || report.quarantined_pages ||
        report.shared_files || report.shared_aliases || report.shared_retiring || report.shared_frames || report.shared_reservations)
        return hold_shutdown_failure(Status::busy("native shutdown retains page or shared-file ownership"));
    return {};
}

Status runtime_status()
{
    return shutdown_failure ? runtime_failure : shutdown_failure;
}
Status terminate_native_process(Kernel &kernel, u64 pid, u8 signal)
{
    const auto *process = kernel.scheduler().find(pid);
    if (!pid || !process) return Status::not_found("native termination process not found");
    if (process->execution() != sched::ProcessExecution::user_process || process->state() == sched::ProcessState::created)
        return Status::invalid_argument("native termination requires a published user process");
    if (process->state() == sched::ProcessState::zombie || process->state() == sched::ProcessState::exited)
        return Status::already_exists("native process has already completed");
    if (!signal || signal > SignalActionCodec<lu::configured_native_profile()>::max_signal)
        return Status::invalid_argument("invalid native termination signal");
    if (runtime_kernel != &kernel)
        return Status::not_initialized("native termination requires its service owner");
    if (!runtime_failure) return runtime_failure;
    if (!started) return Status::not_initialized("native termination requires a running service");
    if (entry_stacks.accounting().active_slots)
        return Status::busy("native termination requires an inactive entry boundary");
    if (!process->resources().native_space.root || process->resources().native_pages.empty())
        return Status::invalid_argument("native termination requires a retained address space");
    usize live{};
    for (const auto &thread : process->threads())
        if (thread.state != sched::ProcessState::exited)
        {
            if (!thread.native_active || !thread.entry_stack.pool || !thread.kernel_stack_top)
                return Status::invalid_argument("native termination requires live native threads and entry leases");
            if (auto status = validate_entry_stack(entry_stacks, thread); !status) return status;
            ++live;
        }
    if (!live) return Status::invalid_argument("native termination has no live native thread");
    finish(kernel, pid, 0, signal);
    return runtime_failure;
}
void release_process(Kernel &kernel, u64 pid)
{
    cancel_poll(kernel, pid);
    if (!cancel_futex(kernel, pid)) return;
    cancel_pipe_read(kernel, pid);
    cancel_pipe_write(kernel, pid);
    cancel_pty_waits(kernel, pid);
    cancel_child_wait(kernel, pid);
    retire_native_udp_receives(kernel, pid);
    const auto linux_udp_retired = retire_linux_udp_receives(kernel, pid, {}, LinuxUdpReceiveRetirement::process_exit);
    if (!runtime_failure) return;
    if (!linux_udp_retired) { runtime_failure = linux_udp_retired; started = false; return; }
    if (auto *process = kernel.scheduler().find(pid))
    {
        if (auto status = retire_group_stacks(*process); !status)
        { runtime_failure = status; return; }
#ifdef OK_PRIVATE_POLL_MASK
        private_signal_retire(pid);
#endif
        clear_group_tids(kernel, *process);
        release(process->resources().native_space, process->resources().native_pages);
        emit_entry_stack_stats();
        for (auto &thread : process->threads())
        {
            thread.native_deadline.cancel();
            thread.wait_registration.cancel();
            thread.native_active = false;
        }
    }
}
void input(char value)
{
    if (!shutdown_admission_closed && !native_shutdown_pending() && runtime_kernel != nullptr)
        runtime_kernel->scheduler().console_input(value);
}
} // namespace ok::user::native
