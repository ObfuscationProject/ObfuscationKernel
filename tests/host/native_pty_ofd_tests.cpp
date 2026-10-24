// Actual copied SchedulerState, PosixService, fork, reservations and UNIX graph.
// Hosted owner execution only; no user-mode ELF, syscall entry or hardware claim.
#include "ok/posix/posix.hpp"
#include "ok/user/native_pty_pair_dispatch.hpp"
#include <cstdio>
#include <cstdlib>
#include <new>
#include <type_traits>

using namespace ok;
static usize checks{}, cases{};
#define CHECK(expression) do { ++checks; if (!static_cast<bool>(expression)) { \
    std::fprintf(stderr, "FAIL %d %s\n", __LINE__, #expression); std::abort(); } } while (false)
namespace ok::sched
{
struct SchedulerIdentityTestAccess
{
    static u64 token(SchedulerState &owner, u64 value)
    { return std::exchange(owner.next_unix_receive_token_, value); }
};
}
std::span<const std::byte> bytes(std::string_view value)
{ return {reinterpret_cast<const std::byte *>(value.data()), value.size()}; }
struct Fixture
{
    sched::Scheduler scheduler;
    fs::VirtualFileSystem vfs;
    driver::ConsoleDriver console;
    posix::PosixService posix;
    u64 pid{};
    Fixture()
    {
        CHECK(console.start()); CHECK(posix.initialize(vfs, console, scheduler));
        const auto context = arch::configured_arch_operations().make_user_context({1, 2, 0});
        const auto made = scheduler.create_user_process("raw-pty-ofd", context); CHECK(made); pid = made.value();
        CHECK(posix.activate(pid));
    }
    ~Fixture()
    {
        CHECK(scheduler.drain_all_resources()); CHECK(!scheduler.open_description_count());
        const auto audit = scheduler.pty_audit(); CHECK(!audit.live_pairs && !audit.live_endpoints);
        CHECK(!audit.buffered_bytes && audit.cleared_outside_live_bytes);
    }
    sched::ProcessResources &resources() { return scheduler.find(pid)->resources(); }
    u16 id(i32 fd) { const auto *slot = resources().files.entry(fd); CHECK(slot && slot->used); return slot->description; }
    std::array<i32, 2> pair(u32 flags = 0)
    {
        auto operation = scheduler.prepare_pty_pair(pid, flags); CHECK(operation && operation.value().validate());
        const auto fds = operation.value().descriptors(); operation.value().publish_checked(); return fds;
    }
};
template <typename Test> void run(const char *name, Test test)
{
    alignas(Fixture) static std::array<std::byte, sizeof(Fixture)> storage{};
    auto &fixture = *::new(storage.data()) Fixture;
    test(fixture); fixture.~Fixture(); ++cases; std::printf("PASS %s\n", name);
}
void prepared_lifetime_and_rollback(Fixture &f)
{
    static_assert(!std::is_copy_constructible_v<sched::PreparedPtyPair>);
    static_assert(std::is_nothrow_move_constructible_v<sched::PreparedPtyPair>);
    const auto initial = f.resources().files.open_count();
    {
        auto prepared = f.scheduler.prepare_pty_pair(f.pid, posix::o_CLOEXEC); CHECK(prepared);
        const auto fds = prepared.value().descriptors(); CHECK(fds[0] != fds[1]);
        for (const auto fd : fds)
        {
            const auto *slot = f.resources().files.entry(fd);
            CHECK(slot && !slot->used && slot->reservation && f.resources().files.contains(fd));
            CHECK(f.posix.duplicate_to(0, fd).status().code() == StatusCode::busy);
        }
        CHECK(f.resources().prepared_descriptor_operations == 2);
        CHECK(f.scheduler.pty_audit().live_pairs == 1);
        CHECK(f.scheduler.fork_process(f.pid).status().code() == StatusCode::busy);
        CHECK(f.scheduler.exit_process(f.pid, 0).code() == StatusCode::busy);
        CHECK(f.scheduler.replace_image(f.pid, {}, {}).code() == StatusCode::busy);
        CHECK(f.scheduler.drain_resources(f.resources()).code() == StatusCode::busy);
        CHECK(f.scheduler.drain_all_resources().code() == StatusCode::busy);
        CHECK(f.scheduler.close_shutdown_admission().code() == StatusCode::busy);
        sched::PreparedPtyPair moved{std::move(prepared.value())};
        CHECK(!prepared.value().validate() && moved.validate());
        CHECK(prepared.value().cancel()); CHECK(moved.cancel()); CHECK(moved.cancel());
        CHECK(!moved.validate());
    }
    CHECK(f.resources().files.open_count() == initial && !f.resources().prepared_descriptor_operations);
    CHECK(!f.scheduler.pty_audit().live_pairs && f.scheduler.pty_audit().cleared_outside_live_bytes);
    { auto scope = f.scheduler.prepare_pty_pair(f.pid); CHECK(scope); }
    CHECK(!f.scheduler.pty_audit().live_pairs && !f.resources().prepared_descriptor_operations);
    CHECK(f.scheduler.prepare_pty_pair(f.pid, 1).status().code() == StatusCode::invalid_argument);
}
void canonical_dup_flags_and_final_retirement(Fixture &f)
{
    const auto pair = f.pair(posix::o_NONBLOCK | posix::o_CLOEXEC);
    const auto master = f.id(pair[0]), slave = f.id(pair[1]);
    const auto handle = f.scheduler.open_description(master)->pty_endpoint;
    CHECK(handle && !f.scheduler.is_unix_socket(master));
    const auto duplicate = f.posix.duplicate(pair[0]); CHECK(duplicate);
    CHECK(f.id(duplicate.value()) == master && f.scheduler.open_description(master)->references == 2);
    CHECK(f.posix.fcntl(pair[0], posix::f_GETFD, 0).value() == posix::fd_CLOEXEC);
    CHECK(f.posix.fcntl(duplicate.value(), posix::f_GETFD, 0).value() == 0);
    CHECK(f.posix.fcntl(duplicate.value(), posix::f_SETFL, 0));
    CHECK(!(f.posix.fcntl(pair[0], posix::f_GETFL, 0).value() & posix::o_NONBLOCK));
    CHECK(f.scheduler.retain_description(master)); // Exact future continuation/poll OFD hold.
    CHECK(f.posix.close(pair[0])); CHECK(f.posix.close(duplicate.value()));
    CHECK(f.scheduler.open_description(master)->references == 1);
    CHECK(f.scheduler.pty_audit().live_endpoints == 2);
    CHECK(f.scheduler.write_pty_description(master, bytes("abc")).value() == 3);
    CHECK(f.scheduler.release_description(master));
    CHECK(!f.scheduler.open_description(master) && f.scheduler.pty_audit().live_endpoints == 1);
    std::array<std::byte, 8> output{};
    CHECK(f.posix.read(pair[1], output).value() == 3 && output[0] == std::byte{'a'});
    CHECK(f.posix.read(pair[1], output).value() == 0);
    CHECK(f.posix.write(pair[1], bytes("x")).status().code() == StatusCode::io_error);
    CHECK(f.posix.close(pair[1])); CHECK(!f.scheduler.open_description(slave));
    const auto next = f.pair(); CHECK(f.scheduler.open_description(f.id(next[0]))->pty_endpoint.generation > handle.generation);
}
void actual_fork_and_exec_cloexec(Fixture &f)
{
    const auto pair = f.pair(posix::o_CLOEXEC); const auto master = f.id(pair[0]);
    const auto survivor = f.posix.duplicate(pair[0]); CHECK(survivor);
    const auto child = f.scheduler.fork_process(f.pid, false); CHECK(child);
    CHECK(f.scheduler.open_description(master)->references == 4);
    CHECK(f.scheduler.replace_image(child.value(), {}, {}));
    auto &resources = f.scheduler.find(child.value())->resources();
    CHECK(!resources.files.contains(pair[0]) && !resources.files.contains(pair[1]));
    CHECK(resources.files.contains(survivor.value()));
    CHECK(f.scheduler.open_description(master)->references == 3);
    CHECK(f.posix.close(pair[0])); CHECK(f.posix.close(survivor.value()));
    CHECK(f.scheduler.open_description(master)->references == 1);
    CHECK(f.scheduler.exit_process(child.value(), 0));
    CHECK(!f.scheduler.open_description(master));
    CHECK(f.scheduler.reap_process(child.value()));
    CHECK(f.scheduler.pty_audit().live_endpoints == 1);
}
void descriptor_ofd_and_provider_exhaustion(Fixture &f)
{
    std::array<i32, 28> duplicates{};
    for (auto &fd : duplicates) { const auto made = f.posix.duplicate(0); CHECK(made); fd = made.value(); }
    const auto before = f.resources().files.open_count();
    CHECK(f.scheduler.prepare_pty_pair(f.pid).status().code() == StatusCode::process_file_limit);
    CHECK(f.resources().files.open_count() == before && !f.resources().prepared_descriptor_operations);
    CHECK(!f.scheduler.pty_audit().live_pairs);
    for (const auto fd : duplicates) CHECK(f.posix.close(fd));

    std::array<u16, sched::max_open_descriptions - 4> held{};
    for (auto &id : held)
    {
        const auto made = f.scheduler.allocate_description({}); CHECK(made); id = made.value();
        CHECK(f.scheduler.retain_description(id));
    }
    const auto descriptions = f.scheduler.open_description_count();
    CHECK(f.scheduler.prepare_pty_pair(f.pid).status().code() == StatusCode::system_file_limit);
    CHECK(f.scheduler.open_description_count() == descriptions && !f.resources().prepared_descriptor_operations);
    CHECK(!f.scheduler.pty_audit().live_pairs);
    for (const auto id : held) CHECK(f.scheduler.release_description(id));

    std::array<std::array<i32, 2>, 8> pairs{};
    for (auto &pair : pairs) pair = f.pair();
    const auto count = f.resources().files.open_count();
    CHECK(f.scheduler.prepare_pty_pair(f.pid).status().code() == StatusCode::system_file_limit);
    CHECK(f.resources().files.open_count() == count && !f.resources().prepared_descriptor_operations);
    CHECK(f.scheduler.pty_audit().live_pairs == 8);
    for (const auto &pair : pairs) { CHECK(f.posix.close(pair[0])); CHECK(f.posix.close(pair[1])); }
}
void token_exhaustion_rolls_back_first_reservation(Fixture &f)
{
    const auto previous = sched::SchedulerIdentityTestAccess::token(f.scheduler, ~u64{0} - 1);
    CHECK(f.scheduler.prepare_pty_pair(f.pid).status().code() == StatusCode::overflow);
    CHECK(!f.resources().prepared_descriptor_operations && f.resources().files.open_count() == 3);
    CHECK(!f.scheduler.pty_audit().live_pairs);
    static_cast<void>(sched::SchedulerIdentityTestAccess::token(f.scheduler, previous));
}
void scm_rights_keeps_endpoint_owner(Fixture &f)
{
    const auto pair = f.pair(); const auto master = f.id(pair[0]);
    const auto sockets = f.posix.unix_socketpair(0); CHECK(sockets);
    const std::array<i32, 2> rights{pair[0], pair[0]};
    CHECK(f.posix.send_unix_rights(sockets.value()[0], bytes("r"), rights));
    CHECK(f.scheduler.open_description(master)->references == 3);
    CHECK(f.scheduler.open_description(master)->unix_queue_references == 2);
    CHECK(f.posix.close(pair[0])); CHECK(f.scheduler.pty_audit().live_endpoints == 2);
    std::array<std::byte, 8> output{};
    auto receive = f.posix.prepare_unix_receive(sockets.value()[1], output, 2); CHECK(receive);
    const auto first = receive.value().descriptors()[0], second = receive.value().descriptors()[1];
    CHECK(receive.value().commit());
    CHECK(f.id(first) == master && f.id(second) == master);
    CHECK(f.scheduler.open_description(master)->references == 2 && !f.scheduler.open_description(master)->unix_queue_references);
    CHECK(f.posix.write(first, bytes("ok")).value() == 2);
    CHECK(f.posix.read(pair[1], output).value() == 2 && output[0] == std::byte{'o'});
    CHECK(f.posix.close(first)); CHECK(f.scheduler.pty_audit().live_endpoints == 2);
    CHECK(f.posix.close(second)); CHECK(f.scheduler.pty_audit().live_endpoints == 1);
    CHECK(f.posix.read(pair[1], output).value() == 0);
}
void unreachable_unix_cycle_releases_pty_edge(Fixture &f)
{
    const auto pair = f.pair(); const auto master = f.id(pair[0]);
    const auto sockets = f.posix.unix_socketpair(0); CHECK(sockets);
    const std::array<i32, 3> rights{pair[0], sockets.value()[0], sockets.value()[1]};
    CHECK(f.posix.send_unix_rights(sockets.value()[0], bytes("x"), rights));
    CHECK(f.posix.close(pair[0]));
    CHECK(f.scheduler.open_description(master) && f.scheduler.pty_audit().live_endpoints == 2);
    CHECK(f.posix.close(sockets.value()[0])); CHECK(f.posix.close(sockets.value()[1]));
    CHECK(!f.scheduler.open_description(master)); CHECK(f.scheduler.pty_audit().live_endpoints == 1);
    const auto ready = f.posix.ready_events(pair[1], posix::ReadyEvents::readable); CHECK(ready);
    CHECK((ready.value().events & posix::ReadyEvents::hangup) == posix::ReadyEvents::hangup);
}
void provider_exclusivity_stat_seek_and_controls(Fixture &f)
{
    const auto pair = f.pair(); const auto id = f.id(pair[0]);
    auto copy = *f.scheduler.open_description(id);
    CHECK(f.scheduler.allocate_description(copy).status().code() == StatusCode::already_exists);
    copy.console = true;
    CHECK(f.scheduler.allocate_description(copy).status().code() == StatusCode::invalid_argument);
    const auto metadata = f.posix.fstat(pair[0]); CHECK(metadata);
    CHECK(metadata.value().type == fs::NodeType::device);
    CHECK((metadata.value().mode & fs::mode_type_mask) == fs::mode_character_device);
    CHECK(f.posix.seek(pair[0], 0, posix::SeekWhence::set).status().code() == StatusCode::not_seekable);
    std::array<std::byte, 1> out{};
    CHECK(f.posix.pread(pair[0], out, 0).status().code() == StatusCode::not_seekable);
    CHECK(f.posix.pwrite(pair[0], bytes("x"), 0).status().code() == StatusCode::not_seekable);
    CHECK(f.posix.console_control_descriptor(pair[0]).code() == StatusCode::not_terminal);
    CHECK(f.posix.close(pair[1]));
    const auto ready = f.posix.ready_events(pair[0], posix::ReadyEvents::none); CHECK(ready);
    CHECK(ready.value().events == posix::ReadyEvents::hangup); // HUP does not need a requested bit.
    CHECK(f.posix.read(pair[0], out).status().code() == StatusCode::io_error);
}
template <bool BigEndian> void actual_pair_checked_copy(Fixture &f)
{
    std::array<u8, 8> bytes{}; usize prepares{}, published{}, failures{};
    const std::array<u64, 6> args{0x1000, posix::o_NONBLOCK | posix::o_CLOEXEC, 0, 0, 0, 0};
    auto prepare = [&](u32 flags) { ++prepares; return f.scheduler.prepare_pty_pair(f.pid, flags); };
    auto publish = [&](sched::PreparedPtyPair &operation) noexcept { operation.publish_checked(); ++published; };
    auto failure = [&](Status) { ++failures; };
    auto call = [&](bool refuse) {
        return user::native_pty::create<BigEndian>(args,
            [](u64 address, usize count) { return address == 0x1000 && count == 8; }, prepare,
            [&](u64, std::span<const u8> output) -> Status {
                CHECK(f.resources().prepared_descriptor_operations == 2);
                CHECK(f.resources().files.open_count() == 3);
                for (usize i = 0; i < (refuse ? 3 : output.size()); ++i) bytes[i] = output[i];
                return refuse ? Status::fault("injected checked copy failure") : Status{};
            }, publish, failure);
    };
    CHECK(call(true).status().code() == StatusCode::fault);
    CHECK(prepares == 1 && !published && !failures);
    CHECK(!f.scheduler.pty_audit().live_pairs && !f.resources().prepared_descriptor_operations);
    CHECK(f.resources().files.open_count() == 3);
    CHECK(call(false)); CHECK(prepares == 2 && published == 1 && !failures);
    std::array<i32, 2> descriptors{};
    for (usize side = 0; side < 2; ++side)
    {
        u32 fd{};
        for (usize i = 0; i < 4; ++i) fd |= static_cast<u32>(bytes[4 * side + i]) << (8 * (BigEndian ? 3 - i : i));
        descriptors[side] = static_cast<i32>(fd);
        CHECK(f.resources().files.entry(descriptors[side])->used);
        CHECK(f.posix.fcntl(descriptors[side], posix::f_GETFD, 0).value() == posix::fd_CLOEXEC);
    }
    CHECK(descriptors[0] != descriptors[1]);
    CHECK(f.posix.write(descriptors[0], ::bytes("wire")).value() == 4);
    std::array<std::byte, 8> output{};
    CHECK(f.posix.read(descriptors[1], output).value() == 4 && output[0] == std::byte{'w'});
}
void checked_read_canonical_hold_and_copy_refusal(Fixture &f)
{
    sched::private_pty::RawPtyTable<> foreign;
    const auto foreign_pair = foreign.create(); CHECK(foreign_pair);
    CHECK(foreign.write(foreign_pair.value().master, bytes("foreign")));
    auto foreign_preview = foreign.prepare_read(foreign_pair.value().slave, 7); CHECK(foreign_preview);
    CHECK(f.scheduler.commit_pty_read(foreign_preview.value()).status().code() == StatusCode::busy);
    CHECK(foreign.readiness(foreign_pair.value().slave).value().buffered == 7);
    CHECK(foreign_preview.value().commit().value().bytes == 7);
    CHECK(foreign.retire(foreign_pair.value().master)); CHECK(foreign.retire(foreign_pair.value().slave));
    const auto pair = f.pair(); const auto master = f.id(pair[0]), slave = f.id(pair[1]);
    CHECK(f.scheduler.retain_description(slave));
    CHECK(f.posix.close(pair[1])); // The checked-read continuation owns the canonical description.
    CHECK(f.scheduler.write_pty_description(master, bytes("private")).value() == 7);
    for (usize prefix = 0; prefix <= 7; ++prefix)
    {
        auto preview = f.scheduler.prepare_pty_read(slave, 32); CHECK(preview);
        CHECK(preview.value().bytes().size() == 7);
        std::array<std::byte, 7> target{};
        for (usize i = 0; i < prefix; ++i) target[i] = preview.value().bytes()[i];
        preview.value().cancel(); // Checked user copy refused at this prefix.
        CHECK(f.scheduler.commit_pty_read(preview.value()).status().code() == StatusCode::busy);
        CHECK(f.scheduler.pty_description_readiness(slave).value().buffered == 7);
    }
    auto preview = f.scheduler.prepare_pty_read(slave, 32); CHECK(preview);
    CHECK(preview.value().bytes()[0] == std::byte{'p'} && preview.value().bytes()[6] == std::byte{'e'});
    CHECK(f.scheduler.commit_pty_read(preview.value()).value() == 7);
    CHECK(f.scheduler.commit_pty_read(preview.value()).status().code() == StatusCode::busy);
    CHECK(f.scheduler.prepare_pty_read(slave, 1).status().code() == StatusCode::would_block);
    CHECK(f.scheduler.release_description(slave));
    CHECK(f.scheduler.prepare_pty_read(slave, 1).status().code() == StatusCode::bad_descriptor);
}
void actual_wait_ticket_read_write_wakes(Fixture &f)
{
    const auto pair = f.pair(); const auto master = f.id(pair[0]), slave = f.id(pair[1]);
    const auto master_handle = f.scheduler.open_description(master)->pty_endpoint;
    const auto slave_handle = f.scheduler.open_description(slave)->pty_endpoint;
    auto pending = [&](sched::WaitTicket ticket) {
        return f.scheduler.find_thread(ticket.owner, ticket.thread)->wait_registration.pending(ticket);
    };
    auto ready = [&](sched::WaitTicket ticket) {
        auto *thread = f.scheduler.find_thread(ticket.owner, ticket.thread);
        CHECK(!pending(ticket)); CHECK(thread->wait_registration.completion() == sched::WaitCompletion::ready);
        CHECK(thread->state == sched::ProcessState::runnable);
        CHECK(!f.scheduler.complete_wait(ticket, sched::WaitCompletion::cancelled));
        CHECK(f.posix.activate(f.pid));
    };
    auto reader = f.scheduler.register_current_wait(sched::ThreadWaitReason::pty_read,
        sched::SchedulerState::pty_wait_key(slave_handle)); CHECK(reader && pending(reader.value()));
    CHECK(f.scheduler.write_pty_description(slave, bytes("wrong-direction")).value() == 15);
    CHECK(pending(reader.value()));
    CHECK(f.scheduler.write_pty_description(master, bytes("r")).value() == 1); ready(reader.value());
    std::array<std::byte, 4096> output{};
    CHECK(f.scheduler.read_pty_description(slave, output).value() == 1);
    CHECK(f.scheduler.read_pty_description(master, output).value() == 15);
    std::array<std::byte, 4096> input{}; input.fill(std::byte{'w'});
    CHECK(f.scheduler.write_pty_description(master, input).value() == input.size());
    auto writer = f.scheduler.register_current_wait(sched::ThreadWaitReason::pty_write,
        sched::SchedulerState::pty_wait_key(master_handle)); CHECK(writer && pending(writer.value()));
    auto refused = f.scheduler.prepare_pty_read(slave, 1); CHECK(refused);
    refused.value().cancel(); CHECK(pending(writer.value()));
    auto copied = f.scheduler.prepare_pty_read(slave, 1); CHECK(copied);
    CHECK(copied.value().bytes()[0] == std::byte{'w'});
    CHECK(f.scheduler.commit_pty_read(copied.value()).value() == 1); ready(writer.value());
    CHECK(f.scheduler.read_pty_description(slave, output).value() == 4095);
    CHECK(f.scheduler.write_pty_description(master, input).value() == input.size());
    writer = f.scheduler.register_current_wait(sched::ThreadWaitReason::pty_write,
        sched::SchedulerState::pty_wait_key(master_handle)); CHECK(writer && pending(writer.value()));
    CHECK(f.scheduler.read_pty_description(slave, output).value() == input.size()); ready(writer.value());
}
void final_ofd_release_wakes_both_sides_and_classes(Fixture &f)
{
    const auto pair = f.pair(); const auto master = f.id(pair[0]);
    const auto master_handle = f.scheduler.open_description(master)->pty_endpoint;
    const auto slave_handle = f.scheduler.open_description(f.id(pair[1]))->pty_endpoint;
    const auto main = f.scheduler.current_tid(0);
    const auto duplicate = f.posix.duplicate(pair[0]); CHECK(duplicate);
    CHECK(f.scheduler.retain_description(master));
    CHECK(f.posix.close(pair[0]));
    std::array<sched::WaitTicket, 4> tickets{};
    usize index{};
    for (const auto handle : {master_handle, slave_handle})
        for (const auto reason : {sched::ThreadWaitReason::pty_read, sched::ThreadWaitReason::pty_write})
        {
            const auto made = f.scheduler.create_thread(f.pid,
                arch::configured_arch_operations().make_user_context({1, 2, 0})); CHECK(made);
            CHECK(f.scheduler.activate_thread(f.pid, made.value()));
            const auto ticket = f.scheduler.register_current_wait(reason, sched::SchedulerState::pty_wait_key(handle));
            CHECK(ticket); tickets[index++] = ticket.value();
        }
    CHECK(f.scheduler.activate_thread(f.pid, main));
    CHECK(f.posix.close(duplicate.value())); // The explicit continuation hold prevents retirement.
    CHECK(f.scheduler.pty_audit().live_endpoints == 2);
    for (const auto ticket : tickets)
        CHECK(f.scheduler.find_thread(ticket.owner, ticket.thread)->wait_registration.pending(ticket));
    CHECK(f.scheduler.release_description(master)); // Final canonical reference owns retirement.
    CHECK(f.scheduler.pty_audit().live_endpoints == 1);
    for (const auto ticket : tickets)
    {
        const auto *thread = f.scheduler.find_thread(ticket.owner, ticket.thread);
        CHECK(!thread->wait_registration.pending(ticket));
        CHECK(thread->wait_registration.completion() == sched::WaitCompletion::ready);
        CHECK(thread->state == sched::ProcessState::runnable);
    }
    CHECK(f.posix.close(pair[1]));
    const auto next = f.pair();
    const auto replacement = f.scheduler.open_description(f.id(next[0]))->pty_endpoint;
    CHECK(replacement.generation != master_handle.generation);
    CHECK(sched::SchedulerState::pty_wait_key(replacement) == sched::SchedulerState::pty_wait_key(master_handle));
    CHECK(replacement != master_handle); // The packed wake key cannot authorize an endpoint identity.
}
int main()
{
    run("prepared-real-reservations-rollback", prepared_lifetime_and_rollback);
    run("canonical-dup-flags-final-reference", canonical_dup_flags_and_final_retirement);
    run("actual-fork-exec-cloexec", actual_fork_and_exec_cloexec);
    run("fd-ofd-provider-exhaustion", descriptor_ofd_and_provider_exhaustion);
    run("token-exhaustion-first-reservation-rollback", token_exhaustion_rolls_back_first_reservation);
    run("scm-rights-retains-one-canonical-pty", scm_rights_keeps_endpoint_owner);
    run("unreachable-unix-cycle-releases-pty", unreachable_unix_cycle_releases_pty_edge);
    run("provider-exclusivity-stat-seek-console", provider_exclusivity_stat_seek_and_controls);
    run("actual-pair-copy-little-endian", actual_pair_checked_copy<false>);
    run("actual-pair-copy-big-endian", actual_pair_checked_copy<true>);
    run("checked-read-canonical-hold-copy-refusal", checked_read_canonical_hold_and_copy_refusal);
    run("actual-wait-ticket-read-write-wakes", actual_wait_ticket_read_write_wakes);
    run("final-ofd-release-wakes-both-sides-classes", final_ofd_release_wakes_both_sides_and_classes);
    std::printf("PRIVATE_PTY_ACTUAL_OFD cases=%zu checks=%zu status=passed\n", cases, checks);
}
