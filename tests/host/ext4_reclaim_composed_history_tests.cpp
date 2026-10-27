// Hosted composed-history checks against an admitted, read-only reclaim binding.
// This is a model of Session/device behavior, not hardware or guest execution.
#include "snapshot_fixture.hpp"
#include <functional>
#include <memory>
#include <type_traits>
// Negative controls reach private history state; build with -fno-access-control.
#include "ok/fs/ext4/alloc.hpp"
#include "ok/fs/ext4/recovery.hpp"

namespace
{
using Registry = block::BlockSessions<2>;
using Admission = Ext4ReclaimRecoveryBinding<2>;
using History = Ext4ReclaimResultHistory<2>;
using Capture = Ext4OrphanChainCapture<2>;
constexpr u64 origin = 37;
static_assert(!std::is_copy_constructible_v<History> && !std::is_move_constructible_v<History>);
static_assert(!std::is_copy_constructible_v<Ext4ReclaimComposedReadBudget> &&
              !std::is_move_constructible_v<Ext4ReclaimComposedReadBudget>);

void reservation_boundaries()
{
    block::SessionReport initial;
    initial.state = block::DeviceState::owned;
    // Independently tabulated complete dry bounds for zero, one and seven members.
    const std::array<std::pair<usize, usize>, 3> rows{{{0, 10165610}, {1, 10172591}, {7, 10214477}}};
    usize checked{};
    for (const auto &[remaining, without_extension] : rows)
        for (bool extension : {false, true})
        {
            const usize expected = without_extension + (extension ? 1 : 0);
            const auto capacity = Ext4ReclaimComposedReadBudget::capacity(initial, remaining, extension, 17);
            CHECK(capacity && capacity.value().aggregate == expected);
            CHECK(capacity.value().per_pass == (std::array<usize, 4>{4654, 2327, 10163275, 2335}));
            auto exact = initial; exact.reads = exact.completed_reads = ~usize{0} - expected;
            CHECK(Ext4ReclaimComposedReadBudget::capacity(exact, remaining, extension, 17));
            ++exact.reads; ++exact.completed_reads;
            CHECK(Ext4ReclaimComposedReadBudget::capacity(exact, remaining, extension, 17).status().code() == StatusCode::overflow);
            const u32 advance = remaining == 0 ? 2 : remaining == 1 ? 5 : 23;
            const auto sequence_fit = Ext4ReclaimComposedReadBudget::capacity(initial, remaining, extension, ~u32{0} - advance);
            CHECK(sequence_fit && sequence_fit.value().projected_sequence_end == ~u32{0});
            CHECK(Ext4ReclaimComposedReadBudget::capacity(initial, remaining, extension, ~u32{0} - advance + 1).status().code() == StatusCode::overflow);
            ++checked;
        }
    CHECK(Ext4ReclaimComposedReadBudget::capacity(initial, 8, true, 17).status().code() == StatusCode::unsupported);
    CHECK(Ext4ReclaimComposedReadBudget::capacity(initial, ~usize{0}, true, 17).status().code() == StatusCode::unsupported);
    auto incomplete = initial; incomplete.reads = 1;
    CHECK(Ext4ReclaimComposedReadBudget::capacity(incomplete, 7, true, 17).status().code() == StatusCode::fault);
    incomplete = initial; incomplete.completed_reads = 1;
    CHECK(Ext4ReclaimComposedReadBudget::capacity(incomplete, 7, true, 17).status().code() == StatusCode::fault);
    incomplete = initial; incomplete.in_flight = true;
    CHECK(Ext4ReclaimComposedReadBudget::capacity(incomplete, 7, true, 17).status().code() == StatusCode::fault);
    incomplete = initial; incomplete.pending_writes = true;
    CHECK(Ext4ReclaimComposedReadBudget::capacity(incomplete, 7, true, 17).status().code() == StatusCode::fault);
    incomplete = initial; incomplete.writes = 1;
    CHECK(Ext4ReclaimComposedReadBudget::capacity(incomplete, 7, true, 17).status().code() == StatusCode::fault);
    incomplete = initial; incomplete.flushes = 1;
    CHECK(Ext4ReclaimComposedReadBudget::capacity(incomplete, 7, true, 17).status().code() == StatusCode::fault);
    // Already completed namespace mutations are retained; the dry route adds none.
    incomplete = initial;
    incomplete.writes = incomplete.completed_writes = ~usize{0};
    incomplete.flushes = incomplete.completed_flushes = ~usize{0};
    CHECK(Ext4ReclaimComposedReadBudget::capacity(incomplete, 7, true, 17));
    Ext4ReclaimComposedReadBudget budget;
    CHECK(budget.reserve(initial, 7, true, 17));
    CHECK(!budget.reserve(initial, 7, true, 17));
    CHECK(budget.exact(initial, 7, true, 17));
    CHECK(!budget.exact(initial, 6, true, 17));
    CHECK(budget.charges(1, {0, 0, 0, 0}, 1));
    CHECK(!budget.charges(2, {0, 0, 0, 0}, 2));
    CHECK(!budget.charges(1, {32579, 0, 0, 0}, 32580));
    CHECK(!budget.charges(1, {~usize{0}, 0, 0, 0}, 0));
    Ext4ReclaimComposedReadBudget refused;
    CHECK(!refused.reserve(initial, 8, true, 17));
    CHECK(!refused.reserve(initial, 7, true, 17));
    std::printf("PASS composed readonly reservations: exact_counter_boundaries=%zu sequence_boundaries=%zu preexisting_completed_mutations_retained=1 write_authority=0\n", checked, checked);
}

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

using Route = Ext4ReclaimComposedHistory<2>;
struct Dry
{
    std::unique_ptr<Route> route = std::make_unique<Route>();
    std::unique_ptr<Ext4ReclaimComposedStorage> storage = std::make_unique<Ext4ReclaimComposedStorage>();
    void prepare(Projection &projection) { CHECK(route->prepare(*projection.history, *storage)); }
};
static_assert(!std::is_copy_constructible_v<Route> && !std::is_move_constructible_v<Ext4ReclaimComposedStorage>);

struct ActiveFixture
{
    Image before;
    Ext4RecoveryInfo binding{};
    std::unique_ptr<Ext4OrphanDirectoryQualifiedReclaimScratch> scratch =
        std::make_unique<Ext4OrphanDirectoryQualifiedReclaimScratch>();
    Ext4OrphanDirectoryQualifiedReclaimPlan plan{};
    Ext4OrphanReclaimJournalScratch formatter{};
    Ext4OrphanReclaimJournalPlan wal{};
    usize members{};
    explicit ActiveFixture(Image image, bool revoke = false) : before(std::move(image))
    {
        Image normalized = before;
        auto super = load(normalized, 1); store32(super, 232, 0);
        store32(super, 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{super}.first(1020)));
        apply(normalized, 1, super);
        MemoryDisk disk(normalized); Ext4RecoveryBinding ordinary;
        CHECK(ordinary.inspect(disk, {0, normalized.size() / 512}) && ordinary.info());
        binding = *ordinary.info();
        if (revoke)
        {
            auto head = load(before, u32(binding.layout.journal_first_block));
            const auto big = [](Block &b, usize at, u32 value) {
                for (usize i = 0; i < 4; ++i) b[at+i] = u8(value >> (24-8*i));
            };
            big(head,40,jbd2::feature_revoke | jbd2::feature_checksum_v3); big(head,252,0);
            big(head,252,jbd2::detail::crc32c(~u32{0},head)); apply(before,u32(binding.layout.journal_first_block),head);
        }
        Fixture fixture(before,binding); u32 number = ext4_checked::le32(fixture.super,232);
        while (number)
        {
            CHECK(fixture.count < 8); auto &entry = fixture.images[fixture.count++];
            entry.number=number;entry.table_block=fixture.table_for(number);entry.table=load(before,entry.table_block);
            entry.expected_generation=ext4_checked::le32(entry.table,slot(number)+100);
            number=ext4_checked::le32(entry.table,slot(number)+20);
        }
        members=fixture.count;CHECK(members);
        Tables tables(before,fixture);DirectoryImages directories(before,fixture,tables);
        CHECK(Ext4OrphanDirectoryQualifiedReclaimPlanner::plan(binding,{tables.source(fixture),directories.blocks},
            *scratch,fixture.images[0].number,fixture.images[0].expected_generation,plan));
        const auto inode=std::span<const u8>{fixture.images[0].table}.subspan(slot(fixture.images[0].number),256);
        CHECK(Ext4OrphanReclaimJournalPlanner::plan(binding,plan.reclaim,load(before,u32(binding.layout.journal_first_block)),
            ext4_checked::le32(inode,12),ext4_checked::le32(inode,132)>>2,formatter,wal));
    }
    Image mixture(unsigned mask) const
    {
        Image image=before;
        for(usize at=0;at<plan.reclaim.count;++at)
            if(mask&(1U<<at))apply(image,plan.reclaim.targets[at],plan.reclaim.after[at]);
        const u32 first=u32(binding.layout.journal_first_block);
        apply(image,first,wal.journal_head);apply(image,first+1,wal.descriptor);
        for(usize at=0;at<wal.count;++at)apply(image,first+2+u32(at),wal.payloads[at]);
        apply(image,first+2+u32(wal.count),wal.commit);apply(image,first+3+u32(wal.count),wal.end);
        return image;
    }
};
usize completed{}, rejected{}, callbacks{}, goldens{}, original_controls{};
void accepted(const ActiveFixture &fixture,unsigned mask,const Image &golden)
{
    auto mixed=fixture.mixture(mask); Harness h(mixed);Projection p;p.prepare(h);p.prove(h);Dry d;d.prepare(p);
    bool stale_tail_observed{};
    h.disk.hook=[&]{
        CHECK(d.route->in_flight_exact());++callbacks;
        if(fixture.members==8 && d.route->index_==2)
        {
            CHECK(d.storage->rows_[1].wal.count==5 && d.storage->rows_[2].wal.count==4);
            const Block *tail{};
            CHECK(d.route->resolve(u32(fixture.binding.layout.journal_first_block)+8,tail));
            CHECK(tail==&d.storage->rows_[1].wal.end && *tail==d.storage->sealed_rows_[1].wal.end);
            stale_tail_observed=true;
        }
    };
    const auto dry_status=d.route->run();
    if(!dry_status)std::fprintf(stderr,"dry status=%u message=%.*s index=%zu reads=%zu phase=%u\n",unsigned(dry_status.code()),
        int(dry_status.message().size()),dry_status.message().data(),d.route->index_,d.route->reads_,unsigned(d.route->completion_.report().phase));
    CHECK(dry_status);CHECK(d.route->complete_ && d.route->index_==fixture.members-1);
    CHECK(d.route->row_count_==fixture.members && d.route->last_enumeration_<=2335);
    CHECK(d.route->completion_.report().checked_inodes && d.route->completion_.report().empty_log);
    CHECK(d.route->charged_[0]<=2*2327*(fixture.members-1) && d.route->charged_[1]<=2327*(fixture.members-1));
    CHECK(d.route->charged_[2]<=10163275 && d.route->charged_[3]<=2335);
    usize aggregate=d.route->extension_reads_;for(usize charge:d.route->charged_)aggregate+=charge;
    CHECK(aggregate==d.route->reads_ && h.session.report().value().reads==d.route->progress_.reads);
    CHECK(d.route->budget_.exact(d.route->initial_, fixture.members - 1, fixture.plan.reclaim.count == 4,
          fixture.wal.sequence));
    CHECK(d.route->budget_.charges(d.route->extension_reads_, d.route->charged_, d.route->reads_));
    CHECK(d.route->reads_ <= d.route->budget_.capacity_.aggregate);
    if(fixture.members==8)
    {const std::array<usize,8> widths{4,5,4,4,4,4,4,5};
     for(usize at=0;at<widths.size();++at)CHECK(d.storage->rows_[at].wal.count==widths[at]);
     CHECK(stale_tail_observed);}
    if(fixture.members>1)
    {
        Ext4OrphanDirectoryQualifiedReclaimPlan denied{};const usize before=h.disk.reads;
        CHECK(d.route->capture_.plan_head(d.route->capture_.report().head,d.route->capture_.report().generation,denied).code()==StatusCode::denied);
        CHECK(!denied.valid && h.disk.reads==before && d.route->stable());
    }
    p.reports_unchanged(h);CHECK(h.disk.writes==0 && h.disk.flushes==0);
    // The full golden was produced by the earlier independent physical C1 campaign.
    Image terminal=mixed;
    CHECK(p.history->each_original([&](u32 physical,const Block&)->Status {
        const Block *expected{};CHECK(d.route->resolve(physical,expected));apply(terminal,physical,*expected);return {};
    }));
    if(d.route->extension_)
    {const Block *expected{};const u32 physical=u32(fixture.binding.layout.journal_first_block)+8;
     CHECK(d.route->resolve(physical,expected));apply(terminal,physical,*expected);}
    if(terminal!=golden)
    {usize at{};while(at<terminal.size() && terminal[at]==golden[at])++at;
     std::fprintf(stderr,"full independent golden differs offset=%zu actual=%u expected=%u\n",at,terminal[at],golden[at]);}
    CHECK(terminal==golden);++goldens;++completed;
    std::printf("CASE composed remaining=%zu mask=%u reads=%zu extension=%zu capture=%zu plan=%zu ordinary=%zu terminal=%zu\n",
        fixture.members-1,mask,d.route->reads_,d.route->extension_reads_,d.route->charged_[0],d.route->charged_[1],
        d.route->charged_[2],d.route->charged_[3]);std::fflush(stdout);
    CHECK(!d.route->run());
}
void negative(const ActiveFixture &fixture)
{
    using Change=std::function<void(Harness&,Projection&,Dry&)>;
    const std::vector<Change> changes{
        [](auto&,auto&,auto&d){d.storage->rows_[7].next^=1;},
        [](auto&,auto&,auto&d){d.route->seal_.rows^=1;},
        [](auto&,auto&,auto&d){d.route->members_[6].generation^=1;},
        [](auto&,auto&,auto&d){d.route->seal_.progress.completed_reads^=1;},
        [](auto&,auto&,auto&d){++d.route->charged_[1];++d.route->seal_.charged[1];},
        [](auto&,auto&,auto&d){++d.route->budget_.capacity_.aggregate;},
        [](auto&,auto&,auto&d){++d.route->budget_.sealed_.per_pass[0];},
        [](auto&,auto&,auto&d){--d.route->budget_.capacity_.aggregate;--d.route->budget_.sealed_.aggregate;},
        [](auto&,auto&,auto&d){++d.route->budget_.capacity_.cumulative[1];++d.route->budget_.sealed_.cumulative[1];},
        [](auto&,auto&,auto&d){d.storage->sealed_extension_[99]^=1;},
        [](auto&h,auto&,auto&){h.disk.bytes[origin*512+1024+900]^=1;},
        [](auto&h,auto&p,auto&){h.disk.bytes[origin*512+(p.history->binding_->journal_first_+8)*1024+900]^=1;},
    };
    for(const auto &change:changes)
    {
        Harness h(fixture.mixture(0));Projection p;p.prepare(h);Dry d;d.prepare(p);change(h,p,d);
        const usize before=h.disk.reads;
        CHECK(!d.route->run());
        if (!d.route->budget_.exact(d.route->initial_,fixture.members-1,fixture.plan.reclaim.count==4,fixture.wal.sequence))
            CHECK(h.disk.reads==before);
        h.quarantined();++rejected;
    }
    {
        Harness h(fixture.mixture(0));Projection p;p.prepare(h);Dry d;d.prepare(p);
        d.route->begin_phase(decltype(d.route->phase_)::terminal);Block out{};
        CHECK(!d.route->read_blocks(0,std::as_writable_bytes(std::span{out})));h.quarantined();++rejected;
    }
    {
        Harness h(fixture.mixture(0));Projection p;p.prepare(h);Dry d;d.prepare(p);
        d.route->begin_phase(decltype(d.route->phase_)::terminal);
        d.route->phase_reads_=d.route->seal_.phase_reads=2335;Block out{};
        const usize before=h.disk.reads;
        CHECK(!d.route->read_blocks(2,std::as_writable_bytes(std::span{out})) && h.disk.reads==before);
        h.quarantined();++rejected;
    }
    const std::vector<Change> callbacks_changes{
        [](auto&,auto&,auto&d){++d.route->index_;},
        [](auto&,auto&,auto&d){d.storage->rows_[0].cleanup[99]^=1;},
        [](auto&,auto&,auto&d){++d.route->capture_.report_.table_blocks;},
        [](auto&,auto&,auto&d){d.route->capture_.storage_=nullptr;},
        [](auto&,auto&,auto&d){++d.route->budget_.sealed_.projected_sequence_end;},
        [](auto&,auto&,auto&d){++d.route->budget_.capacity_.aggregate;++d.route->budget_.sealed_.aggregate;},
    };
    for(const auto &change:callbacks_changes)
    {
        Harness h(fixture.mixture(0));Projection p;p.prepare(h);Dry d;d.prepare(p);bool fired{};
        h.disk.hook=[&]{if(!fired){fired=true;change(h,p,d);}};
        CHECK(!d.route->run() && fired);h.quarantined();++rejected;
    }
    for(bool terminal:{false,true}) for(bool failed_phase:{false,true})
    {
        Harness h(fixture.mixture(0));Projection p;p.prepare(h);Dry d;d.prepare(p);bool fired{};
        h.disk.hook=[&]{if(!fired && (terminal ? d.route->endpoint_ : d.route->ordinary_))
            {fired=true;if(failed_phase)d.route->completion_.report_.failed_phase=Ext4CompletionPhase::preflight;
             else ++d.route->completion_.report_.checked_inodes;}};
        CHECK(!d.route->run() && fired);h.quarantined();++rejected;
    }
    using Substitute=std::function<void(Projection&,Dry&,Harness&,Dry&)>;
    const std::vector<Substitute> substitutions{
        [](auto&,auto&d,auto&decoy,auto&){d.route->original_session_=&decoy.session;},
        [](auto&,auto&d,auto&decoy,auto&){d.route->seal_.session=&decoy.session;},
        [](auto&,auto&d,auto&decoy,auto&){d.route->session_anchor_.first_=&decoy.session;},
        [](auto&,auto&d,auto&decoy,auto&){d.route->session_anchor_.second_=&decoy.session;},
        [](auto&,auto&d,auto&decoy,auto&){d.route->session_anchor_.third_=&decoy.session;},
        [](auto&p,auto&,auto&decoy,auto&){p.history->original_session_=&decoy.session;},
        [](auto&,auto&d,auto&decoy,auto&){d.route->capture_.session_=&decoy.session;},
        [](auto&,auto&d,auto&,auto&other){d.route->capture_.composed_origin_=other.route.get();},
        [](auto&,auto&d,auto&,auto&other){d.route->capture_.composed_history_=other.route.get();},
    };
    for(const auto &change:substitutions)
    {
        Harness h(fixture.mixture(0)),decoy(fixture.mixture(0));Projection p,q;p.prepare(h);q.prepare(decoy);Dry d,e;d.prepare(p);e.prepare(q);bool fired{};
        h.disk.hook=[&]{if(!fired){fired=true;change(p,d,decoy,e);}};
        CHECK(!d.route->run() && fired);h.quarantined();CHECK(decoy.registry.inspect(decoy.key).value().state==block::DeviceState::owned);
        CHECK(decoy.session.close());++original_controls;
    }
    {
        Harness h(fixture.mixture(0));Projection p;p.prepare(h);Dry d;
        bool fired{};h.disk.hook=[&]{if(!fired){fired=true;d.route->seal_.index^=1;}};
        CHECK(!d.route->prepare(*p.history,*d.storage) && fired);h.quarantined();++rejected;
    }
}
} // namespace
int main(int argc,char **argv)
{
    CHECK(argc==2);const std::string root=argv[1];
    reservation_boundaries();
    const auto image=[&](const char *name){return read_image((root+"/"+name).c_str());};
    ActiveFixture zero(image("input-empty.img")),one(image("input-chain2-cross-group.img")),seven(image("input-chain8-cross-group.img"));
    CHECK(zero.members==1 && one.members==2 && seven.members==8);
    CHECK(seven.plan.reclaim.count==4);
    const Image zero_golden=image("finished-empty.img"),one_golden=image("finished-chain2.img"),seven_golden=image("finished-chain8.img");
    for(unsigned mask=0;mask<(1U<<zero.plan.reclaim.count);++mask)
        if(legal(zero.plan.reclaim.count,mask))accepted(zero,mask,zero_golden);
    for(unsigned mask=0;mask<(1U<<one.plan.reclaim.count);++mask)
        if(legal(one.plan.reclaim.count,mask))accepted(one,mask,one_golden);
    for(unsigned mask=0;mask<(1U<<seven.plan.reclaim.count);++mask)
        if(legal(seven.plan.reclaim.count,mask))accepted(seven,mask,seven_golden);
    negative(seven);
    {
        ActiveFixture unsupported(image("input-empty.img"),true);Harness h(unsupported.mixture(0));Projection p;p.prepare(h);p.prove(h);Dry d;d.prepare(p);
        CHECK(!d.route->run());CHECK(d.route->completion_.report().status.code()==StatusCode::unsupported);h.quarantined();++rejected;
    }
    std::printf("PASS composed readonly dry route: completed=%zu zero_one_seven=1 independent_full_goldens=%zu rejected=%zu original_controls=%zu callbacks=%zu writes=0 flushes=0\n",
        completed,goldens,rejected,original_controls,callbacks);
}
