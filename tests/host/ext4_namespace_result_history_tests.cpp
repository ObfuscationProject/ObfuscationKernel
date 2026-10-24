// Hosted-only private inspection; production authority remains mount-owned.
#include "ext4_mounted_support.hpp"
#include "ext4_realtime_model.hpp"
#include <ok/fs/ext4_mounted_metadata.hpp>
#include <memory>

using namespace ok;
using namespace ok::fs;
using namespace owned_fixture;
using Mount = Ext4MountedMetadata<2>;
using Block = Ext4OrphanBlock;
static usize checks{}, callbacks{}, prospective{}, faults{}, drifts{}, locator_refusals{}, late_result_drifts{};
#define CHECK(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "FAIL namespace history:%d: %s\n", __LINE__, #value); std::abort(); } } while (false)
static Bytes load(const std::filesystem::path &path)
{
    std::ifstream stream(path, std::ios::binary); CHECK(stream);
    Bytes result((std::istreambuf_iterator<char>(stream)), {});
    CHECK(result.size() == 32 * 1024 * 1024); return result;
}
static void equal_block(const Bytes &golden, u64 physical, const Block &bytes)
{
    CHECK(physical < golden.size() / 1024);
    CHECK(std::equal(bytes.begin(), bytes.end(), golden.begin() + usize(physical) * 1024));
}
struct Case
{
    Disk disk;
    Mount::Registry registry;
    Mount::Registry::Registration key{};
    std::unique_ptr<Ext4MountedStartupWorkspace> workspace = std::make_unique<Ext4MountedStartupWorkspace>();
    std::unique_ptr<Ext4MountedReclaimWorkspace> live = std::make_unique<Ext4MountedReclaimWorkspace>();
    std::unique_ptr<Mount> mount;
    std::function<void()> change;
    const Bytes *prospective_golden{};
    bool prospective_seen{};
    usize prospective_reads{};
    explicit Case(const Bytes &image, const Bytes *prospective = nullptr) : disk(image), mount(std::make_unique<Mount>(
        Ext4MountedPolicy::ordered_data_dirty_noatime, live.get(), workspace.get()))
    {
        prospective_golden = prospective;
        disk.origin = 11;
        const auto registered = registry.register_device(disk, 49021, 1024); CHECK(registered); key = registered.value();
        disk.hook = [&](char operation) {
            CHECK(operation == 'r' || (prospective_golden && (operation == 'w' || operation == 'f')));
            const auto state = mount->report();
            CHECK(state.phase == Ext4MountPhase::opening && !state.epoch && !state.published && !mount->root());
            CHECK(mount->close().code() == StatusCode::busy && mount->sync().code() == StatusCode::busy &&
                mount->freeze_clean().code() == StatusCode::busy);
            auto io = registry.inspect(key); CHECK(io && io.value().in_flight);
            Mount::Registry::Session foreign;
            CHECK(registry.acquire(key, {11, 65536}, block::SessionAccess::readonly, foreign).code() == StatusCode::busy);
            if (prospective_golden && !prospective_seen && workspace->namespace_result_seal_.terminal)
            {
                const auto capture = mount->owner_.startup_namespace_capture_.report();
                CHECK(capture.valid && capture.prospective && capture.chain_members == 1 &&
                      capture.head == 14 && capture.sequence == 22 && !disk.writes && !disk.flushes);
                equal_block(*prospective_golden, 1, workspace->namespace_result_.superblock);
                equal_block(*prospective_golden, 2, workspace->namespace_result_.descriptors);
                prospective_reads = disk.reads; prospective_seen = true;
            }
            ++callbacks; if (change) change();
        };
    }
    Status run() { return mount->mount(registry, key, {11, 65536}); }
    void refused(bool validated, bool recognized = true)
    {
        const auto state = mount->report(); const auto report = mount->startup_report();
        CHECK(state.phase == Ext4MountPhase::failed && !state.epoch && !state.published && !mount->root());
        CHECK(report.initial_family == (recognized ? Ext4StartupFamily::namespace_active : Ext4StartupFamily::rejected) && !report.namespace_checkpoints &&
            !report.strict_ordinary_admission && report.namespace_result_validated == validated);
        CHECK(!disk.writes && !disk.flushes && disk.pending.empty());
        const auto io = registry.inspect(key); CHECK(io && io.value().state == block::DeviceState::quarantined);
        CHECK(io.value().identity == report.original.identity &&
            io.value().registration_generation == report.original.registration_generation &&
            io.value().session_serial == report.original.session_serial && !io.value().pending_writes && !io.value().in_flight);
        CHECK(io.value().reads == disk.reads && !io.value().writes && !io.value().flushes &&
            io.value().completed_reads <= disk.reads);
        CHECK(report.observed.reads == io.value().reads && report.observed.completed_reads == io.value().completed_reads);
        const auto reads = disk.reads; CHECK(!mount->mount(registry, key, {11, 65536})); CHECK(disk.reads == reads);
    }
};
static usize accepted(const Bytes &input, const Bytes &prospective_golden, const Bytes &final_golden)
{
    auto c = std::make_unique<Case>(input, &prospective_golden);
    CHECK(c->run());
    CHECK(c->prospective_seen && c->prospective_reads > 5);
    const auto startup = c->mount->startup_report();
    CHECK(startup.initial_family == Ext4StartupFamily::namespace_active &&
          startup.namespace_result_validated && startup.prospective_members == 1 &&
          startup.namespace_checkpoints == 1 && startup.chain_members == 1 &&
          startup.reclaimed == 1 && startup.final_head == 0 && startup.final_sequence == 25 &&
          startup.strict_ordinary_admission && startup.root_unpublished);
    CHECK(c->mount->report().phase == Ext4MountPhase::readable && c->mount->report().epoch == 1 && c->mount->root());
    CHECK(c->disk.writes > 0 && c->disk.flushes > 0 && c->disk.pending.empty() &&
          c->disk.live == final_golden && c->disk.durable == final_golden);
    CHECK(c->registry.inspect(c->key).value().completed_reads == c->disk.reads);
    const auto &capture = c->mount->owner_.startup_namespace_capture_;
    const auto &storage = c->workspace->namespace_result_;
    const auto report = capture.report();
    CHECK(report.valid && report.prospective && report.chain_members == 1 && report.head == 14 && report.sequence == 22);
    CHECK(c->mount->startup_report().prospective_members == 1);
    equal_block(prospective_golden, 1, storage.superblock); equal_block(prospective_golden, 2, storage.descriptors);
    equal_block(prospective_golden, capture.binding_.layout.journal_first_block, storage.journal);
    for (usize group = 0; group < 4; ++group)
    {
        equal_block(prospective_golden, capture.binding_.metadata[group * 4 + 1].first, storage.block_bitmaps[group]);
        equal_block(prospective_golden, capture.binding_.metadata[group * 4 + 2].first, storage.inode_bitmaps[group]);
        if (capture.initialized_[group])
            for (usize index = 0; index < 512; ++index)
                equal_block(prospective_golden, capture.binding_.metadata[group * 4 + 3].first + index, storage.inode_tables[group][index]);
    }
    for (usize index = 0; index < capture.directory_count_; ++index)
        equal_block(prospective_golden, storage.directories[index].physical, storage.directories[index].bytes);
    equal_block(prospective_golden, storage.chain[0].table_block, storage.chain[0].table);
    Ext4OrphanDirectoryQualifiedReclaimPlan plan{};
    CHECK(c->mount->owner_.startup_namespace_capture_.plan_head(report.head, report.generation, plan).code() == StatusCode::denied);
    CHECK(!plan.valid); ++prospective; return c->prospective_reads;
}
static void read_fault(const Bytes &input, usize ordinal)
{
    auto c = std::make_unique<Case>(input); c->disk.fail_read = ordinal;
    CHECK(!c->run()); c->refused(false, ordinal > 5);
    CHECK(c->registry.inspect(c->key).value().completed_reads + 1 == c->disk.reads);
    CHECK(c->disk.reads == ordinal && c->disk.live == input && c->disk.durable == input); ++faults;
}
static void drift(const Bytes &input, usize kind)
{
    auto c = std::make_unique<Case>(input); bool changed{};
    c->change = [&] {
        auto &capture = c->mount->owner_.startup_namespace_capture_;
        if (changed || capture.report_.phase != Ext4OrphanSnapshotPhase::opening ||
            (kind == 1 && capture.report_.table_blocks < 10)) return;
        changed = true;
        auto &history = c->workspace->namespace_history_;
        if (kind == 0)
        {
            usize index{};
            while (index < history.count_ && history.current_[index] == history.after_[index]) ++index;
            CHECK(index < history.count_);
            std::copy(history.after_[index].begin(), history.after_[index].end(),
                c->disk.live.begin() + usize(history.targets_[index]) * 1024);
            c->disk.durable = c->disk.live;
        }
        else if (kind == 1)
        {
            const auto at = inode_offset(c->disk.live, 2);
            w32(c->disk.live, at + 16, r32(c->disk.live, at + 16) + 1); inode_crc(c->disk.live, 2, at);
            c->disk.durable = c->disk.live;
        }
        else if (kind == 2) c->workspace->active_namespace_.current_[0][900] ^= 1;
        else if (kind == 3) history.empty_head_[900] ^= 1;
        else if (kind == 4) c->disk.writable = false;
        else if (kind == 5)
        {
            const auto at = descriptor(c->disk.live, 0);
            w16(c->disk.live, at + 18, r16(c->disk.live, at + 18) ^ 4);
            group_crc(c->disk.live, 0, at); c->disk.durable = c->disk.live;
        }
        else
        {
            const auto at = inode_offset(c->disk.live, 8);
            w32(c->disk.live, at + 12, r32(c->disk.live, at + 12) + 1);
            inode_crc(c->disk.live, 8, at); c->disk.durable = c->disk.live;
        }
    };
    CHECK(!c->run() && changed); c->refused(false); ++drifts;
}
static void locator_substitution(const Bytes &before, const Bytes &after)
{
    auto c = std::make_unique<Case>(before); bool changed{};
    c->change = [&] {
        if (changed || c->workspace->active_namespace_.report().phase != Ext4NamespaceRecoveryPhase::bootstrap) return;
        changed = true; c->disk.live = after; c->disk.durable = after;
    };
    CHECK(c->run().code() == StatusCode::fault && changed); c->refused(false);
    CHECK(!c->workspace->namespace_history_.ready_); ++locator_refusals;
}
static void late_result_drift(const Bytes &input, usize kind)
{
    auto c = std::make_unique<Case>(input); bool changed{};
    c->change = [&] {
        auto &capture = c->mount->owner_.startup_namespace_capture_;
        auto &seal = c->workspace->namespace_result_seal_;
        if (changed || !seal.ready) return;
        const usize total = 11 + capture.report_.table_blocks + capture.directory_count_ + capture.bootstrap_count_;
        if ((kind < 5 || kind == 10) && (capture.report_.phase != Ext4OrphanSnapshotPhase::opening ||
                         capture.report_.verified_blocks + 1 != total)) return;
        if (kind >= 5 && kind != 10 && capture.report_.phase != Ext4OrphanSnapshotPhase::admitted) return;
        changed = true;
        if (kind == 0 || kind == 5) c->workspace->namespace_result_.inode_tables[0][0][900] ^= 1;
        else if (kind == 1) capture.initialized_[3] = !capture.initialized_[3];
        else if (kind == 2) ++capture.directory_count_;
        else if (kind == 3) ++capture.binding_.metadata[0].first;
        else if (kind == 4) ++capture.report_.generation;
        else if (kind == 6) ++capture.report_.session_reads;
        else if (kind == 7) capture.storage_ = &seal.storage;
        else if (kind == 8) seal.ready = false;
        else if (kind == 9) seal.terminal = false;
        else if (kind == 10) capture.report_.verified_blocks += 2;
        else capture.namespace_sealed_ = false;
    };
    CHECK(!c->run() && changed); c->refused(false);
    CHECK(c->disk.live == input && c->disk.durable == input); ++late_result_drifts;
}
int main(int argc, char **argv)
{
    CHECK(argc == 3); const std::filesystem::path corpus(argv[1]);
    const auto golden = load(corpus / "terminal-golden.img");
    const auto final_golden = load(argv[2]);
    usize baseline_reads{};
    for (usize mask = 0; mask < 32; ++mask)
    {
        char name[32]; std::snprintf(name, sizeof(name), "mask-%02zx-input.img", mask);
        const auto reads = accepted(load(corpus / name), golden, final_golden); if (!mask) baseline_reads = reads;
    }
    const auto before = load(corpus / "mask-00-input.img");
    for (const auto ordinal : {usize{1}, baseline_reads / 2, baseline_reads}) read_fault(before, ordinal);
    for (usize kind = 0; kind < 7; ++kind) drift(before, kind);
    locator_substitution(before, load(corpus / "mask-1f-input.img"));
    for (usize kind = 0; kind < 12; ++kind) late_result_drift(before, kind);
    std::printf("PASS namespace_result_history checks=%zu prospective=%zu faults=%zu drifts=%zu locator_refusals=%zu late_result_drifts=%zu callbacks=%zu mounted_namespace_checkpoints=32 negative_writes=0 negative_flushes=0\n",
        checks, prospective, faults, drifts, locator_refusals, late_result_drifts, callbacks);
}
