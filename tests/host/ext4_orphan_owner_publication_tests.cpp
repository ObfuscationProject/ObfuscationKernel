// Host device integration, not guest execution. Private access is test-only.
#define main preparation_regression_main
#include "ext4_orphan_owner_preparation_tests.cpp"
#undef main
#include <iostream>

namespace publication_model
{
usize successes{}, write_faults{}, flush_faults{}, read_faults{}, hostile_rejections{}, sealed_callbacks{};
struct ReclaimHarness : Harness
{
    std::unique_ptr<Ext4OrphanReclaimCaptureStorage> published = std::make_unique<Ext4OrphanReclaimCaptureStorage>();
    std::unique_ptr<Ext4OrphanReclaimColdScratch> proof = std::make_unique<Ext4OrphanReclaimColdScratch>();
    ReclaimHarness(const Image &image, bool empty = false, bool replace = false, u64 origin = 37)
        : Harness(image, empty, replace, origin) { CHECK(prepare()); }
    Status publish() { return owner->publish_orphan_reclaim(*published, *proof); }
    Status checkpoint() { return owner->checkpoint_orphan_reclaim(*published, *proof); }
    void quarantined()
    {
        CHECK(owner->report().phase == Ext4OwnedPhase::failed && owner->report().orphan_cleanup_pending);
        CHECK(!owner->report().orphan_reclaimed);
        CHECK(owner->buffers().state == MetadataOwnerState::failed);
        CHECK(registry.inspect(registration).value().state == block::DeviceState::quarantined);
        CHECK(lifetime.snapshot(key).value().phase == Ext4InodeLifetimePhase::publishing);
        CHECK(!lifetime.retry(ticket) && !lifetime.acknowledge_reclaimed(ticket));
        CHECK(!lifetime.schedule_exact(key, [](const auto &) -> Status { return {}; }));
        CHECK(!owner->rearm() && !owner->close() && !owner->commit() && !owner->checkpoint());
        CHECK(!publish() && !checkpoint());
    }
};
struct Counts { usize publication_reads{}, checkpoint_reads{}, publication_writes{}, checkpoint_writes{}; };
Counts successful(const Image &image, bool empty, bool replace, u64 origin)
{
    ReclaimHarness h(image, empty, replace, origin);
    const auto original = h.disk.live;
    const auto homes = h.owner->reclaim_plan_.reclaim;
    const auto journal = h.owner->reclaim_journal_;
    const auto start_reads = h.disk.reads, start_writes = h.disk.writes, start_flushes = h.disk.flushes;
    const auto trace_start = h.disk.trace.size();
    // Lifetime APIs are invoked inside every actual transport callback. They
    // must return with the table lock released and cannot alter the decision.
    h.disk.hook = [&](char) {
        CHECK(h.lifetime.snapshot(h.key).value().phase == Ext4InodeLifetimePhase::publishing);
        CHECK(!h.lifetime.retry(h.ticket) && !h.lifetime.acknowledge_reclaimed(h.ticket));
        CHECK(!h.lifetime.observe(h.key) && !h.lifetime.retain(h.key, Ext4InodeHold::descriptor));
        CHECK(!h.lifetime.schedule_exact(h.key, [](const auto &) -> Status { return {}; }));
        ++sealed_callbacks;
    };
    CHECK(h.publish());
    Counts counts{h.disk.reads - start_reads, 0, h.disk.writes - start_writes, 0};
    CHECK(h.disk.writes - start_writes == homes.count + 4 && h.disk.flushes - start_flushes == 4);
    CHECK(h.disk.live == h.disk.durable && h.disk.pending.empty());
    CHECK(h.owner->report().phase == Ext4OwnedPhase::orphan_reclaim_durable);
    for (usize index = 0; index < homes.count; ++index)
        CHECK(std::equal(homes.before[index].begin(), homes.before[index].end(), h.disk.live.begin() + homes.targets[index] * 1024));
    std::vector<char> order;
    for (usize i = trace_start; i < h.disk.trace.size(); ++i) if (h.disk.trace[i].operation != 'r') order.push_back(h.disk.trace[i].operation);
    std::vector<char> expected{'f'}; expected.insert(expected.end(), homes.count + 2, 'w');
    expected.insert(expected.end(), {'f','w','f','w','f'}); CHECK(order == expected);
    CHECK(!h.owner->commit() && !h.owner->checkpoint() && !h.owner->close() && !h.owner->rearm());
    const auto checkpoint_reads = h.disk.reads, checkpoint_writes = h.disk.writes, checkpoint_flushes = h.disk.flushes;
    const auto checkpoint_trace = h.disk.trace.size();
    CHECK(h.checkpoint());
    counts.checkpoint_reads = h.disk.reads - checkpoint_reads; counts.checkpoint_writes = h.disk.writes - checkpoint_writes;
    CHECK(counts.checkpoint_writes == homes.count + 1 && h.disk.flushes - checkpoint_flushes == 4);
    CHECK(h.disk.live == h.disk.durable && h.disk.pending.empty());
    order.clear();
    for (usize i = checkpoint_trace; i < h.disk.trace.size(); ++i) if (h.disk.trace[i].operation != 'r') order.push_back(h.disk.trace[i].operation);
    expected = {'w','f'}; expected.insert(expected.end(), homes.count - 2, 'w');
    expected.insert(expected.end(), {'f','w','f','w','f'}); CHECK(order == expected);
    CHECK(h.owner->report().phase == Ext4OwnedPhase::checkpointed && h.owner->report().orphan_reclaimed);
    CHECK(!h.owner->report().orphan_cleanup_pending && !h.owner->report().orphan_reclaim_prepared);
    CHECK(!h.lifetime.snapshot(h.key) && h.lifetime.active_records() == 0);
    CHECK(h.owner->buffers().read_pins == 0 && !h.owner->buffers().transaction_active);
    CHECK(h.owner->buffers().session_serial == h.session && h.owner->cache_.transaction_serial_ == h.transaction + 1);
    for (usize index = 0; index < homes.count; ++index)
        CHECK(std::equal(homes.after[index].begin(), homes.after[index].end(), h.disk.live.begin() + homes.targets[index] * 1024));
    const usize journal_at = h.owner->blocks_[0] * 1024;
    CHECK(fixture::be32(h.disk.live, journal_at + 24) == journal.sequence + 2 && fixture::be32(h.disk.live, journal_at + 28) == 0);
    // Only homes and admitted journal records can change. Released data stays
    // intact until subsequent allocator reuse, and directory data is untouched.
    for (usize block = 0; block < original.size() / 1024; ++block)
    {
        bool changed = block >= journal_at / 1024 && block <= journal_at / 1024 + journal.count + 3;
        for (usize i = 0; i < homes.count; ++i) changed = changed || block == homes.targets[i];
        if (!changed) CHECK(std::equal(original.begin() + block * 1024, original.begin() + (block + 1) * 1024, h.disk.live.begin() + block * 1024));
    }
    h.disk.hook = {};
    const auto reports = h.owner->session_report().value();
    CHECK(h.owner->rearm()); CHECK(h.owner->report().phase == Ext4OwnedPhase::open);
    CHECK(Owner::exact_session(h.owner->session_report().value(), reports));
    CHECK(h.owner->close());
    ++successes; return counts;
}
void fault_campaign(const Image &image, const Counts &counts)
{
    for (usize offset = 1; offset <= counts.publication_writes + counts.checkpoint_writes; ++offset)
        for (usize cut : {usize{0}, usize{512}, usize{1024}})
        {
            ReclaimHarness h(image);
            h.disk.fail_write = h.disk.writes + offset; h.disk.cut = cut;
            // Preserve preceding group effects as well as a partial current
            // write. This is deliberately stronger than failure-before-effect.
            h.disk.persist_mask = 0xffffffffU;
            const auto published = h.publish();
            CHECK(!published || !h.checkpoint()); h.quarantined(); ++write_faults;
        }
    for (usize offset = 1; offset <= 8; ++offset)
        for (bool after : {false, true})
        {
            ReclaimHarness h(image);
            if (after) h.disk.fail_flush_after = h.disk.flushes + offset;
            else { h.disk.fail_flush = h.disk.flushes + offset; h.disk.persist_mask = 0xffffffffU; }
            const auto published = h.publish(); CHECK(!published || !h.checkpoint());
            h.quarantined(); ++flush_faults;
        }
    const std::array<usize, 8> reads{1, counts.publication_reads / 2, counts.publication_reads,
        counts.publication_reads + 1, counts.publication_reads + counts.checkpoint_reads / 2,
        counts.publication_reads + counts.checkpoint_reads - 5,
        counts.publication_reads + counts.checkpoint_reads - 1,
        counts.publication_reads + counts.checkpoint_reads};
    for (usize offset : reads)
    {
        ReclaimHarness h(image); h.disk.fail_read = h.disk.reads + offset;
        const auto published = h.publish(); CHECK(!published || !h.checkpoint());
        h.quarantined(); ++read_faults;
    }
}
void hostile(const Image &image)
{
    {
        ReclaimHarness h(image); CHECK(h.lifetime.retry(h.ticket)); h.dispatch();
        CHECK(!h.publish()); CHECK(h.disk.writes == h.writes && h.disk.flushes == h.flushes);
        CHECK(h.owner->report().orphan_cleanup_pending); ++hostile_rejections;
    }
    {
        ReclaimHarness h(image); h.disk.live[1024 + 900] ^= 1;
        CHECK(!h.publish()); h.quarantined(); CHECK(h.disk.writes == h.writes); ++hostile_rejections;
    }
    {
        ReclaimHarness h(image); h.owner->reclaim_plan_.reclaim.before[0][900] ^= 1;
        CHECK(!h.publish()); h.quarantined(); CHECK(h.disk.writes == h.writes); ++hostile_rejections;
    }
    {
        ReclaimHarness h(image); bool done{};
        h.disk.hook = [&](char kind) {
            if (kind == 'w' && !done)
            {
                done = true; auto &entry = h.owner->cache_.entries_[h.owner->pins_[0].slot_];
                entry.staged[900] ^= std::byte{1};
            }
        };
        CHECK(!h.publish()); h.quarantined(); ++hostile_rejections;
    }
    {
        ReclaimHarness h(image); CHECK(h.publish());
        const auto journal_at = h.owner->blocks_[0] * 1024;
        h.disk.live[journal_at + 1024 + 900] ^= 1;
        CHECK(!h.checkpoint()); h.quarantined(); ++hostile_rejections;
    }
    {
        ReclaimHarness h(image); CHECK(h.publish());
        h.owner->authority_.generations[0]++;
        CHECK(!h.checkpoint()); h.quarantined(); ++hostile_rejections;
    }
    {
        ReclaimHarness h(image); CHECK(h.publish());
        // Valid successful extra I/O is still an unadmitted session user.
        Ext4OrphanBlock extra{}; CHECK(h.owner->bound_device().read_blocks(2, std::as_writable_bytes(std::span{extra})));
        CHECK(!h.checkpoint()); h.quarantined(); ++hostile_rejections;
    }
}
}
int main(int argc, char **argv)
{
    CHECK(argc == 3); const auto filled = read_image(argv[1]), empty = read_image(argv[2]);
    const auto counts = publication_model::successful(filled, false, false, 37);
    publication_model::successful(filled, false, true, 0);
    publication_model::successful(empty, true, false, 37);
    publication_model::hostile(filled); publication_model::fault_campaign(filled, counts);
    std::cout << "owner publication successes=" << publication_model::successes
              << " write_faults=" << publication_model::write_faults
              << " flush_faults=" << publication_model::flush_faults
              << " read_faults=" << publication_model::read_faults
              << " hostile=" << publication_model::hostile_rejections
              << " sealed_callbacks=" << publication_model::sealed_callbacks << '\n';
}
