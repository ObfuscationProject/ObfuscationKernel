#pragma once
#include <ok/fs/ext4_replay_view.hpp>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
extern "C" unsigned int ext2fs_crc32c_le(unsigned int, const unsigned char *, size_t);
namespace fixture
{
using namespace ok;
using Bytes = std::vector<u8>;
inline void check(bool condition, const char *what)
{
    if (!condition) { std::fprintf(stderr, "FAIL ext4 preview model: %s\n", what); std::exit(1); }
}
inline u16 r16(const Bytes &b, usize at) { return u16(b[at]) | (u16(b[at + 1]) << 8); }
inline u32 r32(const Bytes &b, usize at) { return u32(r16(b, at)) | (u32(r16(b, at + 2)) << 16); }
inline u32 be32(const Bytes &b, usize at) { return (u32(b[at]) << 24) | (u32(b[at + 1]) << 16) | (u32(b[at + 2]) << 8) | b[at + 3]; }
inline void w16(Bytes &b, usize at, u16 x) { b[at] = u8(x); b[at + 1] = u8(x >> 8); }
inline void w32(Bytes &b, usize at, u32 x) { w16(b, at, u16(x)); w16(b, at + 2, u16(x >> 16)); }
inline void big32(Bytes &b, usize at, u32 x) { b[at] = u8(x >> 24); b[at + 1] = u8(x >> 16); b[at + 2] = u8(x >> 8); b[at + 3] = u8(x); }
inline u32 crc(u32 seed, const Bytes &b, usize at, usize size) { return ext2fs_crc32c_le(seed, b.data() + at, size); }
inline u32 block_size(const Bytes &b) { return 1024U << r32(b, 1048); }
inline u32 seed(const Bytes &b) { return crc(~u32{0}, b, 1128, 16); }
inline usize descriptor(const Bytes &b, u32 group) { return (r32(b, 1044) + 1) * block_size(b) + group * 32; }
inline usize inode_offset(const Bytes &b, u32 number)
{
    const u32 ipg = r32(b, 1064);
    return r32(b, descriptor(b, (number - 1) / ipg) + 8) * block_size(b) + ((number - 1) % ipg) * 256;
}
inline u32 inode_seed(const Bytes &b, u32 number, usize at)
{
    const std::array<u8,4> encoded{u8(number), u8(number >> 8), u8(number >> 16), u8(number >> 24)};
    return crc(ext2fs_crc32c_le(seed(b), encoded.data(), encoded.size()), b, at + 100, 4);
}
inline void inode_crc(Bytes &b, u32 number, usize at)
{
    w16(b, at + 124, 0); w16(b, at + 130, 0);
    const u32 value = crc(inode_seed(b, number, at), b, at, 256);
    w16(b, at + 124, u16(value)); w16(b, at + 130, u16(value >> 16));
}
inline void super_crc(Bytes &b, usize at = 1024) { w32(b, at + 1020, crc(~u32{0}, b, at, 1020)); }
inline void group_crc(Bytes &b, u32 group, usize at)
{
    const std::array<u8,4> encoded{u8(group), u8(group >> 8), u8(group >> 16), u8(group >> 24)};
    w16(b, at + 30, 0);
    w16(b, at + 30, u16(crc(ext2fs_crc32c_le(seed(b), encoded.data(), encoded.size()), b, at, 32)));
}
inline void directory_crc(Bytes &b, u32 number, usize at)
{
    const usize inode = inode_offset(b, number), size = block_size(b);
    w32(b, at + size - 4, crc(inode_seed(b, number, inode), b, at, size - 12));
}
inline usize journal(const Bytes &b) { return r32(b, inode_offset(b, 8) + 60) * block_size(b); }
inline void journal_record_crc(Bytes &b, usize at, usize checksum)
{
    big32(b, at + checksum, 0); big32(b, at + checksum, crc(seed(b), b, at, block_size(b)));
}
// Exact two-payload Linux fixture: use upstream CRC32C independently of Kernel codecs.
inline void linux_payload_crc(Bytes &b, bool inode_payload)
{
    const usize first = journal(b), size = block_size(b), record = first + size;
    const auto payload = first + size * (inode_payload ? 3 : 2);
    const auto seq = be32(b, record + 8);
    const std::array<u8,4> encoded{u8(seq >> 24), u8(seq >> 16), u8(seq >> 8), u8(seq)};
    const auto sum = crc(ext2fs_crc32c_le(seed(b), encoded.data(), encoded.size()), b, payload, size);
    big32(b, record + (inode_payload ? 56 : 24), sum);
    journal_record_crc(b, record, size - 4);
}
struct Device final : driver::BlockDevice
{
    explicit Device(const Bytes &input) : bytes(input) {}
    Bytes bytes;
    usize reads{}, writes{}, flushes{}, fail_read{}, change_after{};
    u64 origin{};
    bool writable{};
    Status read_failure{Status::io_error("injected preview read error")};
    driver::BlockGeometry geometry() const override
    {
        return {origin + bytes.size() / 512 - (change_after && reads >= change_after ? 1 : 0), 512, writable};
    }
    Status read_blocks(u64 first, std::span<std::byte> out) override
    {
        ++reads;
        if (reads == fail_read) return read_failure;
        if (first < origin || first - origin >= bytes.size() / 512 || out.size() % 512 ||
            out.size() / 512 > bytes.size() / 512 - (first - origin)) return Status::io_error("test device bounds");
        for (usize i = 0; i < out.size(); ++i) out[i] = std::byte(bytes[usize(first - origin) * 512 + i]);
        return {};
    }
    Status write_blocks(u64, std::span<const std::byte>) override { ++writes; return Status::fault("unexpected preview write"); }
    Status flush() override { ++flushes; return Status::fault("unexpected preview flush"); }
    void untouched() const { check(!writes && !flushes, "preview attempted mutation or flush"); }
};
inline void status(const ok::Status &s, const char *operation)
{
    if (!s) { std::fprintf(stderr, "%s code=%u reason=%.*s\n", operation, u32(s.code()), int(s.message().size()), s.message().data()); check(false, operation); }
}
} // namespace fixture
