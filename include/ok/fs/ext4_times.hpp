#pragma once

#include "ok/fs/attributes.hpp"
#include <bit>
#include <span>

namespace ok::fs
{
// Decode only stored epoch fields. The caller must establish read-only inode
// provenance; this helper does not turn a writable provider into a clock source.
inline Status decode_ext4_times(std::span<const std::byte> inode, FileAttributes &output)
{
    if (inode.size() != 128 && inode.size() != 256)
        return Status::invalid_argument("unsupported ext4 timestamp inode size");
    auto le16 = [&](usize at) -> u16 {
        return std::to_integer<u16>(inode[at]) | (std::to_integer<u16>(inode[at + 1]) << 8);
    };
    auto le32 = [&](usize at) -> u32 { return le16(at) | (static_cast<u32>(le16(at + 2)) << 16); };
    const usize extra_size = inode.size() == 256 ? le16(128) : 0;
    if (extra_size > inode.size() - 128 || (extra_size & 3))
        return Status::io_error("invalid ext4 extra inode extent");
    auto decode = [&](usize base, usize extra) -> Result<EpochTimestamp> {
        const i64 seconds = std::bit_cast<i32>(le32(base));
        const u32 extension = extra + 4 <= 128 + extra_size ? le32(extra) : 0;
        const u32 nanos = extension >> 2;
        if (nanos >= 1000000000) return Status::io_error("invalid ext4 timestamp nanoseconds");
        return EpochTimestamp{seconds + static_cast<i64>(u64{extension & 3} << 32),
                              nanos, TimestampSource::readonly_ext4};
    };
    auto access = decode(8, 140), modification = decode(16, 136), change = decode(12, 132);
    if (!access) return access.status();
    if (!modification) return modification.status();
    if (!change) return change.status();
    auto result = output;
    result.access = access.value(); result.modification = modification.value(); result.change = change.value();
    result.mark(Attribute::access_time); result.mark(Attribute::modification_time); result.mark(Attribute::change_time);
    output = result;
    return {};
}
enum class Ext4TimestampFields : u32
{
    access = 1,
    modification = 2,
    change = 4,
    all = 7,
};

[[nodiscard]] constexpr Ext4TimestampFields operator|(Ext4TimestampFields one, Ext4TimestampFields two)
{
    return static_cast<Ext4TimestampFields>(static_cast<u32>(one) | static_cast<u32>(two));
}

// Format-only, selected-field update. The caller owns event policy, source
// qualification and any explicit initialization of a newly allocated inode.
// All checks precede writes; unknown, unselected and undeclared bytes survive.
inline Status encode_ext4_times(std::span<std::byte> inode, Ext4TimestampFields fields,
                                i64 seconds, u32 nanoseconds)
{
    if (inode.size() != 128 && inode.size() != 256)
        return Status::invalid_argument("unsupported ext4 timestamp inode size");
    const auto mask = static_cast<u32>(fields);
    if (!mask || (mask & ~u32{7}))
        return Status::invalid_argument("invalid ext4 timestamp field mask");
    if (nanoseconds >= 1000000000)
        return Status::invalid_argument("invalid ext4 timestamp nanoseconds");
    const usize extra_size = inode.size() == 256
        ? std::to_integer<u16>(inode[128]) | (std::to_integer<u16>(inode[129]) << 8) : 0;
    if (extra_size > inode.size() - 128 || (extra_size & 3))
        return Status::io_error("invalid ext4 extra inode extent");
    if (seconds < -2147483648LL || seconds > 15032385535LL)
        return Status::overflow("ext4 timestamp seconds exceed extended range");

    struct Field { u32 bit; usize base, extra; };
    constexpr std::array<Field, 3> layout{{{1, 8, 140}, {2, 16, 136}, {4, 12, 132}}};
    for (const auto &field : layout)
        if ((mask & field.bit) && field.extra + 4 > 128 + extra_size &&
            (seconds < -2147483648LL || seconds > 2147483647LL))
            return Status::overflow("ext4 timestamp seconds require a declared extra word");

    const u32 base = static_cast<u32>(seconds);
    const i64 signed_base = std::bit_cast<i32>(base);
    // The range check bounds this subtraction and its nonnegative result.
    const u64 difference = static_cast<u64>(seconds - signed_base);
    if ((difference & 0xffffffffULL) || (difference >> 32) > 3)
        return Status::overflow("ext4 timestamp epoch cannot be represented");
    const u32 extension = (nanoseconds << 2) | static_cast<u32>(difference >> 32);
    const auto store = [&](usize at, u32 value) {
        for (usize byte = 0; byte < 4; ++byte)
            inode[at + byte] = std::byte(static_cast<u8>(value >> (byte * 8)));
    };
    for (const auto &field : layout)
        if (mask & field.bit)
        {
            store(field.base, base);
            if (field.extra + 4 <= 128 + extra_size)
                store(field.extra, extension);
        }
    return {};
}
} // namespace ok::fs
