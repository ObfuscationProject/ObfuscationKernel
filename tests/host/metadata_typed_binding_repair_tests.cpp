#include <ok/fs/metadata_buffers.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

// Author regression of the preserved independent rejection; no owner attachment.
namespace ok::fs {
template <usize C, usize D> class Ext4OwnedTransaction {
  public:
    using Cache = MetadataBuffers<C, 1024, D>;
    using Plan = typename Cache::TypedReconciliation;
    using Change = typename Cache::TypedCacheChange;
    using Action = typename Cache::TypedCacheAction;
    Ext4OwnedTransaction() = default;
    ~Ext4OwnedTransaction() = default;
    Plan plan{};
    Status prepare(Cache &c, std::span<const Change> x) { return c.prepare_typed_reconciliation(x, plan); }
    Status normalize(Cache &c) { return c.normalize_typed_reconciliation(plan); }
    Status publish(Cache &c) { return c.publish_typed_reconciliation(plan); }
    static auto &session(Cache &c) { return c.session_; }
    static auto &binding(Cache &c) { return c.session_binding_; }
    static auto &sectors_per_block(Cache &c) { return c.sectors_per_block_; }
    auto &plan_binding() { return plan.session_binding_; }
    static auto &entries(Cache &c) { return c.entries_; }
    static auto &report(Cache &c) { return c.report_; }
    static bool same(const MetadataOwnerReport &a, const MetadataOwnerReport &b) { return Cache::same_report(a, b); }
    static bool same(const MetadataBufferReport &a, const MetadataBufferReport &b) { return Cache::same_report(a, b); }
};
}
using namespace ok;
using Owner = fs::Ext4OwnedTransaction<40, 2>;
using Cache = Owner::Cache;
using Registry = Cache::Registry;
using Image = std::array<std::byte, 1024>;

// Explicit instantiation can name a private member. This observes authoritative
// Session/Registry storage directly, without substituting a diagnostic report.
template <class Tag, auto Member> struct PrivateMember {
    friend auto member(Tag) { return Member; }
};
struct FirstTag { friend auto member(FirstTag); };
struct GeometryTag { friend auto member(GeometryTag); };
struct EntriesTag { friend auto member(EntriesTag); };
template struct PrivateMember<FirstTag, &Registry::Session::first_>;
template struct PrivateMember<GeometryTag, &Registry::Session::geometry_>;
template struct PrivateMember<EntriesTag, &Registry::entries_>;

usize checks{}, controls{}, missed_refusals{};
void check(bool value, const char *text) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL independent cache control: %s\n", text); std::exit(1); }
}
struct Disk final : driver::BlockDevice {
    std::vector<std::byte> storage = std::vector<std::byte>(256 * 512);
    usize reads{}, writes{}, flushes{}, geometries{};
    Disk() { for (usize n = 0; n < storage.size(); ++n) storage[n] = std::byte{static_cast<u8>(n * 37 + n / 911 + 9)}; }
    driver::BlockGeometry geometry() const override { return {256, 512, true}; }
    Status read_blocks(u64 first, std::span<std::byte> out) override {
        ++reads;
        if (first >= 256 || out.size() > storage.size() - first * 512) return Status::io_error("independent bounds");
        std::copy_n(storage.begin() + first * 512, out.size(), out.begin()); return {};
    }
    Status write_blocks(u64 first, std::span<const std::byte> in) override {
        ++writes;
        if (first >= 256 || in.size() > storage.size() - first * 512) return Status::io_error("independent write bounds");
        std::copy(in.begin(), in.end(), storage.begin() + first * 512); return {};
    }
    Status flush() override { ++flushes; return {}; }
};
struct Fixture {
    Disk disk{};
    Registry registry{};
    Cache cache{};
    Owner owner{};
    std::array<Image, 40> originals{};
    Image final{};
    Fixture(usize count = 32) {
        auto key = registry.register_device(disk, 202); check(key.ok(), "original registration");
        check(cache.open(registry, key.value(), {16, 192}, block::SessionAccess::writable).ok(), "nonzero partition original Session");
        // Simultaneous pins force unique actual resident slots.
        std::array<Cache::Pin, 40> pins{};
        for (usize n = 0; n < count; ++n) {
            check(cache.pin(n, pins[n]).ok(), "distinct actual residents");
            check(pins[n].read(0, originals[n]).ok(), "expected image from retained original pin");
        }
        for (usize n = 0; n < count; ++n) check(pins[n].release().ok(), "drain actual pin");
        final.fill(std::byte{0xd6});
    }
    ~Fixture() {
        // Drift probes restore authoritative storage before teardown.
        check(cache.close().ok(), "close restored original Session");
    }
    auto &session() { return Owner::session(cache); }
    auto &registration() { return (registry.*member(EntriesTag{}))[0]; }
    Owner::Change change() { return {0, Owner::Action::replace, originals[0], final}; }
};
void stable_boundaries() {
    for (usize residents : {usize{0}, usize{1}, usize{32}, usize{40}}) {
        auto f = std::make_unique<Fixture>(residents);
        const auto before_report = Owner::report(f->cache);
        const auto before_entries = Owner::entries(f->cache);
        std::array<Owner::Change, 32> changes{};
        for (usize n = 0; n < changes.size(); ++n)
            changes[n] = {n, (n % 2 ? Owner::Action::retire : Owner::Action::replace),
                          f->originals[n], n % 2 ? std::span<const std::byte>{} : std::span<const std::byte>{f->final}};
        const usize io = f->disk.reads + f->disk.writes + f->disk.flushes;
        check(f->owner.prepare(f->cache, changes).ok(), "32-key resident boundary prepare");
        check(f->owner.normalize(f->cache).ok(), "32-key resident boundary normalize");
        check(f->owner.publish(f->cache).ok(), "32-key resident boundary publish");
        check(f->disk.reads + f->disk.writes + f->disk.flushes == io, "zero helper transport");
        const auto &after = Owner::entries(f->cache);
        usize retired = 0;
        for (usize n = 0; n < 40; ++n) {
            if (n < residents && n < 32) {
                if (n % 2) {
                    ++retired;
                    check(after[n].report.state == fs::MetadataBufferState::unused && after[n].canonical == Image{} && after[n].staged == Image{}, "complete resident retirement");
                } else check(after[n].canonical == f->final && after[n].staged == f->final, "complete resident replacement");
                check(after[n].report.generation == before_entries[n].report.generation + 1, "exact generation advance");
            } else check(Owner::same(after[n].report, before_entries[n].report) && after[n].canonical == before_entries[n].canonical && after[n].staged == before_entries[n].staged, "complete unrelated resident preservation");
        }
        check(Owner::report(f->cache).resident_blocks == before_report.resident_blocks - retired, "exact resident boundary subtraction");
        ++controls;
    }
    for (u64 generation : {u64{1}, ~u64{0} - 1, ~u64{0}})
        for (auto action : {Owner::Action::replace, Owner::Action::retire}) {
            auto f = std::make_unique<Fixture>(1);
            Owner::entries(f->cache)[0].report.generation = generation;
            auto change = f->change(); change.action = action;
            if (action == Owner::Action::retire) change.replacement = {};
            const auto before = Owner::entries(f->cache);
            const auto status = f->owner.prepare(f->cache, {&change, 1});
            if (generation == ~u64{0} && action == Owner::Action::replace) {
                check(!status, "maximum generation replacement refuses");
                check(Owner::same(before[0].report, Owner::entries(f->cache)[0].report) && before[0].canonical == Owner::entries(f->cache)[0].canonical && before[0].staged == Owner::entries(f->cache)[0].staged, "generation refusal changes no affected bytes");
            } else {
                check(status.ok() && f->owner.normalize(f->cache).ok() && f->owner.publish(f->cache).ok(), "generation boundary successful operation");
                const auto &actual = Owner::entries(f->cache)[0].report;
                check(actual.generation == (generation == ~u64{0} ? generation : generation + 1), "generation no wrap");
                check(actual.state == (action == Owner::Action::replace ? fs::MetadataBufferState::ready : generation == ~u64{0} ? fs::MetadataBufferState::exhausted : fs::MetadataBufferState::unused), "generation terminal state");
            }
            ++controls;
        }
}

// Reproduce every preserved authority drift as an unchanged-cache refusal.
void authority_drifts() {
    for (bool publish_phase : {false, true})
        for (usize field = 0; field < 8; ++field) {
            auto f = std::make_unique<Fixture>(1);
            auto request = f->change();
            check(f->owner.prepare(f->cache, {&request, 1}).ok(), "authority probe stable original prepare");
            if (publish_phase) check(f->owner.normalize(f->cache).ok(), "authority probe stable original normalization");
            const auto cache_report = Owner::report(f->cache);
            const auto cache_entries = Owner::entries(f->cache);
            const auto old_first = f->session().*member(FirstTag{});
            const auto old_geometry = f->session().*member(GeometryTag{});
            auto &record = f->registration();
            const auto old_owner = record.owner;
            const auto old_device = record.device;
            const auto old_registration_geometry = record.geometry;
            const auto old_maximum_transfer = record.maximum_transfer;
            const auto diagnostic_before = f->session().report();
            check(diagnostic_before.ok(), "read genuine original diagnostic report");
            Registry::Session foreign{}; Disk foreign_disk{};
            switch (field) {
                case 0: ++(f->session().*member(FirstTag{})); break;
                case 1: ++(f->session().*member(GeometryTag{})).block_count; break;
                case 2: (f->session().*member(GeometryTag{})).block_size = 1024; break;
                case 3: (f->session().*member(GeometryTag{})).writable = false; break;
                case 4: record.owner = &foreign; break;
                case 5: record.device = &foreign_disk; break;
                case 6: ++record.geometry.block_count; break;
                case 7: record.maximum_transfer = 512; break;
            }
            const auto diagnostic_after = f->session().report();
            check(diagnostic_after.ok() && diagnostic_after.value().identity == diagnostic_before.value().identity && diagnostic_after.value().registration_generation == diagnostic_before.value().registration_generation && diagnostic_after.value().session_serial == diagnostic_before.value().session_serial && diagnostic_after.value().state == diagnostic_before.value().state, "authoritative drift preserves original public identity report");
            const usize io = f->disk.reads + f->disk.writes + f->disk.flushes;
            const auto result = publish_phase ? f->owner.publish(f->cache) : f->owner.normalize(f->cache);
            check(f->disk.reads + f->disk.writes + f->disk.flushes == io, "authority probe zero helper transport");
            if (result) {
                ++missed_refusals;
                std::printf("FINDING authority_drift_accepted phase=%s field=%zu canonical_mutated=%d stage_mutated=%d original_report_unchanged=1\n", publish_phase ? "publish" : "normalize", field,
                            Owner::entries(f->cache)[0].canonical != cache_entries[0].canonical,
                            Owner::entries(f->cache)[0].staged != cache_entries[0].staged);
            } else {
                check(Owner::same(Owner::report(f->cache), cache_report), "authority refusal preserves complete cache report");
                for (usize n = 0; n < cache_entries.size(); ++n)
                    check(Owner::same(Owner::entries(f->cache)[n].report, cache_entries[n].report) && Owner::entries(f->cache)[n].canonical == cache_entries[n].canonical && Owner::entries(f->cache)[n].staged == cache_entries[n].staged, "authority refusal preserves every resident byte");
            }
            f->session().*member(FirstTag{}) = old_first;
            f->session().*member(GeometryTag{}) = old_geometry;
            record.owner = old_owner; record.device = old_device; record.geometry = old_registration_geometry; record.maximum_transfer = old_maximum_transfer;
            ++controls;
        }
}

void binding_controls() {
    for (usize field = 0; field < 3; ++field) {
        auto f = std::make_unique<Fixture>(1);
        auto change = f->change();
        auto &report = Owner::report(f->cache);
        switch (field) {
            case 0: ++report.first_sector; break;
            case 1: report.sectors += 2; ++report.blocks; break;
            case 2: Owner::sectors_per_block(f->cache) = 4; report.blocks = report.sectors / 4; break;
        }
        const auto entries = Owner::entries(f->cache);
        check(!f->owner.prepare(f->cache, {&change, 1}), "internally consistent cache prefix drift refuses before mint");
        for (usize n = 0; n < entries.size(); ++n)
            check(Owner::same(entries[n].report, Owner::entries(f->cache)[n].report) &&
                  entries[n].canonical == Owner::entries(f->cache)[n].canonical &&
                  entries[n].staged == Owner::entries(f->cache)[n].staged,
                  "prefix refusal preserves complete cache entries");
        ++controls;
    }
    {
        auto f = std::make_unique<Fixture>(1);
        auto change = f->change();
        check(f->owner.prepare(f->cache, {&change, 1}).ok(), "prepare before continuing child I/O");
        auto &session = f->session();
        const auto before = session.observe_binding();
        check(before.ok(), "capture continuing immutable binding");
        Image read{};
        check(session.read_blocks(0, read).ok(), "legitimate original read advances counters");
        check(session.write_blocks(0, read).ok(), "legitimate original write advances counters");
        check(!f->owner.normalize(f->cache), "pending original writes cannot publish cache");
        check(session.flush().ok(), "original flush drains pending writes");
        check(session.observe_binding().ok() && session.observe_binding().value() == before.value(),
              "counters do not change immutable binding");
        check(!f->owner.normalize(f->cache), "refused plan cannot revive after pending work drains");
        ++controls;
    }
    {
        auto f = std::make_unique<Fixture>(1);
        auto change = f->change();
        check(f->owner.prepare(f->cache, {&change, 1}).ok(), "prepare before completed child I/O");
        Image read{};
        check(f->session().read_blocks(0, read).ok() && f->session().write_blocks(0, read).ok() &&
              f->session().flush().ok(), "continuing original read write flush complete");
        check(f->owner.normalize(f->cache).ok() && f->owner.publish(f->cache).ok(),
              "completed child counters permit cache publication");
        ++controls;
    }
    for (bool plan_drift : {false, true}) {
        auto f = std::make_unique<Fixture>(1);
        auto change = f->change();
        check(f->owner.prepare(f->cache, {&change, 1}).ok(), "prepare private retained binding control");
        const auto before = Owner::entries(f->cache);
        auto &binding = plan_drift ? f->owner.plan_binding() : Owner::binding(f->cache);
        ++binding.device_sector_bytes;
        check(!f->owner.normalize(f->cache), "private retained binding drift refuses");
        for (usize n = 0; n < before.size(); ++n)
            check(Owner::same(before[n].report, Owner::entries(f->cache)[n].report) &&
                  before[n].canonical == Owner::entries(f->cache)[n].canonical &&
                  before[n].staged == Owner::entries(f->cache)[n].staged,
                  "private binding drift leaves complete cache unchanged");
        ++controls;
    }
}

int main() {
    stable_boundaries();
    authority_drifts();
    check(missed_refusals == 0, "all sixteen preserved authority findings now refuse");
    binding_controls();
    std::printf("PASS author binding repair controls checks=%zu controls=%zu authority_missed_refusals=%zu no_owner_attachment=1 no_target_execution=1\n", checks, controls, missed_refusals);
    return 0;
}
