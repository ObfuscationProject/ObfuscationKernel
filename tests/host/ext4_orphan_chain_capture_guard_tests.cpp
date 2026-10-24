#include "ok/block/guarded_reads.hpp"
#include "borrowed_fixture.hpp"
#include <algorithm>
#include <cstring>
#include <new>
#include <type_traits>

// Private attachment/counter corruption is a labelled host model, compiled
// with -fno-access-control as the existing capture boundary suite is. All
// original and decoy Sessions and caller storage remain alive throughout.
namespace
{
usize checks{}, cases{}, captures{}, guarded_plans{}, default_plans{}, refusals{};
usize media_priority{}, original_aborts{}, decoy_preserved{}, lifetime_cases{};

#undef CHECK
#define CHECK(value) do { ++checks; if (!(value)) { \
    std::fprintf(stderr, "FAIL guarded chain capture line=%u: %s\n", unsigned(__LINE__), #value); \
    std::abort(); } } while (false)

using Session = Registry::Session;

struct Request
{
    u64 first{};
    usize bytes{};
    bool operator==(const Request &) const = default;
};
struct Audit
{
    std::vector<Request> requests{};
    usize begins{}, finishes{}, successful_transports{}, failed_transports{}, refused{};
    bool destroyed{};
};
enum class Mutation { none, counter, session, storage, scratch, report_identity };

struct Guard final
{
    Guard(Harness &owner, Audit &audit) : owner_(owner), audit_(audit) {}
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;
    Guard(Guard &&) = delete;
    Guard &operator=(Guard &&) = delete;
    ~Guard() { audit_.destroyed = true; }

    usize refuse_at{~usize{0}}, mutate_at{~usize{0}}, finish_fault_at{~usize{0}};
    Mutation mutation{Mutation::none};
    Session *decoy{};

    Status begin_read(Session &session, u64 first, usize bytes)
    {
        CHECK(!audit_.destroyed && !pending_);
        CHECK(&session == &owner_.session);
        CHECK(first % 2 == 0 && first < 65536 && bytes == 1024);
        ++audit_.begins; audit_.requests.push_back({first, bytes});
        if (audit_.begins == refuse_at)
        { ++audit_.refused; return Status::busy("capture test Guard refused begin"); }
        const auto current = owner_.registry.inspect(owner_.key); CHECK(current);
        before_ = current.value(); physical_reads_ = owner_.disk.reads;
        CHECK(before_.state == block::DeviceState::owned && !before_.in_flight);
        CHECK(before_.reads == before_.completed_reads && !before_.pending_writes);
        pending_ = true;
        return {};
    }
    Status finish_read(Session &session, Status transport)
    {
        CHECK(!audit_.destroyed && pending_ && &session == &owner_.session);
        pending_ = false; ++audit_.finishes;
        const auto after = owner_.registry.inspect(owner_.key); CHECK(after);
        CHECK(after.value().reads == before_.reads + 1);
        CHECK(after.value().completed_reads == before_.completed_reads + usize(bool(transport)));
        CHECK(!after.value().in_flight && owner_.disk.reads == physical_reads_ + 1);
        if (transport) ++audit_.successful_transports; else ++audit_.failed_transports;
        if (audit_.finishes == mutate_at)
        {
            switch (mutation)
            {
            case Mutation::none: break;
            case Mutation::counter: owner_.capture.report_.session_reads += 7; break;
            case Mutation::session:
                CHECK(decoy); owner_.capture.session_ = decoy; break;
            case Mutation::storage: owner_.capture.storage_ = nullptr; break;
            case Mutation::scratch: owner_.capture.scratch_ = nullptr; break;
            case Mutation::report_identity: owner_.capture.report_.generation ^= 1; break;
            }
        }
        return audit_.finishes == finish_fault_at ? Status::fault("capture test Guard finish fault") : Status{};
    }
  private:
    Harness &owner_;
    Audit &audit_;
    block::SessionReport before_{};
    usize physical_reads_{};
    bool pending_{};
};

struct Expected
{
    std::vector<Request> capture{}, comparison{};
    usize bootstrap{}, first_table{}, first_directory{}, first_comparison{};
};

void add(std::vector<Request> &out, u32 physical) { out.push_back({u64(physical) * 2, 1024}); }

Expected expected_reads(const Harness &owner)
{
    Expected expected;
    add(expected.capture, 1);
    expected.bootstrap = owner.capture.bootstrap_count_;
    CHECK(expected.bootstrap > 0 && expected.bootstrap <= owner.storage->bootstrap_reads.size());
    for (usize at = 0; at < expected.bootstrap; ++at)
        add(expected.capture, owner.storage->bootstrap_reads[at].physical);
    add(expected.capture, 2);
    add(expected.capture, u32(owner.capture.binding_.layout.journal_first_block));
    add(expected.comparison, 1); add(expected.comparison, 2);
    add(expected.comparison, u32(owner.capture.binding_.layout.journal_first_block));
    usize tables{};
    for (usize group = 0; group < 4; ++group)
    {
        for (usize kind : {usize{1}, usize{2}})
        {
            const auto physical = u32(owner.capture.binding_.metadata[group * 4 + kind].first);
            add(expected.capture, physical); add(expected.comparison, physical);
        }
        if (owner.capture.initialized_[group])
        {
            if (!expected.first_table) expected.first_table = expected.capture.size() + 1;
            const auto first = u32(owner.capture.binding_.metadata[group * 4 + 3].first);
            for (u32 offset = 0; offset < 512; ++offset)
            { add(expected.capture, first + offset); add(expected.comparison, first + offset); ++tables; }
        }
    }
    CHECK(tables == owner.capture.report().table_blocks && tables >= 512);
    CHECK(owner.capture.directory_count_ == owner.capture.report().directory_blocks);
    CHECK(owner.capture.directory_count_ > 0);
    expected.first_directory = expected.capture.size() + 1;
    for (usize at = 0; at < owner.capture.directory_count_; ++at)
    {
        add(expected.capture, owner.storage->directories[at].physical);
        add(expected.comparison, owner.storage->directories[at].physical);
    }
    for (usize at = 0; at < expected.bootstrap; ++at)
        add(expected.comparison, owner.storage->bootstrap_reads[at].physical);
    expected.first_comparison = expected.capture.size() + 1;
    expected.capture.insert(expected.capture.end(), expected.comparison.begin(), expected.comparison.end());
    CHECK(expected.comparison.size() == 11 + tables + owner.capture.directory_count_ + expected.bootstrap);
    return expected;
}

void readonly_authority(const Harness &owner)
{
    CHECK(owner.disk.writes == 0 && owner.disk.flushes == 0);
    CHECK(std::all_of(owner.disk.bytes.begin(), owner.disk.bytes.begin() + origin * 512,
                      [](u8 value) { return value == 0xa5; }));
}

void rejection(const Harness &owner, StatusCode code, usize attempted, usize completed)
{
    CHECK(!owner.capture.report().valid && owner.capture.report().phase == Ext4OrphanSnapshotPhase::rejected);
    CHECK(owner.capture.report().status.code() == code);
    const auto actual = owner.registry.inspect(owner.key); CHECK(actual);
    CHECK(actual.value().state == block::DeviceState::quarantined && actual.value().first_error == code);
    CHECK(actual.value().reads == attempted && actual.value().completed_reads == completed);
    CHECK(!actual.value().in_flight && !actual.value().pending_writes);
    readonly_authority(owner); ++original_aborts;
}

Expected accepted(const Image &image, bool populated)
{
    ++cases; Harness owner(image, block::SessionAccess::readonly); Audit capture_audit;
    Guard capture_guard(owner, capture_audit);
    CHECK(owner.capture.capture(owner.session, *owner.storage, *owner.scratch, capture_guard));
    ++captures;
    const auto expected = expected_reads(owner); const auto report = owner.capture.report();
    CHECK(report.valid && report.head == (populated ? 12U : 14U));
    CHECK(report.chain_members == (populated ? 2U : 1U));
    CHECK(report.verified_blocks == expected.comparison.size());
    CHECK(capture_audit.requests == expected.capture);
    CHECK(capture_audit.begins == expected.capture.size() && capture_audit.finishes == capture_audit.begins);
    CHECK(capture_audit.successful_transports == capture_audit.begins && capture_audit.failed_transports == 0);
    CHECK(owner.disk.reads == expected.capture.size() && report.session_reads == owner.disk.reads);
    CHECK(owner.session.report().value().completed_reads == owner.disk.reads);
    Audit plan_audit; Guard plan_guard(owner, plan_audit);
    Ext4OrphanDirectoryQualifiedReclaimPlan plan{};
    CHECK(owner.capture.plan_head(report.head, report.generation, plan, plan_guard) && plan.valid);
    ++guarded_plans;
    CHECK(plan.reclaim.count == (populated ? 5U : 4U));
    CHECK(plan_audit.requests == expected.comparison && plan_audit.begins == plan_audit.finishes);
    CHECK(capture_audit.requests == expected.capture); // No reuse of the earlier Guard.
    CHECK(owner.capture.report().session_reads == expected.capture.size() + expected.comparison.size());
    CHECK(owner.capture.report().verified_blocks == 2 * expected.comparison.size());
    CHECK(owner.session.report().value().completed_reads == owner.disk.reads);
    readonly_authority(owner); CHECK(owner.session.close());
    return expected;
}

void destroyed_guard(const Image &image)
{
    ++cases; ++lifetime_cases;
    Harness owner(image); Audit audit;
    alignas(Guard) std::array<std::byte, sizeof(Guard)> memory{};
    auto *guard = new (memory.data()) Guard(owner, audit);
    CHECK(owner.capture.capture(owner.session, *owner.storage, *owner.scratch, *guard)); ++captures;
    const auto expected = expected_reads(owner); const auto report = owner.capture.report();
    guard->~Guard(); std::memset(memory.data(), 0xa5, memory.size());
    CHECK(audit.destroyed); const auto earlier_begins = audit.begins, earlier_finishes = audit.finishes;
    Ext4OrphanDirectoryQualifiedReclaimPlan plan{};
    CHECK(owner.capture.plan_head(report.head, report.generation, plan) && plan.valid); ++default_plans;
    CHECK(audit.begins == earlier_begins && audit.finishes == earlier_finishes);
    CHECK(owner.disk.reads == expected.capture.size() + expected.comparison.size());
    Audit fresh; Guard fresh_guard(owner, fresh);
    CHECK(owner.capture.plan_head(report.head, report.generation, plan, fresh_guard) && plan.valid); ++guarded_plans;
    CHECK(fresh.requests == expected.comparison && fresh.begins == fresh.finishes);
    CHECK(audit.begins == earlier_begins && audit.finishes == earlier_finishes);
    CHECK(owner.disk.reads == expected.capture.size() + 2 * expected.comparison.size());
    CHECK(owner.capture.report().session_reads == owner.disk.reads);
    readonly_authority(owner); CHECK(owner.session.close());
}

void refused_begins(const Image &image, const Expected &expected)
{
    // Capture-level refusal quarantines the original one-shot operation. The
    // leaf guarded_read_blocks helper alone does not abort on a refused begin.
    for (usize refused_at : {usize{1}, usize{2}, expected.first_table + 19,
                             expected.first_directory, expected.first_comparison,
                             expected.capture.size()})
    {
        ++cases; ++refusals; Harness owner(image); Audit audit; Guard guard(owner, audit);
        guard.refuse_at = refused_at;
        const auto status = owner.capture.capture(owner.session, *owner.storage, *owner.scratch, guard);
        CHECK(status.code() == StatusCode::busy);
        CHECK(audit.begins == refused_at && audit.finishes == refused_at - 1 && audit.refused == 1);
        CHECK(audit.successful_transports == refused_at - 1 && audit.failed_transports == 0);
        CHECK(audit.requests.size() == refused_at &&
              std::equal(audit.requests.begin(), audit.requests.end(), expected.capture.begin()));
        CHECK(owner.disk.reads == refused_at - 1 && owner.capture.report().session_reads == refused_at - 1);
        rejection(owner, StatusCode::busy, refused_at - 1, refused_at - 1);
    }
    for (usize refused_at : {usize{1}, expected.comparison.size()})
    {
        ++cases; ++refusals; Harness owner(image); CHECK(owner.run());
        const auto report = owner.capture.report(); Audit audit; Guard guard(owner, audit);
        guard.refuse_at = refused_at; Ext4OrphanDirectoryQualifiedReclaimPlan plan{}; plan.valid = true;
        CHECK(owner.capture.plan_head(report.head, report.generation, plan, guard).code() == StatusCode::busy);
        CHECK(!plan.valid && audit.begins == refused_at && audit.finishes == refused_at - 1);
        CHECK(owner.disk.reads == expected.capture.size() + refused_at - 1);
        CHECK(owner.capture.report().session_reads == owner.disk.reads);
        rejection(owner, StatusCode::busy, owner.disk.reads, owner.disk.reads);
    }
}

void media_failure_priority(const Image &image, const Expected &expected)
{
    for (usize fail_at : {usize{1}, expected.first_comparison + 5})
        for (Mutation mutation : {Mutation::none, Mutation::counter, Mutation::session,
                                  Mutation::storage, Mutation::scratch, Mutation::report_identity})
        {
            ++cases; ++media_priority;
            Harness owner(image), decoy(image); const auto before = decoy.session.report(); CHECK(before);
            owner.disk.fail_at = fail_at; Audit audit; Guard guard(owner, audit);
            guard.finish_fault_at = fail_at; guard.mutate_at = fail_at;
            guard.mutation = mutation; guard.decoy = &decoy.session;
            const auto status = owner.capture.capture(owner.session, *owner.storage, *owner.scratch, guard);
            CHECK(status.code() == StatusCode::io_error);
            CHECK(status.message() == std::string_view("borrowed capture injected read fault"));
            CHECK(audit.begins == fail_at && audit.finishes == fail_at && audit.failed_transports == 1);
            CHECK(audit.successful_transports == fail_at - 1 && owner.disk.reads == fail_at);
            rejection(owner, StatusCode::io_error, fail_at, fail_at - 1);
            CHECK(reclaim_history::same_session(before.value(), decoy.session.report().value()));
            CHECK(decoy.disk.reads == 0 && decoy.disk.writes == 0 && decoy.disk.flushes == 0);
            ++decoy_preserved; CHECK(decoy.session.close());
        }
}

void successful_transport_integrity(const Image &image)
{
    for (Mutation mutation : {Mutation::counter, Mutation::session, Mutation::storage,
                              Mutation::scratch, Mutation::report_identity})
    {
        ++cases; Harness owner(image), decoy(image); const auto before = decoy.session.report(); CHECK(before);
        Audit audit; Guard guard(owner, audit); guard.mutate_at = 1;
        guard.mutation = mutation; guard.decoy = &decoy.session;
        CHECK(owner.capture.capture(owner.session, *owner.storage, *owner.scratch, guard).code() == StatusCode::fault);
        CHECK(audit.begins == 1 && audit.finishes == 1 && audit.successful_transports == 1);
        CHECK(owner.disk.reads == 1); rejection(owner, StatusCode::fault, 1, 1);
        CHECK(reclaim_history::same_session(before.value(), decoy.session.report().value()));
        CHECK(decoy.disk.reads == 0); ++decoy_preserved; CHECK(decoy.session.close());
    }
    {
        ++cases; Harness owner(image); Audit audit; Guard guard(owner, audit); guard.finish_fault_at = 1;
        CHECK(owner.capture.capture(owner.session, *owner.storage, *owner.scratch, guard).code() == StatusCode::fault);
        CHECK(audit.begins == 1 && audit.finishes == 1 && audit.successful_transports == 1);
        CHECK(owner.disk.reads == 1 && owner.capture.report().session_reads == 0);
        rejection(owner, StatusCode::fault, 1, 1);
    }
}

void later_plan_callbacks(const Image &image, const Expected &expected)
{
    for (usize fail_at : {usize{1}, expected.comparison.size()})
        for (Mutation mutation : {Mutation::none, Mutation::counter, Mutation::session,
                                  Mutation::storage, Mutation::scratch, Mutation::report_identity})
        {
            ++cases; ++media_priority;
            Harness owner(image), decoy(image); CHECK(owner.run());
            const auto report = owner.capture.report(); const auto baseline = owner.disk.reads;
            const auto before = decoy.session.report(); CHECK(before);
            owner.disk.fail_at = baseline + fail_at; Audit audit; Guard guard(owner, audit);
            guard.finish_fault_at = fail_at; guard.mutate_at = fail_at;
            guard.mutation = mutation; guard.decoy = &decoy.session;
            Ext4OrphanDirectoryQualifiedReclaimPlan plan{}; plan.valid = true;
            const auto status = owner.capture.plan_head(report.head, report.generation, plan, guard);
            CHECK(status.code() == StatusCode::io_error && !plan.valid);
            CHECK(status.message() == std::string_view("borrowed capture injected read fault"));
            CHECK(audit.begins == fail_at && audit.finishes == fail_at && audit.failed_transports == 1);
            CHECK(audit.successful_transports == fail_at - 1);
            CHECK(audit.requests.size() == fail_at &&
                  std::equal(audit.requests.begin(), audit.requests.end(), expected.comparison.begin()));
            CHECK(owner.disk.reads == baseline + fail_at);
            rejection(owner, StatusCode::io_error, baseline + fail_at, baseline + fail_at - 1);
            CHECK(reclaim_history::same_session(before.value(), decoy.session.report().value()));
            CHECK(decoy.disk.reads == 0 && decoy.disk.writes == 0 && decoy.disk.flushes == 0);
            ++decoy_preserved; CHECK(decoy.session.close());
        }
    for (Mutation mutation : {Mutation::counter, Mutation::session, Mutation::storage,
                              Mutation::scratch, Mutation::report_identity})
    {
        ++cases; Harness owner(image), decoy(image); CHECK(owner.run());
        const auto report = owner.capture.report(); const auto baseline = owner.disk.reads;
        const auto before = decoy.session.report(); CHECK(before);
        Audit audit; Guard guard(owner, audit); guard.mutate_at = 1;
        guard.mutation = mutation; guard.decoy = &decoy.session;
        Ext4OrphanDirectoryQualifiedReclaimPlan plan{}; plan.valid = true;
        CHECK(owner.capture.plan_head(report.head, report.generation, plan, guard).code() == StatusCode::fault);
        CHECK(!plan.valid && audit.begins == 1 && audit.finishes == 1 && audit.successful_transports == 1);
        CHECK(owner.disk.reads == baseline + 1); rejection(owner, StatusCode::fault, baseline + 1, baseline + 1);
        CHECK(reclaim_history::same_session(before.value(), decoy.session.report().value()));
        CHECK(decoy.disk.reads == 0); ++decoy_preserved; CHECK(decoy.session.close());
    }
    {
        ++cases; Harness owner(image); CHECK(owner.run());
        const auto report = owner.capture.report(); const auto baseline = owner.disk.reads;
        Audit audit; Guard guard(owner, audit); guard.finish_fault_at = 1;
        Ext4OrphanDirectoryQualifiedReclaimPlan plan{}; plan.valid = true;
        CHECK(owner.capture.plan_head(report.head, report.generation, plan, guard).code() == StatusCode::fault);
        CHECK(!plan.valid && audit.begins == 1 && audit.finishes == 1 && audit.successful_transports == 1);
        CHECK(owner.disk.reads == baseline + 1 && owner.capture.report().session_reads == baseline);
        rejection(owner, StatusCode::fault, baseline + 1, baseline + 1);
    }
}
} // namespace

int main(int argc, char **argv)
{
    static_assert(block::ReadDispatchGuard<Guard, Session>);
    static_assert(!std::is_copy_constructible_v<Guard> && !std::is_move_constructible_v<Guard>);
    static_assert(!std::is_copy_constructible_v<Capture> && !std::is_move_constructible_v<Capture>);
    CHECK(argc == 3);
    const auto filled = orphan_image(read_image(argv[1]), true);
    const auto empty = orphan_image(read_image(argv[2]), false);
    const auto filled_expected = accepted(filled, true);
    const auto empty_expected = accepted(empty, false);
    for (const auto &pair : {std::pair{&filled, &filled_expected}, std::pair{&empty, &empty_expected}})
    {
        destroyed_guard(*pair.first); refused_begins(*pair.first, *pair.second);
        media_failure_priority(*pair.first, *pair.second); successful_transport_integrity(*pair.first);
        later_plan_callbacks(*pair.first, *pair.second);
    }
    std::printf("PASS guarded chain capture checks=%zu cases=%zu captures=%zu guarded_plans=%zu default_plans=%zu refusals=%zu transport_priority=%zu original_session_abort=%zu decoy_untouched=%zu destroyed_guard=%zu bootstrap_once=1 retained_overlay_hooks=0 capture_write_flush=0 native=0\n",
        checks, cases, captures, guarded_plans, default_plans, refusals, media_priority,
        original_aborts, decoy_preserved, lifetime_cases);
}
