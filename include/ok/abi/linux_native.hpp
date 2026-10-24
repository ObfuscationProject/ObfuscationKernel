#pragma once

#include "ok/abi/linux_user.hpp"
#include "ok/arch/arch.hpp"
#include "ok/arch/execution.hpp"

#include <concepts>

namespace ok::abi::linux_user
{

struct ThreadPointer
{
    u64 value{0};
};
enum class ThreadPointerRoute : u8
{
    restored_register,
    mips_rdhwr_emulation,
};

template <typename Copy>
concept StackArgumentCopy = requires(Copy &copy, uptr source, std::span<u8> destination) {
    { copy(source, destination) } -> std::same_as<Status>;
};

// The caller selects the already-qualified personality and the syscall's raw
// argument count. This adapter never infers signatures or dereferences userspace.
// Source layout audit: docs/abi/linux-native-bridge.md.
template <Profile Selected>
    requires SupportedProfile<Selected>
class NativeBridge final
{
  public:
    using Frame = arch::execution::UserFrame;
    using UserCodec = Codec<Selected>;
    static constexpr const auto &profile = UserCodec::profile;
    static constexpr u64 word_mask = UserCodec::word_mask;
    static_assert(sizeof(uptr) >= profile.word_bytes, "native frame cannot contain this profile's register words");
    static constexpr usize ppc_condition_offset = 280; // Within fp_state; NOT frame.flags (MSR).
    static constexpr u32 i386_tls_entry = 7;
    static constexpr ThreadPointerRoute thread_pointer_route = Selected == Profile::mips || Selected == Profile::mips64
                                                                   ? ThreadPointerRoute::mips_rdhwr_emulation
                                                                   : ThreadPointerRoute::restored_register;

    [[nodiscard]] static RegisterFile snapshot(const Frame &frame)
    {
        RegisterFile result{};
        if constexpr (Selected == Profile::i386)
        {
            // pushal: EDI, ESI, EBP, kernel ESP, EBX, EDX, ECX, EAX;
            // selectors in slots 8..12 are not integer registers.
            constexpr usize slots[]{7, 6, 5, 4, 0, 2, 1, 0};
            for (usize i = 0; i < 8; ++i)
                result.integer[i] = word(i == 4 ? frame.sp : frame.registers[slots[i]]);
        }
        else if constexpr (Selected == Profile::x86_64)
        {
            // SAVE_GPRS order -> DWARF GPR order. Architectural RSP is separate.
            constexpr usize slots[]{14, 11, 12, 13, 10, 9, 8, 0, 7, 6, 5, 4, 3, 2, 1, 0};
            for (usize i = 0; i < 16; ++i)
                result.integer[i] = word(i == 7 ? frame.sp : frame.registers[slots[i]]);
        }
        else if constexpr (Selected == Profile::aarch64)
        {
            for (usize i = 0; i < 31; ++i)
                result.integer[i] = word(frame.registers[i]);
            // Slot 31 stores packed FPCR/FPSR, not XZR or SP.
        }
        else if constexpr (Selected == Profile::arm32)
        {
            for (usize i = 0; i < 15; ++i)
                result.integer[i] = word(frame.registers[i]);
            result.integer[13] = word(frame.sp);
            result.integer[15] = word(frame.pc);
        }
        else
        {
            for (usize i = 0; i < 32; ++i)
                result.integer[i] = word(frame.registers[i]);
            if constexpr (Selected == Profile::ppc)
            {
                result.integer[1] = word(frame.sp);
                result.integer[2] = word(frame.fs_base);
                result.integer[13] = word(frame.gs_base);
                result.condition = ppc_condition(frame);
            }
            else
            {
                result.integer[0] = 0;
                if constexpr (Selected == Profile::rv32 || Selected == Profile::rv64)
                    result.integer[2] = word(frame.sp);
                else if constexpr (Selected == Profile::loongarch64)
                    result.integer[3] = word(frame.sp);
                else
                {
                    // K0/K1 are trap scratch and are not saved user registers.
                    result.integer[26] = result.integer[27] = 0;
                    result.integer[29] = word(frame.sp);
                }
            }
        }
        return result;
    }

    [[nodiscard]] static Result<Call> decode_call(const Frame &frame, usize count,
                                                  std::span<const u8> copied_stack_from_sp = {})
    {
        return UserCodec::decode_call(snapshot(frame), count, copied_stack_from_sp);
    }

    // Copy only O32 words actually requested, not the unused argument home area.
    // The callback must validate and copy user memory under the caller's process
    // lifetime contract; a successful callback must fill the complete span.
    template <StackArgumentCopy Copy>
    [[nodiscard]] static Result<Call> decode_call_copying_stack(const Frame &frame, usize count, Copy &&copy)
    {
        if (count > 6)
            return Status::unsupported("Linux native bridge supports six raw arguments");
        if constexpr (Selected != Profile::mips)
            return decode_call(frame, count);
        else
        {
            if (count <= 4)
                return decode_call(frame, count);
            const u64 sp = word(frame.sp);
            const usize size = (count - 4) * 4;
            if (sp > word_mask - 16 || size - 1 > word_mask - (sp + 16))
                return Status::fault("Linux O32 stack argument address wraps");
            std::array<u8, 24> copied{};
            if (auto status = copy(static_cast<uptr>(sp + 16), std::span<u8>(copied).subspan(16, size)); !status.ok())
                return status;
            return decode_call(frame, count, std::span<const u8>(copied).first(count * 4));
        }
    }

    [[nodiscard]] static Result<Return> decode_return(const Frame &frame)
    {
        return UserCodec::decode_return(snapshot(frame));
    }

    static void set_stack_pointer(Frame &frame, uptr value)
    {
        frame.sp = value;
        if constexpr (Selected == Profile::arm32) frame.registers[13] = value;
        else if constexpr (Selected == Profile::rv32 || Selected == Profile::rv64) frame.registers[2] = value;
        else if constexpr (Selected == Profile::loongarch64) frame.registers[3] = value;
        else if constexpr (Selected == Profile::mips || Selected == Profile::mips64) frame.registers[29] = value;
        else if constexpr (Selected == Profile::ppc) frame.registers[1] = value;
    }

    [[nodiscard]] static Status encode_return(Frame &frame, Return value)
    {
        auto registers = snapshot(frame);
        if (auto status = UserCodec::encode_return(registers, value); !status.ok())
            return status;
        constexpr usize result_slot = Selected == Profile::i386     ? 7
                                      : Selected == Profile::x86_64 ? 14
                                                                    : profile.return_register;
        frame.registers[result_slot] = static_cast<uptr>(registers.integer[profile.return_register]);
        if constexpr (Selected == Profile::mips || Selected == Profile::mips64)
            frame.registers[7] = static_cast<uptr>(registers.integer[7]);
        if constexpr (Selected == Profile::ppc)
            for (usize i = 0; i < 4; ++i)
                frame.fp_state[ppc_condition_offset + i] = static_cast<u8>(registers.condition >> ((3 - i) * 8));
        return Status::success();
    }

    // This updates the saved state consumed by the existing execution backend.
    // It is not a set_thread_area/user_desc/clone decoder or a TLS allocator.
    // The caller validates the address against its user mapping/entry contract.
    // MIPS requires explicit opt-in to the existing real RI-trap emulation;
    // requesting hardware UserLocal restoration there returns unsupported.
    [[nodiscard]] static Status set_thread_pointer(Frame &frame, ThreadPointer pointer,
                                                   ThreadPointerRoute route = ThreadPointerRoute::restored_register)
    {
        if (route != thread_pointer_route)
            return Status::unsupported("requested Linux thread-pointer restoration route is unavailable");
        if (pointer.value > word_mask)
            return Status::overflow("Linux thread pointer exceeds target word size");
        const auto value = static_cast<uptr>(pointer.value);
        if constexpr (Selected == Profile::i386)
        {
            frame.gs_base = value;
            frame.registers[8] = (i386_tls_entry << 3) | 3;
        }
        else if constexpr (Selected == Profile::arm32)
            frame.gs_base = value; // TPIDRURO: c13,c0,3, read by musl's __get_tp.
        else if constexpr (Selected == Profile::rv32 || Selected == Profile::rv64)
            frame.registers[4] = value; // The real saved/restored x4/tp.
        else if constexpr (Selected == Profile::loongarch64 || Selected == Profile::ppc)
        {
            frame.registers[2] = value;
            frame.fs_base = value; // PPC entry reloads r2 from this mirror.
        }
        else
            frame.fs_base = value; // x86 FS, AArch64 TPIDR_EL0, or explicit MIPS emulation.
        return Status::success();
    }

  private:
    [[nodiscard]] static constexpr u64 word(uptr value)
    {
        return static_cast<u64>(value) & word_mask;
    }
    [[nodiscard]] static u32 ppc_condition(const Frame &frame)
    {
        u32 result = 0;
        for (usize i = 0; i < 4; ++i)
            result = (result << 8) | frame.fp_state[ppc_condition_offset + i];
        return result;
    }
};

[[nodiscard]] consteval Profile configured_native_profile()
{
    switch (arch::configured_architecture())
    {
    case arch::Architecture::i386:
        return Profile::i386;
    case arch::Architecture::x86_64:
        return Profile::x86_64;
    case arch::Architecture::aarch64:
        return Profile::aarch64;
    case arch::Architecture::arm32:
        return Profile::arm32;
    case arch::Architecture::rv64:
        return Profile::rv64;
    case arch::Architecture::rv32:
        return Profile::rv32;
    case arch::Architecture::loongarch64:
        return Profile::loongarch64;
    case arch::Architecture::mips:
        return Profile::mips;
    case arch::Architecture::mips64:
        return Profile::mips64;
    case arch::Architecture::ppc:
        return Profile::ppc;
    }
    return Profile::count;
}
using ConfiguredNativeBridge = NativeBridge<configured_native_profile()>;

} // namespace ok::abi::linux_user
