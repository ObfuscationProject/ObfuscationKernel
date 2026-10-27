// Actual mounted-provider hosted model. Access control is disabled only for
// serialized private-record fault injection; publication uses the actual Node.
#include "ext4_mounted_support.hpp"
#include "ext4_realtime_model.hpp"
#include <ok/fs/ext4/mount.hpp>
#include <array>
#include <memory>

using namespace ok;
using namespace ok::fs;
using namespace owned_fixture;
namespace
{
usize checks{}, identity_controls{}, generation_controls{}, capacity_controls{},
      runtime_callbacks{}, binding_controls{}, admission_controls{}, release_controls{}, fault_controls{}, positives{}, refusals{};
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "mounted mutation identity:%d: %s\n", __LINE__, #expression); std::abort(); } } while (false)
using Mount = Ext4MountedMetadata<2>;
using MountedNode = Mount::MountedNode;
constexpr std::array<std::byte, 4> replacement{std::byte{'N'}, std::byte{'O'}, std::byte{'T'}, std::byte{'E'}};
constexpr usize io = static_cast<usize>(Ext4InodeHold::io);
constexpr usize descriptor = static_cast<usize>(Ext4InodeHold::descriptor);
static_assert(!std::is_copy_constructible_v<Mount::Lease>);
static_assert(!std::is_copy_constructible_v<Mount::NodeMutationContext>);
static_assert(!std::is_move_constructible_v<Mount::NodeMutationContext>);

struct Fixture final
{
    Disk disk;
    Mount::Registry registry;
    Mount::Registry::Registration registration{};
    std::unique_ptr<Mount> mount;
    MountedNode *node{};
    Ext4InodeLifetimeKey key{};
    explicit Fixture(const Bytes &image, Ext4MountedPolicy policy = Ext4MountedPolicy::ordered_data_dirty_noatime)
        : disk(image), mount(std::make_unique<Mount>(policy))
    {
        auto registered = registry.register_device(disk, 42001, 65536); CHECK(registered);
        registration = registered.value();
        CHECK(mount->mount(registry, registration, {0, image.size() / 512}));
        auto found = mount->nodes_[0].lookup_child_retained("seed"); CHECK(found);
        for (auto &slot : mount->nodes_) if (&slot == found.value()) node = &slot;
        CHECK(node); key = node->lifetime_key_;
        const auto state = mount->lifetime_.snapshot(key); CHECK(state);
        CHECK(state.value().holds[descriptor] == 1 && !state.value().holds[io]);
    }
    Status operate(usize kind)
    { return kind == 0 ? node->write(100, replacement) : kind == 1 ? node->truncate(4000) : node->write(100, {}); }
    void no_hold() const
    {
        const auto state = mount->lifetime_.snapshot(key); CHECK(state && !state.value().holds[io]);
        CHECK(!mount->mutation_node_ && !mount->mutation_lease_ && mount->mutation_key_ == Ext4InodeLifetimeKey{});
        CHECK(!mount->mutation_context_);
        auto active = mount->lifetime_.node_mutation_active(mount.get(), key.session);
        CHECK(active && !active.value());
        CHECK(!mount->report().reading);
    }
    void release()
    { if (node->references_) CHECK(node->release()); }
};

void address_and_identity(const Bytes &image)
{
    for (usize kind = 0; kind < 3; ++kind)
    {
        {
            Fixture f(image); MountedNode copied = *f.node;
            const auto reads = f.disk.reads, writes = f.disk.writes, flushes = f.disk.flushes;
            auto status = kind == 0 ? copied.write(100, replacement) :
                          kind == 1 ? copied.truncate(4000) : copied.write(100, {});
            CHECK(status.code() == StatusCode::bad_descriptor);
            CHECK(f.disk.reads == reads && f.disk.writes == writes && f.disk.flushes == flushes);
            f.no_hold(); f.release(); ++identity_controls; ++refusals;
        }
        {
            Fixture f(image), other(image);
            const auto reads = f.disk.reads, writes = f.disk.writes, flushes = f.disk.flushes;
            const auto foreign_reads = other.disk.reads;
            auto status = kind == 1 ? f.mount->truncate_regular(*other.node, 4000) :
                f.mount->write_regular(*other.node, 100, kind == 0 ? std::span<const std::byte>{replacement} : std::span<const std::byte>{});
            CHECK(status.code() == StatusCode::bad_descriptor);
            CHECK(f.disk.reads == reads && f.disk.writes == writes && f.disk.flushes == flushes && other.disk.reads == foreign_reads);
            f.no_hold(); other.no_hold(); f.release(); other.release(); ++identity_controls; ++refusals;
        }
        for (usize variant = 0; variant < 9; ++variant)
        {
            Fixture f(image), other(image);
            const MountedNode original = *f.node;
            auto &duplicate = f.mount->nodes_[2];
            CHECK(!duplicate.inode_);
            switch (variant)
            {
                case 0: f.node->mount_ = other.mount.get(); break;
                case 1: f.node->inode_ = 0; break;
                case 2: f.node->inode_ = 11; break;
                case 3: f.node->lifetime_key_.mount = other.mount.get(); break;
                case 4: ++f.node->lifetime_key_.session; break;
                case 5: ++f.node->lifetime_key_.inode; break;
                case 6: ++f.node->lifetime_key_.generation; break;
                case 7: duplicate = *f.node; break;
                case 8: f.node->inode_ = 11; f.node->lifetime_key_.inode = 11; break;
            }
            const auto reads = f.disk.reads, writes = f.disk.writes, flushes = f.disk.flushes, other_reads = other.disk.reads;
            CHECK(!f.operate(kind));
            CHECK(f.disk.reads == reads && f.disk.writes == writes && f.disk.flushes == flushes && other.disk.reads == other_reads);
            *f.node = original; if (variant == 7) duplicate = {};
            CHECK(f.mount->report().phase == Ext4MountPhase::readable);
            f.no_hold(); f.release(); other.release(); ++identity_controls; ++refusals;
        }
    }
}

void physical_generations(const Bytes &image)
{
    for (usize kind = 0; kind < 4; ++kind)
        for (usize variant = 0; variant < 2; ++variant)
        {
            Fixture f(image);
            // The canonical inode table is deliberately resident from lookup.
            const auto at = inode_offset(f.disk.live, 12);
            if (variant == 0) { w32(f.disk.live, at + 100, f.key.generation + 1); inode_crc(f.disk.live, 12, at); }
            else f.disk.live[at + 100] ^= 1;
            const auto writes = f.disk.writes, flushes = f.disk.flushes;
            const auto status = kind == 3 ? f.node->truncate(4096) : f.operate(kind);
            CHECK(!status && f.mount->report().phase == Ext4MountPhase::failed);
            CHECK(f.disk.writes == writes && f.disk.flushes == flushes);
            CHECK(f.registry.inspect(f.registration).value().state == block::DeviceState::quarantined);
            f.no_hold(); f.release(); ++generation_controls; ++refusals;
        }
    // Drift after the first physical observation must not be adopted by prepare.
    for (usize kind = 0; kind < 2; ++kind)
    {
        Fixture f(image); bool changed{};
        const auto baseline = f.disk.reads;
        f.disk.hook = [&](char call) {
            if (call == 'r' && !changed && f.disk.reads > baseline)
            { changed = true; const auto at = inode_offset(f.disk.live, 12);
              w32(f.disk.live, at + 100, f.key.generation + 1); inode_crc(f.disk.live, 12, at); }
        };
        const auto writes = f.disk.writes, flushes = f.disk.flushes;
        CHECK(!f.operate(kind) && changed);
        CHECK(f.disk.writes == writes && f.disk.flushes == flushes);
        CHECK(f.mount->report().phase == Ext4MountPhase::failed);
        f.disk.hook = {}; f.no_hold(); f.release(); ++generation_controls; ++refusals;
    }
}

void real_capacity(const Bytes &image)
{
    for (usize kind = 0; kind < 3; ++kind)
    {
        Fixture f(image);
        std::array<std::optional<Mount::Lease>, 511> extra;
        for (auto &lease : extra)
        { auto retained = f.mount->lifetime_.retain(f.key, Ext4InodeHold::io); CHECK(retained); lease.emplace(std::move(retained).value()); }
        CHECK(f.mount->lifetime_.snapshot(f.key).value().holds[io] == 511);
        const auto reads = f.disk.reads, writes = f.disk.writes, flushes = f.disk.flushes;
        CHECK(f.operate(kind).code() == StatusCode::no_memory);
        CHECK(f.disk.reads == reads && f.disk.writes == writes && f.disk.flushes == flushes);
        CHECK(f.mount->report().phase == Ext4MountPhase::readable);
        for (auto &lease : extra) CHECK(f.mount->lifetime_.release(*lease));
        f.no_hold(); f.release(); ++capacity_controls; ++refusals;
    }
    for (usize kind = 0; kind < 3; ++kind)
    {
        Fixture f(image); f.mount->lifetime_.lease_serial_ = ~u64{0};
        const auto reads = f.disk.reads, writes = f.disk.writes, flushes = f.disk.flushes;
        CHECK(f.operate(kind).code() == StatusCode::overflow);
        CHECK(f.disk.reads == reads && f.disk.writes == writes && f.disk.flushes == flushes);
        CHECK(f.mount->report().phase == Ext4MountPhase::readable);
        f.no_hold(); f.release(); ++capacity_controls; ++refusals;
    }
}

void original_binding_drift(const Bytes &image)
{
    for (usize kind = 0; kind < 2; ++kind)
        for (usize stage = 0; stage < 6; ++stage)
            for (usize variant = 0; variant < 10; ++variant)
            {
                Fixture f(image), other(image); bool injected{}, premature{};
                std::optional<Mount::Lease> foreign;
                std::optional<Mount::NodeMutationContext> decoy;
                auto unrelated = other.mount->lifetime_.retain(other.key, Ext4InodeHold::io); CHECK(unrelated);
                foreign.emplace(std::move(unrelated).value());
                const auto writes = f.disk.writes, flushes = f.disk.flushes;
                f.disk.hook = [&](char) {
                    const auto state = f.mount->lifetime_.snapshot(f.key); CHECK(state && state.value().holds[io] == 1);
                    const auto report = f.mount->report();
                    if (report.phase == Ext4MountPhase::readable) premature = true;
                    const auto owned = f.mount->owner_.report().phase;
                    const bool selected = stage == 0 ? owned == Ext4OwnedPhase::open && report.epoch == 1 :
                        stage == 1 ? owned == Ext4OwnedPhase::preparing :
                        stage == 2 ? owned == Ext4OwnedPhase::publishing :
                        stage == 3 ? owned == Ext4OwnedPhase::checkpointing :
                        stage == 4 ? owned == Ext4OwnedPhase::checkpointed :
                                     owned == Ext4OwnedPhase::open && report.epoch == 2;
                    if (injected || !selected) return;
                    injected = true;
                    switch (variant)
                    {
                        case 0: f.mount->mutation_node_ = nullptr; break;
                        case 1: f.mount->mutation_key_ = {}; break;
                        case 2: f.mount->mutation_lease_ = nullptr; break;
                        case 3: f.mount->mutation_context_ = nullptr; break;
                        case 4: f.mount->mutation_node_ = nullptr; f.mount->mutation_key_ = {}; f.mount->mutation_lease_ = nullptr; break;
                        case 5: f.mount->mutation_node_ = nullptr; f.mount->mutation_key_ = {}; f.mount->mutation_lease_ = nullptr; f.mount->mutation_context_ = nullptr; break;
                        case 6: f.mount->mutation_node_ = other.node; break;
                        case 7: f.mount->mutation_key_ = other.key; break;
                        case 8: f.mount->mutation_lease_ = &*foreign; break;
                        case 9:
                            decoy.emplace(*f.node, f.key, *f.mount->mutation_lease_, f.mount->mutation_context_->epoch);
                            f.mount->mutation_context_ = &*decoy; break;
                    }
                };
                CHECK(!f.operate(kind) && injected && !premature); f.disk.hook = {};
                CHECK(f.mount->report().phase == Ext4MountPhase::failed);
                CHECK(f.registry.inspect(f.registration).value().state == block::DeviceState::quarantined);
                if (stage < 2) CHECK(f.disk.writes == writes && f.disk.flushes == flushes);
                f.no_hold(); f.release(); CHECK(other.mount->lifetime_.release(*foreign)); other.no_hold(); other.release();
                ++binding_controls; ++refusals;
            }
}

void public_admission_drift(const Bytes &image)
{
    for (usize kind = 0; kind < 2; ++kind)
        for (usize stage = 0; stage < 6; ++stage)
            for (usize variant = 0; variant < 7; ++variant)
            {
                Fixture f(image); bool injected{}, nested{};
                f.disk.hook = [&](char) {
                    if (nested) return;
                    const auto state = f.mount->lifetime_.snapshot(f.key); CHECK(state && state.value().holds[io] == 1);
                    const auto report = f.mount->report();
                    const auto owned = f.mount->owner_.report().phase;
                    const bool selected = stage == 0 ? owned == Ext4OwnedPhase::open && report.epoch == 1 :
                        stage == 1 ? owned == Ext4OwnedPhase::preparing :
                        stage == 2 ? owned == Ext4OwnedPhase::publishing :
                        stage == 3 ? owned == Ext4OwnedPhase::checkpointing :
                        stage == 4 ? owned == Ext4OwnedPhase::checkpointed :
                                     owned == Ext4OwnedPhase::open && report.epoch == 2;
                    if (injected || !selected) return;
                    injected = nested = true;
                    CHECK(report.phase == Ext4MountPhase::updating && f.mount->mutation_context_);
                    const auto active = f.mount->lifetime_.node_mutation_active(f.mount.get(), f.key.session);
                    CHECK(active && active.value());
                    if (variant) f.mount->report_.phase = Ext4MountPhase::readable;
                    if (variant == 3) f.mount->mutation_context_ = nullptr;
                    if (variant == 4 || variant == 5)
                    {
                        f.mount->mutation_node_ = nullptr; f.mount->mutation_key_ = {};
                        f.mount->mutation_lease_ = nullptr; f.mount->mutation_context_ = nullptr;
                    }
                    // A summary count cannot erase the actual occupied role.
                    auto &actual_record = f.mount->lifetime_.records_[f.mount->lifetime_.find(f.key)];
                    if (variant == 5 || variant == 6) actual_record.holds[io] = 0;
                    const auto reads = f.disk.reads, writes = f.disk.writes, flushes = f.disk.flushes;
                    CHECK(f.node->query_metadata().status().code() == StatusCode::busy);
                    CHECK(f.node->read(0, 1).status().code() == StatusCode::busy);
                    CHECK(f.node->retain().code() == StatusCode::busy);
                    CHECK(f.node->write(100, replacement).code() == StatusCode::busy);
                    CHECK(f.node->write(100, {}).code() == StatusCode::busy);
                    CHECK(f.node->truncate(4000).code() == StatusCode::busy);
                    CHECK(f.mount->begin_update(Ext4MountedPolicy::ordered_data_dirty_noatime).status().code() == StatusCode::busy);
                    CHECK(f.mount->begin_drain().code() == StatusCode::busy);
                    CHECK(f.mount->sync().code() == StatusCode::busy);
                    CHECK(f.mount->retain_root().status().code() == StatusCode::busy);
                    CHECK(!f.mount->root());
                    CHECK(f.mount->drain_pending_orphan());
                    CHECK(f.disk.reads == reads && f.disk.writes == writes && f.disk.flushes == flushes);
                    if (variant == 5 || variant == 6) actual_record.holds[io] = 1;
                    CHECK(f.node->release());
                    CHECK(f.mount->lifetime_.snapshot(f.key).value().holds[descriptor] == 0);
                    CHECK(f.mount->lifetime_.snapshot(f.key).value().holds[io] == 1);
                    CHECK(f.disk.reads == reads && f.disk.writes == writes && f.disk.flushes == flushes);
                    if (variant != 2) f.mount->report_.phase = Ext4MountPhase::updating;
                    nested = false;
                };
                const auto status = f.operate(kind); f.disk.hook = {};
                CHECK(injected);
                if (variant == 0 || variant == 1 || variant == 6)
                { CHECK(status && f.mount->report().phase == Ext4MountPhase::readable); ++positives; }
                else
                {
                    CHECK(!status && f.mount->report().phase == Ext4MountPhase::failed);
                    CHECK(f.registry.inspect(f.registration).value().state == block::DeviceState::quarantined);
                    ++refusals;
                }
                f.no_hold(); CHECK(!f.node->references_); ++admission_controls;
            }
}

void original_role_release_boundary(const Bytes &image)
{
    for (usize kind = 0; kind < 2; ++kind)
        for (usize stage = 0; stage < 6; ++stage)
        {
            Fixture f(image); bool injected{}, nested{};
            f.disk.hook = [&](char) {
                if (nested) return;
                const auto report = f.mount->report();
                const auto owned = f.mount->owner_.report().phase;
                const bool selected = stage == 0 ? owned == Ext4OwnedPhase::open && report.epoch == 1 :
                    stage == 1 ? owned == Ext4OwnedPhase::preparing :
                    stage == 2 ? owned == Ext4OwnedPhase::publishing :
                    stage == 3 ? owned == Ext4OwnedPhase::checkpointing :
                    stage == 4 ? owned == Ext4OwnedPhase::checkpointed :
                                 owned == Ext4OwnedPhase::open && report.epoch == 2;
                if (injected || !selected) return;
                injected = nested = true;
                auto &lease = *f.mount->mutation_lease_;
                const auto *original = f.mount->mutation_context_;
                CHECK(original && lease.valid());
                const auto reads = f.disk.reads, writes = f.disk.writes, flushes = f.disk.flushes;
                CHECK(f.mount->lifetime_.release(lease).code() == StatusCode::busy);
                Mount::Lease transferred(std::move(lease));
                CHECK(!transferred.valid() && lease.valid());
                CHECK(f.mount->lifetime_.release(transferred).code() == StatusCode::bad_descriptor);
                Mount::NodeMutationContext decoy(*f.node, f.key, lease, original->epoch);
                CHECK(!f.mount->lifetime_.release_node_mutation(lease, f.key, &decoy, original->epoch));
                CHECK(!f.mount->lifetime_.release_node_mutation(lease, f.key, original, original->epoch + 1));
                auto foreign_key = f.key; ++foreign_key.generation;
                CHECK(!f.mount->lifetime_.release_node_mutation(lease, foreign_key, original, original->epoch));
                CHECK(f.mount->lifetime_.validate_node_mutation(lease, f.key, original, original->epoch));
                CHECK(f.mount->lifetime_.snapshot(f.key).value().holds[io] == 1);
                f.mount->report_.phase = Ext4MountPhase::readable;
                CHECK(f.node->retain().code() == StatusCode::busy);
                CHECK(f.node->query_metadata().status().code() == StatusCode::busy);
                CHECK(f.node->read(0, 1).status().code() == StatusCode::busy);
                CHECK(f.mount->begin_drain().code() == StatusCode::busy);
                CHECK(f.disk.reads == reads && f.disk.writes == writes && f.disk.flushes == flushes);
                f.mount->report_.phase = Ext4MountPhase::updating; nested = false;
            };
            CHECK(f.operate(kind)); f.disk.hook = {}; CHECK(injected);
            f.no_hold(); f.release(); ++release_controls; ++positives;
        }
    // Public transfer and release remain available for ordinary unclaimed I/O.
    Fixture f(image);
    auto held = f.mount->lifetime_.retain(f.key, Ext4InodeHold::io); CHECK(held);
    auto ordinary = std::move(held).value(); CHECK(ordinary.valid());
    Mount::Lease transferred(std::move(ordinary)); CHECK(transferred.valid() && !ordinary.valid());
    CHECK(f.mount->lifetime_.release(transferred)); f.no_hold(); f.release(); ++release_controls; ++positives;
}

std::vector<Trace> success_and_runtime(const Bytes &image, usize kind)
{
    Fixture f(image); const auto start = f.disk.trace.size(); bool released{};
    f.disk.hook = [&](char) {
        ++runtime_callbacks;
        const auto state = f.mount->lifetime_.snapshot(f.key); CHECK(state && state.value().holds[io] == 1);
        CHECK(f.mount->report().phase == Ext4MountPhase::updating);
        CHECK(f.mount->mutation_node_ == f.node && f.mount->mutation_key_ == f.key && f.mount->mutation_lease_);
        CHECK(f.mount->mutation_context_ && &f.mount->mutation_context_->node == f.node &&
              f.mount->mutation_context_->key == f.key && &f.mount->mutation_context_->lease == f.mount->mutation_lease_);
        CHECK(f.mount->lifetime_.validate_lease(*f.mount->mutation_lease_, f.key, Ext4InodeHold::io));
        if (!released) { CHECK(f.node->release()); released = true; }
        CHECK(f.mount->lifetime_.snapshot(f.key).value().holds[descriptor] == 0);
        const auto reads = f.disk.reads, writes = f.disk.writes, flushes = f.disk.flushes;
        CHECK(f.node->write(100, replacement).code() == StatusCode::busy);
        CHECK(f.node->truncate(4000).code() == StatusCode::busy);
        CHECK(f.node->write(100, {}).code() == StatusCode::busy);
        CHECK(f.node->retain().code() == StatusCode::busy);
        CHECK(f.mount->begin_drain().code() == StatusCode::busy);
        CHECK(f.disk.reads == reads && f.disk.writes == writes && f.disk.flushes == flushes);
    };
    CHECK(f.operate(kind)); f.disk.hook = {}; CHECK(released);
    CHECK(f.mount->report().phase == Ext4MountPhase::readable && f.mount->report().epoch == 2);
    f.no_hold(); CHECK(f.mount->lifetime_.snapshot(f.key).value().total() == 0);
    const std::vector<Trace> trace(f.disk.trace.begin() + start, f.disk.trace.end());
    CHECK(f.node->retain()); f.release(); ++positives;
    return trace;
}

void rejected_and_zero(const Bytes &image)
{
    for (usize kind = 0; kind < 4; ++kind)
    {
        Fixture f(image); const auto epoch = f.mount->report().epoch;
        const auto writes = f.disk.writes, flushes = f.disk.flushes;
        auto status = kind == 0 ? f.node->write(4095, replacement) : kind == 1 ? f.node->truncate(0) :
                      kind == 2 ? f.node->truncate(4096) : f.node->write(100, {});
        CHECK(kind < 2 ? !status : bool(status));
        CHECK(f.disk.writes == writes && f.disk.flushes == flushes && f.mount->report().epoch == epoch);
        CHECK(f.mount->report().phase == Ext4MountPhase::readable);
        f.no_hold(); f.release(); kind < 2 ? ++refusals : ++positives;
    }
    Fixture maintenance(image, Ext4MountedPolicy::maintenance_only);
    bool held{};
    maintenance.disk.hook = [&](char call) {
        CHECK(call == 'r'); held = true;
        CHECK(maintenance.mount->report().reading);
        CHECK(maintenance.mount->lifetime_.snapshot(maintenance.key).value().holds[io] == 1);
    };
    CHECK(maintenance.node->write(0, {})); maintenance.disk.hook = {}; CHECK(held);
    maintenance.no_hold(); maintenance.release(); ++positives;
}

void every_transport_failure(const Bytes &image, usize kind, const std::vector<Trace> &trace)
{
    usize read{}, write{}, flush{};
    for (const auto &call : trace)
    {
        Fixture f(image);
        if (call.operation == 'r') f.disk.fail_read = f.disk.reads + ++read;
        else if (call.operation == 'w') f.disk.fail_write = f.disk.writes + ++write;
        else { CHECK(call.operation == 'f'); f.disk.fail_flush = f.disk.flushes + ++flush; }
        bool callback{};
        f.disk.hook = [&](char) {
            callback = true;
            CHECK(f.mount->lifetime_.snapshot(f.key).value().holds[io] == 1);
            CHECK(f.mount->report().phase == Ext4MountPhase::updating);
        };
        CHECK(!f.operate(kind) && callback); f.disk.hook = {};
        f.no_hold(); f.release(); ++fault_controls; ++refusals;
    }
    std::printf("PASS mounted mutation transport kind=%zu reads=%zu writes=%zu flushes=%zu faults=%zu\n", kind, read, write, flush, trace.size());
}
} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 2); std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    const Bytes image((std::istreambuf_iterator<char>(input)), {}); CHECK(image.size() == 32 * 1024 * 1024);
    ok::testing::ext4_clock::set({{1700000400, 123456789}, {400, 1}, 1,
                                time::RealtimeOrigin::administrator_software});
    address_and_identity(image); physical_generations(image); real_capacity(image); rejected_and_zero(image);
    original_binding_drift(image); public_admission_drift(image); original_role_release_boundary(image);
    for (usize kind = 0; kind < 2; ++kind)
    { const auto trace = success_and_runtime(image, kind); every_transport_failure(image, kind, trace); }
    std::printf("PASS mounted mutation identity checks=%zu identity=%zu generation=%zu capacity=%zu runtime_callbacks=%zu binding_drift=%zu admission_drift=%zu role_release=%zu transport_faults=%zu positives=%zu refusals=%zu actual_node=1 actual_io_lease=1 physical_generation=1 original_noncopyable_context=1 actual_mutation_role=1 original_terminal_release=1\n",
        checks, identity_controls, generation_controls, capacity_controls, runtime_callbacks, binding_controls, admission_controls, release_controls, fault_controls, positives, refusals);
    return 0;
}
