#include "ok/boot/fdt.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace ok;
using Bytes = std::vector<std::byte>;

static void word(Bytes &out, u32 value)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(std::byte(value >> shift));
}
static void write32(Bytes &out, usize position, u32 value)
{
    for (unsigned i = 0; i < 4; ++i)
        out[position + i] = std::byte(value >> (24 - i * 8));
}
static u32 read32(const Bytes &in, usize position)
{
    u32 result = 0;
    for (usize i = 0; i < 4; ++i)
        result = (result << 8) | std::to_integer<u32>(in[position + i]);
    return result;
}
static void align(Bytes &out, usize boundary)
{
    while (out.size() % boundary)
        out.push_back(std::byte{0});
}
static Bytes cells(std::initializer_list<u32> values)
{
    Bytes result;
    for (auto value : values)
        word(result, value);
    return result;
}
static Bytes string(std::string_view text)
{
    Bytes result;
    for (auto character : text)
        result.push_back(std::byte(character));
    result.push_back(std::byte{0});
    return result;
}
struct Tree
{
    Bytes structure, strings;
    std::vector<boot::PhysicalRange> reservations;
    void begin(std::string_view name)
    {
        word(structure, 1);
        const auto value = string(name);
        structure.insert(structure.end(), value.begin(), value.end());
        align(structure, 4);
    }
    void end() { word(structure, 2); }
    void prop(std::string_view name, const Bytes &value)
    {
        word(structure, 3);
        word(structure, static_cast<u32>(value.size()));
        word(structure, static_cast<u32>(strings.size()));
        const auto label = string(name);
        strings.insert(strings.end(), label.begin(), label.end());
        structure.insert(structure.end(), value.begin(), value.end());
        align(structure, 4);
    }
    void root(u32 addresses = 2, u32 sizes = 2)
    {
        begin("");
        prop("#address-cells", cells({addresses}));
        prop("#size-cells", cells({sizes}));
    }
    void memory(const Bytes &reg, std::string_view name = "memory@40000000")
    {
        begin(name);
        prop("device_type", string("memory"));
        prop("reg", reg);
        end();
    }
    Bytes blob() const
    {
        Bytes out(40);
        for (auto range : reservations)
        {
            word(out, static_cast<u32>(range.address >> 32));
            word(out, static_cast<u32>(range.address));
            word(out, static_cast<u32>(range.size >> 32));
            word(out, static_cast<u32>(range.size));
        }
        for (unsigned i = 0; i < 4; ++i)
            word(out, 0);
        const auto structure_offset = out.size();
        out.insert(out.end(), structure.begin(), structure.end());
        word(out, 9);
        const auto strings_offset = out.size();
        out.insert(out.end(), strings.begin(), strings.end());
        write32(out, 0, 0xd00dfeed);
        write32(out, 4, static_cast<u32>(out.size()));
        write32(out, 8, static_cast<u32>(structure_offset));
        write32(out, 12, static_cast<u32>(strings_offset));
        write32(out, 16, 40);
        write32(out, 20, 17);
        write32(out, 24, 16);
        write32(out, 28, 7);
        write32(out, 32, static_cast<u32>(strings.size()));
        write32(out, 36, static_cast<u32>(structure.size() + 4));
        return out;
    }
};
static Tree basic(u32 addresses = 2, u32 sizes = 2)
{
    Tree tree;
    tree.root(addresses, sizes);
    Bytes reg;
    if (addresses == 2) word(reg, 0);
    word(reg, 0x40000000);
    if (sizes == 2) word(reg, 0);
    word(reg, 0x10000000);
    tree.memory(reg);
    tree.end();
    return tree;
}
static void fails(const Bytes &blob, StatusCode expected = StatusCode::ok)
{
    boot::FdtMemoryMap map{};
    map.ram_count = 7;
    map.reservation_count = 3;
    map.ram[2] = {91, 37};
    map.blob_size = 0x12345678;
    unsigned char before[sizeof(map)];
    std::memcpy(before, &map, sizeof(map));
    auto status = boot::parse_fdt_memory(blob, map);
    assert(!status);
    if (expected != StatusCode::ok)
        assert(status.code() == expected);
    assert(std::memcmp(before, &map, sizeof(map)) == 0);
}
static void checks()
{
    Tree tree;
    tree.reservations = {{0, 4096}, {0x40000000, 4096}};
    tree.root();
    tree.memory(cells({0, 0x40000000, 0, 0x2000000, 1, 0, 0, 0x1000}));
    tree.begin("memory@disabled");
    tree.prop("status", string("disabled"));
    tree.prop("reg", cells({1})); // Disabled memory is not published.
    tree.end();
    tree.begin("pci@0");
    tree.prop("#address-cells", cells({3})); // Unrelated buses need not use RAM's cell widths.
    tree.prop("#size-cells", cells({2}));
    tree.end();
    tree.begin("reserved-memory");
    tree.prop("#address-cells", cells({2}));
    tree.prop("#size-cells", cells({2}));
    tree.prop("ranges", {});
    tree.begin("firmware@40001000");
    tree.prop("reg", cells({0, 0x40001000, 0, 0x1000}));
    tree.prop("no-map", {});
    tree.end();
    tree.begin("dynamic-disabled");
    tree.prop("status", string("disabled"));
    tree.prop("size", cells({0, 0x1000}));
    tree.end();
    tree.end();
    tree.end();
    auto data = tree.blob();
    boot::FdtMemoryMap map{};
    assert(boot::parse_fdt_memory(data, map));
    assert(map.ram_count == 2 && map.reservation_count == 3 && map.boot_cpu_id == 7 && map.blob_size == data.size());
    assert(map.ram[0].address == 0x40000000 && map.ram[0].size == 0x2000000);
    assert(map.ram[1].address == 0x100000000 && map.ram[1].size == 0x1000);
    assert(map.reservations[0].range.address == 0 && map.reservations[0].range.size == 4096);
    assert(map.reservations[0].source == boot::ReservationSource::reserve_table);
    assert(map.reservations[2].range.address == 0x40001000 &&
           map.reservations[2].source == boot::ReservationSource::reserved_memory &&
           map.reservations[2].no_map && !map.reservations[2].reusable);
    std::fill(data.begin(), data.end(), std::byte{0});
    assert(map.ram[1].address == 0x100000000); // Result owns values, not firmware pointers.

    for (const auto addresses : {1u, 2u})
        for (const auto sizes : {1u, 2u})
            assert(boot::parse_fdt_memory(basic(addresses, sizes).blob(), map));
    Tree defaults;
    defaults.begin("");
    defaults.memory(cells({1, 0, 0x1000}));
    defaults.end();
    assert(boot::parse_fdt_memory(defaults.blob(), map) && map.ram[0].address == 0x100000000);

    const auto baseline = tree.blob();
    for (usize size = 0; size < baseline.size(); ++size)
        fails(Bytes(baseline.begin(), baseline.begin() + size));
    for (const auto &[offset, value] : {std::pair{0u, 0u}, {4u, 39u}, {8u, 41u}, {16u, 41u},
                                     {20u, 16u}, {24u, 18u}, {36u, 0xffffffffu}, {32u, 0xffffffffu}})
    {
        auto damaged = baseline;
        write32(damaged, offset, value);
        fails(damaged);
    }
    auto damaged = baseline;
    write32(damaged, 12, read32(damaged, 8)); // Overlapping structure and strings.
    fails(damaged);
    damaged = baseline;
    write32(damaged, read32(damaged, 8), 77); // Unknown structure token.
    fails(damaged);
    damaged = basic().blob();
    write32(damaged, 40, 1); // Missing reserve-table terminator overlaps structure.
    write32(damaged, 52, 1);
    fails(damaged);
    damaged = basic().blob();
    write32(damaged, read32(damaged, 8) + 16, 0xffffffff); // Bad property string offset.
    fails(damaged);
    damaged = basic().blob();
    const auto strings_offset = read32(damaged, 12);
    std::fill(damaged.begin() + strings_offset, damaged.end(), std::byte{'x'});
    fails(damaged);

    Tree invalid;
    invalid.root();
    invalid.memory(cells({0xffffffff, 0xffffff00, 0, 0x100}));
    invalid.end();
    fails(invalid.blob(), StatusCode::overflow);
    invalid = {};
    invalid.reservations = {{~u64{0}, 1}};
    invalid.root(); invalid.memory(cells({0, 0, 0, 4096})); invalid.end();
    fails(invalid.blob(), StatusCode::overflow);
    invalid = {};
    invalid.root(); invalid.memory(cells({0, 0, 0, 0})); invalid.end();
    fails(invalid.blob());
    invalid = {};
    invalid.root(); invalid.memory(cells({0, 0, 1})); invalid.end();
    fails(invalid.blob());
    invalid = {};
    invalid.root(3, 2); invalid.memory(cells({0, 0, 0, 0, 4096})); invalid.end();
    fails(invalid.blob(), StatusCode::unsupported);
    invalid = {};
    invalid.root(); invalid.memory(cells({0, 0, 0, 4096}));
    invalid.prop("status", string("okay")); invalid.end(); // Property after child.
    fails(invalid.blob());
    invalid = {};
    invalid.root(); invalid.prop("#address-cells", cells({1})); invalid.end();
    fails(invalid.blob());
    invalid = {};
    invalid.root(); invalid.begin("bus"); invalid.memory(cells({0, 0, 0, 4096})); invalid.end(); invalid.end();
    fails(invalid.blob(), StatusCode::unsupported);

    for (unsigned variation = 0; variation < 5; ++variation)
    {
        invalid = {};
        invalid.root(); invalid.memory(cells({0, 0, 0, 4096}));
        invalid.begin("reserved-memory");
        invalid.prop("#address-cells", cells({variation == 0 ? 1u : 2u}));
        invalid.prop("#size-cells", cells({2}));
        if (variation != 1) invalid.prop("ranges", variation == 2 ? cells({1}) : Bytes{});
        invalid.begin("reservation");
        if (variation == 3) invalid.prop("size", cells({0, 4096}));
        else invalid.prop("reg", cells({0, 0, 0, 4096}));
        if (variation == 4) { invalid.prop("no-map", {}); invalid.prop("reusable", {}); }
        invalid.end(); invalid.end(); invalid.end();
        fails(invalid.blob());
    }
    invalid = {};
    invalid.root();
    Bytes ranges;
    for (usize i = 0; i < boot::max_fdt_ram_ranges + 1; ++i)
    {
        word(ranges, 0); word(ranges, static_cast<u32>(i * 4096)); word(ranges, 0); word(ranges, 4096);
    }
    invalid.memory(ranges); invalid.end();
    fails(invalid.blob(), StatusCode::overflow);
    invalid = basic();
    invalid.reservations.assign(boot::max_fdt_reservations + 1, {0, 4096});
    fails(invalid.blob(), StatusCode::overflow);
    invalid = {};
    invalid.root(); invalid.memory(cells({0, 0, 0, 4096}));
    for (usize i = 0; i < boot::max_fdt_depth; ++i) invalid.begin("bus");
    for (usize i = 0; i < boot::max_fdt_depth + 1; ++i) invalid.end();
    fails(invalid.blob(), StatusCode::overflow);
    invalid = {};
    invalid.root(); invalid.end();
    fails(invalid.blob(), StatusCode::not_found);

    // Deterministic hostile-byte corpus: sanitizer coverage and failure-atomic
    // publication, including offsets, lengths, string terminators and tokens.
    u64 random = 0x30519812;
    for (unsigned i = 0; i < 10000; ++i)
    {
        auto bytes = baseline;
        random = random * 6364136223846793005ULL + 1;
        bytes[random % bytes.size()] ^= std::byte((random >> 32) | 1);
        boot::FdtMemoryMap result{};
        const auto status = boot::parse_fdt_memory(bytes, result);
        if (!status) fails(bytes);
        else
        {
            assert(result.ram_count && result.ram_count <= result.ram.size());
            for (usize range = 0; range < result.ram_count; ++range)
                assert(result.ram[range].size <= ~u64{0} - result.ram[range].address);
        }
    }
}

int main(int argc, char **argv)
{
    checks();
    for (int i = 1; i < argc; ++i)
    {
        std::ifstream stream(argv[i], std::ios::binary);
        assert(stream);
        const std::vector<char> characters{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        boot::FdtMemoryMap map{};
        const auto status = boot::parse_fdt_memory({reinterpret_cast<const std::byte *>(characters.data()), characters.size()}, map);
        if (!status)
        {
            std::fprintf(stderr, "%s: %.*s\n", argv[i], static_cast<int>(status.message().size()), status.message().data());
            return 1;
        }
        std::printf("FDT %s: RAM=%zu reservations=%zu\n", argv[i], map.ram_count, map.reservation_count);
        for (usize range = 0; range < map.ram_count; ++range)
            std::printf("  RAM address=0x%llx size=0x%llx\n", static_cast<unsigned long long>(map.ram[range].address),
                        static_cast<unsigned long long>(map.ram[range].size));
    }
    std::puts("PASS bounded FDT memory parser (hosted parsing; no firmware handoff)");
}
