// Host device/cache integration model. Private injection is test-only.
#include "ext4_namespace_journal_fixture.hpp"
#include "ext4_owned_support.hpp"
#include <memory>

using Owner = ok::fs::Ext4OwnedTransaction<1, 8>;
using Lifetime = ok::fs::Ext4InodeLifetimeTable<128, 512>;
using namespace namespace_journal_model;

namespace
{
usize accepted{}, rejected{}, preflight_rejected{}, read_faults{};

Image read_image(const char *path)
{
    std::ifstream input(path, std::ios::binary); CHECK(input);
    Image bytes((std::istreambuf_iterator<char>(input)), {});
    CHECK(bytes.size() == 32 * 1024 * 1024); return bytes;
}
Image dirty_empty(Image image)
{
    fixture::w32(image, 1120, fixture::r32(image, 1120) | 4U);
    fixture::super_crc(image); return image;
}
Ext4NamespaceUpdate operation(const Image &image, bool empty, bool replace)
{
    owned_fixture::Disk disk(image); Ext4RecoveryBinding binding;
    CHECK(binding.inspect(disk, {0, image.size() / 512}));
    Fixture fixture(image, *binding.info(), empty ? 14 : replace ? 14 : 12, 15);
    return replace ? fixture.replacement("basic-note", "second-note") :
        fixture.unlink(empty ? "empty-note" : "seed");
}

struct Harness
{
    owned_fixture::Disk disk;
    Owner::Registry registry;
    std::unique_ptr<Owner> owner = std::make_unique<Owner>(true);
    Lifetime lifetime;
    Ext4InodeLifetimeKey key{};
    Lifetime::Ticket ticket{};
    Owner::Registry::Registration registration{};
    std::unique_ptr<Ext4OrphanChainCaptureStorage> storage =
        std::make_unique<Ext4OrphanChainCaptureStorage>();
    std::unique_ptr<Ext4OrphanDirectoryQualifiedReclaimScratch> scratch =
        std::make_unique<Ext4OrphanDirectoryQualifiedReclaimScratch>();
    usize writes{}, flushes{}, reads{};
    u64 transaction{}, session{};

    Harness(const Image &image, bool empty = false, bool replace = false, u64 origin = 37)
        : disk(dirty_empty(image))
    {
        disk.origin = origin;
        const auto registered = registry.register_device(disk, 2049);
        CHECK(registered); registration = registered.value();
        CHECK(owner->open(registry, registration, {origin, image.size() / 512}));
        const auto update = operation(disk.live, empty, replace);
        CHECK(owner->prepare_namespace(update)); CHECK(owner->commit()); CHECK(owner->checkpoint());
        CHECK(owner->report().phase == Ext4OwnedPhase::orphan_cleanup_pending);
        CHECK(owner->report().orphan_cleanup_pending && owner->report().completion.complete());
        const auto buffers = owner->buffers();
        CHECK(buffers.state == MetadataOwnerState::ready && !buffers.read_pins && !buffers.transaction_active);
        key = {owner.get(), buffers.session_serial, replace ? update.destination : update.source,
               replace ? update.destination_generation : update.source_generation};
        CHECK(lifetime.observe(key)); CHECK(lifetime.mark_orphan(key));
        dispatch();
        writes = disk.writes; flushes = disk.flushes; reads = disk.reads;
        transaction = owner->cache_.transaction_serial_; session = buffers.session_serial;
    }
    void dispatch()
    {
        CHECK(lifetime.schedule_exact(key, [&](const Lifetime::Ticket &issued) -> Status {
            ticket = issued; return {};
        }));
    }
    Status prepare()
    { return owner->prepare_orphan_reclaim(lifetime, ticket, key, *storage, *scratch, 1700000444, 123456789); }
    void untouched() const
    {
        CHECK(disk.writes == writes && disk.flushes == flushes && disk.live == disk.durable);
        CHECK(owner->buffers().session_serial == session);
        CHECK(lifetime.validate_dispatched(ticket, key));
    }
    void failed() const
    {
        untouched(); CHECK(owner->report().phase == Ext4OwnedPhase::failed);
        CHECK(owner->report().orphan_cleanup_pending && !owner->report().orphan_reclaim_prepared);
        CHECK(owner->buffers().state == MetadataOwnerState::failed);
        CHECK(registry.inspect(registration).value().state == block::DeviceState::quarantined);
        CHECK(!owner->close() && !owner->commit() && !owner->checkpoint());
    }
};

usize successful(const Image &image, bool empty, bool replace, u64 origin, bool prefill = true)
{
    Harness h(image, empty, replace, origin);
    const auto live = h.disk.live;
    // A cache-resident victim data block must be retired without writeback.
    const auto offset = fixture::inode_offset(live, h.key.inode);
    const auto physical = fixture::r32(live, offset + 60);
    usize slot = 8; u64 generation{};
    if (!empty && prefill)
    {
        Owner::Pin data;
        CHECK(h.owner->cache_.pin(physical, data)); slot = data.slot_; generation = data.generation_;
        CHECK(data.release());
    }
    const auto before_prepare_reads = h.disk.reads;
    CHECK(h.prepare()); h.untouched(); ++accepted;
    CHECK(h.disk.live == live && h.disk.reads > h.reads);
    CHECK(h.owner->report().phase == Ext4OwnedPhase::orphan_reclaim_prepared &&
          h.owner->report().orphan_reclaim_prepared && h.owner->report().orphan_cleanup_pending);
    const auto &plan = h.owner->reclaim_plan_.reclaim;
    CHECK(plan.inode == h.key.inode && plan.old_generation == h.key.generation);
    CHECK(plan.count == (empty ? 4U : 5U) && plan.released_blocks == (empty ? 0U : replace ? 1U : 4U));
    CHECK(h.owner->buffers().read_pins == plan.count + 1 && h.owner->buffers().transaction_active);
    CHECK(h.owner->cache_.transaction_serial_ == h.transaction + 1);
    CHECK(h.owner->reclaim_capture_.report().original_io.writes == h.writes &&
          h.owner->reclaim_capture_.report().original_io.flushes == h.flushes);
    if (slot < 8)
    {
        const auto &entry = h.owner->cache_.entries_[slot];
        CHECK(entry.report.generation > generation &&
              (entry.report.state != MetadataBufferState::ready || entry.report.block != physical));
    }
    Owner::Pin blocked;
    CHECK(!h.owner->pin(1, blocked) && !h.owner->prepare(owned_fixture::update));
    CHECK(!h.owner->commit() && !h.owner->checkpoint() && !h.owner->discard() &&
          !h.owner->close() && !h.owner->rearm() && !h.prepare());
    h.untouched();
    CHECK(h.lifetime.snapshot(h.key).value().phase == Ext4InodeLifetimePhase::dispatched);
    return h.disk.reads - before_prepare_reads;
}

void initial_rejections(const Image &image)
{
    Harness h(image);
    const auto reads = h.disk.reads;
    auto wrong = h.key; ++wrong.session;
    CHECK(!h.owner->prepare_orphan_reclaim(h.lifetime, h.ticket, wrong, *h.storage, *h.scratch, 1, 0));
    wrong = h.key; ++wrong.inode;
    CHECK(!h.owner->prepare_orphan_reclaim(h.lifetime, h.ticket, wrong, *h.storage, *h.scratch, 1, 0));
    wrong = h.key; ++wrong.generation;
    CHECK(!h.owner->prepare_orphan_reclaim(h.lifetime, h.ticket, wrong, *h.storage, *h.scratch, 1, 0));
    Lifetime foreign; CHECK(foreign.observe(h.key)); CHECK(foreign.mark_orphan(h.key));
    Lifetime::Ticket foreign_ticket;
    CHECK(foreign.schedule_exact(h.key, [&](const auto &issued) -> Status { foreign_ticket = issued; return {}; }));
    CHECK(!h.owner->prepare_orphan_reclaim(h.lifetime, foreign_ticket, h.key, *h.storage, *h.scratch, 1, 0));
    Lifetime::Ticket stale = h.ticket;
    CHECK(h.lifetime.retry(h.ticket)); h.dispatch();
    CHECK(!h.owner->prepare_orphan_reclaim(h.lifetime, stale, h.key, *h.storage, *h.scratch, 1, 0));
    CHECK(h.disk.reads == reads && h.owner->report().phase == Ext4OwnedPhase::orphan_cleanup_pending);
    h.owner->admissions_ = 1; CHECK(!h.prepare()); h.owner->admissions_ = 0;
    Owner::Pin retained; CHECK(h.owner->cache_.pin(1, retained));
    CHECK(!h.prepare()); CHECK(retained.release());
    CHECK(h.owner->report().phase == Ext4OwnedPhase::orphan_cleanup_pending);
    h.untouched(); preflight_rejected += 7;
    CHECK(h.prepare()); h.untouched(); ++accepted;
}

void hostile(const Image &image)
{
    {
        Harness h(image); // Original sealed namespace participant is still enforced after eviction.
        auto &cache = h.owner->cache_;
        for (auto &entry : cache.entries_)
            if (entry.report.state == MetadataBufferState::ready && entry.report.block == 1)
                entry.canonical[900] ^= std::byte{1};
        CHECK(!h.prepare()); h.failed(); ++rejected;
    }
    {
        Harness h(image); // Source changed out of band; no writable preparation follows.
        h.disk.live[1024 + 900] ^= 1; h.disk.durable = h.disk.live;
        CHECK(!h.prepare()); h.failed(); ++rejected;
    }
    {
        Harness h(image);
        h.owner->cache_.transaction_serial_ = ~u64{0};
        CHECK(!h.prepare()); h.failed(); ++rejected;
    }
    {
        Harness h(image);
        for (auto &entry : h.owner->cache_.entries_) entry.report.generation = ~u64{0};
        CHECK(!h.prepare()); h.failed(); ++rejected;
    }
    {
        Harness h(image);
        bool done{};
        h.disk.hook = [&](char kind) {
            if (!done && kind == 'r') { done = true; CHECK(h.lifetime.retry(h.ticket)); h.dispatch(); }
        };
        CHECK(!h.prepare()); h.failed(); ++rejected;
    }
    {
        Harness h(image);
        CHECK(!h.owner->prepare_orphan_reclaim(h.lifetime, h.ticket, h.key, *h.storage, *h.scratch,
                                              1, 1000000000));
        h.failed(); ++rejected;
    }
}

void faults(const Image &image, usize read_count)
{
    // Faults at bootstrap, table sweep, directory capture, plan reread and final validation.
    const std::array<usize, 8> offsets{1, 2, 8, 32, read_count / 4, read_count / 2,
                                     read_count * 3 / 4, read_count};
    for (const auto offset : offsets)
    {
        Harness h(image); h.disk.fail_read = h.disk.reads + offset;
        CHECK(!h.prepare()); h.failed(); ++read_faults;
    }
}
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    const auto filled = read_image(argv[1]), empty = read_image(argv[2]);
    successful(filled, false, false, 37);
    const auto reads = successful(filled, false, false, 37, false);
    successful(filled, false, true, 0); successful(empty, true, false, 37);
    initial_rejections(filled); hostile(filled); faults(filled, reads);
    CHECK(accepted == 5 && preflight_rejected == 7 && rejected == 6 && read_faults == 8);
    std::printf("PASS private orphan owner preparation: accepted=%zu preflight_rejects=%zu hostile_rejects=%zu read_faults=%zu same_session=1 no_new_write_flush=1 no_free_ack=1 publication_disabled=1\n",
                accepted, preflight_rejected, rejected, read_faults);
    return 0;
}
