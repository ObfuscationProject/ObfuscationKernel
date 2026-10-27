// Private hosted common-opening model. Media, native counters and retained
// authority are never altered while an opening is active.
#include "../ext4_mounted_support.hpp"
#include <ok/fs/ext4/mount.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>

namespace
{
using namespace ok;
using namespace ok::fs;
using namespace owned_fixture;
using Mount = Ext4MountedMetadata<2>;
using Owner = Ext4OwnedTransaction<2, 8>;
using Block = Ext4FirstDataBlock;
usize checks{}, successes{}, failures{}, callbacks{}, native_preparations{};
std::filesystem::path outputs;

#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "common legacy:%d: %s\n", __LINE__, #expression); std::abort(); \
} } while (false)

void checked(Status status, const char *label)
{
    ++checks;
    if (!status)
    {
        std::fprintf(stderr, "common legacy %s: code=%u detail=%.*s\n", label, unsigned(status.code()),
            int(status.message().size()), status.message().data());
        std::abort();
    }
}

Bytes load(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary); CHECK(input);
    Bytes image((std::istreambuf_iterator<char>(input)), {});
    CHECK(image.size() == 32 * 1024 * 1024); return image;
}

struct NativeLegacy
{
    Bytes active, terminal;
};

// The inherited legacy startup fixture uses this exact native first-data
// preparation. Its committed cut is copied before the actual checkpoint; no
// descriptor, report, operation serial or Session history is synthesized.
NativeLegacy prepare_legacy(const Bytes &seed)
{
    Disk disk(seed); Owner::Registry registry;
    const auto key = registry.register_device(disk, 55100); CHECK(key);
    auto owner = std::make_unique<Owner>(true);
    checked(owner->open(registry, key.value(), {0, seed.size() / 512}), "native legacy open");
    const auto initial = owner->session_report(); CHECK(initial);
    CHECK(!initial.value().reads && !initial.value().completed_reads && !initial.value().writes &&
        !initial.value().completed_writes && !initial.value().flushes && !initial.value().completed_flushes);
    Block data{}; data.fill('L');
    checked(owner->prepare_first_data({14, 1024, 1700000300, 123456789}, 0, data), "native legacy preparation");
    checked(owner->commit(), "native legacy committed cut");
    CHECK(disk.live == disk.durable && disk.pending.empty());
    NativeLegacy result{disk.durable, {}};
    CHECK(be32(result.active, journal(result.active) + 28) == 1);
    checked(owner->checkpoint(), "native legacy checkpoint");
    CHECK(disk.live == disk.durable && disk.pending.empty());
    const auto terminal = owner->session_report(); CHECK(terminal);
    CHECK(terminal.value().session_serial == initial.value().session_serial &&
        terminal.value().reads == disk.reads && terminal.value().completed_reads == disk.reads &&
        terminal.value().writes == disk.writes && terminal.value().completed_writes == disk.writes &&
        terminal.value().flushes == disk.flushes && terminal.value().completed_flushes == disk.flushes &&
        !terminal.value().in_flight && !terminal.value().pending_writes);
    result.terminal = disk.durable;
    checked(owner->close(), "native legacy close");
    checked(registry.detach(key.value()), "native legacy detach");
    ++native_preparations; return result;
}

Bytes prepare_reclaim(const Bytes &orphan)
{
    Disk disk(orphan); Owner::Registry registry;
    const auto key=registry.register_device(disk,55103); CHECK(key);
    auto startup=std::make_unique<Ext4MountedStartupWorkspace>();
    auto mount=std::make_unique<Mount>(Ext4MountedPolicy::ordered_data_dirty_noatime,nullptr,startup.get());
    // Four genuine successful barriers publish the native reclaim WAL. The
    // fifth transport refusal leaves that exact committed durable before-home
    // cut; no pending write or authoritative history is promoted to durable.
    disk.fail_flush=5;
    const auto status=mount->mount(registry,key.value(),{0,orphan.size()/512});
    CHECK(!status && status.code()==StatusCode::io_error && disk.flushes==5);
    const auto actual=registry.inspect(key.value()); CHECK(actual);
    CHECK(actual.value().state==block::DeviceState::quarantined && actual.value().first_error==StatusCode::io_error &&
        actual.value().completed_flushes==4 && actual.value().session_serial==mount->identity_.session_serial);
    CHECK(be32(disk.durable,journal(disk.durable)+28)==1 && r32(disk.durable,1256)==r32(orphan,1256));
    const auto result=disk.durable;
    std::ofstream saved(outputs/"native-reclaim-committed.img",std::ios::binary); CHECK(saved);
    saved.write(reinterpret_cast<const char *>(result.data()),std::streamsize(result.size())); CHECK(saved);
    ++native_preparations; return result;
}

struct Observation
{
    bool selected{};
    block::SessionReport baseline{}, endpoint{};
    usize root_reads{}, first_root_read{};
};

struct Case
{
    Disk disk;
    Mount::Registry registry;
    Mount::Registry::Registration key{};
    std::unique_ptr<Ext4MountedStartupWorkspace> startup = std::make_unique<Ext4MountedStartupWorkspace>();
    std::unique_ptr<Owner::TypedOwnerWorkspace> child =
        std::unique_ptr<Owner::TypedOwnerWorkspace>(new Owner::TypedOwnerWorkspace);
    std::unique_ptr<Mount> mount = std::make_unique<Mount>(
        Ext4MountedPolicy::ordered_data_dirty_noatime, nullptr, startup.get());
    Ext4MountedCommonFamily expected;
    u8 locator_reads{};
    Observation observation{};
    usize typed_callbacks{};

    Case(const Bytes &input, Ext4MountedCommonFamily family, u8 locator)
        : disk(input), expected(family), locator_reads(locator)
    {
        disk.origin = 23;
        const auto registration = registry.register_device(disk, 55101, 65536); CHECK(registration);
        key = registration.value();
        disk.hook = [&](char operation) { observe(operation); };
    }

    Status run()
    {
        return mount->common_mount_scope(registry, key, {23, 65536}, *child,
            [&](const auto &) -> Status {
                ++typed_callbacks;
                return Status::fault("legacy selected family invoked a typed callback");
            });
    }

    void observe(char operation)
    {
        auto &owner = mount->owner_;
        const auto state = mount->report();
        CHECK(state.phase == Ext4MountPhase::opening && !state.epoch && !state.published);
        CHECK(!mount->root() && !mount->lifetime_snapshot(2));
        const auto actual = owner.session_report(); CHECK(actual);
        CHECK(actual.value().state == block::DeviceState::owned && actual.value().in_flight &&
            actual.value().first_error == StatusCode::ok);
        const auto *opening = owner.common_opening_; CHECK(opening);
        CHECK(opening->original_ == opening && &opening->owner_ == &owner &&
            &opening->mount_ == mount.get() && &opening->startup_ == startup.get() &&
            &opening->workspace_ == child.get() && &opening->session_ == &owner.cache_.session_);
        CHECK(opening->initial_ && opening->binding_ && opening->selection_);
        CHECK(!opening->initial_.value().reads && !opening->initial_.value().completed_reads &&
            !opening->initial_.value().writes && !opening->initial_.value().completed_writes &&
            !opening->initial_.value().flushes && !opening->initial_.value().completed_flushes);
        const auto &selection = *opening->selection_;
        if (selection.minted_)
        {
            CHECK(selection.original_ == &selection && &selection.opening_ == opening &&
                selection.family_ == expected && selection.reads_ == locator_reads);
            CHECK(selection.endpoint_.reads == locator_reads && selection.endpoint_.completed_reads == locator_reads &&
                !selection.endpoint_.writes && !selection.endpoint_.completed_writes &&
                !selection.endpoint_.flushes && !selection.endpoint_.completed_flushes &&
                !selection.endpoint_.in_flight && !selection.endpoint_.pending_writes);
            CHECK(selection.endpoint_.session_serial == opening->initial_.value().session_serial &&
                selection.endpoint_.identity == opening->initial_.value().identity &&
                selection.endpoint_.registration_generation == opening->initial_.value().registration_generation);
            CHECK(selection.sources_ == (std::array<Block, 5>{startup->locator_super_, startup->locator_descriptors_,
                startup->locator_table_, startup->locator_head_, startup->locator_descriptor_}));
            if (!observation.selected)
            {
                observation.selected = true; observation.baseline = opening->initial_.value();
                observation.endpoint = selection.endpoint_;
            }
            else CHECK(Owner::exact_session(observation.endpoint, selection.endpoint_));
        }
        if (operation == 'r' && owner.common_root_domain_active())
        {
            if (!observation.first_root_read) observation.first_root_read = disk.reads + 1;
            ++observation.root_reads;
            CHECK(observation.selected && !owner.startup_active_ && !owner.startup_route_opening_ &&
                owner.startup_report_.phase == Ext4StartupPhase::complete && opening->root_reads_ < 18);
        }
        // These ordinary calls refuse quietly throughout the original common
        // context. The observer reads facts only; no retained pointer changes.
        const auto before = disk.trace.size();
        typename Owner::Pin public_pin;
        CHECK(owner.pin(1, public_pin).code() == StatusCode::busy && !public_pin.active_);
        CHECK(owner.close().code() == StatusCode::busy && owner.discard().code() == StatusCode::busy);
        CHECK(mount->sync().code() == StatusCode::busy && mount->close().code() == StatusCode::busy &&
            mount->freeze_clean().code() == StatusCode::busy);
        CHECK(disk.trace.size() == before);
        Mount::Registry::Session foreign;
        CHECK(registry.acquire(key, {23, 65536}, block::SessionAccess::readonly, foreign).code() == StatusCode::busy);
        ++callbacks;
    }
};

Ext4StartupFamily observed_family(Ext4MountedCommonFamily family)
{
    switch (family)
    {
        case Ext4MountedCommonFamily::ordinary: return Ext4StartupFamily::ordinary;
        case Ext4MountedCommonFamily::legacy: return Ext4StartupFamily::legacy_active;
        case Ext4MountedCommonFamily::empty_orphan: return Ext4StartupFamily::empty_orphan;
        case Ext4MountedCommonFamily::namespace_recovery: return Ext4StartupFamily::namespace_active;
        case Ext4MountedCommonFamily::reclaim: return Ext4StartupFamily::reclaim_active;
        default: std::abort();
    }
}

Observation success(const Bytes &input, const Bytes &golden, Ext4MountedCommonFamily family, u8 locator)
{
    auto c = std::make_unique<Case>(input, family, locator);
    checked(c->run(), "common legacy opening"); c->disk.hook = {};
    CHECK(!c->typed_callbacks && c->observation.selected && !c->mount->owner_.common_opening_);
    const auto startup = c->mount->startup_report();
    const auto actual = c->registry.inspect(c->key); CHECK(actual);
    CHECK(startup.phase == Ext4StartupPhase::complete && startup.initial_family == observed_family(family) &&
        startup.strict_ordinary_admission && startup.root_unpublished && !startup.final_head);
    CHECK(Owner::exact_session(startup.original, c->observation.baseline));
    CHECK(actual.value().session_serial == c->observation.endpoint.session_serial &&
        actual.value().identity == c->observation.endpoint.identity &&
        actual.value().registration_generation == c->observation.endpoint.registration_generation &&
        actual.value().state == block::DeviceState::owned && actual.value().first_error == StatusCode::ok &&
        !actual.value().in_flight && !actual.value().pending_writes);
    CHECK(actual.value().reads == c->disk.reads && actual.value().completed_reads == c->disk.reads &&
        actual.value().writes == c->disk.writes && actual.value().completed_writes == c->disk.writes &&
        actual.value().flushes == c->disk.flushes && actual.value().completed_flushes == c->disk.flushes);
    CHECK(actual.value().reads - startup.observed.reads == c->observation.root_reads && c->observation.root_reads <= 18);
    {
        std::ofstream saved(outputs / ("family-" + std::to_string(unsigned(family)) + "-sequence-" +
            std::to_string(startup.final_sequence) + "-terminal.img"), std::ios::binary);
        CHECK(saved); saved.write(reinterpret_cast<const char *>(c->disk.durable.data()), std::streamsize(c->disk.durable.size()));
        CHECK(saved);
    }
    if (c->disk.live != golden)
    {
        usize shown{};
        for (usize at=0;at<golden.size() && shown<12;++at) if (c->disk.live[at]!=golden[at])
        { std::fprintf(stderr,"common legacy family=%u mismatch offset=%zu actual=%u expected=%u\n",unsigned(family),at,unsigned(c->disk.live[at]),unsigned(golden[at])); ++shown; }
    }
    CHECK(c->disk.live == golden && c->disk.durable == golden && c->disk.pending.empty());
    CHECK(c->mount->report().phase == Ext4MountPhase::readable && c->mount->report().epoch == 1 &&
        !c->mount->report().published && c->mount->lifetime_snapshot(2));
    {
        // A completed legacy route still retains its original permanent child
        // borrow. Reusing that storage through a different real Mount is a
        // normal lifetime refusal, with no native opening or transport.
        Disk alternate_disk(golden);
        const auto alternate_key = c->registry.register_device(alternate_disk, 55102, 65536); CHECK(alternate_key);
        auto alternate_startup = std::make_unique<Ext4MountedStartupWorkspace>();
        auto alternate = std::make_unique<Mount>(Ext4MountedPolicy::ordered_data_dirty_noatime,
            nullptr, alternate_startup.get());
        const auto before = c->disk.trace.size();
        usize alternate_callbacks{};
        const auto refused = alternate->common_mount_scope(c->registry, alternate_key.value(), {0, 65536}, *c->child,
            [&](const auto &) -> Status { ++alternate_callbacks; return {}; });
        CHECK(!refused && !alternate_callbacks && c->disk.trace.size() == before &&
            !alternate_disk.reads && !alternate_disk.writes && !alternate_disk.flushes);
        const auto retained = c->registry.inspect(c->key); CHECK(retained);
        CHECK(Owner::exact_session(actual.value(), retained.value()) &&
            c->mount->report().phase == Ext4MountPhase::readable);
        auto foreign_owner=std::make_unique<Owner>(true);
        checked(foreign_owner->open(c->registry,alternate_key.value(),{0,65536}),"open quiet foreign direct-child Owner");
        const auto direct=foreign_owner->run_typed_zero(*c->child,c->mount->binding_,
            {14,1,0,1700000300,123456789});
        CHECK(!direct && direct.code()==StatusCode::bad_descriptor &&
            !alternate_disk.reads && !alternate_disk.writes && !alternate_disk.flushes);
        checked(foreign_owner->close(),"close quietly refused foreign direct-child Owner");
        checked(c->registry.detach(alternate_key.value()), "detach quiet alternate common backing");
    }
    for (const auto &event : c->disk.trace)
        if (event.operation != 'f') CHECK(event.sector >= 23 && event.sector + event.size / 512 <= 23 + 65536);
    auto root = c->mount->retain_root(); CHECK(root && root.value().valid());
    auto duplicate = root.value().clone(); CHECK(duplicate && duplicate.value().valid());
    const auto before = c->disk.trace.size();
    CHECK(c->mount->close().code() == StatusCode::busy && c->disk.trace.size() == before);
    checked(duplicate.value().release(), "release cloned common root");
    checked(root.value().release(), "release original common root");
    checked(c->mount->close(), "close common legacy mount");
    CHECK(c->mount->report().phase == Ext4MountPhase::closed);
    const auto closed = c->registry.inspect(c->key); CHECK(closed);
    CHECK(closed.value().state == block::DeviceState::ready &&
        closed.value().session_serial == actual.value().session_serial && c->disk.trace.size() == before);
    checked(c->registry.detach(c->key), "detach closed common legacy backing");
    ++successes;
    std::printf("COMMON_LEGACY_PASS family=%u locator=%u reads=%llu writes=%llu flushes=%llu root_reads=%zu original_serial=%llu\n",
        unsigned(family), unsigned(locator), static_cast<unsigned long long>(actual.value().reads),
        static_cast<unsigned long long>(actual.value().writes), static_cast<unsigned long long>(actual.value().flushes),
        c->observation.root_reads, static_cast<unsigned long long>(actual.value().session_serial));
    std::fflush(stdout);
    return c->observation;
}

void transport_failure(const Bytes &input, Ext4MountedCommonFamily family, u8 locator, char operation, usize ordinal)
{
    auto c = std::make_unique<Case>(input, family, locator);
    if (operation == 'r') c->disk.fail_read = ordinal;
    if (operation == 'w') c->disk.fail_write = ordinal;
    if (operation == 'f') c->disk.fail_flush = ordinal;
    const auto status = c->run(); c->disk.hook = {};
    CHECK(status.code() == StatusCode::io_error && !c->typed_callbacks && !c->mount->owner_.common_opening_);
    const auto actual = c->registry.inspect(c->key); CHECK(actual);
    CHECK(actual.value().state == block::DeviceState::quarantined && actual.value().first_error == StatusCode::io_error &&
        !actual.value().in_flight && c->mount->report().phase == Ext4MountPhase::failed &&
        !c->mount->root() && !c->mount->lifetime_snapshot(2));
    CHECK(!c->disk.trace.empty() && c->disk.trace.back().operation == operation);
    if (operation == 'r') CHECK(actual.value().reads == ordinal && actual.value().completed_reads + 1 == ordinal);
    if (operation == 'w') CHECK(actual.value().writes == ordinal && actual.value().completed_writes + 1 == ordinal);
    if (operation == 'f') CHECK(actual.value().flushes == ordinal && actual.value().completed_flushes + 1 == ordinal);
    if (operation == 'r' && ordinal <= locator)
        CHECK(!c->observation.selected && !c->disk.writes && !c->disk.flushes && c->disk.live == input && c->disk.durable == input);
    const auto before = c->disk.trace.size();
    CHECK(!c->run() && !c->mount->retain_root() && !c->mount->sync() && !c->mount->close() && !c->mount->freeze_clean());
    CHECK(c->disk.trace.size() == before); ++failures;
    std::printf("COMMON_LEGACY_TRANSPORT_PASS family=%u operation=%c ordinal=%zu original_error=1 quiet_after_failure=1\n",
        unsigned(family), operation, ordinal);
}
} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 3); outputs=argv[2]; CHECK(std::filesystem::is_directory(outputs));
    const std::filesystem::path inputs(argv[1]);
    const auto ordinary = load(inputs / "ordinary.img");
    const auto orphan = load(inputs / "empty-orphan.img");
    const auto orphan_golden = load(inputs / "empty-orphan-golden.img");
    const auto namespace_input = load(inputs / "namespace.img");
    const auto namespace_golden = load(inputs / "namespace-golden.img");
    const auto reclaim = prepare_reclaim(orphan);
    const auto &reclaim_golden = orphan_golden;
    const auto legacy = prepare_legacy(load(inputs / "legacy-seed.img"));
    const auto ordinary_observation = success(ordinary, ordinary, Ext4MountedCommonFamily::ordinary, 4);
    // Pre-opening pure sequence-envelope controls. Empty ordinary completion
    // preserves these genuine source bytes and admits neither READ history
    // synthesis nor a sequence-advancing writer.
    for (u32 sequence:{~u32{0}-1,~u32{0}})
    {
        auto boundary=ordinary; const usize first=journal(boundary);
        big32(boundary,first+24,sequence); big32(boundary,first+252,0);
        big32(boundary,first+252,crc(~u32{0},boundary,first,1024));
        success(boundary,boundary,Ext4MountedCommonFamily::ordinary,4);
    }
    success(orphan, orphan_golden, Ext4MountedCommonFamily::empty_orphan, 4);
    success(namespace_input, namespace_golden, Ext4MountedCommonFamily::namespace_recovery, 5);
    success(reclaim, reclaim_golden, Ext4MountedCommonFamily::reclaim, 5);
    const auto legacy_observation = success(legacy.active, legacy.terminal, Ext4MountedCommonFamily::legacy, 5);
    transport_failure(ordinary, Ext4MountedCommonFamily::ordinary, 4, 'r', 4);
    transport_failure(orphan, Ext4MountedCommonFamily::empty_orphan, 4, 'r', 4);
    transport_failure(namespace_input, Ext4MountedCommonFamily::namespace_recovery, 5, 'r', 5);
    transport_failure(reclaim, Ext4MountedCommonFamily::reclaim, 5, 'r', 5);
    transport_failure(legacy.active, Ext4MountedCommonFamily::legacy, 5, 'r', 5);
    transport_failure(orphan, Ext4MountedCommonFamily::empty_orphan, 4, 'r', 5);
    transport_failure(namespace_input, Ext4MountedCommonFamily::namespace_recovery, 5, 'r', 6);
    transport_failure(reclaim, Ext4MountedCommonFamily::reclaim, 5, 'r', 6);
    transport_failure(legacy.active, Ext4MountedCommonFamily::legacy, 5, 'r', 6);
    for (char operation : {'w', 'f'})
    {
        transport_failure(orphan, Ext4MountedCommonFamily::empty_orphan, 4, operation, 1);
        transport_failure(namespace_input, Ext4MountedCommonFamily::namespace_recovery, 5, operation, 1);
        transport_failure(reclaim, Ext4MountedCommonFamily::reclaim, 5, operation, 1);
        transport_failure(legacy.active, Ext4MountedCommonFamily::legacy, 5, operation, 1);
    }
    // Select a real miss observed during the final root decoder. Never force a
    // cache miss by changing its entries or reconstructing native counters.
    CHECK(ordinary_observation.first_root_read || legacy_observation.first_root_read);
    if (ordinary_observation.first_root_read)
        transport_failure(ordinary, Ext4MountedCommonFamily::ordinary, 4, 'r', ordinary_observation.first_root_read);
    else transport_failure(legacy.active, Ext4MountedCommonFamily::legacy, 5, 'r', legacy_observation.first_root_read);
    std::printf("COMMON_LEGACY_MODEL_PASS checks=%zu positives=%zu failures=%zu callbacks=%zu native_preparations=%zu native_history_modified=0 source_model_only=1\n",
        checks, successes, failures, callbacks, native_preparations);
    return 0;
}
