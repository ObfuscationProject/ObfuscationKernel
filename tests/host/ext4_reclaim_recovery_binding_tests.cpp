// Actual hosted Session/device capture; no writable recovery or guest execution.
#include "snapshot_fixture.hpp"
#include "ok/fs/ext4_reclaim_history.hpp"
#define private public
#include "ok/fs/ext4_reclaim_recovery_binding.hpp"
#undef private
#include <functional>
#include <memory>
#include <type_traits>

namespace
{
using Registry = block::BlockSessions<2>;
using Admission = Ext4ReclaimRecoveryBinding<2>;
static_assert(!std::is_copy_constructible_v<Admission> && !std::is_move_constructible_v<Admission>);
constexpr u64 origin = 37;
void apply(Image &image, u32 physical, const Block &bytes)
{ for (usize at = 0; at < 1024; ++at) image[usize{physical} * 1024 + at] = bytes[at]; }

struct Scenario
{
    Ext4RecoveryInfo old_binding{};
    Image before;
    std::unique_ptr<Ext4OrphanDirectoryQualifiedReclaimScratch> qualified =
        std::make_unique<Ext4OrphanDirectoryQualifiedReclaimScratch>();
    Ext4OrphanDirectoryQualifiedReclaimPlan reclaim{};
    Ext4OrphanReclaimJournalScratch formatter{};
    Ext4OrphanReclaimJournalPlan journal{};
    Scenario(const Image &original, bool populated) : before(original)
    {
        MemoryDisk disk(original); Ext4RecoveryBinding admission;
        CHECK(admission.inspect(disk, {0, original.size() / 512}) && admission.info());
        old_binding = *admission.info(); old_binding.needs_recovery = true;
        Fixture fixture(original, old_binding);
        if (populated) fixture.add(original, 12, 14);
        fixture.add(original, 14, 0); fixture.head(populated ? 12 : 14);
        store32(fixture.super, 96, 0x46); fixture.stamp_super();
        Tables tables(original, fixture); DirectoryImages directories(original, fixture, tables);
        if (populated) directories.remove_target(fixture, tables, 12);
        directories.remove_target(fixture, tables, 14);
        apply(before, 1, fixture.super);
        for (usize at = 0; at < fixture.count; ++at) apply(before, fixture.images[at].table_block, fixture.images[at].table);
        for (const auto &directory : directories.blocks) apply(before, directory.physical, directory.bytes);
        prepare(populated ? 12 : 14);
    }
    void prepare(u32 inode)
    {
        Fixture fixture(before, old_binding);
        if (inode == 12) { fixture.add(before, 12, 14); fixture.add(before, 14, 0); }
        else fixture.add(before, 14, 0);
        for (usize at = 0; at < fixture.count; ++at) fixture.images[at].table = load(before, fixture.images[at].table_block);
        Tables tables(before, fixture); DirectoryImages directories(before, fixture, tables);
        CHECK(Ext4OrphanDirectoryQualifiedReclaimPlanner::plan(old_binding,
            {tables.source(fixture), directories.blocks}, *qualified, inode,
            ext4_checked::le32(fixture.images[0].table, slot(inode) + 100), reclaim));
        CHECK(reclaim.valid);
        CHECK(Ext4OrphanReclaimJournalPlanner::plan(old_binding, reclaim.reclaim,
            load(before, u32(old_binding.layout.journal_first_block)), 7, 13, formatter, journal));
    }
    Image mixture(unsigned mask) const
    {
        auto image = before;
        for (usize index = 0; index < reclaim.reclaim.count; ++index)
            if (mask & (1U << index)) apply(image, reclaim.reclaim.targets[index], reclaim.reclaim.after[index]);
        const u32 first = u32(old_binding.layout.journal_first_block);
        apply(image, first, journal.journal_head); apply(image, first + 1, journal.descriptor);
        for (usize index = 0; index < journal.count; ++index) apply(image, first + 2 + u32(index), journal.payloads[index]);
        apply(image, first + 2 + u32(journal.count), journal.commit);
        apply(image, first + 3 + u32(journal.count), journal.end);
        return image;
    }
};
bool legal(usize count, unsigned mask)
{
    const unsigned all = (1U << count) - 1;
    return !mask || ((mask & 1) && (!(mask & (1U << (count - 1))) || mask == all));
}
class Disk final : public driver::BlockDevice
{
  public:
    explicit Disk(const Image &image) : bytes(origin * 512, 0xa5)
    { bytes.insert(bytes.end(), image.begin(), image.end()); }
    driver::BlockGeometry geometry() const override { return {bytes.size() / 512, 512, true}; }
    Status read_blocks(u64 first, std::span<std::byte> out) override
    {
        ++reads;
        if (hook) hook();
        if (reads == fail_at) return Status::io_error("fresh binding injected read fault");
        if (first < origin || first >= bytes.size() / 512 || out.size() > bytes.size() - first * 512)
            return Status::io_error("fresh binding physical read bounds");
        for (usize at = 0; at < out.size(); ++at) out[at] = std::byte(bytes[first * 512 + at]);
        return {};
    }
    Status write_blocks(u64, std::span<const std::byte>) override
    { ++writes; return Status::denied("fresh binding test media write forbidden"); }
    Status flush() override { ++flushes; return Status::denied("fresh binding test FLUSH forbidden"); }
    Image bytes;
    usize reads{}, writes{}, flushes{}, fail_at{~usize{0}};
    std::function<void()> hook{};
};
struct Harness
{
    Disk disk;
    Registry registry;
    Registry::Registration key{};
    Registry::Session session;
    std::unique_ptr<Admission> admission = std::make_unique<Admission>();
    std::unique_ptr<Ext4ReclaimRecoveryStorage> storage = std::make_unique<Ext4ReclaimRecoveryStorage>();
    std::unique_ptr<Ext4OrphanReclaimColdScratch> scratch = std::make_unique<Ext4OrphanReclaimColdScratch>();
    Harness(const Image &image, block::SessionAccess access = block::SessionAccess::writable) : disk(image)
    {
        const auto registered = registry.register_device(disk, 994); CHECK(registered); key = registered.value();
        CHECK(registry.acquire(key, {origin, 65536}, access, session));
    }
    Status inspect() { return admission->inspect(session, *storage, *scratch); }
    void rejected()
    {
        CHECK(!admission->report().valid && !admission->info() && !admission->proof());
        CHECK(admission->report().phase == Ext4ReclaimRecoveryPhase::rejected);
        CHECK(registry.inspect(key).value().state == block::DeviceState::quarantined);
        CHECK(!admission->revalidate() && disk.writes == 0 && disk.flushes == 0);
    }
};
usize accepted{}, barrier_rejects{}, hostile_rejects{}, read_faults{}, callbacks{};
usize mixtures(const Scenario &scenario)
{
    usize reads{};
    for (unsigned mask = 0; mask < (1U << scenario.reclaim.reclaim.count); ++mask)
    {
        Harness h(scenario.mixture(mask));
        h.disk.hook = [&] { CHECK(h.registry.inspect(h.key)); ++callbacks; };
        const auto status = h.inspect();
        if (bool(status) != legal(scenario.reclaim.reclaim.count, mask))
        {
            std::fprintf(stderr, "FAIL fresh binding mask=%u: %u %.*s\n", mask,
                unsigned(status.code()), int(status.message().size()), status.message().data()); std::abort();
        }
        if (!status) { h.rejected(); ++barrier_rejects; continue; }
        CHECK(h.admission->report().recognized_marker && h.admission->report().valid);
        const auto &info = *h.admission->info(); const auto proof = *h.admission->proof();
        CHECK(info.layout.first_sector == 0 && info.uuid == scenario.old_binding.uuid && info.needs_recovery && !info.empty_log);
        CHECK(info.layout.journal_first_block == scenario.old_binding.layout.journal_first_block);
        CHECK(proof.inode == scenario.reclaim.reclaim.inode && proof.count == scenario.reclaim.reclaim.count);
        for (usize index = 0; index < proof.count; ++index) CHECK(proof.home_after[index] == bool(mask & (1U << index)));
        reads = h.disk.reads;
        CHECK(h.admission->report().reads == h.session.report().value().reads);
        CHECK(h.admission->revalidate() && h.admission->revalidate());
        CHECK(!h.inspect() && h.admission->report().valid);
        CHECK(h.disk.writes == 0 && h.disk.flushes == 0 && h.session.close()); ++accepted;
    }
    return reads;
}
usize retained_controls{}, final_callback_controls{}, outer_callback_controls{}, early_callback_controls{};
void retained_drift(const Scenario &scenario)
{
    using Change = std::function<void(Harness &)>;
    const std::vector<Change> changes{
        [](auto &h) { h.storage->bootstrap_reads[11].physical ^= 1; },
        [](auto &h) { h.storage->bootstrap_reads[11].bytes[1023] ^= 1; },
        [](auto &h) { h.storage->superblock[900] ^= 1; },
        [](auto &h) { h.storage->normalized[900] ^= 1; },
        [](auto &h) { h.storage->old_head[900] ^= 1; },
        [](auto &h) { h.storage->parser_record[900] ^= 1; },
        [](auto &h) { h.storage->payloads[4][900] ^= 1; },
        [](auto &h) { h.storage->before[4][900] ^= 1; },
        [](auto &h) { h.storage->after[4][900] ^= 1; },
        [](auto &h) { h.storage->actions[4].checksum ^= 1; },
        [](auto &h) { h.storage->revocations[0].target_block ^= 1; },
        [](auto &h) { h.storage->actual.tables[3][511][1023] ^= 1; },
        [](auto &h) { h.storage->actual.directories[255].inode ^= 1; },
        [](auto &h) { h.storage->actual.directories[255].bytes[1023] ^= 1; },
        [](auto &h) { h.storage->actual.inode_bitmaps[3][1023] ^= 1; },
        [](auto &h) { h.admission->binding_.metadata[127].count ^= 1; },
        [](auto &h) { h.admission->binding_.metadata_count = 129; },
        [](auto &h) { h.admission->targets_[4] ^= 1; },
        [](auto &h) { h.admission->count_ = 6; },
        [](auto &h) { h.admission->journal_first_ ^= 1; },
        [](auto &h) { h.admission->session_ = nullptr; },
        [](auto &h) { h.admission->storage_ = nullptr; },
        [](auto &h) { h.admission->scratch_ = nullptr; },
        [](auto &h) { h.admission->report_.reads ^= 1; },
        [](auto &h) { h.admission->report_.original.reads ^= 1; },
        [](auto &h) { h.admission->report_.observed.completed_reads ^= 1; },
        [](auto &h) { h.admission->report_.bootstrap_observations = 13; },
        [](auto &h) { h.admission->report_.phase = Ext4ReclaimRecoveryPhase::unused; },
        [](auto &h) { h.admission->established_ = false; },
        [](auto &h) { h.admission->records_sealed_ = false; h.admission->retained_.records_ready = false; h.storage->after[0][900] ^= 1; },
        [](auto &h) { h.admission->attempted_ = false; },
        [](auto &h) { h.admission->capture_.attempted_ = false; },
        [](auto &h) { h.admission->retained_.ready = false; },
        [](auto &h) { h.admission->retained_.terminal = false; },
        [](auto &h) { h.admission->capture_admitted_ = false; h.storage->actual.tables[3][511][1023] ^= 1; },
        [](auto &h) { h.admission->capture_.report_.semantic.home_after[0] = true; },
        [](auto &h) { h.admission->capture_.report_.semantic.inode ^= 1; },
        [](auto &h) { h.admission->capture_.report_.verified_blocks ^= 1; },
        [](auto &h) { h.admission->capture_.initialized_[3] = !h.admission->capture_.initialized_[3]; },
        [](auto &h) { h.admission->capture_.count_ = 6; },
        [](auto &h) { h.admission->capture_.directory_count_ = 257; },
        [](auto &h) { h.admission->capture_.storage_ = nullptr; },
        [](auto &h) { h.admission->capture_.scratch_ = nullptr; },
        [](auto &h) { h.admission->capture_.report_.phase = Ext4OrphanReclaimCapturePhase::unused; },
        [](auto &h) { h.admission->capture_.retained_.ready = false; },
        [](auto &h) { h.admission->capture_.retained_.terminal = false; },
        [](auto &h) { h.scratch->reclaim.reclaim.released_mapping[15] ^= 1; },
        [](auto &h) { h.scratch->reclaim.reclaim.before[4][900] ^= 1; },
        [](auto &h) { h.scratch->canonical.payloads[4][900] ^= 1; },
        [](auto &h) { h.scratch->canonical.ready = false; },
        [](auto &h) { h.scratch->before[4][900] ^= 1; },
        [](auto &h) { h.scratch->normalized[900] ^= 1; },
    };
    for (const auto &change : changes)
    {
        Harness h(scenario.mixture(0)); CHECK(h.inspect());
        const auto before = h.disk.reads;
        change(h); CHECK(!h.admission->revalidate());
        CHECK(h.disk.reads == before && h.disk.writes == 0 && h.disk.flushes == 0);
        CHECK(h.registry.inspect(h.key).value().state == block::DeviceState::quarantined);
        CHECK(!h.admission->info() && !h.admission->proof());
        // Restore deliberately substituted addresses only for teardown.
        if (!h.admission->session_) h.admission->session_ = &h.session;
        CHECK(!h.inspect()); ++retained_controls;
    }
    const std::vector<Change> late{
        [](auto &h) { h.storage->actual.tables[3][511][1023] ^= 1; },
        [](auto &h) { h.storage->actual.directories[255].physical ^= 1; },
        [](auto &h) { h.admission->capture_.count_ = 6; },
        [](auto &h) { h.admission->capture_.report_.reads ^= 1; },
        [](auto &h) { h.admission->capture_.report_.verified_blocks ^= 1; },
        [](auto &h) { h.admission->capture_.report_.semantic.inode ^= 1; },
        [](auto &h) { h.scratch->reclaim.reclaim.released_mapping[15] ^= 1; },
        [](auto &h) { h.scratch->canonical.ready = false; },
        [](auto &h) { h.storage->bootstrap_reads[0].bytes[900] ^= 1; },
        [](auto &h) { h.admission->report_.bootstrap_observations = 13; },
        [](auto &h) { h.admission->report_.original.reads ^= 1; },
    };
    for (const auto &change : late)
    {
        Harness h(scenario.mixture(0)); bool fired{};
        h.disk.hook = [&] {
            if (!fired && h.admission->capture_.established_)
            { fired = true; change(h); }
        };
        CHECK(!h.inspect() && fired); h.rejected(); ++final_callback_controls;
    }
    const std::vector<Change> outer_late{
        [](auto &h) { h.storage->actual.tables[3][511][1023] ^= 1; },
        [](auto &h) { h.admission->capture_.report_.semantic.inode ^= 1; },
        [](auto &h) { h.storage->bootstrap_reads[0].bytes[900] ^= 1; },
        [](auto &h) { h.admission->report_.observed.completed_reads ^= 1; },
        [](auto &h) { h.admission->capture_admitted_ = false; h.storage->actual.tables[3][511][1023] ^= 1; },
        [](auto &h) { h.admission->capture_.scratch_ = nullptr; },
    };
    for (const auto &change : outer_late)
    {
        Harness h(scenario.mixture(0)); bool fired{};
        h.disk.hook = [&] {
            if (!fired && h.admission->capture_admitted_)
            { fired = true; change(h); }
        };
        CHECK(!h.inspect() && fired); h.rejected(); ++outer_callback_controls;
    }

    {
        Harness h(scenario.mixture(0)); bool fired{};
        h.disk.hook = [&] {
            if (!fired && h.admission->capture_.attempted_ && !h.admission->capture_.established_)
            { fired = true; h.admission->capture_.binding_.metadata[127].count ^= 1; }
        };
        CHECK(!h.inspect() && fired); h.rejected(); ++early_callback_controls;
    }

    for (bool context : {false, true})
    {
        Harness h(scenario.mixture(0)); bool fired{};
        h.disk.hook = [&] {
            if (!fired && h.admission->capture_.attempted_ && !h.admission->capture_.established_)
            {
                fired = true;
                if (context) h.admission->capture_.outer_context_ = nullptr;
                else h.admission->capture_.outer_guard_ = nullptr;
            }
        };
        CHECK(!h.inspect() && fired); h.rejected(); ++early_callback_controls;
    }

    for (bool after : {false, true})
    {
        Harness h(scenario.mixture(0)); bool fired{};
        h.disk.hook = [&] {
            if (!fired && h.admission->report_.phase == Ext4ReclaimRecoveryPhase::before_binding)
            { fired = true; (after ? h.storage->after[0] : h.storage->before[0])[900] ^= 1; }
        };
        CHECK(!h.inspect() && fired); h.rejected(); ++early_callback_controls;
    }
    const std::vector<Change> early{
        [](auto &h) { h.admission->report_.reads ^= 1; },
        [](auto &h) { h.admission->report_.original.reads ^= 1; },
        [](auto &h) { h.admission->report_.phase = Ext4ReclaimRecoveryPhase::unused; },
        [](auto &h) { h.admission->session_ = nullptr; },
    };
    for (const auto &change : early)
    {
        Harness h(scenario.mixture(0)); bool fired{};
        h.disk.hook = [&] { if (!fired) { fired = true; change(h); } };
        CHECK(!h.inspect() && fired && h.disk.reads == 1);
        h.rejected(); CHECK(!h.inspect()); ++early_callback_controls;
    }

}
void hostile(const Scenario &scenario, usize reads)
{
    const u32 journal = u32(scenario.old_binding.layout.journal_first_block);
    const std::array<usize, 7> offsets{1024 + 900, 2 * 1024 + 30,
        usize{journal} * 1024 + 252, (usize{journal} + 1) * 1024 + 128,
        (usize{journal} + 1) * 1024 + 1020, (usize{journal} + 2) * 1024 + 900,
        (usize{journal} + 2 + scenario.journal.count) * 1024 + 16};
    for (usize offset : offsets)
    {
        auto image = scenario.mixture(0); image[offset] ^= 1;
        Harness h(image); CHECK(!h.inspect()); h.rejected(); ++hostile_rejects;
    }
    for (usize cut : {usize{1}, usize{9}, reads / 2, reads})
    {
        Harness h(scenario.mixture(0)); h.disk.fail_at = cut;
        CHECK(!h.inspect()); h.rejected(); ++read_faults;
    }
    for (usize home = 0; home < scenario.reclaim.reclaim.count; ++home)
    {
        const auto &before = scenario.reclaim.reclaim.before[home];
        const auto &after = scenario.reclaim.reclaim.after[home];
        usize cut{};
        while (cut < 1024 && before[cut] == after[cut]) ++cut;
        CHECK(cut < 1023); ++cut;
        Block torn = before;
        for (usize at = 0; at < cut; ++at) torn[at] = after[at];
        // A bitmap may differ in one byte only, so a prefix cut is already an
        // intact before/after image. Exercise foreign whole-home refusal there.
        if (torn == after) torn[900] ^= 1;
        CHECK(torn != before && torn != after);
        auto image = scenario.mixture(0); apply(image, scenario.reclaim.reclaim.targets[home], torn);
        Harness h(image); CHECK(!h.inspect()); h.rejected(); ++hostile_rejects;
    }
    {
        Harness h(scenario.mixture(0)); CHECK(h.inspect());
        h.disk.bytes[origin * 512 + usize{scenario.reclaim.reclaim.targets[0]} * 1024 + 900] ^= 1;
        CHECK(!h.admission->revalidate()); h.rejected(); ++hostile_rejects;
    }
    {
        Harness h(scenario.mixture(0), block::SessionAccess::readonly);
        CHECK(h.inspect() && h.admission->revalidate() && h.session.close()); ++accepted;
    }
    {
        Harness h(scenario.mixture(0)); Block bytes{};
        CHECK(h.session.read_blocks(2, std::as_writable_bytes(std::span{bytes})));
        CHECK(h.inspect() && h.admission->report().original.reads == 1 && h.admission->revalidate());
        CHECK(h.session.close()); ++accepted;
    }
}
} // namespace
int main(int argc, char **argv)
{
    CHECK(argc == 3);
    Scenario filled(read_image(argv[1]), true); auto reads = mixtures(filled); hostile(filled, reads); retained_drift(filled);
    for (usize index = 0; index < filled.reclaim.reclaim.count; ++index)
        apply(filled.before, filled.reclaim.reclaim.targets[index], filled.reclaim.reclaim.after[index]);
    auto head = filled.journal.journal_head;
    const u32 sequence = filled.journal.sequence + 2;
    for (usize at = 0; at < 4; ++at) head[24 + at] = u8(sequence >> (24 - at * 8));
    for (usize at = 28; at < 32; ++at) head[at] = 0;
    for (usize at = 252; at < 256; ++at) head[at] = 0;
    const u32 crc = jbd2::detail::crc32c(~u32{0}, head);
    for (usize at = 0; at < 4; ++at) head[252 + at] = u8(crc >> (24 - at * 8));
    apply(filled.before, u32(filled.old_binding.layout.journal_first_block), head);
    filled.old_binding.journal_sequence = sequence; filled.prepare(14);
    reads = mixtures(filled); hostile(filled, reads);
    Scenario empty(read_image(argv[2]), false); reads = mixtures(empty); hostile(empty, reads);
    std::printf("OK_RECLAIM_RETAINED controls=%zu final_callbacks=%zu outer_callbacks=%zu early_callbacks=%zu zero_write_flush=pass\n", retained_controls, final_callback_controls, outer_callback_controls, early_callback_controls);
    std::printf("PASS fresh read-only reclaim binding: accepted=%zu barrier_rejects=%zu hostile_rejects=%zu read_faults=%zu unlocked_callbacks=%zu external_write_flush=0\n",
        accepted, barrier_rejects, hostile_rejects, read_faults, callbacks);
}
