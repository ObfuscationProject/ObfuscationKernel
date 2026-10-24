#include "ok/fs/ext4_recovery.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <vector>

extern "C" unsigned int ext2fs_crc32c_le(unsigned int, const unsigned char *, size_t);
using namespace ok;
using namespace ok::fs;
namespace
{
[[noreturn]] void fail(const char *text)
{
    std::fprintf(stderr, "FAIL hosted ext4 recovery binding: %s\n", text);
    std::exit(1);
}
void check(bool condition, const char *text)
{
    if (!condition)
        fail(text);
}
u16 r16(const std::vector<u8> &bytes, usize offset)
{
    return u16(bytes[offset]) | (u16(bytes[offset + 1]) << 8);
}
u32 r32(const std::vector<u8> &bytes, usize offset)
{
    return u32(r16(bytes, offset)) | (u32(r16(bytes, offset + 2)) << 16);
}
void w16(std::vector<u8> &bytes, usize offset, u16 value)
{
    bytes[offset] = u8(value);
    bytes[offset + 1] = u8(value >> 8);
}
void w32(std::vector<u8> &bytes, usize offset, u32 value)
{
    w16(bytes, offset, u16(value));
    w16(bytes, offset + 2, u16(value >> 16));
}
void big32(std::vector<u8> &bytes, usize offset, u32 value)
{
    bytes[offset] = u8(value >> 24);
    bytes[offset + 1] = u8(value >> 16);
    bytes[offset + 2] = u8(value >> 8);
    bytes[offset + 3] = u8(value);
}
u32 crc(u32 seed, const std::vector<u8> &bytes, usize offset, usize count)
{
    return ext2fs_crc32c_le(seed, bytes.data() + offset, count);
}
struct Layout
{
    u32 block_size, groups, blocks_per_group, inodes_per_group, first_data;
    usize descriptors, inode, journal;
    u32 journal_blocks;
};
Layout layout(const std::vector<u8> &bytes)
{
    Layout result{};
    result.block_size = 1024U << r32(bytes, 1048);
    result.first_data = r32(bytes, 1044);
    result.blocks_per_group = r32(bytes, 1056);
    result.inodes_per_group = r32(bytes, 1064);
    result.groups = (r32(bytes, 1028) - result.first_data - 1) / result.blocks_per_group + 1;
    result.descriptors = (result.first_data + 1) * result.block_size;
    result.inode = r32(bytes, result.descriptors + 8) * result.block_size + 7 * 256;
    result.journal = r32(bytes, result.inode + 60) * result.block_size;
    result.journal_blocks = r16(bytes, result.inode + 56);
    return result;
}
void super_crc(std::vector<u8> &bytes)
{
    w32(bytes, 2044, crc(~u32{0}, bytes, 1024, 1020));
}
void group_crc(std::vector<u8> &bytes, const Layout &l, u32 group = 0)
{
    const auto descriptor = l.descriptors + group * 32;
    w16(bytes, descriptor + 30, 0);
    const std::array<u8, 4> number{u8(group), u8(group >> 8), u8(group >> 16), u8(group >> 24)};
    auto seed = crc(~u32{0}, bytes, 1128, 16);
    seed = ext2fs_crc32c_le(seed, number.data(), number.size());
    w16(bytes, descriptor + 30, u16(crc(seed, bytes, descriptor, 32)));
}
void inode_crc(std::vector<u8> &bytes, const Layout &l)
{
    w16(bytes, l.inode + 124, 0);
    w16(bytes, l.inode + 130, 0);
    const std::array<u8, 4> number{8, 0, 0, 0};
    auto seed = crc(~u32{0}, bytes, 1128, 16);
    seed = ext2fs_crc32c_le(seed, number.data(), number.size());
    seed = crc(seed, bytes, l.inode + 100, 4);
    const auto sum = crc(seed, bytes, l.inode, 256);
    w16(bytes, l.inode + 124, u16(sum));
    w16(bytes, l.inode + 130, u16(sum >> 16));
}
void bitmap_crc(std::vector<u8> &bytes, const Layout &l, u32 group, bool inode)
{
    const auto descriptor = l.descriptors + group * 32;
    const auto block = r32(bytes, descriptor + (inode ? 4 : 0));
    const auto sum = crc(crc(~u32{0}, bytes, 1128, 16), bytes, block * l.block_size,
                         (inode ? l.inodes_per_group : l.blocks_per_group) / 8);
    w16(bytes, descriptor + (inode ? 26 : 24), u16(sum));
    group_crc(bytes, l, group);
}
void journal_crc(std::vector<u8> &bytes, const Layout &l)
{
    big32(bytes, l.journal + 252, 0);
    big32(bytes, l.journal + 252, crc(~u32{0}, bytes, l.journal, 1024));
}
struct Device final : driver::BlockDevice
{
    explicit Device(const std::vector<u8> &source) : bytes(source) {}
    std::vector<u8> bytes;
    u64 first{};
    u32 sector_size{512};
    usize reads{}, writes{}, flushes{}, fail_read{}, change_after{};
    bool writable{}, change_writable{};
    driver::BlockGeometry geometry() const override
    {
        const bool changed = change_after && reads >= change_after;
        return {first + u64(bytes.size() / 512) - (changed && !change_writable ? 1 : 0), sector_size,
                changed && change_writable ? !writable : writable};
    }
    Status read_blocks(u64 sector, std::span<std::byte> out) override
    {
        ++reads;
        if (reads == fail_read)
            return Status::io_error("injected read failure");
        if (out.size() % 512 || sector < first || sector - first >= bytes.size() / 512 ||
            out.size() / 512 > bytes.size() / 512 - (sector - first))
            return Status::io_error("device read bounds");
        for (usize index = 0; index < out.size(); ++index)
            out[index] = std::byte(bytes[usize(sector - first) * 512 + index]);
        return Status::success();
    }
    Status write_blocks(u64, std::span<const std::byte>) override
    {
        ++writes;
        return Status::io_error("admission attempted a write");
    }
    Status flush() override
    {
        ++flushes;
        return Status::io_error("admission attempted a flush");
    }
    void unchanged() const { check(!writes && !flushes, "admission issued I/O mutations"); }
};
void reject(const std::vector<u8> &bytes, const char *label, const char *reason, usize &cases)
{
    Device device(bytes);
    Ext4RecoveryBinding binding;
    const auto result = binding.inspect(device, {0, device.bytes.size() / 512});
    if (result || binding.info() || result.message().find(reason) == std::string_view::npos)
    {
        std::fprintf(stderr, "case=%s code=%u reason=%.*s\n", label, u32(result.code()),
                     int(result.message().size()), result.message().data());
        fail("corrupted image did not reach expected rejection");
    }
    check(binding.inspect(device, {0, device.bytes.size() / 512}).code() == StatusCode::already_exists,
          "failed binding retried");
    device.unchanged();
    ++cases;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    std::ifstream file(argv[1], std::ios::binary);
    check(bool(file), "open fixture");
    std::vector<u8> original((std::istreambuf_iterator<char>(file)), {});
    check(original.size() >= 1024 * 1024, "fixture too small");
    const auto l = layout(original);
    usize cases = 0;
    Device clean(original);
    Ext4RecoveryBinding admitted;
    const auto success = admitted.inspect(clean, {0, original.size() / 512});
    if (!success)
    {
        std::fprintf(stderr, "initial code=%u reason=%.*s\n", u32(success.code()),
                     int(success.message().size()), success.message().data());
        fail("reference image rejected");
    }
    const auto *facts = admitted.info();
    check(facts && facts->layout.filesystem_blocks == r32(original, 1028) &&
          facts->layout.block_size == l.block_size && facts->layout.journal_first_block * l.block_size == l.journal &&
          facts->layout.journal_blocks == l.journal_blocks && facts->metadata_count == l.groups * 4 &&
          facts->journal_inode == 8 && facts->structural_metadata().valid(), "derived mapping mismatch");
    check(facts->needs_recovery == bool(r32(original, 1120) & 4), "recovery flag mismatch");
    check(admitted.inspect(clean, {0, original.size() / 512}).code() == StatusCode::already_exists,
          "successful binding retried");
    clean.unchanged();
    const usize reads = clean.reads;
    std::printf("MAPPING block_size=%u journal_first=%llu journal_blocks=%u groups=%u needs_recovery=%u empty=%u reads=%zu\n",
                l.block_size, static_cast<unsigned long long>(facts->layout.journal_first_block),
                facts->layout.journal_blocks, l.groups, facts->needs_recovery, facts->empty_log, reads);
    for (bool writable : {false, true})
    {
        Device disk(original);
        disk.writable = writable;
        disk.bytes.insert(disk.bytes.begin(), 13 * 512, 0xa5);
        disk.bytes.insert(disk.bytes.end(), 7 * 512, 0x5a);
        Ext4RecoveryBinding binding;
        check(bool(binding.inspect(disk, {13, original.size() / 512})), "partition offset admission");
        check(binding.info()->layout.first_sector == 13, "partition origin lost");
        disk.unchanged();
        ++cases;
    }
    {
        Device high(original);
        high.first = u64{1} << 34;
        Ext4RecoveryBinding binding;
        check(bool(binding.inspect(high, {high.first, original.size() / 512})) &&
              binding.info()->layout.first_sector == high.first, "u64 device origin truncated");
        high.unchanged();
        ++cases;
    }
    {
        Device wrong(original);
        wrong.sector_size = 4096;
        Ext4RecoveryBinding binding;
        check(binding.inspect(wrong, {0, original.size() / 512}).code() == StatusCode::unsupported &&
              !binding.info() && !wrong.reads, "unsupported sector size issued a read");
        wrong.unchanged();
        ++cases;
    }
    for (usize point = 1; point <= reads; ++point)
    {
        for (int failure = 0; failure != 3; ++failure)
        {
            Device disk(original);
            if (!failure) disk.fail_read = point;
            else { disk.change_after = point; disk.change_writable = failure == 2; }
            Ext4RecoveryBinding binding;
            const auto status = binding.inspect(disk, {0, original.size() / 512});
            check(!status && status.code() == StatusCode::io_error && !binding.info(), "injected I/O failure admitted");
            disk.unchanged();
            ++cases;
        }
    }
    for (const auto bounds : std::array<Ext4RecoveryBounds, 5>{{{0, 0}, {0, 3},
            {u64(original.size() / 512), 4}, {0, u64(original.size() / 512 + 1)},
            {std::numeric_limits<u64>::max() - 1, 10}}})
    {
        Device disk(original);
        Ext4RecoveryBinding binding;
        check(!binding.inspect(disk, bounds) && !binding.info() && !disk.reads, "bounds accepted or read before bounds");
        disk.unchanged();
        ++cases;
    }
    const auto mutation = [&](const char *label, const char *reason, const auto &change) {
        auto damaged = original;
        change(damaged);
        reject(damaged, label, reason, cases);
    };
    const auto super = [&](const char *label, usize offset, u32 value, const char *reason) {
        mutation(label, reason, [&](auto &b) { w32(b, 1024 + offset, value); super_crc(b); });
    };
    mutation("superblock checksum", "superblock checksum", [&](auto &b) { b[2044] ^= 1; });
    super("zero groups", 32, 0, "filesystem geometry");
    super("zero inodes per group", 40, 0, "filesystem geometry");
    super("group capacity", 32, 8, "filesystem geometry"); // Cluster count must agree before capacity admission.
    mutation("admitted geometry exceeds group capacity", "group capacity", [&](auto &b) {
        w32(b, 1056, 8); w32(b, 1060, 8); super_crc(b);
    });
    super("inode groups mismatch", 0, r32(original, 1024) - 1, "inode group geometry");
    super("filesystem beyond device", 4, u32(original.size() / l.block_size + 1), "filesystem geometry");
    super("external journal device", 228, 1, "internal journal");
    super("wrong journal inode", 224, 9, "internal journal");
    super("orphan head", 232, 12, "orphan chain");
    super("bigalloc feature", 100, 0x600, "feature or descriptor");
    super("64bit feature", 96, 0xc2, "feature or descriptor");
    super("checksum seed feature", 96, 0x2042, "feature or descriptor");
    super("foreign creator", 72, 1, "superblock format");
    mutation("external journal UUID", "internal journal", [&](auto &b) { b[1232] ^= 1; super_crc(b); });
    mutation("backup superblock identity", "feature or descriptor", [&](auto &b) { w16(b, 1114, 1); super_crc(b); });
    mutation("error state", "error state", [&](auto &b) { w16(b, 1082, 3); super_crc(b); });
    mutation("descriptor checksum", "descriptor checksum", [&](auto &b) { b[l.descriptors + 30] ^= 1; });
    mutation("descriptor overlap", "metadata overlap", [&](auto &b) {
        w32(b, l.descriptors, r32(b, l.descriptors + 8)); group_crc(b, l);
    });
    mutation("descriptor outside group", "group placement", [&](auto &b) {
        w32(b, l.descriptors, l.first_data + l.blocks_per_group); group_crc(b, l);
    });
    mutation("uninitialized bitmap", "initialization", [&](auto &b) {
        w16(b, l.descriptors + 18, 1); group_crc(b, l);
    });
    mutation("journal in an implicit free block domain", "uninitialized block group", [&](auto &b) {
        const auto first = u32(l.journal / l.block_size), group = (first - l.first_data) / l.blocks_per_group;
        const auto descriptor = l.descriptors + group * 32;
        const auto begin = l.first_data + group * l.blocks_per_group;
        const auto blocks = r32(b, 1028) - begin < l.blocks_per_group ? r32(b, 1028) - begin : l.blocks_per_group;
        w16(b, descriptor + 18, r16(b, descriptor + 18) | 2);
        w16(b, descriptor + 12, u16(blocks - l.inodes_per_group * 256 / l.block_size - 4));
        w16(b, descriptor + 24, 0); group_crc(b, l, group);
    });
    mutation("journal in an implicit free inode domain", "uninitialized inode group", [&](auto &b) {
        w16(b, l.descriptors + 18, r16(b, l.descriptors + 18) | 1);
        w16(b, l.descriptors + 14, u16(l.inodes_per_group));
        w16(b, l.descriptors + 16, 0); w16(b, l.descriptors + 28, u16(l.inodes_per_group));
        w16(b, l.descriptors + 26, 0); group_crc(b, l);
    });
    mutation("inode bitmap checksum", "inode bitmap checksum", [&](auto &b) {
        b[r32(b, l.descriptors + 4) * l.block_size] ^= 1;
    });
    mutation("block bitmap checksum", "block bitmap checksum", [&](auto &b) {
        b[r32(b, l.descriptors) * l.block_size] ^= 1;
    });
    mutation("journal inode unallocated", "journal inode is not allocated", [&](auto &b) {
        b[r32(b, l.descriptors + 4) * l.block_size] &= 0x7f; bitmap_crc(b, l, 0, true);
    });
    mutation("structural metadata unallocated", "structural metadata is not allocated", [&](auto &b) {
        b[r32(b, l.descriptors) * l.block_size] &= 0xfe; bitmap_crc(b, l, 0, false);
    });
    mutation("journal block unallocated", "journal block is not allocated", [&](auto &b) {
        const auto first = u32(l.journal / l.block_size), group = (first - l.first_data) / l.blocks_per_group;
        const auto bit = (first - l.first_data) % l.blocks_per_group;
        b[r32(b, l.descriptors + group * 32) * l.block_size + bit / 8] &= u8(~(1U << (bit % 8)));
        bitmap_crc(b, l, group, false);
    });
    mutation("inode checksum", "journal inode checksum", [&](auto &b) { b[l.inode + 124] ^= 1; });
    const auto inode = [&](const char *label, usize at, u32 value, const char *reason) {
        mutation(label, reason, [&](auto &b) { w32(b, l.inode + at, value); inode_crc(b, l); });
    };
    inode("inode flags", 32, 0, "inode identity");
    inode("inode high size", 108, 1, "inode identity");
    inode("inode ACL", 104, 1, "inode identity");
    inode("inode extra size", 128, 16, "inode extra size");
    inode("extent logical start", 52, 1, "one initialized");
    inode("extent high physical", 56, 0x10000 | l.journal_blocks, "one initialized");
    inode("unwritten extent", 56, 0x8001, "one initialized");
    inode("journal size mismatch", 4, l.journal_blocks * l.block_size - 1, "one initialized");
    inode("journal blocks mismatch", 28, l.journal_blocks * (l.block_size / 512) - 1, "one initialized");
    inode("journal extent outside device", 60, r32(original, 1028), "extent bounds");
    inode("journal extent in metadata", 60, l.first_data, "overlaps structural");
    mutation("journal depth", "one initialized", [&](auto &b) { w16(b, l.inode + 46, 1); inode_crc(b, l); });
    mutation("journal link count", "inode identity", [&](auto &b) { w16(b, l.inode + 26, 0); inode_crc(b, l); });
    mutation("journal superblock checksum", "JBD2 superblock checksum", [&](auto &b) { b[l.journal + 252] ^= 1; });
    mutation("journal UUID", "JBD2 journal UUID mismatch", [&](auto &b) { b[l.journal + 48] ^= 1; journal_crc(b, l); });
    mutation("journal block size", "differs from inode", [&](auto &b) {
        big32(b, l.journal + 12, l.block_size == 1024 ? 2048 : 1024); journal_crc(b, l);
    });
    mutation("journal mapped length", "differs from inode", [&](auto &b) {
        if (l.journal_blocks > 1024)
        {
            big32(b, l.journal + 16, l.journal_blocks - 1); journal_crc(b, l);
        }
        else
        {
            w16(b, l.inode + 56, u16(l.journal_blocks + 1));
            w32(b, l.inode + 4, (l.journal_blocks + 1) * l.block_size);
            w32(b, l.inode + 28, (l.journal_blocks + 1) * (l.block_size / 512));
            inode_crc(b, l);
        }
    });
    mutation("journal recorded error", "recorded journal error", [&](auto &b) {
        big32(b, l.journal + 32, 5); journal_crc(b, l);
    });
    mutation("shared journal", "shared, dynamic", [&](auto &b) {
        big32(b, l.journal + 64, 2); journal_crc(b, l);
    });
    mutation("journal async commit", "only synchronous", [&](auto &b) {
        big32(b, l.journal + 40, 20); journal_crc(b, l);
    });
    mutation("nonempty journal without recovery flag", "clean flag", [&](auto &b) {
        w32(b, 1120, 0x42); super_crc(b); big32(b, l.journal + 28, 1); journal_crc(b, l);
    });
    mutation("missing journal checksums", "only synchronous", [&](auto &b) {
        big32(b, l.journal + 40, 0); journal_crc(b, l);
    });
    // Descriptor flags, not the meaningless physical bytes, define UNINIT maps.
    auto implicit = original;
    usize implicit_count = 0;
    for (u32 group = 0; group < l.groups; ++group)
    {
        const auto descriptor = l.descriptors + group * 32;
        for (const bool inode_bitmap : {false, true})
        {
            if (!(r16(implicit, descriptor + 18) & (inode_bitmap ? 1 : 2)))
                continue;
            const auto first = r32(implicit, descriptor + (inode_bitmap ? 4 : 0)) * l.block_size;
            for (u32 index = 0; index < l.block_size; ++index)
                implicit[first + index] = u8(index * 29 + 17);
            ++implicit_count;
        }
    }
    Device poison(implicit);
    Ext4RecoveryBinding ignored;
    check(bool(ignored.inspect(poison, {0, implicit.size() / 512})) &&
          ignored.info()->metadata_count == facts->metadata_count, "UNINIT physical bytes treated as allocated metadata");
    poison.unchanged();
    std::printf("UNINIT poisoned_bitmaps=%zu metadata_ranges=%zu\n", implicit_count, ignored.info()->metadata_count);
    ++cases;
    // Interrupted completion may leave needs_recovery set with an empty log.
    auto empty_dirty = original;
    w32(empty_dirty, 1120, 0x46); super_crc(empty_dirty);
    big32(empty_dirty, l.journal + 28, 0); journal_crc(empty_dirty, l);
    Device recover(empty_dirty);
    Ext4RecoveryBinding restart;
    check(bool(restart.inspect(recover, {0, empty_dirty.size() / 512})) && restart.info()->needs_recovery &&
          restart.info()->empty_log, "dirty empty log should remain inspectable");
    recover.unchanged();
    ++cases;
    std::printf("PASS hosted ext4 recovery binding block_size=%u cases=%zu no_writes=true no_flushes=true\n",
                l.block_size, cases);
}
