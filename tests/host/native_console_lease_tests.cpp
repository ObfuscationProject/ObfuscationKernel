// Actual native dispatch and scheduler ownership with hosted checked-copy mappings.
#include "native_clock_model.hpp"
#include "../kernel/points/console_input_checks.hpp"
#include <string>
#include <type_traits>

namespace
{
usize ingress_calls{}, ingress_pending{};
bool ingress_stuck{};
std::string console_output;
}
extern "C" int ok_platform_console_poll()
{
    ++ingress_calls;
    if (ingress_stuck) return 'x';
    if (ingress_pending) { --ingress_pending; return 'x'; }
    return -1;
}
extern "C" void ok_platform_console_write_char(char value) { console_output += value; }

namespace ok::sched
{
struct ConsoleInputLeaseTestAccess
{
    static void exhaust(ConsoleInputLease &input) { input.epoch_ = native_console_epoch_limit; }
};
}

namespace
{
using N = syscall::Number;
constexpr uptr out = hw::user_begin + 128;
constexpr uptr vectors_address = hw::user_begin + 512;

void activate(Kernel &kernel, sched::ProcessId pid, sched::ThreadId tid = 0)
{
    auto *process = kernel.scheduler().find(pid); CHECK(process);
    if (process->state() == sched::ProcessState::created) CHECK(kernel.scheduler().set_runnable(pid));
    if (!tid) tid = process->threads()[0].tid;
    auto *thread = kernel.scheduler().find_thread(pid, tid); CHECK(thread);
    thread->native_active = true;
    const auto status = kernel.scheduler().activate_thread(pid, tid);
    if (!status) std::fprintf(stderr, "activate pid=%llu tid=%llu: %.*s\n",
        static_cast<unsigned long long>(pid), static_cast<unsigned long long>(tid),
        static_cast<int>(status.message().size()), status.message().data());
    CHECK(status);
    CHECK(kernel.posix().activate(pid));
}

std::pair<i64, bool> invoke(Kernel &kernel, sched::ProcessId pid, N number,
                           std::array<uptr, 6> arguments = {}, sched::ThreadId tid = 0)
{
    activate(kernel, pid, tid);
    auto *caller = kernel.scheduler().current_thread();
    bool complete = true;
    auto result = native::dispatch(kernel, pid, {static_cast<uptr>(number), arguments}, complete);
    // The native executor clears this stamp only on terminal syscall completion.
    if (complete) caller->console_wait_epoch = 0;
    return {result, complete};
}

i64 call(Kernel &kernel, sched::ProcessId pid, N number, std::array<uptr, 6> arguments = {})
{
    const auto [result, complete] = invoke(kernel, pid, number, arguments);
    CHECK(complete);
    return result;
}

sched::ProcessId child(Kernel &kernel, sched::ProcessId parent)
{
    activate(kernel, parent);
    auto result = kernel.scheduler().fork_process(parent); CHECK(result);
    kernel.scheduler().find(result.value())->threads()[0].native_active = true;
    return result.value();
}

i64 linux_call(Kernel &kernel, sched::ProcessId pid, u64 number, std::array<uptr, 6> arguments)
{
    activate(kernel, pid);
    auto *process = kernel.scheduler().find(pid);
    process->resources().personality = sched::UserPersonality::linux;
    hw::UserFrame frame{};
    frame.registers[14] = number;
    constexpr std::array<usize, 6> slots{9, 10, 11, 5, 7, 8};
    for (usize index = 0; index < slots.size(); ++index) frame.registers[slots[index]] = arguments[index];
    bool complete = true;
    const auto result = user::linux_bootstrap::dispatch<lu::Profile::x86_64>(kernel, pid, frame, complete,
        [&](hw::Syscall request, bool &done) { return native::dispatch(kernel, pid, request, done); });
    CHECK(complete);
    process->resources().personality = sched::UserPersonality::native;
    return result;
}

void bounded_algorithm()
{
    static_assert(!std::is_copy_constructible_v<sched::ConsoleInputLease>);
    CHECK(tests::check_console_input_lease_model());
    sched::ConsoleInputLease input;
    CHECK(input.bind_manager(1));
    const auto epoch = input.claim(1, 2); CHECK(epoch);
    for (usize index = 0; index < 256; ++index) CHECK(input.input(static_cast<char>(index)));
    CHECK(!input.input('x'));
    std::array<std::byte, 63> bytes{};
    for (usize start = 0; start < 256;)
    {
        auto read = input.read(2, epoch.value(), epoch.value(), bytes); CHECK(read);
        for (usize index = 0; index < read.value(); ++index)
            CHECK(bytes[index] == static_cast<std::byte>(static_cast<unsigned char>(start + index)));
        start += read.value();
    }
    CHECK(input.input('z') && input.flush(2, epoch.value(), epoch.value()) && !input.buffered());
    CHECK(input.read(2, epoch.value(), epoch.value(), bytes).status().code() == StatusCode::would_block);
    CHECK(input.revoke(1, epoch.value()));
    sched::ConsoleInputLeaseTestAccess::exhaust(input);
    CHECK(input.claim(1, 3).status().code() == StatusCode::overflow && !input.active());
}

void runtime_ownership(Kernel &kernel)
{
    auto &scheduler = kernel.scheduler();
    const auto context = kernel.arch().make_user_context({hw::user_begin, hw::user_end - 4096});
    auto manager = scheduler.create_init_process(context); CHECK(manager && manager.value() == 1);
    auto service = scheduler.create_user_process("unrelated-root-service", context); CHECK(service);
    scheduler.find(service.value())->set_credentials(user::root_credentials());
    native::runtime_kernel = &kernel;
    native::input('x'); // Dropped before any lease exists.
    CHECK(call(kernel, service.value(), N::read, {0, out, 1}) == -13);
    const auto login = child(kernel, 1);
    CHECK(call(kernel, login, N::console_claim, {service.value()}) == -13); // PID 1 authority is never inherited.
    activate(kernel, service.value());
    bool spoof_complete = true;
    CHECK(native::dispatch(kernel, 1, {static_cast<uptr>(N::console_claim), {login}}, spoof_complete) == -22);
    const auto parent_candidate = child(kernel, 1);
    static_cast<void>(child(kernel, parent_candidate));
    CHECK(call(kernel, 1, N::console_claim, {parent_candidate}) == -13);
    const auto threaded_candidate = child(kernel, 1);
    CHECK(scheduler.create_thread(threaded_candidate, context));
    CHECK(call(kernel, 1, N::console_claim, {threaded_candidate}) == -13);
    CHECK(call(kernel, 1, N::console_claim, {service.value()}) == -13);
    scheduler.find(1)->resources().personality = sched::UserPersonality::linux;
    CHECK(call(kernel, 1, N::console_claim, {login}) == -38);
    scheduler.find(1)->resources().personality = sched::UserPersonality::native;
    CHECK(call(kernel, service.value(), N::console_claim, {login}) == -13);
    CHECK(call(kernel, 1, N::console_claim, {login, 1}) == -22);
    CHECK(call(kernel, 1, N::console_claim, {1}) == -13);
    ingress_pending = 3;
    const auto first = call(kernel, 1, N::console_claim, {login}); CHECK(first > 0 && !ingress_pending);
    CHECK(scheduler.find(login)->console_epoch() == first);
    CHECK(call(kernel, 1, N::console_claim, {login}) == -13);
    CHECK(call(kernel, service.value(), N::console_revoke, {static_cast<uptr>(first)}) == -13);
    const auto echo_before = console_output;
    for (const char value : std::string("s3cr")) native::input(value);
    CHECK(console_output == echo_before);
    CHECK(call(kernel, service.value(), N::read, {0, out, 4}) == -13);
    activate(kernel, service.value());
    const auto alias = kernel.posix().duplicate(0); CHECK(alias);
    CHECK(call(kernel, service.value(), N::read, {static_cast<uptr>(alias.value()), out, 4}) == -13);
    CHECK(linux_call(kernel, service.value(), 0, {0, out, 4}) == -13);
    CHECK(call(kernel, login, N::read, {0, out, 4}) == 4);
    CHECK(std::memcmp(mock::memory.data() + 128, "s3cr", 4) == 0);

    const std::array<native::PipeVector, 2> vectors{{{out, 1}, {out + 8, 1}}};
    std::memcpy(mock::memory.data() + 512, vectors.data(), sizeof(vectors));
    native::input('a'); native::input('b');
    CHECK(linux_call(kernel, service.value(), 19, {0, vectors_address, 2}) == -13);
    CHECK(linux_call(kernel, login, 19, {0, vectors_address, 2}) == 2);
    CHECK(mock::memory[128] == std::byte{'a'} && mock::memory[136] == std::byte{'b'});
    native::input('e');
    mock::write_forbidden = out;
    CHECK(call(kernel, login, N::read, {0, out, 1}) == -14);
    mock::write_forbidden = 0;
    CHECK(call(kernel, login, N::read, {0, out, 1}) == 1 && mock::memory[128] == std::byte{'e'});

    const auto orphan = child(kernel, login);
    CHECK(scheduler.find(orphan)->console_epoch() == first);
    CHECK(call(kernel, orphan, N::console_flush, {static_cast<uptr>(first)}) == -13);
    CHECK(call(kernel, service.value(), N::console_flush, {static_cast<uptr>(first)}) == -13);
    native::input('i');
    CHECK(call(kernel, orphan, N::read, {0, out, 1}) == 1);
    auto *process = scheduler.find(orphan);
    process->set_credentials(user::Credentials{.uid=1000, .gid=1000, .euid=1000, .egid=1000, .suid=1000, .sgid=1000});
    const auto old_space = process->address_space_id();
    auto bad_context = context; bad_context.architecture = arch::Architecture::aarch64;
    CHECK(!scheduler.replace_image(orphan, bad_context, {}));
    CHECK(process->console_epoch() == first && process->address_space_id() == old_space);
    CHECK(scheduler.replace_image(orphan, context, {}));
    CHECK(process->console_epoch() == first && process->address_space_id() != old_space);
    native::input('d');
    CHECK(call(kernel, orphan, N::read, {0, out, 1}) == 1);

    // A poll holds its original generation even after a later claim.
    activate(kernel, orphan);
    const std::array<ok_pollfd, 1> descriptors{{{0, OK_POLLIN, 0}}};
    bool complete = true;
    CHECK(native::begin_poll(kernel, orphan, {out + 128, descriptors, 0, false, false}, complete) == 0 && !complete);
    memory::ObjectHandle poll{};
    for (const auto &entry : native::poll_waits) if (entry.pid == orphan) poll = entry.object;
    CHECK(poll.pool);
    const auto read_thread = scheduler.create_thread(orphan, context); CHECK(read_thread);
    auto blocked = invoke(kernel, orphan, N::readv, {0, vectors_address, 2}, read_thread.value());
    CHECK(blocked.first == -11 && !blocked.second);
    auto *reader = scheduler.find_thread(orphan, read_thread.value());
    CHECK(reader->state == sched::ProcessState::blocked && reader->console_wait_epoch == first);
    native::input('q'); // Already runnable, but its original request remains bound.
    CHECK(reader->state == sched::ProcessState::runnable);
    CHECK(call(kernel, 1, N::console_revoke, {static_cast<uptr>(first)}) == 0);
    CHECK(!scheduler.console_authorized(orphan));
    const auto next_login = child(kernel, 1);
    const auto next = call(kernel, 1, N::console_claim, {next_login}); CHECK(next > first);
    native::input('n');
    CHECK(invoke(kernel, orphan, N::readv, {0, vectors_address, 2}, read_thread.value()) == std::pair(i64{-13}, true));
    auto wait = native::poll_objects.resolve(poll); CHECK(wait);
    auto ready = native::poll_ready(kernel, *wait.value()); CHECK(ready && ready.value() == 1);
    CHECK(wait.value()->fds[0].revents == (OK_POLLERR | OK_POLLHUP));
    CHECK(native::poll_output(scheduler.find(orphan)->resources().native_space, *wait.value()));
    native::cancel_poll(kernel, poll);
    CHECK(call(kernel, 1, N::console_revoke, {static_cast<uptr>(first)}) == -13);
    CHECK(call(kernel, next_login, N::read, {0, out, 1}) == 1 && mock::memory[128] == std::byte{'n'});

    CHECK(scheduler.exit_process(login, 0));
    CHECK(scheduler.find(orphan)->parent_pid() == 1);
    CHECK(scheduler.reap_process(login)); // Moves PCB storage; numeric membership stays stale.
    native::input('z');
    CHECK(call(kernel, orphan, N::read, {0, out, 1}) == -13);
    CHECK(call(kernel, next_login, N::read, {0, out, 1}) == 1 && mock::memory[128] == std::byte{'z'});
    const auto surviving_child = child(kernel, next_login);
    CHECK(call(kernel, next_login, N::console_flush, {static_cast<uptr>(next)}) == 0);
    const auto descendant_waiting = invoke(kernel, surviving_child, N::read, {0, out, 1});
    CHECK(descendant_waiting.first == -11 && !descendant_waiting.second);
    const auto waiting = invoke(kernel, next_login, N::read, {0, out, 1});
    CHECK(waiting.first == -11 && !waiting.second);
    CHECK(scheduler.exit_thread(next_login, scheduler.find(next_login)->threads()[0].tid));
    CHECK(!scheduler.console_authorized(next_login));
    CHECK(scheduler.find(surviving_child)->threads()[0].state == sched::ProcessState::runnable);
    CHECK(invoke(kernel, surviving_child, N::read, {0, out, 1}) == std::pair(i64{-13}, true));
    CHECK(!scheduler.console_authorized(surviving_child));

    // Failure to drain a platform source never returns an active lease.
    const auto flooded = child(kernel, 1);
    ingress_stuck = true;
    const auto before = ingress_calls;
    CHECK(call(kernel, 1, N::console_claim, {flooded}) == -5 && ingress_calls - before == 1024);
    ingress_stuck = false;
    CHECK(!scheduler.console_authorized(flooded));
    CHECK(call(kernel, 1, N::console_claim, {flooded}) == -13); // An issued old generation is never reblessed.
    const auto final_login = child(kernel, 1);
    const auto final_epoch = call(kernel, 1, N::console_claim, {final_login}); CHECK(final_epoch > next);
    CHECK(scheduler.exit_process(1, 1));
    CHECK(!scheduler.console_authorized(final_login));
    native::input('x');
    CHECK(call(kernel, final_login, N::read, {0, out, 1}) == -13);
    CHECK(call(kernel, final_login, N::console_claim, {service.value()}) == -13);
    native::runtime_kernel = nullptr;
}
}

int main()
{
    static_assert(OK_NATIVE_CONSOLE_LEASE_VERSION == 1 && OK_CONSOLE_EPOCH_MAX == sched::native_console_epoch_limit);
    static_assert(OK_SYS_CONSOLE_CLAIM == static_cast<u64>(N::console_claim));
    bounded_algorithm();
    static Kernel kernel; CHECK(kernel.boot({}));
    runtime_ownership(kernel);
    CHECK(native::poll_objects.accounting().live_objects == 0);
    std::puts("NATIVE_CONSOLE_LEASE_MODEL_PASS default_closed=1 pid1_authority=1 read_readv_poll=1 linux=1 fork_exec=1 revoke_epoch=1 adoption=1 noecho=1 ingress_bound=1024 guest=0");
}
