#include "ext4_next_data_test_support.hpp"
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
struct Opened
{
    Device device;
    Ext4RecoveryBinding binding;
    Reader reader;
    Ext4ReadOnlyView<Reader> view;
    explicit Opened(const Bytes &bytes) : device(bytes), reader{device}
    {
        status(binding.inspect(device, {0, bytes.size() / 512}), "next-data ownership binding");
        status(view.open(reader, *binding.info()), "next-data ownership view");
    }
};

u32 nth_free(const Bytes &bytes, u32 ordinal)
{
    const u32 first = r32(bytes, 1044), per_group = r32(bytes, 1056);
    const usize bitmap = r32(bytes, descriptor(bytes, 0)) * 1024;
    for (u32 local = 0; local < per_group; ++local)
        if (!(bytes[bitmap + local / 8] & (1U << (local % 8))) && !ordinal--)
            return first + local;
    check(false, "next-data fixture needs a free block");
    return 0;
}

void target_size(Bytes &bytes, u32 size)
{
    const usize at = inode_offset(bytes, 13);
    w32(bytes, at + 4, size);
    inode_crc(bytes, 13, at);
}

void allocate(Bytes &bytes, u32 first, u32 count)
{
    const u32 group = (first - r32(bytes, 1044)) / r32(bytes, 1056);
    const u32 begin = r32(bytes, 1044) + group * r32(bytes, 1056);
    const usize desc = descriptor(bytes, group);
    const usize bitmap = r32(bytes, desc) * 1024;
    for (u32 physical = first; physical < first + count; ++physical)
    {
        const u32 local = physical - begin;
        check(!(bytes[bitmap + local / 8] & (1U << (local % 8))), "test block was not free");
        bytes[bitmap + local / 8] |= u8(1U << (local % 8));
    }
    w16(bytes, desc + 12, u16(r16(bytes, desc + 12) - count));
    w16(bytes, desc + 24, u16(crc(seed(bytes), bytes, bitmap, r32(bytes, 1056) / 8)));
    group_crc(bytes, group, desc);
    w32(bytes, 1036, r32(bytes, 1036) - count);
    super_crc(bytes);
}

void append_final(Bytes &bytes, u32 physical)
{
    const usize at = inode_offset(bytes, 13);
    const u32 old = r32(bytes, at + 4) / 1024;
    check(old >= 1 && old <= 15 && r32(bytes, at + 4) == old * 1024,
          "test predecessor is not aligned");
    if (physical == r32(bytes, at + 60) + r16(bytes, at + 56))
        w16(bytes, at + 56, u16(r16(bytes, at + 56) + 1));
    else
    {
        const usize extent = at + 52 + 12 * r16(bytes, at + 42);
        check(r16(bytes, at + 42) < 4, "test final extent capacity");
        w32(bytes, extent, old);
        w16(bytes, extent + 4, 1);
        w16(bytes, extent + 6, 0);
        w32(bytes, extent + 8, physical);
        w16(bytes, at + 42, u16(r16(bytes, at + 42) + 1));
    }
    w32(bytes, at + 4, old * 1024 + 31);
    w32(bytes, at + 28, (old + 1) * 2);
    inode_crc(bytes, 13, at);
    allocate(bytes, physical, 1);
}

void expect_rejected(const Bytes &bytes, u32 physical, bool allocated, StatusCode code,
                     usize &cases)
{
    Opened opened(bytes);
    const auto result = opened.view.prove_next_data_blocks(13, physical, allocated);
    check(!result && result.status().code() == code, "next-data ownership rejection");
    opened.device.untouched();
    check(opened.device.bytes == bytes, "next-data rejection changed disk bytes");
    ++cases;
}

void direct_symlink(Bytes &bytes, u32 number, u32 physical)
{
    const usize at = inode_offset(bytes, number);
    w16(bytes, at, 0120777);
    w32(bytes, at + 4, 60);
    w32(bytes, at + 28, 2);
    w32(bytes, at + 32, 0);
    std::fill_n(bytes.begin() + at + 40, 60, 0);
    w32(bytes, at + 40, physical);
    inode_crc(bytes, number, at);
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    check(bool(input), "open next-data ownership image");
    const Bytes original((std::istreambuf_iterator<char>(input)), {});
    Bytes aligned = original;
    target_size(aligned, 1024);
    const u32 candidate = nth_free(aligned, 0);
    const u32 noncontiguous = nth_free(aligned, 1);
    check(candidate == r32(aligned, inode_offset(aligned, 13) + 60) + 1 &&
          noncontiguous != candidate + 1, "test coalesced/noncontiguous candidates");
    usize cases = 0, global_reads = 0;
    {
        Opened opened(aligned);
        const auto proof = opened.view.prove_next_data_blocks(13, candidate, false);
        status(proof.status(), "one old block and free next block");
        check(proof.value().old_blocks == 1 && proof.value().old_physical[0] == candidate - 1 &&
              proof.value().new_physical == candidate && opened.view.valid(),
              "pre-append ownership fields");
        opened.device.untouched(); ++cases;
    }
    for (u32 physical : {candidate, noncontiguous})
    {
        auto bytes = aligned;
        append_final(bytes, physical);
        Opened opened(bytes);
        const auto proof = opened.view.prove_next_data_blocks(13, physical, true);
        status(proof.status(), "final one-block extension ownership");
        check(proof.value().old_blocks == 1 && proof.value().old_physical[0] == candidate - 1 &&
              proof.value().new_physical == physical && opened.view.valid(),
              "final ownership fields");
        opened.device.untouched(); ++cases;
    }
    {
        auto bytes = aligned;
        for (u32 physical : {noncontiguous, noncontiguous + 2, noncontiguous + 4})
            allocate(bytes, physical, 1);
        const usize at = inode_offset(bytes, 13);
        w32(bytes, at + 4, 4 * 1024);
        w32(bytes, at + 28, 8);
        w16(bytes, at + 42, 4);
        for (u32 index = 1; index < 4; ++index)
        {
            const usize extent = at + 52 + index * 12;
            w32(bytes, extent, index);
            w16(bytes, extent + 4, 1);
            w32(bytes, extent + 8, noncontiguous + (index - 1) * 2);
        }
        inode_crc(bytes, 13, at);
        Opened opened(bytes);
        const auto proof = opened.view.prove_next_data_blocks(13, candidate, false);
        status(proof.status(), "four old inline extent records");
        check(proof.value().old_blocks == 4 && proof.value().old_physical[0] == candidate - 1 &&
              proof.value().old_physical[3] == noncontiguous + 4,
              "four-extent ownership collection");
        opened.device.untouched(); ++cases;
    }
    {
        auto bytes = aligned;
        const u32 first = noncontiguous, old = r32(bytes, inode_offset(bytes, 13) + 60);
        allocate(bytes, first, 14);
        const usize at = inode_offset(bytes, 13);
        w32(bytes, at + 4, 15 * 1024);
        w32(bytes, at + 28, 30);
        w16(bytes, at + 42, 2);
        w32(bytes, at + 64, 1);
        w16(bytes, at + 68, 14);
        w32(bytes, at + 72, first);
        inode_crc(bytes, 13, at);
        Opened opened(bytes);
        const auto proof = opened.view.prove_next_data_blocks(13, candidate, false);
        status(proof.status(), "fifteen old blocks");
        check(proof.value().old_blocks == 15 && proof.value().old_physical[0] == old &&
              proof.value().old_physical[14] == first + 13, "fifteen-block ownership collection");
        opened.device.untouched(); ++cases;
        target_size(bytes, 16 * 1024);
        expect_rejected(bytes, candidate, false, StatusCode::unsupported, cases);
    }
    {
        auto bytes = aligned;
        const usize at = inode_offset(bytes, 13);
        w32(bytes, at + 52, 1); inode_crc(bytes, 13, at);
        expect_rejected(bytes, candidate, false, StatusCode::unsupported, cases);
    }
    {
        auto bytes = aligned;
        const usize at = inode_offset(bytes, 13);
        w16(bytes, at + 56, 0x8001); inode_crc(bytes, 13, at);
        expect_rejected(bytes, candidate, false, StatusCode::unsupported, cases);
    }
    expect_rejected(aligned, candidate - 1, false, StatusCode::io_error, cases);
    {
        Opened opened(aligned);
        const usize bitmap = r32(opened.device.bytes, descriptor(opened.device.bytes, 0)) * 1024;
        const u32 local = candidate - r32(opened.device.bytes, 1044);
        opened.device.bytes[bitmap + local / 8] |= u8(1U << (local % 8));
        const auto proof = opened.view.prove_next_data_blocks(13, candidate, false);
        check(!proof && proof.status().code() == StatusCode::io_error && !opened.view.valid(),
              "mutated candidate bitmap retained ownership authority");
        opened.device.untouched(); ++cases;
    }
    {
        auto bytes = aligned;
        direct_symlink(bytes, 12, candidate - 1);
        expect_rejected(bytes, candidate, false, StatusCode::io_error, cases);
    }
    {
        auto bytes = aligned;
        const usize at = inode_offset(bytes, 12);
        w32(bytes, at + 4, 1024);
        w32(bytes, at + 28, 2);
        w16(bytes, at + 56, 1);
        w32(bytes, at + 60, candidate - 1);
        inode_crc(bytes, 12, at);
        expect_rejected(bytes, candidate, false, StatusCode::io_error, cases);
    }
    for (auto error : {Status::io_error("next-data injected read error"),
                       Status::not_found("next-data injected missing read")})
    {
        Opened opened(aligned);
        const usize prior = opened.device.reads;
        opened.device.fail_read = prior + 6;
        opened.device.read_failure = error;
        const auto result = opened.view.prove_next_data_blocks(13, candidate, false);
        check(!result && result.status().code() == error.code() && !opened.view.valid(),
              "midscan reader failure left next-data view valid");
        opened.device.untouched(); ++cases;
    }
    {
        auto bytes = aligned;
        const u32 per_group = r32(bytes, 1064);
        const usize desc = descriptor(bytes, 0), bitmap = r32(bytes, desc + 4) * 1024;
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
        w16(bytes, desc + 14, u16(r16(bytes, desc + 14) - 116));
        w16(bytes, desc + 28, u16(per_group - 129));
        w16(bytes, desc + 26, u16(crc(seed(bytes), bytes, bitmap, per_group / 8)));
        group_crc(bytes, 0, desc);
        w32(bytes, 1040, r32(bytes, 1040) - 116); super_crc(bytes);
        Opened opened(bytes);
        // The candidate scan is bounded by the filesystem's inodes, not by 128;
        // the global proof keeps the bound its zero-truncate reservation assumes.
        const auto proof = opened.view.prove_next_data_blocks(13, candidate, false);
        check(proof && opened.view.valid(), "next-data scan admits more than 128 allocated inodes");
        const auto global = opened.view.prove_global_data_ownership();
        check(!global && global.code() == StatusCode::no_memory && !opened.view.valid(),
              "reserved-inclusive final ownership scan budget");
        opened.device.untouched(); ++cases;
    }
    {
        Opened opened(aligned);
        const usize before = opened.device.reads;
        status(opened.view.prove_global_data_ownership(), "unique final-state data ownership");
        global_reads = opened.device.reads - before;
        check(global_reads > 2, "final ownership scan did not read allocated inodes");
        check(opened.view.valid(), "final ownership proof retired valid view");
        opened.device.untouched(); ++cases;
    }
    {
        auto bytes = aligned;
        const usize at = inode_offset(bytes, 13);
        check(r32(bytes, at + 60) == 534 &&
              r32(bytes, inode_offset(bytes, 16) + 72) == 543,
              "unrelated owner alias fixture");
        w32(bytes, at + 60, 543);
        inode_crc(bytes, 13, at);
        Opened opened(bytes);
        const auto proof = opened.view.prove_global_data_ownership();
        check(!proof && proof.code() == StatusCode::io_error && !opened.view.valid(),
              "unrelated inode data alias escaped final ownership proof");
        opened.device.untouched(); ++cases;
    }
    {
        auto bytes = aligned;
        direct_symlink(bytes, 12, 534);
        Opened opened(bytes);
        const auto proof = opened.view.prove_global_data_ownership();
        check(!proof && proof.code() == StatusCode::io_error && !opened.view.valid(),
              "direct symlink alias escaped final ownership proof");
        opened.device.untouched(); ++cases;
    }
    {
        auto bytes = aligned;
        bytes[inode_offset(bytes, 13) + 124] ^= 1;
        Opened opened(bytes);
        const auto proof = opened.view.prove_global_data_ownership();
        check(!proof && proof.code() == StatusCode::io_error && !opened.view.valid(),
              "unrelated inode checksum escaped final ownership proof");
        opened.device.untouched(); ++cases;
    }
    {
        Opened opened(aligned);
        const usize bitmap = r32(opened.device.bytes, descriptor(opened.device.bytes, 0) + 4) * 1024;
        opened.device.bytes[bitmap + 10] ^= 1;
        const auto proof = opened.view.prove_global_data_ownership();
        check(!proof && proof.code() == StatusCode::io_error && !opened.view.valid(),
              "inode bitmap checksum escaped final ownership proof");
        opened.device.untouched(); ++cases;
    }
    for (auto error : {Status::io_error("final ownership injected read error"),
                       Status::not_found("final ownership injected missing read")})
    for (usize offset : {usize{1}, global_reads / 2, global_reads})
    {
        Opened opened(aligned);
        opened.device.fail_read = opened.device.reads + offset;
        opened.device.read_failure = error;
        const auto proof = opened.view.prove_global_data_ownership();
        check(!proof && proof.code() == error.code() && !opened.view.valid(),
              "final ownership scan hid reader failure");
        opened.device.untouched(); ++cases;
    }
    std::printf("PASS next-data ownership hosted model cases=%zu no_writes=1 budget=128 final_global=1\n", cases);
}
