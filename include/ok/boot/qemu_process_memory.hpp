#pragma once
#include "ok/boot/qemu_runtime_frames.hpp"

namespace ok::boot
{
template <arch::Architecture A> inline constexpr bool qemu_firmware_process_pool =
    A == arch::Architecture::i386 || A == arch::Architecture::x86_64 || A == arch::Architecture::aarch64 ||
    A == arch::Architecture::arm32 || A == arch::Architecture::rv32 || A == arch::Architecture::rv64 || A == arch::Architecture::ppc;

// This is the existing QEMU boot recipe's entry mapping authority. It must be
// used before debug tests or native initialization replace entry translations.
template <arch::Architecture A> struct QemuProcessMemoryReader
{
    u64 epapr_end{};
    Result<std::span<const std::byte>> read_physical(u64 address, usize bytes)
    {
        if constexpr (A == arch::Architecture::arm32)
            if (!valid_arm32_entry(arm32_entry_state()))
                return Status::fault("ARM loader state is not qualified for physical access");
        u64 first = 0, last = 0;
        if constexpr (A == arch::Architecture::i386) last = 0x100000000ULL;
        else if constexpr (A == arch::Architecture::x86_64) last = 0x40000000;
        else if constexpr (A == arch::Architecture::aarch64 || A == arch::Architecture::arm32) { first = 0x40000000; last = 0x60000000; }
        else if constexpr (A == arch::Architecture::rv32 || A == arch::Architecture::rv64)
        { first = 0x80000000; last = 0xa0000000; }
        else if constexpr (A == arch::Architecture::ppc) last = epapr_end;
        if (!bytes || address < first || address >= last || u64(bytes) > last-address ||
            address > ~uptr{0} || u64(bytes-1) > ~uptr{0}-address)
            return Status::fault("firmware source outside QEMU entry mapping");
        return std::span<const std::byte>{reinterpret_cast<const std::byte *>(static_cast<uptr>(address)),bytes};
    }
};
template <arch::Architecture A> constexpr BootProtocol qemu_process_boot_protocol()
{
    if constexpr (A == arch::Architecture::i386 || A == arch::Architecture::x86_64) return BootProtocol::bios_e820;
    if constexpr (A == arch::Architecture::arm32) return BootProtocol::arm32_linux_image;
    if constexpr (A == arch::Architecture::aarch64) return BootProtocol::aarch64_linux_image;
    if constexpr (A == arch::Architecture::rv32 || A == arch::Architecture::rv64) return BootProtocol::riscv_fdt;
    if constexpr (A == arch::Architecture::ppc) return BootProtocol::epapr;
    return BootProtocol::unknown;
}
} // namespace ok::boot
