#include "ok/core/shutdown.hpp"
#include <cstdio>
#include <new>
#include <type_traits>

namespace
{
unsigned checks{};
int failed{};
void check(bool condition, const char *label)
{
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL %s\n", label); ++failed; }
}
struct Services
{
    std::array<int, 32> events{};
    unsigned count{}, fail_at{}, preflights{};
    bool supported{true};
    void (*observe)(void *){};
    void *context{};
    ok::Status event(int value)
    {
        events[count++] = value;
        if (observe) observe(context);
        return count == fail_at ? ok::Status::io_error("injected owner failure") : ok::Status{};
    }
    ok::Status preflight(ok::SystemPowerAction)
    {
        ++preflights;
        auto status = event(1);
        return status && !supported ? ok::Status::unsupported("no clean storage adapter") : status;
    }
    ok::Status check_commit(ok::ShutdownOwner) { return event(2); }
    ok::Status check_outer_boundary() { return event(3); }
    ok::Status close_admission() { return event(4); }
    ok::Status stop_producers() { return event(5); }
    ok::Status retire_native() { return event(6); }
    ok::Status drain_resources() { return event(7); }
    ok::Status seal_storage() { return event(8); }
};
struct MissingSeal : Services { void seal_storage() = delete; };
static_assert(!ok::ShutdownServices<MissingSeal>);
using Coordinator = ok::ShutdownCoordinator<Services>;
static_assert(!std::is_copy_constructible_v<Coordinator> && !std::is_move_constructible_v<Coordinator>);
int runtime;
ok::ShutdownOwner owner{&runtime, 7, 1, 1};
ok::ShutdownCaller caller{owner, 1, 1, true, {}};
void bind(Coordinator &coordinator) { check(coordinator.bind_original_init(owner).ok(), "original binding"); }
ok::ShutdownTicket reserve(Coordinator &coordinator, ok::SystemPowerAction action = ok::SystemPowerAction::halt)
{
    auto ticket = coordinator.prepare(caller, action);
    check(ticket.ok(), "prepare accepted");
    return ticket ? ticket.value() : ok::ShutdownTicket{};
}
void authority()
{
    Services service; Coordinator coordinator{service};
    check(!coordinator.prepare(caller, ok::SystemPowerAction::halt), "unbound denial");
    for (unsigned field = 0; field != 4; ++field)
    {
        auto invalid = owner;
        if (field == 0) invalid.runtime = nullptr;
        if (field == 1) invalid.incarnation = 0;
        if (field == 2) invalid.process = 2;
        if (field == 3) invalid.leader = 0;
        check(!coordinator.bind_original_init(invalid), "invalid binding denial");
    }
    bind(coordinator);
    check(!coordinator.bind_original_init(owner), "no rebinding");
    for (unsigned field = 0; field != 14; ++field)
    {
        auto invalid = caller;
        if (field == 0) invalid.owner.runtime = &service;
        if (field == 1) ++invalid.owner.incarnation;
        if (field == 2) ++invalid.owner.process;
        if (field == 3) ++invalid.owner.leader;
        if (field == 4) ++invalid.thread;
        if (field == 5) invalid.live_threads = 0;
        if (field == 6) invalid.live_threads = 2;
        if (field == 7) invalid.live = false;
        if (field >= 8) invalid.ids[field - 8] = 1000;
        check(!coordinator.prepare(invalid, ok::SystemPowerAction::halt), "caller authority denial");
    }
    check(service.count == 0, "unauthorized callers cause no callbacks");
    for (auto invalid : {ok::SystemPowerAction::none, static_cast<ok::SystemPowerAction>(255)})
        check(!coordinator.prepare(caller, invalid), "invalid action denial");
    check(service.count == 0, "invalid actions cause no callbacks");
    auto ticket = reserve(coordinator);
    check(coordinator.prepare(caller, ok::SystemPowerAction::halt).value() == ticket, "prepare idempotence");
    check(!coordinator.prepare(caller, ok::SystemPowerAction::reboot), "action conflict");
    check(service.count == 1, "prepare callback once");
    for (unsigned field = 0; field != 7; ++field)
    {
        auto invalid = ticket;
        if (field == 0) invalid.coordinator = &service;
        if (field == 1) ++invalid.generation;
        if (field == 2) invalid.action = ok::SystemPowerAction::reboot;
        if (field == 3) ++invalid.owner.incarnation;
        if (field == 4) ++invalid.owner.process;
        if (field == 5) ++invalid.owner.leader;
        if (field == 6) invalid.owner.runtime = nullptr;
        check(!coordinator.commit(caller, invalid), "foreign or stale ticket rejected");
    }
    for (unsigned id = 0; id != 6; ++id)
    {
        auto invalid = caller; invalid.ids[id] = 1000;
        check(!coordinator.commit(invalid, ticket), "commit reauthenticates every root ID");
    }
    check(service.count == 1, "invalid commits cause no callbacks");
    check(!coordinator.advance() && !coordinator.consume_action(), "no advance before commit");
    check(coordinator.commit(caller, ticket).ok(), "valid commit");
    check(coordinator.commit(caller, ticket).ok() && service.count == 2, "queued commit idempotent");
    check(coordinator.must_hold() && coordinator.ready_action() == ok::SystemPowerAction::none,
          "commit queues and holds without publishing power");
}
void lifetime()
{
    alignas(Coordinator) std::byte bytes[sizeof(Coordinator)];
    Services first;
    auto *old = ::new (static_cast<void *>(bytes)) Coordinator(first);
    bind(*old); auto stale = reserve(*old); old->~Coordinator();
    Services second;
    auto *fresh = ::new (static_cast<void *>(bytes)) Coordinator(second);
    auto next = owner; ++next.incarnation;
    check(fresh->bind_original_init(next).ok(), "new runtime incarnation binds");
    auto next_caller = caller; next_caller.owner = next;
    auto current = fresh->prepare(next_caller, ok::SystemPowerAction::halt);
    check(current.ok(), "new incarnation prepares");
    check(!fresh->commit(next_caller, stale) && second.count == 1,
          "same-address previous-lifetime ticket denied");
    fresh->~Coordinator();
}
void unsupported()
{
    for (auto action : {ok::SystemPowerAction::halt, ok::SystemPowerAction::reboot, ok::SystemPowerAction::poweroff})
    {
        Services service; service.supported = false; Coordinator coordinator{service}; bind(coordinator);
        check(coordinator.prepare(caller, action).status().code() == ok::StatusCode::unsupported,
              "unsupported action preserves services");
        check(coordinator.phase() == ok::ShutdownPhase::idle && !coordinator.must_hold() &&
              !coordinator.barrier_started() && service.count == 1, "no mutation on preflight refusal");
        service.supported = true; auto ticket = reserve(coordinator, action);
        check(coordinator.commit(caller, ticket).ok(), "supported successor commits");
        check(service.count == 3, "commit does not retire native caller");
        check(coordinator.advance().ok(), "ordered cleanup succeeds");
        check(coordinator.ready_action() == action && coordinator.barrier_started(), "ready only after seal");
        auto consumed = coordinator.consume_action();
        check(consumed && consumed.value() == action, "action consumed once");
        const auto count = service.count;
        check(!coordinator.consume_action() && !coordinator.advance() &&
              coordinator.ready_action() == ok::SystemPowerAction::none && service.count == count,
              "no repeated cleanup or platform action");
        const std::array<int, 12> expected{1,1,2,3,1,4,5,6,7,8,3,0};
        check(service.count == 11, "bounded callback count");
        for (unsigned i = 0; i != service.count; ++i) check(service.events[i] == expected[i], "exact cleanup order");
    }
}
struct Reentry { Coordinator &coordinator; ok::ShutdownTicket ticket; };
void reenter(void *value)
{
    auto &state = *static_cast<Reentry *>(value);
    check(state.coordinator.prepare(caller, ok::SystemPowerAction::halt).status().code() == ok::StatusCode::busy,
          "callback cannot prepare");
    check(state.coordinator.commit(caller, state.ticket).code() == ok::StatusCode::busy, "callback cannot commit");
    check(state.coordinator.advance().code() == ok::StatusCode::busy, "callback cannot advance");
    check(state.coordinator.consume_action().status().code() == ok::StatusCode::busy, "callback cannot consume");
    check(state.coordinator.bind_original_init(owner).code() == ok::StatusCode::busy, "callback cannot rebind");
}
void failures()
{
    const std::array<ok::ShutdownPhase, 8> phases{ok::ShutdownPhase::checking_boundary,
        ok::ShutdownPhase::revalidating, ok::ShutdownPhase::closing_admission,
        ok::ShutdownPhase::stopping_producers, ok::ShutdownPhase::retiring_native,
        ok::ShutdownPhase::draining_resources, ok::ShutdownPhase::sealing_storage,
        ok::ShutdownPhase::ready};
    for (unsigned fail = 3; fail != 11; ++fail)
    {
        Services service; Coordinator coordinator{service}; bind(coordinator);
        auto ticket = reserve(coordinator);
        check(coordinator.commit(caller, ticket).ok(), "failure fixture queued");
        service.fail_at = fail;
        const auto advanced = coordinator.advance();
        if (fail != 10) check(!advanced, "cleanup callback failure returned");
        else { check(advanced.ok(), "seal precedes consumption boundary"); check(!coordinator.consume_action(), "final boundary failure"); }
        check(coordinator.phase() == ok::ShutdownPhase::failed && coordinator.failed_at() == phases[fail - 3],
              "failure records exact terminal phase");
        check(coordinator.must_hold() && coordinator.ready_action() == ok::SystemPowerAction::none,
              "failure never publishes power or restarts workloads");
        const auto count = service.count;
        check(coordinator.advance().code() == ok::StatusCode::io_error &&
              coordinator.commit(caller, ticket).code() == ok::StatusCode::io_error &&
              coordinator.prepare(caller, ok::SystemPowerAction::halt).status().code() == ok::StatusCode::io_error &&
              coordinator.consume_action().status().code() == ok::StatusCode::io_error && service.count == count,
              "first owner failure sticky and never retried");
    }
    for (unsigned fail : {1u,2u})
    {
        Services service; Coordinator coordinator{service}; bind(coordinator); service.fail_at = fail;
        if (fail == 1)
        {
            check(!coordinator.prepare(caller, ok::SystemPowerAction::halt), "preflight failure");
            check(coordinator.phase() == ok::ShutdownPhase::idle && !coordinator.must_hold(), "preflight refusal is nondestructive");
        }
        else
        {
            auto ticket = reserve(coordinator); check(!coordinator.commit(caller, ticket), "read-only commit refusal");
            check(coordinator.phase() == ok::ShutdownPhase::prepared && !coordinator.must_hold(), "commit refusal does not retire owners");
        }
    }
    Services service; Coordinator coordinator{service}; bind(coordinator);
    Reentry state{coordinator,{}}; service.context = &state; service.observe = reenter;
    state.ticket = reserve(coordinator);
    check(coordinator.commit(caller, state.ticket).ok() && coordinator.advance().ok() && coordinator.consume_action().ok(),
          "reentrant callbacks cannot corrupt normal completion");
}
}
int main()
{
    authority(); lifetime(); unsupported(); failures();
    if (failed) return 1;
    std::printf("SHUTDOWN_COORDINATOR_MODEL_PASS checks=%u native_retirement=0 storage_seal=0 platform_execution=0\n", checks);
}
