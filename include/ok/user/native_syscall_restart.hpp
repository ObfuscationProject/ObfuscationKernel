#pragma once

#include "ok/abi/linux_native.hpp"
#include "ok/abi/linux_udp_call.hpp"

namespace ok::user::native
{

// Pure instruction/frame preparation. A Plan is NOT capture or wait authority.
// The runtime must separately bind the owned backend capture, lease, image,
// address space, sequence and wait, then publish through typed readiness.
template <abi::linux_user::Profile Selected>
    requires abi::linux_user::SupportedProfile<Selected>
struct RestartAdapter
{
    using Profile = abi::linux_user::Profile;
    using Frame = arch::execution::UserFrame;
    using Trap = arch::execution::Trap;
    using Bridge = abi::linux_user::NativeBridge<Selected>;
    using Codec = abi::linux_user::Codec<Selected>;
    enum class Family : u8 { none, pipe_read, futex_wait, child_wait, pipe_write, linux_udp_receive };
    struct Site { uptr instruction{}; u8 bytes{}; bool thumb{}; };
    struct Plan
    {
        Frame frame{};
        Site site{};
        std::array<u8, 4> opcode{};
        // PPC signal metadata is separate from UserFrame. The trusted readiness
        // transaction must publish this value with the exact prepared frame.
        u32 ppc_result{};
    };

    // Only operation-specific inputs are decoded. Supported FUTEX_WAIT uses
    // four register arguments even on O32; unused stack words are not inputs.
    [[nodiscard]] static Family family(const Frame &capture)
    {
        const auto call = Bridge::decode_call(capture, 4);
        if (!call) return Family::none;
        using Udp = abi::linux_user::UdpConstants<Selected>;
        const auto receive = Udp::syscall_number(abi::linux_user::UdpCall::recvfrom);
        const auto socketcall = Udp::socketcall_number();
        // Classification copies no socketcall array or O32 stacked words.
        if ((receive && call.value().number == *receive) ||
            (socketcall && call.value().number == *socketcall && call.value().arguments[0] == 12))
            return Family::linux_udp_receive;
        const auto read = Codec::syscall_number(abi::linux_user::Operation::read);
        const auto readv = Codec::syscall_number(abi::linux_user::Operation::readv);
        if ((read && call.value().number == *read) || (readv && call.value().number == *readv)) return Family::pipe_read;
        const auto write = Codec::syscall_number(abi::linux_user::Operation::write);
        const auto writev = Codec::syscall_number(abi::linux_user::Operation::writev);
        if ((write && call.value().number == *write) || (writev && call.value().number == *writev)) return Family::pipe_write;
        const auto wait4 = Codec::syscall_number(abi::linux_user::Operation::wait4);
        const auto waitid = Codec::syscall_number(abi::linux_user::Operation::waitid);
        if ((wait4 && call.value().number == *wait4) ||
            (Selected == Profile::rv32 && waitid && call.value().number == *waitid)) return Family::child_wait;
        const auto futex = Codec::syscall_number(abi::linux_user::Operation::futex);
        const auto time64 = Codec::syscall_number(abi::linux_user::Operation::futex_time64);
        if ((!futex || call.value().number != *futex) && (!time64 || call.value().number != *time64))
            return Family::none;
        const auto operation = static_cast<u32>(call.value().arguments[1]);
        return operation == 0 || operation == 128 ? Family::futex_wait : Family::none;
    }

    [[nodiscard]] static bool syscall_trap(const Trap &trap)
    {
        if (trap.kind != arch::execution::TrapKind::syscall) return false;
        if constexpr (Selected == Profile::x86_64)
            return trap.vector == 256 && trap.error_code == 0;
        else if constexpr (Selected == Profile::i386)
            return trap.vector == 128 && trap.error_code == 0;
        else if constexpr (Selected == Profile::aarch64)
            // The returned vector is ESR.EC, not raw assembly vector 8.
            return trap.vector == 0x15 && ((trap.error_code >> 26) & 0x3f) == 0x15;
        else if constexpr (Selected == Profile::arm32)
            return trap.vector == 2 && trap.error_code == 0;
        else if constexpr (Selected == Profile::rv32 || Selected == Profile::rv64)
            return trap.vector == 8 && trap.error_code == 8;
        else if constexpr (Selected == Profile::loongarch64)
            return trap.vector == 11 && trap.error_code == 0;
        else if constexpr (Selected == Profile::mips || Selected == Profile::mips64)
            return trap.vector == 8 && ((trap.error_code >> 2) & 31) == 8 &&
                !(trap.error_code & 0x80000000ULL);
        else
            return trap.vector == 8;
    }

    [[nodiscard]] static Result<Site> locate(const Frame &capture, const Trap &trap,
                                            uptr begin, uptr end)
    {
        if (!syscall_trap(trap)) return Status::unsupported("unsupported syscall restart origin");
        if constexpr (Codec::profile.word_bytes == 4)
        {
            if (capture.pc > Codec::word_mask || capture.sp > Codec::word_mask ||
                capture.flags > Codec::word_mask || capture.fs_base > Codec::word_mask ||
                capture.gs_base > Codec::word_mask)
                return Status::invalid_argument("restart capture exceeds profile word");
            for (auto value : capture.registers)
                if (value > Codec::word_mask)
                    return Status::invalid_argument("restart register exceeds profile word");
        }
        bool thumb = false;
        u8 bytes = 4, alignment = 4;
        if constexpr (Selected == Profile::x86_64 || Selected == Profile::i386)
        { bytes = 2; alignment = 1; }
        else if constexpr (Selected == Profile::arm32)
        {
            if ((capture.flags & 31) != 0x10 || capture.registers[15] != capture.pc)
                return Status::unsupported("restart requires canonical ARM user capture");
            thumb = (capture.flags & 0x20) != 0;
            bytes = alignment = thumb ? 2 : 4;
        }
        else if constexpr (Selected == Profile::rv32 || Selected == Profile::rv64)
            alignment = 2; // ECALL stays four bytes with compressed neighbours.
        if (begin >= end || capture.pc < begin || capture.pc > end ||
            capture.pc - begin < bytes || (capture.pc & (alignment - 1)))
            return Status::fault("restart instruction outside user range");
        return Site{capture.pc - bytes, bytes, thumb};
    }

    [[nodiscard]] static bool instruction(const Site &site, std::span<const u8> code)
    {
        constexpr bool x86 = Selected == Profile::x86_64 || Selected == Profile::i386;
        const usize expected = x86 || (Selected == Profile::arm32 && site.thumb) ? 2 : 4;
        if (code.size() != expected || code.size() != site.bytes) return false;
        if constexpr (Selected == Profile::x86_64)
            return code.size() == 2 && code[0] == 0x0f && code[1] == 0x05;
        else if constexpr (Selected == Profile::i386)
            return code.size() == 2 && code[0] == 0xcd && code[1] == 0x80;
        u32 word = 0;
        for (usize i = 0; i < code.size(); ++i)
            word |= static_cast<u32>(code[i]) << (8 * (Codec::profile.big_endian ? code.size() - 1 - i : i));
        if constexpr (Selected == Profile::aarch64)
            return (word & 0xffe0001fU) == 0xd4000001U;
        else if constexpr (Selected == Profile::arm32)
            return site.thumb ? code.size() == 2 && (word & 0xff00U) == 0xdf00U
                : code.size() == 4 && (word & 0x0f000000U) == 0x0f000000U && (word >> 28) != 15;
        else if constexpr (Selected == Profile::rv32 || Selected == Profile::rv64)
            return word == 0x00000073U;
        else if constexpr (Selected == Profile::loongarch64)
            return (word & 0xffff8000U) == 0x002b0000U;
        else if constexpr (Selected == Profile::mips || Selected == Profile::mips64)
            return (word & 0xfc00003fU) == 0x0000000cU;
        else if constexpr (Selected == Profile::ppc)
            return word == 0x44000002U;
        return false;
    }

    // Copy the ORIGINAL syscall-input frame. Never reconstruct input registers
    // from an EINTR/ERESTART result or a user-edited sigreturn frame.
    [[nodiscard]] static Result<Plan> prepare(const Frame &capture, const Trap &trap,
        std::span<const u8> code, uptr begin, uptr end)
    {
        const auto site = locate(capture, trap, begin, end);
        if (!site) return site.status();
        if (family(capture) == Family::none)
            return Status::unsupported("restart adapter requires an admitted blocking syscall family");
        if (!instruction(site.value(), code))
            return Status::unsupported("restart instruction encoding mismatch");
        Plan plan{capture, site.value(), {}, 0};
        for (usize i = 0; i < code.size(); ++i) plan.opcode[i] = code[i];
        plan.frame.pc = site.value().instruction;
        if constexpr (Selected == Profile::arm32) plan.frame.registers[15] = plan.frame.pc;
        if constexpr (Selected == Profile::ppc)
            // Linux leaves CR0.SO set after ERESTARTSYS and restores orig_r3.
            plan.frame.fp_state[Bridge::ppc_condition_offset] |= 0x10;
        return plan;
    }

    // The supplied callback must check executable permission and copy the whole
    // instruction through user access, including both pages for a crossing span.
    // This does not grant provenance to an arbitrary callback or synthetic Trap.
    template <typename CopyExecutable>
        requires requires(CopyExecutable &copy, uptr address, std::span<u8> bytes) {
            { copy(address, bytes) } -> std::same_as<Status>;
        }
    [[nodiscard]] static Result<Plan> prepare_copy(const Frame &capture, const Trap &trap,
        uptr begin, uptr end, CopyExecutable &&copy)
    {
        const auto site = locate(capture, trap, begin, end);
        if (!site) return site.status();
        std::array<u8, 4> code{};
        const auto bytes = std::span<u8>{code}.first(site.value().bytes);
        if (auto status = copy(site.value().instruction, bytes); !status) return status;
        return prepare(capture, trap, bytes, begin, end);
    }
};

} // namespace ok::user::native
