#include "ok/fs/ext4_orphan_snapshot.hpp"
#include "snapshot_fixture.hpp"
#include <functional>
#include <memory>

namespace
{
using Admission = Ext4OrphanSnapshotAdmission<1>;
using Registry = block::BlockSessions<1>;

void save_block(Image &image, u32 physical, const Block &bytes)
{
    CHECK(physical < image.size() / 1024);
    for (usize at = 0; at < 1024; ++at) image[usize{physical} * 1024 + at] = bytes[at];
}

Image orphan_image(const Image &original, bool populated, bool remove_names = true)
{
    MemoryDisk disk(original);
    Ext4RecoveryBinding admission;
    CHECK(admission.inspect(disk, {0, original.size() / 512}));
    CHECK(admission.info());
    Fixture fixture(original, *admission.info());
    if (populated) fixture.add(original, 12, 14);
    fixture.add(original, 14, 0);
    fixture.head(populated ? 12 : 14);
    store32(fixture.super, 96, 0x46);
    fixture.stamp_super();
    const Tables tables(original, fixture);
    DirectoryImages directories(original, fixture, tables);
    if (remove_names)
    {
        if (populated) directories.remove_target(fixture, tables, 12);
        directories.remove_target(fixture, tables, 14);
    }
    Image result = original;
    save_block(result, 1, fixture.super);
    for (usize index = 0; index < fixture.count; ++index)
        save_block(result, fixture.images[index].table_block, fixture.images[index].table);
    for (const auto &entry : directories.blocks) save_block(result, entry.physical, entry.bytes);
    return result;
}

class SnapshotDisk final : public driver::BlockDevice
{
  public:
    static constexpr u64 origin = 37;
    explicit SnapshotDisk(const Image &image) : bytes(origin * 512, 0xa5)
    { bytes.insert(bytes.end(), image.begin(), image.end()); bytes.resize(bytes.size() + 512, 0x5a); }
    driver::BlockGeometry geometry() const override
    { return {bytes.size() / 512, 512, true}; }
    Status read_blocks(u64 first, std::span<std::byte> out) override
    {
        ++reads;
        if (reads == fail_read) return Status::io_error("snapshot injected read failure");
        CHECK(first >= origin && first < origin + 65536 && out.size() == 1024);
        if (hook) hook(first - origin);
        for (usize at = 0; at < out.size(); ++at) out[at] = std::byte(bytes[first * 512 + at]);
        return {};
    }
    Status write_blocks(u64, std::span<const std::byte>) override
    { ++writes; return Status::denied("snapshot test forbids WRITE"); }
    Status flush() override { ++flushes; return Status::denied("snapshot test forbids FLUSH"); }
    Image bytes;
    usize reads{}, writes{}, flushes{}, fail_read{};
    std::function<void(u64)> hook;
};

struct Owner
{
    SnapshotDisk disk;
    Registry registry;
    Registry::Registration key{};
    std::unique_ptr<Ext4OrphanSnapshotStorage> storage = std::make_unique<Ext4OrphanSnapshotStorage>();
    std::unique_ptr<Ext4OrphanDirectoryQualifiedReclaimScratch> scratch =
        std::make_unique<Ext4OrphanDirectoryQualifiedReclaimScratch>();
    Admission admission;
    explicit Owner(const Image &image, usize limit = 65536) : disk(image)
    {
        auto registered = registry.register_device(disk, 919, limit); CHECK(registered); key = registered.value();
    }
    Status open()
    { return admission.open(registry, key, {SnapshotDisk::origin, 65536}, *storage, *scratch); }
    void no_writes() const { CHECK(disk.writes == 0 && disk.flushes == 0); }
    void rejected() const
    {
        CHECK(!admission.report().valid && admission.report().phase == Ext4OrphanSnapshotPhase::rejected);
        CHECK(registry.inspect(key).value().state == block::DeviceState::quarantined);
        no_writes();
    }
};

void stamp_super_image(Image &image)
{
    auto bytes = std::span<u8>{image}.subspan(1024, 1024);
    store32(bytes, 1020, jbd2::detail::crc32c(~u32{0}, std::span<const u8>{bytes}.first(1020)));
}

void positive(const Image &image, bool populated)
{
    auto owner = std::make_unique<Owner>(image);
    CHECK(owner->open());
    const auto report = owner->admission.report();
    CHECK(report.valid && report.identity == 919 && report.registration_generation == 1 && report.session_serial == 1);
    CHECK(report.head == (populated ? 12U : 14U) && report.chain_members == (populated ? 2U : 1U));
    CHECK(report.table_blocks >= 512 && report.directory_blocks >= 2);
    CHECK(report.verified_blocks >= 11 + report.table_blocks + report.directory_blocks);
    CHECK(owner->disk.reads > report.verified_blocks);
    Ext4OrphanDirectoryQualifiedReclaimPlan plan{};
    const auto reads = owner->disk.reads;
    CHECK(!owner->admission.plan_head(report.head, report.generation + 1, plan));
    CHECK(!plan.valid && owner->disk.reads == reads && owner->admission.report().valid);
    CHECK(owner->admission.plan_head(report.head, report.generation, plan));
    CHECK(plan.valid && plan.reclaim.inode == report.head && plan.reclaim.count == (populated ? 5U : 4U));
    CHECK(owner->disk.reads == reads + report.verified_blocks);
    Registry::Session denied;
    CHECK(!owner->registry.acquire(owner->key, {SnapshotDisk::origin, 65536}, block::SessionAccess::writable, denied));
    CHECK(!owner->admission.open(owner->registry, owner->key, {SnapshotDisk::origin, 65536}, *owner->storage, *owner->scratch));
    CHECK(owner->admission.close());
    CHECK(!owner->admission.plan_head(report.head, report.generation, plan) && !plan.valid);
    CHECK(owner->registry.inspect(owner->key).value().state == block::DeviceState::ready);
    owner->no_writes();
    std::printf("ACCEPT %s device snapshot and exact head plan\n", populated ? "populated" : "empty");
}
} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    const auto filled_original = read_image(argv[1]);
    const auto empty_original = read_image(argv[2]);
    const auto filled = orphan_image(filled_original, true);
    const auto empty = orphan_image(empty_original, false);
    positive(filled, true); positive(empty, false);
    usize rejects = 0, faults = 0;
    auto bad = [&](const char *name, Image image) {
        auto owner = std::make_unique<Owner>(image);
        CHECK(!owner->open()); owner->rejected(); ++rejects;
        std::printf("REJECT %s\n", name);
    };
    bad("still named orphan", orphan_image(filled_original, true, false));
    bad("no orphan head", filled_original);
    auto changed = filled;
    store32(std::span<u8>{changed}.subspan(1024, 1024), 96, 0x42); stamp_super_image(changed);
    bad("clean bit with orphan chain", changed);
    changed = filled; changed[1024 + 232] ^= 1;
    bad("superblock checksum", changed);
    changed = filled; store32(std::span<u8>{changed}.subspan(1024, 1024), 232, 8193); stamp_super_image(changed);
    bad("head bounds", changed);
    const u32 table = value(load(filled, 2), 8);
    changed = filled;
    auto bytes = std::span<u8>{changed}.subspan(usize{table + (12 - 1) / 4} * 1024 + slot(12), 256);
    store32(bytes, 20, 12); // Cycle is found before the checksum/ownership pass.
    bad("cycle", changed);
    changed = filled; changed[usize{table} * 1024 + 256 + 8] ^= 1;
    bad("unrelated allocated inode checksum", changed);
    changed = filled;
    const u32 journal = value(load(filled_original, table + 1), slot(8) + 60);
    auto js = load(changed, journal);
    js[28] = js[29] = js[30] = 0; js[31] = 1;
    const u32 crc = jbd2::detail::checksum_zeroed(~u32{0}, js, 252);
    for (usize at = 0; at < 4; ++at) js[252 + at] = u8(crc >> ((3 - at) * 8));
    save_block(changed, journal, js); bad("nonempty journal", changed);
    {
        auto owner = std::make_unique<Owner>(filled, 512);
        CHECK(!owner->open()); owner->rejected(); CHECK(owner->disk.reads == 0); ++rejects;
        std::puts("REJECT sub-block transfer budget");
    }
    {
        auto owner = std::make_unique<Owner>(filled);
        usize seen = 0;
        owner->disk.hook = [&](u64 sector) {
            if (sector == u64{table} * 2 && ++seen == 2)
                owner->disk.bytes[(SnapshotDisk::origin + sector) * 512 + 900] ^= 1;
        };
        CHECK(!owner->open()); owner->rejected(); ++rejects;
        std::puts("REJECT change during final reread");
    }
    // Every source family is reread by plan_head, including unused table slots
    // and raw uninitialized-group bitmap padding that semantic validation skips.
    std::vector<u32> altered{1, 2, journal, table + 511};
    const auto desc = load(filled, 2);
    for (u32 group = 0; group < 4; ++group)
    { altered.push_back(value(desc, group * 32)); altered.push_back(value(desc, group * 32 + 4)); }
    {
        auto owner = std::make_unique<Owner>(filled); CHECK(owner->open());
        for (usize at = 0; at < owner->admission.report().directory_blocks; ++at)
            altered.push_back(owner->storage->directories[at].physical);
    }
    for (u32 physical : altered)
    {
        auto owner = std::make_unique<Owner>(filled); CHECK(owner->open());
        const auto report = owner->admission.report();
        owner->disk.bytes[SnapshotDisk::origin * 512 + usize{physical} * 1024 + 900] ^= 1;
        Ext4OrphanDirectoryQualifiedReclaimPlan plan{}; plan.valid = true;
        CHECK(!owner->admission.plan_head(report.head, report.generation, plan));
        CHECK(!plan.valid); owner->rejected(); ++rejects;
    }
    std::printf("REJECT whole source families count=%zu\n", altered.size());
    {
        auto owner = std::make_unique<Owner>(filled); CHECK(owner->open());
        const auto report = owner->admission.report();
        owner->storage->inode_tables[0][511][900] ^= 1;
        Ext4OrphanDirectoryQualifiedReclaimPlan plan{};
        CHECK(!owner->admission.plan_head(report.head, report.generation, plan));
        owner->rejected(); ++rejects;
        std::puts("REJECT caller storage mutation");
    }
    for (usize fault : {1U, 2U, 8U, 20U, 100U, 520U, 600U, 1000U})
    {
        auto owner = std::make_unique<Owner>(filled); owner->disk.fail_read = fault;
        CHECK(!owner->open()); owner->rejected(); CHECK(owner->disk.reads == fault); ++faults;
    }
    for (usize fault : {1U, 2U, 5U, 100U, 512U})
    {
        auto owner = std::make_unique<Owner>(filled); CHECK(owner->open());
        const auto report = owner->admission.report(); owner->disk.fail_read = owner->disk.reads + fault;
        Ext4OrphanDirectoryQualifiedReclaimPlan plan{}; plan.valid = true;
        CHECK(!owner->admission.plan_head(report.head, report.generation, plan));
        CHECK(!plan.valid); owner->rejected(); ++faults;
    }
    std::printf("PASS read-only orphan snapshot: positives=2 rejects=%zu read_faults=%zu partition_origin=37 no_write_flush=1\n", rejects, faults);
}
