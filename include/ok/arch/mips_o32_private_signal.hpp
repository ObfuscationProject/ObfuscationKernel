#pragma once
#include "ok/arch/execution.hpp"

namespace ok::arch::execution::mips_o32_private
{
inline constexpr u32 user_status = 0x20008013;
inline constexpr u32 unsupported_fcsr = 0x007c0000;
struct Facts
{
    u32 prid{}, config0{}, config1{}, config2{}, config3{}, status{}, fir{}, fcsr{};
    bool has_config1{}, has_config2{}, has_config3{};
};
// This deliberately narrow profile is private to the Debug signal experiment.
// FIR.F64 is a capability; it does not imply live Status.FR=1.
[[nodiscard]] constexpr bool platform(const Facts &f) noexcept
{
    return f.prid == 0x00019300 && f.config0 == 0x80008482 &&
           f.has_config1 && f.config1 == 0x9e190c8f &&
           f.has_config2 && f.config2 == 0x80000000 &&
           f.has_config3 && f.config3 == 0;
}
[[nodiscard]] constexpr bool qualified(const Facts &f) noexcept
{
    return platform(f) && f.status == 0x20000000 && f.fir == 0x00739300 && f.fcsr == 0;
}
[[nodiscard]] constexpr u32 fcsr(const UserFrame &f) noexcept
{
    return (u32{f.fp_state[256]} << 24) | (u32{f.fp_state[257]} << 16) |
           (u32{f.fp_state[258]} << 8) | f.fp_state[259];
}
[[nodiscard]] constexpr bool frame_mode(const UserFrame &f) noexcept
{
    return f.flags == user_status && !(fcsr(f) & unsupported_fcsr);
}
[[nodiscard]] constexpr bool delay_operands_admitted(u32 instruction) noexcept
{
    const u32 op = instruction >> 26, rs = (instruction >> 21) & 31;
    const u32 rt = (instruction >> 16) & 31, rd = (instruction >> 11) & 31;
    const auto retained = [](u32 reg) { return reg != 26 && reg != 27; };
    switch (op)
    {
    case 0:
        return retained(rs) && ((instruction & 63) == 8 ||
            ((instruction & 63) == 9 && rs != rd));
    case 2: case 3: return true;
    case 4: case 5: case 20: case 21: return retained(rs) && retained(rt);
    case 6: case 7: case 22: case 23: return retained(rs);
    case 1:
        return retained(rs) && (rt == 0 || rt == 1 || rt == 2 || rt == 3 ||
            ((rt == 16 || rt == 17 || rt == 18 || rt == 19) && rs != 31));
    case 17: return rs == 8;
    default: return false;
    }
}

class CaptureTracker;
class Capability final
{
    bool admitted_{};
    u64 sequence_{};
    explicit constexpr Capability(bool admitted, u64 sequence) noexcept : admitted_(admitted), sequence_(sequence) {}
    friend class CaptureTracker;
  public:
    constexpr Capability() noexcept = default;
    [[nodiscard]] constexpr bool admitted() const noexcept { return admitted_; }
    [[nodiscard]] constexpr u64 sequence() const noexcept { return sequence_; }
};
enum class CaptureKind : u8 { unverified, syscall_normalized, timer_non_bd, timer_bd };
class Observation final
{
    const UserFrame *frame_{};
    u64 sequence_{};
    CaptureKind kind_{};
    friend class CaptureTracker;
  public:
    [[nodiscard]] constexpr bool matches(const UserFrame &frame) const noexcept
    { return frame_ == &frame && sequence_ != 0; }
    [[nodiscard]] constexpr u64 sequence() const noexcept { return sequence_; }
    [[nodiscard]] constexpr CaptureKind kind() const noexcept { return kind_; }
};
// Only the backend instance supplies observations to runtime. The public pure
// class also permits direct lifecycle testing without mocking exported frames.
class CaptureTracker final
{
    bool admitted_{};
    u64 sequence_{};
    Observation observation_{};
  public:
    [[nodiscard]] constexpr bool qualify(const Facts &facts) noexcept
    {
        observation_ = {};
        admitted_ = qualified(facts);
        return admitted_;
    }
    [[nodiscard]] constexpr Capability capability() const noexcept { return Capability(admitted_, sequence_); }
    constexpr void invalidate() noexcept
    {
        observation_ = {};
        if (sequence_ == ~u64{}) admitted_ = false;
        else ++sequence_;
    }
    constexpr void publish(const UserFrame &frame, CaptureKind kind) noexcept
    {
        observation_ = {};
        if (!admitted_ || !sequence_ || !frame_mode(frame) || kind == CaptureKind::unverified) return;
        observation_.frame_ = &frame;
        observation_.sequence_ = sequence_;
        observation_.kind_ = kind;
    }
    [[nodiscard]] constexpr Observation query(const UserFrame &frame) const noexcept
    {
        return admitted_ && frame_mode(frame) && observation_.matches(frame) ? observation_ : Observation{};
    }
};

// Defined only by the opted-in native o32 backend. Query immediately after
// owned trap/capture validation, before another entry or frame publication.
[[nodiscard]] Capability capability() noexcept;
[[nodiscard]] Observation observation(const UserFrame &) noexcept;
[[nodiscard]] Facts facts() noexcept;
} // namespace ok::arch::execution::mips_o32_private
