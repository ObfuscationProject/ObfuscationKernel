// Actual private x86 runtime/scheduler; synthetic traps and user-copy backends.
#define OK_ENABLE_TEST_POINTS 1
#define OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE 1
#define OK_SYSCALL_ORIGIN_MODEL 1
#include "native_clock_model.hpp"
#include <source_location>
namespace
{
usize checks{};
void expect(bool value, std::source_location at = std::source_location::current())
{
    ++checks;
    if (!value) { std::fprintf(stderr, "POLL_SIGNAL_MODEL_FAIL line=%u checks=%zu\n", at.line(), checks); std::abort(); }
}
constexpr uptr output = hw::user_begin + 128, timeout = hw::user_begin + 256;
constexpr uptr site = hw::user_begin + 1024, handler = hw::user_begin + 2048;
struct Group { sched::ProcessId pid{}; sched::ThreadId waiter{}; };
void put(uptr address, u64 value, usize bytes = 8)
{ for (usize i = 0; i < bytes; ++i) mock::memory[address - hw::user_begin + i] = std::byte(value >> (i * 8)); }
u64 get(uptr address, usize bytes = 8)
{
    u64 out{};
    for (usize i = 0; i < bytes; ++i) out |= static_cast<u64>(mock::memory[address - hw::user_begin + i]) << (i * 8);
    return out;
}
Group create(Kernel &kernel)
{
    mock::now = 3000000042;
    auto made = kernel.scheduler().create_user_process("poll-signal-model",
        kernel.arch().make_user_context({hw::user_begin, hw::user_begin + 8192})); CHECK(made);
    auto *p = kernel.scheduler().find(made.value());
    p->resources().personality = sched::UserPersonality::linux;
    p->resources().native_space = {0x100000 + made.value() * 4096, static_cast<u32>(made.value() % 16), 1};
    CHECK(native::allocate(p->resources().native_pages, hw::user_begin, hw::page_read | hw::page_write | hw::page_execute));
    auto peer = kernel.scheduler().create_thread(p->pid(), p->threads()[0].context); CHECK(peer);
    for (auto &thread : p->threads()) { CHECK(native::prepare_native_thread(thread)); thread.native_active = true; }
    p->threads()[0].native_active = false;
    CHECK(kernel.scheduler().set_runnable(p->pid()));
    CHECK(kernel.scheduler().activate_thread(p->pid(), p->pid())); CHECK(kernel.posix().activate(p->pid()));
    native::runtime_kernel = &kernel; native::started = true;
    return {p->pid(), peer.value()};
}
void retire(Kernel &kernel, Group group)
{
    native::release_process(kernel, group.pid);
    CHECK(kernel.scheduler().exit_process(group.pid, 0)); CHECK(kernel.scheduler().reap_orphans());
    expect(native::poll_objects.accounting().live_objects == 0);
    expect(native::runtime_status().ok());
}
void leader(Kernel &kernel, Group group)
{ CHECK(kernel.scheduler().activate_thread(group.pid, group.pid)); CHECK(kernel.posix().activate(group.pid)); }
void arm(Kernel &kernel, Group group, i32 fd, unsigned kind, bool timed)
{
    constexpr std::array<u64, 4> numbers{7, 271, 23, 270}; // poll, ppoll, select, pselect6
    auto *thread = kernel.scheduler().find_thread(group.pid, group.waiter);
    auto &f = thread->native_frame; f = {}; f.flags = 0x202; f.sp = hw::user_begin + 8192; f.pc = site + 2;
    const auto fp = native::X86SignalFrame::reset_fp();
    for (usize i = 0; i < fp.size(); ++i) f.fp_state[i] = static_cast<u8>(fp[i]);
    f.registers[14] = numbers[kind];
    if (kind < 2)
    {
        put(output, static_cast<u32>(fd), 4); put(output + 4, OK_POLLIN, 2); put(output + 6, 99, 2);
        f.registers[9] = output; f.registers[10] = 1;
        f.registers[11] = kind == 0 ? (timed ? 3000 : ~u64{0}) : timed ? timeout : 0;
    }
    else
    {
        put(output, u64{1} << fd); f.registers[9] = fd + 1; f.registers[10] = output;
        f.registers[7] = timed ? timeout : 0;
    }
    put(timeout, 3); put(timeout + 8, 0);
    mock::memory[site - hw::user_begin] = std::byte{0x0f}; mock::memory[site - hw::user_begin + 1] = std::byte{0x05};
    mock::entry_trap = {hw::TrapKind::syscall, 256}; native::last_pid = native::last_tid = 0;
    expect(native::step(kernel));
    expect(thread->syscall_pending && thread->wait_reason == sched::ThreadWaitReason::poll);
    expect(native::poll_object(group.pid, group.waiter).pool != nullptr);
}
void caught(Kernel &kernel, Group group, bool restart, u32 signal = 10)
{
    auto *actions = native::private_signal_table(kernel, group.pid); CHECK(actions);
    auto &action = *actions->find(signal);
    action.handler = handler; action.restorer = handler + 4;
    action.flags = 0x04000004 | (restart ? 0x10000000 : 0);
}
void enqueue(Kernel &kernel, Group group, u32 signal = 10)
{
    auto *process = kernel.scheduler().find(group.pid); CHECK(process);
    const std::array<u64, 6> arguments{group.pid, group.waiter, signal};
    // Use the actual directed-send gate and actual runtime admission helper.
    // Direct queue insertion would miss refusal of a new blocked wait family.
    const auto result = user::send_directed_signal<lu::Profile::x86_64>(kernel.scheduler(), *process, false, arguments,
        [&](sched::ProcessControlBlock &recipient, sched::ThreadControlBlock &target, sched::PendingSignalInfo info) {
            return user::enqueue_directed_signal<lu::Profile::x86_64>(recipient, target, info,
                [&](sched::ProcessId pid) { return native::private_signal_table(kernel, pid); },
                [&](sched::ThreadControlBlock &owner) { return native::private_signal_queue(owner); });
        }, [&](const sched::ProcessControlBlock &recipient, const sched::ThreadControlBlock &target) {
            return native::private_signal_admit_blocked(kernel, recipient, target);
        });
    expect(result == 0);
}
void publication(Kernel &kernel, unsigned kind, bool timed, bool restart, unsigned fault)
{
    auto group = create(kernel); auto pipes = kernel.posix().pipe(0); CHECK(pipes);
    arm(kernel, group, pipes.value()[0], kind, timed);
    caught(kernel, group, !restart); enqueue(kernel, group); caught(kernel, group, restart);
    auto *p = kernel.scheduler().find(group.pid); auto *thread = kernel.scheduler().find_thread(group.pid, group.waiter);
    const auto object = native::poll_object(group.pid, group.waiter);
    const auto wait = *native::poll_objects.resolve(object).value(); const auto frame = thread->native_frame;
    const auto description = wait.descriptions[0];
    expect(kernel.scheduler().open_description(description)->references == 2);
    // Mutated input is never reread. Select must also preserve the supplied set.
    put(output, 0x1234567812345678); put(timeout, 999);
    mock::forbidden = timeout;
    if (fault == 1) mock::write_forbidden = output + 6;
    const auto copies = mock::copies; mock::now += 1000000000;
    expect(native::private_poll_signal_ready(kernel, *p, *thread));
    expect(native::private_poll_signal_complete(kernel, *p, *thread));
    expect(mock::copies == copies); mock::forbidden = mock::write_forbidden = 0;
    const bool copy_fault = fault == 1 && kind < 2;
    expect(!thread->syscall_pending && !native::poll_objects.resolve(object));
    expect(thread->wait_registration.completion() == sched::WaitCompletion::interrupted);
    expect(kernel.scheduler().open_description(description)->references == 1);
    expect(thread->native_frame.pc == frame.pc);
    expect(thread->native_frame.registers[14] == static_cast<u64>(copy_fault ? -14 : -4));
    expect(get(output) == (kind >= 2 || copy_fault ? 0x1234567812345678 : 0x567812345678));
    // A failed timeout writeback must not replace EINTR/EFAULT.
    expect(get(timeout) == 999);
    expect(!native::private_poll_signal_complete(kernel, *p, *thread));
    expect(native::private_signal_deliver(kernel, *p, *thread));
    expect(thread->native_frame.pc == handler && thread->native_frame.registers[9] == 10);
    retire(kernel, group);
}
void timeout_output(Kernel &kernel, unsigned kind)
{
    auto group = create(kernel); auto pipes = kernel.posix().pipe(0); CHECK(pipes);
    arm(kernel, group, pipes.value()[0], kind, true); caught(kernel, group, true); enqueue(kernel, group);
    auto *p = kernel.scheduler().find(group.pid); auto *thread = kernel.scheduler().find_thread(group.pid, group.waiter);
    mock::now += 1234567890;
    expect(native::private_poll_signal_complete(kernel, *p, *thread));
    expect(get(timeout) == 1 && get(timeout + 8) == (kind == 2 ? 765432 : 765432110));
    retire(kernel, group);
}
void eligibility(Kernel &kernel, unsigned mode)
{
    auto group = create(kernel); auto pipes = kernel.posix().pipe(0); CHECK(pipes);
    arm(kernel, group, pipes.value()[0], 1, true); caught(kernel, group, true); enqueue(kernel, group);
    auto *p = kernel.scheduler().find(group.pid); auto *thread = kernel.scheduler().find_thread(group.pid, group.waiter);
    const auto object = native::poll_object(group.pid, group.waiter); const auto wait = *native::poll_objects.resolve(object).value();
    if (mode == 0) thread->native_signal_mask.set(10);
    if (mode == 1) native::private_signal_table(kernel, group.pid)->find(10)->handler = 1;
    if (mode == 2) ++thread->native_frame.pc;
    if (mode == 3) thread->native_frame.registers[14] = 23;
    if (mode == 4) CHECK(kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::ready));
    if (mode == 5) CHECK(kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::cancelled));
    if (mode == 6) mock::now = wait.timeout.deadline();
    if (mode == 7) { leader(kernel, group); const std::array<std::byte, 1> byte{std::byte{'R'}}; CHECK(kernel.posix().write(pipes.value()[1], byte)); }
    if (mode == 8) { leader(kernel, group); CHECK(kernel.posix().close(pipes.value()[1])); }
    const auto frame = thread->native_frame; const auto outputs = mock::outputs;
    expect(!native::private_poll_signal_ready(kernel, *p, *thread));
    expect(!native::private_poll_signal_complete(kernel, *p, *thread));
    expect(native::poll_objects.resolve(object).ok() && thread->syscall_pending);
    expect(thread->native_frame.pc == frame.pc && thread->native_frame.registers[14] == frame.registers[14]);
    expect(mock::outputs == outputs);
    retire(kernel, group);
}
void ignored_precedes_caught(Kernel &kernel)
{
    auto group = create(kernel); auto pipes = kernel.posix().pipe(0); CHECK(pipes);
    arm(kernel, group, pipes.value()[0], 1, false); caught(kernel, group, false); enqueue(kernel, group);
    caught(kernel, group, false, 2); enqueue(kernel, group, 2);
    native::private_signal_table(kernel, group.pid)->find(2)->handler = 1;
    auto *p = kernel.scheduler().find(group.pid); auto *thread = kernel.scheduler().find_thread(group.pid, group.waiter);
    expect(native::private_poll_signal_complete(kernel, *p, *thread));
    expect(native::private_signal_queue(*thread)->pending_mask().contains(2));
    expect(native::private_signal_deliver(kernel, *p, *thread));
    expect(thread->native_frame.registers[9] == 10);
    retire(kernel, group);
}
void recipient_step(Kernel &kernel, unsigned mode)
{
    auto group = create(kernel); auto pipes = kernel.posix().pipe(0); CHECK(pipes);
    arm(kernel, group, pipes.value()[0], 1, true); caught(kernel, group, true); enqueue(kernel, group);
    auto *p = kernel.scheduler().find(group.pid); auto *thread = kernel.scheduler().find_thread(group.pid, group.waiter);
    const auto wait = *native::poll_objects.resolve(native::poll_object(group.pid, group.waiter)).value();
    if (mode == 1) { leader(kernel, group); const std::array<std::byte, 1> byte{std::byte{'R'}}; CHECK(kernel.posix().write(pipes.value()[1], byte)); }
    if (mode == 2) mock::now = wait.timeout.deadline();
    if (mode == 3) native::private_signal_table(kernel, group.pid)->find(10)->handler = 0;
    if (mode == 4)
    {
        native::X86SignalFrame::Placement placement;
        CHECK(native::X86SignalFrame::place(thread->native_frame.sp, hw::user_begin, hw::user_end, placement));
        mock::write_forbidden = placement.address;
    }
    mock::entry_trap = {hw::TrapKind::timer}; const auto before = mock::entries;
    expect(native::step(kernel)); mock::write_forbidden = 0;
    expect(!native::poll_object(group.pid, group.waiter).pool);
    if (mode == 3 || mode == 4)
    {
        expect(mock::entries == before && !thread->native_active);
        CHECK(kernel.scheduler().reap_orphans()); expect(!kernel.scheduler().find(group.pid));
    }
    else
    {
        expect(mock::entries == before + 1 && mock::last_frame.pc == handler);
        std::array<std::byte, native::X86SignalFrame::prefix_bytes> saved{};
        CHECK(hw::copy_from_user(p->resources().native_space, saved.data(), mock::last_frame.sp, saved.size()));
        expect(native::X86SignalFrame::get(saved, native::X86SignalFrame::context_offset + 128, 8) == site + 2);
        expect(native::X86SignalFrame::get(saved, native::X86SignalFrame::context_offset + 104, 8) ==
            (mode == 0 ? static_cast<u64>(-4) : mode == 1 ? 1 : 0));
        expect(!thread->syscall_pending);
        retire(kernel, group);
    }
}
} // namespace
int main()
{
    static_assert(lu::configured_native_profile() == lu::Profile::x86_64);
    static Kernel kernel; CHECK(kernel.boot({})); CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    for (unsigned kind = 0; kind < 4; ++kind) for (bool timed : {false, true})
        for (bool restart : {false, true}) for (unsigned fault : {0U, 1U}) publication(kernel, kind, timed, restart, fault);
    for (unsigned kind : {1U, 2U, 3U}) timeout_output(kernel, kind);
    for (unsigned mode = 0; mode < 9; ++mode) eligibility(kernel, mode);
    ignored_precedes_caught(kernel);
    for (unsigned mode = 0; mode < 5; ++mode) recipient_step(kernel, mode);
    expect(native::entry_stacks.accounting().leased_slots == 0);
    expect(native::poll_objects.accounting().live_objects == 0);
    std::printf("POLL_SIGNAL_MODEL_PASS hosted=1 native=0 checks=%zu\n", checks);
}
