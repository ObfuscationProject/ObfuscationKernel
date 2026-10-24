#pragma once

#include "ok/arch/execution.hpp"

namespace ok::arch::execution
{

// The ARMv7 D32 backend retains actual user FPEXC after D0-D31 and FPSCR.
// Pending/asynchronous VFP exception state and FPINST are not admitted by this
// synchronous profile. Kernel FPEXC is saved separately on the privileged stack.
struct Arm32FpState
{
    static constexpr usize fpscr_offset = 256;
    static constexpr usize fpexc_offset = 260;
    static constexpr u32 enabled = 0x40000000;

    static u32 fpexc(const UserFrame &frame)
    {
        u32 value{};
        for (usize i = 0; i < 4; ++i) value |= u32{frame.fp_state[fpexc_offset+i]} << (8*i);
        return value;
    }
    static void set_fpexc(UserFrame &frame, u32 value)
    {
        for (usize i = 0; i < 4; ++i) frame.fp_state[fpexc_offset+i] = static_cast<u8>(value >> (8*i));
    }
    static bool admitted(const UserFrame &frame) { return fpexc(frame) == enabled; }
};

} // namespace ok::arch::execution
