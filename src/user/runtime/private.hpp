#pragma once
// Private to the native runtime units (src/user/runtime.cpp and src/user/runtime/).
// The runtime is one serialized boot-CPU owner; every declaration here has
// exactly one definition in one unit, and all state is constant-initialized.
#include "ok/user/native_signal_config.hpp"
#include "ok/uapi/signal.h"
#include "../native_lifecycle.hpp"
#include "../signals/private_signal_scheduler_relocation.hpp"
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
#include "../native_poll_mask.hpp"
#include "../signals/process_pending.hpp"
#include "../signals/wait_state.hpp"
#include "../signals/child_exit_lifecycle.hpp"
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
#include "../../../tests/native/private_futex_peer_shared.hpp"
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
#include "../native_syscall_origin.hpp"
#include <ok/net/tcp_accept_copied.hpp>
#include "../native_shared_capture.hpp"
#if defined(OK_PRIVATE_MIPS64_N64_SIGNAL) || defined(OK_PRIVATE_PPC32_SIGNAL)
#include "../native_syscall_restart_publication.hpp"
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
extern u64 entry_syscalls, entry_timers, entry_faults, entry_rejections;
#ifdef OK_KERNEL_FREESTANDING
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
extern std::array<QuarantinedSpace, hw::max_address_spaces + 1> quarantined_spaces;
extern bool started;
extern Kernel *runtime_kernel;
extern Status runtime_failure;
extern ShutdownOwner original_shutdown_owner;
extern bool runtime_step_active, shutdown_admission_closed, shutdown_retirement_started, shutdown_retirement_completed;
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
extern std::array<PollWaitIndex, sched::max_processes> poll_waits;
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
SyscallOrigins::FutexBinding futex_origin_binding(memory::ObjectHandle object, const FutexWait &wait);
using FutexObjectsLayout = memory::ObjectPool<FutexWait, NativePagePool, 1>;
constexpr usize futex_object_pages =
    (sched::max_processes + FutexObjectsLayout::slots_per_page - 1) / FutexObjectsLayout::slots_per_page;
using FutexObjects = memory::ObjectPool<FutexWait, NativePagePool, futex_object_pages>;
extern FutexObjects futex_objects;
extern std::array<PollWaitIndex, sched::max_processes> futex_waits;

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
};
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

[[maybe_unused]] memory::ObjectHandle poll_object(sched::ProcessId pid, sched::ThreadId tid);
u64 poll_syscall_number(const hw::UserFrame &frame, bool select_sets);
void cancel_poll(Kernel &kernel, memory::ObjectHandle object, std::string_view reason = "cancelled", i64 result = -4);

void cancel_poll(Kernel &kernel, sched::ProcessId pid);

Result<usize> poll_ready(Kernel &kernel, PollWait &wait);

bool poll_output(const hw::AddressSpace &space, const PollWait &wait);
extern "C" int ok_platform_console_poll() __attribute__((weak));
void emit(std::string_view text);
void emit_number(u64 value);
[[maybe_unused]] void emit_hex(u64 value);
void emit_entry_stack_stats();
void emit_pool_stats();
Status activate_pool();
#ifdef OK_ENABLE_TEST_POINTS
Status check_runtime_objects();
#endif
bool same_space(const hw::AddressSpace &a, const hw::AddressSpace &b);
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
// This first PTY runtime is native-only.
memory::ObjectHandle pty_wait_object(sched::ProcessId pid, sched::ThreadId tid);
void cancel_pty_waits(Kernel &kernel, sched::ProcessId pid, sched::ThreadId tid = 0);
void prune_pty_waits(Kernel &kernel);
i64 resume_pty_io(Kernel &kernel, sched::ProcessControlBlock &process,
                  memory::ObjectHandle object, bool &complete);
i64 begin_pty_io(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadId tid,
                 i32 fd, u16 description_id, uptr address, usize count, bool writing,
                 bool &complete, const PipeVectors &vectors = {});

memory::ObjectHandle child_wait_object(sched::ProcessId pid, sched::ThreadId tid);
// Return a ready child PID, ECHILD, EFAULT, zero for WNOHANG, or EAGAIN
// exclusively for a matching live child. Never retain a canonical PCB pointer.
i64 child_wait_probe(Kernel &kernel, sched::ProcessId pid, const ChildWaitRequest &request);
void cancel_child_wait(Kernel &kernel, memory::ObjectHandle object,
                       std::string_view outcome = "cancelled", i64 result = -125);
void cancel_child_wait(Kernel &kernel, sched::ProcessId pid);
void prune_child_waits(Kernel &kernel);
i64 resume_child_wait(Kernel &kernel, sched::ProcessControlBlock &process, memory::ObjectHandle object, bool &complete);
memory::ObjectHandle native_udp_receive_object(sched::ProcessId pid, sched::ThreadId tid);
void retire_native_udp_receives(Kernel &kernel, sched::ProcessId pid,
                               std::optional<sched::ThreadId> tid = {});
Status retire_native_udp_receives_for_exec(Kernel &kernel, sched::ProcessId pid);
i64 resume_native_udp_receive(Kernel &kernel, sched::ProcessControlBlock &process,
                             memory::ObjectHandle object, bool &complete);
Status scan_native_udp_receives(Kernel &kernel);
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
    u8 native_termination{};
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
void private_process_signal_fatal(Kernel &kernel);
bool private_signal_call(Kernel &kernel, sched::ProcessControlBlock &process,
                         sched::ThreadControlBlock &thread, bool &complete, i64 &result);
Status private_child_exit_commit(Kernel &kernel, PrivateChildExit::Completion &completion);

bool private_child_reap(Kernel &kernel);
#endif
void queue_pipe_sigpipe(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadId tid);
bool finish_futex(Kernel &kernel, memory::ObjectHandle object, sched::WaitCompletion cause, i64 result);
bool expire_futex(Kernel &kernel, u64 now);

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
        const auto route = access.linux_personality()
            ? AcceptCall::route(lu::NativeBridge<lu::configured_native_profile()>::snapshot(saved_frame))
            : lu::TcpCallRoute{access.operation(), false, 0, 0};
        TcpOriginalDescription original{kernel.scheduler(), kernel.native_tcp(), runtime_failure};
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
                original_fd = static_cast<i32>(static_cast<u32>(first_argument(saved_frame, access)));
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
            if (result && request.value().operation == Call::write && request.value().linux &&
                result.value().error == lu::tcp_errno_epipe[profile])
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
    static u64 first_argument(const hw::UserFrame &frame, const Access &access)
    {
        if (!access.linux_personality()) return hw::syscall(frame).arguments[0];
        using Codec = lu::Codec<lu::configured_native_profile()>;
        return lu::NativeBridge<lu::configured_native_profile()>::snapshot(frame).integer[
            Codec::profile.argument_registers[0]] & Codec::word_mask;
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
    static Result<net::tcp::Endpoint> address(Access &access, const Request &request)
    {
        const i32 length = static_cast<i32>(static_cast<u32>(request.arguments[2]));
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
            if (static_cast<i32>(static_cast<u32>(a[0])) != 2)
                return lu::Return{0, error(request, lu::UdpErrno::e_afnosupport, 97)};
            if ((type & mask) != stream || (type & ~(mask | nonblock | cloexec)) ||
                (static_cast<i32>(static_cast<u32>(a[2])) != 0 && static_cast<i32>(static_cast<u32>(a[2])) != 6))
                return lu::Return{0, error(request, lu::UdpErrno::e_opnotsupp, 95)};
            if (auto status = kernel.activate_native_tcp(); !status) return translated(access, status);
            if (!access.live()) return Status::denied("TCP socket lost image during link activation");
            auto opened = kernel.native_tcp_operations().open(pid, type & nonblock, type & cloexec);
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
                auto local = address(access, request); if (!local) return local.status();
                if (local.value().address != kernel.native_udp().link().local_ip)
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
                    semantic_error = protocol.value().transport.state == net::tcp::State::syn_sent
                        ? tcp_error(request, lu::tcp_errno_ealready, 114) : tcp_error(request, lu::tcp_errno_eisconn, 106);
                    return Status::fault();
                }
                auto peer = address(access, request); if (!peer) return peer.status();
                if (peer.value().address != kernel.native_udp().link().peer_ip)
                { semantic_error = error(request, lu::UdpErrno::e_netunreach, 101); return Status::fault(); }
                auto now = kernel.native_tcp_now(); if (!now) return now.status();
                if (!access.live()) return Status::denied("TCP connect lost image during clock acquisition");
                auto iss = kernel.native_tcp_iss(state.value().local, peer.value()); if (!iss) return iss.status();
                if (!access.live()) return Status::denied("TCP connect lost image during entropy acquisition");
                const auto status = service.connect(handle, peer.value(), iss.value(), now.value());
                if (status) semantic_error = tcp_error(request, lu::tcp_errno_einprogress, 115);
                return status ? Result<usize>{0} : Result<usize>{status};
            }
            // Defer unsupported operations before touching connection-only state;
            // a listener is a valid stream but not a connection handle.
            if (request.operation != Call::shutdown && request.operation != Call::read && request.operation != Call::write)
                return Status::unsupported("TCP operation requires the next checked continuation slice");
            auto stream = service.snapshot(handle); if (!stream) return stream.status();
            if (!stream.value().endpoint || stream.value().endpoint->kind != net::tcp::HandleKind::connection)
            { semantic_error = tcp_error(request, lu::tcp_errno_enotconn, 107); return Status::fault(); }
            auto protocol = service.endpoints().state(*stream.value().endpoint); if (!protocol) return protocol.status();
            const auto &state = protocol.value().transport;
            if (state.failure != net::tcp::Failure::none)
            {
                if (state.failure == net::tcp::Failure::refused) semantic_error = tcp_error(request, lu::tcp_errno_econnrefused, 111);
                else if (state.failure == net::tcp::Failure::timed_out) semantic_error = tcp_error(request, lu::tcp_errno_etimedout, 110);
                else if (state.failure == net::tcp::Failure::reset) semantic_error = tcp_error(request, lu::tcp_errno_econnreset, 104);
                else semantic_error = tcp_error(request, lu::tcp_errno_econnaborted, 103);
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
            if (request.operation == Call::read)
            {
                auto preview = service.peek(handle, std::span{payload_}.first(size)); if (!preview) return preview.status();
                if (preview.value())
                    if (auto status = access.write(a[1], {payload_.data(), preview.value()}); !status) return status;
                auto consumed = service.read(handle, std::span{payload_}.first(preview.value()));
                if (!consumed || consumed.value() != preview.value()) return Status::fault("TCP bytes changed after checked output");
                return consumed;
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
                 sched::ThreadId calling_tid);
i64 exec_image(Kernel &kernel, sched::ProcessId pid, sched::ProcessControlBlock *process,
               sched::ThreadId calling_tid, hw::AddressSpace &space, const std::array<uptr, 6> &a,
               bool &complete);
// Admit /init under its initial identity and install it as the first user process.
Result<sched::ProcessId> install_init(Kernel &kernel, const fs::BootArchive &archive);
} // namespace runtime_detail
} // namespace ok::user::native
