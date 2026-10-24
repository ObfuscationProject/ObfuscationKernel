#pragma once

#include "ok/arch/types.hpp"
#include "ok/boot/e820.hpp"
#include "ok/boot/fdt.hpp"
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
};

enum class FirmwareFormat : u8 { absent, e820, fdt };
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

// Complete owned parser output. This is not an allocator input: platform RAM
// qualification, all loaded-object reservations and managed-memory policy are
// still required. Retain the original format-specific records and attributes.
struct FirmwareMemory
{
    BootSource source{};
    PhysicalRange source_storage{}; // Full loader-owned allocation to reserve.
    usize decoded_bytes{}; // Header and records read; may be smaller than storage.
    E820Map e820{};
    FdtMemoryMap fdt{};
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
        const auto expected = A == arm32 ? BootProtocol::arm32_direct_elf : BootProtocol::loongarch64_direct_elf;
        if (protocol == expected && arguments == std::array<u64, 4>{})
            return fail(DiscoveryFailure::source_absent, Status::not_found("direct ELF supplied no firmware source"));
    }
    // Malta PROM needs a qualified bounded 32-bit table/string decoder. Neither
    // argc/argv nor the a3 low-RAM count is a complete physical memory map.
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
Result<std::span<const std::byte>> read(Reader &reader, const BootSource &source, usize count)
{
    if (count == 0 || u64(count) > ~u64{0} - source.physical_address)
        return Status::overflow("firmware window wraps physical address space");
    if (source.initial_mapping_end && (source.physical_address >= source.initial_mapping_end ||
        u64(count) > source.initial_mapping_end - source.physical_address))
        return Status::fault("firmware window exceeds loader mapping extent");
    auto bytes = reader.read_physical(source.physical_address, count);
    if (!bytes)
        return bytes.status();
    if (bytes.value().size() != count || bytes.value().data() == nullptr)
        return Status::fault("firmware reader returned an invalid window");
    return bytes;
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
    for (auto &record : workspace.e820.records) record = {};
    workspace.e820.count = 0;
    for (auto &range : workspace.fdt.ram) range = {};
    for (auto &reservation : workspace.fdt.reservations) reservation = {};
    workspace.fdt.ram_count = workspace.fdt.reservation_count = 0;
    workspace.fdt.blob_size = workspace.fdt.boot_cpu_id = 0;
    workspace.source = source;
    workspace.source_storage = {source.physical_address, source.format == FirmwareFormat::e820
        ? e820_header_bytes + e820_record_capacity * e820_record_bytes : count};
    workspace.decoded_bytes = count;
    const auto status = source.format == FirmwareFormat::e820 ? parse_e820_handoff(bytes.value(), workspace.e820)
                                                             : parse_fdt_memory(bytes.value(), workspace.fdt);
    if (!status)
        return fail(DiscoveryFailure::parser_rejected, status, records);
    output = workspace;
    return {Status::success(), DiscoveryFailure::none, records};
}

} // namespace ok::boot
