// Serialized actual mounted-provider model. -fno-access-control is used only
// for independent attachment/record fault injection, never for publication.
#include "ext4_mounted_support.hpp"
#include "ext4_mounted_retention_support.hpp"
#include "ext4_realtime_model.hpp"
#include <array>
#include <memory>
#include <type_traits>

using namespace ok;
using namespace ok::fs;
using namespace owned_fixture;
namespace
{
usize checks{}, cases{};
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "mounted root retention:%d: %s\n", __LINE__, #expression); std::abort(); } } while (false)
using Mount = Ext4MountedMetadata<2>;
static_assert(!std::is_copy_constructible_v<MountedRootReference>);
static_assert(!std::is_copy_assignable_v<MountedRootReference>);
static_assert(std::is_nothrow_move_constructible_v<MountedRootReference>);
static_assert(!std::is_move_assignable_v<MountedRootReference>);
static_assert(MountedRootRetentionDomain::capacity >= 16);

struct Fixture final
{
    Disk disk;
    Mount::Registry registry{};
    Mount::Registry::Registration key{};
    std::unique_ptr<Mount> mount;
    std::unique_ptr<VirtualFileSystem> vfs = std::make_unique<VirtualFileSystem>();
    VfsMountHandle attachment{};
    explicit Fixture(const Bytes &image, Ext4MountedPolicy policy = Ext4MountedPolicy::maintenance_only)
        : disk(image), mount(std::make_unique<Mount>(policy))
    {
        auto registered = registry.register_device(disk, 50001, 65536); CHECK(registered); key = registered.value();
        CHECK(mount->mount(registry, key, {0, image.size() / 512}));
    }
    void publish()
    { auto result = publish_owned_root(*mount, *vfs); CHECK(result); attachment = result.value(); }
    usize records() const
    { usize count{}; for (const auto &record : mount->root_retention_.records_) count += bool(record.serial); return count; }
    void no_io(usize reads, usize writes, usize flushes) const
    { CHECK(disk.reads == reads && disk.writes == writes && disk.flushes == flushes); }
};

void exact_reference(const Bytes &image)
{
    Fixture f(image);
    MountedRootReference empty;
    CHECK(!empty.valid() && empty.release().code() == StatusCode::bad_descriptor);
    CHECK(empty.clone().status().code() == StatusCode::bad_descriptor);
    auto issued = f.mount->retain_root(); CHECK(issued && f.records() == 1);
    CHECK(f.mount->report().published && !f.mount->legacy_exposure_);
    auto moved = std::move(issued).value();
    CHECK(moved.valid() && !issued.value().valid() && f.records() == 1);
    CHECK(issued.value().release().code() == StatusCode::bad_descriptor);
    auto clone = moved.clone(); CHECK(clone && f.records() == 2);
    CHECK(clone.value().serial_ != moved.serial_);
    CHECK(f.mount->begin_drain().code() == StatusCode::busy);
    CHECK(f.mount->report().phase == Ext4MountPhase::readable);
    CHECK(moved.release() && !moved.valid() && f.records() == 1);
    CHECK(moved.release().code() == StatusCode::bad_descriptor && f.records() == 1);
    CHECK(clone.value().release() && !f.records());
    CHECK(f.mount->begin_drain() && f.mount->report().phase == Ext4MountPhase::draining);
    CHECK(f.mount->close() && f.mount->report().phase == Ext4MountPhase::closed);
    CHECK(f.registry.inspect(f.key).value().state == block::DeviceState::ready);
    CHECK(f.mount->report().published); // Historical publication is not a live hold.
    ++cases;
}
void bounded_references(const Bytes &image)
{
    Fixture f(image);
    std::array<std::optional<MountedRootReference>, MountedRootRetentionDomain::capacity> references;
    for (auto &reference : references)
    { auto issued = f.mount->retain_root(); CHECK(issued); reference.emplace(std::move(issued).value()); }
    const auto serial = f.mount->root_retention_.last_serial_;
    CHECK(f.records() == references.size());
    CHECK(f.mount->retain_root().status().code() == StatusCode::no_memory);
    CHECK(references[0]->clone().status().code() == StatusCode::no_memory);
    CHECK(f.records() == references.size() && f.mount->root_retention_.last_serial_ == serial);
    CHECK(references[0]->release()); references[0].reset();
    auto reused = references[1]->clone(); CHECK(reused && reused.value().serial_ > serial);
    CHECK(reused.value().release());
    for (auto &reference : references) if (reference) CHECK(reference->release());
    CHECK(!f.records() && f.mount->close()); ++cases;

    Fixture exhausted(image);
    exhausted.mount->root_retention_.last_serial_ = ~u64{0} - 1;
    auto final = exhausted.mount->retain_root(); CHECK(final && final.value().serial_ == ~u64{0});
    CHECK(final.value().clone().status().code() == StatusCode::overflow);
    CHECK(exhausted.mount->retain_root().status().code() == StatusCode::overflow);
    CHECK(exhausted.records() == 1 && final.value().release());
    CHECK(exhausted.mount->close()); ++cases;
}
void attachment_faults(const Bytes &image)
{
    for (usize variant = 0; variant < 8; ++variant)
    {
        Fixture f(image);
        Disk decoy_disk(image);
        auto registered = f.registry.register_device(decoy_disk, 50002, 65536); CHECK(registered);
        auto decoy = std::make_unique<Mount>(); CHECK(decoy->mount(f.registry, registered.value(), {0, image.size() / 512}));
        auto original = f.mount->retain_root(), other = decoy->retain_root(); CHECK(original && other);
        const auto before = f.registry.inspect(registered.value()).value();
        const auto record = f.mount->root_retention_.records_[0];
        auto &token = original.value();
        switch (variant)
        {
            case 0: token.domain_ = &decoy->root_retention_; break;
            case 1: token.root_ = &decoy->nodes_[0]; break;
            case 2: token.provider_ = decoy.get(); break;
            case 3: ++token.session_; break;
            case 4: ++token.generation_; break;
            case 5: ++token.serial_; break;
            case 6: token.serial_ = 0; break;
            case 7:
            {
                // A byte-equivalent candidate has no record at its own address.
                MountedRootReference impostor;
                impostor.original_anchor_ = token.original_anchor_; impostor.domain_ = token.domain_;
                impostor.root_ = token.root_; impostor.provider_ = token.provider_;
                impostor.session_ = token.session_; impostor.generation_ = token.generation_; impostor.serial_ = token.serial_;
                CHECK(impostor.release().code() == StatusCode::fault && !impostor.valid()); break;
            }
        }
        CHECK(token.release().code() == StatusCode::fault && !token.valid());
        CHECK(f.records() == 1 && f.mount->root_retention_.records_[0].serial == record.serial &&
              f.mount->root_retention_.records_[0].holder == record.holder);
        const auto reads = f.disk.reads, writes = f.disk.writes, flushes = f.disk.flushes;
        CHECK(!f.mount->sync() && !f.mount->close() && !f.mount->begin_drain());
        CHECK(f.mount->report().phase == Ext4MountPhase::failed && f.mount->buffers().state == MetadataOwnerState::failed);
        CHECK(f.registry.inspect(f.key).value().state == block::DeviceState::quarantined);
        f.no_io(reads, writes, flushes);
        CHECK(reclaim_history::same_session(before, f.registry.inspect(registered.value()).value()));
        CHECK(decoy->root_retention_.audit() && other.value().clone());
        CHECK(other.value().release() && decoy->close()); ++cases;
    }
    Fixture moved_fault(image), decoy(image);
    auto issued = moved_fault.mount->retain_root(); CHECK(issued);
    issued.value().domain_ = &decoy.mount->root_retention_;
    MountedRootReference moved(std::move(issued).value());
    CHECK(!issued.value().valid() && moved.release().code() == StatusCode::fault);
    CHECK(moved_fault.records() == 1 && decoy.mount->root_retention_.audit());
    CHECK(!moved_fault.mount->close() && decoy.mount->close()); ++cases;
}
void record_faults(const Bytes &image)
{
    for (usize variant = 0; variant < 6; ++variant)
    {
        Fixture f(image); auto issued = f.mount->retain_root(); CHECK(issued);
        auto &domain = f.mount->root_retention_;
        if (variant == 0) domain.records_[0].holder = nullptr;
        if (variant == 1) domain.records_[1] = domain.records_[0];
        if (variant == 2) ++domain.records_[0].serial;
        if (variant == 3) ++domain.session_;
        if (variant == 4) domain.records_[0] = {};
        if (variant == 5) domain.live_slots_ ^= u64{1};
        CHECK(!f.mount->retain_root() && f.mount->report().phase == Ext4MountPhase::failed);
        CHECK(f.registry.inspect(f.key).value().state == block::DeviceState::quarantined);
        CHECK(!issued.value().release() && !issued.value().valid()); ++cases;
    }
    Fixture f(image); auto issued = f.mount->retain_root(); CHECK(issued);
    CHECK(f.mount->owner_.cache_.session_.abort(Status::io_error("external original Session abort")));
    CHECK(!f.mount->retain_root() && !issued.value().clone());
    CHECK(f.mount->report().phase == Ext4MountPhase::failed && issued.value().release());
    CHECK(!f.records() && !f.mount->close()); ++cases;
}
void one_way_drain(const Bytes &image)
{
    Fixture f(image); f.publish();
    auto path = f.vfs->resolve({}, "/seed"); CHECK(path);
    auto *node = path.value().node(); const auto bytes = node->read(0, 8); CHECK(bytes);
    CHECK(f.mount->begin_drain().code() == StatusCode::busy);
    CHECK(f.vfs->detach(f.attachment));
    CHECK(node->read(0, 8)); // Existing descriptor can finish before drain.
    CHECK(f.mount->begin_drain());
    const auto reads = f.disk.reads, writes = f.disk.writes, flushes = f.disk.flushes;
    CHECK(node->retain().code() == StatusCode::busy);
    CHECK(!f.mount->retain_root() && !f.mount->root());
    CHECK(!node->read(0, 8) && !node->query_metadata() && !node->sync());
    CHECK(!node->write(0, std::span{bytes.value().data}.first(1)));
    CHECK(f.mount->close().code() == StatusCode::busy && f.mount->report().phase == Ext4MountPhase::draining);
    f.no_io(reads, writes, flushes);
    CHECK(path.value().release() && f.mount->lifetime_snapshot(12).value().total() == 0);
    CHECK(f.mount->close()); f.no_io(reads, writes, flushes); ++cases;
}
void retained_path_borrowed_alias(const Bytes &image)
{
    for (auto policy : {Ext4MountedPolicy::maintenance_only, Ext4MountedPolicy::ordered_data_dirty_noatime})
    {
        Fixture f(image, policy); f.publish();
        CHECK(!f.mount->legacy_exposure_);
        auto held = f.vfs->resolve({}, "/"); CHECK(held);
        auto alias = std::make_unique<VirtualFileSystem>();
        CHECK(alias->mount_borrowed("/", *held.value().node()));
        CHECK(held.value().release()); CHECK(f.vfs->detach(f.attachment));
        CHECK(!f.records() && f.mount->lifetime_snapshot(2).value().total() == 0);
        const auto reads = f.disk.reads, writes = f.disk.writes, flushes = f.disk.flushes;
        CHECK(f.mount->begin_drain().code() == StatusCode::busy);
        CHECK(f.mount->close().code() == StatusCode::busy);
        if (policy == Ext4MountedPolicy::ordered_data_dirty_noatime)
            CHECK(f.mount->freeze_clean().code() == StatusCode::busy);
        CHECK(f.mount->legacy_exposure_ && f.mount->report().phase == Ext4MountPhase::readable);
        f.no_io(reads, writes, flushes);
        // Destroying the borrowed namespace cannot revoke permanent exposure.
        alias.reset(); CHECK(f.mount->begin_drain().code() == StatusCode::busy);
        f.no_io(reads, writes, flushes); ++cases;
    }
}
void authentic_cleanup_after_media_failure(const Bytes &image)
{
    Fixture f(image); auto caller = f.mount->retain_root(); CHECK(caller); f.publish();
    auto path = f.vfs->resolve({}, "/seed"); CHECK(path);
    f.disk.fail_flush = f.disk.flushes + 1;
    CHECK(!f.mount->sync() && f.mount->report().phase == Ext4MountPhase::failed);
    const auto reads = f.disk.reads, writes = f.disk.writes, flushes = f.disk.flushes;
    CHECK(!caller.value().clone());
    CHECK(path.value().release() && !path.value().node());
    CHECK(f.vfs->detach(f.attachment) && caller.value().release());
    CHECK(!f.records() && !f.mount->close() && !f.mount->begin_drain());
    CHECK(f.registry.inspect(f.key).value().state == block::DeviceState::quarantined);
    f.no_io(reads, writes, flushes); ++cases;
}
void actual_owner_orphan_gate(const Bytes &image)
{
    constexpr std::array phases{Ext4OwnedPhase::orphan_cleanup_pending,
        Ext4OwnedPhase::orphan_cleanup_capturing, Ext4OwnedPhase::orphan_reclaim_prepared,
        Ext4OwnedPhase::orphan_reclaim_publishing, Ext4OwnedPhase::orphan_reclaim_durable,
        Ext4OwnedPhase::orphan_reclaim_checkpointing};
    for (bool draining : {false, true})
        for (usize variant = 0; variant <= phases.size(); ++variant)
        {
            Fixture f(image, Ext4MountedPolicy::ordered_data_dirty_noatime);
            if (draining) CHECK(f.mount->begin_drain());
            const auto original = f.mount->owner_.report_;
            if (variant < phases.size()) f.mount->owner_.report_.phase = phases[variant];
            else f.mount->owner_.report_.orphan_cleanup_pending = true;
            const auto reads = f.disk.reads, writes = f.disk.writes, flushes = f.disk.flushes;
            if (!draining) CHECK(f.mount->begin_drain().code() == StatusCode::busy);
            CHECK(f.mount->freeze_clean().code() == StatusCode::busy);
            CHECK(f.mount->close().code() == StatusCode::busy);
            CHECK(f.mount->report().phase == (draining ? Ext4MountPhase::draining : Ext4MountPhase::readable));
            f.no_io(reads, writes, flushes);
            f.mount->owner_.report_ = original;
            CHECK(f.mount->close()); f.no_io(reads, writes, flushes); ++cases;
        }
}
void actual_owner_record_gate(const Bytes &image)
{
    for (bool draining : {false, true})
        for (usize variant = 0; variant < 25; ++variant)
        {
            Fixture f(image, Ext4MountedPolicy::ordered_data_dirty_noatime);
            if (draining) CHECK(f.mount->begin_drain());
            auto &owner = f.mount->owner_;
            const auto entry = owner.cache_.entries_[0].report;
            switch (variant)
            {
            case 0: owner.admissions_ = 1; break;
            case 1: owner.startup_active_ = true; break;
            case 2: owner.startup_route_opening_ = true; break;
            case 3: owner.authority_.active = true; break;
            case 4: owner.namespace_active_ = true; break;
            case 5: owner.reclaim_prepared_ = true; break;
            case 6: owner.startup_seal_.active = true; break;
            case 7: owner.startup_namespace_seal_.active = true; break;
            case 8: owner.cache_.transaction_ = &owner.transaction_; break;
            case 9: owner.cache_.entries_[0].report.read_pins = 1; break;
            case 10: owner.cache_.entries_[0].report.transaction_serial = 1; break;
            case 11: owner.cache_.entries_[0].report.staged_dirty = true; break;
            case 12: owner.pins_[0].active_ = true; break;
            case 13: owner.transaction_.active_ = true; break;
            case 14: owner.cache_.entries_[0].report.state = MetadataBufferState::loading; break;
            case 15: owner.rename_active_ = true; break;
            case 16: owner.create_active_ = true; break;
            case 17: owner.first_data_active_ = true; break;
            case 18: owner.next_data_active_ = true; break;
            case 19: owner.namespace_recovery_active_ = true; break;
            case 20: owner.participant_count_ = 1; break;
            case 21: owner.has_data_ = true; break;
            case 22: owner.reclaim_active_ = true; break;
            case 23: owner.reclaim_published_ = true; break;
            case 24: owner.reclaim_completed_ = true; break;
            }
            CHECK(!owner.cache_.report_.read_pins && !owner.cache_.report_.transaction_active &&
                  !owner.cache_.report_.in_flight);
            const auto reads = f.disk.reads, writes = f.disk.writes, flushes = f.disk.flushes;
            if (!draining) CHECK(f.mount->begin_drain().code() == StatusCode::busy);
            CHECK(f.mount->freeze_clean().code() == StatusCode::busy);
            CHECK(f.mount->close().code() == StatusCode::busy);
            CHECK(f.mount->report().phase == (draining ? Ext4MountPhase::draining : Ext4MountPhase::readable));
            f.no_io(reads, writes, flushes);
            owner.admissions_ = 0;
            owner.startup_active_ = owner.startup_route_opening_ = owner.authority_.active = false;
            owner.startup_seal_.active = owner.startup_namespace_seal_.active = false;
            owner.namespace_active_ = owner.namespace_recovery_active_ = false;
            owner.rename_active_ = owner.create_active_ = owner.first_data_active_ = owner.next_data_active_ = false;
            owner.participant_count_ = 0; owner.has_data_ = false;
            owner.reclaim_prepared_ = owner.reclaim_active_ = owner.reclaim_published_ = owner.reclaim_completed_ = false;
            owner.cache_.transaction_ = nullptr; owner.transaction_.active_ = false; owner.pins_[0].active_ = false;
            owner.cache_.entries_[0].report = entry;
            CHECK(f.mount->close()); f.no_io(reads, writes, flushes); ++cases;
        }
    Fixture f(image, Ext4MountedPolicy::ordered_data_dirty_noatime), decoy(image);
    const auto other = decoy.registry.inspect(decoy.key).value();
    CHECK(f.mount->begin_drain());
    const auto reads = f.disk.reads, writes = f.disk.writes, flushes = f.disk.flushes;
    const auto failure = Status::io_error("actual original Owner failed after drain");
    CHECK(f.mount->owner_.fail(failure).code() == StatusCode::io_error);
    CHECK(f.mount->freeze_clean().code() == StatusCode::io_error);
    CHECK(f.mount->report().phase == Ext4MountPhase::failed && f.mount->report().status.message() == failure.message());
    CHECK(f.registry.inspect(f.key).value().state == block::DeviceState::quarantined);
    CHECK(!f.mount->close() && !f.mount->freeze_clean()); f.no_io(reads, writes, flushes);
    CHECK(reclaim_history::same_session(other, decoy.registry.inspect(decoy.key).value()));
    CHECK(decoy.mount->close()); ++cases;
}
void legacy_exposure(const Bytes &image)
{
    {
        Fixture f(image); CHECK(f.mount->root());
        const auto reads = f.disk.reads, writes = f.disk.writes, flushes = f.disk.flushes;
        CHECK(f.mount->close().code() == StatusCode::busy && f.mount->begin_drain().code() == StatusCode::busy);
        CHECK(f.mount->report().phase == Ext4MountPhase::readable && f.mount->legacy_exposure_);
        f.no_io(reads, writes, flushes); ++cases;
    }
    {
        Fixture f(image); f.publish(); auto path = f.vfs->resolve({}, "/seed"); CHECK(path);
        CHECK(path.value().node()->expose_borrowed_identity()); CHECK(path.value().release());
        CHECK(f.vfs->detach(f.attachment));
        CHECK(f.mount->close().code() == StatusCode::busy && f.mount->legacy_exposure_); ++cases;
    }
}
void busy_and_exact_node_gates(const Bytes &image)
{
    {
        Fixture f(image); Mount::Owner::Pin pin;
        CHECK(f.mount->owner_.pin(1, pin));
        const auto reads = f.disk.reads, writes = f.disk.writes, flushes = f.disk.flushes;
        CHECK(f.mount->begin_drain().code() == StatusCode::busy);
        CHECK(f.mount->report().phase == Ext4MountPhase::readable); f.no_io(reads, writes, flushes);
        CHECK(pin.release() && f.mount->close()); ++cases;
    }
    for (usize variant = 0; variant < 10; ++variant)
    {
        Fixture f(image); f.publish(); auto path = f.vfs->resolve({}, "/seed"); CHECK(path);
        Fixture decoy(image); const auto other_before = decoy.registry.inspect(decoy.key).value();
        auto *node = static_cast<Mount::MountedNode *>(path.value().node());
        if (variant == 8) CHECK(node->retain());
        CHECK(f.vfs->detach(f.attachment)); CHECK(f.mount->begin_drain());
        if (variant == 0) ++node->references_;
        if (variant == 1)
            for (auto &held : f.mount->reference_holds_) if (held.node == node) { held.lease.reset(); break; }
        if (variant == 2)
            for (auto &held : f.mount->reference_holds_) if (held.node == node) { held.node = nullptr; held.lease.reset(); --node->references_; break; }
        if (variant >= 3)
            for (auto &held : f.mount->reference_holds_)
                if (held.node == node)
                {
                    auto &lease = *held.lease;
                    auto &actual = f.mount->lifetime_.leases_[lease.slot_];
                    auto &record = f.mount->lifetime_.records_[actual.record];
                    if (variant == 3) ++lease.serial_;
                    if (variant == 4) lease.owner_ = &decoy.mount->lifetime_;
                    if (variant == 5)
                    {
                        actual.kind = Ext4InodeHold::mapping;
                        --record.holds[0]; ++record.holds[1];
                    }
                    if (variant == 6) ++record.key.generation;
                    if (variant == 7) ++actual.record_serial;
                    if (variant == 8)
                        for (auto &duplicate : f.mount->reference_holds_)
                            if (&duplicate != &held && duplicate.node == node)
                            {
                                duplicate.lease->owner_ = lease.owner_; duplicate.lease->slot_ = lease.slot_;
                                duplicate.lease->serial_ = lease.serial_; break;
                            }
                    if (variant == 9) actual.kind = static_cast<Ext4InodeHold>(255);
                    break;
                }
        const auto reads = f.disk.reads, writes = f.disk.writes, flushes = f.disk.flushes;
        CHECK(f.mount->close().code() == StatusCode::fault && f.mount->report().phase == Ext4MountPhase::failed);
        CHECK(f.registry.inspect(f.key).value().state == block::DeviceState::quarantined);
        f.no_io(reads, writes, flushes);
        const auto released = path.value().release();
        CHECK((variant == 0 || variant == 8) ? bool(released) : !released);
        CHECK(!path.value().node());
        CHECK(!f.mount->close() && f.mount->report().phase == Ext4MountPhase::failed);
        CHECK(reclaim_history::same_session(other_before, decoy.registry.inspect(decoy.key).value()));
        CHECK(decoy.mount->close());
        f.no_io(reads, writes, flushes); ++cases;
    }
}
void clean_seal_and_failures(const Bytes &image, const std::filesystem::path &output)
{
    Fixture success(image, Ext4MountedPolicy::ordered_data_dirty_noatime); success.publish();
    auto path = success.vfs->resolve({}, "/seed"); CHECK(path);
    const auto data = path.value().node()->read(0, 8); CHECK(data); CHECK(path.value().release());
    CHECK(detach_and_drain(*success.mount, *success.vfs, success.attachment));
    const auto start = success.disk.trace.size(); CHECK(success.mount->freeze_clean());
    const auto seal_end = success.disk.trace.size();
    CHECK(success.mount->report().phase == Ext4MountPhase::clean_sealed && !(r32(success.disk.durable, 1120) & 4));
    CHECK(success.disk.live == success.disk.durable && success.disk.pending.empty());
    save(output / "sealed.img", success.disk.durable);
    auto reopened = std::make_unique<Mount>(Ext4MountedPolicy::ordered_data_dirty_noatime);
    CHECK(reopened->mount(success.registry, success.key, {0, image.size() / 512}));
    auto second_vfs = std::make_unique<VirtualFileSystem>();
    auto second = publish_owned_root(*reopened, *second_vfs); CHECK(second);
    auto persisted = second_vfs->resolve({}, "/seed"); CHECK(persisted);
    const auto after = persisted.value().node()->read(0, 8); CHECK(after && after.value().data == data.value().data);
    CHECK(persisted.value().release()); CHECK(detach_and_drain(*reopened, *second_vfs, second.value()));
    CHECK(reopened->freeze_clean()); save(output / "reopened-sealed.img", success.disk.durable); ++cases;

    std::array<usize, 3> ordinals{};
    for (usize index = start; index < seal_end; ++index)
    {
        const char operation = success.disk.trace[index].operation;
        const usize kind = operation == 'r' ? 0 : operation == 'w' ? 1 : 2;
        ++ordinals[kind];
    }
    for (usize kind = 0; kind < ordinals.size(); ++kind)
        for (usize ordinal = 1; ordinal <= ordinals[kind]; ++ordinal)
        {
            Fixture f(image, Ext4MountedPolicy::ordered_data_dirty_noatime); f.publish();
            CHECK(detach_and_drain(*f.mount, *f.vfs, f.attachment));
            if (kind == 0) f.disk.fail_read = f.disk.reads + ordinal;
            if (kind == 1) f.disk.fail_write = f.disk.writes + ordinal;
            if (kind == 2) f.disk.fail_flush = f.disk.flushes + ordinal;
            CHECK(!f.mount->freeze_clean() && f.mount->report().phase == Ext4MountPhase::failed);
            CHECK(f.registry.inspect(f.key).value().state == block::DeviceState::quarantined && !f.records());
            const auto reads = f.disk.reads, writes = f.disk.writes, flushes = f.disk.flushes;
            CHECK(!f.mount->freeze_clean() && !f.mount->retain_root()); f.no_io(reads, writes, flushes);
            f.disk.crash(); CHECK(r32(f.disk.durable, 1120) & 4); ++cases;
        }
}
} // namespace
int main(int argc, char **argv)
{
    CHECK(argc == 3); std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    Bytes image((std::istreambuf_iterator<char>(input)), {}); CHECK(image.size() == 32 * 1024 * 1024);
    std::filesystem::create_directory(argv[2]);
    exact_reference(image); bounded_references(image); attachment_faults(image); record_faults(image);
    one_way_drain(image); retained_path_borrowed_alias(image);
    authentic_cleanup_after_media_failure(image); actual_owner_orphan_gate(image);
    actual_owner_record_gate(image);
    legacy_exposure(image); busy_and_exact_node_gates(image);
    clean_seal_and_failures(image, argv[2]);
    std::printf("PASS mounted root retention: checks=%zu cases=%zu capacity=%zu actual_provider=1 original_failure_anchor=1 one_way_drain=1 seal_reopen=1\n",
                checks, cases, MountedRootRetentionDomain::capacity);
}
