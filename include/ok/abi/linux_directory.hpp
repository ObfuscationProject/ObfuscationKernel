#pragma once

#include "ok/abi/linux_user.hpp"
#include "ok/fs/vfs.hpp"

namespace ok::abi::linux_user
{
template <Profile Target>
struct DirectoryCodec final
{
    static constexpr void word(std::span<u8> bytes, u64 value)
    {
        for (usize i = 0; i < bytes.size(); ++i)
        {
            const usize shift = Codec<Target>::profile.big_endian ? (bytes.size() - i - 1) * 8 : i * 8;
            bytes[i] = static_cast<u8>(value >> shift);
        }
    }
    static constexpr usize record_size(usize name_size) { return (20 + name_size + 7) & ~usize{7}; }
    static constexpr usize capacity = record_size(fs::max_path_segment);
    static constexpr u8 type(fs::NodeType type)
    {
        switch (type)
        {
        case fs::NodeType::directory: return 4;
        case fs::NodeType::regular: return 8;
        case fs::NodeType::device: return 2;
        case fs::NodeType::socket: return 12;
        case fs::NodeType::symlink: return 10;
        }
        return 0;
    }
    static Result<usize> encode(const fs::DirectoryEntry &entry, u64 next_cookie, std::span<u8> out)
    {
        const auto name = entry.name.view();
        const usize size = record_size(name.size());
        if (!entry.metadata.attributes.has(fs::Attribute::inode) || !entry.metadata.attributes.inode)
            return Status::unsupported("directory entry inode unavailable");
        if (size > out.size()) return Status::invalid_argument("directory record buffer too small");
        for (usize i = 0; i < size; ++i) out[i] = 0;
        word(out.subspan(0, 8), entry.metadata.attributes.inode);
        word(out.subspan(8, 8), next_cookie);
        word(out.subspan(16, 2), size);
        out[18] = type(entry.metadata.type);
        for (usize i = 0; i < name.size(); ++i) out[19 + i] = static_cast<u8>(name[i]);
        return size;
    }
};
} // namespace ok::abi::linux_user
