// Actual private native runtime with hosted trap/usercopy mappings.
#define OK_ENABLE_TEST_POINTS 1
#define OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE 1
#define OK_SYSCALL_ORIGIN_MODEL 1
#define OK_PIPE_WRITE_SIGNAL_MODEL 1
#include "native_clock_model.hpp"
#include <source_location>
namespace trace_capture
{
std::array<char, 16384> text{};
usize size{};
bool enabled{}, overflow{};
void begin() { size = 0; overflow = false; enabled = true; }
void end() { enabled = false; }
}
extern "C" void ok_platform_console_write_char(char value)
{
    if (!trace_capture::enabled) return;
    if (trace_capture::size == trace_capture::text.size()) { trace_capture::overflow = true; return; }
    trace_capture::text[trace_capture::size++] = value;
}
namespace ok::sched
{
struct WaitRegistrationTestAccess
{
    static void exhaust_idle(WaitRegistration &wait)
    { CHECK(!wait.active_); wait.ticket_.generation = ~u64{0}; }
};
}

namespace
{
usize checks{};
void expect(bool value, std::source_location at = std::source_location::current())
{
    ++checks;
    if (!value) { std::fprintf(stderr, "SIGNAL_WAIT_FAIL line=%u checks=%zu\n", at.line(), checks); std::abort(); }
}
constexpr uptr mask_address = hw::user_begin + 320, timeout_address = hw::user_begin + 352, info_address = hw::user_begin + 512;
constexpr uptr site = hw::user_begin + 1024, handler = hw::user_begin + 2048, scratch = hw::user_begin + 128;
struct Group { sched::ProcessId pid{}; sched::ThreadId peer{}; };
sched::ThreadControlBlock &thread(Kernel &kernel, u64 pid, u64 tid)
{ auto *t = kernel.scheduler().find_thread(pid, tid); CHECK(t); return *t; }
void reset_frame(sched::ThreadControlBlock &t)
{
    auto &f = t.native_frame; f = {}; f.flags = 0x202; f.sp = hw::user_begin + 8192; f.pc = site;
    const auto fp = native::X86SignalFrame::reset_fp();
    for (usize i = 0; i < fp.size(); ++i) f.fp_state[i] = static_cast<u8>(fp[i]);
}
Group create(Kernel &kernel)
{
    auto made = kernel.scheduler().create_user_process("signal-waits",
        kernel.arch().make_user_context({site, hw::user_begin + 8192})); CHECK(made);
    auto *p = kernel.scheduler().find(made.value());
    p->resources().personality = sched::UserPersonality::linux;
    p->resources().native_space = {0x100000 + p->pid() * 4096, static_cast<u32>(p->pid() % 16), 1};
    CHECK(native::allocate(p->resources().native_pages, hw::user_begin, hw::page_read | hw::page_execute));
    const auto peer = kernel.scheduler().create_thread(p->pid(), p->threads()[0].context); CHECK(peer);
    for (auto &t : p->threads()) { CHECK(native::prepare_native_thread(t)); t.native_active = true; reset_frame(t); }
    CHECK(kernel.scheduler().set_runnable(p->pid()));
    native::runtime_kernel = &kernel; native::started = true;
    return {p->pid(), peer.value()};
}
void retire(Kernel &kernel, Group g)
{
    native::release_process(kernel, g.pid);
    if (auto *p = kernel.scheduler().find(g.pid); p && p->state() != sched::ProcessState::zombie)
        CHECK(kernel.scheduler().exit_process(g.pid, 0));
    CHECK(kernel.scheduler().reap_process(g.pid));
    expect(native::runtime_status().ok());
}
i64 kill(Kernel &kernel, Group sender, u64 pid, u64 signal)
{
    auto *p = kernel.scheduler().find(sender.pid); CHECK(p);
    return native::private_process_kill(kernel, *p, pid, signal);
}
void action(Kernel &kernel, Group g, u32 signal, u64 disposition = handler)
{
    auto *a = native::private_signal_table(kernel, g.pid)->find(signal); CHECK(a);
    *a = {}; a->handler = disposition;
    if (disposition > 1) { a->flags = 0x04000004; a->restorer = handler + 4; }
}
native::PrivateProcessQueue &shared(Kernel &kernel, Group g)
{ auto *q = native::private_signal_shared(kernel, g.pid); CHECK(q); return *q; }
native::PrivateSignalView view(Kernel &kernel, Group g, u64 tid)
{ return native::private_signal_pending(kernel, *kernel.scheduler().find(g.pid), thread(kernel, g.pid, tid)); }
void direct(Kernel &kernel, Group g, u64 tid, u32 signal)
{
    auto *q = native::private_signal_queue(thread(kernel, g.pid, tid)); CHECK(q);
    expect(q->enqueue({signal, -6, static_cast<u32>(g.pid), 0}) == native::PrivateSignalQueue::Enqueue::queued);
}
u64 get(uptr address, usize bytes = 8)
{
    u64 value{};
    for (usize i = 0; i < bytes; ++i) value |= static_cast<u64>(mock::memory[address - hw::user_begin + i]) << (8 * i);
    return value;
}
struct Called { bool complete{}; i64 result{}; };
Called intercept(Kernel &kernel, Group g, u64 tid, lu::Operation op, std::array<u64, 6> a)
{
    auto &t = thread(kernel, g.pid, tid); auto &f = t.native_frame;
    f.registers[14] = *lu::Codec<lu::Profile::x86_64>::syscall_number(op);
    f.registers[9] = a[0]; f.registers[10] = a[1]; f.registers[11] = a[2];
    f.registers[5] = a[3]; f.registers[7] = a[4]; f.registers[6] = a[5];
    bool complete = true; i64 result = -999;
    expect(native::private_signal_call(kernel, *kernel.scheduler().find(g.pid), t, complete, result));
    return {complete, result};
}
i64 call(Kernel &kernel, Group g, u64 tid, lu::Operation op, std::array<u64, 6> a)
{
    const auto result = intercept(kernel, g, tid, op, a);
    expect(result.complete); return result.result;
}
void put(uptr address, u64 value, usize bytes = 8)
{ for (usize i = 0; i < bytes; ++i) mock::memory[address - hw::user_begin + i] = std::byte(value >> (i * 8)); }
sched::SignalMask mask(std::initializer_list<u32> signals)
{ sched::SignalMask result; for (const auto signal : signals) result.set(signal); return result; }
void input(sched::SignalMask wanted, i64 seconds = 0, i64 nanoseconds = 0)
{ put(mask_address, wanted.words[0]); put(timeout_address, seconds); put(timeout_address + 8, nanoseconds); }
void activate(Kernel &kernel, Group g, u64 tid)
{ CHECK(kernel.posix().activate(g.pid)); CHECK(kernel.scheduler().activate_thread(g.pid, tid)); }
Called begin(Kernel &kernel, Group g, u64 tid, bool suspend = false, bool finite = false,
             uptr info = info_address)
{
    activate(kernel, g, tid); auto &t = thread(kernel, g.pid, tid); t.syscall_pending = true;
    const auto result = intercept(kernel, g, tid,
        suspend ? lu::Operation::rt_sigsuspend : lu::Operation::rt_sigtimedwait,
        suspend ? std::array<u64, 6>{mask_address, 8} : std::array<u64, 6>{mask_address, info, finite ? timeout_address : 0, 8});
    if (result.complete) { t.syscall_pending = false; CHECK(native::set_result(*kernel.scheduler().find(g.pid), t.native_frame, result.result)); }
    return result;
}
bool ready(Kernel &kernel, Group g, u64 tid)
{ return native::private_signal_wait_ready(kernel, *kernel.scheduler().find(g.pid), thread(kernel, g.pid, tid), mock::now); }
bool service(Kernel &kernel, Group g, u64 tid)
{ return native::private_signal_wait_complete(kernel, *kernel.scheduler().find(g.pid), thread(kernel, g.pid, tid), mock::now); }
void expect_info(uptr address, u32 number, i32 code, u32 pid, u32 uid)
{
    std::array<std::byte, 128> expected{};
    const auto field = [&](usize offset, u32 value) {
        for (usize i = 0; i < 4; ++i) expected[offset + i] = std::byte(value >> (8 * i));
    };
    field(0, number); field(8, code); field(16, pid); field(20, uid);
    expect(std::memcmp(expected.data(), mock::memory.data() + address - hw::user_begin, expected.size()) == 0);
}
void sigreturn(Kernel &kernel, Group g, u64 tid, hw::UserFrame frame)
{
    auto &t = thread(kernel, g.pid, tid); t.native_frame = frame; t.native_frame.sp += 8;
    const auto result = intercept(kernel, g, tid, lu::Operation::rt_sigreturn, {});
    expect(!result.complete && result.result == 0);
}
void only_waiter(Kernel &kernel, Group g)
{ thread(kernel, g.pid, g.pid).native_active = false; native::last_pid = native::last_tid = 0; }
void finish_service(Kernel &kernel, Group g, u64 tid, i64 result, sched::SignalMask original)
{
    auto &t = thread(kernel, g.pid, tid);
    expect(service(kernel, g, tid));
    expect(!t.syscall_pending && !native::signal_waits.find(t));
    expect(!t.wait_registration.active() && t.native_frame.registers[14] == static_cast<u64>(result));
    expect(t.native_signal_mask == original && !native::signal_waits.real_blocked(t));
}

void immediate_and_precedence(Kernel &kernel)
{
    const auto g = create(kernel); auto &t = thread(kernel, g.pid, g.peer);
    action(kernel, g, 10, 1); action(kernel, g, 12, 0);
    t.native_signal_mask = mask({10, 12}); direct(kernel, g, g.peer, 12);
    expect(shared(kernel, g).enqueue({10, 0, 73, 19}) == native::PrivateProcessQueue::Enqueue::queued);
    input(mask({10, 12, 9, 19}));
    const auto clocks = mock::clock_reads;
    expect(call(kernel, g, g.peer, lu::Operation::rt_sigtimedwait, {mask_address, info_address, timeout_address, 8}) == 12);
    expect_info(info_address, 12, -6, g.pid, 0);
    expect(call(kernel, g, g.peer, lu::Operation::rt_sigtimedwait, {mask_address, info_address, timeout_address, 8}) == 10);
    expect_info(info_address, 10, 0, 73, 19);
    expect(mock::clock_reads == clocks && t.native_signal_mask == mask({10, 12}));
    expect(!native::signal_waits.find(t) && !t.wait_registration.active());
    const auto outputs = mock::outputs;
    expect(call(kernel, g, g.peer, lu::Operation::rt_sigtimedwait, {mask_address, 0, timeout_address, 8}) == -11);
    expect(mock::outputs == outputs && mock::clock_reads == clocks);
    direct(kernel, g, g.pid, 10);
    expect(call(kernel, g, g.peer, lu::Operation::rt_sigtimedwait, {mask_address, info_address, timeout_address, 8}) == -11);
    expect(native::private_signal_queue(thread(kernel, g.pid, g.pid))->pending_mask().contains(10));
    direct(kernel, g, g.peer, 9); direct(kernel, g, g.peer, 19);
    input(mask({9, 19}));
    expect(call(kernel, g, g.peer, lu::Operation::rt_sigtimedwait, {mask_address, info_address, timeout_address, 8}) == -11);
    expect(native::private_signal_queue(t)->pending_mask().contains(9) && native::private_signal_queue(t)->pending_mask().contains(19));
    retire(kernel, g);
}

void admission_faults(Kernel &kernel)
{
    const auto g = create(kernel); auto &t = thread(kernel, g.pid, g.peer);
    t.native_signal_mask = mask({10, 14}); direct(kernel, g, g.peer, 10);
    input(mask({10}), -1, 0);
    auto copies = mock::copies;
    expect(call(kernel, g, g.peer, lu::Operation::rt_sigtimedwait, {0, info_address, timeout_address, 7}) == -22);
    expect(mock::copies == copies);
    expect(call(kernel, g, g.peer, lu::Operation::rt_sigtimedwait, {0, info_address, timeout_address, 8}) == -14);
    expect(call(kernel, g, g.peer, lu::Operation::rt_sigtimedwait, {mask_address, info_address, timeout_address, 8}) == -22);
    input(mask({10}), 0, 1000000000);
    expect(call(kernel, g, g.peer, lu::Operation::rt_sigtimedwait, {mask_address, info_address, timeout_address, 8}) == -22);
    input(mask({10}), 0, -1);
    expect(call(kernel, g, g.peer, lu::Operation::rt_sigtimedwait, {mask_address, info_address, timeout_address, 8}) == -22);
    mock::forbidden = timeout_address;
    expect(call(kernel, g, g.peer, lu::Operation::rt_sigtimedwait, {mask_address, info_address, timeout_address, 8}) == -14);
    mock::forbidden = 0;
    expect(native::private_signal_queue(t)->pending_mask().contains(10));
    expect(t.native_signal_mask == mask({10, 14}) && !native::signal_waits.find(t));
    input(mask({10}));
    expect(call(kernel, g, g.peer, lu::Operation::rt_sigtimedwait, {mask_address, 0, timeout_address, 8}) == 10);
    copies = mock::copies;
    expect(call(kernel, g, g.peer, lu::Operation::rt_sigsuspend, {0, 16}) == -22 && mock::copies == copies);
    expect(call(kernel, g, g.peer, lu::Operation::rt_sigsuspend, {0, 8}) == -14);
    expect(t.native_signal_mask == mask({10, 14})); retire(kernel, g);
}

void destructive_copyout(Kernel &kernel)
{
    for (unsigned fault = 0; fault < 3; ++fault)
    {
        const auto g = create(kernel); auto &t = thread(kernel, g.pid, g.peer);
        direct(kernel, g, g.peer, 10); input(mask({10}));
        std::fill(mock::memory.begin() + info_address - hw::user_begin,
                  mock::memory.begin() + info_address - hw::user_begin + 128, std::byte{0x5a});
        if (fault == 0) mock::write_forbidden = info_address;
        if (fault == 1) mock::write_forbidden = info_address + 127;
        if (fault == 2) mock::partial_output = 32;
        expect(call(kernel, g, g.peer, lu::Operation::rt_sigtimedwait, {mask_address, info_address, timeout_address, 8}) == -14);
        mock::write_forbidden = 0; mock::partial_output = 0;
        expect(!native::private_signal_queue(t)->pending_mask().contains(10));
        const auto outputs = mock::outputs;
        expect(call(kernel, g, g.peer, lu::Operation::rt_sigtimedwait, {mask_address, info_address, timeout_address, 8}) == -11);
        expect(mock::outputs == outputs);
        if (fault == 2) expect(get(info_address, 4) == 10 && get(info_address + 32, 1) == 0x5a);
        retire(kernel, g);
    }
    const auto g = create(kernel); direct(kernel, g, g.peer, 12); input(mask({12}));
    // Set and output alias: input decoding must precede destructive output.
    expect(call(kernel, g, g.peer, lu::Operation::rt_sigtimedwait, {mask_address, mask_address, timeout_address, 8}) == 12);
    expect_info(mask_address, 12, -6, g.pid, 0);
    input(mask({12}), 7, 31); direct(kernel, g, g.peer, 12);
    expect(call(kernel, g, g.peer, lu::Operation::rt_sigtimedwait, {mask_address, 0, timeout_address, 8}) == 12);
    expect(get(timeout_address) == 7 && get(timeout_address + 8) == 31);
    retire(kernel, g);
}

void deferred_match(Kernel &kernel, bool process, bool finite, bool fault)
{
    const auto g = create(kernel); auto &t = thread(kernel, g.pid, g.peer);
    const auto original = mask({10, 14}); t.native_signal_mask = original;
    input(mask({10}), 5, 7); const auto before = mock::now;
    expect(!begin(kernel, g, g.peer, false, finite).complete);
    const auto *record = native::signal_waits.find(t); expect(record);
    const auto ticket = record->ticket; const auto deadline = record->timeout.deadline();
    expect(record->request.wanted == mask({10}) && record->original == original);
    expect(t.wait_reason == sched::ThreadWaitReason::signal_wait && t.wait_registration.pending(ticket));
    expect(t.native_signal_mask == mask({14}) && native::signal_waits.real_blocked(t) &&
           *native::signal_waits.real_blocked(t) == original);
    expect(record->timeout.armed() == finite && (!finite || deadline == before + 5000000007));
    expect(!t.native_deadline.armed() && !ready(kernel, g, g.peer));
    input(mask({12}), 0, 0); mock::forbidden = mask_address;
    if (process) expect(shared(kernel, g).enqueue({10, 0, 81, 23}) == native::PrivateProcessQueue::Enqueue::queued);
    else direct(kernel, g, g.peer, 10);
    expect(ready(kernel, g, g.peer)); const auto copies = mock::copies;
    if (fault) mock::write_forbidden = info_address;
    finish_service(kernel, g, g.peer, fault ? -14 : 10, original);
    mock::forbidden = 0; mock::write_forbidden = 0;
    expect(mock::copies == copies && !kernel.scheduler().complete_wait(ticket, sched::WaitCompletion::ready));
    if (!fault) expect_info(info_address, 10, process ? 0 : -6, process ? 81 : g.pid, process ? 23 : 0);
    auto v = view(kernel, g, g.peer); native::PrivateSignalView::Delivery selected;
    expect(!v.peek({}, selected));
    expect(!service(kernel, g, g.peer)); retire(kernel, g);
}

void finite_ordering(Kernel &kernel, unsigned outcome)
{
    const auto g = create(kernel); auto &t = thread(kernel, g.pid, g.peer);
    const auto original = mask({10, 14}); t.native_signal_mask = original;
    action(kernel, g, 12); input(mask({10}), 0, 5);
    expect(!begin(kernel, g, g.peer, false, true).complete);
    const auto deadline = native::signal_waits.find(t)->timeout.deadline();
    mock::now = deadline - 1; expect(!ready(kernel, g, g.peer));
    if (outcome == 1) direct(kernel, g, g.peer, 10);
    if (outcome >= 2) direct(kernel, g, g.peer, 12);
    if (outcome != 3) mock::now = deadline;
    expect(ready(kernel, g, g.peer)); const auto outputs = mock::outputs;
    finish_service(kernel, g, g.peer, outcome == 1 ? 10 : outcome == 3 ? -4 : -11, original);
    if (outcome != 1) expect(mock::outputs == outputs);
    if (outcome >= 2)
    {
        expect(native::private_signal_queue(t)->pending_mask().contains(12));
        expect(native::private_signal_deliver(kernel, *kernel.scheduler().find(g.pid), t));
        expect(t.native_frame.pc == handler && get(t.native_frame.sp + native::X86SignalFrame::mask_offset) == original.words[0]);
        expect(get(t.native_frame.sp + native::X86SignalFrame::context_offset + 104) == static_cast<u64>(outcome == 3 ? -4 : -11));
    }
    expect(get(timeout_address) == 0 && get(timeout_address + 8) == 5); retire(kernel, g);
}

void ignored_generation(Kernel &kernel, bool process, bool alias, bool originally_blocked, bool explicit_ignore)
{
    const auto g = create(kernel), sender = create(kernel);
    const u64 target = alias ? g.peer : g.pid;
    auto &t = thread(kernel, g.pid, target); const u32 signal = explicit_ignore ? 10 : 28;
    action(kernel, g, signal, explicit_ignore ? 1 : 0);
    if (originally_blocked) t.native_signal_mask.set(signal);
    // The other member's live mask must not substitute for the named task.
    thread(kernel, g.pid, alias ? g.pid : g.peer).native_signal_mask.set(signal, !originally_blocked);
    input(mask({signal})); expect(!begin(kernel, g, target).complete);
    expect(!t.native_signal_mask.contains(signal));
    if (process) expect(kill(kernel, sender, target, signal) == 0);
    else if (alias) expect(call(kernel, sender, sender.peer, lu::Operation::tgkill, {g.pid, target, signal}) == 0);
    else expect(call(kernel, sender, sender.peer, lu::Operation::tkill, {target, signal}) == 0);
    expect(ready(kernel, g, target) == originally_blocked);
    expect((process ? shared(kernel, g).pending_mask() : native::private_signal_queue(t)->pending_mask()).contains(signal) == originally_blocked);
    if (originally_blocked) finish_service(kernel, g, target, signal, mask({signal}));
    retire(kernel, sender); retire(kernel, g);
}

void competing_waiters(Kernel &kernel)
{
    const auto g = create(kernel); input(mask({10}));
    for (const auto tid : {g.pid, g.peer})
    { thread(kernel, g.pid, tid).native_signal_mask = mask({10}); expect(!begin(kernel, g, tid).complete); }
    expect(shared(kernel, g).enqueue({10, 0, 99, 8}) == native::PrivateProcessQueue::Enqueue::queued);
    expect(ready(kernel, g, g.pid) && ready(kernel, g, g.peer));
    finish_service(kernel, g, g.peer, 10, mask({10}));
    expect(!ready(kernel, g, g.pid) && !service(kernel, g, g.pid));
    expect(thread(kernel, g.pid, g.pid).syscall_pending && native::signal_waits.find(thread(kernel, g.pid, g.pid)));
    expect(shared(kernel, g).enqueue({10, 0, 100, 9}) == native::PrivateProcessQueue::Enqueue::queued);
    finish_service(kernel, g, g.pid, 10, mask({10})); expect_info(info_address, 10, 0, 100, 9);
    retire(kernel, g);
}
void suspend_delivery(Kernel &kernel, bool queued_before, bool nested)
{
    const auto g = create(kernel); auto &t = thread(kernel, g.pid, g.peer);
    const auto original = mask({10, 14}), temporary = mask({12});
    t.native_signal_mask = original; action(kernel, g, 10); action(kernel, g, 14);
    auto *a = native::private_signal_table(kernel, g.pid)->find(10);
    a->flags |= 0x10000000; a->mask.set(15); // SA_RESTART must not restart suspend.
    input(mask({12, 9, 19}));
    if (queued_before) direct(kernel, g, g.peer, 10);
    expect(!begin(kernel, g, g.peer, true).complete);
    expect(t.syscall_pending && t.wait_reason == sched::ThreadWaitReason::signal_suspend);
    expect(t.native_signal_mask == temporary && !native::signal_waits.real_blocked(t));
    expect(native::signal_waits.find(t)->original == original && !native::signal_waits.find(t)->timeout.armed());
    if (!queued_before) { expect(!ready(kernel, g, g.peer)); direct(kernel, g, g.peer, 10); }
    expect(ready(kernel, g, g.peer)); input(mask({2})); mock::forbidden = mask_address;
    const auto copies = mock::copies;
    expect(service(kernel, g, g.peer));
    expect(!t.syscall_pending && native::signal_waits.saved(t) && *native::signal_waits.saved(t) == original);
    expect(t.native_frame.registers[14] == static_cast<u64>(-4) && t.native_signal_mask == temporary);
    expect(native::private_signal_deliver(kernel, *kernel.scheduler().find(g.pid), t)); mock::forbidden = 0;
    expect(mock::copies == copies && !native::signal_waits.find(t));
    const auto outer = t.native_frame;
    expect(outer.pc == handler && get(outer.sp + native::X86SignalFrame::mask_offset) == original.words[0]);
    expect(get(outer.sp + native::X86SignalFrame::context_offset + 104) == static_cast<u64>(-4));
    const auto in_handler = mask({10, 12, 15}); expect(t.native_signal_mask == in_handler);
    if (nested)
    {
        direct(kernel, g, g.peer, 14);
        expect(native::private_signal_deliver(kernel, *kernel.scheduler().find(g.pid), t));
        const auto inner = t.native_frame;
        expect(get(inner.sp + native::X86SignalFrame::mask_offset) == in_handler.words[0]);
        sigreturn(kernel, g, g.peer, inner); expect(t.native_signal_mask == in_handler && t.native_frame.sp == outer.sp);
    }
    // After commit, user changes to the saved mask govern sigreturn.
    put(outer.sp + native::X86SignalFrame::mask_offset, mask({2}).words[0]);
    sigreturn(kernel, g, g.peer, outer);
    expect(t.native_frame.pc == site && t.native_frame.registers[14] == static_cast<u64>(-4));
    expect(t.native_signal_mask == mask({2}) && !native::signal_waits.find(t)); retire(kernel, g);
}

void ignored_suspend(Kernel &kernel)
{
    const auto g = create(kernel), sender = create(kernel); auto &t = thread(kernel, g.pid, g.peer);
    action(kernel, g, 10, 1); action(kernel, g, 12); t.native_signal_mask = mask({10}); input({});
    expect(!begin(kernel, g, g.peer, true).complete);
    expect(kill(kernel, sender, g.peer, 10) == 0);
    expect(call(kernel, sender, sender.peer, lu::Operation::tgkill, {g.pid, g.peer, 10}) == 0);
    expect(!shared(kernel, g).pending_mask().contains(10) && !native::private_signal_queue(t)->pending_mask().contains(10));
    direct(kernel, g, g.peer, 10);
    expect(ready(kernel, g, g.peer));
    expect(!service(kernel, g, g.peer));
    expect(!native::private_signal_queue(t)->pending_mask().contains(10));
    expect(t.syscall_pending && t.native_signal_mask == sched::SignalMask{} && native::signal_waits.find(t));
    retire(kernel, sender); only_waiter(kernel, g);
    mock::entry_trap = {hw::TrapKind::timer}; const auto entries = mock::entries;
    expect(native::step(kernel)); expect(mock::entries == entries && t.syscall_pending);
    direct(kernel, g, g.peer, 12); expect(native::step(kernel));
    expect(mock::entries == entries + 1 && mock::last_frame.pc == handler && !t.syscall_pending);
    expect(get(t.native_frame.sp + native::X86SignalFrame::mask_offset) == mask({10}).words[0]);
    expect(!native::signal_waits.find(t)); retire(kernel, g);
}

void shared_ignored_suspend(Kernel &kernel)
{
    const auto g = create(kernel); auto &t = thread(kernel, g.pid, g.peer);
    t.native_signal_mask = mask({28}); thread(kernel, g.pid, g.pid).native_signal_mask = mask({28});
    action(kernel, g, 28, 0); action(kernel, g, 10);
    expect(kill(kernel, g, g.peer, 28) == 0 && shared(kernel, g).pending_mask().contains(28));
    expect(call(kernel, g, g.pid, lu::Operation::rt_sigpending, {scratch, 8}) == 0 && get(scratch) == mask({28}).words[0]);
    input({}); only_waiter(kernel, g); expect(!begin(kernel, g, g.peer, true).complete);
    expect(ready(kernel, g, g.peer)); const auto entries = mock::entries, outputs = mock::outputs;
    const auto original_result = t.native_frame.registers[14]; mock::entry_trap = {hw::TrapKind::timer};
    expect(native::step(kernel));
    expect(mock::entries == entries && mock::outputs == outputs && !shared(kernel, g).pending_mask().contains(28));
    thread(kernel, g.pid, g.pid).native_active = true; // Reenable the query actor after the isolated waiter step.
    expect(call(kernel, g, g.pid, lu::Operation::rt_sigpending, {scratch, 8}) == 0 && get(scratch) == 0);
    thread(kernel, g.pid, g.pid).native_active = false;
    expect(t.syscall_pending && t.wait_registration.active() && native::signal_waits.find(t));
    expect(t.native_frame.registers[14] == original_result && t.native_signal_mask == sched::SignalMask{});
    expect(native::signal_waits.find(t)->original == mask({28}));
    direct(kernel, g, g.peer, 10); expect(native::step(kernel));
    expect(mock::entries == entries + 1 && t.native_frame.pc == handler && !t.syscall_pending);
    expect(get(t.native_frame.sp + native::X86SignalFrame::mask_offset) == mask({28}).words[0]);
    expect(!native::signal_waits.find(t)); retire(kernel, g);
}

void unrelated_ignored_timed(Kernel &kernel, bool explicit_ignore)
{
    const auto g = create(kernel); auto &t = thread(kernel, g.pid, g.peer);
    t.native_signal_mask = mask({10}); thread(kernel, g.pid, g.pid).native_signal_mask = mask({28});
    action(kernel, g, 28, explicit_ignore ? 1 : 0); input(mask({10}), 2, 0); only_waiter(kernel, g);
    expect(!begin(kernel, g, g.peer, false, true).complete);
    // The named leader retains WINCH at generation; the waiting sibling has
    // it unblocked. Linux interrupts timedwait, then drains it without a handler.
    expect(kill(kernel, g, g.pid, 28) == 0 && shared(kernel, g).pending_mask().contains(28));
    expect(ready(kernel, g, g.peer)); const auto entries = mock::entries, outputs = mock::outputs;
    mock::entry_trap = {hw::TrapKind::timer}; expect(native::step(kernel));
    expect(mock::entries == entries + 1 && mock::outputs == outputs);
    expect(t.native_frame.pc == site && t.native_frame.registers[14] == static_cast<u64>(-4));
    expect(!t.syscall_pending && !native::signal_waits.find(t) && t.native_signal_mask == mask({10}));
    expect(!shared(kernel, g).pending_mask().contains(28)); retire(kernel, g);
}

void cancellation_and_wake(Kernel &kernel, bool suspend, bool cancel, bool finite = false)
{
    const auto g = create(kernel); auto &t = thread(kernel, g.pid, g.peer);
    const auto original = mask({10, 14}); t.native_signal_mask = original; action(kernel, g, 10);
    input(suspend ? mask({12}) : mask({10}), 0, 51); expect(!begin(kernel, g, g.peer, suspend, finite).complete);
    const auto before_wake = t;
    const auto record = *native::signal_waits.find(t); auto stale = record.ticket; ++stale.generation;
    expect(!kernel.scheduler().complete_wait(stale, sched::WaitCompletion::cancelled));
    expect(t.wait_registration.pending(record.ticket) && native::signal_waits.find(t)->epoch == record.epoch);
    expect(kernel.scheduler().complete_wait(record.ticket, cancel ? sched::WaitCompletion::cancelled : sched::WaitCompletion::ready));
    if (cancel)
    {
        direct(kernel, g, g.peer, 10); expect(ready(kernel, g, g.peer));
        finish_service(kernel, g, g.peer, -4, original);
        expect(native::private_signal_queue(t)->pending_mask().contains(10));
        expect(!service(kernel, g, g.peer));
    }
    else
    {
        if (finite) mock::now += 13;
        static_cast<void>(service(kernel, g, g.peer));
        expect(!native::private_signal_wait_ready(kernel, *kernel.scheduler().find(g.pid), before_wake, mock::now));
        expect(native::signal_waits.find(t)->timeout.deadline() == record.timeout.deadline());
        expect(t.syscall_pending && native::signal_waits.find(t));
        expect(t.native_signal_mask == (suspend ? mask({12}) : mask({14})));
        expect(t.wait_registration.active());
        expect(!kernel.scheduler().complete_wait(record.ticket, sched::WaitCompletion::cancelled));
        direct(kernel, g, g.peer, 10); expect(ready(kernel, g, g.peer));
        if (!suspend) finish_service(kernel, g, g.peer, 10, original);
        else
        {
            expect(service(kernel, g, g.peer));
            expect(native::private_signal_deliver(kernel, *kernel.scheduler().find(g.pid), t));
            expect(t.native_frame.pc == handler && !native::signal_waits.find(t));
        }
    }
    retire(kernel, g);
}

void ignore_commit(Kernel &kernel)
{
    const auto g = create(kernel), sender = create(kernel); auto &t = thread(kernel, g.pid, g.peer);
    t.native_signal_mask = mask({10}); action(kernel, g, 10); input(mask({10}));
    expect(!begin(kernel, g, g.peer).complete);
    direct(kernel, g, g.peer, 10); expect(kill(kernel, sender, g.peer, 10) == 0);
    using Codec = native::SignalActionCodec<lu::Profile::x86_64>;
    sched::SignalAction ignored; ignored.handler = 1; const auto bytes = Codec::encode(ignored);
    std::memcpy(mock::memory.data() + scratch - hw::user_begin, bytes.data(), bytes.size());
    mock::write_forbidden = scratch + 128;
    expect(call(kernel, g, g.pid, lu::Operation::rt_sigaction, {10, scratch, scratch + 128, 8}) == -14);
    mock::write_forbidden = 0;
    expect(!ready(kernel, g, g.peer) && !shared(kernel, g).pending_mask().contains(10) &&
           !native::private_signal_queue(t)->pending_mask().contains(10));
    // Discarding previously queued work does not erase the active real_blocked owner.
    expect(kill(kernel, sender, g.peer, 10) == 0 && ready(kernel, g, g.peer));
    finish_service(kernel, g, g.peer, 10, mask({10})); retire(kernel, sender); retire(kernel, g);
}

void fallback_and_realtime(Kernel &kernel)
{
    const auto g = create(kernel); auto &q = shared(kernel, g);
    for (unsigned i = 0; i < 32; ++i)
        expect(q.enqueue({35, 0, i + 1, 23}) == native::PrivateProcessQueue::Enqueue::queued);
    expect(q.enqueue({34, 0, 91, 27}) == native::PrivateProcessQueue::Enqueue::queued);
    input(mask({34, 35}));
    expect(call(kernel, g, g.peer, lu::Operation::rt_sigtimedwait, {mask_address, info_address, timeout_address, 8}) == 34);
    expect_info(info_address, 34, 0, 0, 0);
    for (unsigned i = 0; i < 32; ++i)
    {
        expect(call(kernel, g, g.peer, lu::Operation::rt_sigtimedwait, {mask_address, info_address, timeout_address, 8}) == 35);
        expect_info(info_address, 35, 0, i + 1, 23);
    }
    expect(!q.pending_mask().contains(34) && !q.pending_mask().contains(35)); retire(kernel, g);
}

void scheduler_steps(Kernel &kernel, bool finite)
{
    const auto g = create(kernel); auto &t = thread(kernel, g.pid, g.peer);
    t.native_signal_mask = mask({10}); input(mask({10}), 0, 41); only_waiter(kernel, g);
    expect(!begin(kernel, g, g.peer, false, finite).complete);
    const auto entries = mock::entries; mock::entry_trap = {hw::TrapKind::timer};
    expect(native::step(kernel)); expect(mock::entries == entries && t.syscall_pending);
    if (finite) mock::now = native::signal_waits.find(t)->timeout.deadline();
    else direct(kernel, g, g.peer, 10);
    expect(native::step(kernel));
    expect(mock::entries == entries + 1 && !t.syscall_pending && !native::signal_waits.find(t));
    expect(mock::last_frame.registers[14] == static_cast<u64>(finite ? -11 : 10));
    expect(t.native_signal_mask == mask({10})); retire(kernel, g);
}
void owner_retirement(Kernel &kernel, bool suspend)
{
    const auto g = create(kernel); auto &t = thread(kernel, g.pid, g.peer);
    const auto original = mask({10, 14}); t.native_signal_mask = original;
    input(suspend ? mask({12}) : mask({10})); expect(!begin(kernel, g, g.peer, suspend).complete);
    const auto record = *native::signal_waits.find(t); const auto old_thread = t;
    // Changing the public owner alone cannot validate a private lease record.
    ++t.tid; expect(!native::signal_waits.find(t) && !native::signal_waits.real_blocked(t)); --t.tid;
    expect(native::signal_waits.find(t)->epoch == record.epoch);
    expect(native::signal_waits.exec_mask(t) == original);
    expect(native::retire_native_thread(t).ok());
    expect(!native::signal_waits.find(t) && !native::signal_waits.find(old_thread));
    expect(t.native_signal_mask == original && !t.wait_registration.active());
    expect(!kernel.scheduler().complete_wait(record.ticket, sched::WaitCompletion::ready));
    t.native_active = false; CHECK(kernel.scheduler().exit_thread(g.pid, g.peer));
    auto *p = kernel.scheduler().find(g.pid);
    auto made = kernel.scheduler().create_thread_prepared(g.pid, p->threads()[0].context, [](auto &next) {
        next.native_active = true; reset_frame(next); return native::prepare_native_thread(next);
    }); CHECK(made); auto &replacement = thread(kernel, g.pid, made.value());
    expect(!native::signal_waits.find(replacement)); input(mask({10}));
    expect(!begin(kernel, g, made.value()).complete);
    expect(native::signal_waits.find(replacement)->lease != record.lease && !native::signal_waits.find(old_thread));
    if (replacement.entry_stack.slot == record.lease.slot)
        expect(native::signal_waits.find(replacement)->epoch > record.epoch);
    expect(!kernel.scheduler().complete_wait(record.ticket, sched::WaitCompletion::cancelled));
    retire(kernel, g); expect(!native::signal_waits.find(old_thread));
}

void admission_rollback(Kernel &kernel)
{
    const auto g = create(kernel); auto &t = thread(kernel, g.pid, g.peer);
    const auto original = mask({10, 14}); t.native_signal_mask = original; input(mask({10}));
    activate(kernel, g, g.peer);
    const auto ticket = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::console_read, 71); CHECK(ticket);
    t.syscall_pending = true;
    const auto result = intercept(kernel, g, g.peer, lu::Operation::rt_sigtimedwait, {mask_address, info_address, 0, 8});
    expect(result.complete && result.result < 0);
    expect(t.native_signal_mask == original && !native::signal_waits.find(t));
    expect(t.wait_registration.pending(ticket.value()));
    expect(kernel.scheduler().complete_wait(ticket.value(), sched::WaitCompletion::cancelled));
    t.syscall_pending = false;
    expect(native::poll_signal_masks.begin(t, mask({12})));
    const auto conflicting = begin(kernel, g, g.peer);
    expect(conflicting.complete && conflicting.result < 0);
    expect(native::poll_signal_masks.find(t) && t.native_signal_mask == mask({12}) && !native::signal_waits.find(t));
    native::poll_signal_masks.restore(t); expect(t.native_signal_mask == original);
    input(mask({10}), 9223372036854775807LL, 999999999);
    expect(!begin(kernel, g, g.peer, false, true).complete);
    expect(native::signal_waits.find(t)->timeout.deadline() == 9223372036854775807ULL);
    retire(kernel, g);
    const auto exhausted = create(kernel); auto &owner = thread(kernel, exhausted.pid, exhausted.peer);
    owner.native_signal_mask = original; sched::WaitRegistrationTestAccess::exhaust_idle(owner.wait_registration);
    input(mask({10})); const auto denied = begin(kernel, exhausted, exhausted.peer);
    expect(denied.complete && denied.result < 0 && !native::signal_waits.find(owner));
    expect(owner.native_signal_mask == original && !owner.wait_registration.active());
    expect(native::runtime_status().ok()); retire(kernel, exhausted);
}

void write_exec_image(Kernel &kernel)
{
    static std::array<std::byte, 4112> elf{}; // Readonly VFS retains the byte span.
    const auto field = [&](usize offset, u64 value, usize size) {
        for (usize i = 0; i < size; ++i) elf[offset + i] = static_cast<std::byte>(value >> (8 * i));
    };
    elf[0]=std::byte{0x7f}; elf[1]=std::byte{'E'}; elf[2]=std::byte{'L'}; elf[3]=std::byte{'F'};
    elf[4]=std::byte{2}; elf[5]=std::byte{1}; elf[6]=std::byte{1}; elf[7]=std::byte{3};
    field(16,2,2); field(18,62,2); field(20,1,4); field(24,hw::user_begin+256,8); field(32,64,8);
    field(52,64,2); field(54,56,2); field(56,2,2);
    field(64,1,4); field(68,5,4); field(80,hw::user_begin,8); field(88,hw::user_begin,8);
    field(96,512,8); field(104,4096,8); field(112,4096,8);
    field(120,1,4); field(124,6,4); field(128,4096,8); field(136,hw::user_begin+4096,8);
    field(144,hw::user_begin+4096,8); field(152,16,8); field(160,4096,8); field(168,4096,8);
    elf[256]=std::byte{0xc3}; CHECK(kernel.vfs().create_readonly_file("/signal-wait-exec", elf));
}
void fork_and_exec(Kernel &kernel, bool suspend)
{
    const auto g = create(kernel); auto &leader = thread(kernel, g.pid, g.pid);
    leader.native_signal_mask = mask({10, 14}); input(suspend ? mask({12}) : mask({10}));
    expect(!begin(kernel, g, g.pid, suspend).complete);
    const auto retired = *native::signal_waits.find(leader); const auto old_leader = leader;
    auto &caller = thread(kernel, g.pid, g.peer); caller.native_signal_mask = mask({2, 15});
    activate(kernel, g, g.peer); mock::spaces_enabled = true; bool complete = true;
    const auto forked = native::dispatch(kernel, g.pid, {OK_SYS_FORK, {}}, complete);
    expect(forked > 0 && complete);
    const Group child{static_cast<u64>(forked), 0}; auto &child_thread = thread(kernel, child.pid, child.pid);
    expect(child_thread.native_signal_mask == mask({2, 15}) && !native::signal_waits.find(child_thread));
    expect(!child_thread.wait_registration.active() && !child_thread.native_deadline.armed());
    expect(native::signal_waits.find(old_leader) && leader.wait_registration.pending(retired.ticket));
    retire(kernel, child);
    constexpr char missing[] = "/signal-wait-no-such-file";
    std::memcpy(mock::memory.data() + scratch - hw::user_begin, missing, sizeof(missing));
    activate(kernel, g, g.peer); complete = true;
    expect(native::dispatch(kernel, g.pid, {OK_SYS_EXECVE, {scratch, 0, 0}}, complete) < 0 && complete);
    expect(native::signal_waits.find(old_leader) && thread(kernel, g.pid, g.pid).wait_registration.pending(retired.ticket));
    constexpr char path[] = "/signal-wait-exec";
    std::memcpy(mock::memory.data() + scratch - hw::user_begin, path, sizeof(path));
    activate(kernel, g, g.peer); complete = true;
    expect(native::dispatch(kernel, g.pid, {OK_SYS_EXECVE, {scratch, 0, 0}}, complete) == 0 && !complete);
    auto *p = kernel.scheduler().find(g.pid);
    expect(p->threads().size() == 1 && p->threads()[0].tid == g.pid);
    expect(p->threads()[0].native_signal_mask == mask({2, 15}) && !native::signal_waits.find(p->threads()[0]));
    expect(!native::signal_waits.find(old_leader) && !kernel.scheduler().complete_wait(retired.ticket, sched::WaitCompletion::ready));
    expect(!p->threads()[0].wait_registration.active()); retire(kernel, g);
}

void fatal_cleanup(Kernel &kernel, bool frame_fault)
{
    const auto g = create(kernel); auto &t = thread(kernel, g.pid, g.peer);
    const auto original = mask({10}); t.native_signal_mask = original; action(kernel, g, 10);
    input({}); only_waiter(kernel, g); expect(!begin(kernel, g, g.peer, true).complete);
    const auto old_thread = t;
    if (frame_fault)
    {
        direct(kernel, g, g.peer, 10); native::X86SignalFrame::Placement place;
        CHECK(native::X86SignalFrame::place(t.native_frame.sp, hw::user_begin, hw::user_end, place));
        mock::write_forbidden = place.address;
        mock::entry_trap = {hw::TrapKind::timer}; expect(native::step(kernel)); mock::write_forbidden = 0;
    }
    else
    {
        expect(kill(kernel, g, g.peer, 9) == 0);
        const auto entries = mock::entries; mock::clock_fail = true;
        expect(!native::step(kernel)); mock::clock_fail = false;
        expect(mock::entries == entries); native::runtime_failure = Status::success(); native::started = true;
    }
    expect(!native::signal_waits.find(old_thread));
    auto *p = kernel.scheduler().find(g.pid);
    expect(p && p->state() == sched::ProcessState::zombie);
    for (const auto &member : p->threads()) expect(!member.native_active && !member.entry_stack.pool);
    retire(kernel, g);
}
void fatal_outside_wait(Kernel &kernel, bool process, bool suspend)
{
    const auto g = create(kernel), sender = create(kernel); auto &t = thread(kernel, g.pid, g.peer);
    t.native_signal_mask = mask({10}); input(suspend ? mask({12}) : mask({10}));
    expect(!begin(kernel, g, g.peer, suspend).complete); only_waiter(kernel, g);
    if (process) expect(kill(kernel, sender, g.peer, 15) == 0);
    else expect(call(kernel, sender, sender.peer, lu::Operation::tgkill, {g.pid, g.peer, 15}) == 0);
    retire(kernel, sender); const auto old_thread = t; const auto entries = mock::entries, outputs = mock::outputs;
    mock::entry_trap = {hw::TrapKind::timer}; expect(native::step(kernel));
    auto *p = kernel.scheduler().find(g.pid);
    expect(p && p->state() == sched::ProcessState::zombie && !native::signal_waits.find(old_thread));
    expect(mock::entries == entries && mock::outputs == outputs && !t.native_active);
    expect(native::runtime_status().ok()); retire(kernel, g);
}

void delayed_suspend_disposition(Kernel &kernel)
{
    const auto g = create(kernel); auto &t = thread(kernel, g.pid, g.peer);
    const auto original = mask({10, 14}), temporary = mask({12});
    t.native_signal_mask = original; action(kernel, g, 10); input(temporary);
    expect(!begin(kernel, g, g.peer, true).complete); direct(kernel, g, g.peer, 10);
    const auto ticket = native::signal_waits.find(t)->ticket;
    expect(service(kernel, g, g.peer) && native::signal_waits.saved(t));
    expect(native::signal_waits.find(t)->result_staged);
    using Codec = native::SignalActionCodec<lu::Profile::x86_64>;
    sched::SignalAction ignored; ignored.handler = 1; const auto bytes = Codec::encode(ignored);
    std::memcpy(mock::memory.data() + scratch - hw::user_begin, bytes.data(), bytes.size());
    expect(call(kernel, g, g.pid, lu::Operation::rt_sigaction, {10, scratch, 0, 8}) == 0);
    expect(!native::private_signal_deliver(kernel, *kernel.scheduler().find(g.pid), t));
    expect(native::runtime_status().ok() && t.syscall_pending && t.native_signal_mask == temporary);
    expect(native::signal_waits.find(t) && native::signal_waits.find(t)->original == original);
    expect(native::signal_waits.find(t)->result_staged && t.native_frame.registers[14] == static_cast<u64>(-4));
    expect(t.wait_registration.active() && t.wait_registration.ticket() != ticket);
    expect(!kernel.scheduler().complete_wait(ticket, sched::WaitCompletion::cancelled));
    action(kernel, g, 10); direct(kernel, g, g.peer, 10);
    expect(ready(kernel, g, g.peer) && service(kernel, g, g.peer));
    expect(native::private_signal_deliver(kernel, *kernel.scheduler().find(g.pid), t));
    expect(t.native_frame.pc == handler && get(t.native_frame.sp + native::X86SignalFrame::mask_offset) == original.words[0]);
    expect(get(t.native_frame.sp + native::X86SignalFrame::context_offset + 104) == static_cast<u64>(-4));
    expect(!native::signal_waits.find(t)); retire(kernel, g);
}

void owner_qualification(Kernel &kernel)
{
    const auto g = create(kernel); auto &t = thread(kernel, g.pid, g.peer); input(mask({10}));
    expect(!begin(kernel, g, g.peer).complete); direct(kernel, g, g.peer, 10);
    expect(ready(kernel, g, g.peer));
    ++t.wait_key; expect(!ready(kernel, g, g.peer) && !native::signal_waits.real_blocked(t)); --t.wait_key;
    t.wait_reason = sched::ThreadWaitReason::console_read;
    expect(!ready(kernel, g, g.peer) && !native::signal_waits.real_blocked(t)); t.wait_reason = sched::ThreadWaitReason::signal_wait;
    ++t.native_frame.pc; expect(!ready(kernel, g, g.peer)); --t.native_frame.pc;
    ++t.native_frame.registers[14]; expect(!ready(kernel, g, g.peer)); --t.native_frame.registers[14];
    t.syscall_pending = false; expect(!ready(kernel, g, g.peer) && !native::signal_waits.real_blocked(t)); t.syscall_pending = true;
    ++t.entry_stack.generation; expect(!ready(kernel, g, g.peer) && !native::signal_waits.find(t)); --t.entry_stack.generation;
    expect(native::private_signal_queue(t)->pending_mask().contains(10));
    t.native_signal_mask = mask({10, 12}); // The admitted wanted set is independent of later live mask changes.
    finish_service(kernel, g, g.peer, 10, {}); retire(kernel, g);
}

void pending_same_number_and_kernel_info(Kernel &kernel)
{
    const auto g = create(kernel); auto &t = thread(kernel, g.pid, g.peer);
    input(mask({10}), 7, 2); const auto clocks = mock::clock_reads;
    expect(shared(kernel, g).enqueue({10, 0, 91, 37}) == native::PrivateProcessQueue::Enqueue::queued);
    direct(kernel, g, g.peer, 10);
    expect(call(kernel, g, g.peer, lu::Operation::rt_sigtimedwait, {mask_address, info_address, timeout_address, 8}) == 10);
    expect_info(info_address, 10, -6, g.pid, 0);
    expect(shared(kernel, g).pending_mask().contains(10));
    expect(call(kernel, g, g.peer, lu::Operation::rt_sigtimedwait, {mask_address, info_address, timeout_address, 8}) == 10);
    expect_info(info_address, 10, 0, 91, 37);
    expect(native::private_signal_queue(t)->enqueue_kernel(10) == native::PrivateSignalQueue::Enqueue::queued);
    expect(call(kernel, g, g.peer, lu::Operation::rt_sigtimedwait, {mask_address, info_address, timeout_address, 8}) == 10);
    expect_info(info_address, 10, 128, 0, 0); expect(mock::clock_reads == clocks);
    expect(get(timeout_address) == 7 && get(timeout_address + 8) == 2);
    input(mask({10}), 0, 7); mock::clock_fail = true;
    const auto failed = begin(kernel, g, g.peer, false, true); mock::clock_fail = false;
    expect(failed.complete && failed.result == -5 && !native::signal_waits.find(t));
    expect(native::runtime_status().ok()); retire(kernel, g);
}

std::string_view trace_line(std::string_view prefix)
{
    expect(!trace_capture::overflow);
    const std::string_view text{trace_capture::text.data(), trace_capture::size};
    const auto at = text.find(prefix); expect(at != std::string_view::npos);
    const auto end = text.find('\n', at); expect(end != std::string_view::npos);
    return text.substr(at, end - at);
}
std::string_view trace_field(std::string_view line, std::string_view key)
{
    char lookup[64]{}; const auto length = std::snprintf(lookup, sizeof(lookup), " %.*s=", int(key.size()), key.data());
    CHECK(length > 0 && usize(length) < sizeof(lookup));
    const auto at = line.find({lookup, static_cast<usize>(length)}); expect(at != std::string_view::npos);
    const auto begin = at + static_cast<usize>(length), end = line.find(' ', begin);
    return line.substr(begin, end == std::string_view::npos ? end : end - begin);
}
void trace_number(std::string_view line, std::string_view key, u64 value)
{
    char number[32]{}; std::snprintf(number, sizeof(number), "%llu", static_cast<unsigned long long>(value));
    expect(trace_field(line, key) == number);
}
void diagnostic_timed(Kernel &kernel, unsigned outcome)
{
    const auto g = create(kernel); auto &t = thread(kernel, g.pid, g.peer);
    const auto original = mask({10, 14}); t.native_signal_mask = original; input(mask({10}), 0, 73);
    trace_capture::begin(); expect(!begin(kernel, g, g.peer, false, true).complete);
    const auto record = *native::signal_waits.find(t); const auto armed = trace_line("OK_NATIVE_SIGNAL_WAIT_ARM");
    trace_number(armed, "pid", g.pid); trace_number(armed, "tid", g.peer);
    trace_number(armed, "epoch", record.epoch); trace_number(armed, "wait", record.ticket.generation);
    expect(trace_field(armed, "kind") == "timed");
    trace_number(armed, "syscall", *lu::Codec<lu::Profile::x86_64>::syscall_number(lu::Operation::rt_sigtimedwait));
    trace_number(armed, "pc", site); trace_number(armed, "set", mask_address);
    trace_number(armed, "info", info_address); trace_number(armed, "timeout", timeout_address);
    trace_number(armed, "sigsetsize", 8); trace_number(armed, "wanted0", mask({10}).words[0]);
    trace_number(armed, "wanted1", 0); trace_number(armed, "original0", original.words[0]);
    trace_number(armed, "original1", 0); trace_number(armed, "active0", mask({14}).words[0]);
    trace_number(armed, "active1", 0); trace_number(armed, "deadline", record.timeout.deadline());
    trace_number(armed, "live", 1);
    if (outcome == 0)
    {
        // A generic wake must trace retained decoded provenance, including the
        // original deadline, even if user-memory/register arguments are changed.
        expect(kernel.scheduler().complete_wait(record.ticket, sched::WaitCompletion::ready));
        input(mask({12}), 99, 1); t.native_frame.registers[9] = 0;
        t.native_frame.registers[10] = 0; t.native_frame.registers[11] = 0; t.native_frame.registers[5] = 16;
        expect(!service(kernel, g, g.peer));
        const auto rearmed = trace_line("OK_NATIVE_SIGNAL_WAIT_REARM");
        trace_number(rearmed, "old_wait", record.ticket.generation);
        trace_number(rearmed, "wait", t.wait_registration.ticket().generation);
        trace_number(rearmed, "set", mask_address); trace_number(rearmed, "info", info_address);
        trace_number(rearmed, "timeout", timeout_address); trace_number(rearmed, "sigsetsize", 8);
        trace_number(rearmed, "deadline", record.timeout.deadline());
        trace_number(rearmed, "wanted0", mask({10}).words[0]); trace_number(rearmed, "active0", mask({14}).words[0]);
        expect(trace_field(rearmed, "cause") == "ready");
        trace_number(rearmed, "published", 0); trace_number(rearmed, "staged", 0);
    }
    if (outcome <= 1) direct(kernel, g, g.peer, 10);
    else if (outcome == 2) mock::now = record.timeout.deadline();
    else expect(kernel.scheduler().complete_wait(record.ticket, sched::WaitCompletion::cancelled));
    if (outcome == 1) mock::write_forbidden = info_address;
    finish_service(kernel, g, g.peer, outcome == 0 ? 10 : outcome == 1 ? -14 : outcome == 2 ? -11 : -4, original);
    mock::write_forbidden = 0;
    const auto completed = trace_line("OK_NATIVE_SIGNAL_WAIT_COMPLETE");
    expect(trace_field(completed, "result") == (outcome == 0 ? "10" : outcome == 1 ? "-14" : outcome == 2 ? "-11" : "-4"));
    expect(trace_field(completed, "code") == (outcome <= 1 ? "-6" : "0"));
    expect(trace_field(completed, "outcome") == (outcome == 0 ? "received" : outcome == 1 ? "efault" : outcome == 2 ? "timeout" : "cancelled"));
    trace_number(completed, "sender_pid", outcome <= 1 ? g.pid : 0); trace_number(completed, "sender_uid", 0);
    trace_number(completed, "info", info_address); trace_number(completed, "signal", outcome <= 1 ? 10 : 0);
    trace_number(completed, "published", 1); trace_number(completed, "staged", 0);
    trace_number(completed, "consumed", outcome <= 1); trace_number(completed, "live", 0);
    trace_number(completed, "deadline", record.timeout.deadline()); trace_number(completed, "now", mock::now);
    trace_capture::end(); retire(kernel, g);
}
void diagnostic_suspend(Kernel &kernel)
{
    const auto g = create(kernel); auto &t = thread(kernel, g.pid, g.peer);
    const auto original = mask({10, 14}), temporary = mask({12}); t.native_signal_mask = original;
    action(kernel, g, 10); input(temporary); trace_capture::begin();
    expect(!begin(kernel, g, g.peer, true).complete); const auto record = *native::signal_waits.find(t);
    const auto armed = trace_line("OK_NATIVE_SIGNAL_WAIT_ARM");
    expect(trace_field(armed, "kind") == "suspend");
    trace_number(armed, "syscall", *lu::Codec<lu::Profile::x86_64>::syscall_number(lu::Operation::rt_sigsuspend));
    trace_number(armed, "set", mask_address); trace_number(armed, "info", 0); trace_number(armed, "timeout", 0);
    trace_number(armed, "sigsetsize", 8); trace_number(armed, "deadline", 0);
    trace_number(armed, "original0", original.words[0]); trace_number(armed, "active0", temporary.words[0]);
    direct(kernel, g, g.peer, 10); expect(service(kernel, g, g.peer));
    const auto completed = trace_line("OK_NATIVE_SIGNAL_WAIT_COMPLETE");
    trace_number(completed, "pid", g.pid); trace_number(completed, "tid", g.peer);
    trace_number(completed, "epoch", record.epoch); trace_number(completed, "wait", record.ticket.generation);
    trace_number(completed, "published", 0); trace_number(completed, "staged", 1);
    trace_number(completed, "consumed", 0); trace_number(completed, "live", 1);
    expect(trace_field(completed, "result") == "-4" && trace_field(completed, "code") == "-6");
    const std::string_view before{trace_capture::text.data(), trace_capture::size};
    expect(before.find("OK_NATIVE_SIGNAL_WAIT_COMMIT") == std::string_view::npos);
    expect(native::private_signal_deliver(kernel, *kernel.scheduler().find(g.pid), t));
    const auto committed = trace_line("OK_NATIVE_SIGNAL_WAIT_COMMIT");
    trace_number(committed, "pid", g.pid); trace_number(committed, "tid", g.peer);
    trace_number(committed, "epoch", record.epoch); trace_number(committed, "wait", record.ticket.generation);
    trace_number(committed, "published", 1); trace_number(committed, "signal", 10);
    trace_number(committed, "live_before", 1); expect(trace_field(committed, "result") == "-4");
    expect(committed.data() > completed.data() && !native::signal_waits.find(t));
    trace_capture::end(); retire(kernel, g);
}


void root_ready_generation(Kernel &kernel, bool process, bool explicit_ignore, bool cancel)
{
    const auto g = create(kernel), sender = create(kernel);
    auto &t = thread(kernel, g.pid, g.peer);
    const u32 signal = explicit_ignore ? 10 : 28;
    action(kernel, g, signal, explicit_ignore ? 1 : 0);
    t.native_signal_mask = mask({signal});
    thread(kernel, g.pid, g.pid).native_signal_mask = mask({signal});
    input(mask({signal})); expect(!begin(kernel, g, g.peer).complete);
    const auto owned = *native::signal_waits.find(t);
    expect(kernel.scheduler().complete_wait(owned.ticket,
        cancel ? sched::WaitCompletion::cancelled : sched::WaitCompletion::ready));
    // This send executes before the recipient restores its mask or rearms.
    if (process) expect(kill(kernel, sender, g.peer, signal) == 0);
    else expect(call(kernel, sender, sender.peer, lu::Operation::tgkill, {g.pid, g.peer, signal}) == 0);
    auto pending = [&] { return (process ? shared(kernel, g).pending_mask()
                                      : native::private_signal_queue(t)->pending_mask()).contains(signal); };
    expect(pending());
    if (cancel)
    {
        finish_service(kernel, g, g.peer, -4, mask({signal}));
        expect(pending());
    }
    else
    {
        expect(!service(kernel, g, g.peer));
        const auto *rearmed = native::signal_waits.find(t);
        expect(rearmed && rearmed->epoch == owned.epoch && rearmed->ticket != owned.ticket);
        expect(rearmed->timeout.deadline() == owned.timeout.deadline() && pending());
        finish_service(kernel, g, g.peer, signal, mask({signal}));
        expect(!pending());
        expect_info(info_address, signal, process ? 0 : -6, sender.pid, 0);
    }
    retire(kernel, sender); retire(kernel, g);
}

} // namespace

int main()
{
    static Kernel kernel; CHECK(kernel.boot({})); CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    for (const bool process : {false, true}) for (const bool ignored : {false, true})
        for (const bool cancel : {false, true}) root_ready_generation(kernel, process, ignored, cancel);
    for (unsigned outcome = 0; outcome < 4; ++outcome) diagnostic_timed(kernel, outcome);
    diagnostic_suspend(kernel);
    immediate_and_precedence(kernel); admission_faults(kernel); destructive_copyout(kernel);
    pending_same_number_and_kernel_info(kernel); owner_qualification(kernel);
    for (const bool process : {false, true}) for (const bool finite : {false, true}) for (const bool fault : {false, true})
        deferred_match(kernel, process, finite, fault);
    for (unsigned outcome = 0; outcome < 4; ++outcome) finite_ordering(kernel, outcome);
    for (const bool process : {false, true}) for (const bool alias : {false, true}) for (const bool blocked : {false, true})
        for (const bool explicit_ignore : {false, true}) ignored_generation(kernel, process, alias, blocked, explicit_ignore);
    competing_waiters(kernel); suspend_delivery(kernel, false, false); suspend_delivery(kernel, true, true);
    ignored_suspend(kernel); shared_ignored_suspend(kernel); delayed_suspend_disposition(kernel);
    unrelated_ignored_timed(kernel, false); unrelated_ignored_timed(kernel, true); ignore_commit(kernel); fallback_and_realtime(kernel);
    for (const bool suspend : {false, true}) for (const bool cancel : {false, true}) cancellation_and_wake(kernel, suspend, cancel);
    cancellation_and_wake(kernel, false, false, true);
    scheduler_steps(kernel, false); scheduler_steps(kernel, true);
    owner_retirement(kernel, false); owner_retirement(kernel, true); admission_rollback(kernel);
    write_exec_image(kernel); fork_and_exec(kernel, false); fork_and_exec(kernel, true);
    fatal_cleanup(kernel, false); fatal_cleanup(kernel, true);
    for (const bool process : {false, true}) for (const bool suspend : {false, true}) fatal_outside_wait(kernel, process, suspend);
    expect(native::entry_stacks.accounting().leased_slots == 0);
    for (const auto &record : native::signal_waits.records()) expect(record.phase == decltype(native::signal_waits)::Phase::none);
    std::printf("SIGNAL_WAIT_PASS checks=%zu hosted=1 native=0\n", checks);
}
