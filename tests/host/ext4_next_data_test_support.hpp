#pragma once
#include <ok/fs/ext4_readonly.hpp>
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
    if (!condition) { std::fprintf(stderr, "FAIL ext4 next-data ownership model: %s\n", what); std::exit(1); }
}
inline u16 r16(const Bytes &b, usize at) { return u16(b[at]) | (u16(b[at + 1]) << 8); }
inline u32 r32(const Bytes &b, usize at) { return u32(r16(b, at)) | (u32(r16(b, at + 2)) << 16); }
inline void w16(Bytes &b, usize at, u16 x) { b[at] = u8(x); b[at + 1] = u8(x >> 8); }
inline void w32(Bytes &b, usize at, u32 x) { w16(b, at, u16(x)); w16(b, at + 2, u16(x >> 16)); }
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
    void untouched() const { check(!writes && !flushes, "ownership proof attempted mutation or flush"); }
};
inline void status(const ok::Status &s, const char *operation)
{
    if (!s) { std::fprintf(stderr, "%s code=%u reason=%.*s\n", operation, u32(s.code()), int(s.message().size()), s.message().data()); check(false, operation); }
}
} // namespace fixture
