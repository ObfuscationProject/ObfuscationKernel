// Actual private native runtime with hosted trap/usercopy mappings.
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
    if (!value) { std::fprintf(stderr, "PROCESS_SIGNAL_FAIL line=%u checks=%zu\n", at.line(), checks); std::abort(); }
}
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
    auto made = kernel.scheduler().create_user_process("process-signals",
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
i64 call(Kernel &kernel, Group g, u64 tid, lu::Operation op, std::array<u64, 6> a)
{
    auto &t = thread(kernel, g.pid, tid); auto &f = t.native_frame;
    f.registers[14] = *lu::Codec<lu::Profile::x86_64>::syscall_number(op);
    f.registers[9] = a[0]; f.registers[10] = a[1]; f.registers[11] = a[2];
    f.registers[5] = a[3]; f.registers[7] = a[4]; f.registers[6] = a[5];
    bool complete = true; i64 result = -999;
    expect(native::private_signal_call(kernel, *kernel.scheduler().find(g.pid), t, complete, result));
    return result;
}

template <u32 Maximum> void queue_transactions()
{
    using Shared = native::ProcessPendingSignals<Maximum, 1, 7>;
    using Directed = sched::PendingSignals<Maximum>;
    Shared q, other; Directed directed;
    expect(q.reset({4, 4, 1}) && other.reset({4, 4, 1}) && directed.reset({4, 5, 8}));
    expect(q.enqueue({32, 0, 44, 77}) == Shared::Enqueue::queued);
    expect(q.enqueue({33, 0, 55, 88}) == Shared::Enqueue::queued);
    expect(q.enqueue({33, 0, 99, 99}) == Shared::Enqueue::coalesced);
    expect(q.enqueue({32, 0, 99, 99}) == Shared::Enqueue::coalesced);
    sched::SignalMask mask; mask.set(32);
    typename Shared::Delivery fallback, allocated;
    expect(q.peek(mask, fallback) && fallback.info == sched::PendingSignalInfo{33, 0, 0, 0});
    expect(!other.consume(fallback));
    auto forged = fallback; forged.info.sender_pid = 4; expect(!q.consume(forged));
    expect(q.peek({}, allocated) && allocated.info == sched::PendingSignalInfo{32, 0, 44, 77});
    expect(q.consume(allocated));
    // A returned allocation slot replaces the pending fallback for that number.
    expect(q.enqueue({33, 0, 66, 11}) == Shared::Enqueue::queued);
    expect(!q.consume(fallback));
    expect(q.peek({}, allocated) && allocated.info == sched::PendingSignalInfo{33, 0, 66, 11});
    expect(q.consume(allocated) && !q.peek({}, allocated));
    expect(q.enqueue({40, 0, 7, 8}) == Shared::Enqueue::queued);
    expect(q.enqueue({Maximum, 0, 9, 10}) == Shared::Enqueue::queued);
    mask = {}; mask.set(40); expect(q.peek(mask, fallback));
    q.discard(Maximum); expect(!q.consume(fallback));
    expect(q.enqueue({Maximum, 0, 1, 2}) == Shared::Enqueue::queued);
    expect(!q.consume(fallback));
    expect(q.peek(mask, fallback)); q.retire(); expect(!q.consume(fallback));
    expect(q.reset({4, 4, 2}) && !q.consume(fallback));
    expect(q.enqueue({10, 0, 1, 2}) == Shared::Enqueue::queued);
    expect(directed.enqueue({12, -6, 1, 2}) == Directed::Enqueue::queued);
    native::SignalPendingView<Directed, Shared> v{&directed, &q};
    typename decltype(v)::Delivery ticket;
    expect(v.peek({}, ticket) && !ticket.from_shared && ticket.info.signal == 12);
    expect(v.consume(ticket)); expect(v.peek({}, ticket) && ticket.from_shared && ticket.info.signal == 10);
    expect(directed.reset({4, 5, 9}) && !v.consume(ticket));
    native::SignalPendingView<Directed, Shared> fresh{&directed, &q};
    expect(fresh.peek({}, ticket) && fresh.consume(ticket));
    using Short = native::ProcessPendingSignals<Maximum, 1, 1>;
    Short shortq; expect(shortq.reset({1, 1, 1}));
    expect(shortq.enqueue({10, 0, 0, 0}) == Short::Enqueue::queued);
    expect(shortq.enqueue({10, 0, 9, 9}) == Short::Enqueue::coalesced);
    expect(shortq.enqueue({11, 0, 0, 0}) == Short::Enqueue::exhausted);
    expect(shortq.enqueue({32, 0, 9, 9}) == Short::Enqueue::queued);
    expect(shortq.enqueue({33, 0, 9, 9}) == Short::Enqueue::exhausted);
    typename Short::Delivery last; sched::SignalMask blocked; blocked.set(10);
    expect(shortq.peek(blocked, last) && shortq.consume(last));
    expect(shortq.reset({2, 2, 2}));
    expect(shortq.enqueue({32, 0, 9, 9}) == Short::Enqueue::exhausted);
}

void routing(Kernel &kernel)
{
    const auto g = create(kernel), sender = create(kernel);
    kernel.scheduler().find(sender.pid)->set_credentials({.uid = 71, .euid = 0});
    action(kernel, g, 10); action(kernel, g, 12);
    for (auto &t : kernel.scheduler().find(g.pid)->threads()) { t.native_signal_mask.set(10); t.native_signal_mask.set(12); }
    expect(call(kernel, sender, sender.pid, lu::Operation::kill, {g.peer, 10}) == 0);
    expect(kill(kernel, sender, g.pid, 10) == 0);
    expect(shared(kernel, g).pending_mask().contains(10));
    direct(kernel, g, g.peer, 12);
    expect(call(kernel, g, g.peer, lu::Operation::rt_sigpending, {scratch, 8}) == 0);
    expect(get(scratch) == ((u64{1} << 9) | (u64{1} << 11)));
    expect(call(kernel, g, g.pid, lu::Operation::rt_sigpending, {scratch, 8}) == 0 && get(scratch) == (u64{1} << 9));
    const auto copies = mock::outputs;
    expect(call(kernel, g, g.peer, lu::Operation::rt_sigpending, {0, 0}) == 0 && mock::outputs == copies);
    expect(call(kernel, g, g.peer, lu::Operation::rt_sigpending, {scratch, 9}) == -22 && mock::outputs == copies);
    mock::memory[scratch-hw::user_begin+1]=std::byte{0x7a};
    expect(call(kernel,g,g.peer,lu::Operation::rt_sigpending,{scratch,1})==0 && get(scratch,1)==0 &&
           mock::memory[scratch-hw::user_begin+1]==std::byte{0x7a});
    auto v = view(kernel, g, g.peer); native::PrivateSignalView::Delivery picked;
    expect(!v.peek(thread(kernel, g.pid, g.peer).native_signal_mask, picked));
    thread(kernel, g.pid, g.peer).native_signal_mask = {};
    expect(v.peek({}, picked) && picked.info.signal == 12 && picked.info.code == -6); expect(v.consume(picked));
    expect(v.peek({}, picked) && picked.info == sched::PendingSignalInfo{10, 0, static_cast<u32>(sender.pid), 71});
    // Sender retirement cannot invalidate its value-owned SI_USER payload.
    retire(kernel, sender);
    auto *p = kernel.scheduler().find(g.pid); auto &peer = thread(kernel, g.pid, g.peer);
    reset_frame(peer); expect(native::private_signal_deliver(kernel, *p, peer));
    expect(peer.native_frame.pc == handler && peer.native_frame.registers[9] == 10);
    const auto info = peer.native_frame.registers[10];
    expect(get(info + 8, 4) == 0 && get(info + 16, 4) == sender.pid && get(info + 20, 4) == 71);
    expect(!shared(kernel, g).pending_mask().contains(10));
    expect(!view(kernel, g, g.pid).consume(picked));
    // Directed and shared occurrences of one number remain independently owned.
    peer.native_signal_mask = {}; direct(kernel, g, g.peer, 10); expect(kill(kernel, g, g.pid, 10) == 0);
    v = view(kernel, g, g.peer); expect(v.peek({}, picked) && !picked.from_shared); expect(v.consume(picked));
    expect(v.peek({}, picked) && picked.from_shared); expect(v.consume(picked));
    // A masked directed number does not hide another eligible shared number.
    direct(kernel, g, g.peer, 12); expect(kill(kernel, g, g.pid, 10) == 0);
    sched::SignalMask blocked; blocked.set(12); expect(v.peek(blocked, picked) && picked.info.signal == 10);
    expect(v.consume(picked)); retire(kernel, g);
}

void ignored_and_copyout(Kernel &kernel)
{
    for (bool alias : {false, true}) for (bool explicit_ignore : {false, true}) for (bool named_blocked : {false, true})
    {
        const auto g = create(kernel); const u32 signal = explicit_ignore ? 10 : 28;
        action(kernel, g, signal, explicit_ignore ? 1 : 0);
        thread(kernel, g.pid, g.pid).native_signal_mask.set(signal);
        thread(kernel, g.pid, g.peer).native_signal_mask.set(signal);
        const auto named = alias ? g.peer : g.pid;
        thread(kernel, g.pid, named).native_signal_mask.set(signal, named_blocked);
        expect(kill(kernel, g, named, signal) == 0);
        expect(shared(kernel, g).pending_mask().contains(signal) == named_blocked);
        retire(kernel, g);
    }
    const auto g = create(kernel); action(kernel, g, 10);
    direct(kernel, g, g.pid, 10); direct(kernel, g, g.peer, 10); expect(kill(kernel, g, g.pid, 10) == 0);
    using Codec = native::SignalActionCodec<lu::Profile::x86_64>;
    sched::SignalAction ignored{}; ignored.handler = 1; const auto bytes = Codec::encode(ignored);
    std::memcpy(mock::memory.data() + scratch - hw::user_begin, bytes.data(), bytes.size());
    mock::write_forbidden = scratch + 128;
    expect(call(kernel, g, g.pid, lu::Operation::rt_sigaction, {10, scratch, scratch + 128, 8}) == -14);
    mock::write_forbidden = 0;
    expect(native::private_signal_table(kernel, g.pid)->find(10)->handler == 1);
    expect(!shared(kernel, g).pending_mask().contains(10));
    expect(!native::private_signal_queue(thread(kernel, g.pid, g.pid))->pending_mask().contains(10));
    expect(!native::private_signal_queue(thread(kernel, g.pid, g.peer))->pending_mask().contains(10));
    // Only the active temporary mask influences ignored-generation admission.
    auto &peer = thread(kernel, g.pid, g.peer); peer.native_signal_mask.set(10);
    expect(native::poll_signal_masks.begin(peer, {}));
    expect(kill(kernel, g, g.peer, 10) == 0 && !shared(kernel, g).pending_mask().contains(10));
    native::poll_signal_masks.restore(peer); peer.native_signal_mask = {};
    sched::SignalMask temp; temp.set(10); expect(native::poll_signal_masks.begin(peer, temp));
    expect(kill(kernel, g, g.peer, 10) == 0 && shared(kernel, g).pending_mask().contains(10));
    native::poll_signal_masks.restore(peer); retire(kernel, g);
}

void permissions_and_lifecycle(Kernel &kernel)
{
    const auto sender = create(kernel), g = create(kernel);
    auto *s = kernel.scheduler().find(sender.pid); auto *p = kernel.scheduler().find(g.pid);
    s->set_credentials({.uid = 7, .euid = 8, .suid = 9});
    p->set_credentials({.uid = 11, .euid = 7, .suid = 12});
    const auto owners = [&] { usize n{}; for (const auto &entry : native::private_signal_actions) n += entry.pid != 0; return n; };
    const auto before = owners();
    expect(kill(kernel, sender, g.pid, 0) == -1);
    expect(kill(kernel, sender, 0x7fffffff, 65) == -3);
    expect(kill(kernel, sender, g.pid, 65) == -22);
    expect(kill(kernel, sender, 0, 0) == 0 && kill(kernel, sender, ~u64{0}, 0) == -95);
    expect(owners() == before);
    for (unsigned match = 0; match < 5; ++match)
    {
        const user::Credentials creds[] = {{.uid=7,.euid=99}, {.uid=99,.euid=7}, {.uid=99,.euid=12}, {.uid=12,.euid=99}, {.uid=99,.euid=0}};
        s->set_credentials(creds[match]); p->set_credentials({.uid=7,.euid=55,.suid=12});
        expect(kill(kernel, sender, g.peer, 0) == 0 && owners() == before);
    }
    p->resources().personality=sched::UserPersonality::native;
    expect(kill(kernel,sender,g.peer,0)==0 && kill(kernel,sender,g.peer,10)==-95 && owners()==before);
    p->resources().personality=sched::UserPersonality::linux;
    expect(kill(kernel,sender,g.pid,19)==-95 && kill(kernel,sender,g.pid,18)==-95 && owners()==before);
    action(kernel, g, 10); expect(kill(kernel, sender, g.pid, 10) == 0);
    expect(kill(kernel,sender,(u64{1}<<32)|g.peer,(u64{1}<<32)|10)==0);
    auto *q = &shared(kernel, g); native::PrivateProcessQueue::Delivery ticket; expect(q->peek({}, ticket));
    // Other PCB removal cannot move the shared queue or invalidate its ticket.
    retire(kernel, sender); expect(&shared(kernel, g) == q && q->consume(ticket));
    expect(kill(kernel, g, g.pid, 10) == 0);
    CHECK(native::retire_native_thread(thread(kernel, g.pid, g.pid)));
    thread(kernel, g.pid, g.pid).native_active = false;
    CHECK(kernel.scheduler().exit_thread(g.pid, g.pid));
    expect(kill(kernel, g, g.pid, 10) == 0); // Retired leader still names its living group.
    native::PrivateSignalView::Delivery selected;
    auto v = view(kernel, g, g.peer); expect(v.peek({}, selected) && selected.from_shared);
    // Fork executes the actual copy/prepare/private lifecycle path.
    CHECK(kernel.posix().activate(g.pid)); CHECK(kernel.scheduler().activate_thread(g.pid, g.peer));
    mock::spaces_enabled = true; bool complete = true;
    const auto child_pid = native::dispatch(kernel, g.pid, {OK_SYS_FORK, {}}, complete);
    expect(child_pid > 0 && complete);
    const Group child{static_cast<u64>(child_pid), 0};
    expect(shared(kernel, child).pending_mask() == sched::SignalMask{});
    expect(native::private_signal_queue(thread(kernel, child.pid, child.pid))->pending_mask() == sched::SignalMask{});
    expect(shared(kernel, g).pending_mask().contains(10));
    retire(kernel, child);
    p = kernel.scheduler().find(g.pid);
    auto newpeer = kernel.scheduler().create_thread_prepared(g.pid, p->threads()[0].context, [](auto &t) {
        t.native_active = true; reset_frame(t); return native::prepare_native_thread(t);
    }); CHECK(newpeer);
    expect(&shared(kernel, g) == q && view(kernel, g, newpeer.value()).peek({}, selected));
    auto departed_view = view(kernel, g, newpeer.value());
    expect(departed_view.peek({}, selected));
    CHECK(native::retire_native_thread(thread(kernel, g.pid, newpeer.value())));
    thread(kernel, g.pid, newpeer.value()).native_active = false;
    CHECK(kernel.scheduler().exit_thread(g.pid, newpeer.value()));
    expect(!departed_view.consume(selected));
    expect(view(kernel, g, g.peer).peek({}, selected));
    expect(kill(kernel, g, newpeer.value(), 0) == -3);
    expect(q->peek({}, ticket));
    // Group retirement invalidates both allocated and fallback shared tickets.
    native::finish(kernel, g.pid, 17);
    expect(!q->consume(ticket) && !v.consume(selected));
    expect(kill(kernel, g, g.pid, 0) == 0 && kill(kernel, g, g.pid, 10) == 0);
    expect(kill(kernel, g, g.pid, 65) == -22);
    CHECK(kernel.scheduler().reap_orphans());
    const auto replacement = create(kernel);
    auto &rebound = shared(kernel, replacement);
    expect(&rebound == q && rebound.identity() != ticket.owner && rebound.identity().generation > ticket.owner.generation &&
           !q->consume(ticket));
    expect(kill(kernel, replacement, g.pid, 65) == -3);
    retire(kernel, replacement);
}

void realtime_pressure(Kernel &kernel)
{
    const auto g = create(kernel); auto &q = shared(kernel, g);
    for (unsigned i = 0; i < 32; ++i) expect(kill(kernel, g, g.pid, 35) == 0);
    expect(q.realtime_size() == 32);
    expect(kill(kernel, g, g.pid, 35) == 0 && kill(kernel, g, g.pid, 34) == 0 && kill(kernel, g, g.pid, 34) == 0);
    native::PrivateProcessQueue::Delivery ticket;
    expect(q.peek({}, ticket) && ticket.info == sched::PendingSignalInfo{34, 0, 0, 0});
    const auto fallback = ticket; q.discard(35);
    expect(kill(kernel, g, g.pid, 34) == 0 && !q.consume(fallback));
    expect(q.peek({}, ticket) && ticket.info == sched::PendingSignalInfo{34, 0, static_cast<u32>(g.pid), 0});
    expect(q.consume(ticket) && !q.peek({}, ticket));
    for (unsigned i = 0; i < 32; ++i) expect(kill(kernel, g, g.pid, 35) == 0);
    for (u32 signal = 32; signal <= 64; ++signal) expect(kill(kernel, g, g.pid, signal) == 0);
    usize count{}; while (q.peek({}, ticket)) { expect(q.consume(ticket)); ++count; }
    expect(count == 64); // 32 allocated at 35, and one bit for each other number.
    auto *directed = native::private_signal_queue(thread(kernel, g.pid, g.peer));
    for (unsigned i = 0; i < 32; ++i)
        expect(directed->enqueue({35, -6, static_cast<u32>(g.pid), 0}) == native::PrivateSignalQueue::Enqueue::queued);
    expect(call(kernel, g, g.pid, lu::Operation::tgkill, {g.pid, g.peer, 34}) == -11);
    expect(!q.pending_mask().contains(34));
    retire(kernel, g);
}

void nonleader_exec(Kernel &kernel)
{
    // Real loader input and runtime exec transaction, never guest execution.
    struct HostedEntropy final : driver::EntropySource
    {
        Status fill(std::span<std::byte> output) override
        {
            for (usize index = 0; index < output.size(); ++index)
                output[index] = static_cast<std::byte>(index + 1);
            return Status::success();
        }
    };
    static HostedEntropy entropy;
    kernel.posix().set_entropy_source(&entropy);
    std::array<std::byte, 4112> elf{};
    const auto put = [&](usize offset, u64 value, usize size) {
        for (usize i = 0; i < size; ++i) elf[offset + i] = static_cast<std::byte>(value >> (8 * i));
    };
    elf[0]=std::byte{0x7f}; elf[1]=std::byte{'E'}; elf[2]=std::byte{'L'}; elf[3]=std::byte{'F'};
    elf[4]=std::byte{2}; elf[5]=std::byte{1}; elf[6]=std::byte{1}; elf[7]=std::byte{3};
    put(16,2,2); put(18,62,2); put(20,1,4); put(24,hw::user_begin+256,8); put(32,64,8);
    put(52,64,2); put(54,56,2); put(56,2,2);
    put(64,1,4); put(68,5,4); put(80,hw::user_begin,8); put(88,hw::user_begin,8);
    put(96,512,8); put(104,4096,8); put(112,4096,8);
    put(120,1,4); put(124,6,4); put(128,4096,8); put(136,hw::user_begin+4096,8);
    put(144,hw::user_begin+4096,8); put(152,16,8); put(160,4096,8); put(168,4096,8);
    elf[256]=std::byte{0xc3};
    CHECK(kernel.vfs().create_readonly_file("/process-signal-exec", elf));
    const auto g = create(kernel); action(kernel,g,10); action(kernel,g,12); action(kernel,g,14,1);
    auto &caller = thread(kernel,g.pid,g.peer); caller.native_signal_mask.set(10); caller.native_signal_mask.set(12);
    thread(kernel,g.pid,g.pid).native_signal_mask.set(10);
    expect(kill(kernel,g,g.pid,10)==0); direct(kernel,g,g.peer,12); direct(kernel,g,g.pid,4);
    auto *directed_before = native::private_signal_queue(caller);
    expect(directed_before->pending_mask().contains(12));
    auto *q=&shared(kernel,g); native::PrivateProcessQueue::Delivery ticket; expect(q->peek({},ticket));
    const auto identity=q->identity();
    CHECK(kernel.posix().activate(g.pid)); CHECK(kernel.scheduler().activate_thread(g.pid,g.peer));
    expect(kernel.scheduler().current_thread()->tid==g.peer);
    constexpr char path[]="/process-signal-exec";
    std::memcpy(mock::memory.data()+scratch-hw::user_begin,path,sizeof(path));
    mock::spaces_enabled=true; bool complete=true;
    const auto exec_result = native::dispatch(kernel,g.pid,{OK_SYS_EXECVE,{scratch,0,0}},complete);
    if (exec_result != 0 || complete)
        std::fprintf(stderr, "PROCESS_SIGNAL_EXEC result=%lld complete=%u\n",
                     static_cast<long long>(exec_result), static_cast<unsigned>(complete));
    expect(exec_result == 0 && !complete);
    auto *p=kernel.scheduler().find(g.pid); expect(p&&p->threads().size()==1&&p->threads()[0].tid==g.pid);
    expect(&shared(kernel,g)==q&&q->identity()==identity&&q->pending_mask().contains(10));
    expect(native::private_signal_queue(p->threads()[0])->pending_mask().contains(12));
    expect(!native::private_signal_queue(p->threads()[0])->pending_mask().contains(4));
    expect(p->threads()[0].native_signal_mask.contains(10)&&p->threads()[0].native_signal_mask.contains(12));
    expect(native::private_signal_table(kernel,g.pid)->find(10)->handler==0);
    expect(native::private_signal_table(kernel,g.pid)->find(14)->handler==1);
    expect(q->consume(ticket)); retire(kernel,g);
    kernel.posix().set_entropy_source(nullptr);
}

void blocked_fatal(Kernel &kernel)
{
    const auto sender = create(kernel), g = create(kernel);
    action(kernel, g, 10);
    for (auto tid : {g.pid, g.peer})
    {
        CHECK(kernel.scheduler().activate_thread(g.pid, tid));
        auto ticket = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::console_read, 0); CHECK(ticket);
        thread(kernel, g.pid, tid).syscall_pending = true;
    }
    expect(kill(kernel, sender, g.pid, 10) == -95);
    for (auto &t : kernel.scheduler().find(g.pid)->threads()) t.native_signal_mask.set(10);
    expect(kill(kernel, sender, g.pid, 10) == 0 && shared(kernel, g).pending_mask().contains(10));
    const auto entries = mock::entries;
    expect(kill(kernel, sender, g.peer, 9) == 0);
    expect(kernel.scheduler().find(g.pid)->state() == sched::ProcessState::blocked);
    // Fatal service is independent of both frame readiness and wait admission.
    native::private_process_signal_fatal(kernel);
    expect(mock::entries == entries && kernel.scheduler().find(g.pid)->state() == sched::ProcessState::zombie);
    for (const auto &t : kernel.scheduler().find(g.pid)->threads()) expect(!t.native_active && !t.entry_stack.pool);
    retire(kernel, g); retire(kernel, sender);
    // Self-kill publishes before the next step retires the current native image.
    const auto self = create(kernel);
    expect(call(kernel, self, self.peer, lu::Operation::kill, {self.pid, 9}) == 0);
    expect(thread(kernel, self.pid, self.peer).native_active);
    native::started = true; mock::clock_fail = true;
    expect(!native::step(kernel)); mock::clock_fail = false;
    expect(kernel.scheduler().find(self.pid)->state() == sched::ProcessState::zombie && mock::entries == entries);
    // Clock failure after teardown is unrelated to signal lifetime.
    native::runtime_failure = Status::success(); retire(kernel, self);
}
} // namespace

int main()
{
    static Kernel kernel; CHECK(kernel.boot({})); CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    queue_transactions<64>(); queue_transactions<128>(); routing(kernel); ignored_and_copyout(kernel);
    permissions_and_lifecycle(kernel); realtime_pressure(kernel); nonleader_exec(kernel); blocked_fatal(kernel);
    expect(native::entry_stacks.accounting().leased_slots == 0);
    std::printf("PROCESS_SIGNAL_PASS checks=%zu hosted=1 native=0\n", checks);
}
