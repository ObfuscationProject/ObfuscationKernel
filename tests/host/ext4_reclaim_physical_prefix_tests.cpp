// Isolated hosted prefix controls. No writer admission, target or guest execution.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main retained_composed_compatibility_main
#include "ext4_reclaim_composed_history_tests.cpp"
#undef main
#pragma GCC diagnostic pop
#include "ext4_mounted_support.hpp"
#include <ok/fs/ext4/mount.hpp>
#include <set>

namespace
{
using Prefix = Route::PhysicalPrefix;
using Head = Route::PrefixHead;
usize prefix_states{}, prefix_blocks{}, prefix_refusals{}, future_changes{}, owner_handoffs{}, raw_slot_controls{};

std::set<u32> affected(const Dry &dry)
{
    const u32 first = dry.route->origin_->binding_->journal_first_;
    std::set<u32> result{1, 2, first, first + 1, first + 2, first + 3,
        first + 4, first + 5, first + 6, first + 7, first + 8};
    for (usize row = 0; row < dry.route->row_count_; ++row)
        for (usize home = 0; home < dry.storage->rows_[row].plan.reclaim.count; ++home)
            result.insert(dry.storage->rows_[row].plan.reclaim.targets[home]);
    // A retained peer table remains exactly original at every stage.
    result.insert(u32(dry.route->origin_->binding_->capture_.retained_.binding.metadata[3].first) + 20);
    return result;
}
void compare_prefix(Harness &h, Dry &dry, const Prefix &prefix, const Image &replayed)
{
    const auto before = h.session.report().value();
    const auto count = h.disk.reads;
    const auto status = dry.route->prefix_exact(prefix);
    if (!status) std::fprintf(stderr, "prefix rows=%zu wal=%zu mask=%u head=%u status=%u: %.*s\n",
        prefix.completed_rows, prefix.wal_writes, unsigned(prefix.home_after_mask), unsigned(prefix.head),
        unsigned(status.code()), int(status.message().size()), status.message().data());
    CHECK(status);
    for (u32 physical : affected(dry))
    {
        const Block *expected{};
        CHECK(dry.route->resolve_physical_prefix(prefix, physical, expected) && expected);
        CHECK(*expected == load(replayed, physical));
        ++prefix_blocks;
    }
    CHECK(h.disk.reads == count && !h.disk.writes && !h.disk.flushes);
    CHECK(reclaim_history::same_session(before, h.session.report().value()));
    ++prefix_states;
}
void refusal(Harness &h, Dry &dry, const Prefix &prefix)
{
    const Block *out = &dry.storage->checking_;
    const auto count = h.disk.reads;
    CHECK(!dry.route->resolve_physical_prefix(prefix, 1, out));
    CHECK(!out && h.disk.reads == count && !h.disk.writes && !h.disk.flushes);
    ++prefix_refusals;
}
void prefix_case(const ActiveFixture &fixture, unsigned original_mask, const Image &golden)
{
    Image replayed = fixture.mixture(original_mask);
    Harness h(replayed); Projection p; p.prepare(h); p.prove(h); Dry dry; dry.prepare(p);
    // Prefix selection is refused until the entire original dry route finishes.
    refusal(h, dry, {0, 0, u8(original_mask), Head::active});
    CHECK(dry.route->run() && dry.route->retained_exact());
    const u32 first = u32(fixture.binding.layout.journal_first_block);
    for (usize current = 0; current < dry.route->row_count_; ++current)
    {
        const auto &row = dry.storage->rows_[current];
        const auto &plan = row.plan.reclaim;
        Prefix prefix{current, 0, current ? u8{0} : u8(original_mask),
            current ? Head::predecessor : Head::active};
        compare_prefix(h, dry, prefix, replayed);
        // Exact prefix must reject premature homes belonging to a future row.
        if (current + 1 < dry.route->row_count_)
        {
            const auto &future = dry.storage->rows_[current + 1].plan.reclaim;
            bool different{};
            for (usize home = 0; home < future.count; ++home)
            {
                const Block *expected{};
                CHECK(dry.route->resolve_physical_prefix(prefix, future.targets[home], expected));
                if (*expected != future.after[home]) { different = true; ++future_changes; }
            }
            CHECK(different);
        }
        if (current)
        {
            // Independent event replay follows real C1 transport order. The
            // resolver must agree after each returned block, before any FLUSH.
            apply(replayed, first + 1, row.wal.descriptor); ++prefix.wal_writes;
            compare_prefix(h, dry, prefix, replayed);
            for (usize payload = 0; payload < row.wal.count; ++payload)
            {
                apply(replayed, first + 2 + u32(payload), row.wal.payloads[payload]); ++prefix.wal_writes;
                compare_prefix(h, dry, prefix, replayed);
            }
            apply(replayed, first + 3 + u32(row.wal.count), row.wal.end); ++prefix.wal_writes;
            compare_prefix(h, dry, prefix, replayed);
            apply(replayed, first + 2 + u32(row.wal.count), row.wal.commit); ++prefix.wal_writes;
            compare_prefix(h, dry, prefix, replayed);
            apply(replayed, first, row.wal.journal_head); prefix.head = Head::active;
            compare_prefix(h, dry, prefix, replayed);
        }
        const Image active_before = replayed;
        const unsigned full = (1U << plan.count) - 1;
        // Exhaust every allocation mixture rather than a numeric home prefix.
        for (unsigned mask = 0; mask <= full; ++mask)
        {
            Prefix mixture = prefix; mixture.home_after_mask = u8(mask);
            if (!legal(plan.count, mask) || (!current && (mask & original_mask) != original_mask))
            { refusal(h, dry, mixture); continue; }
            Image stage = active_before;
            for (usize home = 0; home < plan.count; ++home)
                if (mask & (1U << home)) apply(stage, plan.targets[home], plan.after[home]);
            compare_prefix(h, dry, mixture, stage);
        }
        for (usize home = 0; home < plan.count; ++home)
            apply(replayed, plan.targets[home], plan.after[home]);
        prefix.home_after_mask = u8(full); compare_prefix(h, dry, prefix, replayed);
        apply(replayed, first, row.cleanup); prefix.head = Head::cleanup;
        compare_prefix(h, dry, prefix, replayed);
        const Image completed = replayed;
        Prefix next{current + 1, 0, 0, Head::predecessor};
        compare_prefix(h, dry, next, completed);
        if (current)
        {
            refusal(h, dry, {current, row.wal.count + 4, 0, Head::predecessor});
            refusal(h, dry, {current, row.wal.count + 2, 0, Head::active});
            refusal(h, dry, {current, 0, 1, Head::predecessor});
        }
        refusal(h, dry, {current, prefix.wal_writes, 0, Head::cleanup});
        refusal(h, dry, {current, prefix.wal_writes, u8(full | (1U << plan.count)), Head::active});
        refusal(h, dry, {current, prefix.wal_writes, u8(full), Head(99)});
    }
    CHECK(replayed == golden); ++goldens;
    Prefix terminal{dry.route->row_count_, 0, 0, Head::predecessor};
    Image whole = fixture.mixture(original_mask);
    CHECK(p.history->each_original([&](u32 physical, const Block &) -> Status {
        const Block *expected{}; CHECK(dry.route->resolve_physical_prefix(terminal, physical, expected));
        apply(whole, physical, *expected); return {};
    }));
    if (dry.route->extension_) {
        const Block *expected{}; CHECK(dry.route->resolve_physical_prefix(terminal, first + 8, expected));
        apply(whole, first + 8, *expected);
    }
    CHECK(whole == golden); ++goldens;
    refusal(h, dry, {dry.route->row_count_ + 1, 0, 0, Head::predecessor});
    refusal(h, dry, {dry.route->row_count_, 1, 0, Head::predecessor});
    refusal(h, dry, {dry.route->row_count_, 0, 1, Head::predecessor});
    const Block *unknown = &dry.storage->checking_;
    CHECK(dry.route->resolve_physical_prefix(terminal, 32767, unknown).code() == StatusCode::unsupported && !unknown);
    // Pure retained authority remains valid after real, external Session I/O;
    // readonly stable() deliberately fails and is never silently rebased.
    CHECK(h.session.read_blocks(2, std::as_writable_bytes(std::span{dry.storage->checking_})));
    CHECK(dry.route->retained_exact() && !dry.route->stable());
    const Block *expected{}; CHECK(dry.route->resolve_physical_prefix(terminal, 1, expected));
    CHECK(*expected == load(golden, 1));
    auto saved = dry.storage->rows_[0].plan.reclaim.after[0];
    dry.storage->rows_[0].plan.reclaim.after[0][900] ^= 1;
    CHECK(!dry.route->resolve_physical_prefix(terminal, 1, expected) && !expected);
    dry.storage->rows_[0].plan.reclaim.after[0] = saved;
    CHECK(dry.route->retained_exact());
    std::printf("CASE physical prefix members=%zu original_mask=%u states=%zu independent_goldens=%zu\n",
        fixture.members, original_mask, prefix_states, goldens); std::fflush(stdout);
}

void owner_phase_split(const ActiveFixture &fixture, bool raw_failure = false)
{
    using Mount = Ext4MountedMetadata<2>;
    owned_fixture::Disk disk(fixture.mixture(0)); disk.origin = 37;
    Registry registry; auto registration = registry.register_device(disk, 38103); CHECK(registration);
    auto workspace = std::make_unique<Ext4MountedStartupWorkspace>();
    auto mount = std::make_unique<Mount>(Ext4MountedPolicy::ordered_data_dirty_noatime, nullptr, workspace.get());
    auto &owner = mount->owner_;
    mount->report_.phase = Ext4MountPhase::opening;
    CHECK(workspace->claim(mount.get(), &owner));
    CHECK(owner.open(registry, registration.value(), {37, 65536}));
    mount->identity_ = owner.buffers();
    owner.startup_workspace_ = workspace.get(); owner.startup_mount_ = mount.get(); owner.startup_epoch_ = 1;
    owner.startup_reader_ = &mount->startup_reader_; owner.startup_ordinary_reader_ = &mount->startup_ordinary_reader_;
    owner.startup_identity_ = owner.buffers(); owner.reclaim_io_ = owner.cache_.session_.report().value(); owner.publication_domain_ = 1;
    owner.startup_active_ = true; owner.startup_generation_ = 1; owner.report_.phase = Ext4OwnedPhase::preparing;
    owner.startup_report_.phase = Ext4StartupPhase::classifying;
    owner.startup_report_.original = owner.startup_report_.observed = owner.reclaim_io_;
    CHECK(owner.startup_domain_guard());
    CHECK(owner.startup_route_budget_.reserve_owned_opening(owner.cache_.session_, &owner)); owner.startup_route_opening_ = true;
    // Split only before the installed classifier's refusal: real original
    // binding, capture, complete result and all dry rows still execute here.
    CHECK(owner.startup_reclaim_binding_.inspect(owner.cache_.session_, workspace->active_reclaim_,
        workspace->verification_, owner.startup_route_budget_));
    const auto &proof = owner.startup_reclaim_binding_.report();
    owner.reclaim_io_.reads += proof.reads; owner.reclaim_io_.completed_reads += proof.reads;
    owner.startup_report_.initial_family = Ext4StartupFamily::reclaim_active;
    owner.startup_report_.initial_sequence = fixture.wal.sequence;
    CHECK(owner.startup_reclaim_result_history());
    const auto io = owner.reclaim_io_; const auto disk_reads = disk.reads;
    // Hidden per-slot work must refuse even while the aggregate stays idle.
    const auto budget = owner.startup_route_budget_.report();
    for (usize slot = 0; slot < owner.cache_.entries_.size(); ++slot)
        for (usize field = 0; field < 6; ++field)
        {
            auto &entry = owner.cache_.entries_[slot].report; const auto saved = entry;
            if (field == 0) entry.state = MetadataBufferState::loading;
            if (field == 1) entry.read_pins = 1;
            if (field == 2) entry.transaction_serial = 7;
            if (field == 3) entry.staged_dirty = true;
            if (field == 4) entry.state = MetadataBufferState::failed;
            if (field == 5) entry.state = MetadataBufferState(99);
            const auto aggregate = owner.cache_.report();
            CHECK(!aggregate.in_flight && !aggregate.read_pins && !aggregate.transaction_active);
            CHECK(!owner.startup_handoff_cache_quiescent());
            CHECK(owner.startup_route_opening_ && owner.startup_route_budget_.report().phase == budget.phase);
            CHECK(disk.reads == disk_reads && !disk.writes && !disk.flushes);
            entry = saved; CHECK(owner.startup_handoff_cache_quiescent()); ++raw_slot_controls;
        }
    if (raw_failure)
    {
        owner.cache_.entries_[7].report.staged_dirty = true;
        CHECK(!owner.startup_handoff_opening_route());
        CHECK(owner.startup_route_opening_ && owner.startup_route_budget_.report().phase == Ext4ReclaimRoutePhase::refined);
        CHECK(owner.cache_.session_.report().value().state == block::DeviceState::quarantined);
        CHECK(disk.reads == disk_reads && !disk.writes && !disk.flushes && !mount->root());
        ++raw_slot_controls; return;
    }
    CHECK(owner.startup_handoff_opening_route());
    CHECK(!owner.startup_route_opening_ && owner.startup_route_budget_.report().phase == Ext4ReclaimRoutePhase::handoff);
    CHECK(!owner.startup_route_budget_.control_.writer && !owner.transaction_.active_ && !mount->root());
    CHECK(reclaim_history::same_session(io, owner.reclaim_io_) && disk.reads == disk_reads && !disk.writes && !disk.flushes);
    CHECK(owner.startup_reclaim_composed_.retained_exact());
    CHECK(!owner.startup_handoff_opening_route());
    CHECK(owner.cache_.session_.report().value().state == block::DeviceState::quarantined);
    CHECK(disk.reads == disk_reads && !disk.writes && !disk.flushes && !mount->root());
    ++owner_handoffs;
}
}
int main(int argc, char **argv)
{
    CHECK(argc == 2); const std::string root = argv[1];
    const auto image = [&](const char *name) { return read_image((root + "/" + name).c_str()); };
    ActiveFixture zero(image("input-empty.img")), one(image("input-chain2-cross-group.img")), seven(image("input-chain8-cross-group.img"));
    prefix_case(zero, 0, image("finished-empty.img"));
    prefix_case(one, 0, image("finished-chain2.img"));
    prefix_case(seven, 0, image("finished-chain8.img"));
    prefix_case(one, 3, image("finished-chain2.img"));
    owner_phase_split(zero); owner_phase_split(zero, true);
    std::printf("PASS physical prefix hosted states=%zu blocks=%zu refusals=%zu future_images_not_projected=%zu full_goldens=%zu owner_handoffs=%zu raw_slot_controls=%zu original_readonly_seals_retained=1 writer_admitted=0 native=0\n",
        prefix_states, prefix_blocks, prefix_refusals, future_changes, goldens, owner_handoffs, raw_slot_controls);
}
