// Hosted actual canonical Owner/Session I/O, distinct from guest execution.
#include "ext4_mounted_support.hpp"
#include "ext4_realtime_model.hpp"
#include <ok/fs/ext4/mount.hpp>
// Destructive negative controls reach private wrapper/workspace state; build this
// model with -fno-access-control. The native probe uses real privacy.
#include <memory>
#include <type_traits>
#include <atomic>
#include <thread>

using namespace ok;
using namespace ok::fs;
using namespace owned_fixture;
#define CHECK(expression) do { if (!(expression)) { std::fprintf(stderr, "FAIL fresh empty orphan:%d: %s\n", __LINE__, #expression); std::abort(); } } while (false)
static void checked(Status status, const char *label)
{
    if (!status) { std::fprintf(stderr, "FAIL %s: %.*s\n", label, int(status.message().size()), status.message().data()); std::abort(); }
}
using Recovery = Ext4EmptyOrphanRecoveryOwner<1>;
static_assert(!std::is_copy_constructible_v<Recovery> && !std::is_move_constructible_v<Recovery>);
static_assert(!std::is_copy_constructible_v<Ext4EmptyOrphanRecoveryWorkspace> && !std::is_move_constructible_v<Ext4EmptyOrphanRecoveryWorkspace>);
struct Fresh
{
    Disk disk;
    Recovery::Registry registry;
    std::unique_ptr<Ext4EmptyOrphanRecoveryWorkspace> workspace = std::make_unique<Ext4EmptyOrphanRecoveryWorkspace>();
    std::unique_ptr<Recovery> owner = std::make_unique<Recovery>();
    Recovery::Registry::Registration registration{};
    explicit Fresh(const Bytes &image, u64 origin = 0) : disk(image)
    {
        disk.origin = origin;
        const auto key = registry.register_device(disk, 911); CHECK(key); registration = key.value();
        checked(owner->open(registry, registration, {origin, image.size() / 512}, *workspace), "fresh open");
        CHECK(!disk.reads && !disk.writes && !disk.flushes);
    }
};
static Bytes load(const char *path)
{
    std::ifstream input(path, std::ios::binary); CHECK(input);
    Bytes bytes((std::istreambuf_iterator<char>(input)), {}); CHECK(bytes.size() == 32 * 1024 * 1024); return bytes;
}
static bool allocated(const Bytes &bytes, u32 inode)
{
    const u32 group = (inode - 1) / 2048, bit = (inode - 1) % 2048;
    return bytes[r32(bytes, descriptor(bytes, group) + 4) * 1024 + bit / 8] & (1U << (bit % 8));
}
static Bytes make_orphan(const Bytes &filled, bool empty)
{
    using Mount = Ext4MountedMetadata<1>;
    Disk disk(filled); Mount::Registry registry;
    auto workspace = std::make_unique<Ext4MountedReclaimWorkspace>();
    auto mount = std::make_unique<Mount>(Ext4MountedPolicy::ordered_data_dirty_noatime, workspace.get());
    auto vfs = std::make_unique<VirtualFileSystem>();
    const auto key = registry.register_device(disk, 801); CHECK(key);
    checked(mount->mount(registry, key.value(), {0, filled.size() / 512}), "input mount");
    checked(vfs->create("/mnt", NodeType::directory), "input mountpoint");
    checked(vfs->mount("/mnt", *mount->root()), "input publish");
    const char *path = empty ? "/mnt/fresh-empty" : "/mnt/seed";
    if (empty) checked(vfs->create(path, NodeType::regular), "input empty file");
    CHECK(vfs->lookup(path)); // Explicit raw exposure defers the live Owner.
    checked(vfs->unlink(path), "input committed unlink");
    const u32 head = r32(disk.durable, 1256); CHECK(head >= 11 && allocated(disk.durable, head));
    CHECK(be32(disk.durable, journal(disk.durable) + 28) == 0);
    CHECK(disk.pending.empty() && disk.live == disk.durable);
    return disk.durable;
}
static void quarantine(Fresh &h)
{
    CHECK(h.owner->report().phase == Ext4EmptyOrphanRecoveryPhase::failed);
    CHECK(h.owner->buffers().state == MetadataOwnerState::failed);
    CHECK(h.registry.inspect(h.registration).value().state == block::DeviceState::quarantined);
    const auto writes = h.disk.writes, flushes = h.disk.flushes, reads = h.disk.reads;
    CHECK(!h.owner->recover() && !h.owner->close());
    CHECK(h.disk.writes == writes && h.disk.flushes == flushes && h.disk.reads == reads);
    Recovery::Registry::Session foreign;
    CHECK(!h.registry.acquire(h.registration, {h.disk.origin, h.disk.live.size() / 512}, block::SessionAccess::writable, foreign));
}
static std::vector<u32> chain(const Bytes &image)
{
    std::vector<u32> result;
    u32 number = r32(image, 1256);
    while (number)
    {
        CHECK(result.size() < 8 && std::find(result.begin(), result.end(), number) == result.end());
        result.push_back(number); number = r32(image, inode_offset(image, number) + 20);
    }
    return result;
}
struct Success { Bytes dirty; std::vector<Trace> trace; usize reads{}, writes{}, flushes{}; };
static Success success(const Bytes &input, const std::filesystem::path &output, u64 origin = 0)
{
    auto h = std::make_unique<Fresh>(input, origin);
    const auto members = chain(input); CHECK(!members.empty());
    const auto initial = h->owner->report().original;
    // Every transport callback can re-enter observer methods. No Owner/cache
    // spinlock is held across device I/O, and no public execution is admitted.
    usize observations{};
    h->disk.hook = [&](char) {
        CHECK(h->owner->report().phase == Ext4EmptyOrphanRecoveryPhase::recovering);
        CHECK(h->owner->buffers().state == MetadataOwnerState::ready);
        CHECK(h->owner->recover().code() == StatusCode::busy);
        CHECK(h->owner->close().code() == StatusCode::busy);
        ++observations;
    };
    testing::ext4_clock::reset();
    checked(h->owner->recover(), "fresh complete chain");
    CHECK(testing::ext4_clock::calls() == 0);
    h->disk.hook = {};
    const auto report = h->owner->report();
    CHECK(report.complete && report.reclaimed == members.size() && report.chain_members == members.size());
    CHECK(report.verified_publications == members.size() * 3);
    CHECK(report.initial_head == members[0] && report.original.session_serial == initial.session_serial);
    CHECK(report.original.identity == report.observed.identity && report.original.registration_generation == report.observed.registration_generation);
    CHECK(report.original.session_serial == report.observed.session_serial && !report.observed.pending_writes && !report.observed.in_flight);
    CHECK(report.observed.reads == h->disk.reads && report.observed.writes == h->disk.writes && report.observed.flushes == h->disk.flushes);
    CHECK(report.final_sequence == be32(input, journal(input) + 24) + members.size() * 3);
    CHECK(observations == h->disk.reads + h->disk.writes + h->disk.flushes);
    CHECK(!r32(h->disk.durable, 1256) && be32(h->disk.durable, journal(h->disk.durable) + 28) == 0);
    CHECK(h->disk.pending.empty() && h->disk.live == h->disk.durable);
    for (const auto number : members)
    {
        CHECK(!allocated(h->disk.durable, number));
        const usize at = inode_offset(input, number);
        CHECK(r32(h->disk.durable, at + 100) == r32(input, at + 100) + 1);
        CHECK(!r16(h->disk.durable, at) && !r16(h->disk.durable, at + 26));
        CHECK(r32(h->disk.durable, at + 160) == 0x3146524f);
    }
    // Unrelated inode, file data, every directory and every non-participant
    // filesystem byte is preserved; journal records are the only extra writes.
    std::vector<bool> changed(input.size() / 1024);
    const auto first = journal(input) / 1024;
    for (usize index = 0; index < 9; ++index) changed[first + index] = true;
    changed[1] = changed[2] = true;
    for (u32 number : members)
    {
        const u32 group = (number - 1) / 2048;
        changed[inode_offset(input, number) / 1024] = true;
        changed[r32(input, descriptor(input, group) + 4)] = true;
        changed[r32(input, descriptor(input, group))] = true;
    }
    for (usize block = 0; block < changed.size(); ++block)
        if (!changed[block]) CHECK(std::equal(input.begin() + block * 1024, input.begin() + (block + 1) * 1024, h->disk.durable.begin() + block * 1024));
    Success result{h->disk.durable, h->disk.trace, h->disk.reads, h->disk.writes, h->disk.flushes};
    checked(h->owner->close(), "fresh close");
    CHECK(h->registry.inspect(h->registration).value().state == block::DeviceState::ready);
    CHECK(h->owner->report().complete && h->owner->report().phase == Ext4EmptyOrphanRecoveryPhase::closed);
    CHECK(!h->owner->open(h->registry, h->registration, {origin, input.size() / 512}, *h->workspace));
    auto other = std::make_unique<Recovery>();
    const auto io = h->disk.reads + h->disk.writes + h->disk.flushes;
    CHECK(other->open(h->registry, h->registration, {origin, input.size() / 512}, *h->workspace).code() == StatusCode::already_exists);
    CHECK(h->disk.reads + h->disk.writes + h->disk.flushes == io);
    save(output, result.dirty);
    return result;
}
static void refused_input(const Bytes &input)
{
    auto h = std::make_unique<Fresh>(input);
    CHECK(!h->owner->recover()); quarantine(*h);
    CHECK(!h->disk.writes && !h->disk.flushes);
}
static void negative_inputs(const Bytes &input, const Bytes &filled)
{
    refused_input(filled); // No orphan head is never a supplied execution plan.
    auto altered = input;
    const u32 inode = r32(input, 1256); const usize at = inode_offset(input, inode);
    w32(altered, at + 20, inode); inode_crc(altered, inode, at); refused_input(altered);
    altered = input; w32(altered, at + 100, ~u32{0}); inode_crc(altered, inode, at); refused_input(altered);
    altered = input; w32(altered, at + 132, (1000000000U << 2)); inode_crc(altered, inode, at); refused_input(altered);
    altered = input; w32(altered, at + 132, 1); inode_crc(altered, inode, at); refused_input(altered);
    altered = input; big32(altered, journal(altered) + 24, 0xfffffffdU); big32(altered, journal(altered) + 252, 0);
    big32(altered, journal(altered) + 252, crc(~u32{0}, altered, journal(altered), 1024)); refused_input(altered);
    altered = input; w32(altered, 1256, 8); super_crc(altered); refused_input(altered);
    altered = input;
    const usize root = r32(filled, inode_offset(filled, 2) + 60) * 1024;
    std::copy_n(filled.begin() + root, 1024, altered.begin() + root); refused_input(altered);
    altered = input; w32(altered, at + 60, u32(journal(input) / 1024)); inode_crc(altered, inode, at); refused_input(altered);
    auto geometry = std::make_unique<Fresh>(input); geometry->disk.writable = false;
    CHECK(!geometry->owner->recover()); quarantine(*geometry); CHECK(!geometry->disk.writes && !geometry->disk.flushes);
    // A whole-source mutation during retained rereads is refused before writes.
    auto h = std::make_unique<Fresh>(input);
    bool changed{};
    h->disk.hook = [&](char op) {
        if (op == 'r' && !changed && h->disk.reads == 100) { changed = true; h->disk.live[at + 40] ^= 1; }
    };
    CHECK(!h->owner->recover() && changed); h->disk.hook = {}; quarantine(*h);
    CHECK(!h->disk.writes && !h->disk.flushes);
}
static void fresh_restart(const Bytes &durable, const std::filesystem::path &output)
{
    // The empty-only Owner must refuse any actually active or torn publication
    // before WRITE/FLUSH. Active-reclaim recovery is a separate bounded gate.
    if (be32(durable, journal(durable) + 28)) { refused_input(durable); return; }
    if (r32(durable, 1256)) static_cast<void>(success(durable, output));
    else {
        Ext4RecoveryBinding binding; Disk disk(durable);
        checked(binding.inspect(disk, {0, durable.size() / 512}), "finished restart ordinary binding");
        CHECK(binding.info()->empty_log); save(output, durable);
    }
}
static void faults(const Bytes &input, const Success &baseline, const std::filesystem::path &directory)
{
    usize write_cases{}, flush_cases{}, read_cases{}, torn_cases{}, restarts{};
    for (usize failure = 1; failure <= baseline.writes; ++failure)
        for (bool after : {false, true})
        {
            auto h = std::make_unique<Fresh>(input);
            if (after) h->disk.fail_write_after = failure; else h->disk.fail_write = failure;
            CHECK(!h->owner->recover()); quarantine(*h); CHECK(h->disk.writes == failure);
            CHECK(h->disk.submitted.size() == failure);
            fresh_restart(h->disk.durable, directory / ("restart-write-" + std::to_string(failure) + (after ? "-after.img" : "-before.img")));
            ++write_cases; ++restarts;
        }
    for (usize failure = 1; failure <= baseline.flushes; ++failure)
        for (bool after : {false, true})
        {
            auto h = std::make_unique<Fresh>(input);
            if (after) h->disk.fail_flush_after = failure; else h->disk.fail_flush = failure;
            CHECK(!h->owner->recover()); quarantine(*h); CHECK(h->disk.flushes == failure);
            fresh_restart(h->disk.durable, directory / ("restart-flush-" + std::to_string(failure) + (after ? "-after.img" : "-before.img")));
            ++flush_cases; ++restarts;
        }
    usize selective_flush_cases{};
    for (usize failure = 1; failure <= baseline.flushes; ++failure)
        for (u32 mask : {u32{1}, u32{0xaaaaaaaaU}, ~u32{0}})
        {
            auto h = std::make_unique<Fresh>(input); h->disk.fail_flush = failure; h->disk.persist_mask = mask;
            CHECK(!h->owner->recover()); quarantine(*h); CHECK(h->disk.flushes == failure);
            auto restart = std::make_unique<Fresh>(h->disk.durable);
            const auto status = restart->owner->recover();
            if (!status) {
                if (r32(h->disk.durable, 1256)) { quarantine(*restart); CHECK(!restart->disk.writes && !restart->disk.flushes); }
                else { quarantine(*restart); CHECK(!restart->disk.writes && !restart->disk.flushes); }
            }
            else { CHECK(restart->owner->report().complete); checked(restart->owner->close(), "selective flush restart close"); }
            ++selective_flush_cases;
        }
    CHECK(selective_flush_cases == baseline.flushes * 3);
    // Identify actual home writes/readbacks from the recorded real I/O stream,
    // then sweep genuine byte/sector tears only on changed home bytes.
    const usize journal_first = journal(input) / 512;
    usize write_index{}, read_index{}, first_home_read{}, publication_read{};
    std::vector<usize> home_writes;
    for (const auto &op : baseline.trace)
    {
        if (op.operation == 'w')
        {
            ++write_index;
            if (op.sector < journal_first || op.sector >= journal_first + 18) home_writes.push_back(write_index);
        }
        if (op.operation == 'r')
        {
            ++read_index;
            if (write_index && !publication_read) publication_read = read_index;
            if (!home_writes.empty() && !first_home_read) first_home_read = read_index;
        }
    }
    for (usize failure : {usize{1}, publication_read, first_home_read, baseline.reads})
    {
        CHECK(failure);
        auto h = std::make_unique<Fresh>(input); h->disk.fail_read = failure;
        CHECK(!h->owner->recover()); quarantine(*h); CHECK(h->disk.reads == failure); ++read_cases;
    }
    usize all_cut_cases{};
    for (usize failure = 1; failure <= baseline.writes; ++failure)
        for (usize cut : {usize{0}, usize{1}, usize{256}, usize{512}, usize{768}, usize{1023}, usize{1024}})
        {
            auto h = std::make_unique<Fresh>(input); h->disk.fail_write = failure; h->disk.cut = cut;
            h->disk.persist_mask = 0xffffffffU; // All completed batch writes may survive.
            CHECK(!h->owner->recover()); quarantine(*h); CHECK(h->disk.writes == failure);
            const auto &write = h->disk.submitted.back();
            const bool whole_old = std::equal(h->disk.durable.begin() + write.sector * 512,
                h->disk.durable.begin() + write.sector * 512 + 1024, input.begin() + write.sector * 512);
            const bool whole_after = std::equal(h->disk.durable.begin() + write.sector * 512,
                h->disk.durable.begin() + write.sector * 512 + 1024, write.bytes.begin());
            const bool home = std::find(home_writes.begin(), home_writes.end(), failure) != home_writes.end();
            if (home && !whole_old && !whole_after) ++torn_cases;
            // A fresh Session and Owner never borrows the failed lifetime or
            // substitutes a copied report. Actual active/CRC-invalid states are
            // refused by this empty-only gate before any mutation.
            auto restart = std::make_unique<Fresh>(h->disk.durable);
            const auto status = restart->owner->recover();
            if (!status) { quarantine(*restart); CHECK(!restart->disk.writes && !restart->disk.flushes); }
            else { CHECK(restart->owner->report().complete); checked(restart->owner->close(), "cut restart close"); }
            ++all_cut_cases;
        }
    CHECK(all_cut_cases == baseline.writes * 7);
    CHECK(write_cases == baseline.writes * 2 && flush_cases == baseline.flushes * 2 && read_cases == 4 && torn_cases > 0);
    std::printf("fault sweep writes=%zu flushes=%zu reads=%zu genuine-torn-homes=%zu fresh-restarts=%zu all-write-cuts=%zu selective-flushes=%zu\n", write_cases, flush_cases, read_cases, torn_cases, restarts, all_cut_cases, selective_flush_cases);
}
static Bytes cross_group_chain(Bytes image)
{
    // Independent raw fixture relocation, with upstream libext2fs CRC32C.
    // Move the last orphan inode into initialized group 1. Its mapped data
    // remains in group 0, exercising distinct inode/data allocation groups.
    const auto members = chain(image); CHECK(members.size() == 2 || members.size() == 8);
    const u32 old = members.back(), moved = 2050;
    const usize old_at = inode_offset(image, old), new_at = inode_offset(image, moved);
    CHECK(!allocated(image, moved));
    std::copy_n(image.begin() + old_at, 256, image.begin() + new_at);
    std::fill_n(image.begin() + old_at, 256, 0);
    inode_crc(image, moved, new_at);
    const usize predecessor = inode_offset(image, members[members.size() - 2]);
    w32(image, predecessor + 20, moved); inode_crc(image, members[members.size() - 2], predecessor);
    for (u32 group : {u32{0}, u32{1}})
    {
        const usize desc = descriptor(image, group), bitmap = r32(image, desc + 4) * 1024;
        if (!group)
        {
            const u32 bit = old - 1; image[bitmap + bit / 8] &= u8(~(1U << (bit % 8)));
            w16(image, desc + 14, u16(r16(image, desc + 14) + 1));
            u32 highest{};
            for (u32 bit_index = 0; bit_index < 2048; ++bit_index)
                if (image[bitmap + bit_index / 8] & (1U << (bit_index % 8))) highest = bit_index + 1;
            w16(image, desc + 28, u16(2048 - highest));
        }
        else
        {
            std::fill_n(image.begin() + bitmap, 256, 0);
            std::fill_n(image.begin() + bitmap + 256, 768, 0xff);
            image[bitmap] = 2; // inode 2050 is group-local index 1.
            w16(image, desc + 14, u16(r16(image, desc + 14) - 1));
            w16(image, desc + 18, u16(r16(image, desc + 18) & ~1U));
            w16(image, desc + 28, 2046);
        }
        w16(image, desc + 26, u16(crc(seed(image), image, bitmap, 256)));
        group_crc(image, group, desc);
    }
    CHECK(allocated(image, moved) && !allocated(image, old));
    return image;
}
static void private_controls(const Bytes &input)
{
    for (usize control = 0; control < 5; ++control)
    {
        auto h = std::make_unique<Fresh>(input); bool changed{};
        h->disk.hook = [&](char operation) {
            if (operation != 'f' || changed) return;
            changed = true;
            if (control == 0) h->workspace->original_.inode_tables[0][500][0] ^= 1;
            if (control == 1) h->owner->seal_.generation ^= 1;
            if (control == 2) h->owner->seal_.session_serial ^= 1;
            if (control == 3) h->owner->seal_.workspace = nullptr;
            if (control == 4) h->workspace->claimant_ = nullptr;
        };
        CHECK(!h->owner->recover() && changed); h->disk.hook = {}; quarantine(*h);
        CHECK(!h->disk.writes && h->disk.flushes == 1);
    }
    // A real hosted thread attempts a shared workspace takeover while the
    // first Owner is performing device I/O. The foreign transport sees none.
    auto h = std::make_unique<Fresh>(input);
    Disk foreign_disk(input); Recovery::Registry foreign_registry;
    const auto foreign = foreign_registry.register_device(foreign_disk, 919); CHECK(foreign);
    auto contender = std::make_unique<Recovery>(); bool contended{};
    h->disk.hook = [&](char operation) {
        if (operation != 'r' || contended) return;
        contended = true;
        std::thread thread([&] {
            CHECK(contender->open(foreign_registry, foreign.value(), {0, input.size() / 512}, *h->workspace).code() == StatusCode::already_exists);
        });
        thread.join();
    };
    checked(h->owner->recover(), "thread contender recovery"); h->disk.hook = {};
    CHECK(contended && !foreign_disk.reads && !foreign_disk.writes && !foreign_disk.flushes);
    checked(h->owner->close(), "thread contender close");
}
static void next_member_history(const Bytes &input)
{
    auto h = std::make_unique<Fresh>(input);
    const auto members = chain(input); CHECK(members.size() == 2);
    bool changed{}; usize writes_at_change{};
    h->disk.hook = [&](char operation) {
        if (operation == 'r' && !changed && h->owner->report().reclaimed == 1 && r32(h->disk.live, 1256) == members[1] &&
            be32(h->disk.live, journal(h->disk.live) + 28) == 0)
        {
            changed = true; writes_at_change = h->disk.writes;
            const usize at = inode_offset(h->disk.live, members[1]);
            w32(h->disk.live, at + 100, r32(h->disk.live, at + 100) + 1);
            inode_crc(h->disk.live, members[1], at);
        }
    };
    CHECK(!h->owner->recover() && changed); h->disk.hook = {}; quarantine(*h);
    CHECK(h->disk.writes == writes_at_change && h->owner->report().reclaimed == 1);
}
int main(int argc, char **argv)
{
    CHECK(argc == 6); const auto filled = load(argv[1]); const std::filesystem::path output = argv[5];
    std::filesystem::create_directory(output);
    testing::ext4_clock::set({{1700000666, 123456789}, {200, 1}, 1, time::RealtimeOrigin::administrator_software});
    const auto data = make_orphan(filled, false), empty = make_orphan(filled, true);
    save(output / "input-data.img", data); save(output / "input-empty.img", empty);
    const auto d = success(data, output / "finished-data.img");
    static_cast<void>(success(empty, output / "finished-empty.img"));
    static_cast<void>(success(data, output / "finished-origin.img", 2048));
    const auto chain2 = cross_group_chain(load(argv[2])), chain8 = cross_group_chain(load(argv[3]));
    save(output / "input-chain2-cross-group.img", chain2); save(output / "input-chain8-cross-group.img", chain8);
    static_cast<void>(success(chain2, output / "finished-chain2.img"));
    static_cast<void>(success(chain8, output / "finished-chain8.img"));
    negative_inputs(data, filled);
    next_member_history(chain2);
    private_controls(data);
    refused_input(load(argv[4])); // Independent nine-member chain exceeds the complete proof budget.
    faults(data, d, output);
    std::printf("PASS fresh empty orphan: members=1,2,8 real-cache-session WAL/checkpoint, exact counters, permanent workspace, no realtime, prefix restarts and torn refusal; baseline r=%zu w=%zu f=%zu\n", d.reads, d.writes, d.flushes);
}
