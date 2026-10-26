#pragma once

#include "ok/boot/qualified_memory.hpp"

namespace ok::boot
{
// These are entry-phase QEMU contracts, before native VM setup changes the
// mappings. They never establish firmware RAM or physical-machine support.
struct QemuEntryMemoryPlan
{
    ManagedMemoryPolicy policy{};
    StaticVector<LoadedReservation, max_loaded_reservations> reservations;
};

// linked_image includes all linked Kernel/OS text, data, BSS, bootstrap page
// tables and stack, using physical linker bounds. External loaded objects must
// be supplied explicitly; empty extras means the boot recipe loads none.
template <arch::Architecture A>
Status qemu_entry_memory_plan(const FirmwareMemory &source, PhysicalRange linked_image,
                             std::span<const LoadedReservation> extra_objects, u64 maximum_pages,
                             QemuEntryMemoryPlan &output)
{
    using enum arch::Architecture;
    using namespace qualified_detail;
    if (source.source.architecture != A || !valid_range(linked_image) || !maximum_pages ||
        maximum_pages > ~u64{0}/4096 || extra_objects.size() > max_loaded_reservations-2)
        return Status::invalid_argument("entry memory identity, image or capacity invalid");
    QemuEntryMemoryPlan candidate;
    auto &policy = candidate.policy;
    policy.page_size = 4096;
    policy.maximum_pages = maximum_pages;
    policy.selection = ManagedSelection::lowest_addresses;
    // A deliberately bounded initial managed subset. Higher or unmapped RAM is
    // preserved in source/exclusions for a future general mapping backend.
    if constexpr (A == x86_64 || A == i386)
    {
        if (source.source.protocol != BootProtocol::bios_e820 || source.source.format != FirmwareFormat::e820 ||
            linked_image.address != 0x100000)
            return Status::invalid_argument("entry does not match BIOS image contract");
        policy.accessible = {0, A == x86_64 ? 0x40000000ULL : 0x100000000ULL};
        policy.address_ceiling = 0x100000000ULL;
        // Preserve the fixed BIOS payload allocation, including trailing pad.
        if (end(linked_image) < 0x500000) linked_image.size = 0x500000-linked_image.address;
        if (auto status = candidate.reservations.push_back({{0,0x100000},false}); !status) return status;
    }
    else if constexpr (A == arm32)
    {
        if (source.source.protocol != BootProtocol::arm32_linux_image || source.source.format != FirmwareFormat::fdt ||
            linked_image.address != 0x40010000)
            return Status::invalid_argument("entry does not match ARM Linux-header contract");
        policy.accessible = {0x40000000,0x20000000};
        policy.address_ceiling = 0x100000000ULL;
        if (auto status = candidate.reservations.push_back({{0x40000000,0x10000},false}); !status) return status;
    }
    else if constexpr (A == aarch64)
    {
        if (source.source.protocol != BootProtocol::aarch64_linux_image || source.source.format != FirmwareFormat::fdt ||
            linked_image.address != 0x40080000)
            return Status::invalid_argument("entry does not match AArch64 Linux Image contract");
        policy.accessible = {0x40000000,0x20000000};
        policy.address_ceiling = 0x100000000ULL;
        if (auto status = candidate.reservations.push_back({{0x40000000,0x80000},false}); !status) return status;
    }
    else if constexpr (A == rv64 || A == rv32)
    {
        if (source.source.protocol != BootProtocol::riscv_fdt || source.source.format != FirmwareFormat::fdt ||
            linked_image.address != 0x80000000)
            return Status::invalid_argument("entry does not match RISC-V no-firmware ELF contract");
        policy.accessible = {0x80000000,0x20000000};
        policy.address_ceiling = 0x100000000ULL;
    }
    else if constexpr (A == loongarch64)
    {
        // QEMU virt enters in direct-address mode with the image in high RAM.
        if (source.source.protocol != BootProtocol::loongarch64_efi_fdt || source.source.format != FirmwareFormat::fdt ||
            linked_image.address != 0x90000000)
            return Status::invalid_argument("entry does not match LoongArch EFI/FDT contract");
        policy.accessible = {0, u64{1} << 48};
        policy.address_ceiling = u64{1} << 48;
        // The command line is loader data outside the decoded tables.
        if (auto status = candidate.reservations.push_back(
                {{source.source.arguments[1] & ~u64{4095}, 4096}, false}); !status) return status;
    }
    else if constexpr (A == mips || A == mips64)
    {
        // Physical image bounds; the kernel runs from their KSEG0 alias.
        if (source.source.protocol != BootProtocol::malta_prom || source.source.format != FirmwareFormat::board ||
            linked_image.address != 0x10000)
            return Status::invalid_argument("entry does not match Malta PROM board contract");
        policy.accessible = {0, malta_kseg_size};
        policy.address_ceiling = 0x100000000ULL;
    }
    else if constexpr (A == ppc)
    {
        if (source.source.protocol != BootProtocol::epapr || source.source.format != FirmwareFormat::fdt ||
            linked_image.address != 0x1000000 || !source.source.initial_mapping_end ||
            source.source.initial_mapping_end > 0x100000000ULL)
            return Status::invalid_argument("entry does not match ePAPR mapping contract");
        policy.accessible = {0,source.source.initial_mapping_end};
        policy.address_ceiling = 0x100000000ULL;
        if (auto status = candidate.reservations.push_back({{0,linked_image.address},false}); !status) return status;
    }
    if (linked_image.address < policy.accessible.address || end(linked_image) > end(policy.accessible))
        return Status::fault("complete loaded image exceeds entry mapping");
    if (auto status = candidate.reservations.push_back({linked_image,true}); !status) return status;
    for (const auto object : extra_objects)
    {
        if (!valid_range(object.range)) return Status::invalid_argument("external loaded object wraps or is empty");
        if (auto status = candidate.reservations.push_back(object); !status) return status;
    }
    output = candidate;
    return Status::success();
}
} // namespace ok::boot
