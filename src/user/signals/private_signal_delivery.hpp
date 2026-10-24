// Included inside the native runtime's anonymous namespace, after finish().
// Ordinary bounded boot-CPU service; diagnostics have a separate Debug gate.
constexpr auto private_signal_profile = lu::configured_native_profile();
using PrivateSignalQueue = sched::PendingSignals<SignalActionCodec<private_signal_profile>::max_signal>;
#if defined(__i386__)
using PrivateSignalDelivery = I386SignalDelivery;
using PrivateSignalFrame = I386SignalReturn;
#elif defined(__arm__)
using PrivateSignalDelivery = Arm32SignalDelivery;
using PrivateSignalFrame = Arm32SignalReturn;
#elif defined(__aarch64__)
using PrivateSignalDelivery = Aarch64SignalDelivery;
using PrivateSignalFrame = Aarch64SignalFrame;
#elif defined(OK_PRIVATE_PPC32_SIGNAL)
using PrivateSignalDelivery = Ppc32SignalDelivery;
using PrivateSignalFrame = Ppc32SignalReturn;
#elif defined(OK_PRIVATE_MIPS64_N64_SIGNAL)
using PrivateSignalDelivery = Mips64N64SignalDelivery;
using PrivateSignalFrame = Mips64N64SignalReturn;
#elif defined(OK_PRIVATE_MIPS_O32_SIGNAL)
using PrivateSignalDelivery = MipsO32SignalDelivery;
using PrivateSignalFrame = MipsO32SignalReturn;
#elif defined(__loongarch64)
using PrivateSignalDelivery = Loongarch64SignalDelivery;
using PrivateSignalFrame = Loongarch64SignalFrame;
#elif defined(__riscv)
using PrivateSignalDelivery = RiscvSignalDelivery<private_signal_profile>;
using PrivateSignalFrame = RiscvSignalFrame<private_signal_profile>;
#else
using PrivateSignalDelivery = X86SignalDelivery;
using PrivateSignalFrame = X86SignalFrame;
#endif

#if !defined(__i386__) && !defined(__arm__) && !defined(__loongarch64) && !defined(OK_PRIVATE_MIPS_O32_SIGNAL) && !defined(OK_PRIVATE_MIPS64_N64_SIGNAL) && !defined(OK_PRIVATE_PPC32_SIGNAL)
template <typename Read>
bool private_signal_decode(const hw::UserFrame &current, Read &&read, PrivateSignalFrame::Restored &out)
{
    auto raw_stack = [](PrivateSignalFrame::Stack raw, PrivateSignalFrame::Stack &normalized) noexcept {
        normalized = raw; return true;
    };
#if defined(__aarch64__)
    return PrivateSignalFrame::decode(current.sp, current, hw::user_begin, hw::user_end, read, out, raw_stack);
#elif defined(__riscv)
    static_cast<void>(raw_stack);
    return PrivateSignalFrame::decode(current.sp, current, hw::user_begin, hw::user_end, read, out);
#else
    return PrivateSignalFrame::decode(current.sp, current, hw::user_begin, hw::user_end, 0xffbf, read, out, raw_stack);
#endif
}
#endif
using PrivateProcessQueue = ProcessPendingSignals<SignalActionCodec<private_signal_profile>::max_signal>;
using PrivateSignalView = SignalPendingView<PrivateSignalQueue, PrivateProcessQueue>;
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
std::array<PrivateSignalActions, sched::max_processes> private_signal_actions{};
std::array<PrivateSignalQueue, memory::KernelEntryStackPool::capacity> private_signal_queues{};
std::array<sched::SignalAltstack, memory::KernelEntryStackPool::capacity> private_signal_altstacks{};

using PrivateChildExit = ChildExitLifecycle<private_signal_profile>;
PrivateSignalActions *private_signal_find(sched::ProcessId pid)
{
    for (auto &entry : private_signal_actions) if (entry.pid == pid) return &entry;
    return nullptr;
}

PrivateSignalActions *private_signal_process(Kernel &kernel, sched::ProcessId pid)
{
    const auto *process = kernel.scheduler().find(pid);
    if (!pid || !process || process->state() == sched::ProcessState::zombie ||
        process->state() == sched::ProcessState::exited) return nullptr;
    for (auto &entry : private_signal_actions)
        if (entry.pid == pid) return &entry;
    for (auto &entry : private_signal_actions)
        if ((!entry.pid || !kernel.scheduler().find(entry.pid)) && entry.epoch != ~u64{0})
        {
            if (!entry.shared.reset({pid, pid, ++entry.epoch})) return nullptr;
            entry.pid = pid;
            entry.table = {};
            entry.group_kill = false;
            entry.native_termination = 0;
            entry.lineage = {};
            return &entry;
        }
    return nullptr;
}
sched::SignalActionTable *private_signal_table(Kernel &kernel, sched::ProcessId pid)
{
    auto *entry = private_signal_process(kernel, pid);
    return entry ? &entry->table : nullptr;
}
PrivateProcessQueue *private_signal_shared(Kernel &kernel, sched::ProcessId pid)
{
    auto *entry = private_signal_process(kernel, pid);
    return entry ? &entry->shared : nullptr;
}
void private_signal_retire(sched::ProcessId pid)
{
    for (auto &entry : private_signal_actions)
        if (entry.pid == pid)
        {
            entry.shared.retire();
            entry.pid = 0;
            entry.table = {};
            entry.group_kill = false;
            entry.native_termination = 0;
            entry.lineage = {};
            return;
        }
}
PrivateSignalQueue *private_signal_queue(const sched::ThreadControlBlock &thread)
{
    if (!thread.entry_stack.pool || thread.entry_stack.slot >= private_signal_queues.size()) return nullptr;
    auto &queue = private_signal_queues[thread.entry_stack.slot];
    const PrivateSignalQueue::Identity identity{thread.owner, thread.tid, thread.entry_stack.generation};
    if (queue.identity() != identity)
    {
        if (!queue.reset(identity)) return nullptr;
        private_signal_altstacks[thread.entry_stack.slot].reset();
    }
    return &queue;
}
void private_signal_thread_retire(sched::ProcessId pid, sched::ThreadId tid, memory::KernelEntryStackHandle lease)
{
    if (!lease.pool || lease.slot >= private_signal_queues.size()) return;
    auto &queue = private_signal_queues[lease.slot];
    if (queue.identity() != PrivateSignalQueue::Identity{pid, tid, lease.generation}) return;
    // Revoke borrowed views while preserving the non-repeating sequence counter.
    queue.retire();
    private_signal_altstacks[lease.slot].reset();
}
PrivateSignalView private_signal_pending(Kernel &kernel, const sched::ProcessControlBlock &process,
                                         const sched::ThreadControlBlock &thread)
{
    if (thread.owner != process.pid() || kernel.scheduler().find(process.pid()) != &process ||
        kernel.scheduler().find_thread(process.pid(), thread.tid) != &thread || !thread.native_active)
        return {nullptr, nullptr};
    return {private_signal_queue(thread), private_signal_shared(kernel, process.pid())};
}
sched::SignalAltstack *private_signal_altstack(const sched::ThreadControlBlock &thread)
{
    return private_signal_queue(thread) ? &private_signal_altstacks[thread.entry_stack.slot] : nullptr;
}
// Fixed FXSAVE/FPSIMD/ARM32 D32/RISC-V D/LoongArch scalar FPU state: no dynamic extension-state permissions.
constexpr auto private_signal_stack_size = [](u64) noexcept { return true; };

bool private_signal_can_enter(const sched::ProcessControlBlock &process, const hw::UserFrame &frame)
{
    // Match the backend's user PC and SP entry checks before publishing signal
    // state. A rejected destination is a bad user frame, not an exit(127).
    u64 stack_address = frame.sp;
#if defined(__i386__)
    if (!I386SignalEntry::effective_sp(frame, hw::user_begin, hw::user_end, stack_address)) return false;
    for (usize i = 8; i < 12; ++i)
        if (!I386RtSignalFrame::data_selector(frame.registers[i] & 0xffff)) return false;
#endif
    if (frame.pc < hw::user_begin || frame.pc >= hw::user_end ||
        stack_address <= hw::user_begin || stack_address > hw::user_end ||
        !hw::user_range(process.resources().native_space, stack_address - 1, 1, true)) return false;
#if defined(__arm__)
    if (!Arm32RtSignalFrame::program(frame.pc, frame.flags, hw::user_begin, hw::user_end) ||
        !hw::Arm32FpState::admitted(frame)) return false;
#elif defined(OK_PRIVATE_PPC32_SIGNAL)
    if ((frame.pc & 3) || !private_ppc_backend::frame_mode(frame)) return false;
#elif defined(OK_PRIVATE_MIPS64_N64_SIGNAL)
    if ((frame.pc & 3) || (frame.sp & 7) || !private_n64_backend::frame_mode(frame)) return false;
#elif defined(OK_PRIVATE_MIPS_O32_SIGNAL)
    if ((frame.pc & 3) || (frame.sp & 7) || !private_mips_backend::frame_mode(frame)) return false;
#elif defined(__aarch64__) || defined(__loongarch64)
    if (frame.pc & 3) return false;
#elif defined(__riscv)
    if (frame.pc & 1) return false;
#endif
    for (const auto &page : process.resources().native_pages)
        if (frame.pc >= page.virtual_address && frame.pc - page.virtual_address < hw::page_size)
            return (page.flags & hw::page_execute) != 0;
    return false;
}

bool private_signal_restorer(const sched::ProcessControlBlock &process, u64 &address)
{
    address = 0;
#ifdef OK_PRIVATE_SIGNAL_HAS_TRAMPOLINE
    // Validate the complete code extent and its retained ownership. User VM
    // mutation cannot replace/unmap/protect this reserved page in the fixture.
    const hw::PageMapping *mapping = nullptr;
    for (const auto &page : process.resources().native_pages)
        if (page.virtual_address == private_signal_code_address) { mapping = &page; break; }
    if (!mapping || mapping->flags != (hw::page_read | hw::page_execute) ||
        !page_pool.owns(mapping->physical_address)) return false;
    constexpr auto code = PrivateSignalCode::image();
    const auto *bytes = reinterpret_cast<const std::byte *>(mapping->physical_address);
    for (usize i = 0; i < code.size(); ++i) if (bytes[i] != code[i]) return false;
    return PrivateSignalCode::entry_address(private_signal_code_address, SignalTrampolineEntry::realtime,
                                            hw::user_begin, hw::user_end, address);
#else
    static_cast<void>(process);
    return true;
#endif
}
Status private_signal_fork(Kernel &kernel, sched::ProcessId parent, sched::ThreadId caller, sched::ProcessId child)
{
    auto *parent_thread = kernel.scheduler().find_thread(parent, caller);
    auto *child_process = kernel.scheduler().find(child);
    if (!parent_thread || !child_process || child_process->threads().empty())
        return Status::fault("private signal fork thread owner");
    auto &child_thread = child_process->threads()[0];
    auto *from = private_signal_table(kernel, parent);
    auto *to = private_signal_table(kernel, child);
    if (!from || !to)
        return Status::fault("private signal action owner capacity");
    auto *parent_stack = private_signal_altstack(*parent_thread);
    auto *child_stack = private_signal_altstack(child_thread);
    if (!parent_stack || !child_stack)
        return Status::fault("private signal fork altstack owner");
#ifdef OK_PRIVATE_PPC32_SIGNAL
    auto *parent_process = kernel.scheduler().find(parent);
    if (!parent_process || (parent_process->resources().personality == Personality::linux &&
        !private_ppc_inherit(*parent_process, *parent_thread, *child_process, child_thread, false)))
        return Status::fault("private PPC fork signal context inheritance failed");
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
    auto *parent_process = kernel.scheduler().find(parent);
    if (!parent_process || !private_n64_inherit(*parent_process, *parent_thread, *child_process, child_thread))
        return Status::fault("private n64 fork signal context inheritance failed");
#endif
#ifdef OK_PRIVATE_MIPS_O32_SIGNAL
    // The caller has finalized the child return registers, SP and TLS.
    if (!private_mips_inherit(*parent_thread, child_thread))
        return Status::fault("private MIPS fork signal context inheritance failed");
#endif
    *to = from->fork_copy();
    *child_stack = *parent_stack;
    return PrivateChildExit::inherit(kernel.scheduler(), parent, caller, child, private_signal_find);
}
void private_signal_exec(Kernel &kernel, sched::ProcessControlBlock &process, PrivateSignalQueue *previous,
                         sched::SignalAltstack old_stack)
{
    auto *owner = private_signal_find(process.pid());
    auto *actions = owner ? &owner->table : nullptr;
    auto *next = private_signal_queue(process.threads()[0]);
    if (!PrivateChildExit::owns(owner, process.pid()) || !actions || !next || !previous)
    {
        runtime_failure = Status::fault("private signal exec lost owner");
        started = false;
        return;
    }
    actions->reset_for_exec();
    auto *stack = private_signal_altstack(process.threads()[0]);
    if (!stack) { runtime_failure = Status::fault("private signal exec altstack owner"); started = false; return; }
    old_stack.clear_for_exec();
    *stack = old_stack;
    if (previous == next)
    {
        if (auto status = PrivateChildExit::exec_committed(kernel.scheduler(), process.pid(), private_signal_find); !status)
        { runtime_failure = status; started = false; }
        return;
    }
    // Only the actual calling thread survives exec. Old tickets never escape
    // the serialized service stack; recreate them for the retained leader.
    if (!next->reset(next->identity()))
    {
        runtime_failure = Status::fault("private signal exec queue reset");
        started = false;
        return;
    }
    PrivateSignalQueue::Delivery ticket;
    while (previous->peek({}, ticket))
    {
        const auto &info = ticket.info;
        const auto moved = info.signal == 8 && info.code == 128 && !info.sender_pid && !info.sender_uid
            ? next->enqueue_kernel(8) : next->enqueue(info);
        if (moved != PrivateSignalQueue::Enqueue::queued || !previous->consume(ticket))
        {
            runtime_failure = Status::fault("private signal exec queue transfer");
            started = false;
            return;
        }
    }
    if (auto status = PrivateChildExit::exec_committed(kernel.scheduler(), process.pid(), private_signal_find); !status)
    { runtime_failure = status; started = false; }
}
bool private_signal_wait_rearm_delivery(Kernel &kernel, sched::ProcessControlBlock &process,
                                        sched::ThreadControlBlock &thread);
bool private_signal_deliver(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread)
{
    if (process.resources().personality != Personality::linux) return true;
    auto queue = private_signal_pending(kernel, process, thread);
    auto *actions = private_signal_table(kernel, process.pid());
    if (!queue || !actions)
    {
        runtime_failure = Status::fault("private signal owner missing");
        started = false;
        return false;
    }
    for (;;)
    {
        u32 signal{};
        u64 restorer{};
        if (!private_signal_restorer(process, restorer))
        { runtime_failure = Status::fault("private signal return page lost ownership"); started = false; return false; }
#ifdef OK_PRIVATE_PPC32_SIGNAL
        auto *readiness = private_ppc_state(process, thread);
        const auto identity = private_ppc_identity(process, thread);
        const auto capability = private_ppc_backend::capability();
        if (!readiness)
        { runtime_failure = Status::fault("private PPC signal owner missing"); started = false; return false; }
        PrivateSignalView::Delivery pending;
        if (queue.peek(thread.native_signal_mask, pending))
        {
            const auto *action = actions->find(pending.info.signal);
            if (action && !PrivateSignalDelivery::Actions::ignored(pending.info.signal, *action) &&
                action->handler != sched::SignalAction::default_handler && PrivateSignalDelivery::supported(*action) &&
                !readiness->ready(identity, thread.native_frame, capability))
            {
                if (private_ppc_initial_valid(process, thread)) return !signal_waits.saved(thread);
                runtime_failure = Status::fault("private PPC handler origin unverified");
                started = false; return false;
            }
        }
        const auto cpu_context = readiness->context(identity, thread.native_frame, capability);
#elif defined(OK_PRIVATE_MIPS64_N64_SIGNAL)
        auto *readiness = private_n64_state(process, thread);
        if (!readiness)
        { runtime_failure = Status::fault("private n64 signal lease readiness missing"); started = false; return false; }
        const auto identity = private_n64_identity(process, thread);
        const auto capability = private_n64_backend::capability();
        const auto gate = mips64_n64_handler_gate(queue, *actions, thread.native_signal_mask,
                                                *readiness, identity, thread.native_frame, capability);
        if (gate == Mips64N64HandlerGate::defer_handler) return !signal_waits.saved(thread);
        if (gate != Mips64N64HandlerGate::proceed)
        { runtime_failure = Status::fault("private n64 handler resume unverified"); started = false; return false; }
        const auto cpu_context = readiness->context(identity, thread.native_frame, capability);
#elif defined(OK_PRIVATE_MIPS_O32_SIGNAL)
        auto *readiness = private_mips_state(thread);
        if (!readiness)
        { runtime_failure = Status::fault("private MIPS signal lease readiness missing"); started = false; return false; }
        const auto identity = private_mips_identity(thread);
        const auto capability = private_mips_backend::capability();
        const auto gate = mips_o32_handler_gate(queue, *actions, thread.native_signal_mask,
                                               *readiness, identity, capability);
        if (gate == MipsO32HandlerGate::defer_handler) return !signal_waits.saved(thread);
        if (gate != MipsO32HandlerGate::proceed)
        { runtime_failure = Status::fault("private MIPS handler resume unverified"); started = false; return false; }
        const auto cpu_context = readiness->context(identity, capability);
#else
        constexpr u32 cpu_context = 0xffbf;
#endif
        const auto result = PrivateSignalDelivery::deliver(queue, *actions, thread.native_signal_mask,
            thread.native_frame, *private_signal_altstack(thread), hw::user_begin, hw::user_end, cpu_context,
            [&](u64 address, std::span<const std::byte> bytes) {
                return hw::copy_to_user(process.resources().native_space, address, bytes.data(), bytes.size()).ok();
            }, signal, restorer, [&](const hw::UserFrame &staged) {
                #if defined(__arm__)
                const u64 restorer_pc = staged.registers[14] & ~u64{1};
                bool executable = false;
                for (const auto &page : process.resources().native_pages)
                    if (restorer_pc >= page.virtual_address && restorer_pc - page.virtual_address < hw::page_size)
                        executable = (page.flags & hw::page_execute) != 0;
                if (!executable) return false;
#endif
#ifdef OK_PRIVATE_PPC32_SIGNAL
                return private_signal_can_enter(process, staged) &&
                    readiness->stage_handler(identity, thread.native_frame, staged, capability);
#else
                return private_signal_can_enter(process, staged);
#endif
            }, signal_waits.saved(thread) ? signal_waits.saved(thread) : poll_signal_masks.saved(thread));
        using R = PrivateSignalDelivery::Result;
        if (result == R::handler)
        {
#if defined(OK_PRIVATE_MIPS_O32_SIGNAL) || defined(OK_PRIVATE_MIPS64_N64_SIGNAL)
            if (!readiness->publish_handler(identity, thread.native_frame, capability))
            { runtime_failure = Status::fault("private MIPS handler publication lost readiness"); started = false; return false; }
#endif
#ifdef OK_PRIVATE_PPC32_SIGNAL
            if (!readiness->publish_handler(identity, thread.native_frame, capability))
            { runtime_failure = Status::fault("private PPC handler publication failed"); started = false; return false; }
#endif
            poll_signal_masks.handler_committed(thread);
            private_signal_wait_trace_commit(thread, signal);
            signal_waits.handler_committed(thread);
            return true;
        }
#ifdef OK_PRIVATE_PPC32_SIGNAL
        if (readiness->transaction() != PrivatePpcReadiness::Transaction::none) readiness->cancel();
#endif
        if (result == R::none)
        {
            // An architecture deferral permits a peer to commit SIG_IGN before
            // handler publication. The logical suspend must remain blocked.
            if (signal_waits.saved(thread))
                return private_signal_wait_rearm_delivery(kernel, process, thread);
            // Ignored work may drain the queue without constructing a frame.
            if (poll_signal_masks.saved(thread)) poll_signal_masks.restore(thread);
            return true;
        }
        if (result == R::ignored) continue;
        if (result == R::terminate) finish(kernel, process.pid(), 0, static_cast<u8>(signal));
        else
        {
            runtime_failure = Status::fault("private signal unsupported/corrupt delivery");
            started = false;
        }
        return false;
    }
}
using PrivateSleepInterruption = LinuxSleepInterruption<private_signal_profile>;
bool private_sleep_signal_ready(Kernel &kernel, const sched::ProcessControlBlock &process,
                                const sched::ThreadControlBlock &thread)
{
    if (!PrivateSleepInterruption::owns(process, thread, thread.wait_registration.ticket())) return false;
    const auto queue = private_signal_pending(kernel, process, thread);
    const auto *actions = private_signal_table(kernel, process.pid());
    return queue && actions && sleep_interrupting_signal<private_signal_profile>(queue, *actions,
        thread.native_signal_mask, [](const sched::SignalAction &action) { return PrivateSignalDelivery::supported(action); });
}
bool private_sleep_signal_complete(Kernel &kernel, sched::ProcessControlBlock &process,
                                   sched::ThreadControlBlock &thread)
{
    // No peer execution is permitted between this recipient-side action check
    // and private_signal_deliver. Enqueue never commits EINTR or consumes work.
    if (!private_sleep_signal_ready(kernel, process, thread)) return false;
    const auto now = arch::monotonic_nanoseconds();
    if (!now) { runtime_failure = now.status(); started = false; return false; }
    const auto ticket = thread.wait_registration.ticket();
    const auto completion = PrivateSleepInterruption::complete(kernel.scheduler(), process, thread, ticket, now.value(),
        [&](uptr destination, std::span<const u8> bytes) {
            return hw::copy_to_user(process.resources().native_space, destination, bytes.data(), bytes.size()).ok();
        }, [&](i64 result) { return set_result(process, thread.native_frame, result).ok(); });
    using Outcome = PrivateSleepInterruption::Outcome;
    if (completion.outcome == Outcome::lost) return false;
    if (completion.outcome == Outcome::failed)
    { runtime_failure = Status::fault("sleep interruption completion failed"); started = false; return false; }
    if (native_signal_diagnostics && completion.outcome == Outcome::interrupted)
    {
        emit("OK_NATIVE_SLEEP_INTERRUPT pid="); emit_number(process.pid());
        emit(" tid="); emit_number(thread.tid); emit(" wait="); emit_number(ticket.generation);
        emit(completion.request.absolute ? " kind=absolute" : " kind=relative");
        emit(PrivateSleepInterruption::Codec::timespec_size(completion.request.format) == 8 ? " format=time32" : " format=time64");
        emit(completion.result == -14 ? " result=efault" : " result=eintr");
        emit(" deadline="); emit_number(completion.deadline); emit(" now="); emit_number(now.value()); emit("\n");
    }
    return true;
}
#include "private_pipe_signal.hpp"
#include "private_pipe_write_signal.hpp"
#include "private_child_signal.hpp"
#include "private_futex_signal.hpp"
#include "private_poll_signal.hpp"
#include "private_signal_wait.hpp"
bool private_signal_admit_blocked(Kernel &kernel, const sched::ProcessControlBlock &recipient,
                                  const sched::ThreadControlBlock &target)
{
    // Only the captured Linux receive family may admit its pending frame.
    if (linux_udp_receive_object(recipient.pid(), target.tid).pool)
        return private_linux_udp_signal_owned(kernel, recipient, target);
    PrivatePipeSignal pipe;
    PrivatePipeWriteSignal pipe_write;
    PrivateChildSignal child;
    PrivateFutexSignal futex;
    PrivatePollSignal poll;
    return private_signal_wait_owned(kernel, recipient, target) ||
           PrivateSleepInterruption::owns(recipient, target, target.wait_registration.ticket()) ||
           private_pipe_signal_owned(kernel, recipient, target, pipe) ||
           private_pipe_write_signal_owned(kernel, recipient, target, pipe_write) ||
           private_child_signal_owned(kernel, recipient, target, child) ||
           private_futex_signal_owned(kernel, recipient, target, futex) ||
           private_poll_signal_owned(kernel, recipient, target, poll);
}
i64 private_process_kill_one(Kernel &kernel, user::ProcessSignalSender sender, u64 raw_pid, u64 raw_signal)
{
    using Codec = SignalActionCodec<private_signal_profile>;
    const auto pid = static_cast<i32>(raw_pid);
    const auto signal = static_cast<i32>(raw_signal);
    if (pid <= 0) return -95;
    const auto target = user::find_directed_signal_target(kernel.scheduler(), 0, static_cast<u32>(pid));
    if (!target) return -3;
    if (signal < 0 || signal > static_cast<i32>(Codec::max_signal)) return -22;
    if (!user::directed_signal_permitted(sender.credentials, target.process->credentials(),
                                        sender.pid == target.process->pid())) return -1;
    if (!signal) return 0;
    auto &recipient = *target.process;
    if (recipient.state() == sched::ProcessState::zombie || recipient.state() == sched::ProcessState::exited)
        return 0;
    if (recipient.resources().personality != Personality::linux) return -95;
    if (signal == static_cast<i32>(Codec::stop_signal) || signal == (Codec::mips ? 24 : 20) ||
        signal == (Codec::mips ? 26 : 21) || signal == (Codec::mips ? 27 : 22) ||
        signal == static_cast<i32>(Codec::continue_signal)) return -95;
    auto *state = private_signal_process(kernel, recipient.pid());
    if (!state) return -12;
    if (signal == 9)
    {
        // Service on the next serialized step, before any user/wait completion.
        // Self-kill must not retire the caller while its syscall is publishing.
        state->group_kill = true;
        return 0;
    }
    const auto *action = state->table.find(static_cast<u32>(signal));
    if (!action) return -22;
    const bool ignored = Codec::ignored(static_cast<u32>(signal), *action);
    // Linux prepare_signal examines the named task, not a chosen recipient or
    // an intersection across siblings. A poll saved return mask is not real_blocked.
    const auto *real = signal_waits.real_blocked(*target.thread);
    if (ignored && !target.thread->native_signal_mask.contains(static_cast<u32>(signal)) &&
        !(real && real->contains(static_cast<u32>(signal)))) return 0;
    bool live = false, unblocked = false, qualified = false;
    for (const auto &peer : recipient.threads())
    {
        if (peer.state == sched::ProcessState::exited || !peer.native_active) continue;
        live = true;
        if (peer.native_signal_mask.contains(static_cast<u32>(signal))) continue;
        unblocked = true;
        qualified |= peer.state == sched::ProcessState::runnable || peer.state == sched::ProcessState::running ||
                     private_signal_admit_blocked(kernel, recipient, peer);
    }
    if (!live || (!ignored && unblocked && !qualified)) return -95;
    const auto enqueued = state->shared.enqueue({static_cast<u32>(signal), 0,
        static_cast<u32>(sender.pid), sender.credentials.uid});
    if (enqueued == PrivateProcessQueue::Enqueue::exhausted)
    {
        runtime_failure = Status::fault("private process signal sequence exhausted");
        started = false;
        return -12;
    }
    return enqueued == PrivateProcessQueue::Enqueue::queued || enqueued == PrivateProcessQueue::Enqueue::coalesced
        ? 0 : -22;
}
i64 native_process_kill_one(Kernel &kernel, user::ProcessSignalSender sender, u64 raw_pid, u64 raw_signal)
{
    const auto pid = static_cast<i32>(raw_pid);
    const auto signal = static_cast<i32>(raw_signal);
    if (pid <= 0) return -95;
    auto *recipient = kernel.scheduler().find(static_cast<u32>(pid));
    if (!recipient) return -3;
    if (signal < 0 || signal > OK_NATIVE_SIGNAL_MAX) return -22;
    if (!user::directed_signal_permitted(sender.credentials, recipient->credentials(),
                                        sender.pid == recipient->pid())) return -1;
    if (recipient->execution() != sched::ProcessExecution::user_process) return -95;
    if (!signal || recipient->state() == sched::ProcessState::zombie ||
        recipient->state() == sched::ProcessState::exited) return 0;
    if (signal != OK_SIGTERM && signal != OK_SIGKILL) return -95;
    if (recipient->pid() == sched::user_init_pid) return -1;
    // These two numbers have the same meaning on every retained Linux ABI.
    if (recipient->resources().personality == Personality::linux)
        return private_process_kill_one(kernel, sender, raw_pid, raw_signal);
    bool live = false;
    for (const auto &thread : recipient->threads())
        live |= thread.native_active && thread.state != sched::ProcessState::exited;
    if (!live) return -95;
    auto *state = private_signal_process(kernel, recipient->pid());
    if (!state) return -12;
    // Commit on the next service step, after the sender's saved return frame.
    // A later TERM must not replace an already accepted uncatchable KILL.
    if (state->native_termination != OK_SIGKILL)
        state->native_termination = static_cast<u8>(signal);
    return 0;
}
i64 private_process_kill(Kernel &kernel, const sched::ProcessControlBlock &sender, u64 raw_pid, u64 raw_signal)
{
    const auto captured = user::process_signal_sender(sender);
    if (static_cast<i32>(raw_pid) > 0)
        return private_process_kill_one(kernel, captured, raw_pid, raw_signal);
    return user::send_process_group_signal<SignalActionCodec<private_signal_profile>::max_signal>(
        kernel.scheduler(), captured, raw_pid, raw_signal,
        [&](user::ProcessSignalSender origin, sched::ProcessId target, i32 signal) {
            return native_process_kill_one(kernel, origin, target, static_cast<u64>(signal));
        });
}
i64 native_process_kill(Kernel &kernel, const sched::ProcessControlBlock &sender, u64 raw_pid, u64 raw_signal)
{
    const auto captured = user::process_signal_sender(sender);
    if (static_cast<i32>(raw_pid) > 0)
        return native_process_kill_one(kernel, captured, raw_pid, raw_signal);
    return user::send_process_group_signal<OK_NATIVE_SIGNAL_MAX>(kernel.scheduler(), captured, raw_pid, raw_signal,
        [&](user::ProcessSignalSender origin, sched::ProcessId target, i32 signal) {
            return native_process_kill_one(kernel, origin, target, static_cast<u64>(signal));
        });
}
void private_process_signal_fatal(Kernel &kernel)
{
    // Stable side-table iteration survives group teardown and PCB compaction.
    for (auto &state : private_signal_actions)
        if (state.pid && (state.group_kill || state.native_termination))
        {
            const auto pid = state.pid;
            const auto signal = state.group_kill ? u8{9} : state.native_termination;
            state.group_kill = false;
            state.native_termination = 0;
            const auto *process = kernel.scheduler().find(pid);
            if (!process || process->state() == sched::ProcessState::zombie ||
                process->state() == sched::ProcessState::exited)
            {
                private_signal_retire(pid);
                continue;
            }
            finish(kernel, pid, 0, signal);
            if (!runtime_failure) return;
        }
}
bool private_signal_call(Kernel &kernel, sched::ProcessControlBlock &process,
                         sched::ThreadControlBlock &thread, bool &complete, i64 &result)
{
    if (process.resources().personality != Personality::linux) return false;
    if (private_signal_wait_call(kernel, process, thread, complete, result)) return true;
    const auto call = hw::syscall(thread.native_frame);
    using Codec = SignalActionCodec<private_signal_profile>;
    using Operation = lu::Operation;
    auto is = [&](Operation operation) {
        const auto number = lu::Codec<private_signal_profile>::syscall_number(operation);
        return number && call.number == *number;
    };
    const bool returning = is(Operation::rt_sigreturn)
#if defined(__i386__) || defined(__arm__) || defined(OK_PRIVATE_MIPS_O32_SIGNAL) || defined(OK_PRIVATE_PPC32_SIGNAL)
        || is(Operation::sigreturn)
#endif
        ;
    if (!is(Operation::rt_sigaction) && !returning &&
        !is(Operation::kill) && !is(Operation::tgkill) && !is(Operation::tkill) && !is(Operation::rt_sigpending) &&
        !is(Operation::sigaltstack)) return false;
    if (is(Operation::kill))
    {
        result = private_process_kill(kernel, process, call.arguments[0], call.arguments[1]);
        return true;
    }
    if (is(Operation::rt_sigpending))
    {
        const auto &arguments = call.arguments;
        using MaskCodec = SignalMaskCodec<private_signal_profile>;
        if (arguments[1] > MaskCodec::bytes) { result = -22; return true; }
        if (!arguments[1]) { result = 0; return true; }
        auto pending = private_signal_pending(kernel, process, thread);
        if (!pending) { result = -12; return true; }
        result = user::query_directed_signal_pending<private_signal_profile>(pending.pending_mask(),
            thread.native_signal_mask, arguments[0], arguments[1],
            [&](u64 address, std::span<const std::byte> bytes) {
                return hw::copy_to_user(process.resources().native_space, address,
                                        bytes.data(), bytes.size()).ok();
            });
        return true;
    }
    if (is(Operation::tkill) || is(Operation::tgkill))
    {
        std::array<u64, 6> arguments{};
        for (usize index = 0; index < arguments.size(); ++index)
            arguments[index] = call.arguments[index];
        result = user::send_directed_signal<private_signal_profile>(kernel.scheduler(), process,
            is(Operation::tkill), arguments,
            [&](sched::ProcessControlBlock &recipient, sched::ThreadControlBlock &target,
                sched::PendingSignalInfo info) -> i64 {
                return private_signal_wait_enqueue(kernel, recipient, target, info);
            }, [&](const sched::ProcessControlBlock &recipient, const sched::ThreadControlBlock &target) {
                return private_signal_admit_blocked(kernel, recipient, target);
            });
        return true;
    }
    auto &space = process.resources().native_space;
    auto read = [&](u64 address, std::span<std::byte> bytes) {
        return hw::copy_from_user(space, bytes.data(), address, bytes.size()).ok();
    };
    auto write = [&](u64 address, std::span<const std::byte> bytes) {
        return hw::copy_to_user(space, address, bytes.data(), bytes.size()).ok();
    };
    auto *actions = private_signal_table(kernel, process.pid());
    auto *queue = private_signal_queue(thread);
    auto *shared = private_signal_shared(kernel, process.pid());
    if (!actions || !queue || !shared) { result = -12; return true; }
    const auto &a = call.arguments;
    if (is(Operation::sigaltstack))
    {
        result = update_signal_altstack<private_signal_profile>(*private_signal_altstack(thread),
            a[0], a[1], thread.native_frame.sp, read, write, private_signal_stack_size);
        return true;
    }
    if (is(Operation::rt_sigaction))
    {
        if (a[3] != Codec::Mask::bytes) { result = -22; return true; }
        if (a[1])
        {
            std::array<std::byte, Codec::bytes> bytes{};
            if (!read(a[1], bytes)) { result = -14; return true; }
            if (!PrivateSignalDelivery::supported(Codec::sanitize(Codec::decode(bytes))))
            { result = -95; return true; }
        }
        result = update_signal_action<private_signal_profile>(*actions, static_cast<i32>(a[0]), a[1], a[2], a[3],
            read, write, [&](u32 signal) noexcept {
                shared->discard(signal);
                for (auto &peer : process.threads())
                    if (auto *pending = private_signal_queue(peer)) pending->discard(signal);
            });
        return true;
    }
    if (returning)
    {
#if defined(OK_PRIVATE_PPC32_SIGNAL)
        auto *readiness = private_ppc_state(process, thread);
        const auto identity = private_ppc_identity(process, thread);
        const auto capability = private_ppc_backend::capability();
        if (!readiness)
        { runtime_failure = Status::fault("private PPC return owner missing"); started = false; complete = false; return true; }
        Ppc32SignalReturn::Restored restored;
        const bool valid = Ppc32SignalReturn::decode(
            is(Operation::sigreturn) ? Operation::sigreturn : Operation::rt_sigreturn,
            thread.native_frame, private_ppc_return_context(process, thread), hw::user_begin, hw::user_end,
            read, [&](const hw::UserFrame &staged) { return private_signal_can_enter(process, staged); }, restored) &&
            readiness->stage_sigreturn(identity, thread.native_frame, restored.frame, capability);
        complete = false;
        thread.syscall_pending = false;
        if (!valid) finish(kernel, process.pid(), 0, 11);
        else
        {
            auto stack = *private_signal_altstack(thread);
            if (restored.restore_stack)
                apply_restored_altstack<private_signal_profile>(stack, restored.stack, restored.frame.sp, private_signal_stack_size);
            thread.native_frame = restored.frame;
            thread.native_signal_mask = restored.mask;
            *private_signal_altstack(thread) = stack;
            // User diagnostic metadata never becomes trusted capture state.
            if (!readiness->publish_sigreturn(identity, thread.native_frame, capability))
            { runtime_failure = Status::fault("private PPC return publication failed"); started = false; }
        }
#elif defined(OK_PRIVATE_MIPS64_N64_SIGNAL)
        auto *readiness = private_n64_state(process, thread);
        if (!readiness)
        { runtime_failure = Status::fault("private n64 return readiness missing"); started = false; complete = false; return true; }
        const auto identity = private_n64_identity(process, thread);
        const auto capability = private_n64_backend::capability();
        const bool valid = Mips64N64SignalReturn::restore(Operation::rt_sigreturn,
            *queue, *actions, thread.native_signal_mask, thread.native_frame,
            *private_signal_altstack(thread), readiness->context(identity, thread.native_frame, capability),
            hw::user_begin, hw::user_end, read, write,
            [&](const hw::UserFrame &staged) { return private_signal_can_enter(process, staged); },
            private_signal_stack_size);
        complete = false;
        thread.syscall_pending = false;
        if (!valid) finish(kernel, process.pid(), 0, 11);
        else if (!readiness->publish_sigreturn(identity, thread.native_frame, capability))
        { runtime_failure = Status::fault("private n64 return publication lost readiness"); started = false; }
#elif defined(OK_PRIVATE_MIPS_O32_SIGNAL)
        auto *readiness = private_mips_state(thread);
        if (!readiness)
        { runtime_failure = Status::fault("private MIPS return lease readiness missing"); started = false; complete = false; return true; }
        const auto identity = private_mips_identity(thread);
        const auto capability = private_mips_backend::capability();
        const bool valid = MipsO32SignalReturn::restore(
            is(Operation::sigreturn) ? Operation::sigreturn : Operation::rt_sigreturn,
            *queue, *actions, thread.native_signal_mask, thread.native_frame,
            *private_signal_altstack(thread), readiness->context(identity, capability),
            hw::user_begin, hw::user_end, read, write,
            [&](const hw::UserFrame &staged) { return private_signal_can_enter(process, staged); },
            private_signal_stack_size);
        complete = false;
        thread.syscall_pending = false;
        if (!valid) finish(kernel, process.pid(), 0, 11);
        else if (!readiness->publish_sigreturn(identity, thread.native_frame, capability))
        { runtime_failure = Status::fault("private MIPS return publication lost readiness"); started = false; }
        // No normal completion: edited v0 and a3 belong to the restored frame.
#elif defined(__loongarch64)
        const bool valid = Loongarch64SignalReturn::restore(*queue, *actions,
            thread.native_signal_mask, thread.native_frame, *private_signal_altstack(thread),
            hw::user_begin, hw::user_end, read, write,
            [&](const hw::UserFrame &staged) { return private_signal_can_enter(process, staged); },
            private_signal_stack_size);
        // The adapter commits r4 and edited r2/fs_base together. No TCB TLS
        // cache overrides r2 on entry. Pending SIGFPE is selected before entry.
        complete = false;
        thread.syscall_pending = false;
        if (!valid) finish(kernel, process.pid(), 0, 11);
#else
        PrivateSignalFrame::Restored restored;
#if defined(__arm__)
        const bool valid = Arm32SignalReturn::decode(
            is(Operation::sigreturn) ? Operation::sigreturn : Operation::rt_sigreturn,
            thread.native_frame, hw::user_begin, hw::user_end, read,
            [&](const hw::UserFrame &staged) { return private_signal_can_enter(process, staged); }, restored);
#elif defined(__i386__)
        const bool valid = I386SignalReturn::decode(
            is(Operation::sigreturn) ? Operation::sigreturn : Operation::rt_sigreturn,
            thread.native_frame, hw::user_begin, hw::user_end, read,
            [&](const hw::UserFrame &staged, u64) { return private_signal_can_enter(process, staged); }, restored);
#else
        const bool valid = private_signal_decode(thread.native_frame, read, restored) &&
                           private_signal_can_enter(process, restored.frame);
#endif
        complete = false;
        thread.syscall_pending = false;
        if (!valid) finish(kernel, process.pid(), 0, 11);
        else
        {
            thread.native_frame = restored.frame;
            thread.native_signal_mask = restored.mask;
#if defined(__i386__) || defined(__arm__)
            // Only RT restores alternate-stack metadata. Architectural SP is
            // preserved; any segmented effective address is admission-only.
            if (restored.restore_stack)
                apply_restored_altstack<private_signal_profile>(*private_signal_altstack(thread),
                    restored.stack, restored.frame.sp, private_signal_stack_size);
#else
#if defined(__riscv)
            // RV TP is exclusively saved in native_frame.registers[4]. No
            // FS/GS TCB mirror is consulted by this backend on subsequent entry.
            const sched::SignalAltstack stack{restored.stack.base, restored.stack.size, restored.stack.flags};
#else
            const sched::SignalAltstack stack{restored.stack.pointer, restored.stack.size, restored.stack.flags};
#endif
            apply_restored_altstack<private_signal_profile>(*private_signal_altstack(thread),
                stack, restored.frame.sp, private_signal_stack_size);
#endif
        }
#endif
        result = 0;
        return true;
    }
    result = -38;
    return true;
}
