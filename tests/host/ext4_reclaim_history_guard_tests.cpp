// Compile with -fno-access-control: private mutations are hosted integrity models.
// Reuse the qualified physical fixtures, planners and independent golden helpers.
// The renamed legacy entry point also compiles every compatibility API unchanged.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main v33_composed_compatibility_main
#include "ext4_reclaim_composed_history_tests.cpp"
#undef main
#pragma GCC diagnostic pop
#include "ext4_mounted_support.hpp"
#include "ok/fs/ext4/mount.hpp"
#include <cstring>
#include <new>

namespace v33
{
using Session = Registry::Session;
using Budget = Ext4ReclaimRouteBudget<Session>;
using ActualCapture = Ext4OrphanReclaimCapture<2>;
usize checks{}, cases{}, refusals{}, finish_failures{}, transport_priority{}, integrity{},
    decoy_controls{}, lifetime_controls{}, exact_callbacks{}, binding_successes{}, capture_successes{},
    history_successes{}, composed_successes{}, mounted_successes{}, mounted_failures{}, golden_checks{};
#undef CHECK
#define CHECK(expression) do { ++v33::checks; if (!(expression)) { \
    std::fprintf(stderr, "history Guard model:%d: %s\n", __LINE__, #expression); std::abort(); \
} } while (false)

enum class Family : usize
{
    bootstrap, wal, overlay, capture_early, table, directory, capture_compare, binding_compare,
    history_ordinary, history_capture, history_terminal,
    composed_extension, composed_capture, composed_plan, composed_ordinary, composed_terminal, count
};
struct Request
{
    u64 first{};
    usize bytes{};
    bool operator==(const Request &) const = default;
};
struct Audit
{
    usize begins{}, finishes{}, successes{}, failures{};
    bool destroyed{};
    std::vector<Request> requests{};
    std::array<usize, usize(Family::count)> firsts{};
};
enum class Failure { refuse, media, finish, media_finish, exhausted };

// This Guard has no stored life in any production object. Its callback checks
// the original registry tuple and each independently paused successful counter.
struct Guard final
{
    Guard(Harness &owner, Audit &audit, Budget *budget = nullptr)
        : owner(owner), audit(audit), budget(budget) {}
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;
    Guard(Guard &&) = delete;
    Guard &operator=(Guard &&) = delete;
    ~Guard() { CHECK(!pending); audit.destroyed = true; }
    Harness &owner;
    Audit &audit;
    Budget *budget{};
    std::vector<const usize *> counters{};
    std::vector<usize> paused{};
    std::function<Family(u64)> family{};
    std::function<bool(Family)> select{};
    std::function<void()> change{};
    Failure failure{Failure::media};
    usize cut{~usize{0}}, mutate{~usize{0}}, finish_fault{~usize{0}};
    bool selected{}, pending{};
    block::SessionReport previous{};
    usize physical_before{};

    Status begin_read(Session &session, u64 first, usize bytes)
    {
        CHECK(!audit.destroyed && !pending && &session == &owner.session);
        CHECK(!(first % 2) && first < 65536 && bytes == 1024);
        ++audit.begins; audit.requests.push_back({first, bytes});
        const auto current_family = family ? family(first) : Family::bootstrap;
        auto &first_seen = audit.firsts[usize(current_family)];
        if (!first_seen) first_seen = audit.begins;
        if (!selected && select && select(current_family)) { selected = true; cut = audit.begins; }
        if (audit.begins == cut)
        {
            if (failure == Failure::refuse) return Status::busy("history Guard refused begin");
            if (failure == Failure::exhausted) return Status::overflow("history Guard credit exhausted before dispatch");
            if (failure == Failure::media || failure == Failure::media_finish)
                owner.disk.fail_at = owner.disk.reads + 1;
            if (failure == Failure::finish || failure == Failure::media_finish) finish_fault = audit.begins;
        }
        const auto report = owner.session.report(); CHECK(report);
        previous = report.value(); physical_before = owner.disk.reads;
        CHECK(previous.state == block::DeviceState::owned && !previous.in_flight);
        CHECK(previous.reads == previous.completed_reads && !previous.pending_writes);
        paused.clear(); for (const auto *counter : counters) paused.push_back(*counter);
        if (budget) { const auto status = budget->begin_read(session, first, bytes); if (!status) return status; }
        pending = true;
        return {};
    }
    void in_callback()
    {
        CHECK(pending && !audit.destroyed);
        auto expected = previous; ++expected.reads; expected.in_flight = true;
        CHECK(reclaim_history::same_session(expected, owner.session.report().value()));
        CHECK(owner.disk.reads == physical_before + 1);
        for (usize at = 0; at < counters.size(); ++at) CHECK(*counters[at] == paused[at]);
        if (budget) CHECK(budget->in_flight_exact(owner.session));
        ++exact_callbacks;
    }
    Status finish_read(Session &session, Status transport)
    {
        CHECK(pending && !audit.destroyed && &session == &owner.session);
        pending = false; ++audit.finishes;
        const auto actual = owner.session.report(); CHECK(actual);
        CHECK(!actual.value().in_flight && actual.value().reads == previous.reads + 1);
        CHECK(actual.value().completed_reads == previous.completed_reads + usize(bool(transport)));
        CHECK(owner.disk.reads == physical_before + 1);
        if (transport) ++audit.successes; else ++audit.failures;
        for (usize at = 0; at < counters.size(); ++at) CHECK(*counters[at] == paused[at]);
        if (audit.finishes == mutate && change) change();
        Status result;
        if (budget) result = budget->finish_read(session, transport);
        if (audit.finishes == finish_fault) return Status::fault("history Guard injected finish fault");
        return result;
    }
};
static_assert(block::ReadDispatchGuard<Guard, Session>);
static_assert(!std::is_copy_constructible_v<Guard> && !std::is_move_constructible_v<Guard>);

void readonly(const Harness &h)
{
    CHECK(!h.disk.writes && !h.disk.flushes);
    CHECK(std::all_of(h.disk.bytes.begin(), h.disk.bytes.begin() + origin * 512,
                     [](u8 byte) { return byte == 0xa5; }));
}
void failed(Harness &h, Audit &audit, usize before, Failure failure, Status status)
{
    const bool refused = failure == Failure::refuse || failure == Failure::exhausted;
    const auto actual = h.session.report(); CHECK(actual);
    CHECK(status.code() == (failure == Failure::refuse ? StatusCode::busy :
          failure == Failure::exhausted ? StatusCode::overflow :
          failure == Failure::finish ? StatusCode::fault : StatusCode::io_error));
    CHECK(audit.begins && audit.finishes == audit.begins - usize(refused));
    CHECK(h.disk.reads == before + audit.finishes);
    CHECK(actual.value().reads == h.disk.reads && !actual.value().in_flight);
    CHECK(actual.value().completed_reads == actual.value().reads - audit.failures);
    CHECK(actual.value().state == block::DeviceState::quarantined);
    if (failure == Failure::media || failure == Failure::media_finish)
    {
        CHECK(status.message() == std::string_view("prospective injected transport read failure"));
        CHECK(audit.failures == 1); ++transport_priority;
    }
    if (refused) { CHECK(!audit.failures); ++refusals; }
    if (failure == Failure::finish) { CHECK(!audit.failures); ++finish_failures; }
    readonly(h); ++cases;
}
void decoy_untouched(Harness &decoy, const block::SessionReport &before)
{
    CHECK(reclaim_history::same_session(before, decoy.session.report().value()));
    CHECK(!decoy.disk.reads && !decoy.disk.writes && !decoy.disk.flushes);
    CHECK(decoy.session.close()); ++decoy_controls;
}
Family binding_family(const Harness &h, u64 first)
{
    const auto &binding = *h.admission;
    if (binding.capture_admitted_) return Family::binding_compare;
    switch (binding.report_.phase)
    {
    case Ext4ReclaimRecoveryPhase::bootstrap: return Family::bootstrap;
    case Ext4ReclaimRecoveryPhase::journal: return Family::wal;
    case Ext4ReclaimRecoveryPhase::before_binding: return Family::overlay;
    default: break;
    }
    const auto &capture = binding.capture_;
    if (capture.established_) return Family::capture_compare;
    if (capture.directory_count_) return Family::directory;
    for (usize group = 0; group < 4; ++group)
    {
        const auto &table = capture.binding_.metadata[group * 4 + 3];
        if (first / 2 >= table.first && first / 2 - table.first < table.count) return Family::table;
    }
    return Family::capture_early;
}
void add(std::vector<Request> &out, u32 physical) { out.push_back({u64{physical} * 2, 1024}); }
std::vector<Request> actual_sources(const ActualCapture &capture)
{
    std::vector<Request> requests;
    add(requests, 1); add(requests, 2);
    const u32 journal = u32(capture.binding_.layout.journal_first_block);
    for (u32 offset = 0; offset < capture.count_ + 4; ++offset) add(requests, journal + offset);
    for (usize group = 0; group < 4; ++group)
    {
        add(requests, u32(capture.binding_.metadata[group * 4 + 1].first));
        add(requests, u32(capture.binding_.metadata[group * 4 + 2].first));
        if (capture.initialized_[group])
            for (u32 offset = 0; offset < 512; ++offset)
                add(requests, u32(capture.binding_.metadata[group * 4 + 3].first) + offset);
    }
    for (usize at = 0; at < capture.directory_count_; ++at)
        add(requests, capture.storage_->directories[at].physical);
    CHECK(requests.size() == capture.count_ + 14 + capture.report_.table_blocks + capture.directory_count_);
    return requests;
}
std::vector<Request> bootstrap_comparisons(const Admission &binding)
{
    std::vector<Request> requests;
    add(requests, 1); add(requests, 2); add(requests, binding.journal_table_);
    for (usize at = 0; at < binding.report_.bootstrap_observations; ++at)
        add(requests, binding.storage_->bootstrap_reads[at].physical);
    return requests;
}

std::array<usize, usize(Family::count)> binding_lifetime(const ActiveFixture &fixture)
{
    Harness h(fixture.mixture(0)); Audit old_audit; Budget budget;
    CHECK(budget.reserve_opening(h.session));
    alignas(Guard) std::array<std::byte, sizeof(Guard)> memory{};
    auto *guard = new (memory.data()) Guard(h, old_audit, &budget);
    guard->counters = {&h.admission->report_.reads, &h.admission->capture_.report_.reads};
    guard->family = [&](u64 first) { return binding_family(h, first); };
    h.disk.hook = [&] { guard->in_callback(); };
    CHECK(h.admission->inspect(h.session, *h.storage, *h.scratch, *guard));
    const auto sources = actual_sources(h.admission->capture_);
    std::vector<Request> expected{{2, 1024}, {4, 1024}, {u64{h.admission->journal_table_} * 2, 1024}};
    for (u32 offset = 0; offset < h.admission->count_ + 4; ++offset)
        add(expected, h.admission->journal_first_ + offset);
    for (usize at = 0; at < h.admission->report_.bootstrap_observations; ++at)
        add(expected, h.storage->bootstrap_reads[at].physical);
    expected.insert(expected.end(), sources.begin(), sources.end());
    expected.insert(expected.end(), sources.begin(), sources.end());
    auto comparisons = bootstrap_comparisons(*h.admission);
    expected.insert(expected.end(), comparisons.begin(), comparisons.end());
    CHECK(old_audit.requests == expected && old_audit.begins == h.disk.reads);
    CHECK(old_audit.finishes == old_audit.begins && !old_audit.failures);
    CHECK(h.admission->report_.reads == h.disk.reads && h.admission->info() && h.admission->proof());
    const auto firsts = old_audit.firsts;
    guard->~Guard(); std::memset(memory.data(), 0xa5, memory.size());
    CHECK(old_audit.destroyed); const usize old_count = old_audit.begins;
    Audit fresh_audit; Guard fresh(h, fresh_audit, &budget);
    fresh.counters = {&h.admission->report_.reads, &h.admission->capture_.report_.reads};
    h.disk.hook = [&] { fresh.in_callback(); };
    CHECK(h.admission->revalidate(fresh));
    comparisons.insert(comparisons.end(), sources.begin(), sources.end());
    CHECK(fresh_audit.requests == comparisons && fresh_audit.begins == fresh_audit.finishes);
    CHECK(old_audit.begins == old_count);
    CHECK(budget.report().phase == Ext4ReclaimRoutePhase::opening && !budget.report().refined);
    CHECK(budget.report().attempts == (std::array<usize, 3>{h.disk.reads, 0, 0}));
    CHECK(budget.close(h.session));
    h.disk.hook = {}; const auto before = h.disk.reads;
    CHECK(h.admission->revalidate());
    CHECK(h.disk.reads == before + comparisons.size() && old_audit.begins == old_count);
    CHECK(fresh_audit.begins == comparisons.size());
    readonly(h); CHECK(h.session.close()); ++lifetime_controls; ++binding_successes; ++cases;
    return firsts;
}
void binding_failures(const ActiveFixture &fixture,
                      const std::array<usize, usize(Family::count)> &firsts)
{
    for (Family family : {Family::bootstrap, Family::wal, Family::overlay, Family::capture_early,
                          Family::table, Family::directory, Family::capture_compare, Family::binding_compare})
    {
        CHECK(firsts[usize(family)]);
        for (Failure failure : {Failure::refuse, Failure::media, Failure::finish, Failure::media_finish})
        {
            Harness h(fixture.mixture(0)); Audit audit; Guard guard(h, audit);
            guard.cut = firsts[usize(family)]; guard.failure = failure;
            guard.counters = {&h.admission->report_.reads, &h.admission->capture_.report_.reads};
            h.disk.hook = [&] { guard.in_callback(); };
            const auto status = h.admission->inspect(h.session, *h.storage, *h.scratch, guard);
            failed(h, audit, 0, failure, status);
            CHECK(!h.admission->report_.valid && !h.admission->info() && !h.admission->proof());
        }
    }
    {
        Harness h(fixture.mixture(0)); Audit audit; Guard guard(h, audit);
        guard.cut = firsts[usize(Family::table)] + 17; guard.failure = Failure::exhausted;
        h.disk.hook = [&] { guard.in_callback(); };
        failed(h, audit, 0, Failure::exhausted,
               h.admission->inspect(h.session, *h.storage, *h.scratch, guard));
    }
    using Change = std::function<void(Harness &, Harness &)>;
    const std::vector<Change> changes{
        [](auto &h, auto &d) { h.admission->session_ = &d.session; },
        [](auto &h, auto &) { h.admission->storage_ = nullptr; },
        [](auto &h, auto &) { h.admission->scratch_ = nullptr; },
        [](auto &h, auto &) { ++h.admission->report_.reads; },
        [](auto &h, auto &) { ++h.admission->report_.observed.completed_reads; },
        [](auto &h, auto &) { h.admission->retained_.ready = false; },
        [](auto &h, auto &d) { h.admission->capture_.session_ = &d.session; },
        [](auto &h, auto &) { h.admission->capture_.storage_ = nullptr; },
        [](auto &h, auto &) { h.admission->capture_.scratch_ = nullptr; },
        [](auto &h, auto &) { ++h.admission->capture_.report_.reads; },
        [](auto &h, auto &) { ++h.admission->capture_.report_.observed.completed_reads; },
        [](auto &h, auto &) { h.admission->capture_.retained_.ready = false; }
    };
    for (usize index = 0; index < changes.size(); ++index) for (bool media : {true, false})
    {
        Harness h(fixture.mixture(0)), decoy(fixture.mixture(0));
        const auto original_decoy = decoy.session.report().value(); Audit audit; Guard guard(h, audit);
        // Retained seals exist at these two actual leaf families.
        const auto cut = firsts[usize(index < 6 ? Family::binding_compare : Family::capture_compare)];
        guard.mutate = cut; guard.change = [&] { changes[index](h, decoy); };
        if (media) { guard.cut = cut; guard.failure = Failure::media_finish; }
        h.disk.hook = [&] { guard.in_callback(); };
        const auto status = h.admission->inspect(h.session, *h.storage, *h.scratch, guard);
        if (media) failed(h, audit, 0, Failure::media_finish, status);
        else { CHECK(!status && status.code() != StatusCode::io_error); h.quarantined(); ++integrity; ++cases; }
        decoy_untouched(decoy, original_decoy);
    }
}

void standalone_capture(const ActiveFixture &fixture)
{
    Harness h(fixture.mixture(0)); auto capture = std::make_unique<ActualCapture>();
    auto binding = fixture.binding; binding.empty_log = false; binding.journal_sequence = fixture.wal.sequence;
    Audit old; alignas(Guard) std::array<std::byte, sizeof(Guard)> memory{};
    auto *guard = new (memory.data()) Guard(h, old);
    guard->counters = {&capture->report_.reads}; h.disk.hook = [&] { guard->in_callback(); };
    CHECK(capture->capture(h.session, binding, h.storage->actual, *h.scratch, *guard));
    auto expected = actual_sources(*capture); auto doubled = expected;
    doubled.insert(doubled.end(), expected.begin(), expected.end());
    CHECK(old.requests == doubled && old.begins == old.finishes);
    guard->~Guard(); std::memset(memory.data(), 0xa5, memory.size());
    Audit fresh_audit; Guard fresh(h, fresh_audit); fresh.counters = {&capture->report_.reads};
    h.disk.hook = [&] { fresh.in_callback(); };
    CHECK(capture->revalidate(fresh) && fresh_audit.requests == expected);
    h.disk.hook = {}; const auto before = h.disk.reads;
    CHECK(capture->revalidate() && h.disk.reads == before + expected.size());
    CHECK(old.begins == doubled.size() && fresh_audit.begins == expected.size());
    readonly(h); CHECK(h.session.close()); ++capture_successes; ++lifetime_controls; ++cases;
    for (usize cut : {usize{1}, doubled.size() - 5})
        for (Failure failure : {Failure::refuse, Failure::media, Failure::finish, Failure::media_finish})
        {
            Harness failed_h(fixture.mixture(0)); auto failed_capture = std::make_unique<ActualCapture>();
            Audit audit; Guard failed_guard(failed_h, audit); failed_guard.cut = cut; failed_guard.failure = failure;
            failed_guard.counters = {&failed_capture->report_.reads};
            failed_h.disk.hook = [&] { failed_guard.in_callback(); };
            failed(failed_h, audit, 0, failure, failed_capture->capture(failed_h.session, binding,
                failed_h.storage->actual, *failed_h.scratch, failed_guard));
        }
}

void later_revalidation(const ActiveFixture &fixture)
{
    // The follow-up call receives a new synchronous Guard after an unguarded
    // initial scope. No nested shadow adoption adds an external hook.
    for (bool standalone : {false, true}) for (usize cut : {usize{1}, usize{19}})
        for (Failure failure : {Failure::refuse, Failure::media, Failure::finish, Failure::media_finish})
    {
        Harness h(fixture.mixture(0)); auto actual = std::make_unique<ActualCapture>();
        if (standalone)
        {
            auto binding = fixture.binding; binding.empty_log = false; binding.journal_sequence = fixture.wal.sequence;
            CHECK(actual->capture(h.session, binding, h.storage->actual, *h.scratch));
        }
        else CHECK(h.admission->inspect(h.session, *h.storage, *h.scratch));
        const auto before = h.disk.reads; Audit audit; Guard guard(h, audit);
        guard.cut = cut; guard.failure = failure;
        guard.counters = {&h.admission->report_.reads, &actual->report_.reads};
        h.disk.hook = [&] { guard.in_callback(); };
        const auto status = standalone ? actual->revalidate(guard) : h.admission->revalidate(guard);
        failed(h, audit, before, failure, status);
    }
}

void prepare_history(Harness &h, Projection &p)
{
    p.prepare(h);
    const auto before = h.disk.reads;
    CHECK(p.history->stable() && !p.history->reads_ && h.disk.reads == before);
}
Status history_operation(Harness &h, Projection &p, Family family, Guard &guard)
{
    switch (family)
    {
    case Family::history_ordinary:
    {
        Ext4RecoveryBinding ordinary; return p.history->prove_ordinary(ordinary, guard);
    }
    case Family::history_capture:
        return p.capture->capture_reclaim_result(h.session, *p.storage, *p.scratch, *p.history, *p.seal, guard);
    default: return p.history->compare_all(guard);
    }
}
void history_lifetime(const ActiveFixture &fixture)
{
    Harness h(fixture.mixture(0)); Projection p; prepare_history(h, p); Budget budget;
    CHECK(budget.reserve_opening(h.session));
    Audit old; alignas(Guard) std::array<std::byte, sizeof(Guard)> memory{};
    auto *guard = new (memory.data()) Guard(h, old, &budget);
    guard->counters = {&p.history->reads_, &h.admission->report_.reads, &h.admission->capture_.report_.reads};
    h.disk.hook = [&] { guard->in_callback(); CHECK(p.history->in_flight_exact()); };
    Block out{}; CHECK(p.history->read_blocks(2, std::as_writable_bytes(std::span{out}), *guard));
    CHECK(old.begins == 1 && old.finishes == 1);
    guard->~Guard(); std::memset(memory.data(), 0xa5, memory.size());
    Audit fresh_audit; Guard fresh(h, fresh_audit, &budget); fresh.counters = {&p.history->reads_};
    h.disk.hook = [&] { fresh.in_callback(); CHECK(p.history->in_flight_exact()); };
    const auto before = h.disk.reads; CHECK(p.history->compare_all(fresh));
    CHECK(fresh_audit.begins == h.disk.reads - before && fresh_audit.begins == fresh_audit.finishes);
    CHECK(budget.report().attempts[0] == old.begins + fresh_audit.begins);
    CHECK(budget.report().phase == Ext4ReclaimRoutePhase::opening && budget.close(h.session));
    h.disk.hook = {}; CHECK(p.history->read_blocks(2, std::as_writable_bytes(std::span{out})));
    CHECK(old.begins == 1 && fresh_audit.begins == h.disk.reads - before - 1);
    p.reports_unchanged(h); readonly(h); CHECK(h.session.close()); ++history_successes; ++lifetime_controls; ++cases;
}
void history_failures(const ActiveFixture &zero, const ActiveFixture &nonzero)
{
    for (Family family : {Family::history_ordinary, Family::history_capture, Family::history_terminal})
        for (Failure failure : {Failure::refuse, Failure::media, Failure::finish, Failure::media_finish})
    {
        const auto &fixture = family == Family::history_ordinary ? zero : nonzero;
        Harness h(fixture.mixture(0)); Projection p; prepare_history(h, p);
        const auto before = h.disk.reads; Audit audit; Guard guard(h, audit);
        guard.cut = family == Family::history_terminal ? 19 : 1; guard.failure = failure;
        guard.counters = {&p.history->reads_, &p.capture->report_.session_reads};
        h.disk.hook = [&] { guard.in_callback(); CHECK(p.history->in_flight_exact()); };
        failed(h, audit, before, failure, history_operation(h, p, family, guard));
        CHECK(p.history->rejected_);
    }
    using Change = std::function<void(Harness &, Projection &, Harness &)>;
    const std::vector<Change> changes{
        [](auto &, auto &p, auto &d) { p.history->original_session_ = &d.session; },
        [](auto &, auto &p, auto &) { ++p.history->reads_; },
        [](auto &, auto &p, auto &) { ++p.history->progress_.completed_reads; },
        [](auto &, auto &p, auto &) { p.history->domain_.workspace = nullptr; },
        [](auto &, auto &p, auto &) { p.history->seal_.ready = false; },
        [](auto &, auto &p, auto &) { p.capture->reclaim_history_ = nullptr; },
        [](auto &, auto &p, auto &) { p.capture->storage_ = nullptr; },
        [](auto &, auto &p, auto &) { p.capture->scratch_ = nullptr; },
        [](auto &, auto &p, auto &) { ++p.capture->report_.session_reads; }
    };
    for (const auto &change : changes) for (bool media : {true, false})
    {
        Harness h(nonzero.mixture(0)), decoy(nonzero.mixture(0)); Projection p; prepare_history(h, p);
        const auto original_decoy = decoy.session.report().value(), before = h.session.report().value();
        Audit audit; Guard guard(h, audit); guard.mutate = 1; guard.change = [&] { change(h, p, decoy); };
        if (media) { guard.cut = 1; guard.failure = Failure::media_finish; }
        h.disk.hook = [&] { guard.in_callback(); };
        const auto status = history_operation(h, p, Family::history_capture, guard);
        if (media) failed(h, audit, before.reads, Failure::media_finish, status);
        else { CHECK(!status && status.code() != StatusCode::io_error); h.quarantined(); ++integrity; ++cases; }
        decoy_untouched(decoy, original_decoy);
    }
}

Family composed_family(const Dry &d)
{
    switch (d.route->phase_)
    {
    case Route::ReadPhase::capture: return Family::composed_capture;
    case Route::ReadPhase::plan: return Family::composed_plan;
    case Route::ReadPhase::ordinary: return Family::composed_ordinary;
    case Route::ReadPhase::terminal: return Family::composed_terminal;
    default: return Family::composed_extension;
    }
}
void composed_lifetime(const ActiveFixture &fixture, const Image &golden)
{
    const auto mixed = fixture.mixture(0); Harness h(mixed); Projection p; prepare_history(h, p); Dry d;
    Audit old; alignas(Guard) std::array<std::byte, sizeof(Guard)> memory{};
    auto *guard = new (memory.data()) Guard(h, old);
    guard->counters = {&p.history->reads_, &d.route->reads_, &d.route->extension_reads_};
    h.disk.hook = [&] { guard->in_callback(); CHECK(d.route->in_flight_exact()); };
    CHECK(d.route->prepare(*p.history, *d.storage, *guard));
    CHECK(old.begins == usize(fixture.plan.reclaim.count == 4) && old.begins == old.finishes);
    guard->~Guard(); std::memset(memory.data(), 0xa5, memory.size());
    Budget budget; CHECK(budget.reserve_opening(h.session));
    Audit fresh_audit; Guard fresh(h, fresh_audit, &budget);
    fresh.family = [&](u64) { return composed_family(d); };
    fresh.counters = {&p.history->reads_, &d.route->reads_, &d.route->phase_reads_,
                      &d.route->capture_.report_.session_reads};
    h.disk.hook = [&] { fresh.in_callback(); CHECK(d.route->in_flight_exact()); };
    const auto before = h.disk.reads;
    CHECK(d.route->run(fresh));
    CHECK(d.route->complete_ && d.route->member_count_ == fixture.members - 1);
    CHECK(fresh_audit.begins == h.disk.reads - before && fresh_audit.begins == fresh_audit.finishes);
    CHECK(d.route->reads_ == fresh_audit.begins + old.begins && p.history->reads_ == d.route->reads_);
    usize charged = d.route->extension_reads_; for (const auto count : d.route->charged_) charged += count;
    CHECK(charged == d.route->reads_ && d.route->budget_.charges(d.route->extension_reads_, d.route->charged_, charged));
    CHECK(budget.report().phase == Ext4ReclaimRoutePhase::opening && !budget.report().refined);
    CHECK(budget.report().attempts[0] == fresh_audit.begins && budget.close(h.session));
    Image terminal = mixed;
    CHECK(p.history->each_original([&](u32 physical, const Block &) -> Status {
        const Block *projected{}; CHECK(d.route->resolve(physical, projected)); apply(terminal, physical, *projected); return {};
    }));
    if (d.route->extension_)
    {
        const auto physical = u32(fixture.binding.layout.journal_first_block) + 8;
        const Block *projected{}; CHECK(d.route->resolve(physical, projected)); apply(terminal, physical, *projected);
    }
    CHECK(terminal == golden); ++golden_checks;
    h.disk.hook = {}; CHECK(d.route->run().code() == StatusCode::already_exists);
    CHECK(old.begins == usize(fixture.plan.reclaim.count == 4));
    p.reports_unchanged(h); readonly(h); CHECK(h.session.close());
    ++composed_successes; ++lifetime_controls; ++cases;
    std::printf("CASE history Guard composed remaining=%zu reads=%zu extension=%zu capture=%zu plan=%zu ordinary=%zu terminal=%zu\n",
        fixture.members - 1, d.route->reads_, d.route->extension_reads_, d.route->charged_[0],
        d.route->charged_[1], d.route->charged_[2], d.route->charged_[3]); std::fflush(stdout);
}
void composed_failures(const ActiveFixture &zero, const ActiveFixture &nonzero)
{
    for (Family family : {Family::composed_extension, Family::composed_capture, Family::composed_plan,
                          Family::composed_ordinary, Family::composed_terminal})
        for (Failure failure : {Failure::refuse, Failure::media, Failure::finish, Failure::media_finish})
    {
        const auto &fixture = family == Family::composed_capture || family == Family::composed_plan ? nonzero : zero;
        Harness h(fixture.mixture(0)); Projection p; prepare_history(h, p); Dry d;
        Audit audit; Guard guard(h, audit); guard.failure = failure;
        guard.family = [&](u64) { return composed_family(d); };
        guard.select = [=](Family observed) { return observed == family; };
        guard.counters = {&p.history->reads_, &d.route->reads_, &d.route->phase_reads_};
        h.disk.hook = [&] { guard.in_callback(); CHECK(d.route->in_flight_exact()); };
        const auto before = h.disk.reads;
        auto status = d.route->prepare(*p.history, *d.storage, guard);
        if (status) status = d.route->run(guard);
        CHECK(guard.selected); failed(h, audit, before, failure, status);
    }
    using Change = std::function<void(Projection &, Dry &, Harness &)>;
    const std::vector<Change> changes{
        [](auto &, auto &d, auto &decoy) { d.route->original_session_ = &decoy.session; },
        [](auto &, auto &d, auto &) { d.route->origin_ = nullptr; },
        [](auto &, auto &d, auto &) { d.route->storage_ = nullptr; },
        [](auto &, auto &d, auto &) { ++d.route->reads_; },
        [](auto &, auto &d, auto &) { ++d.route->progress_.completed_reads; },
        [](auto &, auto &d, auto &) { d.route->seal_.ready = false; },
        [](auto &, auto &d, auto &) { ++d.route->completion_.report_.checked_inodes; }
    };
    for (const auto &change : changes) for (bool media : {true, false})
    {
        Harness h(zero.mixture(0)), decoy(zero.mixture(0)); Projection p; prepare_history(h, p); Dry d; d.prepare(p);
        const auto original_decoy = decoy.session.report().value(); const auto before = h.disk.reads;
        d.route->begin_phase(Route::ReadPhase::ordinary);
        d.route->ordinary_ = d.route->seal_.ordinary = true;
        Audit audit; Guard guard(h, audit); guard.mutate = 1; guard.change = [&] { change(p, d, decoy); };
        if (media) { guard.cut = 1; guard.failure = Failure::media_finish; }
        h.disk.hook = [&] { guard.in_callback(); };
        Block out{}; const auto status = d.route->read_blocks(2, std::as_writable_bytes(std::span{out}), guard);
        if (media) failed(h, audit, before, Failure::media_finish, status);
        else { CHECK(!status && status.code() != StatusCode::io_error); h.quarantined(); ++integrity; ++cases; }
        decoy_untouched(decoy, original_decoy);
    }
}

using Mount = Ext4MountedMetadata<2>;
struct Mounted
{
    owned_fixture::Disk disk;
    Mount::Registry registry;
    Mount::Registry::Registration key{};
    std::unique_ptr<Ext4MountedStartupWorkspace> startup = std::make_unique<Ext4MountedStartupWorkspace>();
    std::unique_ptr<Ext4MountedReclaimWorkspace> reclaim = std::make_unique<Ext4MountedReclaimWorkspace>();
    std::unique_ptr<Mount> mount = std::make_unique<Mount>(Ext4MountedPolicy::ordered_data_dirty_noatime,
                                                       reclaim.get(), startup.get());
    explicit Mounted(const Image &image) : disk(image)
    {
        disk.origin = origin; const auto registered = registry.register_device(disk, 49101, 65536);
        CHECK(registered); key = registered.value();
    }
    Status run() { return mount->mount(registry, key, {origin, 65536}); }
};
void mounted_readonly(const ActiveFixture &fixture)
{
    const auto image = fixture.mixture(0); Mounted h(image); usize callbacks{};
    h.disk.hook = [&](char operation) {
        CHECK(operation == 'r'); auto &owner = h.mount->owner_;
        CHECK(owner.startup_route_opening_ && owner.startup_route_budget_.in_flight_exact(owner.cache_.session_));
        const auto budget = owner.startup_route_budget_.report(); const auto actual = owner.cache_.session_.report().value();
        CHECK(actual.in_flight && actual.reads == h.disk.reads + 1 && actual.completed_reads == h.disk.reads);
        CHECK(budget.dispatch_pending && budget.attempts == (std::array<usize, 3>{h.disk.reads, 0, 0}));
        CHECK(budget.completions == (std::array<usize, 3>{h.disk.reads, 0, 0}));
        if (owner.startup_reclaim_history_.reading_) CHECK(owner.startup_reclaim_history_.in_flight_exact());
        if (owner.startup_reclaim_composed_.reading_) CHECK(owner.startup_reclaim_composed_.in_flight_exact());
        ++callbacks;
    };
    const auto status = h.run(); auto &owner = h.mount->owner_;
    CHECK(status.code() == StatusCode::unsupported);
    CHECK(status.message() == std::string_view("startup active reclaim writable gate is not qualified"));
    const auto budget = owner.startup_route_budget_.report();
    CHECK(budget.phase == Ext4ReclaimRoutePhase::closed && budget.refined && !budget.dispatch_pending);
    CHECK(budget.attempts == (std::array<usize, 3>{h.disk.reads, 0, 0}) && budget.completions == budget.attempts);
    CHECK(budget.observed.reads == h.disk.reads && budget.observed.completed_reads == h.disk.reads);
    CHECK(owner.reclaim_io_.reads == h.disk.reads && owner.reclaim_io_.completed_reads == h.disk.reads);
    CHECK(!owner.startup_route_budget_.control_.writer && !owner.startup_route_budget_.sealed_.writer);
    CHECK(owner.startup_report_.reclaim_dry_route_validated && owner.startup_report_.reclaim_result_validated);
    CHECK(owner.startup_report_.prospective_members == fixture.members - 1);
    CHECK(!h.disk.writes && !h.disk.flushes && h.disk.live == image && h.disk.durable == image);
    CHECK(callbacks == h.disk.reads && !h.mount->root());
    ++mounted_successes; ++cases;
    std::printf("CASE history Guard mounted remaining=%zu reads=%zu exact_charged=1 closed_writer=1\n",
        fixture.members - 1, h.disk.reads); std::fflush(stdout);
}
void mounted_priority(const ActiveFixture &zero, const ActiveFixture &nonzero)
{
    // Named live delegate layers, including extension before Composed::run.
    for (usize layer = 0; layer < 5; ++layer) for (bool media : {true, false})
    {
        const auto &fixture = layer == 1 ? nonzero : zero; const auto image = fixture.mixture(0);
        Mounted h(image); owned_fixture::Disk decoy_disk(image); decoy_disk.origin = origin;
        const auto registered = h.registry.register_device(decoy_disk, 49102, 65536); CHECK(registered);
        Mount::Registry::Session decoy; CHECK(h.registry.acquire(registered.value(), {origin, 65536},
                                                               block::SessionAccess::writable, decoy));
        const auto original_decoy = decoy.report().value(); bool fired{}; usize prefix{};
        h.disk.hook = [&](char operation) {
            CHECK(operation == 'r'); auto &owner = h.mount->owner_;
            bool selected{};
            if (layer == 0) selected = owner.startup_reclaim_binding_.attempted_ && !owner.startup_reclaim_binding_.capture_.attempted_;
            if (layer == 1) selected = owner.startup_reclaim_binding_.capture_.attempted_;
            if (layer == 2) selected = owner.startup_reclaim_history_.reading_ && !owner.startup_reclaim_composed_.attempted_;
            if (layer == 3) selected = owner.startup_reclaim_composed_.reading_ && !owner.startup_reclaim_composed_.extension_;
            if (layer == 4) selected = owner.startup_reclaim_composed_.ordinary_;
            if (fired || !selected) return;
            fired = true; prefix = h.disk.reads + 1;
            CHECK(owner.startup_route_budget_.in_flight_exact(owner.cache_.session_));
            if (media) h.disk.fail_read = prefix;
            // Corrupt both the finish authority and the independent Owner shadow.
            ++owner.startup_route_budget_.control_.attempts[0];
            ++owner.reclaim_io_.reads;
            owner.startup_route_opening_ = false;
            if (layer <= 1) owner.startup_reclaim_binding_.session_ = &decoy;
            if (layer == 2) owner.startup_reclaim_history_.original_session_ = &decoy;
            if (layer >= 3) owner.startup_reclaim_composed_.original_session_ = &decoy;
        };
        const auto status = h.run(); CHECK(fired && !status && h.disk.reads == prefix);
        if (media)
        {
            CHECK(status.code() == StatusCode::io_error && status.message() == h.disk.read_error.message());
            CHECK(h.mount->owner_.startup_report_.status.code() == StatusCode::io_error); ++transport_priority;
        }
        else { CHECK(status.code() == StatusCode::fault); ++integrity; }
        const auto actual = h.mount->owner_.cache_.session_.report().value();
        CHECK(actual.state == block::DeviceState::quarantined && !actual.in_flight);
        CHECK(actual.reads == prefix && actual.completed_reads == prefix - usize(media));
        CHECK(!h.disk.writes && !h.disk.flushes && h.disk.live == image && h.disk.durable == image);
        CHECK(reclaim_history::same_session(original_decoy, decoy.report().value()));
        CHECK(!decoy_disk.reads && !decoy_disk.writes && !decoy_disk.flushes && decoy.close());
        ++decoy_controls; ++mounted_failures; ++cases;
    }
}
} // namespace v33

int main(int argc, char **argv)
{
    CHECK(argc == 2); const std::string root = argv[1];
    const auto image = [&](const char *name) { return read_image((root + "/" + name).c_str()); };
    const auto zero_store = std::make_unique<ActiveFixture>(image("input-empty.img"));
    const auto one_store = std::make_unique<ActiveFixture>(image("input-chain2-cross-group.img"));
    const auto seven_store = std::make_unique<ActiveFixture>(image("input-chain8-cross-group.img"));
    const auto &zero = *zero_store; const auto &one = *one_store; const auto &seven = *seven_store;
    CHECK(zero.members == 1 && one.members == 2 && seven.members == 8);
    const auto phase = [](const char *name) { std::printf("PHASE history Guard %s\n", name); std::fflush(stdout); };
    phase("binding");
    const auto cuts = v33::binding_lifetime(one);
    v33::binding_failures(one, cuts); v33::standalone_capture(zero);
    v33::later_revalidation(one);
    phase("history");
    v33::history_lifetime(one); v33::history_failures(zero, one);
    phase("composed-faults");
    v33::composed_failures(zero, one);
    phase("composed-goldens");
    v33::composed_lifetime(zero, image("finished-empty.img"));
    v33::composed_lifetime(one, image("finished-chain2.img"));
    v33::composed_lifetime(seven, image("finished-chain8.img"));
    phase("mounted");
    v33::mounted_readonly(zero); v33::mounted_readonly(one); v33::mounted_readonly(seven);
    v33::mounted_priority(zero, one);
    std::printf("PASS guarded reclaim history checks=%zu cases=%zu binding=%zu actual_capture=%zu history=%zu composed=%zu mounted=%zu mounted_faults=%zu refusals=%zu finish_faults=%zu transport_priority=%zu integrity=%zu decoy_untouched=%zu destroyed_guard=%zu exact_callbacks=%zu independent_goldens=%zu shared_opening_exact=1 writer_closed=1 writes=0 flushes=0 native=0\n",
        v33::checks, v33::cases, v33::binding_successes, v33::capture_successes, v33::history_successes,
        v33::composed_successes, v33::mounted_successes, v33::mounted_failures, v33::refusals,
        v33::finish_failures, v33::transport_priority, v33::integrity, v33::decoy_controls,
        v33::lifetime_controls, v33::exact_callbacks, v33::golden_checks);
}
