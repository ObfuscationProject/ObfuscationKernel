// Hosted models of ordered free-block initialization; no hardware execution.
#include "ext4_mounted_support.hpp"
#include "ext4_realtime_model.hpp"
#include <ok/fs/ext4/mount.hpp>
#include <memory>

using namespace owned_fixture;
namespace clock_model = ok::testing::ext4_clock;
using Owner = Ext4OwnedTransaction<1, 10>;
using Mount = Ext4MountedMetadata<1>;
constexpr Ext4CreateUpdate mkdir_update{2, 1700000311, 123456789, "initialized-inbox",
    {NodeType::directory, 02750, 0x10001, 0x20002}};
constexpr u32 free_block = 535;
#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "FAIL ordered mkdir:%d: %s\n", __LINE__, #expression); std::exit(1); \
} } while (false)
namespace
{
struct Owned
{
    Disk disk;
    Owner::Registry registry;
    Owner owner{true};
    explicit Owned(const Bytes &image) : disk(image)
    {
        auto key = registry.register_device(disk, 17); CHECK(key);
        CHECK(owner.open(registry, key.value(), {0, image.size() / 512}));
    }
};
Bytes model_old_image(Bytes bytes)
{
    // Local ordinary-inode variants only; the input fixture and stale free data
    // bytes remain unchanged. Zero inactive inode records are explicit fixtures.
    const usize bitmap = r32(bytes, descriptor(bytes, 0) + 4) * 1024;
    for (u32 number : {14U, 15U})
    {
        CHECK(!(bytes[bitmap + (number - 1) / 8] & (1U << ((number - 1) % 8))));
        std::fill_n(bytes.begin() + inode_offset(bytes, number), 256, 0);
    }
    CHECK(std::any_of(bytes.begin() + free_block * 1024, bytes.begin() + (free_block + 1) * 1024,
        [](u8 byte) { return byte != 0; }));
    w32(bytes, 1120, r32(bytes, 1120) | 4U); super_crc(bytes);
    return bytes;
}
void parent_drift(Bytes &bytes)
{
    const auto at = inode_offset(bytes, 2);
    w16(bytes, at, u16(r16(bytes, at) ^ 1U)); inode_crc(bytes, 2, at);
}
void unchanged_allocations(const Bytes &old, const Bytes &now)
{
    CHECK(std::equal(old.begin() + 1024, old.begin() + 3072, now.begin() + 1024));
    const auto ipg = r32(old, 1064);
    const auto groups = (old.size() / 1024 - 1 + r32(old, 1056) - 1) / r32(old, 1056);
    for (usize group = 0; group < groups; ++group)
    {
        const auto gd = descriptor(old, group);
        for (usize offset : {usize{0}, usize{4}})
        {
            const auto first = usize(r32(old, gd + offset)) * 1024;
            CHECK(std::equal(old.begin() + first, old.begin() + first + 1024, now.begin() + first));
        }
        const auto table = usize(r32(old, gd + 8)) * 1024;
        CHECK(std::equal(old.begin() + table, old.begin() + table + ipg * 256, now.begin() + table));
    }
    const auto parent = usize(r32(old, inode_offset(old, 2) + 60)) * 1024;
    CHECK(std::equal(old.begin() + parent, old.begin() + parent + 1024, now.begin() + parent));
    const auto head = journal(old);
    CHECK(std::equal(old.begin() + head, old.begin() + head + 1024, now.begin() + head));
}
void preserved_seed(const Bytes &old, const Bytes &now)
{
    const auto inode = inode_offset(old, 12);
    CHECK(std::equal(old.begin() + inode, old.begin() + inode + 256, now.begin() + inode));
    const auto physical = usize(r32(old, inode + 60)) * 1024;
    CHECK(std::equal(old.begin() + physical, old.begin() + physical + 1024, now.begin() + physical));
}
void cold_directory(const Bytes &bytes, bool require_created, bool allow_old)
{
    Disk disk(bytes);
    auto completion = std::make_unique<Ext4RecoveryCompletion<8, 2>>(
        false, true, Ext4ReplayPolicy::bounded_directory_create);
    CHECK(completion->run(disk, {0, bytes.size() / 512}));
    auto view = std::make_unique<Ext4ReplayPreview<8, 2>>(
        false, true, Ext4ReplayPolicy::bounded_directory_create);
    CHECK(view->open(disk, {0, bytes.size() / 512}));
    auto found = view->lookup(2, mkdir_update.name);
    if (!found)
    {
        CHECK(allow_old && !require_created && found.status().code() == StatusCode::not_found);
        return;
    }
    CHECK(require_created);
    auto created = view->inode(found.value()); CHECK(created);
    CHECK((created.value().mode & 0170000) == 0040000 && created.value().size == 1024 &&
        r32(disk.live, inode_offset(disk.live, found.value()) + 28) == 2 && created.value().links == 2);
    CHECK(view->lookup(found.value(), ".").value() == found.value());
    CHECK(view->lookup(found.value(), "..").value() == 2);
    CHECK(view->lookup(2, "seed").value() == 12);
    preserved_seed(bytes, disk.live);
}
struct Success
{
    Bytes complete;
    usize prezero_reads{}, writes{}, flushes{};
    std::vector<char> mutations;
};
Success successful_owner(const Bytes &old, bool stale)
{
    auto owned = std::make_unique<Owned>(old);
    CHECK(owned->owner.prepare_create(mkdir_update));
    CHECK(!owned->disk.writes && !owned->disk.flushes && owned->disk.live == old && owned->disk.durable == old);
    const auto start = owned->disk.trace.size();
    CHECK(owned->owner.commit());
    Success result{};
    usize first_write = owned->disk.trace.size();
    for (usize index = start; index < owned->disk.trace.size(); ++index)
        if (owned->disk.trace[index].operation == 'w') { first_write = index; break; }
    CHECK(first_write < owned->disk.trace.size());
    for (usize index = start; index < first_write; ++index)
        result.prezero_reads += owned->disk.trace[index].operation == 'r' ? 1 : 0;
    if (stale)
    {
        CHECK(owned->disk.trace[first_write].sector == free_block * 2 &&
            owned->disk.trace[first_write].size == 1024 && owned->disk.trace[first_write + 1].operation == 'f');
        CHECK(std::all_of(owned->disk.submitted.front().bytes.begin(), owned->disk.submitted.front().bytes.end(),
            [](u8 byte) { return byte == 0; }));
        CHECK(std::all_of(owned->disk.durable.begin() + free_block * 1024,
            owned->disk.durable.begin() + (free_block + 1) * 1024, [](u8 byte) { return byte == 0; }));
    }
    else CHECK(owned->disk.trace[first_write].sector == journal(old) / 512 + 2);
    const auto homes = owned->owner.report().writer.completed_writes - (stale ? 5U : 4U);
    CHECK(homes == 7 || homes == 8);
    CHECK(owned->disk.writes == homes + (stale ? 5 : 4) && owned->disk.flushes == (stale ? 5U : 4U));
    cold_directory(owned->disk.durable, true, false);
    CHECK(owned->owner.checkpoint());
    CHECK(owned->disk.writes == homes * 2 + (stale ? 6 : 5) && owned->disk.flushes == (stale ? 10U : 9U));
    CHECK(owned->disk.writes <= 22 && owned->disk.flushes <= 10);
    result.complete = owned->disk.durable; result.writes = owned->disk.writes; result.flushes = owned->disk.flushes;
    for (usize index = start; index < owned->disk.trace.size(); ++index)
        if (owned->disk.trace[index].operation != 'r') result.mutations.push_back(owned->disk.trace[index].operation);
    preserved_seed(old, result.complete); cold_directory(result.complete, true, false);
    CHECK(owned->owner.close());
    return result;
}
struct ProbeSpoofDisk final : driver::BlockDevice
{
    Disk backing;
    explicit ProbeSpoofDisk(const Bytes &bytes) : backing(bytes) {}
    driver::BlockGeometry geometry() const override { return backing.geometry(); }
    Status read_blocks(u64 sector, std::span<std::byte> out) override
    {
        auto status = backing.read_blocks(sector, out);
        if (status && backing.reads == 1)
        {
            CHECK(sector == 2 && out.size() == 1024);
            // Only the detached first probe sees a false 1 KiB log size.
            // The backing image and every subsequent binding read remain 4 KiB.
            for (usize byte = 24; byte < 28; ++byte) out[byte] = std::byte{0};
        }
        return status;
    }
    Status write_blocks(u64 sector, std::span<const std::byte> bytes) override
    { return backing.write_blocks(sector, bytes); }
    Status flush() override { return backing.flush(); }
};
void actual_format_binding_control(const Bytes &image)
{
    CHECK(block_size(image) == 4096);
    Disk validation(image); auto binding = std::make_unique<Ext4RecoveryBinding>();
    CHECK(binding->inspect(validation, {0, image.size() / 512}));
    CHECK(binding->info()->layout.block_size == 4096);
    ProbeSpoofDisk device(image); Owner::Registry registry;
    auto key = registry.register_device(device, 18); CHECK(key);
    auto owner = std::make_unique<Owner>(true);
    CHECK(owner->open(registry, key.value(), {0, image.size() / 512}));
    const auto status = owner->prepare_create(mkdir_update);
    CHECK(!status && status.code() == StatusCode::unsupported &&
        status.message() == "selected ext4 route requires an actual 1 KiB binding");
    CHECK(device.backing.reads > 1 && device.backing.reads < 20 &&
        !device.backing.writes && !device.backing.flushes);
    CHECK(device.backing.live == image && device.backing.durable == image);
}
void read_only_preparation_controls(const Bytes &old)
{
    for (u32 block_log : {1U, 2U})
    {
        auto unsupported = old; w32(unsupported, 1048, block_log); super_crc(unsupported);
        auto owned = std::make_unique<Owned>(unsupported);
        CHECK(owned->owner.prepare_create(mkdir_update).code() == StatusCode::unsupported);
        CHECK(owned->disk.reads == 1 && !owned->disk.writes && !owned->disk.flushes);
        CHECK(owned->disk.live == unsupported && owned->disk.durable == unsupported);
    }
    auto format_drift = std::make_unique<Owned>(old);
    format_drift->disk.hook = [&](char call) {
        if (call == 'r' && format_drift->disk.reads == 1)
        { w32(format_drift->disk.live, 1048, 2); super_crc(format_drift->disk.live); }
    };
    CHECK(!format_drift->owner.prepare_create(mkdir_update));
    CHECK(format_drift->disk.reads < 20 && !format_drift->disk.writes && !format_drift->disk.flushes);
    auto existing = std::make_unique<Owned>(old); auto update = mkdir_update; update.name = "seed";
    CHECK(existing->owner.prepare_create(update).code() == StatusCode::already_exists);
    CHECK(!existing->disk.writes && !existing->disk.flushes);
    auto alias = old;
    const auto inode = inode_offset(alias, 12);
    w32(alias, inode + 60, free_block); inode_crc(alias, 12, inode);
    auto referenced = std::make_unique<Owned>(alias);
    CHECK(!referenced->owner.prepare_create(mkdir_update) && !referenced->disk.writes && !referenced->disk.flushes);
    Disk readonly(old); readonly.writable = false; Owner::Registry registry;
    auto key = registry.register_device(readonly, 17); CHECK(key);
    auto owner = std::make_unique<Owner>(true);
    CHECK(!owner->open(registry, key.value(), {0, old.size() / 512}));
    CHECK(!readonly.writes && !readonly.flushes && readonly.live == old);
}
void prepared_refusals(const Bytes &old, usize prezero_reads)
{
    // Valid-CRC participant drift, stale free inode generation, changed head,
    // and changed parent directory are all refused before any mutation.
    for (unsigned kind = 0; kind < 4; ++kind)
    {
        auto owned = std::make_unique<Owned>(old); CHECK(owned->owner.prepare_create(mkdir_update));
        if (kind == 0) parent_drift(owned->disk.live);
        if (kind == 1)
        {
            const auto at = inode_offset(old, owned->owner.created_inode());
            w32(owned->disk.live, at + 100, r32(owned->disk.live, at + 100) + 1);
            inode_crc(owned->disk.live, owned->owner.created_inode(), at);
        }
        if (kind == 2)
        {
            const auto head = journal(old); big32(owned->disk.live, head + 24, be32(old, head + 24) + 1);
            journal_record_crc(owned->disk.live, head, 252);
        }
        if (kind == 3)
        {
            const auto parent = usize(r32(old, inode_offset(old, 2) + 60)) * 1024;
            owned->disk.live[parent + 32] ^= 1; directory_crc(owned->disk.live, 2, parent);
        }
        CHECK(!owned->owner.commit() && owned->owner.report().phase == Ext4OwnedPhase::failed);
        CHECK(!owned->disk.writes && !owned->disk.flushes);
    }
    // Inject after the repeated ownership scan's last physical read. The
    // following physical bookend must catch the CRC-valid participant drift.
    CHECK(prezero_reads > 9);
    auto late = std::make_unique<Owned>(old); CHECK(late->owner.prepare_create(mkdir_update));
    usize reads{}; bool injected{};
    late->disk.hook = [&](char kind) {
        if (kind == 'r' && ++reads == prezero_reads - 9) { parent_drift(late->disk.live); injected = true; }
    };
    CHECK(!late->owner.commit() && injected && !late->disk.writes && !late->disk.flushes);
    CHECK(late->owner.report().phase == Ext4OwnedPhase::failed);
    // An additional public Session read invalidates the exact admitted prefix.
    auto endpoint = std::make_unique<Owned>(old); CHECK(endpoint->owner.prepare_create(mkdir_update));
    Owner::Pin extra; CHECK(endpoint->owner.pin(500, extra) && extra.release());
    CHECK(!endpoint->owner.commit() && !endpoint->disk.writes && !endpoint->disk.flushes);
}
void initialization_failures(const Bytes &old)
{
    for (usize cut : {0U, 1U, 511U, 512U, 1023U, 1024U})
    {
        auto owned = std::make_unique<Owned>(old); CHECK(owned->owner.prepare_create(mkdir_update));
        owned->disk.fail_write = 1; owned->disk.cut = cut;
        CHECK(!owned->owner.commit() && owned->owner.report().phase == Ext4OwnedPhase::failed);
        CHECK(owned->disk.writes == 1 && !owned->disk.flushes);
        unchanged_allocations(old, owned->disk.live); unchanged_allocations(old, owned->disk.durable);
        owned->disk.crash(); cold_directory(owned->disk.durable, false, true);
    }
    for (unsigned kind = 0; kind < 4; ++kind)
    {
        auto owned = std::make_unique<Owned>(old); CHECK(owned->owner.prepare_create(mkdir_update));
        if (kind == 0) owned->disk.fail_write_after = 1;
        if (kind == 1) owned->disk.fail_flush = 1;
        if (kind == 2) owned->disk.fail_flush_after = 1;
        if (kind == 3) owned->disk.hook = [&](char call) {
            if (call == 'r' && owned->disk.writes == 1 && owned->disk.flushes == 1)
                owned->disk.live[free_block * 1024] = 1;
        };
        CHECK(!owned->owner.commit() && owned->owner.report().phase == Ext4OwnedPhase::failed);
        CHECK(owned->disk.writes == 1 && owned->disk.flushes <= 1);
        unchanged_allocations(old, owned->disk.live); unchanged_allocations(old, owned->disk.durable);
    }
    auto drift = std::make_unique<Owned>(old); CHECK(drift->owner.prepare_create(mkdir_update));
    drift->disk.writable = false;
    CHECK(!drift->owner.commit() && !drift->disk.writes && !drift->disk.flushes);
    CHECK(drift->owner.report().phase == Ext4OwnedPhase::failed);
}
void later_mutation_cuts(const Bytes &old, const Success &success)
{
    usize old_count{}, complete_count{};
    for (usize point = 2; point < success.mutations.size(); ++point)
    {
        auto owned = std::make_unique<Owned>(old); CHECK(owned->owner.prepare_create(mkdir_update));
        usize writes{}, flushes{};
        for (usize index = 0; index < point; ++index)
            success.mutations[index] == 'w' ? ++writes : ++flushes;
        if (success.mutations[point] == 'w') owned->disk.fail_write = writes + 1;
        else owned->disk.fail_flush = flushes + 1;
        const auto committed = owned->owner.commit();
        const auto status = committed ? owned->owner.checkpoint() : committed;
        CHECK(!status && owned->owner.report().phase == Ext4OwnedPhase::failed);
        owned->disk.crash();
        Disk recovered(owned->disk.durable);
        auto completion = std::make_unique<Ext4RecoveryCompletion<8, 2>>(
            false, true, Ext4ReplayPolicy::bounded_directory_create);
        CHECK(completion->run(recovered, {0, old.size() / 512}));
        auto view = std::make_unique<Ext4ReplayPreview<8, 2>>(
            false, true, Ext4ReplayPolicy::bounded_directory_create);
        CHECK(view->open(recovered, {0, old.size() / 512}));
        const auto found = view->lookup(2, mkdir_update.name);
        if (found) { ++complete_count; cold_directory(recovered.durable, true, false); }
        else { ++old_count; CHECK(found.status().code() == StatusCode::not_found); unchanged_allocations(old, recovered.durable); }
        preserved_seed(old, recovered.durable);
    }
    CHECK(old_count && complete_count);
}
void mounted_success(const Bytes &old, const char *output)
{
    Disk disk(old); Mount::Registry registry;
    auto key = registry.register_device(disk, 17); CHECK(key);
    auto mounted = std::make_unique<Mount>(Ext4MountedPolicy::ordered_data_dirty_noatime);
    CHECK(mounted->mount(registry, key.value(), {0, old.size() / 512}));
    clock_model::set({{1700000311, 123456789}, {311, 1}, 1, time::RealtimeOrigin::administrator_software});
    const auto before = mounted->root()->query_metadata(); CHECK(before);
    auto created = mounted->root()->create_child_retained(mkdir_update.name, mkdir_update.attributes); CHECK(created);
    const auto metadata = created.value()->query_metadata(); CHECK(metadata);
    CHECK(metadata.value().type == NodeType::directory && metadata.value().size == 1024 &&
        metadata.value().blocks == 2 && metadata.value().link_count == 2 && metadata.value().mode == 0042750);
    CHECK(mounted->root()->query_metadata().value().link_count == before.value().link_count + 1);
    CHECK(created.value()->release());
    preserved_seed(old, disk.durable); cold_directory(disk.durable, true, false);
    save(output, disk.durable);
    CHECK(mounted->root()->sync());
}
} // namespace
int main(int argc, char **argv)
{
    CHECK(argc == 4);
    std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    Bytes original((std::istreambuf_iterator<char>(input)), {});
    CHECK(original.size() == 32 * 1024 * 1024);
    std::ifstream format_input(argv[3], std::ios::binary); CHECK(format_input);
    Bytes format_image((std::istreambuf_iterator<char>(format_input)), {});
    actual_format_binding_control(format_image);
    const auto old = model_old_image(original);
    const auto success = successful_owner(old, true);
    auto zero = old; std::fill_n(zero.begin() + free_block * 1024, 1024, 0);
    static_cast<void>(successful_owner(zero, false));
    read_only_preparation_controls(old);
    prepared_refusals(old, success.prezero_reads);
    initialization_failures(old);
    later_mutation_cuts(old, success);
    mounted_success(old, argv[2]);
    std::printf("ORDERED_DIRECTORY_INITIALIZATION_PASS public_api=1 source_fixture_unchanged=1 mutation_cuts=%zu writes=%zu flushes=%zu zero_path=1 prezero_bookend=1\n",
        success.mutations.size(), success.writes, success.flushes);
}
