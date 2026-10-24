// Hosted execution of the actual mounted Owner/cache/Session; no hardware claim.
#include "ext4_mounted_support.hpp"
#include <ok/fs/ext4_mounted_metadata.hpp>
#include <memory>

using namespace owned_fixture;
using Mount = Ext4MountedMetadata<1>;
using Owner = Ext4OwnedTransaction<1, 8>;
using Block = std::array<u8, 1024>;
static usize checks{}, mixtures{}, callbacks{}, refusals{}, faults{}, read_faults{}, locator_drifts{}, geometry_drifts{}, canonical_drifts{}, original_direct_drifts{}, original_canonical_drifts{}, coherent_workspace_drifts{}, nonreclaim_selector_drifts{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL legacy startup:%d: %s\n", __LINE__, #x); std::abort(); } } while (false)
static void checked(Status status)
{
    ++checks;
    if (!status) { std::fprintf(stderr, "FAIL legacy startup: %.*s\n", int(status.message().size()), status.message().data()); std::abort(); }
}
static Bytes load(const char *path)
{
    std::ifstream input(path, std::ios::binary); CHECK(input);
    Bytes image((std::istreambuf_iterator<char>(input)), {}); CHECK(image.size() == 32 * 1024 * 1024);
    return image;
}
struct Published
{
    Bytes active, final;
    std::array<u32, 6> targets{};
    usize actions{};
    usize ring{};
};
static Published generate(const Bytes &source, unsigned family)
{
    Disk disk(source); Owner::Registry registry;
    auto key = registry.register_device(disk, 71); CHECK(key);
    auto owner = std::make_unique<Owner>(true);
    checked(owner->open(registry, key.value(), {0, source.size() / 512}));
    Block data{}; data.fill('L');
    if (family == 0) checked(owner->prepare_first_data({14, 1024, 1700000300, 123456789}, 0, data));
    else if (family == 1) checked(owner->prepare_next_data({14, 1024, 2048, 1700000400, 234567891}, 1024, data));
    else if (family == 2) checked(owner->prepare_create({2, 1700000500, 123456789, "legacy-note",
                                                       {NodeType::regular, 0600, 1000, 1001}}));
    else checked(owner->prepare_rename({2, 12, 1700000600, 123456789, "seed", "legacy-seed"}));
    checked(owner->commit()); CHECK(disk.live == disk.durable);
    Published result; result.active = disk.durable; result.ring = journal(result.active);
    for (usize index = 0; index < result.targets.size(); ++index)
    {
        const auto tag = result.ring + 1024 + (index ? 44 + (index - 1) * 16 : 12);
        result.targets[index] = be32(result.active, tag); ++result.actions;
        if (be32(result.active, tag + 4) & jbd2::flag_last_tag) break;
    }
    CHECK(result.actions == (family < 2 ? 4 : family == 2 ? 6 : 3));
    checked(owner->checkpoint()); checked(owner->close());
    CHECK(disk.live == disk.durable); result.final = disk.durable; return result;
}
struct CallbackDisk final : driver::BlockDevice
{
    explicit CallbackDisk(Disk &disk) : disk_(disk) {}
    mutable std::function<void()> geometry_hook;
    driver::BlockGeometry geometry() const override
    { if (geometry_hook) geometry_hook(); return disk_.geometry(); }
    Status read_blocks(u64 first, std::span<std::byte> bytes) override { return disk_.read_blocks(first, bytes); }
    Status write_blocks(u64 first, std::span<const std::byte> bytes) override { return disk_.write_blocks(first, bytes); }
    Status flush() override { return disk_.flush(); }
  private:
    Disk &disk_;
};
struct Case
{
    Disk disk;
    CallbackDisk device{disk};
    Mount::Registry registry;
    Mount::Registry::Registration key{};
    std::unique_ptr<Ext4MountedStartupWorkspace> workspace = std::make_unique<Ext4MountedStartupWorkspace>();
    std::unique_ptr<Mount> mount;
    explicit Case(const Bytes &image, bool supplied = true) : disk(image),
        mount(std::make_unique<Mount>(Ext4MountedPolicy::ordered_data_dirty_noatime, nullptr,
                                     supplied ? workspace.get() : nullptr))
    {
        disk.origin = 19;
        auto registered = registry.register_device(device, 79, 65536); CHECK(registered); key = registered.value();
    }
    Status run() { return mount->mount(registry, key, {19, 65536}); }
    void observe()
    {
        disk.hook = [&](char) {
            CHECK(mount->report().phase == Ext4MountPhase::opening && !mount->report().published);
            CHECK(!mount->root() && !mount->lifetime_snapshot(2));
            CHECK(mount->sync().code() == StatusCode::busy && mount->close().code() == StatusCode::busy);
            Mount::Registry::Session foreign;
            CHECK(registry.acquire(key, {19, 65536}, block::SessionAccess::readonly, foreign).code() == StatusCode::busy);
            ++callbacks;
        };
    }
    void failed()
    {
        disk.hook = {};
        device.geometry_hook = {};
        CHECK(mount->report().phase == Ext4MountPhase::failed && !mount->root());
        const auto actual = registry.inspect(key); CHECK(actual);
        CHECK(actual.value().state == block::DeviceState::quarantined && !actual.value().in_flight);
        const auto observed = mount->startup_report().observed;
        CHECK(observed.reads == actual.value().reads && observed.writes == actual.value().writes &&
              observed.flushes == actual.value().flushes && observed.completed_reads == actual.value().completed_reads &&
              observed.completed_writes == actual.value().completed_writes && observed.completed_flushes == actual.value().completed_flushes);
        const auto before = disk.trace.size();
        CHECK(!run() && !mount->sync() && !mount->freeze_clean() && !mount->close());
        CHECK(disk.trace.size() == before);
    }
};
static void success(const Bytes &input, const Published &published, bool supplied = true)
{
    auto c = std::make_unique<Case>(input, supplied); c->observe(); checked(c->run()); c->disk.hook = {};
    CHECK(c->disk.live == published.final && c->disk.durable == published.final && c->disk.pending.empty());
    CHECK(c->mount->report().phase == Ext4MountPhase::readable && !c->mount->report().published && c->mount->report().epoch == 1);
    const auto io = c->registry.inspect(c->key).value();
    CHECK(io.reads == c->disk.reads && io.writes == c->disk.writes && io.flushes == c->disk.flushes &&
          io.reads == io.completed_reads && io.writes == io.completed_writes && io.flushes == io.completed_flushes && !io.pending_writes);
    if (supplied)
    {
        const auto report = c->mount->startup_report();
        CHECK(report.initial_family == Ext4StartupFamily::legacy_active && report.phase == Ext4StartupPhase::complete &&
              report.strict_ordinary_admission && report.root_unpublished && report.final_head == 0);
        CHECK(report.original.session_serial == io.session_serial && report.original.registration_generation == io.registration_generation);
    }
    CHECK(c->mount->root()); ++mixtures;
}
static void refusal(const Bytes &input)
{
    auto c = std::make_unique<Case>(input); c->observe(); CHECK(!c->run()); c->failed();
    CHECK(!c->disk.writes && !c->disk.flushes && c->disk.live == input && c->disk.durable == input); ++refusals;
}
struct ForeignSession
{
    Disk disk;
    Mount::Registry registry;
    Mount::Registry::Session session;
    Mount::Registry::Registration key{};
    block::SessionReport before{};
    explicit ForeignSession(const Bytes &image) : disk(image)
    {
        const auto registered = registry.register_device(disk, 83, 65536); CHECK(registered); key = registered.value();
        checked(registry.acquire(key, {0, image.size() / 512}, block::SessionAccess::readonly, session));
        const auto actual = session.report(); CHECK(actual); before = actual.value();
    }
    void quiet()
    {
        const auto actual = session.report(), registered = registry.inspect(key); CHECK(actual && registered);
        CHECK(reclaim_history::same_session(actual.value(), before) &&
              reclaim_history::same_session(registered.value(), before));
        CHECK(!disk.reads && !disk.writes && !disk.flushes && disk.trace.empty() &&
              disk.pending.empty() && disk.submitted.empty());
    }
};
static void original_route_controls(const Published &published)
{
    // Both branches borrow the original mounted Owner and Session. A coherent
    // alternate claim proves continuing domain validity is weaker than identity
    // with the workspace selected before this synchronous dispatch.
    for (bool canonical : {false, true})
        for (unsigned stage = 0; stage < 3; ++stage)
            for (bool alternate_attachment : {false, true})
            {
                auto c = std::make_unique<Case>(published.active);
                auto foreign = std::make_unique<ForeignSession>(published.active);
                auto alternate = std::make_unique<Ext4MountedStartupWorkspace>();
                auto &owner = c->mount->owner_;
                auto *const original_session = &owner.cache_.session_;
                auto *const original_workspace = c->workspace.get();
                bool introduced{};
                block::SessionReport attempted{}, paused{};
                usize physical_before{}, trace_before{}, loading_slot{8};
                MetadataBufferReport loading{};
                Ext4OrphanBlock head{}, descriptor{};
                const auto introduce = [&]() {
                    const auto io = original_session->report(); CHECK(io);
                    introduced = true; attempted = io.value(); paused = owner.reclaim_io_;
                    physical_before = attempted.completed_reads; trace_before = c->disk.trace.size();
                    CHECK(&owner.cache_.session_ == original_session && owner.startup_workspace_ == original_workspace &&
                          owner.startup_mount_ == c->mount.get() && owner.startup_legacy_selected_ &&
                          owner.startup_report_.initial_family == Ext4StartupFamily::legacy_active &&
                          owner.startup_report_.phase == Ext4StartupPhase::admitting &&
                          !owner.startup_route_opening_ && !owner.startup_reclaim_seal_.minted);
                    auto expected = paused; ++expected.reads; expected.in_flight = true;
                    CHECK(reclaim_history::same_session(attempted, expected));
                    CHECK(c->disk.reads == physical_before + (stage == 2 ? 1 : 0) &&
                          c->disk.writes == paused.completed_writes && c->disk.flushes == paused.completed_flushes);
                    CHECK(c->mount->report().phase == Ext4MountPhase::opening && !c->mount->report().published &&
                          !c->mount->root() && !c->mount->lifetime_snapshot(2));
                    for (const auto &pin : owner.pins_) CHECK(!pin.active_);
                    if (canonical)
                    {
                        const auto cache = owner.cache_.report(); CHECK(cache.in_flight && cache.read_pins == 1);
                        usize loading_count{};
                        for (usize slot = 0; slot < owner.cache_.entries_.size(); ++slot)
                        {
                            const auto entry = owner.cache_.entries_[slot].report;
                            if (entry.state == MetadataBufferState::loading)
                            {
                                ++loading_count; loading_slot = slot; loading = entry;
                                CHECK(entry.generation && entry.read_pins == 1 &&
                                      !entry.transaction_serial && !entry.staged_dirty);
                            }
                            else CHECK(!entry.read_pins);
                        }
                        CHECK(loading_count == 1 && loading_slot < owner.cache_.entries_.size());
                    }
                    else CHECK(attempted.reads == 6 && attempted.completed_reads == 5 && !owner.cache_.report().in_flight);
                    checked(owner.startup_domain_guard());
                    head = original_workspace->locator_head_; descriptor = original_workspace->locator_descriptor_;
                    if (alternate_attachment)
                    {
                        CHECK(alternate.get() != original_workspace && !original_workspace->namespace_result_seal_.ready &&
                              !original_workspace->namespace_result_seal_.terminal);
                        alternate->claimant_ = original_workspace->claimant_;
                        alternate->owner_ = original_workspace->owner_;
                        alternate->locator_super_ = original_workspace->locator_super_;
                        alternate->locator_descriptors_ = original_workspace->locator_descriptors_;
                        alternate->locator_table_ = original_workspace->locator_table_;
                        alternate->locator_head_ = head; alternate->locator_descriptor_ = descriptor;
                        alternate->legacy_head_seal_ = original_workspace->legacy_head_seal_;
                        alternate->legacy_descriptor_seal_ = original_workspace->legacy_descriptor_seal_;
                        owner.startup_workspace_ = alternate.get();
                        CHECK(alternate->claimant_ == c->mount.get() && alternate->owner_ == &owner &&
                              alternate->locator_head_ == alternate->legacy_head_seal_ &&
                              alternate->locator_descriptor_ == alternate->legacy_descriptor_seal_);
                        checked(owner.startup_domain_guard());
                    }
                    else owner.startup_reclaim_seal_.minted = true;
                    CHECK(&owner.cache_.session_ == original_session &&
                          reclaim_history::same_session(owner.reclaim_io_, paused) &&
                          original_workspace->claimant_ == c->mount.get() && original_workspace->owner_ == &owner &&
                          original_workspace->locator_head_ == head && original_workspace->locator_descriptor_ == descriptor);
                    foreign->quiet();
                };
                const auto selected = [&]() {
                    const auto io = original_session->report(); CHECK(io);
                    return io.value().in_flight && owner.startup_report_.phase == Ext4StartupPhase::admitting &&
                           (canonical ? owner.cache_.report().in_flight :
                            io.value().reads == 6 && io.value().completed_reads == 5 && !owner.cache_.report().in_flight);
                };
                c->device.geometry_hook = [&]() {
                    if (introduced || stage == 1 || !selected()) return;
                    const auto io = original_session->report(); CHECK(io);
                    if (stage == 0 ? c->disk.reads + 1 == io.value().reads : c->disk.reads == io.value().reads)
                        introduce();
                };
                c->disk.hook = [&](char operation) {
                    if (!introduced && stage == 1 && operation == 'r' && selected()) introduce();
                };
                CHECK(c->run().code() == StatusCode::fault && introduced); c->failed();
                const auto actual = original_session->report(); CHECK(actual);
                auto terminal = attempted; terminal.in_flight = false;
                terminal.state = block::DeviceState::quarantined; terminal.first_error = StatusCode::fault;
                CHECK(reclaim_history::same_session(actual.value(), terminal) &&
                      reclaim_history::same_session(owner.reclaim_io_, paused));
                CHECK(c->disk.reads == physical_before + (stage ? 1 : 0) &&
                      c->disk.trace.size() == trace_before + (stage == 1 ? 1 : 0) &&
                      c->disk.writes == attempted.completed_writes && c->disk.flushes == attempted.completed_flushes);
                CHECK(c->disk.live == (canonical ? published.final : published.active) &&
                      c->disk.durable == (canonical ? published.final : published.active) && c->disk.pending.empty());
                const auto cache = owner.cache_.report(); CHECK(!cache.in_flight && !cache.read_pins);
                for (const auto &entry : owner.cache_.entries_)
                    CHECK(!entry.report.read_pins && entry.report.state != MetadataBufferState::loading);
                for (const auto &pin : owner.pins_) CHECK(!pin.active_);
                if (canonical)
                {
                    const auto failed = owner.cache_.entries_[loading_slot].report;
                    CHECK(failed.state == MetadataBufferState::failed && failed.block == loading.block &&
                          failed.generation == loading.generation && !failed.read_pins &&
                          !failed.transaction_serial && !failed.staged_dirty);
                    ++original_canonical_drifts;
                }
                else ++original_direct_drifts;
                Owner::Pin refused;
                CHECK(!owner.startup_pin(1, refused) && !refused.active_ && !refused.owner_ && !refused.report());
                CHECK(!c->mount->root() && !c->mount->report().published &&
                      original_workspace->locator_head_ == head && original_workspace->locator_descriptor_ == descriptor);
                foreign->quiet();
                CHECK(foreign->disk.live == published.active && foreign->disk.durable == published.active);
                if (alternate_attachment) ++coherent_workspace_drifts;
                else ++nonreclaim_selector_drifts;
                checked(foreign->session.close());
            }
}
static void cases(const Published &published)
{
    success(published.active, published, false);
    for (usize mask = 0; mask < (usize{1} << published.actions); ++mask)
    {
        auto image = published.active;
        for (usize action = 0; action < published.actions; ++action)
            if (mask & (usize{1} << action))
                std::copy_n(published.final.begin() + usize(published.targets[action]) * 1024, 1024,
                            image.begin() + usize(published.targets[action]) * 1024);
        // Preserve the actual inherited barrier profile, including unsupported
        // mixtures. The original no-startup route supplies an execution oracle.
        auto original = std::make_unique<Case>(image, false);
        const auto inherited = original->run();
        if (inherited)
        {
            CHECK(original->disk.live == published.final && original->disk.durable == published.final);
            original.reset(); success(image, published);
        }
        else
        {
            CHECK(!original->disk.writes && !original->disk.flushes && original->disk.live == image);
            original.reset(); refusal(image);
        }
    }
    auto bad = published.active; bad[published.ring + 1024 + 1020] ^= 1; refusal(bad);
    bad = published.active; bad[published.ring + (published.actions + 2) * 1024 + 16] ^= 1; refusal(bad);
    bad = published.active; bad[published.ring + 2 * 1024 + 10] ^= 1; refusal(bad);
    auto positive = std::make_unique<Case>(published.active); checked(positive->run());
    usize before_flush{};
    for (const auto &event : positive->disk.trace)
    {
        if (event.operation == 'f') break;
        if (event.operation == 'r') ++before_flush;
    }
    CHECK(before_flush > 6);
    for (const auto cut : {usize{1}, usize{2}, usize{3}, usize{4}, usize{5},
                          before_flush - 1, before_flush, positive->disk.reads})
    {
        auto c = std::make_unique<Case>(published.active); c->observe(); c->disk.fail_read = cut;
        CHECK(!c->run()); c->failed(); CHECK(c->disk.reads == cut);
        if (cut <= before_flush) CHECK(!c->disk.writes && !c->disk.flushes);
        ++read_faults;
    }
    for (bool head : {false, true})
    {
        auto c = std::make_unique<Case>(published.active);
        c->disk.hook = [&](char operation) {
            if (operation == 'r' && c->disk.reads == 5)
            {
                auto &image = head ? c->workspace->legacy_head_seal_ : c->workspace->legacy_descriptor_seal_;
                image[100] ^= 1;
            }
        };
        CHECK(!c->run()); c->failed();
        CHECK(c->disk.reads == 6 && !c->disk.writes && !c->disk.flushes); ++locator_drifts;
    }
    for (bool write : {false, true})
        for (bool selection : {false, true})
        {
            auto c = std::make_unique<Case>(published.active); bool introduced{};
            c->device.geometry_hook = [&]() {
                const auto io = c->registry.inspect(c->key); CHECK(io);
                if (!introduced && io.value().in_flight &&
                    (write ? io.value().writes == 1 && !c->disk.writes : io.value().flushes == 1 && !c->disk.flushes))
                {
                    introduced = true;
                    if (selection) c->mount->owner_.startup_legacy_selected_ = false;
                    else c->workspace->legacy_descriptor_seal_[100] ^= 1;
                }
            };
            CHECK(!c->run() && introduced); c->failed();
            const auto io = c->registry.inspect(c->key).value();
            if (write) CHECK(!c->disk.writes && io.writes == 1 && !io.completed_writes && io.pending_writes);
            else CHECK(!c->disk.flushes && io.flushes == 1 && !io.completed_flushes);
            ++geometry_drifts;
        }
    for (bool selection : {false, true})
    {
        auto c = std::make_unique<Case>(published.active); bool introduced{};
        c->device.geometry_hook = [&]() {
            const auto io = c->registry.inspect(c->key); CHECK(io);
            if (!introduced && io.value().in_flight && io.value().reads == 6 && c->disk.reads == 5)
            {
                introduced = true;
                if (selection) c->mount->owner_.startup_legacy_selected_ = false;
                else c->workspace->legacy_descriptor_seal_[100] ^= 1;
            }
        };
        CHECK(!c->run() && introduced); c->failed();
        const auto io = c->registry.inspect(c->key).value();
        CHECK(c->disk.reads == 5 && io.reads == 6 && io.completed_reads == 5 &&
              !io.writes && !io.completed_writes && !io.flushes && !io.completed_flushes && !io.pending_writes);
        CHECK(c->disk.live == published.active && c->disk.durable == published.active);
        ++geometry_drifts;
    }
    for (unsigned stage = 0; stage < 3; ++stage)
        for (bool selection : {false, true})
        {
            auto c = std::make_unique<Case>(published.active); bool introduced{};
            block::SessionReport attempted{}; usize physical_before{};
            const auto introduce = [&]() {
                const auto io = c->registry.inspect(c->key); CHECK(io);
                introduced = true; attempted = io.value(); physical_before = attempted.completed_reads;
                CHECK(attempted.in_flight && attempted.reads == physical_before + 1 &&
                      c->disk.reads == physical_before + (stage == 2 ? 1 : 0));
                if (selection) c->mount->owner_.startup_legacy_selected_ = false;
                else c->workspace->legacy_descriptor_seal_[100] ^= 1;
            };
            c->device.geometry_hook = [&]() {
                const auto io = c->registry.inspect(c->key); CHECK(io);
                const auto cache = c->mount->owner_.cache_.report();
                if (!introduced && stage != 1 && io.value().in_flight && cache.in_flight &&
                    c->mount->startup_report().phase == Ext4StartupPhase::admitting &&
                    (stage ? c->disk.reads == io.value().reads : c->disk.reads + 1 == io.value().reads))
                    introduce();
            };
            c->disk.hook = [&](char operation) {
                if (introduced || stage != 1 || operation != 'r') return;
                const auto cache = c->mount->owner_.cache_.report();
                if (cache.in_flight && c->mount->startup_report().phase == Ext4StartupPhase::admitting)
                    introduce();
            };
            CHECK(!c->run() && introduced); c->failed();
            const auto io = c->registry.inspect(c->key).value();
            CHECK(io.reads == attempted.reads && io.completed_reads == attempted.completed_reads &&
                  io.writes == attempted.writes && io.completed_writes == attempted.completed_writes &&
                  io.flushes == attempted.flushes && io.completed_flushes == attempted.completed_flushes &&
                  io.pending_writes == attempted.pending_writes &&
                  c->disk.reads == physical_before + (stage ? 1 : 0));
            CHECK(c->disk.live == published.final && c->disk.durable == published.final);
            ++canonical_drifts;
        }
    original_route_controls(published);
    for (bool after : {false, true})
    {
        for (usize cut = 1; cut <= positive->disk.writes; ++cut)
        {
            auto c = std::make_unique<Case>(published.active); c->observe();
            (after ? c->disk.fail_write_after : c->disk.fail_write) = cut;
            CHECK(!c->run()); c->failed(); CHECK(c->disk.writes == cut); ++faults;
        }
        for (usize cut = 1; cut <= positive->disk.flushes; ++cut)
        {
            auto c = std::make_unique<Case>(published.active); c->observe();
            (after ? c->disk.fail_flush_after : c->disk.fail_flush) = cut;
            CHECK(!c->run()); c->failed(); CHECK(c->disk.flushes == cut); ++faults;
        }
    }
}
int main(int argc, char **argv)
{
    CHECK(argc == 3);
    auto clean = load(argv[1]); w32(clean, 1120, r32(clean, 1120) | 4); super_crc(clean);
    // Match the inherited bounded-create fixture preparation. Only these
    // unallocated private image slots change; the original input stays intact.
    const auto bitmap = usize(r32(clean, descriptor(clean, 0) + 4)) * 1024;
    for (const u32 inode : {14u, 15u})
    {
        CHECK(!(clean[bitmap + (inode - 1) / 8] & (1u << ((inode - 1) % 8))));
        std::fill_n(clean.begin() + inode_offset(clean, inode), 256, 0);
    }
    const auto empty = load(argv[2]);
    const auto first = generate(empty, 0); cases(first);
    cases(generate(first.final, 1)); cases(generate(clean, 2)); cases(generate(clean, 3));
    std::printf("PASS hosted legacy startup: checks=%zu mixtures=%zu callbacks=%zu refusals=%zu write_flush_faults=%zu read_faults=%zu locator_drifts=%zu geometry_drifts=%zu canonical_drifts=%zu original_direct_drifts=%zu original_canonical_drifts=%zu coherent_workspace_drifts=%zu nonreclaim_selector_drifts=%zu original_session=1 hardware=0\n",
                checks, mixtures, callbacks, refusals, faults, read_faults, locator_drifts, geometry_drifts, canonical_drifts,
                original_direct_drifts, original_canonical_drifts, coherent_workspace_drifts, nonreclaim_selector_drifts);
}
