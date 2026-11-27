// The user CPU hotplug service (include/ok/smp/hotplug.hpp) over a model
// backend: record lifecycle and consume-once results, no-op and coalesced
// requests, refusals, orphaned and abandoned records, result mapping, the
// failure reports, a wedged CPU and its recovery, service order, the
// kstrtobool parser, and
// (built with OK_HOSTED_LOCK_MODEL) host threads filing and abandoning
// requests while another serves them. Model scope: no CPU stops or starts.
#include "ok/smp/hotplug.hpp"

#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace ok;
using namespace ok::smp;
using hotplug::RecordState;
static usize checks{};
#define CHECK(expression) do { ++checks; if (!static_cast<bool>(expression)) { \
    std::fprintf(stderr, "FAIL %d %s\n", __LINE__, #expression); std::abort(); } } while (false)

namespace
{
// What the model's stop or restart does.
enum class Outcome : u8 { succeed, busy, timeout, fault, invalid, start_failed };

struct Model
{
    std::array<CpuState, max_cpus> state{};
    std::array<bool, max_cpus> parks_late{};
    Outcome outcome{Outcome::succeed};
    u32 knob{}; // The smp_hotplug_test CPU, 0 for none.
    bool shutdown{};
    std::vector<sched::WaitTicket> waiting{}, completed{};
    usize stops{}, restarts{};
    u64 clock{};
    // The reason a failed start names (start_failed), and the latest failure's.
    std::string_view start_reason{"tsc-loose"}, reason{};
    // Runs inside stop/restart, while the record is running (abandon races).
    void (*during)(Model &){};
    void *service{};
};

struct Backend
{
    Model *model;
    [[nodiscard]] bool online(CpuId cpu) const { return model->state[cpu] == CpuState::online; }
    [[nodiscard]] hotplug::Refusal refusal(CpuId cpu) const
    {
        if (cpu == model->knob || model->shutdown) return hotplug::Refusal::busy;
        switch (model->state[cpu])
        {
        case CpuState::stopping:
        case CpuState::starting:
        case CpuState::boot: return hotplug::Refusal::busy;
        case CpuState::halted: return hotplug::Refusal::io_error;
        default: return hotplug::Refusal::none;
        }
    }
    bool owner_waiting(u64, u64, sched::WaitTicket ticket)
    {
        for (const auto &waiting : model->waiting)
            if (waiting == ticket) return true;
        return false;
    }
    Status act(CpuId cpu, bool want)
    {
        ++(want ? model->restarts : model->stops);
        model->clock += 1500;
        if (model->during) model->during(*model);
        model->reason = {};
        switch (model->outcome)
        {
        case Outcome::succeed: model->state[cpu] = want ? CpuState::online : CpuState::offline; return {};
        case Outcome::busy: model->reason = "mask-busy"; return Status::busy("a process allows no other online CPU");
        case Outcome::timeout:
            model->state[cpu] = CpuState::stopping;
            model->reason = "park-timeout";
            return Status::io_error("did not park");
        case Outcome::fault:
            model->state[cpu] = CpuState::offline;
            model->reason = "leave-fault";
            return Status::fault("left with a record");
        case Outcome::invalid: model->reason = "not-online"; return Status::invalid_argument("CPU is not online");
        case Outcome::start_failed:
            // A start that gave up leaves the CPU parked and offline.
            model->state[cpu] = CpuState::offline;
            model->reason = model->start_reason;
            return Status::io_error("x86 secondary TSC was not synchronized");
        }
        return {};
    }
    Status stop(CpuId cpu) { return act(cpu, false); }
    Status restart(CpuId cpu) { return act(cpu, true); }
    void complete(sched::WaitTicket ticket)
    {
        model->completed.push_back(ticket);
        std::erase(model->waiting, ticket);
    }
    [[nodiscard]] std::string_view failure() const { return model->reason; }
    bool recover(CpuId cpu)
    {
        if (!model->parks_late[cpu] || model->state[cpu] != CpuState::stopping) return false;
        model->state[cpu] = CpuState::offline;
        return true;
    }
    [[nodiscard]] u64 now_ns() const { return model->clock; }
};
static_assert(hotplug::Backend<Backend>);
using Service = hotplug::HotplugService<Backend>;
// The kernel's instance is constant-initialized.
constinit Service constant_service{};

Model fresh()
{
    Model model{};
    model.state.fill(CpuState::online);
    return model;
}

sched::WaitTicket ticket(u64 tid, u64 generation) { return {.owner = 10, .thread = tid, .generation = generation}; }
hotplug::Request request(CpuId cpu, bool want, u64 tid, u64 generation, i64 count = 2)
{
    return {.cpu = cpu, .want = want, .pid = 10, .tid = tid, .ticket = ticket(tid, generation), .count = count};
}

// File, serve, complete, consume once.
void lifecycle()
{
    auto model = fresh();
    Backend backend{&model};
    auto service = std::make_unique<Service>();
    const auto first = request(1, false, 21, 1);
    model.waiting.push_back(first.ticket);
    const auto filed = service->submit(backend, 5, first);
    CHECK(filed.armed && service->pending() == 1 && service->record(5).state == RecordState::armed);
    CHECK(!service->consume(5, first.ticket) && !service->finished(5));
    CHECK(service->pass(backend) && model.stops == 1 && model.state[1] == CpuState::offline);
    CHECK(service->pending() == 0 && service->record(5).state == RecordState::done && service->finished(5));
    CHECK(model.completed.size() == 1 && model.completed[0] == first.ticket);
    // Only the current ticket consumes it, once.
    CHECK(!service->consume(5, ticket(21, 2)) && !service->consume(4, first.ticket));
    const auto result = service->consume(5, first.ticket);
    CHECK(result && *result == 2 && !service->consume(5, first.ticket) && !service->finished(5));
    CHECK(service->record(5).state == RecordState::free && !service->pass(backend));
    // And back online.
    const auto again = request(1, true, 21, 2, 1);
    model.waiting.push_back(again.ticket);
    CHECK(service->submit(backend, 5, again).armed && service->pass(backend) && model.restarts == 1);
    CHECK(*service->consume(5, again.ticket) == 1 && model.state[1] == CpuState::online);
    const auto statistics = service->statistics();
    CHECK(statistics.requests == 2 && statistics.offline == 1 && statistics.online == 1 && statistics.max_us == 1);
}

// Linux returns success for a write that changes nothing, at submit or when
// an earlier request already made the change (coalesced).
void no_ops()
{
    auto model = fresh();
    Backend backend{&model};
    auto service = std::make_unique<Service>();
    const auto idle = service->submit(backend, 1, request(2, true, 31, 1));
    CHECK(!idle.armed && idle.result == 2 && service->pending() == 0);
    const auto one = request(2, false, 31, 2), two = request(2, false, 32, 1, 3);
    model.waiting = {one.ticket, two.ticket};
    CHECK(service->submit(backend, 1, one).armed && service->submit(backend, 2, two).armed && service->pending() == 2);
    CHECK(service->pass(backend) && service->pass(backend) && !service->pass(backend));
    CHECK(model.stops == 1 && *service->consume(1, one.ticket) == 2 && *service->consume(2, two.ticket) == 3);
    CHECK(service->statistics().noop == 2 && service->statistics().offline == 1);
}

// Refusals complete at submit; the record is untouched.
void refusals()
{
    auto model = fresh();
    Backend backend{&model};
    auto service = std::make_unique<Service>();
    model.knob = 3;
    CHECK(service->submit(backend, 0, request(3, false, 41, 1)).result == -hotplug::errno_busy);
    model.knob = 0;
    model.shutdown = true;
    CHECK(service->submit(backend, 0, request(3, false, 41, 2)).result == -hotplug::errno_busy);
    model.shutdown = false;
    for (const auto state : {CpuState::stopping, CpuState::starting, CpuState::boot})
    {
        model.state[3] = state;
        CHECK(service->submit(backend, 0, request(3, true, 41, 3)).result == -hotplug::errno_busy);
    }
    model.state[3] = CpuState::halted;
    CHECK(service->submit(backend, 0, request(3, true, 41, 4)).result == -hotplug::errno_io);
    CHECK(service->pending() == 0 && service->record(0).state == RecordState::free);
    // Only the EIO refusal is a failure, reported once.
    const auto halted = service->take_failure();
    CHECK(halted && halted->number == 1 && halted->cpu == 3 && halted->want && halted->error == hotplug::errno_io);
    CHECK(halted->reason == "halted" && halted->us == 0 && !service->take_failure());
    CHECK(service->submit(backend, hotplug::record_capacity, request(3, true, 41, 5)).result == -hotplug::errno_invalid);
    const auto statistics = service->statistics();
    CHECK(statistics.busy == 5 && statistics.failures == 1 && statistics.requests == 6);
}

// The service maps the CPU state first, then the Status.
void results()
{
    struct Case { Outcome outcome; i64 result; };
    for (const auto [outcome, expected] : {Case{Outcome::busy, -hotplug::errno_busy}, Case{Outcome::fault, -hotplug::errno_io},
                                           Case{Outcome::invalid, -hotplug::errno_io}})
    {
        auto model = fresh();
        Backend backend{&model};
        auto service = std::make_unique<Service>();
        model.outcome = outcome;
        const auto filed = request(1, false, 51, 1);
        model.waiting.push_back(filed.ticket);
        CHECK(service->submit(backend, 9, filed).armed && service->pass(backend));
        CHECK(*service->consume(9, filed.ticket) == expected && service->wedged() == 0);
        // EBUSY is no failure; each EIO names the backend's reason.
        const auto failure = service->take_failure();
        CHECK(outcome == Outcome::busy ? !failure : failure && failure->reason == model.reason && failure->us == 1);
    }
}

// Every counted failure is reported once as OK_SMP_HOTPLUG_FAIL would print
// it: numbered, with the CPU, the value, EIO, the reason and the duration;
// the latest failure_capacity are kept until a report takes them.
void failure_reports()
{
    auto model = fresh();
    Backend backend{&model};
    auto service = std::make_unique<Service>();
    model.state[1] = CpuState::offline;
    model.outcome = Outcome::start_failed;
    const auto online = request(1, true, 61, 1);
    model.waiting.push_back(online.ticket);
    CHECK(service->submit(backend, 4, online).armed && service->pass(backend));
    CHECK(*service->consume(4, online.ticket) == -hotplug::errno_io && model.state[1] == CpuState::offline);
    auto failure = service->take_failure();
    CHECK(failure && failure->number == 1 && failure->cpu == 1 && failure->want && failure->error == hotplug::errno_io);
    CHECK(failure->reason == "tsc-loose" && failure->us == 1 && !service->take_failure());
    // A later "1" restarts it: the failed start left it offline, not wedged.
    model.outcome = Outcome::succeed;
    const auto again = request(1, true, 61, 2);
    model.waiting.push_back(again.ticket);
    CHECK(service->submit(backend, 4, again).armed && service->pass(backend) && *service->consume(4, again.ticket) == 2);
    CHECK(model.state[1] == CpuState::online && service->wedged() == 0 && !service->take_failure());
    // A backend that names no reason.
    model.outcome = Outcome::start_failed;
    model.start_reason = {};
    model.state[1] = CpuState::offline;
    const auto unnamed = request(1, true, 61, 3);
    model.waiting.push_back(unnamed.ticket);
    CHECK(service->submit(backend, 4, unnamed).armed && service->pass(backend));
    CHECK(*service->consume(4, unnamed.ticket) == -hotplug::errno_io);
    failure = service->take_failure();
    CHECK(failure && failure->number == 2 && failure->reason == "unknown");
    // More failures than the ring holds before a report: the oldest are
    // skipped, the numbers show the gap, and none is reported twice.
    model.start_reason = "tsc-silent";
    constexpr u64 burst = hotplug::failure_capacity + 3;
    for (u64 index = 0; index < burst; ++index)
    {
        const auto failing = request(1, true, 61, 4 + index);
        model.waiting.push_back(failing.ticket);
        CHECK(service->submit(backend, 4, failing).armed && service->pass(backend));
        CHECK(*service->consume(4, failing.ticket) == -hotplug::errno_io);
    }
    for (u64 number = 2 + burst - hotplug::failure_capacity + 1; number <= 2 + burst; ++number)
    {
        failure = service->take_failure();
        CHECK(failure && failure->number == number && failure->reason == "tsc-silent");
    }
    CHECK(!service->take_failure() && service->statistics().failures == 2 + burst);
}

// A writer that no longer waits (killed, its wait ended): freed, no completion.
void orphaned()
{
    auto model = fresh();
    Backend backend{&model};
    auto service = std::make_unique<Service>();
    const auto filed = request(1, false, 61, 1);
    CHECK(service->submit(backend, 7, filed).armed);
    CHECK(service->pass(backend) && model.stops == 0 && model.completed.empty());
    CHECK(service->record(7).state == RecordState::free && service->pending() == 0 && service->statistics().orphaned == 1);
}

// Teardown while armed frees the record; while running moves it to the spare,
// abandoned, and the service frees it and completes nothing. The lease slot
// is free at once: a new thread that received it files its own request.
Service *racing{};
void abandon_running(Model &) { racing->abandon(8, 10, 71); }
hotplug::Submission reused{};
void abandon_then_reuse(Model &model)
{
    racing->abandon(8, 10, 71);
    CHECK(racing->record(8).state == RecordState::free && racing->record(Service::spare).state == RecordState::abandoned);
    // Another process's thread now holds lease slot 8 and writes another CPU.
    const auto next = request(2, false, 99, 1);
    model.waiting.push_back(next.ticket);
    reused = racing->submit(Backend{&model}, 8, next);
}
void abandoned()
{
    auto model = fresh();
    Backend backend{&model};
    auto service = std::make_unique<Service>();
    const auto armed = request(1, false, 71, 1);
    CHECK(service->submit(backend, 8, armed).armed && service->pending() == 1);
    service->abandon(8, 10, 72); // Another thread's teardown names a different owner.
    CHECK(service->pending() == 1);
    service->abandon(8, 10, 71);
    CHECK(service->pending() == 0 && service->record(8).state == RecordState::free && !service->pass(backend));
    racing = service.get();
    model.during = abandon_running;
    const auto running = request(1, false, 71, 2);
    model.waiting.push_back(running.ticket);
    CHECK(service->submit(backend, 8, running).armed && service->pass(backend) && model.stops == 1);
    CHECK(model.completed.empty() && service->record(8).state == RecordState::free && !service->finished(8));
    CHECK(service->record(Service::spare).state == RecordState::free && service->statistics().abandoned == 1);
    // A result nobody consumed is dropped by teardown too.
    model.during = nullptr;
    const auto done = request(1, true, 71, 3);
    model.waiting.push_back(done.ticket);
    CHECK(service->submit(backend, 8, done).armed && service->pass(backend) && service->finished(8));
    service->abandon(8, 10, 71);
    CHECK(service->record(8).state == RecordState::free && !service->finished(8));
    // A stale result is replaced by the slot's next request.
    const auto stale = request(1, false, 71, 4), next = request(1, true, 71, 5);
    model.waiting = {stale.ticket};
    CHECK(service->submit(backend, 8, stale).armed && service->pass(backend) && service->finished(8));
    model.waiting = {next.ticket};
    CHECK(service->submit(backend, 8, next).armed && !service->finished(8) && service->pending() == 1);
    CHECK(!service->consume(8, stale.ticket) && service->pass(backend) && *service->consume(8, next.ticket) == 2);
    // Exec keeps the leader's lease and wait ticket for the new image: its
    // unconsumed result goes with the old image and never reaches the new one.
    const auto leader = request(1, false, 71, 6);
    model.waiting = {leader.ticket};
    CHECK(service->submit(backend, 8, leader).armed && service->pass(backend) && service->finished(8));
    service->abandon(8, 10, 71);
    CHECK(!service->finished(8) && !service->consume(8, leader.ticket));
    CHECK(service->statistics().dropped_waiting == 1 && service->statistics().dropped_done == 2);
    CHECK(service->statistics().abandoned == 1);
}

// The writer leaves while its request runs and a new thread receives its lease
// slot: the new request is armed, not refused, and served after the first.
void reused_slot()
{
    auto model = fresh();
    Backend backend{&model};
    auto service = std::make_unique<Service>();
    racing = service.get();
    model.during = abandon_then_reuse;
    const auto first = request(1, false, 71, 1);
    model.waiting.push_back(first.ticket);
    CHECK(service->submit(backend, 8, first).armed && service->pass(backend));
    CHECK(reused.armed && service->pending() == 1 && service->record(8).state == RecordState::armed);
    CHECK(service->record(Service::spare).state == RecordState::free && model.completed.empty());
    model.during = nullptr;
    CHECK(service->pass(backend) && model.state[2] == CpuState::offline && model.completed.size() == 1);
    CHECK(*service->consume(8, ticket(99, 1)) == 2 && service->statistics().abandoned == 1);
    CHECK(service->statistics().busy == 0 && service->record(8).state == RecordState::free);
}

// A stop that timed out: EIO, the CPU reads offline and refuses writes until
// it parks after all; then a "1" restarts it.
void wedged()
{
    auto model = fresh();
    Backend backend{&model};
    auto service = std::make_unique<Service>();
    model.outcome = Outcome::timeout;
    const auto filed = request(2, false, 81, 1);
    model.waiting.push_back(filed.ticket);
    CHECK(service->submit(backend, 3, filed).armed && service->pass(backend));
    CHECK(*service->consume(3, filed.ticket) == -hotplug::errno_io && service->wedged() == 0b100);
    const auto timeout = service->take_failure();
    CHECK(timeout && !timeout->want && timeout->cpu == 2 && timeout->reason == "park-timeout");
    CHECK(!backend.online(2) && service->submit(backend, 3, request(2, true, 81, 2)).result == -hotplug::errno_busy);
    CHECK(!service->pass(backend) && service->wedged() == 0b100); // Still stopping.
    model.parks_late[2] = true;
    CHECK(!service->pass(backend) && service->wedged() == 0 && model.state[2] == CpuState::offline);
    model.outcome = Outcome::succeed;
    const auto restart = request(2, true, 81, 3);
    model.waiting.push_back(restart.ticket);
    CHECK(service->submit(backend, 3, restart).armed && service->pass(backend) && *service->consume(3, restart.ticket) == 2);
    const auto statistics = service->statistics();
    CHECK(statistics.wedged == 1 && statistics.failures == 1 && statistics.busy == 1 && model.state[2] == CpuState::online);
}

// The oldest armed request goes first, whatever its slot.
void order()
{
    auto model = fresh();
    Backend backend{&model};
    auto service = std::make_unique<Service>();
    const auto late = request(1, false, 91, 1), early = request(2, false, 92, 1);
    model.waiting = {late.ticket, early.ticket};
    CHECK(service->submit(backend, 100, early).armed && service->submit(backend, 3, late).armed);
    CHECK(service->pass(backend) && service->finished(100) && !service->finished(3));
    CHECK(service->pass(backend) && service->finished(3));
}

void parser()
{
    using hotplug::parse_bool;
    auto parse = [](std::string_view text) { return parse_bool({text.data(), text.size()}); };
    for (const auto yes : {"1", "1\n", "y", "Y", "t", "T", "on", "On", "oN\n", "yes", "true"}) CHECK(parse(yes) == true);
    for (const auto no : {"0", "0\n", "n", "N", "f", "F", "of", "off", "OFF\n", "no", "false"}) CHECK(parse(no) == false);
    for (const auto bad : {"", "x", "2", "o", "ox", " 1", "\n", "-1"}) CHECK(!parse(bad));
}

// Host threads stand in for CPUs: writers file, consume and sometimes leave
// while one CPU serves. Every request completes or is dropped exactly once.
void race()
{
    if constexpr (!concurrent_locks) return;
    struct Shared
    {
        std::atomic<bool> state[max_cpus]{};
        std::atomic<usize> completions{}, stops{};
    };
    static Shared shared{};
    struct RaceBackend
    {
        [[nodiscard]] bool online(CpuId cpu) const { return shared.state[cpu].load(); }
        [[nodiscard]] hotplug::Refusal refusal(CpuId) const { return hotplug::Refusal::none; }
        bool owner_waiting(u64, u64, sched::WaitTicket) { return true; }
        Status stop(CpuId cpu) { shared.stops.fetch_add(1); shared.state[cpu].store(false); return {}; }
        Status restart(CpuId cpu) { shared.state[cpu].store(true); return {}; }
        void complete(sched::WaitTicket) { shared.completions.fetch_add(1); }
        [[nodiscard]] std::string_view failure() const { return {}; }
        bool recover(CpuId) { return false; }
        [[nodiscard]] u64 now_ns() const { return 0; }
    };
    static hotplug::HotplugService<RaceBackend> service{};
    for (auto &state : shared.state) state.store(true);
    constexpr usize writers = 3, rounds = 2000;
    std::atomic<usize> finished_writers{}, consumed{}, left{};
    std::vector<std::thread> threads;
    threads.emplace_back([&] {
        const ScopedHostedCpu cpu(0);
        RaceBackend backend{};
        while (finished_writers.load() != writers || service.pending() != 0) static_cast<void>(service.pass(backend));
    });
    for (usize writer = 0; writer < writers; ++writer)
        threads.emplace_back([&, writer] {
            const ScopedHostedCpu cpu(static_cast<CpuId>(1 + writer));
            RaceBackend backend{};
            for (usize round = 0; round < rounds; ++round)
            {
                const auto filed = hotplug::Request{.cpu = static_cast<CpuId>(1 + round % 3), .want = round % 2 == 0,
                                                    .pid = 1, .tid = 100 + writer,
                                                    .ticket = {1, 100 + writer, round + 1}, .count = 1};
                const auto submitted = service.submit(backend, writer, filed);
                if (!submitted.armed) continue;
                if (round % 7 == 0)
                {
                    service.abandon(writer, 1, 100 + writer);
                    left.fetch_add(1);
                    // Wait until the service let go of the slot.
                    while (service.record(writer).state != RecordState::free) std::this_thread::yield();
                    continue;
                }
                while (!service.consume(writer, filed.ticket)) std::this_thread::yield();
                consumed.fetch_add(1);
            }
            finished_writers.fetch_add(1);
        });
    for (auto &thread : threads) thread.join();
    const auto statistics = service.statistics();
    CHECK(service.pending() == 0 && consumed.load() + left.load() > 0);
    CHECK(shared.completions.load() >= consumed.load() && shared.completions.load() <= consumed.load() + left.load());
    for (usize writer = 0; writer < writers; ++writer) CHECK(service.record(writer).state == RecordState::free);
    CHECK(statistics.requests == writers * rounds && lock_debug_depth(0) == 0);
}
} // namespace

int main()
{
    lifecycle();
    no_ops();
    refusals();
    results();
    failure_reports();
    orphaned();
    abandoned();
    reused_slot();
    wedged();
    order();
    parser();
    race();
    CHECK(constant_service.pending() == 0);
    std::printf("CPU_HOTPLUG_SERVICE_MODEL_PASS records=%zu concurrent=%d constinit=1 scope=model\n",
                hotplug::record_capacity, concurrent_locks ? 1 : 0);
    std::printf("checks=%zu\n", checks);
}
