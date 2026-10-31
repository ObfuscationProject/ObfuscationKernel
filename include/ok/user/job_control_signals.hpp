#pragma once

#include "ok/abi/linux_profile_data.hpp"
#include "ok/core/types.hpp"
#include <array>

// Job-control signal numbering and default actions. Native numbers equal the
// Linux generic numbering on every profile; only MIPS and MIPS64 Linux differ.
// Everything here is a constant table: a kernel image carries one Linux
// profile, so translation is the identity except on MIPS.
namespace ok::user::job
{

enum class DefaultAction : u8 { terminate, ignore, stop, cont };

struct Numbering
{
    u32 max{}, hup{}, interrupt{}, quit{}, kill{}, term{}, child{}, cont{}, stop{}, tstp{}, ttin{}, ttou{},
        window{}, urgent{};
    // Generic number of each standard signal 1..31; 0 has no generic counterpart.
    std::array<u8, 32> to_generic{};
};

namespace detail
{
[[nodiscard]] consteval std::array<u8, 32> identity_numbers()
{
    std::array<u8, 32> numbers{};
    for (usize i = 1; i < numbers.size(); ++i) numbers[i] = static_cast<u8>(i);
    return numbers;
}
} // namespace detail

inline constexpr Numbering generic{64, 1, 2, 3, 9, 15, 17, 18, 19, 20, 21, 22, 28, 23, detail::identity_numbers()};
// Linux arch/mips/include/uapi/asm/signal.h. EMT (7) has no generic
// counterpart and generic STKFLT (16) has none on MIPS.
inline constexpr Numbering mips{128, 1, 2, 3, 9, 15, 18, 25, 23, 24, 26, 27, 20, 21,
    {0, 1, 2, 3, 4, 5, 6, 0, 8, 9, 7, 11, 31, 13, 14, 15, 10, 12, 17, 30, 28, 23, 29, 19, 20, 18, 21, 22, 26, 27,
     24, 25}};
// OK_SIG* numbers of the native personality.
inline constexpr const Numbering &native = generic;

template <abi::linux_user::Profile P>
inline constexpr const Numbering &linux_numbering =
    (P == abi::linux_user::Profile::mips || P == abi::linux_user::Profile::mips64) ? mips : generic;

[[nodiscard]] constexpr bool stop_family(const Numbering &numbering, u32 signal)
{
    return signal == numbering.stop || signal == numbering.tstp || signal == numbering.ttin ||
        signal == numbering.ttou;
}

[[nodiscard]] constexpr DefaultAction default_action(const Numbering &numbering, u32 signal)
{
    if (signal == numbering.child || signal == numbering.window || signal == numbering.urgent)
        return DefaultAction::ignore;
    if (signal == numbering.cont) return DefaultAction::cont;
    if (stop_family(numbering, signal)) return DefaultAction::stop;
    return DefaultAction::terminate;
}

// Realtime numbers (32 and up) are shared up to the smaller maximum. Returns 0
// when the signal is out of range or has no counterpart in `to`.
[[nodiscard]] constexpr u32 translate(const Numbering &from, const Numbering &to, u32 signal)
{
    if (!signal || signal > from.max) return 0;
    if (signal >= 32) return signal <= to.max ? signal : 0;
    const u32 generic_number = from.to_generic[signal];
    if (!generic_number) return 0;
    for (u32 candidate = 1; candidate < 32; ++candidate)
        if (to.to_generic[candidate] == generic_number) return candidate;
    return 0;
}

} // namespace ok::user::job
