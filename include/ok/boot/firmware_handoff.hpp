#pragma once

#include "ok/arch/types.hpp"
#include "ok/boot/e820.hpp"
#include "ok/boot/fdt.hpp"
#include "ok/boot/loader_tables.hpp"
#include "ok/boot/raw_handoff.hpp"

#include <concepts>

namespace ok::boot
{

// The caller selects an established loader contract. Architecture alone cannot
// identify a firmware protocol, and a plausible pointer does not establish one.
enum class BootProtocol : u8
{
    unknown,
    bios_e820,
    aarch64_linux_image,
    riscv_fdt,
    epapr,
    arm32_direct_elf,
    loongarch64_direct_elf,
    malta_prom,
    arm32_linux_image,
    loongarch64_efi_fdt,
};

// `board` is a fixed platform table sized by values the loader reports; it is
// not a firmware map format and records which facts came from the loader.
enum class FirmwareFormat : u8 { absent, e820, fdt, board };
enum class DiscoveryFailure : u8
{
    none,
    not_captured,
    source_absent,
    unsupported_protocol,
    protocol_mismatch,
    unmapped_source,
    incomplete_source,
    source_capacity_exceeded,
    malformed_source,
    parser_rejected,
    invalid_workspace,
};

struct DiscoveryResult
{
    Status status{};
    DiscoveryFailure failure{DiscoveryFailure::none};
    u32 diagnostic_record_count{};
    [[nodiscard]] explicit operator bool() const { return status.ok(); }
};

struct BootSource
{
    arch::Architecture architecture{};
    BootProtocol protocol{BootProtocol::unknown};
    FirmwareFormat format{FirmwareFormat::absent};
    std::array<u64, 4> arguments{};
    u64 physical_address{};
    u64 initial_mapping_end{}; // ePAPR r7; zero means the reader owns this limit.
};

struct BoardMemoryMap
{
    static constexpr usize max_ranges = 4;
    std::array<PhysicalRange, max_ranges> ram{};
    std::array<PhysicalRange, max_ranges> reserved{};
    std::array<PhysicalRange, max_ranges> devices{};
    usize ram_count{};
    usize reserved_count{};
    usize device_count{};
    u64 reported_low{};   // Loader-reported RAM below the board I/O hole.
    u64 reported_total{}; // Loader-reported total RAM.
};

// Complete owned parser output. This is not an allocator input: platform RAM
// qualification, all loaded-object reservations and managed-memory policy are
// still required. Retain the original format-specific records and attributes.
struct FirmwareMemory
{
    BootSource source{};
    PhysicalRange source_storage{}; // Full loader-owned allocation to reserve.
    // Loader tables read on the way to the source (EFI system/configuration tables).
    std::array<PhysicalRange, 2> loader_storage{};
    usize loader_storage_count{};
    usize decoded_bytes{}; // Header and records read; may be smaller than storage.
    E820Map e820{};
    FdtMemoryMap fdt{};
    BoardMemoryMap board{};
};

// Readers must return a stable, already mapped, exactly sized readable window,
// or failure. They own physical-to-virtual translation and mapping authority.
// Returning a span made by unchecked pointer conversion does not meet this
// contract. Firmware must remain immutable throughout the discovery call.
template <typename Reader>
concept FirmwareReader = requires(Reader &reader, u64 address, usize bytes) {
    { reader.read_physical(address, bytes) } -> std::same_as<Result<std::span<const std::byte>>>;
};

namespace handoff_detail
{
inline DiscoveryResult fail(DiscoveryFailure reason, Status status, u32 count = 0)
{
    return {status, reason, count};
}
inline u32 word(std::span<const std::byte> bytes, usize offset, bool big_endian)
{
    u32 value{};
    for (usize i = 0; i < 4; ++i)
        value |= std::to_integer<u32>(bytes[offset + i]) << ((big_endian ? 3 - i : i) * 8);
    return value;
}

template <arch::Architecture A>
DiscoveryResult select(const RawBootHandoff &raw, BootProtocol protocol, BootSource &output)
{
    using enum arch::Architecture;
    if (!raw.captured)
        return fail(DiscoveryFailure::not_captured, Status::not_initialized("boot arguments not captured"));
    BootSource candidate{A, protocol};
    for (usize i = 0; i < raw.arguments.size(); ++i)
        candidate.arguments[i] = raw.arguments[i];
    const auto &arguments = candidate.arguments;
    bool matched = false;
    if constexpr (A == i386 || A == x86_64)
    {
        if (protocol == BootProtocol::bios_e820)
        {
            matched = true;
            if (arguments[0] == 0 && arguments[1] == 0)
                return fail(DiscoveryFailure::source_absent, Status::not_found("BIOS E820 source absent"));
            if (arguments[0] != e820_handoff_magic || arguments[1] != 0x5000 || arguments[2] || arguments[3])
                return fail(DiscoveryFailure::protocol_mismatch, Status::invalid_argument("BIOS E820 tag or table mismatch"));
            candidate.format = FirmwareFormat::e820;
            candidate.physical_address = 0x5000;
        }
    }
    else if constexpr (A == aarch64)
    {
        if (protocol == BootProtocol::aarch64_linux_image)
        {
            matched = true;
            if (arguments[1] || arguments[2] || arguments[3])
                return fail(DiscoveryFailure::protocol_mismatch, Status::invalid_argument("Linux Image reserved argument nonzero"));
            candidate.format = FirmwareFormat::fdt;
            candidate.physical_address = arguments[0];
        }
    }
    else if constexpr (A == rv32 || A == rv64)
    {
        if (protocol == BootProtocol::riscv_fdt)
        {
            matched = true;
            candidate.format = FirmwareFormat::fdt;
            candidate.physical_address = arguments[1];
        }
    }
    else if constexpr (A == ppc)
    {
        if (protocol == BootProtocol::epapr)
        {
            matched = true;
            if (arguments[1] != 0x45504150 || arguments[3] || arguments[0] > 0xffffffff ||
                arguments[2] > 0xffffffff || arguments[2] == 0)
                return fail(DiscoveryFailure::protocol_mismatch, Status::invalid_argument("ePAPR magic or mapping extent invalid"));
            candidate.format = FirmwareFormat::fdt;
            candidate.physical_address = arguments[0];
            candidate.initial_mapping_end = arguments[2];
        }
    }
    else if constexpr (A == arm32)
    {
        if (protocol == BootProtocol::arm32_direct_elf && arguments == std::array<u64, 4>{})
            return fail(DiscoveryFailure::source_absent, Status::not_found("direct ELF supplied no firmware source"));
        if (protocol == BootProtocol::arm32_linux_image)
        {
            matched = true;
            if (arguments[0] || arguments[1] != 0xffffffff || arguments[2] > 0xffffffff)
                return fail(DiscoveryFailure::protocol_mismatch, Status::invalid_argument("ARM Linux header arguments invalid"));
            candidate.format = FirmwareFormat::fdt;
            candidate.physical_address = arguments[2];
            // r3 is unspecified by Linux's ARM boot convention.
        }
    }
    else if constexpr (A == loongarch64)
    {
        if (protocol == BootProtocol::loongarch64_direct_elf && arguments == std::array<u64, 4>{})
            return fail(DiscoveryFailure::source_absent, Status::not_found("direct ELF supplied no firmware source"));
        // Linux LoongArch boot: a0 = 1 (EFI), a1 = command line, a2 = system table.
        if (protocol == BootProtocol::loongarch64_efi_fdt)
        {
            matched = true;
            if (arguments == std::array<u64, 4>{})
                return fail(DiscoveryFailure::source_absent, Status::not_found("loader supplied no EFI system table"));
            if (arguments[0] != 1 || (arguments[2] & 7))
                return fail(DiscoveryFailure::protocol_mismatch, Status::invalid_argument("EFI boot flag or system table invalid"));
            candidate.format = FirmwareFormat::fdt;
            candidate.physical_address = arguments[2]; // Replaced by the device tree it names.
        }
    }
    else if constexpr (A == mips || A == mips64)
    {
        // Malta loader: a0 = argc, a1 = argv, a2 = envp (KSEG0), a3 = low RAM bytes.
        // argc/argv and a3 alone are not a memory map; the board table is.
        if (protocol == BootProtocol::malta_prom)
        {
            matched = true;
            if (arguments == std::array<u64, 4>{})
                return fail(DiscoveryFailure::source_absent, Status::not_found("Malta loader supplied no PROM"));
            auto envp = malta_kseg0_physical(arguments[2]);
            if (arguments[0] > max_malta_prom_pointers || !malta_kseg0_physical(arguments[1]) || !envp ||
                (envp.value() & 3) || !arguments[3] || (arguments[3] & 4095) || arguments[3] > malta_low_ram_limit)
                return fail(DiscoveryFailure::protocol_mismatch, Status::invalid_argument("Malta PROM arguments invalid"));
            candidate.format = FirmwareFormat::board;
            candidate.physical_address = envp.value();
        }
    }
    if (!matched)
        return fail(DiscoveryFailure::unsupported_protocol, Status::unsupported("boot memory protocol unsupported for this profile"));
    if (candidate.physical_address == 0)
        return fail(DiscoveryFailure::source_absent, Status::not_found("firmware pointer absent"));
    if (candidate.format == FirmwareFormat::fdt && (candidate.physical_address & 7))
        return fail(DiscoveryFailure::protocol_mismatch, Status::invalid_argument("FDT pointer is not eight-byte aligned"));
    output = candidate;
    return {};
}

template <FirmwareReader Reader>
Result<std::span<const std::byte>> read_at(Reader &reader, const BootSource &source, u64 address, usize count)
{
    if (count == 0 || u64(count) > ~u64{0} - address)
        return Status::overflow("firmware window wraps physical address space");
    if (source.initial_mapping_end && (address >= source.initial_mapping_end ||
        u64(count) > source.initial_mapping_end - address))
        return Status::fault("firmware window exceeds loader mapping extent");
    auto bytes = reader.read_physical(address, count);
    if (!bytes)
        return bytes.status();
    if (bytes.value().size() != count || bytes.value().data() == nullptr)
        return Status::fault("firmware reader returned an invalid window");
    return bytes;
}

template <FirmwareReader Reader>
Result<std::span<const std::byte>> read(Reader &reader, const BootSource &source, usize count)
{
    return read_at(reader, source, source.physical_address, count);
}

// Field-wise reset keeps the large record arrays out of the discovery stack.
inline void clear(FirmwareMemory &memory)
{
    for (auto &record : memory.e820.records) record = {};
    memory.e820.count = 0;
    for (auto &range : memory.fdt.ram) range = {};
    for (auto &reservation : memory.fdt.reservations) reservation = {};
    memory.fdt.ram_count = memory.fdt.reservation_count = 0;
    memory.fdt.blob_size = memory.fdt.boot_cpu_id = 0;
    memory.board = {};
    memory.loader_storage = {};
    memory.loader_storage_count = 0;
    memory.source = {};
    memory.source_storage = {};
    memory.decoded_bytes = 0;
}

// Replaces the EFI system table address with the device tree it names and
// records both loader tables for reservation.
template <FirmwareReader Reader>
DiscoveryResult locate_efi_device_tree(Reader &reader, BootSource &source,
                                       std::array<PhysicalRange, 2> &storage, usize &storage_count)
{
    auto system = read(reader, source, efi_system_table_bytes);
    if (!system)
        return fail(DiscoveryFailure::unmapped_source, system.status());
    auto tables = parse_efi_system_table(system.value());
    if (!tables)
        return fail(DiscoveryFailure::malformed_source, tables.status());
    const auto table_bytes = static_cast<usize>(tables.value().count * efi_configuration_entry_bytes);
    auto entries = read_at(reader, source, tables.value().address, table_bytes);
    if (!entries)
        return fail(DiscoveryFailure::unmapped_source, entries.status(), static_cast<u32>(tables.value().count));
    auto tree = find_efi_device_tree(entries.value(), tables.value().count);
    if (!tree)
        return fail(tree.status().code() == StatusCode::not_found ? DiscoveryFailure::source_absent
                                                                  : DiscoveryFailure::malformed_source,
                    tree.status(), static_cast<u32>(tables.value().count));
    if (tree.value() & 7)
        return fail(DiscoveryFailure::protocol_mismatch, Status::invalid_argument("FDT pointer is not eight-byte aligned"));
    storage = {PhysicalRange{source.physical_address, efi_system_table_bytes},
               PhysicalRange{tables.value().address, table_bytes}};
    storage_count = 2;
    source.physical_address = tree.value();
    return {Status::success(), DiscoveryFailure::none, static_cast<u32>(tables.value().count)};
}

// Malta: decode the PROM environment, then size the fixed board table from it.
template <FirmwareReader Reader>
DiscoveryResult discover_malta(Reader &reader, const BootSource &source, FirmwareMemory &workspace,
                               FirmwareMemory &output)
{
    constexpr bool big_endian = true; // Both preserved Malta profiles are big-endian.
    u64 first = source.physical_address, last = source.physical_address;
    usize decoded{};
    u64 memsize{}, ememsize{};
    bool have_memsize{}, have_ememsize{};
    auto note = [&](u64 address, u64 size) {
        first = address < first ? address : first;
        last = address + size > last ? address + size : last;
    };
    auto string_at = [&](u32 pointer) -> Result<std::string_view> {
        auto address = malta_kseg0_physical(pointer);
        if (!address)
            return address.status();
        const u64 window_end = malta_kseg_size;
        const auto count = static_cast<usize>(window_end - address.value() < max_malta_prom_string
                                                  ? window_end - address.value() : max_malta_prom_string);
        auto bytes = read_at(reader, source, address.value(), count);
        if (!bytes)
            return bytes.status();
        decoded += count;
        const auto *text = reinterpret_cast<const char *>(bytes.value().data());
        usize length = 0;
        while (length < count && text[length] != '\0')
            ++length;
        if (length == count)
            return Status::invalid_argument("PROM string is not terminated within bound");
        note(address.value(), length + 1);
        return std::string_view{text, length};
    };
    if (auto argv = malta_kseg0_physical(source.arguments[1]); argv && source.arguments[0])
        note(argv.value(), source.arguments[0] * 4);
    u32 pairs{};
    for (usize index = 0;; index += 2)
    {
        if (index + 2 > max_malta_prom_pointers)
            return fail(DiscoveryFailure::source_capacity_exceeded, Status::overflow("PROM environment exceeds bound"), pairs);
        auto words = read_at(reader, source, source.physical_address + index * 4, 8);
        if (!words)
            return fail(DiscoveryFailure::unmapped_source, words.status(), pairs);
        decoded += 8;
        const u32 name_pointer = word(words.value(), 0, big_endian);
        const u32 value_pointer = word(words.value(), 4, big_endian);
        note(source.physical_address + index * 4, name_pointer ? 8 : 4);
        if (!name_pointer)
            break;
        auto name = string_at(name_pointer);
        auto value = name ? string_at(value_pointer) : Result<std::string_view>{name.status()};
        if (!name || !value)
            return fail(DiscoveryFailure::malformed_source, name ? value.status() : name.status(), pairs);
        ++pairs;
        if (name.value() == "memsize" || name.value() == "ememsize")
        {
            auto number = parse_malta_prom_number(value.value());
            bool &seen = name.value() == "memsize" ? have_memsize : have_ememsize;
            if (!number || seen)
                return fail(DiscoveryFailure::malformed_source,
                            number ? Status::invalid_argument("PROM repeats a memory size") : number.status(), pairs);
            seen = true;
            (name.value() == "memsize" ? memsize : ememsize) = number.value();
        }
    }
    if (!have_memsize)
        return fail(DiscoveryFailure::source_absent, Status::not_found("PROM environment has no memsize"), pairs);
    if (!have_ememsize)
        ememsize = memsize;
    // The loader's a3 is the same low-RAM count; disagreement is not resolved by choosing one.
    if (memsize != source.arguments[3] || ememsize < memsize ||
        (memsize < malta_low_ram_limit && ememsize != memsize) || (ememsize & 4095))
        return fail(DiscoveryFailure::malformed_source, Status::invalid_argument("PROM memory sizes are inconsistent"), pairs);
    clear(workspace);
    workspace.source = source;
    workspace.source_storage = {first, last - first};
    workspace.decoded_bytes = decoded;
    auto &board = workspace.board;
    board.reported_low = memsize;
    board.reported_total = ememsize;
    // Malta physical map: low RAM at zero below the 256 MiB I/O hole, RAM past
    // it aliased at 0x90000000; exception vectors in the first page.
    board.ram[board.ram_count++] = {0, memsize};
    if (ememsize > malta_low_ram_limit)
        board.ram[board.ram_count++] = {malta_high_ram_base, ememsize - malta_low_ram_limit};
    board.reserved[board.reserved_count++] = {0, 4096};
    board.devices[board.device_count++] = {malta_low_ram_limit, malta_kseg_size - malta_low_ram_limit};
    output = workspace;
    return {Status::success(), DiscoveryFailure::none, pairs};
}
} // namespace handoff_detail

// Workspace must be caller-owned and distinct from output. Failure preserves
// output; workspace is unspecified. No fallback map is manufactured. Keeping
// the full output candidate in caller storage avoids a large discovery stack.
template <arch::Architecture A, FirmwareReader Reader>
DiscoveryResult discover_firmware_memory(const RawBootHandoff &raw, BootProtocol protocol,
                                         Reader &reader, FirmwareMemory &workspace, FirmwareMemory &output)
{
    using namespace handoff_detail;
    if (&workspace == &output)
        return fail(DiscoveryFailure::invalid_workspace, Status::invalid_argument("firmware workspace aliases output"));
    BootSource source;
    if (const auto result = select<A>(raw, protocol, source); !result)
        return result;
    if (source.format == FirmwareFormat::board)
        return discover_malta(reader, source, workspace, output);
    std::array<PhysicalRange, 2> loader_storage{};
    usize loader_storage_count{};
    if (source.protocol == BootProtocol::loongarch64_efi_fdt)
        if (const auto located = locate_efi_device_tree(reader, source, loader_storage, loader_storage_count); !located)
            return located;
    const usize header_size = source.format == FirmwareFormat::e820 ? e820_header_bytes : 40;
    auto header = read(reader, source, header_size);
    if (!header)
        return fail(DiscoveryFailure::unmapped_source, header.status());
    std::array<std::byte, 40> header_copy{};
    for (usize i = 0; i < header_size; ++i)
        header_copy[i] = header.value()[i];
    usize count{};
    u32 records{};
    if (source.format == FirmwareFormat::e820)
    {
        if (word(header.value(), 0, false) != e820_handoff_magic ||
            word(header.value(), 12, false) != e820_record_bytes)
            return fail(DiscoveryFailure::malformed_source, Status::invalid_argument("E820 header invalid"));
        records = word(header.value(), 8, false);
        const auto state = word(header.value(), 4, false);
        if (records > e820_record_capacity || state == 3)
            return fail(DiscoveryFailure::source_capacity_exceeded, Status::overflow("E820 source capacity exceeded"), records);
        if (state == 2)
            return fail(DiscoveryFailure::incomplete_source, Status::io_error("BIOS E820 collection failed"), records);
        if (state != 1 || records == 0)
            return fail(DiscoveryFailure::malformed_source, Status::invalid_argument("E820 collection state invalid"), records);
        count = e820_header_bytes + records * e820_record_bytes;
    }
    else
    {
        if (word(header.value(), 0, true) != 0xd00dfeed)
            return fail(DiscoveryFailure::malformed_source, Status::invalid_argument("FDT magic invalid"));
        count = word(header.value(), 4, true);
        if (count > max_fdt_bytes)
            return fail(DiscoveryFailure::source_capacity_exceeded, Status::overflow("FDT source capacity exceeded"));
        if (count < header_size)
            return fail(DiscoveryFailure::malformed_source, Status::invalid_argument("FDT total size invalid"));
    }
    auto bytes = read(reader, source, count);
    if (!bytes)
        return fail(DiscoveryFailure::unmapped_source, bytes.status(), records);
    // A changed size/count could otherwise allow the decoder to accept fewer
    // records than the first read advertised. Require the complete header to
    // remain stable; the reader contract supplies stability for the payload.
    for (usize i = 0; i < header_size; ++i)
        if (header_copy[i] != bytes.value()[i])
            return fail(DiscoveryFailure::malformed_source, Status::invalid_argument("firmware header changed during discovery"), records);
    clear(workspace);
    workspace.source = source;
    workspace.source_storage = {source.physical_address, source.format == FirmwareFormat::e820
        ? e820_header_bytes + e820_record_capacity * e820_record_bytes : count};
    workspace.loader_storage = loader_storage;
    workspace.loader_storage_count = loader_storage_count;
    workspace.decoded_bytes = count;
    const auto status = source.format == FirmwareFormat::e820 ? parse_e820_handoff(bytes.value(), workspace.e820)
                                                             : parse_fdt_memory(bytes.value(), workspace.fdt);
    if (!status)
        return fail(DiscoveryFailure::parser_rejected, status, records);
    output = workspace;
    return {Status::success(), DiscoveryFailure::none, records};
}

} // namespace ok::boot
