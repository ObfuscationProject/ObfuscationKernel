// Actual private x86 runtime and scheduler; synthetic trap/copy backends only.
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
    if (!value) { std::fprintf(stderr, "PIPE_SIGNAL_MODEL_FAIL line=%u checks=%zu\n", at.line(), checks); std::abort(); }
}
constexpr uptr output = hw::user_begin + 128, site = hw::user_begin + 1024, handler = hw::user_begin + 2048;
struct Group { sched::ProcessId pid{}; sched::ThreadId reader{}; };
Group create(Kernel &kernel)
{
    auto made = kernel.scheduler().create_user_process("pipe-signal-model",
        kernel.arch().make_user_context({hw::user_begin, hw::user_begin + 8192})); CHECK(made);
    auto *p = kernel.scheduler().find(made.value());
    p->resources().personality = sched::UserPersonality::linux;
    p->resources().native_space = {0x100000 + made.value() * 4096, static_cast<u32>(made.value() % 16), 1};
    CHECK(native::allocate(p->resources().native_pages, hw::user_begin, hw::page_read | hw::page_execute));
    auto peer = kernel.scheduler().create_thread(p->pid(), p->threads()[0].context); CHECK(peer);
    for (auto &thread : p->threads()) { CHECK(native::prepare_native_thread(thread)); thread.native_active = true; }
    p->threads()[0].native_active = false;
    CHECK(kernel.scheduler().set_runnable(p->pid()));
    CHECK(kernel.scheduler().activate_thread(p->pid(), p->pid())); CHECK(kernel.posix().activate(p->pid()));
    native::runtime_kernel = &kernel; native::started = true;
    return {p->pid(), peer.value()};
}
void leader(Kernel &kernel, Group group)
{ CHECK(kernel.scheduler().activate_thread(group.pid, group.pid)); CHECK(kernel.posix().activate(group.pid)); }
void retire(Kernel &kernel, Group group)
{
    native::release_process(kernel, group.pid);
    CHECK(kernel.scheduler().exit_process(group.pid, 0)); CHECK(kernel.scheduler().reap_orphans());
    expect(native::pipe_read_objects.accounting().live_objects == 0);
    expect(native::runtime_status().ok());
}
void arm(Kernel &kernel, Group group, i32 fd)
{
    auto *thread = kernel.scheduler().find_thread(group.pid, group.reader);
    auto &f = thread->native_frame; f = {}; f.flags = 0x202; f.sp = hw::user_begin + 8192; f.pc = site + 2;
    const auto fp = native::X86SignalFrame::reset_fp();
    for (usize i = 0; i < fp.size(); ++i) f.fp_state[i] = static_cast<u8>(fp[i]);
    f.registers[9] = fd; f.registers[10] = output; f.registers[11] = 3;
    mock::memory[site - hw::user_begin] = std::byte{0x0f}; mock::memory[site - hw::user_begin + 1] = std::byte{0x05};
    mock::entry_trap = {hw::TrapKind::syscall, 256}; native::last_pid = native::last_tid = 0;
    expect(native::step(kernel));
    expect(thread->syscall_pending && thread->wait_reason == sched::ThreadWaitReason::pipe_read);
    expect(native::syscall_origins.live(native::syscall_origins.identity(*kernel.scheduler().find(group.pid), *thread)));
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
    auto *thread = kernel.scheduler().find_thread(group.pid, group.reader);
    auto *queue = native::private_signal_queue(*thread); CHECK(queue);
    expect(queue->enqueue({signal, -6, static_cast<u32>(group.pid), 0}) == native::PrivateSignalQueue::Enqueue::queued);
}
void publication(Kernel &kernel, bool restart, bool already_ready, bool fd_reused)
{
    auto group = create(kernel); auto pipes = kernel.posix().pipe(0); CHECK(pipes);
    auto other = kernel.posix().pipe(0); CHECK(other);
    arm(kernel, group, pipes.value()[0]); caught(kernel, group, !restart); enqueue(kernel, group);
    caught(kernel, group, restart); // Recipient must observe the current action.
    auto *p = kernel.scheduler().find(group.pid); auto *thread = kernel.scheduler().find_thread(group.pid, group.reader);
    const auto object = native::pipe_read_object(group.pid, group.reader);
    const auto wait = *native::pipe_read_objects.resolve(object).value();
    const auto origin = native::syscall_origins.identity(*p, *thread);
    const auto frame = thread->native_frame;
    if (fd_reused) { leader(kernel, group); CHECK(kernel.posix().duplicate_to(other.value()[0], pipes.value()[0])); }
    else if (already_ready) CHECK(kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::ready));
    expect(native::private_pipe_signal_ready(kernel, *p, *thread));
    expect(native::private_pipe_signal_complete(kernel, *p, *thread));
    expect(!thread->syscall_pending && !native::pipe_read_object(group.pid, group.reader).pool);
    expect(!native::syscall_origins.live(origin) && !native::pipe_read_objects.resolve(object));
    expect(thread->native_frame.pc == (restart ? site : site + 2));
    expect(thread->native_frame.registers[14] == (restart ? 0 : static_cast<u64>(-4)));
    expect(thread->native_frame.registers[9] == frame.registers[9]);
    expect(thread->native_frame.registers[10] == frame.registers[10]);
    expect(thread->native_frame.registers[11] == frame.registers[11]);
    const auto *description = kernel.scheduler().open_description(wait.description);
    expect(fd_reused ? description == nullptr : description && description->references == 1);
    expect(!native::private_pipe_signal_complete(kernel, *p, *thread));
    expect(native::private_signal_deliver(kernel, *p, *thread));
    expect(thread->native_frame.pc == handler && thread->native_frame.registers[9] == 10);
    expect(!native::private_signal_queue(*thread)->pending_mask().contains(10));
    retire(kernel, group);
}
void eligibility(Kernel &kernel, unsigned mode)
{
    auto group = create(kernel); auto pipes = kernel.posix().pipe(0); CHECK(pipes);
    arm(kernel, group, pipes.value()[0]); caught(kernel, group, true); enqueue(kernel, group);
    auto *p = kernel.scheduler().find(group.pid); auto *thread = kernel.scheduler().find_thread(group.pid, group.reader);
    const auto before = thread->native_frame; const auto object = native::pipe_read_object(group.pid, group.reader);
    const auto wait = *native::pipe_read_objects.resolve(object).value();
    if (mode == 0) thread->native_signal_mask.set(10);
    if (mode == 1) native::private_signal_table(kernel, group.pid)->find(10)->handler = 1;
    if (mode == 2) native::syscall_origins.revoke(*thread);
    if (mode == 3) ++thread->native_frame.registers[7];
    if (mode == 4)
    {
        leader(kernel, group); const std::array<std::byte, 1> byte{std::byte{'R'}};
        CHECK(kernel.posix().write(pipes.value()[1], byte));
    }
    if (mode == 5) { leader(kernel, group); CHECK(kernel.posix().close(pipes.value()[1])); }
    if (mode == 6) CHECK(kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::cancelled));
    if (mode == 7)
    {
        CHECK(native::entry_stacks.activate_entry(thread->entry_stack, {group.pid, group.reader}, 0));
        CHECK(native::entry_stacks.deactivate_entry(thread->entry_stack, {group.pid, group.reader}, 0));
    }
    if (mode == 8)
    {
        CHECK(kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::ready));
        CHECK(kernel.scheduler().activate_thread(group.pid, group.reader));
        CHECK(kernel.scheduler().register_current_wait(sched::ThreadWaitReason::deadline, 909));
    }
    expect(!native::private_pipe_signal_ready(kernel, *p, *thread));
    expect(!native::private_pipe_signal_complete(kernel, *p, *thread));
    expect(native::pipe_read_objects.resolve(object).ok() && thread->syscall_pending);
    expect(thread->native_frame.pc == before.pc && thread->native_frame.registers[14] == before.registers[14]);
    expect(native::private_signal_queue(*thread)->pending_mask().contains(10));
    retire(kernel, group);
}
void ignored_before_caught(Kernel &kernel)
{
    auto group = create(kernel); auto pipes = kernel.posix().pipe(0); CHECK(pipes);
    arm(kernel, group, pipes.value()[0]); caught(kernel, group, false); enqueue(kernel, group);
    native::private_signal_table(kernel, group.pid)->find(4)->handler = 1; enqueue(kernel, group, 4);
    auto *p = kernel.scheduler().find(group.pid); auto *thread = kernel.scheduler().find_thread(group.pid, group.reader);
    expect(native::private_pipe_signal_complete(kernel, *p, *thread));
    expect(native::private_signal_deliver(kernel, *p, *thread));
    expect(thread->native_frame.pc == handler && thread->native_frame.registers[9] == 10);
    expect(!native::private_signal_queue(*thread)->pending_mask().contains(4));
    retire(kernel, group);
}
void recipient_step(Kernel &kernel, unsigned mode)
{
    auto group = create(kernel); auto pipes = kernel.posix().pipe(0); CHECK(pipes);
    arm(kernel, group, pipes.value()[0]); caught(kernel, group, true); enqueue(kernel, group);
    auto *p = kernel.scheduler().find(group.pid); auto *thread = kernel.scheduler().find_thread(group.pid, group.reader);
    const auto ticket = thread->wait_registration.ticket();
    if (mode == 1) CHECK(kernel.scheduler().complete_wait(ticket, sched::WaitCompletion::ready));
    if (mode == 2) native::private_signal_table(kernel, group.pid)->find(10)->handler = 0;
    mock::entry_trap = {hw::TrapKind::timer}; const auto before = mock::entries;
    expect(native::step(kernel));
    expect(!native::pipe_read_object(group.pid, group.reader).pool);
    if (mode == 2)
    {
        expect(mock::entries == before && !thread->native_active);
        CHECK(kernel.scheduler().reap_orphans());
        expect(!kernel.scheduler().find(group.pid));
    }
    else
    {
        expect(mock::entries == before + 1 && mock::last_frame.pc == handler);
        std::array<std::byte, native::X86SignalFrame::prefix_bytes> saved{};
        CHECK(hw::copy_from_user(p->resources().native_space, saved.data(), mock::last_frame.sp, saved.size()));
        expect(native::X86SignalFrame::get(saved, native::X86SignalFrame::context_offset + 128, 8) == site);
        expect(native::X86SignalFrame::get(saved, native::X86SignalFrame::context_offset + 104, 8) == 0);
        expect(!thread->syscall_pending);
        retire(kernel, group);
    }
}
} // namespace
int main()
{
    static_assert(lu::configured_native_profile() == lu::Profile::x86_64);
    static Kernel kernel; CHECK(kernel.boot({})); CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    for (bool restart : {false, true}) for (unsigned wake = 0; wake < 3; ++wake)
        publication(kernel, restart, wake == 1, wake == 2);
    for (unsigned mode = 0; mode < 9; ++mode) eligibility(kernel, mode);
    ignored_before_caught(kernel);
    for (unsigned mode = 0; mode < 3; ++mode) recipient_step(kernel, mode);
    expect(native::entry_stacks.accounting().leased_slots == 0);
    std::printf("PIPE_SIGNAL_MODEL_PASS hosted=1 native=0 checks=%zu\n", checks);
}
