// Hosted device/crash composition over a retained Linux-created ext4 image.
// This exercises synchronous transport models, not hardware or mounted move.
#include "ext4_mounted_support.hpp"
#include "ext4_realtime_model.hpp"
#include <ok/fs/ext4/mount.hpp>
#include <ok/fs/ext4/txn.hpp>
#include <ok/fs/ext4/recovery.hpp>
#include <memory>

using namespace owned_fixture;
using Block = Ext4RegularMoveBlock;
namespace
{
usize checks{}, crash_cases{}, mixed_cases{}, refusal_cases{};
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "regular move integration:%d: %s\n", __LINE__, #expression); std::abort(); \
} } while (false)

Ext4RegularMoveUpdate request(const Bytes &bytes)
{
    return {2, 14, 12, r32(bytes, inode_offset(bytes, 2) + 100),
        r32(bytes, inode_offset(bytes, 14) + 100), r32(bytes, inode_offset(bytes, 12) + 100),
        "seed", "moved.txt", 1700000555, 123456789};
}
Block block(const Bytes &bytes, u64 number)
{
    CHECK(number < bytes.size() / 1024);
    Block result{}; std::copy_n(bytes.begin() + number * 1024, 1024, result.begin()); return result;
}
void replace(Bytes &bytes, u64 number, const Block &image)
{ std::copy(image.begin(), image.end(), bytes.begin() + number * 1024); }
struct Prepared
{
    std::array<u64, 6> homes{};
    std::array<Block, 6> after{};
    usize count{};
    u32 sequence{};
};
Prepared prepare(Disk &disk, Ext4RegularMoveTransaction &writer)
{
    const auto update = request(disk.live);
    CHECK(writer.prepare(disk, {0, disk.live.size() / 512}, update));
    CHECK(disk.writes == 0 && disk.flushes == 0);
    Prepared result{}; CHECK(writer.logical_images(result.homes, result.after, result.count));
    CHECK(result.count >= 4 && result.count <= 6);
    result.sequence = writer.report().sequence;
    CHECK(!writer.prepare(disk, {0, disk.live.size() / 512}, update));
    return result;
}
void old_homes(const Bytes &original, const Bytes &current, const Prepared &prepared)
{
    for (usize index = 1; index < prepared.count; ++index)
        CHECK(block(current, prepared.homes[index]) == block(original, prepared.homes[index]));
}
void new_homes(const Bytes &current, const Prepared &prepared)
{
    for (usize index = 1; index < prepared.count; ++index)
        CHECK(block(current, prepared.homes[index]) == prepared.after[index]);
}
// All non-journal, non-participant bytes, including every file payload and all
// allocation/orphan metadata, must remain physically unchanged.
void untouched(const Bytes &original, const Bytes &current, const Prepared &prepared)
{
    const usize first = journal(original) / 1024;
    const usize journal_count = r16(original, inode_offset(original, 8) + 56);
    for (usize number = 0; number < original.size() / 1024; ++number)
    {
        if (number >= first && number - first < journal_count) continue;
        bool participant{};
        for (usize index = 1; index < prepared.count; ++index)
            participant |= number == prepared.homes[index];
        if (!participant) CHECK(block(original, number) == block(current, number));
    }
}
void complete(Disk &disk, const Prepared &prepared)
{
    Ext4RegularMoveCompletion completion;
    const auto before_writes = disk.writes, before_flushes = disk.flushes;
    CHECK(completion.prepare(disk, {0, disk.live.size() / 512}));
    CHECK(disk.writes == before_writes && disk.flushes == before_flushes);
    CHECK(completion.execute());
    CHECK(completion.report().homes_durable && completion.report().journal_durable &&
        completion.report().physical_verified && completion.report().complete());
    CHECK(!completion.execute());
    new_homes(disk.durable, prepared);
    CHECK(be32(disk.durable, journal(disk.durable) + 28) == 0);
    CHECK(be32(disk.durable, journal(disk.durable) + 24) == prepared.sequence + 2);
    CHECK(r32(disk.durable, 1120) & 4U);
}
void verify_after(const Bytes &original, const Bytes &bytes, const Prepared &prepared)
{
    new_homes(bytes, prepared); untouched(original, bytes, prepared);
    Disk inspect(bytes); Ext4RecoveryBinding binding;
    CHECK(binding.inspect(inspect, {0, bytes.size() / 512}));
    CHECK(binding.info()->empty_log && binding.info()->needs_recovery);
    struct Reader
    {
        Disk &disk;
        Status read_block(u32 number, std::span<u8> bytes)
        { return disk.read_blocks(u64{number} * 2, std::as_writable_bytes(bytes)); }
    } reader{inspect};
    Ext4ReadOnlyView<Reader> view; CHECK(view.open(reader, *binding.info()));
    CHECK(view.lookup(2, "seed").status().code() == StatusCode::not_found);
    CHECK(view.lookup(14, "moved.txt").value() == 12);
    CHECK(view.lookup(14, "note.txt").value() == 15);
    CHECK(view.inode(12).value().generation == request(original).source_generation);
    CHECK(view.inode(12).value().size == r32(original, inode_offset(original, 12) + 4));
    CHECK(view.inode(12).value().links == 1);
}
Bytes publish(const Bytes &original, Prepared &prepared)
{
    Disk disk(original); Ext4RegularMoveTransaction writer;
    prepared = prepare(disk, writer);
    CHECK(writer.publish_prepared());
    CHECK(writer.report().publication_durable);
    old_homes(original, disk.durable, prepared); untouched(original, disk.durable, prepared);
    CHECK(block(disk.durable, prepared.homes[0]) == prepared.after[0]);
    return disk.durable;
}
void mixtures(const Bytes &original, const Bytes &published, const Prepared &prepared)
{
    for (u32 mask = 0; mask < (1U << (prepared.count - 1)); ++mask)
    {
        Bytes mixed = published;
        for (usize index = 1; index < prepared.count; ++index)
            if (mask & (1U << (index - 1))) replace(mixed, prepared.homes[index], prepared.after[index]);
        Disk disk(mixed); complete(disk, prepared); verify_after(original, disk.durable, prepared); ++mixed_cases;
    }
}
void corruption(const Bytes &published, const Prepared &prepared)
{
    const u64 first = prepared.homes[0];
    const std::array<usize, 5> offsets{usize((first + 1) * 1024 + 128),
        usize((first + 1) * 1024 + 320), usize((first + 2) * 1024 + 100),
        usize((first + prepared.count + 1) * 1024 + 16),
        usize(prepared.homes[1] * 1024 + 500)};
    for (usize offset : offsets)
    {
        Bytes damaged = published; damaged[offset] ^= 1;
        Disk disk(damaged); Ext4RegularMoveCompletion completion;
        CHECK(!completion.prepare(disk, {0, damaged.size() / 512}));
        CHECK(disk.writes == 0 && disk.flushes == 0 && disk.live == damaged); ++refusal_cases;
    }
    // A valid outer checksum cannot make an unknown family marker admissible.
    Bytes foreign = published; foreign[(first + 1) * 1024 + 128] ^= 1;
    std::array<u8, 16> uuid{}; std::copy_n(foreign.begin() + 1128, 16, uuid.begin());
    const u32 seed = jbd2::detail::crc32c(~u32{0}, uuid);
    big32(foreign, (first + 1) * 1024 + 1020, 0);
    big32(foreign, (first + 1) * 1024 + 1020,
        crc(seed, foreign, (first + 1) * 1024, 1024));
    const auto codec = jbd2::Codec::open(block(foreign, first),
        r16(foreign, inode_offset(foreign, 8) + 56), uuid, jbd2::DescriptorUuidPolicy::linux_internal_zero);
    CHECK(codec);
    CHECK(codec.value().descriptor(block(foreign, first + 1), prepared.sequence,
        [](const jbd2::Tag &) -> Status { return {}; }));
    Disk disk(foreign); Ext4RegularMoveCompletion completion;
    CHECK(!completion.prepare(disk, {0, foreign.size() / 512}));
    CHECK(disk.writes == 0 && disk.flushes == 0); ++refusal_cases;
}
void completion_snapshot_refusal(const Bytes &published, const Prepared &prepared)
{
    const u64 first = prepared.homes[0];
    const std::array<u64, 10> targets{first, first + 1, first + 2, first + prepared.count + 1,
        first + prepared.count + 2, 1, 2, r32(published, 2048),
        u64(inode_offset(published, 8) / 1024), prepared.homes[1]};
    for (u64 target : targets)
    {
        Disk disk(published); Ext4RegularMoveCompletion completion;
        CHECK(completion.prepare(disk, {0, disk.live.size() / 512}));
        disk.live[target * 1024 + 700] ^= 1;
        CHECK(!completion.execute());
        CHECK(disk.writes == 0 && disk.flushes == 0 && disk.durable == published); ++refusal_cases;
    }
}
void recovery_cuts(const Bytes &original, const Bytes &published, const Prepared &prepared)
{
    // Retrying a model power loss uses a new owner and whole physical bytes.
    // Partial home images refuse; exact old/new images remain recoverable.
    for (usize failure = 1; failure <= prepared.count; ++failure)
        for (usize prefix : {usize{0}, usize{128}, usize{512}, usize{1024}})
        {
            Disk disk(published); Ext4RegularMoveCompletion completion;
            CHECK(completion.prepare(disk, {0, disk.live.size() / 512}));
            disk.fail_write = failure; disk.cut = prefix;
            CHECK(!completion.execute()); disk.crash();
            Ext4RecoveryBinding binding;
            const auto status = binding.inspect(disk, {0, disk.live.size() / 512});
            if (!status) CHECK(disk.writes == 0 && disk.flushes == 0);
            else if (binding.info()->empty_log) verify_after(original, disk.durable, prepared);
            else
            {
                Ext4RegularMoveCompletion retry;
                const auto admitted = retry.prepare(disk, {0, disk.live.size() / 512});
                if (!admitted) { CHECK(disk.writes == 0 && disk.flushes == 0); ++refusal_cases; }
                else { CHECK(retry.execute()); verify_after(original, disk.durable, prepared); }
            }
            ++crash_cases;
        }
    for (usize failure = 1; failure <= 3; ++failure)
        for (bool after_effect : {false, true})
        {
            Disk disk(published); Ext4RegularMoveCompletion completion;
            CHECK(completion.prepare(disk, {0, disk.live.size() / 512}));
            if (after_effect) disk.fail_flush_after = failure; else disk.fail_flush = failure;
            CHECK(!completion.execute()); disk.crash();
            Ext4RecoveryBinding binding; CHECK(binding.inspect(disk, {0, disk.live.size() / 512}));
            if (binding.info()->empty_log) verify_after(original, disk.durable, prepared);
            else { complete(disk, prepared); verify_after(original, disk.durable, prepared); }
            ++crash_cases;
        }
}
void writer_preimage_refusal(const Bytes &original)
{
    Disk disk(original); Ext4RegularMoveTransaction writer;
    const auto prepared = prepare(disk, writer);
    disk.live[prepared.homes[1] * 1024 + 700] ^= 1;
    CHECK(!writer.publish_prepared());
    CHECK(disk.writes == 0 && disk.flushes == 0 && disk.durable == original); ++refusal_cases;
}
void publication_cuts(const Bytes &original, const Prepared &expected)
{
    const usize total_writes = expected.count + 3;
    for (usize failure = 1; failure <= total_writes; ++failure)
        for (usize prefix : {usize{0}, usize{128}, usize{512}, usize{1024}})
        {
            Disk disk(original); Ext4RegularMoveTransaction writer;
            const auto prepared = prepare(disk, writer);
            disk.fail_write = failure; disk.cut = prefix;
            CHECK(!writer.publish_prepared()); disk.crash();
            Ext4RecoveryBinding binding;
            const auto status = binding.inspect(disk, {0, original.size() / 512});
            if (!status)
            { CHECK(disk.writes == 0 && disk.flushes == 0); }
            else if (binding.info()->empty_log) old_homes(original, disk.durable, prepared);
            else { complete(disk, prepared); verify_after(original, disk.durable, prepared); }
            ++crash_cases;
        }
    for (usize failure = 1; failure <= 4; ++failure)
        for (bool after_effect : {false, true})
        {
            Disk disk(original); Ext4RegularMoveTransaction writer;
            const auto prepared = prepare(disk, writer);
            if (after_effect) disk.fail_flush_after = failure; else disk.fail_flush = failure;
            CHECK(!writer.publish_prepared()); disk.crash();
            Ext4RecoveryBinding binding; CHECK(binding.inspect(disk, {0, original.size() / 512}));
            if (binding.info()->empty_log) old_homes(original, disk.durable, prepared);
            else { complete(disk, prepared); verify_after(original, disk.durable, prepared); }
            ++crash_cases;
        }
}
void completed_directory_rename(const Bytes &original, const Bytes &completed, const char *output)
{
    Disk disk(completed); using Mount = Ext4MountedMetadata<1>;
    Mount::Registry registry; const auto registration = registry.register_device(disk, 251); CHECK(registration);
    auto mount = std::make_unique<Mount>(Ext4MountedPolicy::ordered_data_dirty_noatime);
    CHECK(mount->mount(registry, registration.value(), {0, completed.size() / 512}));
    ok::testing::ext4_clock::set({{1700000666, 987654321}, {666, 1}, 1,
        time::RealtimeOrigin::administrator_software});
    auto *root = mount->root(); CHECK(root);
    auto directory = root->lookup_child_retained("archived-documents"); CHECK(directory);
    auto file = directory.value()->lookup_child_retained("moved.txt"); CHECK(file);
    const auto old_name = directory.value()->name();
    const auto before = file.value()->query_metadata(); CHECK(before);
    CHECK(root->rename("archived-documents", *root, "files-archive"));
    CHECK(old_name == "archived-documents");
    CHECK(directory.value()->name() == "files-archive");
    CHECK(file.value()->query_metadata().value().attributes.inode == 12);
    CHECK(root->lookup_child("archived-documents").status().code() == StatusCode::not_found);
    CHECK(root->lookup_child("files-archive").value() == directory.value());
    CHECK(directory.value()->lookup_child("moved.txt").value() == file.value());
    CHECK(directory.value()->directory_path(*root).value().view() == "/files-archive");
    const auto payload = file.value()->read(0, 256); CHECK(payload);
    const usize data = r32(original, inode_offset(original, 12) + 60) * 1024;
    CHECK(payload.value().size == 256);
    CHECK(std::equal(payload.value().data.begin(), payload.value().data.begin() + 256,
        reinterpret_cast<const std::byte *>(original.data() + data)));
    CHECK(file.value()->release() && directory.value()->release());
    CHECK(root->sync()); save(output, disk.durable);
}
}

int main(int argc, char **argv)
{
    CHECK(argc == 4);
    std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    const Bytes original((std::istreambuf_iterator<char>(input)), {});
    CHECK(original.size() == 32 * 1024 * 1024);
    CHECK(r16(original, inode_offset(original, 14)) == 0040755);
    CHECK(r32(original, inode_offset(original, 12) + 4) == 4096);
    Prepared prepared; const auto published = publish(original, prepared);
    mixtures(original, published, prepared); corruption(published, prepared);
    writer_preimage_refusal(original); publication_cuts(original, prepared);
    completion_snapshot_refusal(published, prepared); recovery_cuts(original, published, prepared);
    Disk disk(published); complete(disk, prepared); verify_after(original, disk.durable, prepared);
    save(argv[2], disk.durable);
    completed_directory_rename(original, disk.durable, argv[3]);
    std::printf("EXT4_REGULAR_MOVE_INTEGRATION_MODEL_PASS checks=%zu crash_cases=%zu mixed_cases=%zu refusal_cases=%zu\n",
        checks, crash_cases, mixed_cases, refusal_cases);
}
