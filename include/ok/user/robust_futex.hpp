#pragma once

#include "ok/abi/linux_user.hpp"

#include <array>
#include <concepts>
#include <span>

namespace ok::user::native
{

inline constexpr u32 robust_waiters = 0x80000000U;
inline constexpr u32 robust_owner_died = 0x40000000U;
inline constexpr u32 robust_tid_mask = 0x3fffffffU;
inline constexpr usize robust_list_limit = 2048;

enum class RobustStop : u8 { complete, head_fault, node_fault, word_fault, limit };
struct RobustWalkReport
{
    RobustStop stop{RobustStop::complete};
    usize visited{}, marked{}, wake_attempts{}, pi_entries{};
};

// Access must serialize against every eligible user writer. A CPU0 service
// loop with all native users stopped can satisfy this; it is not an SMP CAS.
template <typename Access>
concept SerializedRobustAccess = Access::serialized_native_execution && requires(
    Access &access, u64 address, std::span<std::byte> bytes, u32 expected, u32 desired) {
    { access.read(address, bytes) } -> std::same_as<Status>;
    { access.compare_exchange32(address, expected, desired) } -> std::same_as<Result<u32>>;
    { access.wake_one(address) } -> std::same_as<void>;
};

template <abi::linux_user::Profile Profile>
struct RobustListCodec
{
    using Codec = abi::linux_user::Codec<Profile>;
    static constexpr usize word_bytes = Codec::profile.word_bytes;
    static constexpr bool big_endian = Codec::profile.big_endian;
    static constexpr usize head_bytes = 3 * word_bytes;
    static constexpr u64 mask = Codec::word_mask;

    template <SerializedRobustAccess Access>
    static Result<u64> word(Access &access, u64 address)
    {
        std::array<std::byte, word_bytes> bytes{};
        if (auto status = access.read(address & mask, bytes); !status) return status;
        u64 value{};
        for (usize i = 0; i < word_bytes; ++i)
            value |= static_cast<u64>(bytes[i]) << (8 * (Codec::profile.big_endian ? word_bytes - 1 - i : i));
        return value;
    }
    static constexpr u64 add(u64 address, u64 offset) { return (address + offset) & mask; }
};

template <abi::linux_user::Profile Profile, SerializedRobustAccess Access>
RobustWalkReport retire_robust_list(u64 head, u32 tid, Access &access)
{
    using Codec = RobustListCodec<Profile>;
    RobustWalkReport report;
    head &= Codec::mask;
    if (!head) return report;
    auto entry_word = Codec::word(access, head);
    if (!entry_word) { report.stop = RobustStop::head_fault; return report; }
    auto offset = Codec::word(access, Codec::add(head, Codec::word_bytes));
    if (!offset) { report.stop = RobustStop::head_fault; return report; }
    auto pending_word = Codec::word(access, Codec::add(head, 2 * Codec::word_bytes));
    if (!pending_word) { report.stop = RobustStop::head_fault; return report; }
    const u64 pending = pending_word.value() & ~u64{1};
    auto death = [&](u64 node, bool pi, bool is_pending) {
        const u64 address = Codec::add(node, offset.value());
        if (pi) ++report.pi_entries;
        if (address % sizeof(u32)) return false;
        for (;;)
        {
            std::array<std::byte, sizeof(u32)> bytes{};
            if (!access.read(address, bytes)) return false;
            u32 value{};
            for (usize i = 0; i < bytes.size(); ++i)
                value |= static_cast<u32>(bytes[i]) << (8 * (Codec::big_endian ? 3 - i : i));
            const u32 owner = value & robust_tid_mask;
            if (is_pending && !pi && owner == 0)
            {
                ++report.wake_attempts;
                access.wake_one(address);
                return true;
            }
            if (owner != tid) return true;
            auto changed = access.compare_exchange32(address, value, (value & robust_waiters) | robust_owner_died);
            if (!changed) return false;
            if (changed.value() != value) continue;
            ++report.marked;
            // PI state teardown is a separate service; do not wake a normal
            // futex waiter on behalf of an unimplemented PI owner record.
            if (!pi && (value & robust_waiters))
            {
                ++report.wake_attempts;
                access.wake_one(address);
            }
            return true;
        }
    };
    u64 entry = entry_word.value() & ~u64{1};
    bool pi = entry_word.value() & 1;
    while (entry != head)
    {
        // Linux handles the current owner before acting on a failed next-link
        // read. A pending entry is handled exactly once after traversal.
        auto next = Codec::word(access, entry);
        ++report.visited;
        if (entry != pending && !death(entry, pi, false))
        { report.stop = RobustStop::word_fault; return report; }
        if (!next) { report.stop = RobustStop::node_fault; return report; }
        entry = next.value() & ~u64{1};
        pi = next.value() & 1;
        if (report.visited == robust_list_limit)
        { report.stop = RobustStop::limit; break; }
    }
    if (pending && !death(pending, pending_word.value() & 1, true))
        report.stop = RobustStop::word_fault;
    return report;
}

} // namespace ok::user::native
