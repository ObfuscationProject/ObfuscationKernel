#pragma once

#include "ok/abi/linux_user.hpp"
#include "ok/posix/posix.hpp"

namespace ok::abi::linux_user
{
enum class StatLayout : u8 { statx, kernel_stat };

// Format-only candidate. Callers supply one checked FileStatus snapshot and
// own the user-copy boundary. This codec performs no lookup, sampling or I/O.
template <Profile Target>
    requires SupportedProfile<Target>
struct StatCodec final
{
    static constexpr bool has_kernel_stat = Target == Profile::x86_64 ||
        Target == Profile::aarch64 || Target == Profile::rv64;
    static constexpr usize size(StatLayout layout)
    {
        if (layout == StatLayout::statx) return 256;
        if (layout == StatLayout::kernel_stat && has_kernel_stat)
            return Target == Profile::x86_64 ? 144 : 128;
        return 0;
    }
    [[nodiscard]] static Result<usize> encode(const posix::FileStatus &status,
                                              StatLayout layout, std::span<u8> out)
    {
        if (layout != StatLayout::statx && layout != StatLayout::kernel_stat)
            return Status::invalid_argument("unknown Linux stat layout");
        const usize length = size(layout);
        if (!length) return Status::unsupported("Linux legacy stat layout unavailable");
        if (out.size() != length) return Status::invalid_argument("Linux stat output size");
        if (auto checked = validate(status); !checked) return checked;
        if (layout == StatLayout::kernel_stat)
        {
            if (status.attributes.device.major > 0xfff || status.attributes.device.minor > 0xfffff)
                return Status::overflow("Linux stat device number");
            if (static_cast<u64>(status.size) > 0x7fffffffffffffffull || status.blocks > 0x7fffffffffffffffull)
                return Status::overflow("Linux signed stat size or block count");
            if constexpr (Target != Profile::x86_64)
                if (status.block_size > 0x7fffffffu)
                    return Status::overflow("Linux signed stat block size");
        }
        std::array<u8,256> bytes{};
        auto put = [&](usize offset, usize width, u64 value) {
            for (usize index = 0; index < width; ++index)
            {
                const usize shift = Codec<Target>::profile.big_endian ? (width - index - 1) * 8 : index * 8;
                bytes[offset + index] = static_cast<u8>(value >> shift);
            }
        };
        const auto &attributes = status.attributes;
        const auto stamp = [&](usize offset, usize width, const fs::EpochTimestamp &time) {
            put(offset,8,static_cast<u64>(time.seconds)); put(offset+8,width,time.nanoseconds);
        };
        if (layout == StatLayout::statx)
        {
            // Volatile shared files have no admitted epoch timestamps or device identity.
            // Statx reports only fields supplied by that explicit provider.
            put(0,4,status.provider == posix::FileStatusProvider::volatile_shared_file ? 0x71f : 0x7ff);
            put(4,4,status.block_size); put(16,4,status.link_count);
            put(20,4,status.uid); put(24,4,status.gid); put(28,2,status.mode);
            put(32,8,attributes.inode); put(40,8,status.size); put(48,8,status.blocks);
            stamp(64,4,attributes.access); stamp(96,4,attributes.change); stamp(112,4,attributes.modification);
            put(136,4,attributes.device.major); put(140,4,attributes.device.minor);
        }
        else
        {
            const u64 minor = attributes.device.minor;
            const u64 device = (minor & 255) | (u64{attributes.device.major} << 8) | ((minor & ~u64{255}) << 12);
            put(0,8,device); put(8,8,attributes.inode);
            if constexpr (Target == Profile::x86_64)
            {
                put(16,8,status.link_count); put(24,4,status.mode); put(28,4,status.uid); put(32,4,status.gid);
                put(56,8,status.block_size);
            }
            else
            {
                put(16,4,status.mode); put(20,4,status.link_count); put(24,4,status.uid); put(28,4,status.gid);
                put(56,4,status.block_size);
            }
            put(48,8,status.size); put(64,8,status.blocks);
            stamp(72,8,attributes.access); stamp(88,8,attributes.modification); stamp(104,8,attributes.change);
        }
        for (usize index = 0; index < length; ++index) out[index] = bytes[index];
        return length;
    }
  private:
    static Status validate(const posix::FileStatus &status)
    {
        const u32 mode_type = status.mode & 0170000u;
        const bool type_matches = (status.type == fs::NodeType::regular && mode_type == 0100000u) ||
            (status.type == fs::NodeType::directory && mode_type == 0040000u) ||
            (status.type == fs::NodeType::symlink && mode_type == 0120000u) ||
            (status.type == fs::NodeType::socket && mode_type == fs::mode_socket) ||
            (status.type == fs::NodeType::device && mode_type == fs::mode_character_device);
        if (!type_matches) return Status::unsupported("Linux stat provider type unavailable");
        const auto &a = status.attributes;
        if (!a.has(fs::Attribute::blocks512))
            return Status::unsupported("Linux stat allocated-512 count unavailable");
        if (static_cast<u64>(status.size) > 0x7fffffffffffffffull || status.blocks > 0x7fffffffffffffffull ||
            status.block_size > 0x7fffffffu)
            return Status::overflow("Linux stat libc field representability");
        if (status.provider == posix::FileStatusProvider::volatile_shared_file)
        {
            constexpr u32 fields = static_cast<u32>(fs::Attribute::inode) |
                static_cast<u32>(fs::Attribute::blocks512);
            if (status.type != fs::NodeType::regular || status.mode != fs::mode_for(fs::NodeType::regular, 0600u) ||
                status.link_count || status.block_size != 4096 || status.size % status.block_size ||
                status.blocks != status.size / 512 || a.valid != fields || !a.inode ||
                a.device.major || a.device.minor)
                return Status::unsupported("Linux volatile shared file metadata unavailable");
            for (const auto *time : {&a.access, &a.modification, &a.change})
                if (time->source != fs::TimestampSource::unavailable || time->seconds || time->nanoseconds)
                    return Status::unsupported("Linux volatile shared file has no epoch provenance");
            return Status::success();
        }
        if (status.provider != posix::FileStatusProvider::filesystem)
            return Status::unsupported("Linux stat provider provenance unavailable");
        if (!a.has(fs::Attribute::inode) || !a.inode || !a.has(fs::Attribute::device) ||
            (!a.device.major && !a.device.minor) ||
            !a.has(fs::Attribute::access_time) || !a.has(fs::Attribute::modification_time) || !a.has(fs::Attribute::change_time))
            return Status::unsupported("Linux stat basic provider fields unavailable");
        if (status.mode > 0xffffu) return Status::overflow("Linux stat mode width");
        for (const auto *time : {&a.access,&a.modification,&a.change})
        {
            if (time->source != fs::TimestampSource::readonly_ext4 && time->source != fs::TimestampSource::maintained_ext4 &&
                time->source != fs::TimestampSource::kernel_fixed)
                return Status::unsupported("Linux stat epoch provenance unavailable");
            if (time->nanoseconds >= 1000000000u)
                return Status::io_error("Linux stat malformed provider nanoseconds");
        }
        if (a.access.source != a.modification.source || a.access.source != a.change.source)
            return Status::unsupported("Linux stat mixed provider provenance");
        return Status::success();
    }
};
} // namespace ok::abi::linux_user
