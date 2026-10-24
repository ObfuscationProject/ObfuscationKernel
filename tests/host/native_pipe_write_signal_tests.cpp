// Actual private x86 runtime and scheduler; synthetic trap/copy backends only.
#define OK_ENABLE_TEST_POINTS 1
#define OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE 1
#define OK_SYSCALL_ORIGIN_MODEL 1
#define OK_PIPE_WRITE_SIGNAL_MODEL 1
#include "native_clock_model.hpp"
#include <source_location>
namespace
{
usize checks{};
void expect(bool value, std::source_location at = std::source_location::current())
{
    ++checks;
    if (!value) { std::fprintf(stderr, "PIPE_WRITE_SIGNAL_MODEL_FAIL line=%u checks=%zu\n", at.line(), checks); std::abort(); }
}
constexpr uptr input = hw::user_begin + 3072, site = hw::user_begin + 1024, handler = hw::user_begin + 2048;
struct Group { sched::ProcessId pid{}; sched::ThreadId writer{}; };
Group create(Kernel &kernel)
{
    auto made = kernel.scheduler().create_user_process("pipe-write-signal-model",
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
    expect(native::pipe_write_objects.accounting().live_objects == 0);
    expect(native::runtime_status().ok());
}
void arm(Kernel &kernel, Group group, i32 fd, usize count = 3)
{
    auto *thread = kernel.scheduler().find_thread(group.pid, group.writer);
    auto &f = thread->native_frame; f = {}; f.flags = 0x202; f.sp = hw::user_begin + 8192; f.pc = site + 2;
    const auto fp = native::X86SignalFrame::reset_fp();
    for (usize i = 0; i < fp.size(); ++i) f.fp_state[i] = static_cast<u8>(fp[i]);
    f.registers[9] = fd; f.registers[10] = input; f.registers[11] = count; f.registers[14] = 1;
    mock::memory[site - hw::user_begin] = std::byte{0x0f}; mock::memory[site - hw::user_begin + 1] = std::byte{0x05};
    mock::entry_trap = {hw::TrapKind::syscall, 256}; native::last_pid = native::last_tid = 0;
    expect(native::step(kernel));
    expect(thread->syscall_pending && thread->wait_reason == sched::ThreadWaitReason::pipe_write);
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
    auto *thread = kernel.scheduler().find_thread(group.pid, group.writer);
    auto *queue = native::private_signal_queue(*thread); CHECK(queue);
    expect(queue->enqueue({signal, -6, static_cast<u32>(group.pid), 0}) == native::PrivateSignalQueue::Enqueue::queued);
}

std::array<i32, 2> full_pipe(Kernel &kernel, usize filled = sched::pipe_capacity)
{
    auto made = kernel.posix().pipe(0); CHECK(made);
    std::array<std::byte, sched::pipe_capacity> bytes{};
    bytes.fill(std::byte{0x41});
    auto result = kernel.posix().write(made.value()[1], {bytes.data(), filled});
    expect(result && result.value() == filled);
    for (usize i = 0; i <= sched::pipe_capacity; ++i)
        mock::memory[input - hw::user_begin + i] = std::byte{0x42};
    return made.value();
}

usize drain(Kernel &kernel, i32 fd, usize count)
{
    std::array<std::byte, sched::pipe_capacity> bytes{};
    auto result = kernel.posix().read(fd, {bytes.data(), count}); CHECK(result);
    return result.value();
}

i64 resume(Kernel &kernel, Group group, memory::ObjectHandle object, bool &complete)
{
    CHECK(kernel.posix().activate(group.pid));
    CHECK(kernel.scheduler().activate_thread(group.pid, group.writer));
    return native::resume_pipe_write(kernel, *kernel.scheduler().find(group.pid), object, complete);
}

void publication(Kernel &kernel, bool restart, unsigned mode)
{
    const auto group = create(kernel); auto fds = full_pipe(kernel);
    auto other = kernel.posix().pipe(0); CHECK(other);
    arm(kernel, group, fds[1]); caught(kernel, group, !restart); enqueue(kernel, group); caught(kernel, group, restart);
    auto *p = kernel.scheduler().find(group.pid); auto *thread = kernel.scheduler().find_thread(group.pid, group.writer);
    const auto object = native::pipe_write_object(group.pid, group.writer);
    const auto wait = *native::pipe_write_objects.resolve(object).value();
    const auto origin = native::syscall_origins.identity(*p, *thread); const auto frame = thread->native_frame;
    if (mode == 1) CHECK(kernel.scheduler().complete_wait(wait.ticket, sched::WaitCompletion::ready));
    if (mode == 2) { leader(kernel, group); CHECK(kernel.posix().duplicate_to(other.value()[1], fds[1])); }
    expect(native::private_signal_admit_blocked(kernel, *p, *thread));
    expect(native::private_pipe_write_signal_ready(kernel, *p, *thread));
    expect(native::private_pipe_write_signal_complete(kernel, *p, *thread));
    expect(!thread->syscall_pending && !native::pipe_write_object(group.pid, group.writer).pool);
    expect(!native::syscall_origins.live(origin) && !native::pipe_write_objects.resolve(object));
    expect(thread->native_frame.pc == (restart ? site : site+2));
    expect(thread->native_frame.registers[14] == (restart ? 1 : static_cast<u64>(-4)));
    for (usize index : {9, 10, 11}) expect(thread->native_frame.registers[index] == frame.registers[index]);
    const auto *description = kernel.scheduler().open_description(wait.description);
    expect(mode == 2 ? description == nullptr : description && description->references == 1);
    expect(!native::private_pipe_write_signal_complete(kernel, *p, *thread));
    expect(native::private_signal_deliver(kernel, *p, *thread));
    const auto outer = thread->native_frame;
    expect(outer.pc == handler && !native::private_signal_queue(*thread)->pending_mask().contains(10));
    // Execute the real private sigreturn decoder, then re-enter the captured
    // write through the real runtime. The mock supplies only trap/copy hardware.
    thread->native_frame.sp += 8; thread->native_frame.registers[14] = 15;
    bool complete = true; i64 result = -999;
    expect(native::private_signal_call(kernel, *p, *thread, complete, result));
    expect(!complete && result == 0 && thread->native_frame.pc == (restart ? site : site+2));
    if (restart && mode != 2)
    {
        arm(kernel, group, fds[1]);
        const auto next = *native::pipe_write_objects.resolve(native::pipe_write_object(group.pid, group.writer)).value();
        expect(next.request > wait.request && next.ticket.generation > wait.ticket.generation);
    }
    retire(kernel, group);
}

void completion(Kernel &kernel, unsigned mode)
{
    const auto group = create(kernel); const auto fds = full_pipe(kernel);
    arm(kernel, group, fds[1]); caught(kernel, group, true); enqueue(kernel, group);
    const auto object = native::pipe_write_object(group.pid, group.writer);
    const auto original = *native::pipe_write_objects.resolve(object).value();
    leader(kernel, group);
    if (mode == 3) CHECK(kernel.posix().close(fds[0]));
    else expect(drain(kernel, fds[0], 3) == 3);
    if (mode == 1) mock::forbidden = input;
    if (mode == 2)
    {
        auto replacement = kernel.posix().pipe(0); CHECK(replacement);
        CHECK(kernel.posix().duplicate_to(replacement.value()[1], fds[1]));
    }
    // The numeric pointer is retained; payload bytes are checked/copied again
    // before progress. Peer modifications cannot redirect the retained fd.
    mock::memory[input-hw::user_begin] = std::byte{0x5a};
    auto *p = kernel.scheduler().find(group.pid); auto *thread = kernel.scheduler().find_thread(group.pid, group.writer);
    expect(!native::private_pipe_write_signal_ready(kernel, *p, *thread));
    bool complete = true; const auto result = resume(kernel, group, object, complete);
    expect(complete && result == (mode == 1 ? -14 : mode == 3 ? -32 : 3));
    expect(!native::pipe_write_object(group.pid, group.writer).pool);
    expect(native::private_signal_queue(*thread)->pending_mask().contains(10));
    mock::forbidden = 0;
    const auto ready = kernel.scheduler().pipe_readiness(original.pipe); CHECK(ready);
    expect(ready.value().buffered == (mode == 1 ? sched::pipe_capacity-3 : sched::pipe_capacity));
    if (mode == 0 || mode == 2)
    {
        leader(kernel, group); expect(drain(kernel, fds[0], sched::pipe_capacity-3) == sched::pipe_capacity-3);
        std::array<std::byte, 3> bytes{}; auto read = kernel.posix().read(fds[0], bytes); CHECK(read);
        expect(read.value() == 3 && bytes[0] == std::byte{0x5a} && bytes[1] == std::byte{0x42} && bytes[2] == std::byte{0x42});
    }
    retire(kernel, group);
}

void rearm(Kernel &kernel)
{
    const auto group = create(kernel); const auto fds = full_pipe(kernel); arm(kernel, group, fds[1]);
    const auto object = native::pipe_write_object(group.pid, group.writer);
    const auto before = *native::pipe_write_objects.resolve(object).value();
    leader(kernel, group); expect(drain(kernel, fds[0], 3) == 3);
    const std::array<std::byte, 3> peer{}; CHECK(kernel.posix().write(fds[1], peer));
    bool complete = true; expect(resume(kernel, group, object, complete) == -11 && !complete);
    const auto after = *native::pipe_write_objects.resolve(object).value();
    if (after.request != before.request || after.ticket.generation <= before.ticket.generation)
        std::fprintf(stderr, "PIPE_WRITE_REARM before=%llu/%llu after=%llu/%llu\n",
            static_cast<unsigned long long>(before.request), static_cast<unsigned long long>(before.ticket.generation),
            static_cast<unsigned long long>(after.request), static_cast<unsigned long long>(after.ticket.generation));
    expect(after.request == before.request && after.ticket.generation > before.ticket.generation);
    expect(after.input == before.input && after.description == before.description);
    caught(kernel, group, true); enqueue(kernel, group);
    auto *p = kernel.scheduler().find(group.pid); auto *thread = kernel.scheduler().find_thread(group.pid, group.writer);
    expect(native::private_pipe_write_signal_ready(kernel, *p, *thread));
    expect(native::private_pipe_write_signal_complete(kernel, *p, *thread));
    retire(kernel, group);
}

void eligibility(Kernel &kernel, unsigned mode)
{
    const auto group = create(kernel); const auto fds = full_pipe(kernel); arm(kernel, group, fds[1]);
    caught(kernel, group, true); enqueue(kernel, group);
    auto *p = kernel.scheduler().find(group.pid); auto *thread = kernel.scheduler().find_thread(group.pid, group.writer);
    const auto object = native::pipe_write_object(group.pid, group.writer);
    auto &wait = *native::pipe_write_objects.resolve(object).value(); const auto ticket = wait.ticket;
    if (mode == 0) thread->native_signal_mask.set(10);
    if (mode == 1) native::private_signal_table(kernel, group.pid)->find(10)->handler = 1;
    if (mode == 2) native::syscall_origins.revoke(*thread);
    if (mode == 3) ++thread->native_frame.registers[7];
    if (mode == 4) ++wait.request;
    if (mode == 5) ++wait.input;
    if (mode == 6)
    {
        CHECK(kernel.scheduler().complete_wait(ticket, sched::WaitCompletion::ready));
        CHECK(kernel.scheduler().activate_thread(group.pid, group.writer));
        CHECK(kernel.scheduler().register_current_wait(sched::ThreadWaitReason::deadline, 900));
    }
    expect(!native::private_pipe_write_signal_ready(kernel, *p, *thread));
    expect(!native::private_pipe_write_signal_complete(kernel, *p, *thread));
    expect(native::private_signal_queue(*thread)->pending_mask().contains(10));
    const auto replacement = thread->wait_registration.ticket();
    native::cancel_pipe_write(kernel, object);
    if (mode == 6) expect(thread->wait_registration.pending(replacement));
    retire(kernel, group);
}

void immediate(Kernel &kernel, unsigned mode)
{
    const auto group = create(kernel); const auto fds = full_pipe(kernel, mode >= 2 ? sched::pipe_capacity-1 : sched::pipe_capacity);
    CHECK(kernel.posix().activate(group.pid)); CHECK(kernel.scheduler().activate_thread(group.pid, group.writer));
    auto *p = kernel.scheduler().find(group.pid); const auto *fd = p->resources().files.entry(fds[1]); CHECK(fd);
    if (mode == 1 || mode == 2) CHECK(kernel.posix().fcntl(fds[1], posix::f_SETFL, posix::o_NONBLOCK));
    bool complete = true;
    const usize count = mode == 0 ? 0 : mode == 2 ? sched::pipe_capacity+1 : mode == 3 ? sched::pipe_capacity : 3;
    const auto result = native::begin_pipe_write(kernel, *p, group.writer, fds[1], fd->description, input, count, complete);
    expect(result == (mode == 0 ? 0 : mode == 2 ? 1 : -11));
    expect(complete == (mode != 3));
    expect(bool(native::pipe_write_object(group.pid, group.writer).pool) == (mode == 3));
    retire(kernel, group);
}
} // namespace

int main()
{
    static_assert(lu::configured_native_profile() == lu::Profile::x86_64);
    static Kernel kernel; CHECK(kernel.boot({})); CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    for (bool restart : {false, true}) for (unsigned mode = 0; mode < 3; ++mode) publication(kernel, restart, mode);
    for (unsigned mode = 0; mode < 4; ++mode) completion(kernel, mode);
    rearm(kernel);
    for (unsigned mode = 0; mode < 7; ++mode) eligibility(kernel, mode);
    for (unsigned mode = 0; mode < 4; ++mode) immediate(kernel, mode);
    expect(native::entry_stacks.accounting().leased_slots == 0);
    expect(native::pipe_write_objects.accounting().live_objects == 0);
    std::printf("PIPE_WRITE_SIGNAL_MODEL_PASS hosted=1 native=0 checks=%zu\n", checks);
}
