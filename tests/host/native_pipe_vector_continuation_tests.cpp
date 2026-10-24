// Actual pipe vector dispatcher, owned objects and scheduler; hosted mappings only.
#define OK_ENABLE_TEST_POINTS 1
#include "native_clock_model.hpp"
#include <source_location>
namespace
{
using Vector = native::PipeVector;
constexpr uptr array_address = hw::user_begin + 512;
constexpr uptr first = hw::user_begin + 2048, second = hw::user_begin + 4096;
usize checks{};
void expect(bool value, std::source_location at = std::source_location::current())
{
    ++checks;
    if (!value) { std::fprintf(stderr, "PIPE_VECTOR_FAIL line=%u checks=%zu\n", at.line(), checks); std::abort(); }
}
struct Group { sched::ProcessId pid; sched::ThreadId peer; };
Group create(Kernel &kernel)
{
    auto made = kernel.scheduler().create_user_process("pipe-vectors",
        kernel.arch().make_user_context({hw::user_begin, hw::user_begin + 8192})); CHECK(made);
    auto *process = kernel.scheduler().find(made.value()); process->resources().personality = sched::UserPersonality::linux;
    auto peer = kernel.scheduler().create_thread(process->pid(), process->threads()[0].context); CHECK(peer);
    for (auto &thread : process->threads()) { CHECK(native::prepare_native_thread(thread)); thread.native_active = true; }
    CHECK(kernel.scheduler().set_runnable(process->pid())); CHECK(kernel.scheduler().activate_thread(process->pid(), process->pid()));
    CHECK(kernel.posix().activate(process->pid())); return {process->pid(), peer.value()};
}
void leader(Kernel &kernel, Group group)
{ CHECK(kernel.scheduler().activate_thread(group.pid, group.pid)); CHECK(kernel.posix().activate(group.pid)); }
void retire(Kernel &kernel, Group group)
{
    mock::forbidden = mock::write_forbidden = 0;
    native::release_process(kernel, group.pid); CHECK(kernel.scheduler().exit_process(group.pid, 0)); CHECK(kernel.scheduler().reap_orphans());
    expect(native::pipe_read_objects.accounting().live_objects == 0 && native::pipe_read_objects.accounting().backing_pages == 0);
    expect(native::pipe_write_objects.accounting().live_objects == 0 && native::pipe_write_objects.accounting().backing_pages == 0);
    expect(native::runtime_status().ok());
}
void vectors(std::span<const Vector> values)
{ std::memcpy(mock::memory.data() + array_address - hw::user_begin, values.data(), values.size_bytes()); }
void ordinary_vectors()
{
    const std::array<Vector, 4> values{{{first, 2}, {hw::user_end - 1, 0}, {second, 3}, {first + 16, 1}}}; vectors(values);
}
i64 call(Kernel &kernel, Group group, bool writing, i32 fd, usize count, bool &complete, uptr array = array_address)
{
    return native::dispatch(kernel, group.pid, {writing ? OK_SYS_WRITEV : OK_SYS_READV,
        {static_cast<uptr>(fd), array, count, 0, 0, 0}}, complete);
}
auto object(Group group, bool writing)
{ return writing ? native::pipe_write_object(group.pid, group.peer) : native::pipe_read_object(group.pid, group.peer); }
sched::WaitTicket arm(Kernel &kernel, Group group, bool writing, i32 fd, usize count = 4)
{
    CHECK(kernel.scheduler().activate_thread(group.pid, group.peer)); auto *thread = kernel.scheduler().current_thread();
    thread->syscall_pending = true; bool complete = true;
    expect(call(kernel, group, writing, fd, count, complete) == -11 && !complete);
    expect(object(group, writing).pool != nullptr);
    const auto ticket = thread->wait_registration.ticket(); expect(thread->wait_registration.pending(ticket)); return ticket;
}
i64 resume(Kernel &kernel, Group group, bool expected_complete = true)
{
    CHECK(kernel.scheduler().activate_thread(group.pid, group.peer)); bool complete = true;
    // The owned operation wins over an unrelated call, invalid fd and pointer.
    const auto result = call(kernel, group, false, -1, 1000, complete, hw::user_end - 1);
    expect(complete == expected_complete);
    if (complete) kernel.scheduler().current_thread()->syscall_pending = false;
    return result;
}
u16 description(Kernel &kernel, Group group, i32 fd)
{ return kernel.scheduler().find(group.pid)->fd_table().entry(fd)->description; }
void fill(Kernel &kernel, i32 fd, usize count = sched::pipe_capacity, std::byte value = std::byte{'F'})
{
    std::array<std::byte, sched::pipe_capacity> bytes{}; bytes.fill(value);
    auto wrote = kernel.posix().write(fd, {bytes.data(), count}); CHECK(wrote && wrote.value() == count);
}
std::array<std::byte, sched::pipe_capacity> take(Kernel &kernel, i32 fd, usize count)
{
    std::array<std::byte, sched::pipe_capacity> bytes{}; auto read = kernel.posix().read(fd, {bytes.data(), count});
    CHECK(read && read.value() == count); return bytes;
}
void payload()
{
    mock::memory[first-hw::user_begin] = std::byte{'A'}; mock::memory[first-hw::user_begin+1] = std::byte{'B'};
    mock::memory[second-hw::user_begin] = std::byte{'C'}; mock::memory[second-hw::user_begin+1] = std::byte{'D'};
    mock::memory[second-hw::user_begin+2] = std::byte{'E'}; mock::memory[first-hw::user_begin+16] = std::byte{'F'};
}
void immediate(Kernel &kernel)
{
    auto group = create(kernel); auto fds = kernel.posix().pipe(0); CHECK(fds); ordinary_vectors(); payload(); bool complete = true;
    expect(call(kernel, group, true, fds.value()[1], 4, complete) == 6 && complete);
    mock::memory[first-hw::user_begin] = mock::memory[second-hw::user_begin] = std::byte{0};
    expect(call(kernel, group, false, fds.value()[0], 4, complete) == 6 && complete);
    expect(mock::memory[first-hw::user_begin] == std::byte{'A'} && mock::memory[second-hw::user_begin] == std::byte{'C'});
    // Short reads scatter only the returned prefix, leaving later spans intact.
    fill(kernel, fds.value()[1], 3, std::byte{'S'}); mock::memory[second-hw::user_begin+1] = std::byte{'Z'};
    expect(call(kernel, group, false, fds.value()[0], 4, complete) == 3);
    expect(mock::memory[second-hw::user_begin] == std::byte{'S'} && mock::memory[second-hw::user_begin+1] == std::byte{'Z'});
    expect(!native::pipe_read_object(group.pid, group.pid).pool && !native::pipe_write_object(group.pid, group.pid).pool); retire(kernel, group);
}
void owned(Kernel &kernel, bool writing, unsigned mode)
{
    auto group = create(kernel); auto a = kernel.posix().pipe(0), b = kernel.posix().pipe(0); CHECK(a && b);
    ordinary_vectors(); payload(); if (writing) fill(kernel, a.value()[1]);
    const auto fd = a.value()[writing ? 1 : 0]; const auto id = description(kernel, group, fd);
    const auto ticket = arm(kernel, group, writing, fd); const auto handle = object(group, writing);
    expect(kernel.scheduler().open_description(id)->references == 2);
    // Replace the entire user array with invalid metadata. Only payload mappings
    // from the admitted object remain relevant until completion.
    const std::array<Vector, 4> changed{{{hw::user_end-1, 500}, {0, 50}, {0, 99}, {0, 0}}}; vectors(changed);
    mock::forbidden = array_address;
    leader(kernel, group);
    if (mode == 1) CHECK(kernel.posix().duplicate_to(b.value()[writing ? 1 : 0], fd));
    if (mode == 2) CHECK(kernel.scheduler().complete_wait(ticket, sched::WaitCompletion::ready));
    if (mode == 2)
    {
        expect(resume(kernel, group, false) == -11);
        const auto next = kernel.scheduler().find_thread(group.pid, group.peer)->wait_registration.ticket();
        expect(next.generation > ticket.generation && object(group, writing) == handle);
        expect(!kernel.scheduler().complete_wait(ticket, sched::WaitCompletion::ready)); leader(kernel, group);
    }
    mock::memory[first-hw::user_begin] = std::byte{'X'};
    if (writing) static_cast<void>(take(kernel, a.value()[0], 6)); else fill(kernel, a.value()[1], 6, std::byte{'R'});
    expect(resume(kernel, group) == 6); expect(!object(group, writing).pool);
    leader(kernel, group);
    if (writing)
    {
        static_cast<void>(take(kernel, a.value()[0], sched::pipe_capacity-6)); auto bytes = take(kernel, a.value()[0], 6);
        expect(bytes[0] == std::byte{'X'} && bytes[1] == std::byte{'B'} && bytes[2] == std::byte{'C'} && bytes[5] == std::byte{'F'});
        auto readiness = kernel.scheduler().pipe_readiness(kernel.scheduler().open_description(description(kernel, group, b.value()[1]))->pipe);
        expect(readiness && readiness.value().buffered == 0);
    }
    else expect(mock::memory[first-hw::user_begin] == std::byte{'R'} && mock::memory[second-hw::user_begin+2] == std::byte{'R'});
    expect(mode == 1 ? !kernel.scheduler().open_description(id) : kernel.scheduler().open_description(id)->references == 1);
    retire(kernel, group);
}
void mapping(Kernel &kernel, bool writing, bool permission)
{
    auto group = create(kernel); auto fds = kernel.posix().pipe(0); CHECK(fds); ordinary_vectors(); payload();
    if (writing) fill(kernel, fds.value()[1]);
    arm(kernel, group, writing, fds.value()[writing ? 1 : 0]); leader(kernel, group);
    if (writing) static_cast<void>(take(kernel, fds.value()[0], 6)); else fill(kernel, fds.value()[1], 6);
    if (permission) mock::write_forbidden = second; else mock::forbidden = second;
    const bool fault = !writing || !permission;
    expect(resume(kernel, group) == (fault ? -14 : 6)); mock::write_forbidden = mock::forbidden = 0;
    leader(kernel, group); const auto desc = kernel.scheduler().open_description(description(kernel, group, fds.value()[0]));
    auto readiness = kernel.scheduler().pipe_readiness(desc->pipe); CHECK(readiness);
    expect(readiness.value().buffered == (writing ? sched::pipe_capacity - (fault ? 6 : 0) : 6)); retire(kernel, group);
}
void boundary(Kernel &kernel, usize total, bool nonblock)
{
    auto group = create(kernel); auto fds = kernel.posix().pipe(0); CHECK(fds);
    const std::array<Vector, 2> values{{{first, 2048}, {second, total-2048}}}; vectors(values);
    for (usize i = 2048; i < 6146; ++i) mock::memory[i] = std::byte{'V'};
    fill(kernel, fds.value()[1], sched::pipe_capacity-2);
    if (nonblock) CHECK(kernel.posix().fcntl(fds.value()[1], posix::f_SETFL, posix::o_NONBLOCK));
    if (total > sched::pipe_capacity && !nonblock)
    {
        arm(kernel, group, true, fds.value()[1], 2); leader(kernel, group);
        static_cast<void>(take(kernel, fds.value()[0], sched::pipe_capacity-2));
        expect(resume(kernel, group, false) == -11); leader(kernel, group);
        auto bytes = take(kernel, fds.value()[0], sched::pipe_capacity);
        for (auto byte : bytes) expect(byte == std::byte{'V'});
        expect(resume(kernel, group) == static_cast<i64>(total)); leader(kernel, group);
        bytes = take(kernel, fds.value()[0], total-sched::pipe_capacity);
        expect(bytes[0] == std::byte{'V'});
    }
    else if (total > sched::pipe_capacity)
    {
        bool complete = true; expect(call(kernel, group, true, fds.value()[1], 2, complete) == 2 && complete);
        static_cast<void>(take(kernel, fds.value()[0], sched::pipe_capacity-2)); auto bytes = take(kernel, fds.value()[0], 2);
        expect(bytes[0] == std::byte{'V'} && bytes[1] == std::byte{'V'});
    }
    else if (nonblock)
    {
        bool complete = true; expect(call(kernel, group, true, fds.value()[1], 2, complete) == -11 && complete);
        expect(native::pipe_write_objects.accounting().live_objects == 0);
    }
    else
    {
        arm(kernel, group, true, fds.value()[1], 2); leader(kernel, group); static_cast<void>(take(kernel, fds.value()[0], sched::pipe_capacity-2));
        expect(resume(kernel, group) == static_cast<i64>(total)); leader(kernel, group); static_cast<void>(take(kernel, fds.value()[0], total));
    }
    retire(kernel, group);
}
void controls(Kernel &kernel, bool writing)
{
    auto group = create(kernel); auto fds = kernel.posix().pipe(0); CHECK(fds); ordinary_vectors(); bool complete = true;
    auto fd = fds.value()[writing ? 1 : 0];
    expect(call(kernel, group, writing, fd, 17, complete) == -22 && complete);
    expect(call(kernel, group, writing, fd, 4, complete, hw::user_end-1) == -14);
    expect(call(kernel, group, writing, -1, 4, complete) == -9);
    expect(call(kernel, group, writing, fd, 0, complete) == 0);
    const std::array<Vector, 1> empty{{{hw::user_end-1, 0}}}; vectors(empty);
    expect(call(kernel, group, writing, fd, 1, complete) == 0);
    const std::array<Vector, 1> overflow{{{first, static_cast<usize>(__INTPTR_MAX__) + 1}}}; vectors(overflow);
    expect(call(kernel, group, writing, fd, 1, complete) == -22);
    const std::array<Vector, 2> sum_overflow{{{first, static_cast<usize>(__INTPTR_MAX__)}, {second, 1}}}; vectors(sum_overflow);
    expect(call(kernel, group, writing, fd, 2, complete) == -22);
    const std::array<Vector, 1> bad{{{hw::user_end-1, 2}}}; vectors(bad);
    expect(call(kernel, group, writing, fd, 1, complete) == -14);
    std::array<Vector, 16> maximum{}; for (usize i = 0; i < maximum.size(); ++i) maximum[i] = {first+i, 1}; vectors(maximum);
    if (writing) fill(kernel, fds.value()[1]);
    CHECK(kernel.posix().fcntl(fd, posix::f_SETFL, posix::o_NONBLOCK));
    expect(call(kernel, group, writing, fd, 16, complete) == -11 && complete);
    CHECK(kernel.posix().fcntl(fd, posix::f_SETFL, 0));
    arm(kernel, group, writing, fd, 16); leader(kernel, group); CHECK(kernel.posix().close(fds.value()[writing ? 0 : 1]));
    expect(resume(kernel, group) == (writing ? -32 : 0)); retire(kernel, group);
}
void teardown(Kernel &kernel, bool writing, unsigned mode)
{
    auto group = create(kernel); auto fds = kernel.posix().pipe(0); CHECK(fds); ordinary_vectors();
    if (writing) fill(kernel, fds.value()[1]);
    const auto fd = fds.value()[writing ? 1 : 0];
    const auto id = description(kernel, group, fd); const auto ticket = arm(kernel, group, writing, fd);
    if (mode == 0) CHECK(kernel.scheduler().complete_wait(ticket, sched::WaitCompletion::cancelled));
    leader(kernel, group); CHECK(kernel.posix().close(fd)); expect(kernel.scheduler().open_description(id)->references == 1);
    if (mode == 0) expect(resume(kernel, group) == -125);
    else if (mode == 1) native::release_process(kernel, group.pid);
    else if (mode == 2)
    {
        CHECK(native::retire_native_thread(*kernel.scheduler().find_thread(group.pid, group.peer)));
        CHECK(kernel.scheduler().exit_thread(group.pid, group.peer)); native::prune_pipe_reads(kernel); native::prune_pipe_writes(kernel);
    }
    else
    {
        auto *p = kernel.scheduler().find(group.pid); native::ExecRetirement retirement{*p, group.pid};
        auto replaced = native::replace_entry_stacks(native::entry_stacks, *p, [&] {
            return kernel.posix().exec(group.pid, p->threads()[0].context, p->resources().memory);
        }); CHECK(replaced && replaced.committed); retirement.consume(kernel, *p);
    }
    expect(!kernel.scheduler().open_description(id) && !object(group, writing).pool); retire(kernel, group);
}
} // namespace
int main()
{
    static Kernel kernel; CHECK(kernel.boot({})); CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    for (unsigned round = 0; round < 3; ++round)
    {
        immediate(kernel);
        for (bool writing : {false, true})
        {
            for (unsigned mode = 0; mode < 3; ++mode) owned(kernel, writing, mode);
            mapping(kernel, writing, false); mapping(kernel, writing, true); controls(kernel, writing);
            for (unsigned mode = 0; mode < 4; ++mode) teardown(kernel, writing, mode);
        }
        for (usize count : {usize{4096}, usize{4097}, usize{4098}})
            for (bool nonblock : {false, true}) boundary(kernel, count, nonblock);
    }
    expect(native::pipe_read_objects.accounting().allocations == native::pipe_read_objects.accounting().destructions);
    expect(native::pipe_write_objects.accounting().allocations == native::pipe_write_objects.accounting().destructions);
    std::printf("PIPE_VECTOR_CONTINUATION_PASS retained_vectors=1 hosted=1 guest=0 checks=%zu\n", checks);
}
