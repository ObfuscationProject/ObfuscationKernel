#include "ok/boot/boot_memory.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace ok;
using namespace ok::boot;
using enum arch::Architecture;
using Bytes = std::vector<std::byte>;

#define CHECK(condition)                                                                                   \
    do                                                                                                     \
    {                                                                                                      \
        if (!static_cast<bool>(condition))                                                                 \
        {                                                                                                  \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                            \
            std::abort();                                                                                  \
        }                                                                                                  \
    } while (false)

namespace
{

void put(Bytes &bytes, usize offset, u64 value, usize width, bool big_endian)
{
    if (bytes.size() < offset + width)
        bytes.resize(offset + width);
    for (usize i = 0; i < width; ++i)
        bytes[offset + i] = std::byte(value >> ((big_endian ? width - 1 - i : i) * 8));
}

// Several disjoint physical windows; a read must lie inside exactly one.
struct Reader
{
    struct Window
    {
        u64 address;
        Bytes bytes;
    };
    std::vector<Window> windows;
    usize calls{};
    Result<std::span<const std::byte>> read_physical(u64 address, usize count)
    {
        ++calls;
        for (const auto &window : windows)
            if (address >= window.address && address - window.address + count <= window.bytes.size())
                return std::span<const std::byte>{window.bytes.data() + (address - window.address), count};
        return Status::fault("unmapped test window");
    }
};
static_assert(FirmwareReader<Reader>);

RawBootHandoff raw(u64 a, u64 b = 0, u64 c = 0, u64 d = 0)
{
    return {{static_cast<uptr>(a), static_cast<uptr>(b), static_cast<uptr>(c), static_cast<uptr>(d)}, true};
}

// Minimal FDT v17: root #address/#size-cells 2, the given memory banks.
Bytes fdt(std::initializer_list<PhysicalRange> banks)
{
    Bytes structure, names;
    auto word = [&](u32 value) { put(structure, structure.size(), value, 4, true); };
    auto label = [&](const char *text) {
        do structure.push_back(std::byte(*text)); while (*text++);
        while (structure.size() % 4) structure.push_back(std::byte{});
    };
    auto property = [&](const char *name, std::initializer_list<u32> cells) {
        word(3);
        word(static_cast<u32>(cells.size() * 4));
        word(static_cast<u32>(names.size()));
        do names.push_back(std::byte(*name)); while (*name++);
        for (auto cell : cells) word(cell);
    };
    auto text_property = [&](const char *name, const char *value) {
        usize length = 0;
        while (value[length]) ++length;
        word(3);
        word(static_cast<u32>(length + 1));
        word(static_cast<u32>(names.size()));
        do names.push_back(std::byte(*name)); while (*name++);
        for (usize i = 0; i <= length; ++i) structure.push_back(std::byte(value[i]));
        while (structure.size() % 4) structure.push_back(std::byte{});
    };
    word(1); label("");
    property("#address-cells", {2});
    property("#size-cells", {2});
    for (const auto bank : banks)
    {
        const auto name = "memory@" + std::to_string(bank.address);
        word(1); label(name.c_str());
        text_property("device_type", "memory");
        property("reg", {static_cast<u32>(bank.address >> 32), static_cast<u32>(bank.address),
                         static_cast<u32>(bank.size >> 32), static_cast<u32>(bank.size)});
        word(2);
    }
    word(2);
    word(9);
    Bytes blob(56);
    const usize structure_offset = blob.size();
    blob.insert(blob.end(), structure.begin(), structure.end());
    const usize strings_offset = blob.size();
    blob.insert(blob.end(), names.begin(), names.end());
    while (blob.size() % 8) blob.push_back(std::byte{});
    put(blob, 0, 0xd00dfeed, 4, true);
    put(blob, 4, blob.size(), 4, true);
    put(blob, 8, structure_offset, 4, true);
    put(blob, 12, strings_offset, 4, true);
    put(blob, 16, 40, 4, true);
    put(blob, 20, 17, 4, true);
    put(blob, 24, 16, 4, true);
    put(blob, 32, names.size(), 4, true);
    put(blob, 36, structure.size(), 4, true);
    return blob;
}

Bytes efi_system_table(u64 tables, u64 count)
{
    Bytes bytes(efi_system_table_bytes);
    put(bytes, 0, efi_system_table_signature, 8, false);
    put(bytes, 0x68, count, 8, false);
    put(bytes, 0x70, tables, 8, false);
    return bytes;
}

void configuration_entry(Bytes &table, usize index, bool device_tree, u64 pointer)
{
    const usize offset = index * efi_configuration_entry_bytes;
    table.resize(offset + efi_configuration_entry_bytes);
    for (usize i = 0; i < 16; ++i)
        table[offset + i] = std::byte(device_tree ? efi_device_tree_guid[i] : i);
    put(table, offset + 16, pointer, 8, false);
}

void efi_decoders()
{
    auto system = efi_system_table(0x10200, 3);
    CHECK(parse_efi_system_table(system).value().address == 0x10200);
    CHECK(!parse_efi_system_table({system.data(), system.size() - 1}));
    auto bad = system;
    bad[0] = std::byte{0};
    CHECK(!parse_efi_system_table(bad));
    for (u64 count : {u64{0}, u64{max_efi_configuration_entries + 1}})
        CHECK(!parse_efi_system_table(efi_system_table(0x10200, count)));
    CHECK(!parse_efi_system_table(efi_system_table(0x10204, 3)));
    CHECK(!parse_efi_system_table(efi_system_table(0, 3)));

    Bytes table;
    configuration_entry(table, 0, false, 0x1234);
    configuration_entry(table, 1, true, 0x100000);
    CHECK(find_efi_device_tree(table, 2).value() == 0x100000);
    CHECK(find_efi_device_tree(table, 1).status().code() == StatusCode::not_found);
    CHECK(!find_efi_device_tree({table.data(), table.size() - 1}, 2));
    configuration_entry(table, 2, true, 0x200000);
    CHECK(!find_efi_device_tree(table, 3));
    configuration_entry(table, 1, true, 0);
    CHECK(!find_efi_device_tree(table, 2));
}

void loongarch_discovery()
{
    // QEMU layout: system table at 0x200, configuration array at 0x10200, FDT at 1 MiB.
    Bytes table;
    configuration_entry(table, 0, false, 0x20200);
    configuration_entry(table, 1, false, 0x30200);
    configuration_entry(table, 2, true, 0x100000);
    const auto blob = fdt({{0x80000000, 0x70000000}, {0, 0x10000000}});
    Reader reader{{{0x200, efi_system_table(0x10200, 3)}, {0x10200, table}, {0x100000, blob}}};
    auto workspace = std::make_unique<FirmwareMemory>();
    auto output = std::make_unique<FirmwareMemory>();
    const auto found = discover_firmware_memory<loongarch64>(raw(1, 0, 0x200), BootProtocol::loongarch64_efi_fdt,
                                                             reader, *workspace, *output);
    CHECK(found && reader.calls == 4);
    CHECK(output->source.physical_address == 0x100000 && output->source_storage.size == blob.size());
    CHECK(output->loader_storage_count == 2 && output->loader_storage[0].address == 0x200 &&
          output->loader_storage[1].address == 0x10200 && output->loader_storage[1].size == 72);
    CHECK(output->fdt.ram_count == 2);

    // Failure leaves the output unchanged.
    const auto saved = *output;
    auto expect = [&](RawBootHandoff input, Reader &bad, DiscoveryFailure failure) {
        const auto result = discover_firmware_memory<loongarch64>(input, BootProtocol::loongarch64_efi_fdt, bad,
                                                                  *workspace, *output);
        CHECK(!result && result.failure == failure && std::memcmp(output.get(), &saved, sizeof(saved)) == 0);
    };
    Reader none;
    expect(raw(0), none, DiscoveryFailure::source_absent);
    expect(raw(2, 0, 0x200), none, DiscoveryFailure::protocol_mismatch);
    expect(raw(1, 0, 0x204), none, DiscoveryFailure::protocol_mismatch);
    Reader no_tree{{{0x200, efi_system_table(0x10200, 1)}, {0x10200, table}}};
    expect(raw(1, 0, 0x200), no_tree, DiscoveryFailure::source_absent);
    Reader no_table{{{0x200, efi_system_table(0x10200, 3)}}};
    expect(raw(1, 0, 0x200), no_table, DiscoveryFailure::unmapped_source);
    auto misaligned = table;
    configuration_entry(misaligned, 2, true, 0x100004);
    Reader skewed{{{0x200, efi_system_table(0x10200, 3)}, {0x10200, misaligned}}};
    expect(raw(1, 0, 0x200), skewed, DiscoveryFailure::protocol_mismatch);
}

// QEMU Malta PROM: 16 big-endian KSEG0 pointers at 0x2000, 256-byte strings from 0x2040.
Bytes malta_prom(std::initializer_list<const char *> strings)
{
    Bytes prom(16 * 4 + 16 * 256);
    usize index = 0;
    for (const char *text : strings)
    {
        const usize offset = 64 + index * 256;
        put(prom, index * 4, 0x80002000 + offset, 4, true);
        for (usize i = 0; text[i]; ++i)
            prom[offset + i] = std::byte(text[i]);
        ++index;
    }
    return prom;
}

void malta_discovery()
{
    auto workspace = std::make_unique<FirmwareMemory>();
    auto output = std::make_unique<FirmwareMemory>();
    auto prom = malta_prom({"kernel", "", "memsize", "268435456", "ememsize", "1073741824", "modetty0", "38400n8r"});
    Reader reader{{{0x2000, prom}}};
    CHECK(discover_firmware_memory<mips>(raw(2, 0x80002000, 0x80002008, 0x10000000), BootProtocol::malta_prom,
                                         reader, *workspace, *output));
    const auto &board = output->board;
    CHECK(output->source.format == FirmwareFormat::board && board.reported_low == 0x10000000 &&
          board.reported_total == 0x40000000);
    CHECK(board.ram_count == 2 && board.ram[0].address == 0 && board.ram[0].size == 0x10000000);
    CHECK(board.ram[1].address == malta_high_ram_base && board.ram[1].size == 0x30000000);
    CHECK(board.reserved_count == 1 && board.device_count == 1 && board.devices[0].address == 0x10000000);
    CHECK(output->source_storage.address == 0x2000 && output->source_storage.size > 0x740);

    // mips64 registers sign-extend the same KSEG0 pointers.
    Reader again{{{0x2000, prom}}};
    CHECK(discover_firmware_memory<mips64>(raw(2, 0xffffffff80002000ULL, 0xffffffff80002008ULL, 0x10000000),
                                           BootProtocol::malta_prom, again, *workspace, *output));

    // Smaller RAM: memsize equals ememsize and a3.
    Reader small{{{0x2000, malta_prom({"k", "", "memsize", "0x8000000", "ememsize", "134217728"})}}};
    CHECK(discover_firmware_memory<mips>(raw(2, 0x80002000, 0x80002008, 0x8000000), BootProtocol::malta_prom, small,
                                         *workspace, *output));
    CHECK(output->board.ram_count == 1 && output->board.ram[0].size == 0x8000000);

    const auto saved = *output;
    auto expect = [&](RawBootHandoff input, Bytes bytes, DiscoveryFailure failure) {
        Reader bad{{{0x2000, std::move(bytes)}}};
        const auto result = discover_firmware_memory<mips>(input, BootProtocol::malta_prom, bad, *workspace, *output);
        CHECK(!result && result.failure == failure && std::memcmp(output.get(), &saved, sizeof(saved)) == 0);
    };
    const auto standard = raw(2, 0x80002000, 0x80002008, 0x10000000);
    expect(standard, malta_prom({"k", "", "memsize", "134217728"}), DiscoveryFailure::malformed_source);
    expect(standard, malta_prom({"k", "", "modetty0", "38400n8r"}), DiscoveryFailure::source_absent);
    expect(standard, malta_prom({"k", "", "memsize", "268435456", "memsize", "268435456"}),
           DiscoveryFailure::malformed_source);
    expect(standard, malta_prom({"k", "", "memsize", "268435456", "ememsize", "4096"}),
           DiscoveryFailure::malformed_source);
    expect(standard, malta_prom({"k", "", "memsize", "26843545x"}), DiscoveryFailure::malformed_source);
    auto unterminated = malta_prom({"k", "", "memsize", "268435456"});
    for (usize i = 0; i < 256; ++i) unterminated[64 + 2 * 256 + i] = std::byte{'m'};
    expect(standard, unterminated, DiscoveryFailure::malformed_source);
    Bytes endless(0x2000);
    for (usize i = 0; i < 64; ++i) put(endless, 8 + i * 4, 0x80002000 + 0x1000, 4, true);
    put(endless, 0x1000, 0x41, 1, true);
    expect(standard, endless, DiscoveryFailure::source_capacity_exceeded);
    Reader none;
    for (auto input : {raw(2, 0x80002000, 0x80002008, 0x10000001), raw(2, 0x80002000, 0x2008, 0x10000000),
                       raw(2, 0x80002000, 0x80002008, 0x20000000), raw(33, 0x80002000, 0x80002008, 0x10000000)})
    {
        const auto result = discover_firmware_memory<mips>(input, BootProtocol::malta_prom, none, *workspace, *output);
        CHECK(!result && result.failure == DiscoveryFailure::protocol_mismatch);
    }
}

void malta_kernel_map()
{
    auto workspace = std::make_unique<FirmwareMemory>();
    auto source = std::make_unique<FirmwareMemory>();
    Reader reader{{{0x2000, malta_prom({"k", "", "memsize", "268435456", "ememsize", "1073741824"})}}};
    CHECK(discover_firmware_memory<mips>(raw(2, 0x80002000, 0x80002008, 0x10000000), BootProtocol::malta_prom,
                                         reader, *workspace, *source));
    QemuEntryMemoryPlan plan;
    const PhysicalRange image{0x10000, 0x400000}, bootstrap{0x310000, 0x100000};
    CHECK(kernel_memory_plan<mips>(*source, image, bootstrap, plan));
    CHECK(plan.policy.accessible.size == malta_kseg_size && plan.policy.selection == ManagedSelection::require_all);
    auto scratch = std::make_unique<QualifiedBootMemory>(), qualified = std::make_unique<QualifiedBootMemory>();
    CHECK(qualify_boot_memory(*source, {plan.reservations.begin(), plan.reservations.size()}, plan.policy, *scratch,
                              *qualified));
    // Page zero, the PROM page and the image are excluded; high RAM is outside KSEG0.
    u64 managed{};
    for (const auto range : qualified->managed())
    {
        CHECK(range.address >= 0x1000 && range.address + range.size <= 0x10000000);
        CHECK(!(range.address < 0x3000 && range.address + range.size > 0x2000));
        CHECK(!(range.address < image.address + image.size && range.address + range.size > image.address));
        managed += range.size;
    }
    CHECK(managed == 0x10000000 - 0x1000 - 0x1000 - image.size);
    bool high_excluded = false;
    for (const auto &excluded : qualified->excluded())
        high_excluded |= excluded.range.address == malta_high_ram_base && excluded.reason == ExclusionReason::unmapped;
    CHECK(high_excluded);
    memory::PhysicalMemoryMap map;
    CHECK(kernel_physical_map(*qualified, 4096, map));
    CHECK(map.product_grade() && map.usable_bytes() == managed && map.inputs()[0].source == memory::MapSource::board_table);
    CHECK(!kernel_physical_map(*qualified, 8192, map));
}

void e820_kernel_map()
{
    auto source = std::make_unique<FirmwareMemory>();
    source->source = {x86_64, BootProtocol::bios_e820, FirmwareFormat::e820};
    source->source_storage = {0x5000, 3088};
    source->e820.count = 3;
    source->e820.records[0] = {0, 0x9fc00, 1, 1};
    source->e820.records[1] = {0x100000, 0x7ee0000, 1, 1};
    source->e820.records[2] = {0x100000000ULL, 0x40000000, 1, 1}; // Above the managed window.
    QemuEntryMemoryPlan plan;
    const PhysicalRange image{0x100000, 0x4600000}, bootstrap{0x4500000, 0x100000};
    CHECK(kernel_memory_plan<x86_64>(*source, image, bootstrap, plan));
    CHECK(plan.policy.accessible.size == 0x40000000 && plan.policy.maximum_ranges == max_qualified_ranges);
    auto scratch = std::make_unique<QualifiedBootMemory>(), qualified = std::make_unique<QualifiedBootMemory>();
    CHECK(qualify_boot_memory(*source, {plan.reservations.begin(), plan.reservations.size()}, plan.policy, *scratch,
                              *qualified));
    CHECK(qualified->managed().size() == 1 && qualified->managed()[0].address == 0x4700000 &&
          qualified->managed()[0].size == 0x7fe0000 - 0x4700000);
    memory::PhysicalMemoryMap map;
    CHECK(kernel_physical_map(*qualified, 4096, map) && map.usable().size() == 1);
    CHECK(map.usable()[0].base == 0x4700000 && map.inputs()[0].source == memory::MapSource::e820);
    // The bootstrap section must lie inside usable RAM.
    CHECK(kernel_memory_plan<x86_64>(*source, image, {0xa0000, 0x1000}, plan));
    CHECK(!qualify_boot_memory(*source, {plan.reservations.begin(), plan.reservations.size()}, plan.policy, *scratch,
                               *qualified));
}

void loongarch_kernel_map()
{
    auto source = std::make_unique<FirmwareMemory>();
    source->source = {loongarch64, BootProtocol::loongarch64_efi_fdt, FirmwareFormat::fdt, {1, 0, 0x200, 0}};
    source->source_storage = {0x100000, 0x100000};
    source->loader_storage = {PhysicalRange{0x200, 0x78}, PhysicalRange{0x10200, 0x48}};
    source->loader_storage_count = 2;
    source->fdt.ram_count = 2;
    source->fdt.ram[0] = {0x80000000, 0x70000000};
    source->fdt.ram[1] = {0, 0x10000000};
    QemuEntryMemoryPlan plan;
    CHECK(kernel_memory_plan<loongarch64>(*source, {0x90000000, 0x4500000}, {0x94400000, 0x100000}, plan));
    auto scratch = std::make_unique<QualifiedBootMemory>(), qualified = std::make_unique<QualifiedBootMemory>();
    CHECK(qualify_boot_memory(*source, {plan.reservations.begin(), plan.reservations.size()}, plan.policy, *scratch,
                              *qualified));
    // Both banks are managed apart from the loader tables, FDT and image.
    CHECK(qualified->page_count() == (0x10000000ULL + 0x70000000ULL - 0x4500000ULL) / 4096 - 1 - 1 - 256);
    memory::PhysicalMemoryMap map;
    CHECK(kernel_physical_map(*qualified, 4096, map) && map.usable()[0].base == 0x1000);
}

} // namespace

int main()
{
    efi_decoders();
    loongarch_discovery();
    malta_discovery();
    malta_kernel_map();
    e820_kernel_map();
    loongarch_kernel_map();
    std::puts("OK_HOST boot_memory efi=pass malta=pass kernel_plan=pass kernel_map=pass");
    return 0;
}
