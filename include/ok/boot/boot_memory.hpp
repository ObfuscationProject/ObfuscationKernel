#pragma once

#include "ok/boot/qemu_process_memory.hpp"
#include "ok/memory/physical_map.hpp"

#include <string_view>

namespace ok::boot
{

// Kernel-lifetime memory policy. It starts from the QEMU entry contract and
// narrows the managed window to RAM the supervisor keeps reaching after the
// execution backend initializes and that the backend accepts as user backing.
// Every qualifying page is managed; RAM outside the window stays recorded as
// excluded. `bootstrap` is the boot stack and page-table section.
template <arch::Architecture A>
Status kernel_memory_plan(const FirmwareMemory &source, PhysicalRange linked_image, PhysicalRange bootstrap,
                          QemuEntryMemoryPlan &output)
{
    using enum arch::Architecture;
    QemuEntryMemoryPlan candidate;
    if (auto status = qemu_entry_memory_plan<A>(source, linked_image, {}, ~u64{0} / 4096, candidate); !status)
        return status;
    auto &policy = candidate.policy;
    if constexpr (A == i386 || A == x86_64)
        policy.accessible = {0, 0x40000000}; // Identity first GiB; backends map user frames below it.
    else if constexpr (A == aarch64)
        policy.accessible = {0x40000000, 0x40000000}; // Backend 1 GiB supervisor RAM block.
    else if constexpr (A == arm32)
        policy.accessible = {0x40000000, 0x10000000}; // Backend sections 1024..1279.
    else if constexpr (A == rv32)
        policy.accessible = {0, 0x100000000ULL}; // M-mode physical, native width.
    else if constexpr (A == rv64)
    {
        policy.accessible = {0, u64{1} << 56}; // M-mode physical, Sv39 PPN width.
        policy.address_ceiling = u64{1} << 56;
    }
    else if constexpr (A == ppc)
    {
        if (policy.accessible.size > 0x10000000) // Boot TLB1 256 MiB supervisor RAM.
            policy.accessible.size = 0x10000000;
    }
    // LoongArch keeps the DMW0 identity window; MIPS keeps KSEG0.
    policy.selection = ManagedSelection::require_all;
    policy.maximum_ranges = max_qualified_ranges;
    auto reserve = [&](PhysicalRange range, bool ram) -> Status {
        return candidate.reservations.push_back({range, ram});
    };
    // Physical page zero is never a frame: several backends reject it.
    if (auto status = reserve({0, 4096}, false); !status)
        return status;
    if (bootstrap.size)
        if (auto status = reserve(bootstrap, true); !status)
            return status;
    // Platform device windows. They lie outside declared RAM on these boards;
    // listing them keeps a firmware RAM claim from ever covering them.
    if constexpr (A == i386 || A == x86_64)
        if (auto status = reserve({0xfec00000, 0x01400000}, false); !status) // IOAPIC, LAPIC, firmware ROM.
            return status;
    if constexpr (A == aarch64 || A == arm32)
        if (auto status = reserve({0, 0x40000000}, false); !status) // virt flash, GIC, UART, virtio-mmio, PCIe.
            return status;
    if constexpr (A == rv32 || A == rv64)
        if (auto status = reserve({0x1000, 0x7ffff000}, false); !status) // virt CLINT, PLIC, UART, virtio, PCIe.
            return status;
    if constexpr (A == loongarch64)
        if (auto status = reserve({0x10000000, 0x70000000}, false); !status) // virt I/O, flash, PCIe windows.
            return status;
    if constexpr (A == ppc)
        if (auto status = reserve({0xc0000000, 0x10000000}, false); !status) // e500 PCI outbound window.
            return status;
    output = candidate;
    return Status::success();
}

[[nodiscard]] constexpr memory::MapSource map_source(FirmwareFormat format)
{
    switch (format)
    {
    case FirmwareFormat::e820:
        return memory::MapSource::e820;
    case FirmwareFormat::fdt:
        return memory::MapSource::fdt;
    case FirmwareFormat::board:
        return memory::MapSource::board_table;
    case FirmwareFormat::absent:
        break;
    }
    return memory::MapSource::synthetic;
}

// Normalized kernel map of the qualified managed RAM, in physical addresses.
// Loaded objects are recorded as reservations so the map keeps them visible.
inline Status kernel_physical_map(const QualifiedBootMemory &qualified, usize page_size, memory::PhysicalMemoryMap &map)
{
    if (!qualified.valid() || qualified.policy().page_size != page_size)
        return Status::invalid_argument("qualified boot memory does not match the kernel page size");
    const auto source = map_source(qualified.source().source.format);
    if (source == memory::MapSource::synthetic)
        return Status::invalid_argument("qualified boot memory has no firmware source");
    map.reset();
    for (const auto range : qualified.managed())
    {
        if (range.address > ~uptr{0} || range.size > ~usize{0} || range.size - 1 > ~uptr{0} - range.address)
            return Status::overflow("managed range is not natively representable");
        if (auto status = map.add({static_cast<uptr>(range.address), static_cast<usize>(range.size),
                                   memory::RegionType::usable}, source); !status.ok())
            return status;
    }
    for (const auto &object : qualified.loaded())
    {
        // Reservations beyond the native width cannot overlap managed RAM.
        if (object.range.address > ~uptr{0} || object.range.size - 1 > ~uptr{0} - object.range.address)
            continue;
        if (auto status = map.reserve(static_cast<uptr>(object.range.address), static_cast<usize>(object.range.size),
                                      object.require_usable_ram ? memory::RegionType::kernel_image
                                                                : memory::RegionType::reserved); !status.ok())
            return status;
    }
    return map.normalize(page_size);
}

// The boot CPU discovers memory once, before any translation changes; the
// result is immutable afterwards. `map` is the input of MemoryManager.
struct KernelBootMemory
{
    Status status{Status::not_initialized("boot memory has not been discovered")};
    DiscoveryFailure failure{DiscoveryFailure::none};
    const QualifiedBootMemory *qualified{};
    const memory::PhysicalMemoryMap *map{};
    PhysicalRange image{};
    PhysicalRange bootstrap{};
    uptr direct_offset{};
};

using BootMemoryWrite = void (*)(std::string_view text);

// Freestanding kernels only. Emits OK_BOOT_MEMORY diagnostics through `write`.
const KernelBootMemory &discover_kernel_memory(BootMemoryWrite write);
[[nodiscard]] const KernelBootMemory &kernel_boot_memory();

} // namespace ok::boot
