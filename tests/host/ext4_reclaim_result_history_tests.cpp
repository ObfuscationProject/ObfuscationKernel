// Hosted prospective-history checks against an admitted, read-only reclaim binding.
// This is a model of Session/device behavior, not hardware or guest execution.
#include "snapshot_fixture.hpp"
#include <functional>
#include <memory>
#include <type_traits>
#define private public
#include "ok/fs/ext4_reclaim_history.hpp"
#include "ok/fs/ext4_orphan_chain_capture.hpp"
#undef private

namespace
{
using Registry = block::BlockSessions<2>;
using Admission = Ext4ReclaimRecoveryBinding<2>;
using History = Ext4ReclaimResultHistory<2>;
using Capture = Ext4OrphanChainCapture<2>;
constexpr u64 origin = 37;
static_assert(!std::is_copy_constructible_v<History> && !std::is_move_constructible_v<History>);

void apply(Image &image, u32 physical, const Block &bytes)
{
    for (usize at = 0; at < bytes.size(); ++at)
        image[usize{physical} * bytes.size() + at] = bytes[at];
}

// The same committed-journal and directory-removal construction used by the
// original read-only recovery-binding test. A second prepare() advances the
// populated image to its final zero-next victim.
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
        for (usize at = 0; at < fixture.count; ++at)
            apply(before, fixture.images[at].table_block, fixture.images[at].table);
        for (const auto &directory : directories.blocks)
            apply(before, directory.physical, directory.bytes);
        prepare(populated ? 12 : 14);
    }

    void prepare(u32 inode)
    {
        Fixture fixture(before, old_binding);
        if (inode == 12) { fixture.add(before, 12, 14); fixture.add(before, 14, 0); }
        else fixture.add(before, 14, 0);
        for (usize at = 0; at < fixture.count; ++at)
            fixture.images[at].table = load(before, fixture.images[at].table_block);
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
            if (mask & (1U << index))
                apply(image, reclaim.reclaim.targets[index], reclaim.reclaim.after[index]);
        const u32 first = u32(old_binding.layout.journal_first_block);
        apply(image, first, journal.journal_head); apply(image, first + 1, journal.descriptor);
        for (usize index = 0; index < journal.count; ++index)
            apply(image, first + 2 + u32(index), journal.payloads[index]);
        apply(image, first + 2 + u32(journal.count), journal.commit);
        apply(image, first + 3 + u32(journal.count), journal.end);
        return image;
    }

    void advance_to_final_victim()
    {
        CHECK(reclaim.reclaim.inode == 12 && reclaim.reclaim.next_head == 14);
        for (usize index = 0; index < reclaim.reclaim.count; ++index)
            apply(before, reclaim.reclaim.targets[index], reclaim.reclaim.after[index]);
        auto head = journal.journal_head;
        const u32 sequence = journal.sequence + 2;
        for (usize at = 0; at < 4; ++at) head[24 + at] = u8(sequence >> (24 - at * 8));
        for (usize at = 28; at < 32; ++at) head[at] = 0;
        for (usize at = 252; at < 256; ++at) head[at] = 0;
        const u32 crc = jbd2::detail::crc32c(~u32{0}, head);
        for (usize at = 0; at < 4; ++at) head[252 + at] = u8(crc >> (24 - at * 8));
        apply(before, u32(old_binding.layout.journal_first_block), head);
        old_binding.journal_sequence = sequence;
        prepare(14);
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
        if (reads == fail_at) return Status::io_error("prospective injected transport read failure");
        if (first < origin || first >= bytes.size() / 512 || out.size() > bytes.size() - first * 512)
            return Status::io_error("prospective physical read bounds");
        for (usize at = 0; at < out.size(); ++at) out[at] = std::byte(bytes[first * 512 + at]);
        return {};
    }
    Status write_blocks(u64, std::span<const std::byte>) override
    { ++writes; return Status::denied("prospective test media WRITE forbidden"); }
    Status flush() override
    { ++flushes; return Status::denied("prospective test media FLUSH forbidden"); }
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
    bool domain_ok{true};

    explicit Harness(const Image &image) : disk(image)
    {
        const auto registered = registry.register_device(disk, 994);
        CHECK(registered); key = registered.value();
        CHECK(registry.acquire(key, {origin, 65536}, block::SessionAccess::writable, session));
    }
    static Status guard(const void *context)
    {
        return static_cast<const Harness *>(context)->domain_ok ? Status{} :
            Status::fault("prospective test opening domain changed");
    }
    void quarantined() const
    {
        CHECK(registry.inspect(key).value().state == block::DeviceState::quarantined);
        CHECK(disk.writes == 0 && disk.flushes == 0);
    }
};

struct Projection
{
    std::unique_ptr<History> history = std::make_unique<History>();
    std::unique_ptr<Capture> capture = std::make_unique<Capture>();
    std::unique_ptr<Ext4OrphanChainCaptureStorage> storage =
        std::make_unique<Ext4OrphanChainCaptureStorage>();
    std::unique_ptr<Ext4OrphanDirectoryQualifiedReclaimScratch> scratch =
        std::make_unique<Ext4OrphanDirectoryQualifiedReclaimScratch>();
    std::unique_ptr<Ext4NamespaceSnapshotSeal> seal = std::make_unique<Ext4NamespaceSnapshotSeal>();
    Ext4ReclaimRecoveryReport binding_report{};
    Ext4OrphanReclaimCaptureReport capture_report{};

    void prepare(Harness &h)
    {
        CHECK(h.admission->inspect(h.session, *h.storage, *h.scratch));
        CHECK(h.admission->proof() && h.admission->report().valid);
        binding_report = h.admission->report(); capture_report = h.admission->capture_.report_;
        CHECK(history->prepare(*h.admission,
            {&h, h.storage.get(), h.scratch.get(), &h.disk, 1, 1, Harness::guard, &h}));
        CHECK(history->stable());
    }
    void reports_unchanged(const Harness &h) const
    {
        CHECK(Admission::same_report(h.admission->report(), binding_report));
        CHECK(Ext4OrphanReclaimCapture<2>::same_report(h.admission->capture_.report_, capture_report));
    }
    void prove(Harness &h)
    {
        const auto proof = *h.admission->proof();
        if (!proof.next_head)
        {
            Ext4RecoveryBinding ordinary;
            CHECK(history->prove_ordinary(ordinary));
            CHECK(ordinary.info() && ordinary.info()->empty_log);
            CHECK(ordinary.info()->journal_sequence == proof.cleanup_sequence);
        }
        else
        {
            CHECK(capture->capture_reclaim_result(h.session, *storage, *scratch, *history, *seal));
            CHECK(capture->report().valid && capture->report().prospective);
            CHECK(capture->report().head == proof.next_head);
            CHECK(history->validate_remaining(capture->binding_, capture->source()));
            CHECK(capture->namespace_storage_exact());
        }
        CHECK(history->stable());
        reports_unchanged(h);
        CHECK(h.disk.writes == 0 && h.disk.flushes == 0);
    }
};

usize accepted{}, noncontiguous{}, zero_head{}, nonzero_head{}, rejected{};

void all_legal_masks(const Scenario &scenario)
{
    const usize count = scenario.reclaim.reclaim.count;
    CHECK(count >= 4 && count <= 5);
    for (unsigned mask = 0; mask < (1U << count); ++mask)
    {
        if (!legal(count, mask)) continue;
        Harness h(scenario.mixture(mask)); Projection projection;
        projection.prepare(h);
        const auto proof = *h.admission->proof();
        CHECK(proof.count == count && proof.next_head == scenario.reclaim.reclaim.next_head);
        for (usize index = 0; index < count; ++index)
            CHECK(proof.home_after[index] == bool(mask & (1U << index)));
        projection.prove(h);
        if (mask == 5) ++noncontiguous;
        if (proof.next_head) ++nonzero_head; else ++zero_head;
        ++accepted;
    }
}

void reject_read(Harness &h, Projection &projection, u32 physical = 1)
{
    Block result{};
    CHECK(!projection.history->read_blocks(u64{physical} * 2,
        std::as_writable_bytes(std::span{result})));
    CHECK(projection.history->rejected_);
    h.quarantined(); ++rejected;
}

void drift_controls(const Scenario &zero, const Scenario &nonzero)
{
    using Change = std::function<void(Harness &, Projection &)>;
    const std::vector<Change> changes{
        [](auto &h, auto &) { h.storage->actual.tables[3][511][1023] ^= 1; },
        [](auto &h, auto &) { h.admission->report_.reads ^= 1; },
        [](auto &h, auto &) { h.admission->capture_.report_.verified_blocks ^= 1; },
        [](auto &, auto &p) { p.history->domain_.cache = p.storage.get(); },
        [](auto &h, auto &) { h.domain_ok = false; },
        [](auto &, auto &p) { p.history->progress_.reads ^= 1; },
        [](auto &, auto &p) { p.history->seal_.progress.completed_reads ^= 1; },
        [](auto &, auto &p) { p.history->initial_.reads ^= 1; },
        [](auto &, auto &p) { p.history->seal_.initial.reads ^= 1; },
        [](auto &, auto &p) { p.history->attempted_ = false; },
        [](auto &, auto &p) { p.history->ready_ = false; },
        [](auto &, auto &p) { p.history->seal_.ready = false; },
        [](auto &, auto &p) { p.history->reading_ = true; },
        [](auto &, auto &p) { p.history->members_[6].next ^= 1; },
        [](auto &, auto &p) { p.history->empty_head_[900] ^= 1; },
    };
    for (const auto &change : changes)
    {
        Harness h(zero.mixture(0)); Projection projection; projection.prepare(h);
        const auto reads = h.disk.reads;
        change(h, projection); reject_read(h, projection);
        CHECK(h.disk.reads == reads);
    }
    {
        Harness h(nonzero.mixture(0)); Projection projection; projection.prepare(h);
        projection.history->members_[0].generation ^= 1; reject_read(h, projection);
    }
    {
        Harness h(nonzero.mixture(0)); Projection projection; projection.prepare(h);
        projection.history->seal_.members[0].next ^= 1; reject_read(h, projection);
    }
    {
        Harness h(nonzero.mixture(0)); Projection projection; projection.prepare(h);
        CHECK(projection.capture->capture_reclaim_result(h.session, *projection.storage,
            *projection.scratch, *projection.history, *projection.seal));
        projection.storage->chain[0].expected_generation ^= 1;
        CHECK(!projection.history->validate_remaining(projection.capture->binding_,
            projection.capture->source()));
        CHECK(projection.history->rejected_); h.quarantined(); ++rejected;
    }
    {
        Harness h(nonzero.mixture(0)); Projection projection; projection.prepare(h);
        CHECK(projection.capture->capture_reclaim_result(h.session, *projection.storage,
            *projection.scratch, *projection.history, *projection.seal));
        projection.capture->binding_.journal_sequence ^= 1;
        CHECK(!projection.history->validate_remaining(projection.capture->binding_,
            projection.capture->source()));
        CHECK(projection.history->rejected_); h.quarantined(); ++rejected;
    }
}

void exact_callback_and_result_controls(const Scenario &scenario)
{
    {
        Harness h(scenario.mixture(0)); Projection p; p.prepare(h);
        usize callbacks{};
        h.disk.hook = [&] { CHECK(p.history->in_flight_exact()); ++callbacks; };
        p.prove(h); CHECK(callbacks == p.history->reads_);
    }
    {
        Harness h(scenario.mixture(0)); Projection p; p.prepare(h);
        bool fired{};
        h.disk.hook = [&] { if (!fired) { fired = true; ++p.capture->report_.table_blocks; } };
        CHECK(!p.capture->capture_reclaim_result(h.session, *p.storage, *p.scratch, *p.history, *p.seal));
        CHECK(fired); h.quarantined(); ++rejected;
    }
    {
        Harness h(scenario.mixture(0)); Projection p; p.prepare(h);
        CHECK(p.capture->capture_reclaim_result(h.session, *p.storage, *p.scratch, *p.history, *p.seal));
        bool fired{};
        h.disk.hook = [&] { if (!fired) { fired = true; p.storage->inode_tables[3][511][1023] ^= 1; } };
        CHECK(p.history->validate_remaining(p.capture->binding_, p.capture->source()));
        const auto result = p.capture->namespace_storage_exact(); CHECK(!result && fired);
        CHECK(!p.history->reject(result)); h.quarantined(); ++rejected;
    }
}

usize original_quarantine_controls{};
void original_quarantine(const Scenario &scenario)
{
    using Change = std::function<void(Harness &, Projection &, Harness &, Projection &)>;
    const std::vector<Change> history_changes{
        [](auto &, auto &p, auto &decoy, auto &) { p.history->seal_.session = &decoy.session; },
        [](auto &, auto &p, auto &decoy, auto &) { p.history->original_session_ = &decoy.session; },
        [](auto &, auto &p, auto &decoy, auto &) { p.history->session_anchor_.first_ = &decoy.session; },
        [](auto &, auto &p, auto &decoy, auto &) { p.history->session_anchor_.second_ = &decoy.session; },
        [](auto &, auto &p, auto &decoy, auto &) { p.history->session_anchor_.third_ = &decoy.session; },
        [](auto &h, auto &, auto &decoy, auto &) { h.admission->original_session_ = &decoy.session; },
        [](auto &h, auto &, auto &decoy, auto &) { h.admission->session_ = &decoy.session; },
        [](auto &h, auto &, auto &decoy, auto &) { h.admission->retained_.session = &decoy.session; },
        [](auto &h, auto &, auto &decoy, auto &) { h.admission->session_anchor_.first_ = &decoy.session; },
        [](auto &h, auto &, auto &decoy, auto &) { h.admission->session_anchor_.second_ = &decoy.session; },
        [](auto &h, auto &, auto &decoy, auto &) { h.admission->session_anchor_.third_ = &decoy.session; },
        [](auto &h, auto &, auto &decoy, auto &) { h.admission->capture_.original_session_ = &decoy.session; },
        [](auto &h, auto &, auto &decoy, auto &) { h.admission->capture_.session_ = &decoy.session; },
        [](auto &h, auto &, auto &decoy, auto &) { h.admission->capture_.retained_.session = &decoy.session; },
        [](auto &h, auto &, auto &decoy, auto &) { h.admission->capture_.session_anchor_.first_ = &decoy.session; },
        [](auto &h, auto &, auto &decoy, auto &) { h.admission->capture_.session_anchor_.second_ = &decoy.session; },
        [](auto &h, auto &, auto &decoy, auto &) { h.admission->capture_.session_anchor_.third_ = &decoy.session; },
    };
    const auto run = [&](const Change &change, bool nested) {
        Harness h(scenario.mixture(0)), decoy(scenario.mixture(0)); Projection p, alternate;
        p.prepare(h); alternate.prepare(decoy); bool fired{};
        h.disk.hook = [&] { if (!fired) { fired = true; change(h,p,decoy,alternate); } };
        if (nested)
            CHECK(!p.capture->capture_reclaim_result(h.session,*p.storage,*p.scratch,*p.history,*p.seal));
        else
        { Block result{}; CHECK(!p.history->read_blocks(2,std::as_writable_bytes(std::span{result}))); }
        CHECK(fired); h.quarantined();
        CHECK(decoy.registry.inspect(decoy.key).value().state == block::DeviceState::owned);
        CHECK(decoy.session.close()); ++original_quarantine_controls;
    };
    for (const auto &change : history_changes) run(change,false);
    const std::vector<Change> nested_changes{
        [](auto &, auto &p, auto &, auto &alternate) { p.capture->reclaim_origin_ = alternate.history.get(); },
        [](auto &, auto &p, auto &, auto &alternate) { p.capture->reclaim_history_ = alternate.history.get(); },
        [](auto &, auto &p, auto &decoy, auto &) { p.capture->session_ = &decoy.session; },
        [](auto &, auto &p, auto &decoy, auto &) { p.capture->reclaim_session_anchor_.first_ = &decoy.session; },
        [](auto &, auto &p, auto &decoy, auto &) { p.capture->reclaim_session_anchor_.second_ = &decoy.session; },
        [](auto &, auto &p, auto &decoy, auto &) { p.capture->reclaim_session_anchor_.third_ = &decoy.session; },
    };
    for (const auto &change : nested_changes) run(change,true);
    {
        Harness h(scenario.mixture(0)), decoy(scenario.mixture(0)); Projection p;
        p.prepare(h); h.admission->original_session_ = &decoy.session;
        CHECK(!h.admission->revalidate()); h.quarantined();
        CHECK(decoy.registry.inspect(decoy.key).value().state == block::DeviceState::owned);
        CHECK(decoy.session.close()); ++original_quarantine_controls;
    }
    {
        Harness h(scenario.mixture(0)), decoy(scenario.mixture(0)); Projection p;
        p.prepare(h); h.admission->capture_.original_session_ = &decoy.session;
        CHECK(!h.admission->capture_.revalidate()); h.quarantined();
        CHECK(decoy.registry.inspect(decoy.key).value().state == block::DeviceState::owned);
        CHECK(decoy.session.close()); ++original_quarantine_controls;
    }
}

void late_controls(const Scenario &scenario)
{
    using Change = std::function<void(Harness &, Projection &)>;
    const std::vector<Change> changes{
        [](auto &h, auto &) { h.storage->actual.tables[3][511][1023] ^= 1; },
        [](auto &h, auto &) { h.admission->report_.reads ^= 1; },
        [](auto &, auto &p) { p.history->domain_.workspace = p.scratch.get(); },
        [](auto &, auto &p) { p.history->progress_.reads ^= 1; },
        [](auto &, auto &p) { p.history->empty_head_[900] ^= 1; },
        [](auto &, auto &p) { p.history->reads_ ^= 1; },
    };
    for (const auto &change : changes)
    {
        Harness h(scenario.mixture(0)); Projection projection; projection.prepare(h);
        bool fired{};
        h.disk.hook = [&] { if (!fired) { fired = true; change(h, projection); } };
        reject_read(h, projection);
        CHECK(fired);
    }
    {
        Harness h(scenario.mixture(0)); Projection projection; projection.prepare(h);
        h.disk.fail_at = h.disk.reads + 1;
        reject_read(h, projection);
    }
    {
        Harness h(scenario.mixture(0)); Projection projection; projection.prepare(h);
        h.disk.bytes[origin * 512 + 1024 + 900] ^= 1;
        reject_read(h, projection);
    }
    {
        Harness h(scenario.mixture(0)); Projection projection; projection.prepare(h);
        reject_read(h, projection, 0); // No retained identity for block zero.
    }
}
} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    Scenario filled(read_image(argv[1]), true);
    all_legal_masks(filled);
    Scenario final_victim(read_image(argv[1]), true);
    final_victim.advance_to_final_victim();
    all_legal_masks(final_victim);
    Scenario empty(read_image(argv[2]), false);
    all_legal_masks(empty);
    drift_controls(empty, filled);
    late_controls(filled);
    exact_callback_and_result_controls(filled);
    original_quarantine(filled);
    CHECK(noncontiguous >= 3 && zero_head && nonzero_head);
    std::printf("OK_ORIGINAL_SESSION_QUARANTINE controls=%zu original=quarantined decoy=owned writes=0 flushes=0\n", original_quarantine_controls);
    std::printf("PASS prospective reclaim history: accepted=%zu zero_head=%zu nonzero_head=%zu noncontiguous=%zu rejected=%zu writes=0 flushes=0\n",
        accepted, zero_head, nonzero_head, noncontiguous, rejected);
}
