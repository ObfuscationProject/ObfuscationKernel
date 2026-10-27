#include "ok/boot/boot_memory.hpp"
#include "ok/arch/arch.hpp"

namespace ok::boot
{
namespace
{

KernelBootMemory result;

#ifdef OK_KERNEL_FREESTANDING
constexpr auto boot_arch = arch::configured_architecture();
// Serialized boot-CPU storage; large records stay out of the boot stack.
FirmwareMemory firmware_workspace, firmware_memory;
QualifiedBootMemory qualified_workspace, qualified_memory;
memory::PhysicalMemoryMap kernel_map;
bool attempted{};
extern "C" char _kernel_start[], _kernel_end[], _kernel_bootstrap_start[], _kernel_bootstrap_end[];

void hex(BootMemoryWrite write, u64 value)
{
    constexpr char digits[] = "0123456789abcdef";
    char text[18] = {'0', 'x'};
    for (usize i = 0; i < 16; ++i)
        text[2 + i] = digits[(value >> ((15 - i) * 4)) & 15];
    write({text, sizeof(text)});
}

void decimal(BootMemoryWrite write, u64 value)
{
    char digits[20];
    usize count = 0;
    do
    {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    while (count)
        write({&digits[--count], 1});
}

void range(BootMemoryWrite write, std::string_view prefix, PhysicalRange value)
{
    write(prefix);
    write(" base=");
    hex(write, value.address);
    write(" size=");
    hex(write, value.size);
}

std::string_view protocol_name(BootProtocol protocol)
{
    switch (protocol)
    {
    case BootProtocol::bios_e820: return "bios-e820";
    case BootProtocol::aarch64_linux_image: return "aarch64-linux-image";
    case BootProtocol::riscv_fdt: return "riscv-fdt";
    case BootProtocol::epapr: return "epapr";
    case BootProtocol::arm32_direct_elf: return "arm32-direct-elf";
    case BootProtocol::loongarch64_direct_elf: return "loongarch64-direct-elf";
    case BootProtocol::malta_prom: return "malta-prom";
    case BootProtocol::arm32_linux_image: return "arm32-linux-image";
    case BootProtocol::loongarch64_efi_fdt: return "loongarch64-efi-fdt";
    case BootProtocol::unknown: break;
    }
    return "unknown";
}

std::string_view format_name(FirmwareFormat format)
{
    switch (format)
    {
    case FirmwareFormat::e820: return "e820";
    case FirmwareFormat::fdt: return "fdt";
    case FirmwareFormat::board: return "board";
    case FirmwareFormat::absent: break;
    }
    return "absent";
}

std::string_view exclusion_name(ExclusionReason reason)
{
    switch (reason)
    {
    case ExclusionReason::reserved: return "reserved";
    case ExclusionReason::unmapped: return "unmapped";
    case ExclusionReason::address_width: return "address-width";
    case ExclusionReason::page_edge: return "page-edge";
    case ExclusionReason::capacity: return "capacity";
    }
    return "unknown";
}

void report(BootMemoryWrite write)
{
    constexpr auto protocol = qemu_process_boot_protocol<boot_arch>();
    write("OK_BOOT_MEMORY version=1 protocol=");
    write(protocol_name(protocol));
    write(" source=");
    write(format_name(firmware_memory.source.format));
    write(result.status.ok() ? " status=pass" : " status=fail");
    write(" image_begin=");
    hex(write, result.image.address);
    write(" image_end=");
    hex(write, result.image.address + result.image.size);
    write(" direct_offset=");
    hex(write, result.direct_offset);
    if (!result.status.ok())
    {
        write(" message=");
        write(result.status.message());
        write("\n");
        return;
    }
    write(" managed_pages=");
    decimal(write, qualified_memory.page_count());
    write(" managed_ranges=");
    decimal(write, qualified_memory.managed().size());
    write(" excluded=");
    decimal(write, qualified_memory.excluded().size());
    write("\n");
    const auto &source = qualified_memory.source();
    range(write, "OK_BOOT_MEMORY_SOURCE", source.source_storage);
    write(" decoded=");
    hex(write, source.decoded_bytes);
    write("\n");
    for (usize i = 0; i < source.loader_storage_count; ++i)
    {
        range(write, "OK_BOOT_MEMORY_LOADER", source.loader_storage[i]);
        write("\n");
    }
    if (source.source.format == FirmwareFormat::board)
    {
        write("OK_BOOT_MEMORY_BOARD reported_low=");
        hex(write, source.board.reported_low);
        write(" reported_total=");
        hex(write, source.board.reported_total);
        write("\n");
    }
    for (const auto &object : qualified_memory.loaded())
    {
        range(write, "OK_BOOT_MEMORY_LOADED", object.range);
        write(object.require_usable_ram ? " ram=1\n" : " ram=0\n");
    }
    for (const auto managed : qualified_memory.managed())
    {
        range(write, "OK_BOOT_MEMORY_RANGE", managed);
        write("\n");
    }
    for (const auto &excluded : qualified_memory.excluded())
    {
        range(write, "OK_BOOT_MEMORY_EXCLUDED", excluded.range);
        write(" reason=");
        write(exclusion_name(excluded.reason));
        write("\n");
    }
}
#endif

} // namespace

const KernelBootMemory &discover_kernel_memory([[maybe_unused]] BootMemoryWrite write)
{
#ifdef OK_KERNEL_FREESTANDING
    if (attempted)
        return result;
    attempted = true;
    // Linker bounds are supervisor addresses; reservations are physical.
    constexpr uptr offset = qemu_entry_direct_offset<boot_arch>;
    const uptr first = reinterpret_cast<uptr>(_kernel_start) - offset;
    const uptr last = reinterpret_cast<uptr>(_kernel_end) - offset;
    const uptr bootstrap_first = reinterpret_cast<uptr>(_kernel_bootstrap_start) - offset;
    const uptr bootstrap_last = reinterpret_cast<uptr>(_kernel_bootstrap_end) - offset;
    result.image = {first, last - first};
    result.bootstrap = {bootstrap_first, bootstrap_last - bootstrap_first};
    result.direct_offset = offset;
    if (last <= first || bootstrap_first < first || bootstrap_last > last || bootstrap_last < bootstrap_first)
        result.status = Status::fault("invalid final linker bounds");
    else
        result.status = Status::success();
    if constexpr (boot_arch == arch::Architecture::arm32)
    {
        const auto &entry = arm32_entry_state();
        write("OK_ARM32_ENTRY cpsr=");
        hex(write, entry.cpsr);
        write(" sctlr=");
        hex(write, entry.sctlr);
        write(" mpidr=");
        hex(write, entry.mpidr);
        write(valid_arm32_entry(entry) ? " status=pass\n" : " status=fail\n");
        if (!valid_arm32_entry(entry))
            result.status = Status::fault("ARM loader entry state rejected");
    }
    if (result.status.ok())
    {
        // The reader is the entry mapping authority; nothing has replaced it yet.
        QemuProcessMemoryReader<boot_arch> reader{raw_boot_handoff().arguments[2]};
        const auto discovery = discover_firmware_memory<boot_arch>(
            raw_boot_handoff(), qemu_process_boot_protocol<boot_arch>(), reader, firmware_workspace, firmware_memory);
        result.status = discovery.status;
        result.failure = discovery.failure;
    }
    QemuEntryMemoryPlan plan;
    if (result.status.ok())
        result.status = kernel_memory_plan<boot_arch>(firmware_memory, result.image, result.bootstrap, plan);
    if (result.status.ok())
        result.status = qualify_boot_memory(firmware_memory, {plan.reservations.begin(), plan.reservations.size()},
                                            plan.policy, qualified_workspace, qualified_memory);
    if (result.status.ok())
        result.status = kernel_physical_map(qualified_memory, 4096, kernel_map);
    if (result.status.ok())
    {
        result.qualified = &qualified_memory;
        result.map = &kernel_map;
    }
    report(write);
#else
    result.status = Status::unsupported("hosted builds do not discover boot memory");
#endif
    return result;
}

const KernelBootMemory &kernel_boot_memory()
{
    return result;
}

} // namespace ok::boot
