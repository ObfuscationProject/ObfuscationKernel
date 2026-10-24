#include <ok/fs/metadata_buffers.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <type_traits>
#include <vector>

// Privileged hosted stand-in for the existing canonical friend, not an attached
// production owner, mounted exclusion protocol, typed writer or durable device.
namespace ok::fs
{
template <usize Capacity, usize Devices> class Ext4OwnedTransaction
{
  public:
    using Cache = MetadataBuffers<Capacity, 1024, Devices>;
    using Plan = typename Cache::TypedReconciliation;
    using Change = typename Cache::TypedCacheChange;
    using Action = typename Cache::TypedCacheAction;
    Ext4OwnedTransaction() = default;
    ~Ext4OwnedTransaction() = default;
    Plan plan{};
    struct Snapshot
    {
        MetadataOwnerReport report{};
        std::array<typename Cache::Entry, Capacity> entries{};
        usize sectors{};
        u64 serial{};
        typename Cache::Transaction *transaction{};
        bool writable{};
        bool operator==(const Snapshot &b) const
        {
            if (!Cache::same_report(report, b.report) || sectors != b.sectors || serial != b.serial ||
                transaction != b.transaction || writable != b.writable) return false;
            for (usize n = 0; n < Capacity; ++n)
                if (!Cache::same_report(entries[n].report, b.entries[n].report) ||
                    entries[n].canonical != b.entries[n].canonical || entries[n].staged != b.entries[n].staged)
                    return false;
            return true;
        }
    };
    static Snapshot snapshot(const Cache &cache)
    { return {cache.report_, cache.entries_, cache.sectors_per_block_, cache.transaction_serial_, cache.transaction_, cache.writable_}; }
    static void restore(Cache &cache, const Snapshot &snapshot)
    {
        cache.report_ = snapshot.report; cache.entries_ = snapshot.entries; cache.sectors_per_block_ = snapshot.sectors;
        cache.transaction_serial_ = snapshot.serial; cache.transaction_ = snapshot.transaction; cache.writable_ = snapshot.writable;
    }
    static usize slot(const Cache &cache, u64 block)
    {
        for (usize n = 0; n < Capacity; ++n)
            if (cache.entries_[n].report.state == MetadataBufferState::ready && cache.entries_[n].report.block == block) return n;
        return Capacity;
    }
    static Status prepare(Cache &cache, std::span<const Change> changes, Ext4OwnedTransaction &owner)
    { return cache.prepare_typed_reconciliation(changes, owner.plan); }
    static Status normalize(Cache &cache, Ext4OwnedTransaction &owner)
    { return cache.normalize_typed_reconciliation(owner.plan); }
    static Status publish(Cache &cache, Ext4OwnedTransaction &owner)
    { return cache.publish_typed_reconciliation(owner.plan); }
    static auto &entry(Cache &cache, usize slot) { return cache.entries_[slot]; }
    static auto &report(Cache &cache) { return cache.report_; }
    static auto &serial(Cache &cache) { return cache.transaction_serial_; }
    static auto &transaction(Cache &cache) { return cache.transaction_; }
    static auto &sectors(Cache &cache) { return cache.sectors_per_block_; }
    static auto &writable(Cache &cache) { return cache.writable_; }
    static Status direct_read(Cache &cache, std::span<std::byte> out) { return cache.session_.read_blocks(0, out); }
    static Status quarantine(Cache &cache) { return cache.session_.abort(Status::interrupted("model Session quarantine")); }
    static void clone(Ext4OwnedTransaction &out, const Ext4OwnedTransaction &in)
    { std::memcpy(static_cast<void *>(&out.plan), static_cast<const void *>(&in.plan), sizeof(Plan)); }
    static bool normalized(const Ext4OwnedTransaction &owner)
    { return owner.plan.phase_ == Plan::Phase::normalized; }
    static bool failed(const Ext4OwnedTransaction &owner)
    { return owner.plan.phase_ == Plan::Phase::failed; }
    static void corrupt_plan(Ext4OwnedTransaction &owner, usize field)
    {
        auto &p = owner.plan;
        switch (field)
        {
            case 0: p.entries_[2].canonical[500] ^= std::byte{1}; break;
            case 1: p.entries_[2].staged[501] ^= std::byte{1}; break;
            case 2: ++p.entries_[2].report.generation; break;
            case 3: p.final_images_[0][502] ^= std::byte{1}; break;
            case 4: ++p.final_reports_[0].generation; break;
            case 5: p.actions_[0] = Action::retire; break;
            case 6: ++p.targets_[0]; break;
            case 7: p.target_actions_[0] = Action::retire; break;
            case 8: ++p.targets_count_; break;
            case 9: ++p.resident_after_; break;
            case 10: ++p.sectors_per_block_; break;
            case 11: ++p.transaction_serial_; break;
            case 12: p.writable_ = false; break;
            case 13: ++p.report_.first_sector; break;
            case 14: ++p.seal_; break;
            case 15: p.session_ = nullptr; break;
            case 16: p.cache_ = nullptr; break;
            case 17: p.original_ = nullptr; break;
        }
    }
};
}

using namespace ok;
using namespace ok::fs;
using Owner = Ext4OwnedTransaction<4, 2>;
using Cache = Owner::Cache;
using Image = std::array<std::byte, 1024>;
usize checks{}, refusals{}, drift_checks{};
void check(bool condition, const char *why)
{
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL typed cache reconciliation: %s\n", why); std::exit(1); }
}
struct Device final : driver::BlockDevice
{
    std::vector<std::byte> bytes = std::vector<std::byte>(64 * 512);
    usize reads{}, writes{}, flushes{};
    std::function<void()> callback{};
    Device() { for (usize n = 0; n < bytes.size(); ++n) bytes[n] = std::byte{static_cast<u8>(n * 11 + n / 256 + 1)}; }
    driver::BlockGeometry geometry() const override { return {64, 512, true}; }
    Status read_blocks(u64 first, std::span<std::byte> out) override
    {
        ++reads;
        if (callback) callback();
        if (first >= 64 || out.size() > bytes.size() - first * 512) return Status::io_error("model read extent");
        std::copy_n(bytes.begin() + first * 512, out.size(), out.begin()); return {};
    }
    Status write_blocks(u64, std::span<const std::byte>) override { ++writes; return Status::fault("unexpected write"); }
    Status flush() override { ++flushes; return Status::fault("unexpected flush"); }
};
struct Fixture
{
    Device disk{};
    Cache::Registry registry{};
    Cache cache{};
    Owner owner{};
    Image old1{}, old2{}, old3{}, final{};
    Fixture()
    {
        const auto key = registry.register_device(disk, 17); check(key.ok(), "register original Session");
        check(cache.open(registry, key.value(), {0, 64}, block::SessionAccess::writable).ok(), "open writable cache");
        std::array<Cache::Pin, 3> pins{};
        for (usize n = 0; n < pins.size(); ++n) check(cache.pin(n + 1, pins[n]).ok(), "seed distinct residents");
        check(pins[0].read(0, old1).ok() && pins[1].read(0, old2).ok() && pins[2].read(0, old3).ok(), "retain exact old images");
        Cache::Transaction transaction{};
        std::array<Cache::Pin *, 1> selected{&pins[2]};
        final.fill(std::byte{0xe7});
        check(cache.begin(selected, transaction).ok() && transaction.edit(3, 0, final).ok() && transaction.discard().ok(), "leave discarded dormant staging");
        for (auto &pin : pins) check(pin.release().ok(), "drain authentic seed pins");
    }
    ~Fixture()
    {
        check(disk.writes == 0 && disk.flushes == 0, "helper never dispatches write or flush");
        if (cache.report().state == MetadataOwnerState::ready) check(cache.close().ok(), "drain original cache");
    }
    std::array<Owner::Change, 2> changes()
    { return {{{1, Owner::Action::replace, old1, final}, {2, Owner::Action::retire, old2, {}}}}; }
};
template <class Function> void refused(Fixture &f, Function function, const char *why)
{
    const auto before = Owner::snapshot(f.cache);
    const usize reads = f.disk.reads;
    check(!function(), why); ++refusals;
    check(Owner::snapshot(f.cache) == before && f.disk.reads == reads && !f.disk.writes && !f.disk.flushes,
          "refusal preserves all cache storage and has no I/O");
}
void current_drift(Fixture &f, usize field)
{
    auto &entry = Owner::entry(f.cache, 2);
    switch (field)
    {
        case 0: entry.canonical[777] ^= std::byte{1}; break;
        case 1: entry.staged[778] ^= std::byte{1}; break;
        case 2: ++entry.report.generation; break;
        case 3: entry.report.block = 7; break;
        case 4: entry.report.state = MetadataBufferState::unused; break;
        case 5: entry.report.read_pins = 1; break;
        case 6: entry.report.transaction_serial = 1; break;
        case 7: entry.report.staged_dirty = true; break;
        case 8: ++Owner::report(f.cache).resident_blocks; break;
        case 9: ++Owner::report(f.cache).read_pins; break;
        case 10: Owner::report(f.cache).in_flight = true; break;
        case 11: Owner::report(f.cache).transaction_active = true; break;
        case 12: ++Owner::report(f.cache).device_identity; break;
        case 13: ++Owner::report(f.cache).registration_generation; break;
        case 14: ++Owner::report(f.cache).session_serial; break;
        case 15: ++Owner::report(f.cache).first_sector; break;
        case 16: ++Owner::report(f.cache).sectors; break;
        case 17: ++Owner::report(f.cache).blocks; break;
        case 18: Owner::report(f.cache).first_error = StatusCode::fault; break;
        case 19: Owner::report(f.cache).state = MetadataOwnerState::closed; break;
        case 20: ++Owner::serial(f.cache); break;
        case 21: ++Owner::sectors(f.cache); break;
        case 22: Owner::writable(f.cache) = false; break;
        case 23: Owner::entry(f.cache, 3).canonical[17] ^= std::byte{1}; break;
        case 24: Owner::entry(f.cache, 3).staged[18] ^= std::byte{1}; break;
        case 25: Owner::entry(f.cache, 3).report.generation = 1; break;
    }
}
int main()
{
    static_assert(!std::is_copy_constructible_v<Owner> && !std::is_move_constructible_v<Owner>);
    {
        auto f = std::make_unique<Fixture>();
        auto changes = f->changes(); const auto original = Owner::snapshot(f->cache);
        check(original.entries[0].canonical != original.entries[0].staged &&
              original.entries[2].canonical != original.entries[2].staged, "miss and discard stages are dormant distinct bytes");
        const usize reads = f->disk.reads;
        check(Owner::prepare(f->cache, changes, f->owner).ok() && Owner::snapshot(f->cache) == original, "preflight accepts and seals dormant stages without mutation");
        check(Owner::normalize(f->cache, f->owner).ok() && Owner::normalized(f->owner), "explicit normalization succeeds");
        auto normalized = Owner::snapshot(f->cache);
        check(normalized.entries[0].canonical == original.entries[0].canonical && normalized.entries[0].staged == original.entries[0].canonical &&
              normalized.entries[1].staged == original.entries[1].canonical && normalized.entries[2].staged == original.entries[2].staged &&
              normalized.report.resident_blocks == original.report.resident_blocks, "normalization preserves unrelated dormant stage");
        const auto retained_final = f->final;
        f->old1.fill(std::byte{0}); f->old2.fill(std::byte{0}); f->final.fill(std::byte{0}); changes = {};
        Image scratch{}; check(Owner::direct_read(f->cache, scratch).ok(), "original Session counters may advance after preflight");
        check(Owner::publish(f->cache, f->owner).ok(), "final publication uses retained images after input lifetime ends");
        const auto result = Owner::snapshot(f->cache);
        check(result.entries[0].canonical == retained_final && result.entries[0].staged == retained_final &&
              result.entries[0].report.generation == original.entries[0].report.generation + 1 &&
              result.entries[1].report.state == MetadataBufferState::unused && result.entries[1].canonical == Image{} &&
              result.entries[1].staged == Image{} && result.report.resident_blocks == 2 &&
              result.entries[2].canonical == original.entries[2].canonical && result.entries[2].staged == original.entries[2].staged &&
              f->disk.reads == reads + 1, "atomic replacement and retirement preserve unrelated complete state");
        refused(*f, [&]{ return Owner::publish(f->cache, f->owner); }, "published plan is one-shot");
    }
    for (usize malformed = 0; malformed < 15; ++malformed)
    {
        auto f = std::make_unique<Fixture>(); auto changes = f->changes();
        std::array<Owner::Change, 33> oversized{};
        std::span<const Owner::Change> request = changes;
        switch (malformed)
        {
            case 0: changes[1].block = changes[0].block; break;
            case 1: changes[1].block = 32; break;
            case 2: changes[1].expected = std::span<const std::byte>{f->old2}.first(1023); break;
            case 3: changes[0].replacement = std::span<const std::byte>{f->final}.first(1023); break;
            case 4: changes[1].replacement = f->final; break;
            case 5: changes[0].action = Owner::Action::unchanged; break;
            case 6: changes[0].expected = f->old2; break;
            case 7: changes[0].expected = Owner::entry(f->cache, 0).canonical; break;
            case 8: changes[0].replacement = Owner::entry(f->cache, 0).staged; break;
            case 9: changes[0].expected = {reinterpret_cast<const std::byte *>(&f->owner.plan), 1024}; break;
            case 10: changes[0].replacement = {reinterpret_cast<const std::byte *>(&f->owner.plan), 1024}; break;
            case 11: request = {reinterpret_cast<const Owner::Change *>(&f->cache), 1}; break;
            case 12: request = {reinterpret_cast<const Owner::Change *>(&f->owner.plan), 1}; break;
            case 13: request = oversized; break;
            case 14: changes[0].expected = {reinterpret_cast<const std::byte *>(~uptr{0} - 16), 1024}; break;
        }
        refused(*f, [&]{ return Owner::prepare(f->cache, request, f->owner); }, "malformed or aliased preflight refuses");
    }
    for (usize malformed = 0; malformed < 9; ++malformed)
    {
        auto f = std::make_unique<Fixture>(); const auto original = Owner::snapshot(f->cache); auto changes = f->changes();
        auto &entry = Owner::entry(f->cache, 2).report;
        switch (malformed)
        {
            case 0: entry.block = 1; break;
            case 1: entry.generation = 0; break;
            case 2: entry.block = 32; break;
            case 3: entry.state = MetadataBufferState::loading; break;
            case 4: entry.state = MetadataBufferState::failed; break;
            case 5: entry.state = MetadataBufferState::exhausted; break;
            case 6: entry.read_pins = 1; break;
            case 7: entry.transaction_serial = 1; break;
            case 8: entry.staged_dirty = true; break;
        }
        refused(*f, [&]{ return Owner::prepare(f->cache, changes, f->owner); }, "invalid resident or retained work refuses");
        Owner::restore(f->cache, original);
    }
    for (bool final_phase : {false, true})
        for (usize field = 0; field < 26; ++field)
        {
            auto f = std::make_unique<Fixture>(); const auto original = Owner::snapshot(f->cache); auto changes = f->changes();
            check(Owner::prepare(f->cache, changes, f->owner).ok(), "current drift preparation");
            if (final_phase) check(Owner::normalize(f->cache, f->owner).ok(), "current drift normalized prefix");
            current_drift(*f, field);
            refused(*f, [&]{ return final_phase ? Owner::publish(f->cache, f->owner) : Owner::normalize(f->cache, f->owner); }, "complete actual snapshot drift refuses");
            check(Owner::failed(f->owner), "original drift makes plan terminal"); ++drift_checks;
            Owner::restore(f->cache, original);
            refused(*f, [&]{ return Owner::normalize(f->cache, f->owner); }, "restoring cache cannot revive refused plan");
        }
    for (bool final_phase : {false, true})
        for (usize field = 0; field < 18; ++field)
        {
            auto f = std::make_unique<Fixture>(); auto changes = f->changes();
            check(Owner::prepare(f->cache, changes, f->owner).ok(), "retained drift preparation");
            if (final_phase) check(Owner::normalize(f->cache, f->owner).ok(), "retained drift normalized prefix");
            Owner::corrupt_plan(f->owner, field);
            refused(*f, [&]{ return final_phase ? Owner::publish(f->cache, f->owner) : Owner::normalize(f->cache, f->owner); }, "complete private plan drift refuses");
            ++drift_checks;
        }
    {
        auto f = std::make_unique<Fixture>(); auto other = std::make_unique<Fixture>(); auto changes = f->changes();
        check(Owner::prepare(f->cache, changes, f->owner).ok(), "original binding preparation");
        Owner::clone(other->owner, f->owner);
        refused(*f, [&]{ return Owner::normalize(f->cache, other->owner); }, "copied bytes do not transfer original plan authority");
        refused(*other, [&]{ return Owner::normalize(other->cache, f->owner); }, "foreign cache cannot use original plan");
        check(Owner::normalize(f->cache, f->owner).ok() && Owner::publish(f->cache, f->owner).ok(), "foreign refusal preserves authentic original plan");
    }
    {
        auto f = std::make_unique<Fixture>(); auto changes = f->changes();
        Owner::entry(f->cache, 0).report.generation = ~u64{0};
        refused(*f, [&]{ return Owner::prepare(f->cache, changes, f->owner); }, "replacement cannot wrap generation");
        changes[0].action = Owner::Action::retire; changes[0].replacement = {};
        check(Owner::prepare(f->cache, changes, f->owner).ok() && Owner::normalize(f->cache, f->owner).ok() &&
              Owner::publish(f->cache, f->owner).ok(), "retirement handles generation exhaustion explicitly");
        const auto exhausted = Owner::snapshot(f->cache);
        check(exhausted.entries[0].report.state == MetadataBufferState::exhausted && exhausted.entries[0].report.generation == ~u64{0} &&
              exhausted.entries[1].report.state == MetadataBufferState::unused && exhausted.report.resident_blocks == 1, "exhausted retired slot remains unreusable");
        Cache::Pin pin{}; check(f->cache.pin(7, pin).ok() && Owner::entry(f->cache, 0).report.state == MetadataBufferState::exhausted && pin.release().ok(), "ordinary miss skips exhausted slot");
    }
    for (bool empty : {false, true})
    {
        auto f = std::make_unique<Fixture>();
        if (empty)
        {
            Owner::entry(f->cache, 0).report.generation = ~u64{0};
            Owner::serial(f->cache) = ~u64{0};
        }
        const auto original = Owner::snapshot(f->cache);
        std::array<Owner::Change, 1> absent{{{17, Owner::Action::replace, f->old1, f->final}}};
        const std::span<const Owner::Change> changes = empty ? std::span<const Owner::Change>{} : absent;
        const usize reads = f->disk.reads;
        check(Owner::prepare(f->cache, changes, f->owner).ok() && Owner::normalize(f->cache, f->owner).ok() &&
              Owner::publish(f->cache, f->owner).ok() && Owner::snapshot(f->cache) == original && f->disk.reads == reads,
              "empty or entirely absent request changes no slot and performs no miss");
    }
    {
        auto f = std::make_unique<Fixture>(); auto changes = f->changes(); Cache::Pin pin{};
        check(f->cache.pin(1, pin).ok(), "retain authentic cache pin");
        refused(*f, [&]{ return Owner::prepare(f->cache, changes, f->owner); }, "authentic retained pin prevents preflight");
        check(pin.release().ok() && Owner::prepare(f->cache, changes, f->owner).ok(), "preflight refusal did not mint or consume plan");
        refused(*f, [&]{ return Owner::publish(f->cache, f->owner); }, "publication requires completed normalization phase");
        check(Owner::failed(f->owner), "early publication refusal makes original plan terminal");
    }
    {
        auto f = std::make_unique<Fixture>(); auto changes = f->changes(); Cache::Pin pin{};
        f->disk.callback = [&] {
            check(f->cache.report().in_flight, "transport callback sees actual in-flight cache reservation");
            refused(*f, [&]{ return Owner::prepare(f->cache, changes, f->owner); }, "authentic in-flight read prevents preflight");
        };
        check(f->cache.pin(7, pin).ok(), "in-flight refusal preserves completing original read");
        f->disk.callback = {}; check(pin.release().ok(), "drain authentic completed read pin");
    }
    {
        auto f = std::make_unique<Fixture>(); std::array<Owner::Change, 32> changes{};
        for (usize n = 0; n < changes.size(); ++n)
        {
            const auto expected = n == 1 ? std::span<const std::byte>{f->old1} :
                                  n == 2 ? std::span<const std::byte>{f->old2} :
                                  n == 3 ? std::span<const std::byte>{f->old3} : std::span<const std::byte>{f->old1};
            changes[n] = {n, Owner::Action::replace, expected, f->final};
        }
        const usize reads = f->disk.reads;
        check(Owner::prepare(f->cache, changes, f->owner).ok() && Owner::normalize(f->cache, f->owner).ok() &&
              Owner::publish(f->cache, f->owner).ok() && f->disk.reads == reads, "exact 32-key target bound accepts without loading absent entries");
        for (usize n = 0; n < 3; ++n)
            check(Owner::entry(f->cache, n).canonical == f->final && Owner::entry(f->cache, n).staged == f->final,
                  "bound-sized publication replaces exactly actual residents");
    }
    {
        auto f = std::make_unique<Fixture>(); auto changes = f->changes();
        check(Owner::prepare(f->cache, changes, f->owner).ok() && Owner::normalize(f->cache, f->owner).ok(), "Session quarantine prepared prefix");
        check(Owner::quarantine(f->cache).ok(), "quarantine original Session");
        refused(*f, [&]{ return Owner::publish(f->cache, f->owner); }, "lost original Session ownership forbids publication");
    }
    std::printf("PASS hosted typed cache reconciliation checks=%zu refusals=%zu drift_checks=%zu raw_helper_io=0 owner_attachment=0 target_execution=0\n",
                checks, refusals, drift_checks);
    return 0;
}
