// Actual x86 runtime origins, private signals and sigreturn; hosted trap/copy only.
#define OK_ENABLE_TEST_POINTS 1
#define OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE 1
#define OK_SYSCALL_ORIGIN_MODEL 1
#define OK_PIPE_WRITE_SIGNAL_MODEL 1
#include "native_clock_model.hpp"
#include <source_location>
namespace
{
constexpr uptr array_address = hw::user_begin + 512, site = hw::user_begin + 1024;
constexpr uptr handler = hw::user_begin + 2048, payload = hw::user_begin + 3072;
usize checks{};
void expect(bool value, std::source_location at = std::source_location::current())
{
    ++checks;
    if (!value) { std::fprintf(stderr, "PIPE_VECTOR_SIGNAL_FAIL line=%u checks=%zu\n", at.line(), checks); std::abort(); }
}
struct Group { sched::ProcessId pid; sched::ThreadId peer; };
Group create(Kernel &kernel)
{
    auto made = kernel.scheduler().create_user_process("pipe-vector-signals", kernel.arch().make_user_context({hw::user_begin, hw::user_begin+8192})); CHECK(made);
    auto *p = kernel.scheduler().find(made.value()); p->resources().personality = sched::UserPersonality::linux;
    p->resources().native_space = {0x100000 + made.value()*4096, static_cast<u32>(made.value()%16), 1};
    CHECK(native::allocate(p->resources().native_pages, hw::user_begin, hw::page_read | hw::page_execute));
    auto peer = kernel.scheduler().create_thread(p->pid(), p->threads()[0].context); CHECK(peer);
    for (auto &thread : p->threads()) { CHECK(native::prepare_native_thread(thread)); thread.native_active = true; }
    p->threads()[0].native_active = false; CHECK(kernel.scheduler().set_runnable(p->pid()));
    CHECK(kernel.scheduler().activate_thread(p->pid(), p->pid())); CHECK(kernel.posix().activate(p->pid()));
    native::runtime_kernel = &kernel; native::started = true; return {p->pid(), peer.value()};
}
void leader(Kernel &kernel, Group group)
{ CHECK(kernel.scheduler().activate_thread(group.pid, group.pid)); CHECK(kernel.posix().activate(group.pid)); }
void retire(Kernel &kernel, Group group)
{
    mock::forbidden = mock::write_forbidden = 0; native::release_process(kernel, group.pid);
    CHECK(kernel.scheduler().exit_process(group.pid, 0)); CHECK(kernel.scheduler().reap_orphans());
    expect(native::pipe_read_objects.accounting().live_objects == 0 && native::pipe_write_objects.accounting().live_objects == 0);
    expect(native::runtime_status().ok());
}
void metadata(uptr address = payload)
{
    const std::array<native::PipeVector, 4> values{{{address, 1}, {hw::user_end-1, 0}, {address+16, 1}, {address+32, 1}}};
    std::memcpy(mock::memory.data()+array_address-hw::user_begin, values.data(), sizeof(values));
}
template <bool Writing> auto object(Group group)
{
    if constexpr (Writing) return native::pipe_write_object(group.pid, group.peer);
    else return native::pipe_read_object(group.pid, group.peer);
}
template <bool Writing> auto &wait(Group group)
{
    if constexpr (Writing) return *native::pipe_write_objects.resolve(object<Writing>(group)).value();
    else return *native::pipe_read_objects.resolve(object<Writing>(group)).value();
}
template <bool Writing> auto binding(Group group)
{
    if constexpr (Writing) return native::pipe_write_origin_binding(object<Writing>(group), wait<Writing>(group));
    else return native::pipe_origin_binding(object<Writing>(group), wait<Writing>(group));
}
template <bool Writing> bool ready(Kernel &kernel, sched::ProcessControlBlock &p, sched::ThreadControlBlock &thread)
{
    if constexpr (Writing) return native::private_pipe_write_signal_ready(kernel, p, thread);
    else return native::private_pipe_signal_ready(kernel, p, thread);
}
template <bool Writing> bool interrupt(Kernel &kernel, sched::ProcessControlBlock &p, sched::ThreadControlBlock &thread)
{
    if constexpr (Writing) return native::private_pipe_write_signal_complete(kernel, p, thread);
    else return native::private_pipe_signal_complete(kernel, p, thread);
}
template <bool Writing> auto make_pipe(Kernel &kernel)
{
    auto fds = kernel.posix().pipe(0); CHECK(fds);
    if constexpr (Writing)
    {
        std::array<std::byte, sched::pipe_capacity> data{};
        auto wrote = kernel.posix().write(fds.value()[1], data); CHECK(wrote && wrote.value() == data.size());
    }
    return fds.value();
}
template <bool Writing> void arm(Kernel &kernel, Group group, i32 fd)
{
    metadata(); auto *thread = kernel.scheduler().find_thread(group.pid, group.peer); auto &f = thread->native_frame;
    f = {}; f.flags = 0x202; f.sp = hw::user_begin+8192; f.pc = site+2;
    const auto fp = native::X86SignalFrame::reset_fp(); for (usize i = 0; i < fp.size(); ++i) f.fp_state[i] = static_cast<u8>(fp[i]);
    f.registers[9] = fd; f.registers[10] = array_address; f.registers[11] = 4; f.registers[14] = Writing ? 20 : 19;
    mock::memory[site-hw::user_begin] = std::byte{0x0f}; mock::memory[site-hw::user_begin+1] = std::byte{0x05};
    mock::entry_trap = {hw::TrapKind::syscall, 256}; native::last_pid = native::last_tid = 0;
    expect(native::step(kernel)); expect(thread->syscall_pending && object<Writing>(group).pool);
    expect(native::syscall_origins.live(native::syscall_origins.identity(*kernel.scheduler().find(group.pid), *thread)));
}
void signal(Kernel &kernel, Group group, bool restart)
{
    auto *action = native::private_signal_table(kernel, group.pid)->find(10); CHECK(action);
    action->handler = handler; action->restorer = handler+4; action->flags = 0x04000004 | (restart ? 0x10000000 : 0);
    auto *thread = kernel.scheduler().find_thread(group.pid, group.peer);
    expect(native::private_signal_queue(*thread)->enqueue({10, -6, static_cast<u32>(group.pid), 0}) == native::PrivateSignalQueue::Enqueue::queued);
}
template <bool Writing> void publication(Kernel &kernel, bool restart, unsigned mode)
{
    auto group = create(kernel); auto fds = make_pipe<Writing>(kernel); arm<Writing>(kernel, group, fds[Writing ? 1 : 0]);
    auto *p = kernel.scheduler().find(group.pid); auto *thread = kernel.scheduler().find_thread(group.pid, group.peer);
    auto saved = wait<Writing>(group); auto old_binding = binding<Writing>(group);
    const auto origin = native::syscall_origins.identity(*p, *thread); auto original = thread->native_frame;
    // The live continuation never rereads the array, even for signal eligibility.
    metadata(payload+64); mock::forbidden = array_address;
    if (mode == 1) CHECK(kernel.scheduler().complete_wait(saved.ticket, sched::WaitCompletion::ready));
    if (mode == 2)
    {
        leader(kernel, group); auto other = make_pipe<Writing>(kernel);
        CHECK(kernel.posix().duplicate_to(other[Writing ? 1 : 0], fds[Writing ? 1 : 0]));
    }
    signal(kernel, group, restart); expect(ready<Writing>(kernel, *p, *thread));
    expect(interrupt<Writing>(kernel, *p, *thread)); expect(!object<Writing>(group).pool && !thread->syscall_pending);
    expect(!native::syscall_origins.live(origin)); expect(!native::syscall_origins.prepare_restart(origin, original, old_binding));
    expect(thread->native_frame.pc == (restart ? site : site+2));
    expect(thread->native_frame.registers[14] == (restart ? (Writing ? 20U : 19U) : static_cast<u64>(-4)));
    expect(thread->native_frame.registers[10] == array_address && thread->native_frame.registers[11] == 4);
    expect(!interrupt<Writing>(kernel, *p, *thread)); expect(native::private_signal_deliver(kernel, *p, *thread));
    thread->native_frame.sp += 8; thread->native_frame.registers[14] = 15;
    bool complete = true; i64 result = -999; expect(native::private_signal_call(kernel, *p, *thread, complete, result));
    expect(!complete && result == 0 && thread->native_frame.pc == (restart ? site : site+2));
    mock::forbidden = 0;
    if (restart)
    {
        // Reexecute the restored call frame after sigreturn. The new request
        // admits the peer's changed array, without reusing the retired snapshot.
        thread->native_frame.pc += 2; native::last_pid = native::last_tid = 0; expect(native::step(kernel));
        auto next = wait<Writing>(group);
        expect(next.request > saved.request && next.vectors.entries[0].base == payload+64);
        expect(next.vectors != saved.vectors && next.vectors.count == saved.vectors.count);
    }
    retire(kernel, group);
}
template <bool Writing> void corruption(Kernel &kernel, unsigned mode)
{
    auto group = create(kernel); auto fds = make_pipe<Writing>(kernel); arm<Writing>(kernel, group, fds[Writing ? 1 : 0]);
    auto *p = kernel.scheduler().find(group.pid); auto *thread = kernel.scheduler().find_thread(group.pid, group.peer);
    const auto origin = native::syscall_origins.identity(*p, *thread); auto before = wait<Writing>(group);
    const auto valid = binding<Writing>(group); auto permit = native::syscall_origins.prepare_restart(origin, thread->native_frame, valid); expect(permit.ok());
    auto &owned = wait<Writing>(group);
    switch (mode)
    {
    case 0: ++owned.vectors.entries[0].base; break;
    case 1: ++owned.vectors.entries[0].length; break;
    case 2: ++owned.vectors.entries[1].base; break;
    case 3: ++owned.vectors.count; break;
    case 4: owned.vectors.count = 0; break;
    case 5: ++owned.vectors.entries[15].base; break;
    case 6: ++owned.request; break;
    case 7: ++owned.count; break;
    case 8: ++owned.fd; break;
    case 9: owned.vectors.entries[2].length = static_cast<usize>(__INTPTR_MAX__); break;
    }
    signal(kernel, group, true); expect(!ready<Writing>(kernel, *p, *thread)); expect(!interrupt<Writing>(kernel, *p, *thread));
    const auto wrong = binding<Writing>(group);
    expect(!native::syscall_origins.validate_restart(permit.value(), origin, thread->native_frame, wrong));
    expect(!native::syscall_origins.consume_restart(permit.value(), origin, thread->native_frame, wrong));
    expect(native::private_signal_queue(*thread)->pending_mask().contains(10)); owned = before;
    expect(ready<Writing>(kernel, *p, *thread)); expect(interrupt<Writing>(kernel, *p, *thread)); retire(kernel, group);
}
template <bool Writing> void rearm(Kernel &kernel)
{
    auto group = create(kernel); auto fds = make_pipe<Writing>(kernel); arm<Writing>(kernel, group, fds[Writing ? 1 : 0]);
    auto before = wait<Writing>(group); auto *p = kernel.scheduler().find(group.pid); auto *thread = kernel.scheduler().find_thread(group.pid, group.peer);
    auto origin = native::syscall_origins.identity(*p, *thread); auto old = binding<Writing>(group);
    auto permit = native::syscall_origins.prepare_restart(origin, thread->native_frame, old); expect(permit.ok());
    CHECK(kernel.scheduler().complete_wait(before.ticket, sched::WaitCompletion::ready));
    CHECK(kernel.scheduler().activate_thread(group.pid, group.peer)); bool complete = true;
    i64 result;
    if constexpr (Writing) result = native::resume_pipe_write(kernel, *p, object<Writing>(group), complete);
    else result = native::resume_pipe_read(kernel, *p, object<Writing>(group), complete);
    expect(result == -11 && !complete); auto after = wait<Writing>(group);
    expect(after.request == before.request && after.vectors == before.vectors && after.ticket.generation > before.ticket.generation);
    expect(!native::syscall_origins.validate_restart(permit.value(), origin, thread->native_frame, old));
    auto current = binding<Writing>(group); auto new_permit = native::syscall_origins.prepare_restart(origin, thread->native_frame, current); expect(new_permit.ok());
    expect(native::syscall_origins.consume_restart(new_permit.value(), origin, thread->native_frame, current));
    expect(!native::syscall_origins.consume_restart(new_permit.value(), origin, thread->native_frame, current)); retire(kernel, group);
}
} // namespace
int main()
{
    static_assert(lu::configured_native_profile() == lu::Profile::x86_64);
    static Kernel kernel; CHECK(kernel.boot({})); CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    for (bool restart : {false, true}) for (unsigned mode = 0; mode < 3; ++mode)
    { publication<false>(kernel, restart, mode); publication<true>(kernel, restart, mode); }
    for (unsigned mode = 0; mode < 10; ++mode) { corruption<false>(kernel, mode); corruption<true>(kernel, mode); }
    rearm<false>(kernel); rearm<true>(kernel);
    expect(native::entry_stacks.accounting().leased_slots == 0);
    std::printf("PIPE_VECTOR_SIGNAL_PASS hosted=1 native=0 checks=%zu\n", checks);
}
