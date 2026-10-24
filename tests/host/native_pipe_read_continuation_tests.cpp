// Actual native scalar marshalling, ownership and scheduler; hosted mappings only.
#define OK_ENABLE_TEST_POINTS 1
#include "native_clock_model.hpp"
namespace
{
using Ticket = sched::WaitTicket;
constexpr uptr output = hw::user_begin + 128;
usize checks{};
void expect(bool value) { ++checks; CHECK(value); }
struct Group { sched::ProcessId pid; sched::ThreadId peer; };
Group create(Kernel &kernel)
{
    const auto made = kernel.scheduler().create_user_process("pipe-identity",
        kernel.arch().make_user_context({hw::user_begin, hw::user_begin + 8192})); CHECK(made);
    const auto pid = made.value();
    auto *process = kernel.scheduler().find(pid);
    process->resources().personality = sched::UserPersonality::linux;
    const auto peer = kernel.scheduler().create_thread(pid, process->threads()[0].context); CHECK(peer);
    for (auto &thread : process->threads())
    {
        CHECK(native::prepare_native_thread(thread));
        thread.native_active = true;
    }
    CHECK(kernel.scheduler().set_runnable(pid)); CHECK(kernel.scheduler().activate_thread(pid, pid));
    CHECK(kernel.posix().activate(pid));
    return {pid, peer.value()};
}
void leader(Kernel &kernel, Group g)
{ CHECK(kernel.scheduler().activate_thread(g.pid, g.pid)); CHECK(kernel.posix().activate(g.pid)); }
void retire(Kernel &kernel, Group g)
{
    mock::write_forbidden = mock::forbidden = 0;
    native::release_process(kernel, g.pid);
    CHECK(kernel.scheduler().exit_process(g.pid, 0)); CHECK(kernel.scheduler().reap_orphans());
    expect(native::pipe_read_objects.accounting().live_objects == 0);
    expect(native::pipe_read_objects.accounting().backing_pages == 0);
    expect(native::runtime_status().ok());
}
hw::Syscall read_call(i32 fd, uptr destination = output, usize count = 1)
{ return {OK_SYS_READ, {static_cast<uptr>(fd), destination, count, 0, 0, 0}}; }
Ticket arm(Kernel &kernel, Group g, i32 fd, uptr destination = output, usize count = 1)
{
    CHECK(kernel.scheduler().activate_thread(g.pid, g.peer));
    auto &thread = *kernel.scheduler().current_thread(); thread.syscall_pending = true;
    bool complete = true;
    expect(native::dispatch(kernel, g.pid, read_call(fd, destination, count), complete) == -11 && !complete);
    const auto ticket = thread.wait_registration.ticket();
    expect(thread.wait_registration.pending(ticket) && thread.wait_reason == sched::ThreadWaitReason::pipe_read);
    expect(native::pipe_read_object(g.pid, g.peer).pool != nullptr);
    return ticket;
}
i64 resume(Kernel &kernel, Group g, bool expected_complete = true)
{
    CHECK(kernel.scheduler().activate_thread(g.pid, g.peer));
    bool complete = true;
    // Even a different decoded request cannot redirect an owned continuation.
    const auto result = native::dispatch(kernel, g.pid, read_call(-1, hw::user_end - 1, 7000), complete);
    expect(complete == expected_complete);
    if (complete) kernel.scheduler().find_thread(g.pid, g.peer)->syscall_pending = false;
    return result;
}
u16 description(Kernel &kernel, Group g, i32 fd)
{ return kernel.scheduler().find(g.pid)->fd_table().entry(fd)->description; }
void put(Kernel &kernel, i32 fd, char value)
{
    const std::array<std::byte, 1> bytes{static_cast<std::byte>(value)};
    const auto wrote = kernel.posix().write(fd, bytes); CHECK(wrote && wrote.value() == 1);
}
void identity(Kernel &kernel, bool duplicate, bool close_first)
{
    auto g = create(kernel); auto a = kernel.posix().pipe(0), b = kernel.posix().pipe(0); CHECK(a && b);
    if (duplicate) CHECK(kernel.posix().duplicate(a.value()[0]));
    const auto id = description(kernel, g, a.value()[0]);
    const auto refs = kernel.scheduler().open_description(id)->references;
    const auto ticket = arm(kernel, g, a.value()[0], output, 3);
    expect(kernel.scheduler().open_description(id)->references == refs + 1);
    leader(kernel, g);
    if (close_first) CHECK(kernel.posix().close(a.value()[0]));
    CHECK(kernel.posix().duplicate_to(b.value()[0], a.value()[0]));
    // Dropping a description emits a spurious wake. B data cannot satisfy A.
    put(kernel, b.value()[1], 'B');
    expect(resume(kernel, g, false) == -11);
    auto *thread = kernel.scheduler().find_thread(g.pid, g.peer);
    const auto rearmed = thread->wait_registration.ticket();
    expect(rearmed.generation > ticket.generation && thread->wait_registration.pending(rearmed));
    expect(!kernel.scheduler().complete_wait(ticket, sched::WaitCompletion::ready));
    leader(kernel, g); put(kernel, a.value()[1], 'A');
    mock::memory[129] = std::byte{0x55};
    expect(resume(kernel, g) == 1); expect(mock::memory[128] == std::byte{'A'} && mock::memory[129] == std::byte{0x55});
    expect(!native::pipe_read_object(g.pid, g.peer).pool);
    if (duplicate) expect(kernel.scheduler().open_description(id)->references == 1);
    else expect(kernel.scheduler().open_description(id) == nullptr);
    leader(kernel, g); std::array<std::byte, 1> left{};
    auto remaining = kernel.posix().read(b.value()[0], left); expect(remaining && remaining.value() == 1 && left[0] == std::byte{'B'});
    retire(kernel, g);
}
void eof_and_fault(Kernel &kernel, unsigned mode)
{
    auto g = create(kernel); auto a = kernel.posix().pipe(0), b = kernel.posix().pipe(0); CHECK(a && b);
    const auto id = description(kernel, g, a.value()[0]);
    arm(kernel, g, a.value()[0]); leader(kernel, g);
    if (!mode)
    {
        CHECK(kernel.posix().close(a.value()[0])); CHECK(kernel.posix().duplicate_to(b.value()[0], a.value()[0]));
        CHECK(kernel.posix().close(a.value()[1])); expect(resume(kernel, g) == 0);
        expect(kernel.scheduler().open_description(id) == nullptr);
    }
    else
    {
        put(kernel, a.value()[1], 'F');
        if (mode == 1) mock::write_forbidden = output;
        else mock::forbidden = output;
        expect(resume(kernel, g) == -14); mock::write_forbidden = mock::forbidden = 0;
        leader(kernel, g); std::array<std::byte, 1> left{};
        const auto read = kernel.posix().read(a.value()[0], left);
        expect(read && read.value() == 1 && left[0] == std::byte{'F'});
        expect(kernel.scheduler().open_description(id)->references == 1);
    }
    retire(kernel, g);
}
void multiple(Kernel &kernel)
{
    auto g = create(kernel); auto a = kernel.posix().pipe(0); CHECK(a);
    auto second = kernel.scheduler().create_thread(g.pid, kernel.scheduler().find(g.pid)->threads()[0].context); CHECK(second);
    auto *thread = kernel.scheduler().find_thread(g.pid, second.value()); CHECK(native::prepare_native_thread(*thread)); thread->native_active = true;
    const auto id = description(kernel, g, a.value()[0]);
    arm(kernel, g, a.value()[0]); Group other{g.pid, second.value()}; const auto ticket = arm(kernel, other, a.value()[0], output + 1);
    expect(kernel.scheduler().open_description(id)->references == 3);
    leader(kernel, g); put(kernel, a.value()[1], '1');
    expect(resume(kernel, g) == 1); expect(resume(kernel, other, false) == -11);
    expect(kernel.scheduler().find_thread(g.pid, other.peer)->wait_registration.ticket().generation > ticket.generation);
    leader(kernel, g); put(kernel, a.value()[1], '2'); expect(resume(kernel, other) == 1);
    expect(mock::memory[128] == std::byte{'1'} && mock::memory[129] == std::byte{'2'});
    expect(kernel.scheduler().open_description(id)->references == 1); retire(kernel, g);
}
void controls(Kernel &kernel)
{
    auto g = create(kernel); auto a = kernel.posix().pipe(0); CHECK(a);
    const auto id = description(kernel, g, a.value()[0]);
    const auto before = native::pipe_read_objects.accounting(); bool complete = true;
    expect(native::dispatch(kernel, g.pid, read_call(a.value()[0], hw::user_end - 1, 0), complete) == 0 && complete);
    CHECK(kernel.posix().fcntl(a.value()[0], posix::f_SETFL, posix::o_NONBLOCK));
    expect(native::dispatch(kernel, g.pid, read_call(a.value()[0]), complete) == -11 && complete);
    expect(native::pipe_read_objects.accounting().allocations == before.allocations);
    CHECK(kernel.posix().fcntl(a.value()[0], posix::f_SETFL, 0));
    const auto ticket = arm(kernel, g, a.value()[0]); leader(kernel, g);
    CHECK(kernel.posix().fcntl(a.value()[0], posix::f_SETFL, posix::o_NONBLOCK));
    CHECK(kernel.scheduler().complete_wait(ticket, sched::WaitCompletion::ready));
    expect(resume(kernel, g) == -11); expect(kernel.scheduler().open_description(id)->references == 1);
    CHECK(kernel.posix().fcntl(a.value()[0], posix::f_SETFL, 0));
    const auto cancelled = arm(kernel, g, a.value()[0]);
    CHECK(kernel.scheduler().complete_wait(cancelled, sched::WaitCompletion::cancelled));
    expect(resume(kernel, g) == -125); expect(kernel.scheduler().open_description(id)->references == 1);
    retire(kernel, g);
}
void stale(Kernel &kernel)
{
    auto g = create(kernel); auto a = kernel.posix().pipe(0); CHECK(a);
    const auto id = description(kernel, g, a.value()[0]);
    const auto ticket = arm(kernel, g, a.value()[0]); const auto object = native::pipe_read_object(g.pid, g.peer);
    CHECK(kernel.scheduler().complete_wait(ticket, sched::WaitCompletion::cancelled));
    CHECK(kernel.scheduler().activate_thread(g.pid, g.peer));
    const auto replacement = kernel.scheduler().register_current_wait(sched::ThreadWaitReason::deadline, 123); CHECK(replacement);
    auto *thread = kernel.scheduler().find_thread(g.pid, g.peer); thread->native_frame.pc = 12345;
    native::prune_pipe_reads(kernel);
    expect(thread->wait_registration.pending(replacement.value()) && thread->wait_reason == sched::ThreadWaitReason::deadline);
    expect(thread->native_frame.pc == 12345 && thread->syscall_pending);
    expect(!native::pipe_read_objects.resolve(object)); expect(kernel.scheduler().open_description(id)->references == 1);
    expect(!kernel.scheduler().complete_wait(ticket, sched::WaitCompletion::ready));
    native::prune_pipe_reads(kernel); expect(thread->wait_registration.pending(replacement.value())); retire(kernel, g);
}
void lifecycle(Kernel &kernel)
{
    // Direct owner retirement and whole-group finish repeatedly release the last
    // read endpoint pin, even with the numeric descriptor already gone.
    for (unsigned mode = 0; mode < 4; ++mode)
    {
        auto g = create(kernel); auto a = kernel.posix().pipe(0); CHECK(a);
        const auto id = description(kernel, g, a.value()[0]); arm(kernel, g, a.value()[0]);
        leader(kernel, g); CHECK(kernel.posix().close(a.value()[0]));
        expect(kernel.scheduler().open_description(id)->references == 1);
        if (mode == 0) native::release_process(kernel, g.pid);
        else if (mode == 1) native::finish(kernel, g.pid, 0);
        else if (mode == 2)
        {
            // Scheduler-only retirement is reclaimed by the actual step sweep.
            CHECK(native::retire_native_thread(*kernel.scheduler().find_thread(g.pid, g.peer)));
            CHECK(kernel.scheduler().exit_thread(g.pid, g.peer)); native::prune_pipe_reads(kernel);
        }
        else
        {
            auto *p = kernel.scheduler().find(g.pid);
            native::ExecRetirement retirement{*p, g.pid};
            const auto replaced = native::replace_entry_stacks(native::entry_stacks, *p, [&] {
                return kernel.posix().exec(g.pid, p->threads()[0].context, p->resources().memory);
            });
            CHECK(replaced && replaced.committed); retirement.consume(kernel, *p);
        }
        expect(kernel.scheduler().open_description(id) == nullptr);
        expect(native::pipe_read_objects.accounting().live_objects == 0);
        if (mode == 1) CHECK(kernel.scheduler().reap_orphans());
        else retire(kernel, g);
    }
}
void failed_exec(Kernel &kernel)
{
    auto g = create(kernel); auto a = kernel.posix().pipe(0); CHECK(a);
    const auto ticket = arm(kernel, g, a.value()[0]); const auto object = native::pipe_read_object(g.pid, g.peer);
    leader(kernel, g); std::strcpy(reinterpret_cast<char *>(mock::memory.data() + 512), "/no-such-pipe-test-image");
    bool complete = true;
    expect(native::dispatch(kernel, g.pid, {OK_SYS_EXECVE, {hw::user_begin + 512, 0, 0, 0, 0, 0}}, complete) < 0 && complete);
    expect(native::pipe_read_object(g.pid, g.peer) == object);
    expect(kernel.scheduler().find_thread(g.pid, g.peer)->wait_registration.pending(ticket));
    put(kernel, a.value()[1], 'E'); expect(resume(kernel, g) == 1); retire(kernel, g);
}
void compaction_and_step(Kernel &kernel)
{
    auto first = create(kernel), g = create(kernel); auto a = kernel.posix().pipe(0); CHECK(a);
    const auto ticket = arm(kernel, g, a.value()[0]);
    native::release_process(kernel, first.pid); CHECK(kernel.scheduler().exit_process(first.pid, 0)); CHECK(kernel.scheduler().reap_orphans());
    auto *thread = kernel.scheduler().find_thread(g.pid, g.peer);
    expect(thread && thread->wait_registration.pending(ticket));
    leader(kernel, g); put(kernel, a.value()[1], 'S');
    // Real step must bypass even a changed Linux number before ABI completion.
    thread->native_frame = {}; kernel.scheduler().find_thread(g.pid, g.pid)->native_active = false;
    const auto entries = mock::entries; native::started = true; expect(native::step(kernel));
    thread = kernel.scheduler().find_thread(g.pid, g.peer);
    const auto result = lu::NativeBridge<lu::configured_native_profile()>::decode_return(thread->native_frame);
    expect(result && result.value().value == 1 && result.value().error == 0);
    expect(!thread->syscall_pending && mock::entries == entries && mock::memory[128] == std::byte{'S'});
    expect(native::pipe_read_objects.accounting().live_objects == 0); retire(kernel, g);
}
void admission_failure(Kernel &kernel)
{
    auto g = create(kernel); auto a = kernel.posix().pipe(0); CHECK(a);
    const auto id = description(kernel, g, a.value()[0]);
    auto *thread = kernel.scheduler().current_thread();
    // Inject a conflicting authoritative ticket without changing running state.
    const auto ticket = thread->wait_registration.arm(g.pid, g.pid); CHECK(ticket);
    bool complete = true; expect(native::dispatch(kernel, g.pid, read_call(a.value()[0]), complete) == -16 && complete);
    expect(thread->wait_registration.pending(ticket.value())); expect(kernel.scheduler().open_description(id)->references == 1);
    expect(native::pipe_read_objects.accounting().live_objects == 0); thread->wait_registration.cancel(); retire(kernel, g);
}
void capacity(Kernel &kernel)
{
    constexpr usize groups = native::pipe_read_capacity / sched::max_threads_per_process;
    static_assert(groups * sched::max_threads_per_process == native::pipe_read_capacity);
    std::array<Group, groups> owners{};
    std::array<u16, groups> descriptions{};
    for (usize i = 0; i < groups; ++i)
    {
        const auto g = create(kernel); owners[i] = g;
        auto *p = kernel.scheduler().find(g.pid);
        while (p->threads().size() < sched::max_threads_per_process)
        {
            const auto made = kernel.scheduler().create_thread(g.pid, p->threads()[0].context); CHECK(made);
            auto *thread = kernel.scheduler().find_thread(g.pid, made.value());
            CHECK(native::prepare_native_thread(*thread)); thread->native_active = true;
        }
        const auto pipe = kernel.posix().pipe(0); CHECK(pipe);
        descriptions[i] = description(kernel, g, pipe.value()[0]);
        for (const auto &thread : p->threads()) arm(kernel, {g.pid, thread.tid}, pipe.value()[0]);
        expect(kernel.scheduler().open_description(descriptions[i])->references == sched::max_threads_per_process + 1);
    }
    expect(native::pipe_read_objects.accounting().live_objects == native::pipe_read_capacity);
    expect(native::pipe_read_objects.accounting().backing_pages == native::pipe_read_object_pages);
    // Quota admission is tested by a hosted control TCB without a native lease:
    // every real native lease is already occupied by the 128 blocked readers.
    const auto made = kernel.scheduler().create_user_process("pipe-quota-control",
        kernel.arch().make_user_context({hw::user_begin, hw::user_begin + 8192})); CHECK(made);
    const auto pid = made.value(); CHECK(kernel.scheduler().set_runnable(pid));
    CHECK(kernel.scheduler().activate_thread(pid, pid)); CHECK(kernel.posix().activate(pid));
    const auto id = descriptions[0];
    CHECK(kernel.scheduler().install_descriptor(kernel.scheduler().find(pid)->resources(), 3, id));
    const auto references = kernel.scheduler().open_description(id)->references;
    bool complete = true;
    expect(native::dispatch(kernel, pid, read_call(3), complete) == -12 && complete);
    expect(kernel.scheduler().open_description(id)->references == references);
    expect(!kernel.scheduler().find_thread(pid, pid)->wait_registration.active());
    CHECK(kernel.scheduler().exit_process(pid, 0)); CHECK(kernel.scheduler().reap_orphans());
    for (usize i = 0; i < groups; ++i)
    {
        native::release_process(kernel, owners[i].pid);
        expect(kernel.scheduler().open_description(descriptions[i])->references == 1);
        CHECK(kernel.scheduler().exit_process(owners[i].pid, 0)); CHECK(kernel.scheduler().reap_orphans());
        expect(native::pipe_read_objects.accounting().live_objects == (groups - i - 1) * sched::max_threads_per_process);
    }
    expect(native::pipe_read_objects.accounting().backing_pages == 0);
    expect(native::entry_stacks.accounting().leased_slots == 0);
}
} // namespace
int main()
{
    static Kernel kernel; CHECK(kernel.boot({})); CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    for (unsigned round = 0; round < 4; ++round)
    {
        identity(kernel, false, false); identity(kernel, false, true); identity(kernel, true, false);
        for (unsigned mode = 0; mode < 3; ++mode) eof_and_fault(kernel, mode);
        multiple(kernel); controls(kernel); stale(kernel); lifecycle(kernel);
        failed_exec(kernel); compaction_and_step(kernel); admission_failure(kernel);
    }
    capacity(kernel);
    const auto stats = native::pipe_read_objects.accounting();
    expect(stats.allocations == stats.destructions && stats.live_objects == 0 && stats.backing_pages == 0);
    std::printf("PIPE_READ_CONTINUATION_PASS retained_fd=1 hosted=1 guest=0 checks=%zu allocations=%llu\n",
        checks, static_cast<unsigned long long>(stats.allocations));
}
