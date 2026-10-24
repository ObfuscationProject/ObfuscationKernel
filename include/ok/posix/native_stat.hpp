#pragma once
#include "ok/posix/posix.hpp"
#include "ok/uapi/syscall.h"
#include <bit>

namespace ok::posix
{
// The native record keeps its original layout. Padding must never carry a
// temporary C++ object's unspecified bytes across the user-copy boundary.
[[nodiscard]] inline std::array<std::byte, sizeof(ok_stat)> native_file_status(const FileStatus &status)
{
    std::array<std::byte, sizeof(ok_stat)> output{};
    auto field = [&]<typename Value>(usize offset, Value value) {
        const auto bytes = std::bit_cast<std::array<std::byte, sizeof(Value)>>(value);
        for (usize i = 0; i < bytes.size(); ++i) output[offset + i] = bytes[i];
    };
    field(offsetof(ok_stat, type), static_cast<u8>(status.type));
    field(offsetof(ok_stat, size), static_cast<decltype(ok_stat::size)>(status.size));
    field(offsetof(ok_stat, mode), status.mode);
    field(offsetof(ok_stat, uid), status.uid);
    field(offsetof(ok_stat, gid), status.gid);
    field(offsetof(ok_stat, link_count), status.link_count);
    field(offsetof(ok_stat, block_size), status.block_size);
    field(offsetof(ok_stat, blocks), status.blocks);
    return output;
}
inline void write_native_file_status(void *destination, const FileStatus &status)
{
    const auto bytes = native_file_status(status);
    auto *output = static_cast<std::byte *>(destination);
    for (usize i = 0; i < bytes.size(); ++i) output[i] = bytes[i];
}

// Query once and serialize the same provider result. An unadvertised identity
// remains zero; neither internal C++ padding nor stale attributes are exposed.
[[nodiscard]] inline std::array<std::byte, sizeof(ok_stat_v2)> native_file_status_v2(const FileStatus &status)
{
    static_assert(offsetof(ok_stat_v2, basic) == 0);
    std::array<std::byte, sizeof(ok_stat_v2)> output{};
    const auto basic = native_file_status(status);
    for (usize i = 0; i < basic.size(); ++i) output[i] = basic[i];
    const u64 inode = status.attributes.has(fs::Attribute::inode) ? status.attributes.inode : 0;
    const auto identity = std::bit_cast<std::array<std::byte, sizeof(inode)>>(inode);
    for (usize i = 0; i < identity.size(); ++i) output[offsetof(ok_stat_v2, inode) + i] = identity[i];
    return output;
}
inline void write_native_file_status_v2(void *destination, const FileStatus &status)
{
    const auto bytes = native_file_status_v2(status);
    auto *output = static_cast<std::byte *>(destination);
    for (usize i = 0; i < bytes.size(); ++i) output[i] = bytes[i];
}
} // namespace ok::posix
