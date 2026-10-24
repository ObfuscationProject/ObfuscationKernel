#include "ext4_view_test_support.hpp"
#include <algorithm>

using namespace ok;
using namespace ok::fs;
using namespace fixture;

namespace
{
struct Reader
{
    Device &device;
    Status read_block(u32 number, std::span<u8> bytes)
    { return device.read_blocks(u64{number} * 2, std::as_writable_bytes(bytes)); }
};
using View = Ext4ReadOnlyView<Reader>;

struct Opened
{
    Device device;
    Ext4RecoveryBinding binding;
    Reader reader;
    View view;
    explicit Opened(const Bytes &bytes) : device(bytes), reader{device}
    {
        status(binding.inspect(device, {0, bytes.size() / 512}), "ownership fixture binding");
        status(view.open(reader, *binding.info()), "ownership fixture view");
    }
};

u32 first_free(const Bytes &bytes)
{
    const u32 first = r32(bytes, 1044), per_group = r32(bytes, 1056);
    const usize bitmap = r32(bytes, descriptor(bytes, 0)) * 1024;
    for (u32 local = 0; local < per_group; ++local)
        if (!(bytes[bitmap + local / 8] & (1U << (local % 8)))) return first + local;
    check(false, "fixture needs one free data block");
    return 0;
}

void reserved_crc(Bytes &bytes, u32 number)
{
    const usize at = inode_offset(bytes, number);
    w16(bytes, at + 124, 0);
    w16(bytes, at + 124, u16(crc(inode_seed(bytes, number, at), bytes, at, 256)));
}

void direct_symlink(Bytes &bytes, u32 number, u32 physical)
{
    const usize at = inode_offset(bytes, number);
    w16(bytes, at, 0120777); w32(bytes, at + 4, 60);
    w32(bytes, at + 28, 2); w32(bytes, at + 32, 0);
    std::fill_n(bytes.begin() + at + 40, 60, 0);
    w32(bytes, at + 40, physical);
    inode_crc(bytes, number, at);
}

void expect_rejection(const Bytes &bytes, u32 physical, StatusCode expected, usize &cases)
{
    Opened fixture(bytes);
    const auto result = fixture.view.unreferenced_free_data_block(physical);
    check(!result && result.status().code() == expected, "free-block ownership rejection");
    fixture.device.untouched();
    check(fixture.device.bytes == bytes, "ownership rejection changed image");
    ++cases;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    check(bool(input), "open ownership image");
    const Bytes original((std::istreambuf_iterator<char>(input)), {});
    const u32 physical = first_free(original);
    usize cases = 0;
    {
        Opened fixture(original);
        const auto admitted = fixture.view.unreferenced_free_data_block(physical);
        status(admitted.status(), "free block no-reference admission");
        check(admitted.value() == physical && fixture.view.valid(), "free candidate identity");
        const auto owned = fixture.view.unique_first_allocated_data_block(13);
        check(owned && owned.value() == r32(original, inode_offset(original, 13) + 60),
              "final one-block ownership admission");
        fixture.device.untouched();
        check(fixture.device.bytes == original, "successful ownership scan changed input");
        ++cases;
    }
    expect_rejection(original, r32(original, inode_offset(original, 13) + 60), StatusCode::io_error, cases);
    expect_rejection(original, 1, StatusCode::io_error, cases);
    expect_rejection(original, r32(original, descriptor(original, 0) + 8), StatusCode::io_error, cases);
    expect_rejection(original, u32(journal(original) / 1024), StatusCode::io_error, cases);
    expect_rejection(original, r32(original, 1028), StatusCode::unsupported, cases);
    {
        auto bytes = original;
        const usize at = inode_offset(bytes, 3);
        w32(bytes, at + 40, physical); reserved_crc(bytes, 3);
        expect_rejection(bytes, physical, StatusCode::unsupported, cases);
    }
    {
        auto bytes = original;
        bytes[inode_offset(bytes, 3) + 124] ^= 1;
        expect_rejection(bytes, physical, StatusCode::io_error, cases);
    }
    {
        auto bytes = original;
        const usize at = inode_offset(bytes, 13);
        w32(bytes, at + 60, physical); inode_crc(bytes, 13, at);
        expect_rejection(bytes, physical, StatusCode::io_error, cases);
    }
    {
        auto bytes = original;
        direct_symlink(bytes, 13, physical);
        expect_rejection(bytes, physical, StatusCode::io_error, cases);
    }
    {
        auto bytes = original;
        const u32 inodes_per_group = r32(bytes, 1064);
        const u32 number = inodes_per_group + 1;
        const usize desc = descriptor(bytes, 1);
        const usize bitmap = r32(bytes, desc + 4) * 1024;
        const usize source = inode_offset(bytes, 13);
        const usize target = inode_offset(bytes, number);
        // Turn a qualified empty inode-bitmap group into an initialized group
        // with one allocated record that points at the still-clear candidate.
        bytes[bitmap] |= 1;
        std::copy_n(bytes.begin() + source, 256, bytes.begin() + target);
        w32(bytes, target + 60, physical);
        inode_crc(bytes, number, target);
        w16(bytes, desc + 18, u16(r16(bytes, desc + 18) & ~1U));
        w16(bytes, desc + 14, u16(r16(bytes, desc + 14) - 1));
        w16(bytes, desc + 28, u16(r16(bytes, desc + 28) - 1));
        w16(bytes, desc + 26, u16(crc(seed(bytes), bytes, bitmap, inodes_per_group / 8)));
        group_crc(bytes, 1, desc);
        w32(bytes, 1040, r32(bytes, 1040) - 1);
        super_crc(bytes);
        Opened fixture(bytes);
        const auto result = fixture.view.unreferenced_free_data_block(physical);
        check(!result && result.status().code() == StatusCode::io_error &&
              result.status().message() == "ext4 checked data block is not allocated" &&
              !fixture.view.valid(),
              "cross-group allocated inode reference was admitted");
        fixture.device.untouched();
        check(fixture.device.bytes == bytes, "cross-group rejection changed image");
        ++cases;
    }
    for (bool xattr : {false, true})
    {
        auto bytes = original;
        const usize at = inode_offset(bytes, 13);
        if (xattr) w32(bytes, at + 104, physical);
        else w16(bytes, at + 46, 1);
        inode_crc(bytes, 13, at);
        expect_rejection(bytes, physical, StatusCode::unsupported, cases);
    }
    {
        auto bytes = original;
        bytes[inode_offset(bytes, 13) + 124] ^= 1;
        expect_rejection(bytes, physical, StatusCode::io_error, cases);
    }
    {
        auto bytes = original;
        const usize at = inode_offset(bytes, 13);
        w16(bytes, at, 0120777); w32(bytes, at + 4, 4);
        w32(bytes, at + 28, 0); w32(bytes, at + 32, 0);
        std::fill_n(bytes.begin() + at + 40, 60, 0);
        w32(bytes, at + 40, physical); inode_crc(bytes, 13, at);
        Opened fixture(bytes);
        check(fixture.view.unreferenced_free_data_block(physical).ok(),
              "fast symlink bytes were interpreted as a physical pointer");
        fixture.device.untouched(); ++cases;
    }
    for (bool direct : {false, true})
    {
        auto bytes = original;
        const u32 shared = r32(bytes, inode_offset(bytes, 13) + 60);
        const usize at = inode_offset(bytes, 12);
        if (direct) direct_symlink(bytes, 12, shared);
        else
        {
            w32(bytes, at + 4, 1); w32(bytes, at + 28, 2);
            w16(bytes, at + 56, 1); w32(bytes, at + 60, shared);
            inode_crc(bytes, 12, at);
        }
        Opened fixture(bytes);
        const auto result = fixture.view.unique_first_allocated_data_block(13);
        check(!result && result.status().code() == StatusCode::io_error,
              "final overlay admitted a shared mapped block");
        fixture.device.untouched(); ++cases;
    }
    {
        Opened fixture(original);
        const usize at = inode_offset(fixture.device.bytes, 8);
        w32(fixture.device.bytes, at + 60, physical); inode_crc(fixture.device.bytes, 8, at);
        const auto result = fixture.view.unreferenced_free_data_block(physical);
        check(!result && result.status().code() == StatusCode::unsupported,
              "ownership scan ignored changed reserved journal mapping");
        fixture.device.untouched(); ++cases;
    }
    for (auto error : {Status::io_error("ownership read failed"), Status::not_found("ownership read absent")})
    {
        Opened fixture(original);
        fixture.device.fail_read = fixture.device.reads + 1;
        fixture.device.read_failure = error;
        check(!fixture.view.unreferenced_free_data_block(physical) && !fixture.view.valid(),
              "actual reader failure preserved ownership authority");
        fixture.device.untouched(); ++cases;
    }
    for (auto error : {Status::io_error("mid-scan read failed"), Status::not_found("mid-scan read absent")})
    {
        Opened fixture(original);
        const usize before = fixture.device.reads;
        // The first three reads obtain the selected block bitmap, group-zero
        // inode bitmap and inode-one table. Fail the next inode-table read.
        fixture.device.fail_read = before + 4;
        fixture.device.read_failure = error;
        const auto result = fixture.view.unreferenced_free_data_block(physical);
        check(!result && result.status().code() == error.code() && !fixture.view.valid() &&
              fixture.device.reads == before + 4,
              "mid-scan read failure preserved ownership authority");
        fixture.device.untouched(); ++cases;
    }
    {
        auto bytes = original;
        const u32 per_group = r32(bytes, 1064);
        const usize desc = descriptor(bytes, 0), bitmap = r32(bytes, desc + 4) * 1024;
        const u16 previous_free = r16(bytes, desc + 14);
        for (u32 number = 14; number <= 129; ++number)
        {
            const usize at = inode_offset(bytes, number);
            std::fill_n(bytes.begin() + at, 256, 0);
            w16(bytes, at, 0100640); w16(bytes, at + 26, 1);
            w32(bytes, at + 32, 0x80000); w16(bytes, at + 40, 0xf30a);
            w16(bytes, at + 44, 4); w16(bytes, at + 128, 32);
            inode_crc(bytes, number, at);
            bytes[bitmap + (number - 1) / 8] |= u8(1U << ((number - 1) % 8));
        }
        w16(bytes, desc + 14, previous_free - 116);
        w16(bytes, desc + 28, u16(per_group - 129));
        w16(bytes, desc + 26, u16(crc(seed(bytes), bytes, bitmap, per_group / 8)));
        group_crc(bytes, 0, desc);
        w32(bytes, 1040, r32(bytes, 1040) - 116); super_crc(bytes);
        Opened fixture(bytes);
        const auto result = fixture.view.unreferenced_free_data_block(physical);
        check(!result && result.status().code() == StatusCode::no_memory && fixture.view.valid(),
              "ownership budget did not reject without retiring the valid reader");
        fixture.device.untouched(); ++cases;
    }
    std::printf("PASS first-data ownership hosted model cases=%zu no_writes=1 reserved_scan=1 budget=128\n", cases);
}
