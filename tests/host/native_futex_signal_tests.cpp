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
    if (!value) { std::fprintf(stderr, "FUTEX_SIGNAL_MODEL_FAIL line=%u checks=%zu\n", at.line(), checks); std::abort(); }
}
constexpr uptr word = hw::user_begin + 128, timeout = hw::user_begin + 256;
constexpr uptr site = hw::user_begin + 1024, handler = hw::user_begin + 2048;
constexpr u64 futex_number = *lu::Codec<lu::Profile::x86_64>::syscall_number(lu::Operation::futex);
struct Group { sched::ProcessId pid{}; sched::ThreadId waiter{}; };
void put(uptr address, u64 value, usize bytes = 8)
{ for (usize i = 0; i < bytes; ++i) mock::memory[address - hw::user_begin + i] = std::byte(value >> (i * 8)); }
Group create(Kernel &kernel)
{
    auto made = kernel.scheduler().create_user_process("futex-signal-model",
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
    expect(native::futex_objects.accounting().live_objects == 0);
    expect(native::runtime_status().ok());
}
void arm(Kernel &kernel, Group group, bool timed, u32 operation = 128)
{
    auto *thread = kernel.scheduler().find_thread(group.pid, group.waiter);
    auto &f = thread->native_frame; f = {}; f.flags = 0x202; f.sp = hw::user_begin + 8192; f.pc = site + 2;
    const auto fp = native::X86SignalFrame::reset_fp();
    for (usize i = 0; i < fp.size(); ++i) f.fp_state[i] = static_cast<u8>(fp[i]);
    f.registers[14] = futex_number; f.registers[9] = word; f.registers[10] = operation;
    f.registers[11] = 7; f.registers[5] = timed ? timeout : 0;
    put(word, 7, 4); put(timeout, 3); put(timeout + 8, 19);
    mock::memory[site - hw::user_begin] = std::byte{0x0f}; mock::memory[site - hw::user_begin + 1] = std::byte{0x05};
    mock::entry_trap = {hw::TrapKind::syscall, 256}; native::last_pid = native::last_tid = 0;
    expect(native::step(kernel));
    expect(thread->syscall_pending && thread->wait_reason == sched::ThreadWaitReason::futex);
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
    auto *thread = kernel.scheduler().find_thread(group.pid, group.waiter);
    auto *queue = native::private_signal_queue(*thread); CHECK(queue);
    expect(queue->enqueue({signal, -6, static_cast<u32>(group.pid), 0}) == native::PrivateSignalQueue::Enqueue::queued);
}
void publication(Kernel &kernel, bool timed, bool sa_restart, u32 operation)
{
    auto group = create(kernel); arm(kernel, group, timed, operation);
    caught(kernel, group, !sa_restart); enqueue(kernel, group); caught(kernel, group, sa_restart);
    auto *p = kernel.scheduler().find(group.pid); auto *thread = kernel.scheduler().find_thread(group.pid, group.waiter);
    const auto object = native::futex_object(group.pid, group.waiter);
    const auto wait = *native::futex_objects.resolve(object).value();
    const auto origin = native::syscall_origins.identity(*p, *thread);
    const auto frame = thread->native_frame;
    const auto binding = native::futex_origin_binding(object, wait);
    expect(native::syscall_origins.validate_bound(origin, frame, binding));
    expect(native::syscall_origins.prepare_restart(origin, frame, binding).ok() == !timed);
    // No completion path may fetch the original timeout or word again.
    put(timeout, 0); put(timeout + 8, 1000000000); put(word, 99, 4);
    mock::forbidden = timed ? timeout : word;
    const auto copies = mock::copies;
    expect(native::private_futex_signal_ready(kernel, *p, *thread));
    expect(native::private_futex_signal_complete(kernel, *p, *thread));
    expect(mock::copies == copies);
    mock::forbidden = 0;
    const bool restart = !timed && sa_restart;
    expect(!thread->syscall_pending && !native::futex_object(group.pid, group.waiter).pool);
    expect(!native::syscall_origins.live(origin) && !native::futex_objects.resolve(object));
    expect(thread->wait_registration.completion() == sched::WaitCompletion::interrupted);
    expect(thread->native_frame.pc == (restart ? site : site + 2));
    expect(thread->native_frame.registers[14] == (restart ? futex_number : static_cast<u64>(-4)));
    for (usize i : {9, 10, 11, 5}) expect(thread->native_frame.registers[i] == frame.registers[i]);
    expect(!native::private_futex_signal_complete(kernel, *p, *thread));
    expect(native::private_signal_deliver(kernel, *p, *thread));
    expect(thread->native_frame.pc == handler && thread->native_frame.registers[9] == 10);
    expect(!native::private_signal_queue(*thread)->pending_mask().contains(10));
    retire(kernel, group);
}
void eligibility(Kernel &kernel, unsigned mode)
{
    auto group = create(kernel); arm(kernel, group, mode == 9); caught(kernel, group, true); enqueue(kernel, group);
    auto *p = kernel.scheduler().find(group.pid); auto *thread = kernel.scheduler().find_thread(group.pid, group.waiter);
    const auto before = thread->native_frame; const auto object = native::futex_object(group.pid, group.waiter);
    auto &wait = *native::futex_objects.resolve(object).value();
    if (mode == 0) thread->native_signal_mask.set(10);
    if (mode == 1) native::private_signal_table(kernel, group.pid)->find(10)->handler = 1;
    if (mode == 2) native::syscall_origins.revoke(*thread);
    if (mode == 3) ++thread->native_frame.registers[7];
    if (mode == 4) CHECK(kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::ready));
    if (mode == 5) CHECK(kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::timeout));
    if (mode == 6) CHECK(kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::cancelled));
    if (mode == 7)
    {
        CHECK(native::entry_stacks.activate_entry(thread->entry_stack, {group.pid, group.waiter}, 0));
        CHECK(native::entry_stacks.deactivate_entry(thread->entry_stack, {group.pid, group.waiter}, 0));
    }
    if (mode == 8)
    {
        CHECK(kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::ready));
        CHECK(kernel.scheduler().activate_thread(group.pid, group.waiter));
        CHECK(kernel.scheduler().register_current_wait(sched::ThreadWaitReason::deadline, 909));
    }
    if (mode == 9) mock::now = wait.timeout.deadline();
    if (mode == 10) ++wait.expected;
    expect(!native::private_futex_signal_ready(kernel, *p, *thread));
    expect(!native::private_futex_signal_complete(kernel, *p, *thread));
    expect(native::futex_objects.resolve(object).ok() && thread->syscall_pending);
    expect(thread->native_frame.pc == before.pc && thread->native_frame.registers[14] == before.registers[14]);
    expect(native::private_signal_queue(*thread)->pending_mask().contains(10));
    retire(kernel, group);
}
void recipient_step(Kernel &kernel, unsigned mode)
{
    auto group = create(kernel); arm(kernel, group, mode == 2); caught(kernel, group, true); enqueue(kernel, group);
    auto *p = kernel.scheduler().find(group.pid); auto *thread = kernel.scheduler().find_thread(group.pid, group.waiter);
    const auto ticket = thread->wait_registration.ticket();
    if (mode == 1) CHECK(kernel.scheduler().complete_wait(ticket, sched::WaitCompletion::ready));
    if (mode == 2) mock::now = native::futex_objects.resolve(native::futex_object(group.pid, group.waiter)).value()->timeout.deadline();
    if (mode == 3) native::private_signal_table(kernel, group.pid)->find(10)->handler = 0;
    if (mode == 4)
    {
        native::X86SignalFrame::Placement placement;
        CHECK(native::X86SignalFrame::place(thread->native_frame.sp, hw::user_begin, hw::user_end, placement));
        mock::write_forbidden = placement.address;
    }
    mock::entry_trap = {hw::TrapKind::timer}; const auto before = mock::entries;
    expect(native::step(kernel));
    mock::write_forbidden = 0;
    expect(!native::futex_object(group.pid, group.waiter).pool);
    if (mode == 3 || mode == 4)
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
        expect(native::X86SignalFrame::get(saved, native::X86SignalFrame::context_offset + 128, 8) == (mode == 0 ? site : site + 2));
        expect(native::X86SignalFrame::get(saved, native::X86SignalFrame::context_offset + 104, 8) ==
            (mode == 0 ? futex_number : mode == 1 ? 0 : static_cast<u64>(-110)));
        expect(!thread->syscall_pending);
        retire(kernel, group);
    }
}
} // namespace
int main()
{
    static_assert(lu::configured_native_profile() == lu::Profile::x86_64);
    static Kernel kernel; CHECK(kernel.boot({})); CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    for (bool timed : {false, true}) for (bool restart : {false, true}) for (u32 op : {0U, 128U})
        publication(kernel, timed, restart, op);
    for (unsigned mode = 0; mode < 11; ++mode) eligibility(kernel, mode);
    for (unsigned mode = 0; mode < 5; ++mode) recipient_step(kernel, mode);
    expect(native::entry_stacks.accounting().leased_slots == 0);
    expect(native::futex_objects.accounting().live_objects == 0);
    std::printf("FUTEX_SIGNAL_MODEL_PASS hosted=1 native=0 checks=%zu\n", checks);
}
