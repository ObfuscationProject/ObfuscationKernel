// Actual hosted capture and canonical Owner/Session, distinct from a guest.
#include "ext4_owned_support.hpp"
#include <ok/fs/ext4/mount.hpp>
#include <memory>

using namespace ok;
using namespace ok::fs;
using namespace owned_fixture;
static unsigned checks, rejects, captures, recoveries;
#define CHECK(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "FAIL nonmember:%d: %s\n", __LINE__, #value); std::abort(); } } while (false)
static void checked(Status value, const char *label)
{
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL %s: %.*s\n", label, int(value.message().size()), value.message().data()); std::abort(); }
}
using Registry = block::BlockSessions<1>;
using Recovery = Ext4EmptyOrphanRecoveryOwner<1>;
static constexpr u64 origin = 11;

static Bytes load(const char *path)
{
    std::ifstream input(path, std::ios::binary); CHECK(input);
    Bytes bytes((std::istreambuf_iterator<char>(input)), {});
    CHECK(bytes.size() == 32 * 1024 * 1024); return bytes;
}
static bool contains(Status value, std::string_view expected)
{ return value.message().find(expected) != std::string_view::npos; }

struct CaptureHarness
{
    Disk disk;
    Registry registry;
    Registry::Session session;
    Registry::Registration key{};
    std::unique_ptr<Ext4OrphanChainCapture<1>> capture = std::make_unique<Ext4OrphanChainCapture<1>>();
    std::unique_ptr<Ext4OrphanChainCaptureStorage> storage = std::make_unique<Ext4OrphanChainCaptureStorage>();
    std::unique_ptr<Ext4OrphanDirectoryQualifiedReclaimScratch> scratch = std::make_unique<Ext4OrphanDirectoryQualifiedReclaimScratch>();
    explicit CaptureHarness(const Bytes &image) : disk(image)
    {
        disk.origin = origin;
        auto registration = registry.register_device(disk, 611, 1024); CHECK(registration); key = registration.value();
        checked(registry.acquire(key, {origin, 65536}, block::SessionAccess::writable, session), "capture Session");
    }
    Status run() { return capture->capture(session, *storage, *scratch); }
};
struct OwnerHarness
{
    Disk disk;
    Registry registry;
    Registry::Registration key{};
    std::unique_ptr<Ext4EmptyOrphanRecoveryWorkspace> workspace = std::make_unique<Ext4EmptyOrphanRecoveryWorkspace>();
    std::unique_ptr<Recovery> owner = std::make_unique<Recovery>();
    explicit OwnerHarness(const Bytes &image) : disk(image)
    {
        disk.origin = origin;
        auto registration = registry.register_device(disk, 612, 1024); CHECK(registration); key = registration.value();
        checked(owner->open(registry, key, {origin, 65536}, *workspace), "fresh Owner");
    }
};
static void capture_success(const Bytes &image)
{
    auto h = std::make_unique<CaptureHarness>(image);
    checked(h->run(), "whole original capture");
    const auto &report = h->capture->report();
    CHECK(report.valid && report.phase == Ext4OrphanSnapshotPhase::admitted &&
          report.head == 20 && report.chain_members == 1 && report.directory_blocks == 15);
    auto plan = std::make_unique<Ext4OrphanDirectoryQualifiedReclaimPlan>();
    checked(h->capture->plan_head(report.head, report.generation, *plan), "qualified head plan");
    CHECK(plan->valid && plan->directory.valid && plan->reclaim.inode == 20 &&
          plan->reclaim.released_blocks == 1);
    CHECK(!h->disk.writes && !h->disk.flushes && h->disk.live == image && h->disk.durable == image);
    checked(h->session.close(), "readonly capture close"); ++captures;
}
static void recovery_success(const Bytes &image, const std::filesystem::path &path)
{
    auto h = std::make_unique<OwnerHarness>(image);
    const auto before = h->owner->report().original;
    checked(h->owner->recover(), "actual original Owner reclaim");
    const auto report = h->owner->report();
    CHECK(report.complete && report.initial_head == 20 && report.chain_members == 1 &&
          report.reclaimed == 1 && report.released_blocks == 1 && report.verified_publications == 3);
    CHECK(report.original.identity == report.observed.identity &&
          report.original.registration_generation == report.observed.registration_generation &&
          before.session_serial == report.observed.session_serial && !report.observed.pending_writes && !report.observed.in_flight);
    CHECK(report.observed.completed_reads == h->disk.reads &&
          report.observed.completed_writes == h->disk.writes && report.observed.completed_flushes == h->disk.flushes);
    CHECK(h->disk.writes && h->disk.flushes && h->disk.pending.empty() && h->disk.live == h->disk.durable);
    CHECK(r32(h->disk.durable, 1256) == 0 && be32(h->disk.durable, journal(image) + 28) == 0);
    CHECK(be32(h->disk.durable, journal(image) + 24) == be32(image, journal(image) + 24) + 3);
    const usize orphan = inode_offset(image, 20);
    const usize bitmap = r32(image, descriptor(image, 0) + 4) * 1024;
    CHECK(!(h->disk.durable[bitmap + 19 / 8] & (1U << (19 % 8))));
    CHECK(!r16(h->disk.durable, orphan) && r32(h->disk.durable, orphan + 100) == r32(image, orphan + 100) + 1);
    // These linked nonmembers are checked for ownership, never rewritten.
    for (u32 number : {12U, 15U, 16U, 17U, 18U, 19U})
    {
        const auto at = inode_offset(image, number);
        CHECK(std::equal(image.begin() + at, image.begin() + at + 256, h->disk.durable.begin() + at));
    }
    std::vector<bool> changed(image.size() / 1024);
    for (usize at = 0; at < 9; ++at) changed[journal(image) / 1024 + at] = true;
    changed[1] = changed[2] = changed[orphan / 1024] = true;
    changed[r32(image, descriptor(image, 0))] = changed[r32(image, descriptor(image, 0) + 4)] = true;
    for (usize block = 0; block < changed.size(); ++block)
        if (!changed[block]) CHECK(std::equal(image.begin() + block * 1024, image.begin() + (block + 1) * 1024,
                                            h->disk.durable.begin() + block * 1024));
    checked(h->owner->close(), "completed Owner close");
    CHECK(h->owner->report().phase == Ext4EmptyOrphanRecoveryPhase::closed && h->owner->report().complete);
    CHECK(h->registry.inspect(h->key).value().state == block::DeviceState::ready);
    save(path, h->disk.durable); ++recoveries;
    std::printf("NONMEMBER_RECOVERY_PASS reads=%zu writes=%zu flushes=%zu head=20 released=1\n", h->disk.reads, h->disk.writes, h->disk.flushes);
}
static void refused(const Bytes &image, std::string_view expected, const char *label)
{
    {
        auto h = std::make_unique<CaptureHarness>(image);
        const auto result = h->run();
        CHECK(!result && contains(result, expected));
        CHECK(!h->capture->report().valid && h->registry.inspect(h->key).value().state == block::DeviceState::quarantined);
        CHECK(!h->disk.writes && !h->disk.flushes && h->disk.live == image && h->disk.durable == image);
    }
    {
        auto h = std::make_unique<OwnerHarness>(image);
        const auto result = h->owner->recover();
        CHECK(!result && contains(result, expected));
        CHECK(h->owner->report().phase == Ext4EmptyOrphanRecoveryPhase::failed && !h->owner->report().complete);
        CHECK(h->registry.inspect(h->key).value().state == block::DeviceState::quarantined);
        CHECK(!h->disk.writes && !h->disk.flushes && h->disk.live == image && h->disk.durable == image);
        const auto reads = h->disk.reads;
        CHECK(!h->owner->recover() && !h->owner->close() && h->disk.reads == reads && !h->disk.writes && !h->disk.flushes);
    }
    ++rejects; std::printf("NONMEMBER_REJECT_PASS case=%s zero_write_flush=1\n", label);
}
static void clear_allocated_block(Bytes &image, u32 physical)
{
    const u32 group = (physical - 1) / 8192, local = (physical - 1) % 8192;
    const auto at = descriptor(image, group);
    const usize bitmap = r32(image, at) * 1024;
    CHECK(image[bitmap + local / 8] & (1U << (local % 8)));
    image[bitmap + local / 8] &= u8(~(1U << (local % 8)));
    w16(image, at + 12, r16(image, at + 12) + 1);
    w16(image, at + 24, u16(crc(seed(image), image, bitmap, 1024)));
    group_crc(image, group, at);
    w32(image, 1036, r32(image, 1036) + 1); super_crc(image);
}
static void directory_type(Bytes &image, u32 target, u8 replacement)
{
    const auto at = r32(image, inode_offset(image, 2) + 60) * 1024;
    for (usize offset = 0; offset < 1012; offset += r16(image, at + offset + 4))
        if (r32(image, at + offset) == target)
        {
            image[at + offset + 7] = replacement; directory_crc(image, 2, at); return;
        }
    CHECK(false);
}
static void negatives(const Bytes &image)
{
    const usize init = inode_offset(image, 16), link = inode_offset(image, 15);
    const auto regular = [&](usize offset, u32 value, const char *label, const char *expected) {
        auto changed = image; w32(changed, init + offset, value); inode_crc(changed, 16, init); refused(changed, expected, label);
    };
    // A checksum-valid second large extent aliases the first, with all involved
    // bitmap bits still allocated. The owner-overlap check must reject it.
    regular(72, r32(image, init + 60) + 2, "large-overlap", "physical extents overlap");
    auto changed = image; clear_allocated_block(changed, r32(image, init + 72));
    refused(changed, "block is unallocated", "large-unallocated-valid-bitmap-checksums");
    regular(72, 32767, "large-out-of-bounds", "extent bounds");
    regular(28, r32(image, init + 28) + 2, "large-sector-mismatch", "mapped-sector coverage");
    regular(4, 32 * 1024 * 1024 + 1, "large-size-outside-geometry", "ordinary inode profile");
    regular(32, 0x180000, "large-unknown-flags", "ordinary inode profile");
    changed = image; w16(changed, init + 46, 1); inode_crc(changed, 16, init);
    refused(changed, "ordinary inode profile", "large-extent-depth");
    changed = image; w16(changed, init + 70, 1); inode_crc(changed, 16, init);
    refused(changed, "unsupported inline extent", "large-high-physical-field");
    changed = image; w32(changed, inode_offset(changed, 20) + 4, 65537);
    inode_crc(changed, 20, inode_offset(changed, 20)); refused(changed, "orphan", "orphan-member-size-limit-unchanged");
    changed = image; w32(changed, inode_offset(changed, 17) + 4, 65537);
    inode_crc(changed, 17, inode_offset(changed, 17)); refused(changed, "ordinary inode profile", "directory-size-limit-unchanged");
    changed = image; changed[link + 124] ^= 1; refused(changed, "inode checksum", "symlink-checksum");
    changed = image; w32(changed, link + 100, 1); refused(changed, "inode checksum", "symlink-generation-seed-mismatch");
    const auto symlink = [&](usize offset, u32 value, const char *label) {
        auto bad = image; w32(bad, link + offset, value); inode_crc(bad, 15, link);
        refused(bad, "fast symlink", label);
    };
    symlink(28, 2, "symlink-blockcount"); symlink(104, 1, "symlink-ACL");
    symlink(108, 1, "symlink-size-high"); symlink(112, 1, "symlink-fragment-address");
    symlink(32, 0x80000, "symlink-unknown-flags"); symlink(4, 60, "symlink-long-target");
    symlink(20, 20, "symlink-orphan-next"); symlink(156, 1, "symlink-project-field");
    symlink(160, 1, "symlink-extra-field");
    changed = image; w16(changed, link + 26, 0); inode_crc(changed, 15, link);
    refused(changed, "fast symlink", "symlink-unlinked");
    changed = image; w16(changed, link + 116, 1); inode_crc(changed, 15, link);
    refused(changed, "fast symlink", "symlink-blockcount-high");
    changed = image; w16(changed, link + 118, 1); inode_crc(changed, 15, link);
    refused(changed, "fast symlink", "symlink-ACL-high");
    changed = image; changed[link + 44] = 'x'; inode_crc(changed, 15, link);
    refused(changed, "unused target bytes", "symlink-trailing-target-byte");
    changed = image; changed[link + 41] = 0; inode_crc(changed, 15, link);
    refused(changed, "target contains zero", "symlink-embedded-zero");
    changed = image; directory_type(changed, 15, 1);
    refused(changed, "name and inode type differ", "symlink-directory-type-drift");
    changed = image; directory_type(changed, 16, 7);
    refused(changed, "name and inode type differ", "regular-directory-symlink-type-drift");
}
int main(int argc, char **argv)
{
    CHECK(argc == 3); const auto image = load(argv[1]);
    CHECK(r32(image, 1256) == 20 && r32(image, inode_offset(image, 16) + 4) == 252200 &&
          r16(image, inode_offset(image, 16) + 68) == 211 &&
          (r16(image, inode_offset(image, 15)) & 0170000) == 0120000);
    if (std::string_view(argv[2]) == "--predecessor")
    {
        refused(image, "ordinary inode profile", "exact-failed-Notes-predecessor");
        std::printf("PASS predecessor profile refusal: checks=%u zero_write_flush=1\n", checks); return 0;
    }
    const std::filesystem::path output = argv[2]; std::filesystem::create_directory(output);
    capture_success(image); recovery_success(image, output / "reclaimed-exact-failed.img");
    // Generation participates in the checksum identity but need not be zero for
    // a linked nonmember. This input remains outside all orphan authority.
    auto changed = image; const auto link = inode_offset(image, 15);
    w32(changed, link + 100, 7); inode_crc(changed, 15, link);
    capture_success(changed); recovery_success(changed, output / "reclaimed-symlink-generation.img");
    negatives(image);
    std::printf("PASS nonmember profiles: checks=%u captures=%u recoveries=%u rejects=%u exact-source=failed-Notes large-linked-inline=1 fast-symlink=1 orphan-limits-unchanged=1\n",
                checks, captures, recoveries, rejects);
}
