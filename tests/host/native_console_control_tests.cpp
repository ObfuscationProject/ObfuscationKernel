// Canonical scheduler and native/Linux dispatch with hosted checked-copy maps.
#include "native_clock_model.hpp"

namespace
{
using N = syscall::Number;
using Pid = sched::ProcessId;
constexpr uptr output = hw::user_begin + 128, vectors = hw::user_begin + 512;
usize checks{}, profile_calls{};

void activate(Kernel &kernel, Pid pid)
{
    auto *process = kernel.scheduler().find(pid); CHECK(process);
    if (process->state() == sched::ProcessState::created) CHECK(kernel.scheduler().set_runnable(pid));
    process->threads()[0].native_active = true;
    CHECK(kernel.posix().activate(pid));
}

std::pair<i64, bool> invoke(Kernel &kernel, Pid pid, N number, std::array<uptr, 6> args = {})
{
    ++checks;
    activate(kernel, pid);
    auto *thread = kernel.scheduler().current_thread();
    bool done = true;
    const auto result = native::dispatch(kernel, pid, {static_cast<uptr>(number), args}, done);
    if (done) thread->console_wait_epoch = 0;
    return {result, done};
}

i64 call(Kernel &kernel, Pid pid, N number, std::array<uptr, 6> args = {})
{
    const auto [result, done] = invoke(kernel, pid, number, args); CHECK(done);
    return result;
}

i64 ioctl(Kernel &kernel, Pid pid, u64 command, uptr argument = 0, i32 fd = 0)
{
    return call(kernel, pid, N::ioctl, {static_cast<uptr>(fd), static_cast<uptr>(command), argument});
}

void store_pid(i32 pid)
{
    std::memcpy(mock::memory.data() + 128, &pid, sizeof(pid));
}

i32 loaded_pid()
{
    i32 result{}; std::memcpy(&result, mock::memory.data() + 128, sizeof(result)); return result;
}

Pid fork(Kernel &kernel, Pid parent)
{
    activate(kernel, parent);
    const auto child = kernel.scheduler().fork_process(parent); CHECK(child);
    return child.value();
}

template <lu::Profile Profile>
hw::UserFrame linux_ioctl_frame(u64 fd, u64 command, u64 argument)
{
    using Codec = lu::Codec<Profile>;
    hw::UserFrame frame{};
    frame.registers.fill(~uptr{0});
    // x86 trap-frame slots differ from the generic profile register indices.
    constexpr usize number = Profile == lu::Profile::i386 ? 7 :
                             Profile == lu::Profile::x86_64 ? 14 : Codec::profile.number_register;
    constexpr std::array<usize, 3> slots = Profile == lu::Profile::i386 ? std::array<usize, 3>{4, 6, 5} :
        Profile == lu::Profile::x86_64 ? std::array<usize, 3>{9, 10, 11} :
        std::array<usize, 3>{static_cast<usize>(Codec::profile.argument_registers[0]),
                             static_cast<usize>(Codec::profile.argument_registers[1]),
                             static_cast<usize>(Codec::profile.argument_registers[2])};
    frame.registers[number] = *Codec::syscall_number(lu::Operation::ioctl);
    frame.registers[slots[0]] = fd; frame.registers[slots[1]] = command; frame.registers[slots[2]] = argument;
    return frame;
}

template <lu::Profile Profile>
void linux_profile(Kernel &kernel, Pid login, Pid peer)
{
    using Ioctl = lu::ConsoleIoctlCodec<Profile>;
    activate(kernel, login);
    auto *process = kernel.scheduler().find(login);
    process->resources().personality = sched::UserPersonality::linux;
    auto dispatch = [&](u64 fd, u64 command, u64 argument) {
        auto frame = linux_ioctl_frame<Profile>(fd, command, argument);
        bool done = true;
        ++profile_calls;
        const auto result = user::linux_bootstrap::dispatch<Profile>(kernel, login, frame, done,
            [&](hw::Syscall, bool &) -> i64 { CHECK(false); return -5; });
        CHECK(done);
        CHECK(user::linux_bootstrap::complete<Profile>(frame, result));
        const auto decoded = lu::NativeBridge<Profile>::decode_return(frame); CHECK(decoded);
        CHECK(decoded.value().error == (result < 0 ? static_cast<u32>(-result) : 0));
        return result;
    };
    // A foreign requested PID must fail before either checked-copy callback.
    for (const auto operation : {lu::ConsoleIoctlOperation::get_pgrp, lu::ConsoleIoctlOperation::set_pgrp})
    {
        auto frame = linux_ioctl_frame<Profile>(0, *Ioctl::number(operation), hw::user_end);
        const auto copies = mock::copies, outputs = mock::outputs;
        bool done = true;
        ++profile_calls;
        CHECK(user::linux_bootstrap::dispatch<Profile>(kernel, peer, frame, done,
            [&](hw::Syscall, bool &) -> i64 { CHECK(false); return -5; }) == -22);
        CHECK(done && mock::copies == copies && mock::outputs == outputs);
    }
    constexpr u64 high = u64{0x12345678} << 32;
    CHECK(dispatch(high | 99, ~u64{0}, 0) == -9);
    CHECK(dispatch(high, ~u64{0}, 0) == -25);
    // Linux request truncation and scalar arg narrowing preserve the syscall ABI.
    CHECK(dispatch(high, high | *Ioctl::number(lu::ConsoleIoctlOperation::acquire), high) == 0);
    mock::memory.fill(std::byte{0xa5});
    CHECK(dispatch(0, *Ioctl::number(lu::ConsoleIoctlOperation::get_sid), output) == 0);
    std::array<u8, 4> expected{};
    CHECK(Ioctl::encode_pid(expected, static_cast<i32>(login)));
    for (usize index = 0; index < expected.size(); ++index)
        CHECK(mock::memory[128 + index] == static_cast<std::byte>(expected[index]));
    CHECK(mock::memory[127] == std::byte{0xa5} && mock::memory[132] == std::byte{0xa5});
    CHECK(Ioctl::encode_pid(expected, static_cast<i32>(peer)));
    std::memcpy(mock::memory.data() + 128, expected.data(), expected.size());
    const u64 setter = *Ioctl::number(lu::ConsoleIoctlOperation::set_pgrp);
    CHECK(dispatch(0, setter | high, output) == 0);
    if (setter & 0x80000000U) CHECK(dispatch(0, setter | 0xffffffff00000000ULL, output) == 0);
    CHECK(dispatch(0, *Ioctl::number(lu::ConsoleIoctlOperation::get_pgrp), hw::user_end) == -14);
    // A PID wire value is exactly four bytes, including on LP64 profiles.
    constexpr uptr endpoint = hw::user_begin + mock::memory.size() - 4;
    constexpr usize tail = mock::memory.size() - 4;
    mock::memory[tail - 1] = std::byte{0xa5};
    CHECK(dispatch(0, *Ioctl::number(lu::ConsoleIoctlOperation::get_pgrp), endpoint) == 0);
    CHECK(Ioctl::encode_pid(expected, static_cast<i32>(peer)));
    for (usize index = 0; index < expected.size(); ++index)
        CHECK(mock::memory[tail + index] == static_cast<std::byte>(expected[index]));
    CHECK(dispatch(0, *Ioctl::number(lu::ConsoleIoctlOperation::get_sid), endpoint) == 0);
    CHECK(Ioctl::encode_pid(expected, static_cast<i32>(login)));
    for (usize index = 0; index < expected.size(); ++index)
        CHECK(mock::memory[tail + index] == static_cast<std::byte>(expected[index]));
    CHECK(mock::memory[tail - 1] == std::byte{0xa5});
    const auto boundary_outputs = mock::outputs;
    const auto boundary_bytes = mock::memory;
    CHECK(dispatch(0, *Ioctl::number(lu::ConsoleIoctlOperation::get_sid), endpoint + 1) == -14);
    CHECK(mock::outputs == boundary_outputs && mock::memory == boundary_bytes);
    mock::write_forbidden = endpoint + 3;
    CHECK(dispatch(0, *Ioctl::number(lu::ConsoleIoctlOperation::get_pgrp), endpoint) == -14);
    CHECK(mock::outputs == boundary_outputs && mock::memory == boundary_bytes);
    mock::write_forbidden = 0;
    CHECK(Ioctl::encode_pid(expected, static_cast<i32>(peer)));
    std::memcpy(mock::memory.data() + tail, expected.data(), expected.size());
    CHECK(dispatch(0, setter, endpoint) == 0);
    CHECK(mock::last_input == endpoint && mock::last_input_size == 4);
    CHECK(dispatch(0, setter, endpoint + 1) == -14);
    CHECK(dispatch(0, *Ioctl::number(lu::ConsoleIoctlOperation::get_pgrp), output) == 0);
    for (usize index = 0; index < expected.size(); ++index)
        CHECK(mock::memory[128 + index] == static_cast<std::byte>(expected[index]));
    CHECK(Ioctl::encode_pid(expected, static_cast<i32>(login)));
    std::memcpy(mock::memory.data() + 128, expected.data(), expected.size());
    mock::forbidden = output + 3;
    CHECK(dispatch(0, setter, output) == -14);
    CHECK(mock::last_input == output && mock::last_input_size == 4);
    mock::forbidden = 0;
    CHECK(dispatch(0, *Ioctl::number(lu::ConsoleIoctlOperation::get_pgrp), output) == 0);
    CHECK(Ioctl::encode_pid(expected, static_cast<i32>(peer)));
    for (usize index = 0; index < expected.size(); ++index)
        CHECK(mock::memory[128 + index] == static_cast<std::byte>(expected[index]));
    CHECK(Ioctl::encode_pid(expected, static_cast<i32>(login)));
    std::memcpy(mock::memory.data() + 128, expected.data(), expected.size());
    CHECK(dispatch(0, setter, output) == 0);
    process->resources().personality = sched::UserPersonality::native;
}

void readiness_handoff(Kernel &kernel, Pid login, Pid peer)
{
    auto &scheduler = kernel.scheduler();
    CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    for (const bool select : {false, true})
    {
        store_pid(static_cast<i32>(login)); CHECK(ioctl(kernel, login, OK_TIOCSPGRP, output) == 0);
        activate(kernel, login);
        const auto tid = scheduler.current_thread()->tid;
        const auto description = scheduler.find(login)->fd_table().entry(0)->description;
        const auto references = scheduler.open_description(description)->references;
        const auto live_objects = native::poll_objects.accounting().live_objects;
        mock::memory.fill(std::byte{0xa5});
        bool done = true;
        const std::array<ok_pollfd, 1> fds{{{0, OK_POLLIN, 0}}};
        if (select)
        {
            native::SelectRequest request{};
            request.sets = {{output + 32, output + 48, output + 64}, {1, 0, 1}, 1, 8};
            CHECK(native::begin_select(kernel, login, request, done) == 0 && !done);
        }
        else
        {
            std::memcpy(mock::memory.data() + 160, fds.data(), sizeof(fds));
            native::PollRequest request{};
            request.output = output + 32; request.descriptors = fds;
            CHECK(native::begin_poll(kernel, login, request, done) == 0 && !done);
        }
        auto *waiter = scheduler.find_thread(login, tid); CHECK(waiter);
        const auto ticket = waiter->wait_registration.ticket();
        const auto object = native::poll_object(login, tid);
        CHECK(waiter->state == sched::ProcessState::blocked && waiter->wait_reason == sched::ThreadWaitReason::poll);
        CHECK(waiter->wait_registration.pending(ticket) && native::poll_objects.resolve(object));
        CHECK(native::poll_objects.accounting().live_objects == live_objects + 1);
        CHECK(scheduler.open_description(description)->references == references + 1);
        // Direct admission above models the saved syscall; completion itself is
        // performed by the actual runtime scan, followed by one synthetic timer.
        const auto original_frame = waiter->native_frame;
        waiter->native_frame = hw::initial_frame(hw::user_begin + 1024, hw::user_begin + 4096);
        waiter->native_frame.registers[0] = 99;
        waiter->syscall_pending = true;
        CHECK(native::prepare_native_thread(*waiter));
        const auto outputs = mock::outputs;
        store_pid(static_cast<i32>(peer)); CHECK(ioctl(kernel, peer, OK_TIOCSPGRP, output) == 0);
        CHECK(waiter->state == sched::ProcessState::blocked && waiter->syscall_pending);
        CHECK(waiter->wait_registration.pending(ticket) && mock::outputs == outputs);
        const char nonce = select ? 'S' : 'P';
        native::input(nonce);
        CHECK(waiter->state == sched::ProcessState::blocked && waiter->syscall_pending);
        CHECK(waiter->wait_registration.pending(ticket) && mock::outputs == outputs);
        struct ActiveThread { Pid pid{}; sched::ThreadId tid{}; bool active{}; };
        std::array<ActiveThread, sched::max_processes * sched::max_threads_per_process> saved{};
        usize saved_count{};
        for (auto &process : scheduler.processes())
            for (auto &thread : process.threads())
                if (process.pid() != login || thread.tid != tid)
                {
                    CHECK(saved_count < saved.size());
                    saved[saved_count++] = {process.pid(), thread.tid, thread.native_active};
                    thread.native_active = false;
                }
        const auto started = native::started;
        const auto last_pid = native::last_pid;
        const auto last_tid = native::last_tid;
        const auto entries = mock::entries;
        native::started = true; native::last_pid = 0; native::last_tid = 0;
        CHECK(native::step(kernel));
        CHECK(native::runtime_status() && mock::entries == entries + 1);
        waiter = scheduler.find_thread(login, tid); CHECK(waiter);
        CHECK(!waiter->syscall_pending && !waiter->wait_registration.pending(ticket));
        CHECK(waiter->wait_registration.completion() == sched::WaitCompletion::ready);
        CHECK(waiter->native_frame.registers[0] == 1);
        CHECK(!native::poll_objects.resolve(object));
        CHECK(native::poll_objects.accounting().live_objects == live_objects);
        CHECK(scheduler.open_description(description)->references == references);
        if (select)
        {
            std::array<u64, 3> observed{};
            for (usize index = 0; index < observed.size(); ++index)
                std::memcpy(&observed[index], mock::memory.data() + 160 + index * 16, sizeof(u64));
            CHECK(observed[0] == 1 && observed[1] == 0 && observed[2] == 0);
        }
        else
        {
            ok_pollfd observed{};
            std::memcpy(&observed, mock::memory.data() + 160, sizeof(observed));
            CHECK(observed.fd == 0 && observed.events == OK_POLLIN && observed.revents == OK_POLLERR);
        }
        CHECK(native::retire_native_thread(*waiter));
        waiter->native_frame = original_frame;
        for (usize index = 0; index < saved_count; ++index)
        {
            auto *thread = scheduler.find_thread(saved[index].pid, saved[index].tid); CHECK(thread);
            thread->native_active = saved[index].active;
        }
        native::started = started; native::last_pid = last_pid; native::last_tid = last_tid;
        CHECK(call(kernel, login, N::read, {0, output, 1}) == -5);
        CHECK(call(kernel, peer, N::read, {0, output, 1}) == 1 && mock::memory[128] == static_cast<std::byte>(nonce));
        CHECK(invoke(kernel, peer, N::read, {0, output, 1}) == std::pair(i64{-11}, false));
        store_pid(static_cast<i32>(login)); CHECK(ioctl(kernel, login, OK_TIOCSPGRP, output) == 0);
        CHECK(invoke(kernel, peer, N::read, {0, output, 1}) == std::pair(i64{-5}, true));
    }
}

void foreground_group_lifetime(Kernel &kernel, Pid login)
{
    auto &scheduler = kernel.scheduler();
    const auto leader = fork(kernel, login);
    CHECK(call(kernel, login, N::setpgid, {leader, leader}) == 0);
    const auto member = fork(kernel, leader);
    store_pid(static_cast<i32>(leader)); CHECK(ioctl(kernel, login, OK_TIOCSPGRP, output) == 0);
    native::input('r');
    CHECK(scheduler.exit_process(leader, 0));
    CHECK(scheduler.find(leader)->state() == sched::ProcessState::zombie);
    CHECK(ioctl(kernel, login, OK_TIOCGPGRP, output) == 0 && loaded_pid() == static_cast<i32>(leader));
    CHECK(scheduler.console_authorized(login) && scheduler.console_authorized(member));
    CHECK(call(kernel, member, N::read, {0, output, 1}) == 1 && mock::memory[128] == std::byte{'r'});
    CHECK(scheduler.reap_process(leader) && !scheduler.find(leader));
    CHECK(ioctl(kernel, login, OK_TIOCGPGRP, output) == 0 && loaded_pid() == static_cast<i32>(leader));
    native::input('s');
    CHECK(call(kernel, member, N::read, {0, output, 1}) == 1 && mock::memory[128] == std::byte{'s'});
    CHECK(scheduler.exit_process(member, 0));
    // A retained zombie still names the group; reaping its final member removes
    // membership, but never silently replaces the stored foreground PGID.
    store_pid(static_cast<i32>(leader)); CHECK(ioctl(kernel, login, OK_TIOCSPGRP, output) == 0);
    native::input('t');
    CHECK(call(kernel, login, N::read, {0, output, 1}) == -5);
    CHECK(scheduler.reap_process(member) && !scheduler.find(member));
    CHECK(scheduler.console_authorized(login));
    CHECK(ioctl(kernel, login, OK_TIOCGPGRP, output) == 0 && loaded_pid() == static_cast<i32>(leader));
    store_pid(static_cast<i32>(leader)); CHECK(ioctl(kernel, login, OK_TIOCSPGRP, output) == -3);
    CHECK(ioctl(kernel, login, OK_TIOCGPGRP, output) == 0 && loaded_pid() == static_cast<i32>(leader));
    CHECK(call(kernel, login, N::read, {0, output, 1}) == -5);
    store_pid(static_cast<i32>(login)); CHECK(ioctl(kernel, login, OK_TIOCSPGRP, output) == 0);
    CHECK(call(kernel, login, N::read, {0, output, 1}) == 1 && mock::memory[128] == std::byte{'t'});
}

void lifecycle(Kernel &kernel)
{
    auto &scheduler = kernel.scheduler();
    const auto context = kernel.arch().make_user_context({hw::user_begin, hw::user_end - 4096});
    const auto init = scheduler.create_init_process(context); CHECK(init && init.value() == 1);
    const auto login = fork(kernel, 1);
    const auto epoch = call(kernel, 1, N::console_claim, {login}); CHECK(epoch > 0);
    CHECK(ioctl(kernel, login, ~u64{0}, hw::user_end, 99) == -9);
    CHECK(ioctl(kernel, login, ~u64{0}, hw::user_end) == -25);
    CHECK(ioctl(kernel, login, OK_TIOCGSID, output) == -25);
    CHECK(ioctl(kernel, login, OK_TIOCSCTTY, 1) == -95);
    CHECK(ioctl(kernel, login, OK_TIOCSCTTY) == -1);
    native::runtime_kernel = &kernel;
    native::input('p');
    CHECK(call(kernel, login, N::read, {0, output, 1}) == 1 && mock::memory[128] == std::byte{'p'});
    CHECK(call(kernel, login, N::setsid) == static_cast<i64>(login));
    CHECK(ioctl(kernel, login, OK_TIOCSCTTY) == 0);
    mock::memory.fill(std::byte{0xa5});
    CHECK(ioctl(kernel, login, OK_TIOCGSID, output) == 0 && loaded_pid() == static_cast<i32>(login));
    CHECK(mock::memory[127] == std::byte{0xa5} && mock::memory[132] == std::byte{0xa5});
    CHECK(ioctl(kernel, login, OK_TIOCGPGRP, output) == 0 && loaded_pid() == static_cast<i32>(login));
    activate(kernel, login);
    const auto duplicate = kernel.posix().duplicate(0); CHECK(duplicate);
    CHECK(ioctl(kernel, login, OK_TIOCGSID, output, duplicate.value()) == 0);
    const auto pipe = kernel.posix().pipe(); CHECK(pipe);
    CHECK(ioctl(kernel, login, OK_TIOCGSID, output, pipe.value()[0]) == -25);
    const auto peer = fork(kernel, login);
    CHECK(call(kernel, login, N::setpgid, {peer, peer}) == 0);
    native::input('a');
    CHECK(call(kernel, peer, N::read, {0, output, 1}) == -5);
    CHECK(call(kernel, login, N::read, {0, output, 1}) == 1 && mock::memory[128] == std::byte{'a'});
    for (const auto &[target, error] : std::array<std::pair<i32, i64>, 4>{{{-1, -22}, {0, -3}, {999999, -3}, {1, -1}}})
    {
        store_pid(target); CHECK(ioctl(kernel, login, OK_TIOCSPGRP, output) == error);
        CHECK(ioctl(kernel, login, OK_TIOCGPGRP, output) == 0 && loaded_pid() == static_cast<i32>(login));
    }
    store_pid(static_cast<i32>(peer));
    mock::forbidden = output;
    CHECK(ioctl(kernel, login, OK_TIOCSPGRP, output) == -14);
    mock::forbidden = 0;
    CHECK(ioctl(kernel, login, OK_TIOCGPGRP, output) == 0 && loaded_pid() == static_cast<i32>(login));
    CHECK(invoke(kernel, login, N::read, {0, output, 1}) == std::pair(i64{-11}, false));
    store_pid(static_cast<i32>(peer));
    CHECK(ioctl(kernel, peer, OK_TIOCSPGRP, output) == 0);
    CHECK(scheduler.console_authorized(login) && scheduler.console_authorized(peer));
    CHECK(invoke(kernel, login, N::read, {0, output, 1}) == std::pair(i64{-5}, true));
    native::input('b');
    CHECK(scheduler.console_authorized(peer)); // Ingress never revokes a background shell.
    CHECK(call(kernel, peer, N::read, {0, output, 1}) == 1 && mock::memory[128] == std::byte{'b'});
    activate(kernel, login);
    bool done = true;
    const std::array<ok_pollfd, 1> fds{{{0, OK_POLLIN, 0}}};
    CHECK(native::begin_poll(kernel, login, {output + 32, fds, 0, false, false}, done) == 1 && done);
    ok_pollfd observed{}; std::memcpy(&observed, mock::memory.data() + 160, sizeof(observed));
    CHECK(observed.revents == OK_POLLERR);
    const std::array<native::PipeVector, 2> iov{{{output, 1}, {output + 8, 1}}};
    std::memcpy(mock::memory.data() + 512, iov.data(), sizeof(iov));
    CHECK(invoke(kernel, peer, N::readv, {0, vectors, 2}) == std::pair(i64{-11}, false));
    store_pid(static_cast<i32>(login));
    CHECK(ioctl(kernel, login, OK_TIOCSPGRP, output) == 0);
    CHECK(invoke(kernel, peer, N::readv, {0, vectors, 2}) == std::pair(i64{-5}, true));
    const auto detached = fork(kernel, login);
    CHECK(call(kernel, detached, N::setsid) == static_cast<i64>(detached));
    CHECK(scheduler.console_authorized(detached));
    CHECK(call(kernel, detached, N::read, {0, output, 1}) == -5);
    CHECK(ioctl(kernel, detached, OK_TIOCGSID, output) == -25);
    CHECK(ioctl(kernel, detached, OK_TIOCSCTTY) == -1);
    CHECK(scheduler.console_authorized(login));
    // Same-session ordinary users may hand foreground authority back to the shell.
    scheduler.find(peer)->set_credentials({.uid=1000,.gid=1000,.euid=1000,.egid=1000,.suid=1000,.sgid=1000});
    store_pid(static_cast<i32>(peer)); CHECK(ioctl(kernel, peer, OK_TIOCSPGRP, output) == 0);
    store_pid(static_cast<i32>(login)); CHECK(ioctl(kernel, peer, OK_TIOCSPGRP, output) == 0);
    linux_profile<lu::Profile::i386>(kernel, login, peer);
    linux_profile<lu::Profile::x86_64>(kernel, login, peer);
    linux_profile<lu::Profile::aarch64>(kernel, login, peer);
    linux_profile<lu::Profile::arm32>(kernel, login, peer);
    linux_profile<lu::Profile::rv64>(kernel, login, peer);
    linux_profile<lu::Profile::rv32>(kernel, login, peer);
    linux_profile<lu::Profile::loongarch64>(kernel, login, peer);
    linux_profile<lu::Profile::mips>(kernel, login, peer);
    linux_profile<lu::Profile::mips64>(kernel, login, peer);
    linux_profile<lu::Profile::ppc>(kernel, login, peer);
    readiness_handoff(kernel, login, peer);
    foreground_group_lifetime(kernel, login);
    CHECK(call(kernel, 1, N::console_revoke, {static_cast<uptr>(epoch)}) == 0);
    CHECK(call(kernel, login, N::read, {0, output, 1}) == -13);
    CHECK(ioctl(kernel, login, OK_TIOCGSID, output) == -25);
    const auto next = fork(kernel, 1);
    CHECK(call(kernel, 1, N::console_claim, {next}) > epoch);
    CHECK(call(kernel, next, N::setsid) == static_cast<i64>(next));
    CHECK(ioctl(kernel, next, OK_TIOCSCTTY) == 0);
    native::input('n');
    CHECK(call(kernel, login, N::read, {0, output, 1}) == -13);
    CHECK(call(kernel, next, N::read, {0, output, 1}) == 1 && mock::memory[128] == std::byte{'n'});
    CHECK(scheduler.exit_process(next, 0));
    CHECK(!scheduler.console_authorized(next));
    native::runtime_kernel = nullptr;
}
} // namespace

int main()
{
    static Kernel kernel; CHECK(kernel.boot({}));
    lifecycle(kernel);
    CHECK(native::poll_objects.accounting().live_objects == 0);
    std::printf("NATIVE_CONSOLE_CONTROL_RUNTIME_PASS native_calls=%zu linux_calls=%zu profiles=10 hosted=1 guest=0\n", checks, profile_calls);
}
