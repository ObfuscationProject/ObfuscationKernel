#include "ext4_namespace_journal_fixture.hpp"
#include "ext4_owned_support.hpp"
#include <memory>

using namespace namespace_journal_model;
using Owner = Ext4OwnedTransaction<1, 8>;
namespace
{
usize mixtures{}, live_cases{}, faults{}, rejected{}, all_after{}, read_faults{}, history_rejected{};
void put(Image &image, u32 number, const Block &block)
{ std::copy(block.begin(), block.end(), image.begin() + number * 1024); }
void dirty(Fixture &fixture)
{
    store32(fixture.super, 96, value(fixture.super, 96) | 4U);
    store32(fixture.super, 1020, jbd2::detail::crc32c(~u32{0},
        std::span<const u8>{fixture.super}.first(1020)));
}
Image materialize(const Fixture &fixture)
{
    Image image = fixture.disk;
    put(image, 1, fixture.super); put(image, 2, fixture.descriptors);
    for (usize group = 0; group < 4; ++group)
    {
        put(image, value(fixture.descriptors, group * 32), fixture.block_bitmaps[group]);
        put(image, value(fixture.descriptors, group * 32 + 4), fixture.inode_bitmaps[group]);
    }
    put(image, fixture.parent_block, fixture.parent); put(image, fixture.source_block, fixture.source);
    put(image, fixture.destination_block, fixture.destination); put(image, fixture.directory_block, fixture.directory);
    return image;
}
struct Transaction
{
    Image before;
    Ext4NamespaceJournalScratch scratch{};
    Ext4NamespaceJournalPlan journal{};
    Ext4NamespaceUpdate update{};
    u32 first{};
    Transaction(const Fixture &fixture, Ext4NamespaceUpdate request) : before(materialize(fixture)), update(request)
    {
        owned_fixture::Disk disk(before); Ext4RecoveryBinding binding;
        CHECK(binding.inspect(disk, {0, before.size() / 512}));
        first = u32(binding.info()->layout.journal_first_block);
        CHECK(Ext4NamespaceDurableJournalPlanner::plan(*binding.info(), request, fixture.images(),
            load(before, first), scratch, journal));
        CHECK(journal.ready && !journal.no_op);
    }
    Image mixed(u32 mask) const
    {
        Image image = before;
        put(image, first, journal.journal_head); put(image, first + 1, journal.descriptor);
        for (usize i = 0; i < journal.count; ++i)
        {
            put(image, first + u32(i) + 2, journal.payloads[i]);
            put(image, journal.targets[i], mask & (1U << i) ? scratch.namespace_plan.after[i] :
                scratch.namespace_plan.before[i]);
        }
        put(image, first + u32(journal.count) + 2, journal.commit);
        put(image, first + u32(journal.count) + 3, journal.end);
        return image;
    }
    void verify(const Image &image) const
    {
        for (usize i = 0; i < journal.count; ++i)
            CHECK(load(image, journal.targets[i]) == scratch.namespace_plan.after[i]);
        auto head = journal.journal_head;
        const auto big32 = [&](usize at, u32 number) {
            for (usize i = 0; i < 4; ++i) head[at + i] = u8(number >> (24 - i * 8));
        };
        big32(24, journal.sequence + 2); big32(28, 0); big32(252, 0);
        big32(252, jbd2::detail::crc32c(~u32{0}, head));
        CHECK(load(image, first) == head);
        auto expected = mixed((1U << journal.count) - 1); put(expected, first, head);
        CHECK(expected == image); // No data, allocation or unrelated byte changed.
    }
};
void final_state(Owner &owner, owned_fixture::Disk &disk, const Transaction &tx)
{
    CHECK(owner.report().completion.complete() && disk.live == disk.durable);
    tx.verify(disk.durable);
    CHECK(owner.report().completion.journal_sequence == tx.journal.sequence + 2);
    CHECK(!owner.buffers().read_pins && !owner.buffers().transaction_active);
    const auto pending = tx.scratch.namespace_plan.orphan_published;
    CHECK(owner.report().orphan_cleanup_pending == pending);
    CHECK(owner.report().phase == (pending ? Ext4OwnedPhase::orphan_cleanup_pending : Ext4OwnedPhase::checkpointed));
    if (pending)
    {
        Owner::Pin pin;
        const auto reads = disk.reads;
        CHECK(!owner.pin(1, pin) && !owner.close() && !owner.prepare_namespace(tx.update));
        CHECK(disk.reads == reads);
        Ext4RecoveryBinding ordinary;
        CHECK(!ordinary.inspect(disk, {disk.origin, disk.live.size() / 512}));
    }
    else CHECK(owner.close());
}
void live(const Transaction &tx)
{
    owned_fixture::Disk disk(tx.before); Owner::Registry registry;
    const auto key = registry.register_device(disk, 401); CHECK(key);
    auto owner = std::make_unique<Owner>(true);
    CHECK(owner->open(registry, key.value(), {0, disk.live.size() / 512}));
    CHECK(owner->prepare_namespace(tx.update)); CHECK(owner->commit());
    CHECK(disk.durable == tx.mixed(0));
    const auto writes = disk.writes, flushes = disk.flushes;
    CHECK(owner->checkpoint()); final_state(*owner, disk, tx);
    CHECK(disk.writes - writes == tx.journal.count + 1 && disk.flushes - flushes == 3);
    CHECK(!owner->report().cold_namespace); ++live_cases;
}
void foreign_history(const Transaction &published, const Transaction &foreign)
{
    CHECK(published.journal.journal_head == foreign.journal.journal_head);
    CHECK(published.journal.descriptor != foreign.journal.descriptor);
    CHECK(published.journal.payloads == foreign.journal.payloads && published.journal.commit == foreign.journal.commit);
    CHECK(published.scratch.namespace_plan.after == foreign.scratch.namespace_plan.after);
    owned_fixture::Disk disk(published.before); Owner::Registry registry;
    const auto key = registry.register_device(disk, 410); CHECK(key);
    auto owner = std::make_unique<Owner>(true);
    CHECK(owner->open(registry, key.value(), {0, disk.live.size() / 512}));
    CHECK(owner->prepare_namespace(published.update) && owner->commit());
    // A forged old timestamp has the identical current head and every after
    // home. The actual live owner's original durable witness still must match.
    disk.live = disk.durable = foreign.mixed((1U << foreign.journal.count) - 1);
    const auto bytes = disk.durable;
    const usize writes = disk.writes, flushes = disk.flushes;
    CHECK(!owner->checkpoint());
    CHECK(disk.durable == bytes && disk.live == bytes && disk.writes == writes && disk.flushes == flushes);
    CHECK(owner->report().phase == Ext4OwnedPhase::failed && owner->buffers().state == MetadataOwnerState::failed);
    ++history_rejected;
}
void cold(const Transaction &tx, u32 mask)
{
    const auto image = tx.mixed(mask);
    owned_fixture::Disk disk(image); disk.origin = 17;
    Owner::Registry registry; const auto key = registry.register_device(disk, 402); CHECK(key);
    auto owner = std::make_unique<Owner>();
    CHECK(owner->open(registry, key.value(), {disk.origin, image.size() / 512}));
    CHECK(owner->prepare_namespace_recovery());
    CHECK(owner->report().phase == Ext4OwnedPhase::durable && owner->report().cold_namespace);
    CHECK(!disk.writes && !disk.flushes && disk.live == image && disk.durable == image);
    CHECK(!owner->commit()); // Recovery authority cannot publish a new writer.
    Owner::Pin pin;
    CHECK(owner->pin(tx.first, pin));
    const usize reads = disk.reads;
    CHECK(owner->checkpoint().code() == StatusCode::busy && disk.reads == reads && !disk.writes && !disk.flushes);
    CHECK(pin.release());
    const usize start = disk.trace.size();
    CHECK(owner->checkpoint()); final_state(*owner, disk, tx);
    const usize old = tx.journal.count - usize(__builtin_popcount(mask));
    CHECK(disk.writes == old + 1 && disk.flushes == (old ? 3 : 2));
    std::vector<owned_fixture::Trace> ops;
    for (usize i = start; i < disk.trace.size(); ++i)
        if (disk.trace[i].operation != 'r') ops.push_back(disk.trace[i]);
    CHECK(ops.front().operation == 'f');
    usize at = 1;
    for (usize i = 0; i < tx.journal.count; ++i)
        if (!(mask & (1U << i)))
        { CHECK(ops[at].operation == 'w' && ops[at].sector == disk.origin + tx.journal.targets[i] * 2 && ops[at].size == 1024); ++at; }
    if (old) { CHECK(ops[at].operation == 'f'); ++at; }
    CHECK(ops[at].operation == 'w' && ops[at].sector == disk.origin + tx.first * 2 && ops[at].size == 1024); ++at;
    CHECK(ops[at].operation == 'f' && at + 1 == ops.size());
    if (!old) { CHECK(ops.size() == 3); ++all_after; }
    ++mixtures;
}
void reject_image(Image image)
{
    owned_fixture::Disk disk(image); Owner::Registry registry;
    const auto key = registry.register_device(disk, 403); CHECK(key);
    auto owner = std::make_unique<Owner>();
    CHECK(owner->open(registry, key.value(), {0, image.size() / 512}));
    CHECK(!owner->recover_namespace());
    CHECK(!disk.writes && !disk.flushes && image == disk.live && image == disk.durable);
    CHECK(owner->report().phase == Ext4OwnedPhase::failed && owner->buffers().state == MetadataOwnerState::failed);
    CHECK(registry.inspect(key.value()).value().state == block::DeviceState::quarantined);
    ++rejected;
}
void completion_fault(const Transaction &tx, usize write, usize flush, bool after, usize cut = 0)
{
    const auto image = tx.mixed(0);
    owned_fixture::Disk disk(image); Owner::Registry registry;
    const auto key = registry.register_device(disk, 404); CHECK(key);
    auto owner = std::make_unique<Owner>();
    CHECK(owner->open(registry, key.value(), {0, image.size() / 512}));
    CHECK(owner->prepare_namespace_recovery());
    disk.fail_write = after ? 0 : write; disk.fail_write_after = after ? write : 0;
    disk.fail_flush = after ? 0 : flush; disk.fail_flush_after = after ? flush : 0; disk.cut = cut;
    CHECK(!owner->checkpoint());
    CHECK(owner->report().phase == Ext4OwnedPhase::failed && owner->buffers().state == MetadataOwnerState::failed);
    CHECK(!owner->checkpoint() && !owner->close());
    CHECK(registry.inspect(key.value()).value().state == block::DeviceState::quarantined);
    ++faults;
}
void read_failure(const Transaction &tx)
{
    const auto image = tx.mixed(0);
    // Exercise complete admission, cache-staging and final physical verification
    // read positions, plus the no-I/O checkpoint authority checks separately.
    owned_fixture::Disk probe(image); Owner::Registry registry;
    const auto key = registry.register_device(probe, 405); CHECK(key);
    auto owner = std::make_unique<Owner>();
    CHECK(owner->open(registry, key.value(), {0, image.size() / 512}));
    CHECK(owner->prepare_namespace_recovery()); const auto admitted_reads = probe.reads;
    CHECK(owner->checkpoint()); const auto total_reads = probe.reads;
    const std::array<usize, 8> positions{1, 2, 3, admitted_reads - 1, admitted_reads,
        admitted_reads + 1, total_reads - 1, total_reads};
    for (const auto position : positions)
    {
        owned_fixture::Disk disk(image); disk.fail_read = position; Owner::Registry fresh;
        const auto registered = fresh.register_device(disk, 406); CHECK(registered);
        auto recovery = std::make_unique<Owner>();
        CHECK(recovery->open(fresh, registered.value(), {0, image.size() / 512}));
        CHECK(!recovery->recover_namespace());
        CHECK(recovery->report().phase == Ext4OwnedPhase::failed);
        CHECK(fresh.inspect(registered.value()).value().state == block::DeviceState::quarantined);
        if (position <= admitted_reads + 1) CHECK(!disk.writes && !disk.flushes);
        ++read_faults;
    }
}
void exercise(const Transaction &tx)
{
    live(tx);
    for (u32 mask = 0; mask < (1U << tx.journal.count); ++mask) cold(tx, mask);
}
}
int main(int argc, char **argv)
{
    CHECK(argc == 3);
    std::ifstream filled_in(argv[1], std::ios::binary), empty_in(argv[2], std::ios::binary);
    CHECK(filled_in && empty_in);
    const Image filled((std::istreambuf_iterator<char>(filled_in)), {}), empty((std::istreambuf_iterator<char>(empty_in)), {});
    CHECK(filled.size() == 32 * 1024 * 1024 && empty.size() == filled.size());
    owned_fixture::Disk fd(filled), ed(empty); Ext4RecoveryBinding fb, eb;
    CHECK(fb.inspect(fd, {0, filled.size() / 512}) && eb.inspect(ed, {0, empty.size() / 512}));
    Fixture same(filled, *fb.info()); dirty(same);
    Transaction basic(same, same.replacement("basic-note", "second-note")); exercise(basic);
    Fixture other_history(filled, *fb.info()); dirty(other_history);
    store32(other_history.parent, slot(2) + 12, value(other_history.parent, slot(2) + 12) + 17);
    other_history.stamp_inode(other_history.parent, 2);
    Transaction altered_history(other_history, other_history.replacement("basic-note", "second-note"));
    foreign_history(basic, altered_history);
    Transaction unlinked(same, same.unlink("basic-note")); exercise(unlinked);
    Fixture wide(filled, *fb.info(), 12, 14); dirty(wide);
    Transaction separate(wide, wide.replacement("seed", "basic-note")); exercise(separate);
    Fixture nonlast(filled, *fb.info()); dirty(nonlast);
    store16(nonlast.destination, slot(15) + 26, 2); nonlast.stamp_inode(nonlast.destination, 15);
    nonlast.source = nonlast.destination; nonlast.add_alias(15, "second-note", "second-link");
    Transaction linked(nonlast, nonlast.replacement("basic-note", "second-note")); exercise(linked);
    Fixture wide_nonlast(filled, *fb.info(), 12, 14); dirty(wide_nonlast);
    store16(wide_nonlast.destination, slot(14) + 26, 2); wide_nonlast.stamp_inode(wide_nonlast.destination, 14);
    wide_nonlast.add_alias(14, "second-note", "basic-link");
    Transaction separate_linked(wide_nonlast, wide_nonlast.replacement("seed", "basic-note")); exercise(separate_linked);
    Fixture alias(filled, *fb.info()); dirty(alias);
    store16(alias.source, slot(14) + 26, 2); alias.stamp_inode(alias.source, 14);
    alias.destination = alias.source; alias.add_alias(14, "second-note", "basic-link");
    Transaction unlink_linked(alias, alias.unlink("basic-note")); exercise(unlink_linked);
    Fixture zero(empty, *eb.info()); dirty(zero);
    Transaction empty_unlink(zero, zero.unlink("empty-note")); exercise(empty_unlink);
    // Foreign whole-home bytes, malformed witnessed records and empty head are
    // terminal failures; no fallback family or ordinary orphan mount is attempted.
    auto bad = basic.mixed(0); bad[basic.journal.targets[0] * 1024 + 200] ^= 1; reject_image(bad);
    bad = basic.mixed(0); bad[(basic.first + 1) * 1024 + 128] ^= 1; reject_image(bad);
    bad = basic.mixed(0); bad[(basic.first + u32(basic.journal.count) + 2) * 1024 + 200] ^= 1; reject_image(bad);
    bad = basic.mixed(0); put(bad, basic.first, load(basic.before, basic.first)); reject_image(bad);
    bad = basic.mixed(0); bad[1024 + 104] ^= 1; reject_image(bad);
    for (usize index = 1; index <= separate.journal.count + 1; ++index)
    {
        completion_fault(separate, index, 0, false); completion_fault(separate, index, 0, true);
        for (usize cut : {1U, 28U, 64U, 256U, 512U, 1023U}) completion_fault(separate, index, 0, false, cut);
    }
    for (usize index = 1; index <= 3; ++index)
    { completion_fault(separate, 0, index, false); completion_fault(separate, 0, index, true); }
    read_failure(basic);
    CHECK(mixtures == 112 && live_cases == 7 && all_after == 7 && faults == 54 && rejected == 5 && read_faults == 8 && history_rejected == 1);
    std::printf("PASS namespace completion mixtures=%zu live=%zu all_after=%zu faults=%zu rejected=%zu read_faults=%zu history_rejected=%zu same_session_cache=1 orphan_cleanup_deferred=1 hardware=0\n",
        mixtures, live_cases, all_after, faults, rejected, read_faults, history_rejected);
}
