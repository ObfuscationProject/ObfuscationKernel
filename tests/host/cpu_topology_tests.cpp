// Model coverage for CPU topology discovery: synthetic FDT blobs and synthetic
// ACPI physical memory, plus optional DTBs dumped by QEMU. Parsing only; no
// secondary CPU is started and no firmware call is made.
#include "ok/boot/acpi.hpp"
#include "ok/boot/cpu_topology.hpp"
#include "ok/boot/fdt.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace ok;
using boot::CpuStartMethod;
using boot::CpuTopologyMap;
using boot::FdtCpuFlavor;
using Bytes = std::vector<std::byte>;

// The kernel runs no dynamic initializers: the map must be constant-initialized.
constinit CpuTopologyMap constant_map{};
static_assert([] {
    CpuTopologyMap map{};
    map.count = 2;
    map.cpus[0] = {5, true, false};
    map.cpus[1] = {6, true, false};
    map.method = CpuStartMethod::x86_sipi;
    return boot::mark_boot_cpu(map, 6) && map.boot_index == 1 && map.cpus[1].boot && map.enabled_count() == 2 &&
           !boot::mark_boot_cpu(map, 7) && map.method == CpuStartMethod::none;
}());

static unsigned fdt_cases = 0;
static unsigned acpi_cases = 0;
static unsigned prefixes = 0;

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
// One or more NUL-terminated strings, as in compatible or enable-method.
static Bytes strings(std::initializer_list<std::string_view> texts)
{
    Bytes result;
    for (auto text : texts)
    {
        for (auto character : text)
            result.push_back(std::byte(character));
        result.push_back(std::byte{0});
    }
    return result;
}

struct Tree
{
    Bytes structure, strings_block;
    u32 boot_cpu{};
    void begin(std::string_view name)
    {
        word(structure, 1);
        const auto value = strings({name});
        structure.insert(structure.end(), value.begin(), value.end());
        align(structure, 4);
    }
    void end() { word(structure, 2); }
    void prop(std::string_view name, const Bytes &value)
    {
        word(structure, 3);
        word(structure, static_cast<u32>(value.size()));
        word(structure, static_cast<u32>(strings_block.size()));
        const auto label = strings({name});
        strings_block.insert(strings_block.end(), label.begin(), label.end());
        structure.insert(structure.end(), value.begin(), value.end());
        align(structure, 4);
    }
    Bytes blob() const
    {
        Bytes out(40);
        for (unsigned i = 0; i < 4; ++i)
            word(out, 0);
        const auto structure_offset = out.size();
        out.insert(out.end(), structure.begin(), structure.end());
        word(out, 9);
        const auto strings_offset = out.size();
        out.insert(out.end(), strings_block.begin(), strings_block.end());
        write32(out, 0, 0xd00dfeed);
        write32(out, 4, static_cast<u32>(out.size()));
        write32(out, 8, static_cast<u32>(structure_offset));
        write32(out, 12, static_cast<u32>(strings_offset));
        write32(out, 16, 40);
        write32(out, 20, 17);
        write32(out, 24, 16);
        write32(out, 28, boot_cpu);
        write32(out, 32, static_cast<u32>(strings_block.size()));
        write32(out, 36, static_cast<u32>(structure.size() + 4));
        return out;
    }
};

struct ArmShape
{
    u32 cpus{4};
    std::string_view conduit{"hvc"}; // Empty omits the method property.
    std::vector<std::string_view> psci{"arm,psci-1.0", "arm,psci-0.2", "arm,psci"}; // Empty omits /psci.
    bool cpu_on{};
    bool enable_method{true};
    std::string_view secondary_method{"psci"};
    bool two_cells{};
    u32 aff3{};
    int disabled{-1};
    u32 boot{};
};

static void arm_cpu(Tree &tree, const ArmShape &shape, u32 index)
{
    const u32 mpidr = (index / 4) << 8 | index % 4;
    char name[32];
    std::snprintf(name, sizeof(name), "cpu@%x", mpidr);
    tree.begin(name);
    tree.prop("phandle", cells({index + 1}));
    tree.prop("compatible", strings({"arm,cortex-a57"}));
    tree.prop("device_type", strings({"cpu"}));
    if (shape.two_cells)
        tree.prop("reg", cells({index ? shape.aff3 : 0, mpidr}));
    else
        tree.prop("reg", cells({mpidr}));
    if (shape.enable_method)
        tree.prop("enable-method", strings({index ? shape.secondary_method : "psci"}));
    if (static_cast<int>(index) == shape.disabled)
        tree.prop("status", strings({"disabled"}));
    tree.end();
}

static Tree arm_tree(const ArmShape &shape)
{
    Tree tree;
    tree.boot_cpu = shape.boot;
    tree.begin("");
    tree.prop("#address-cells", cells({2}));
    tree.prop("#size-cells", cells({2}));
    tree.prop("compatible", strings({"linux,dummy-virt"}));
    if (!shape.psci.empty())
    {
        tree.begin("psci");
        Bytes compatible;
        for (auto text : shape.psci)
        {
            const auto value = strings({text});
            compatible.insert(compatible.end(), value.begin(), value.end());
        }
        tree.prop("compatible", compatible);
        if (!shape.conduit.empty())
            tree.prop("method", strings({shape.conduit}));
        if (shape.cpu_on)
            tree.prop("cpu_on", cells({0x84000003}));
        tree.end();
    }
    tree.begin("memory@40000000");
    tree.prop("device_type", strings({"memory"}));
    tree.prop("reg", cells({0, 0x40000000, 0, 0x8000000}));
    tree.end();
    tree.begin("cpus");
    tree.prop("#address-cells", cells({shape.two_cells ? 2u : 1u}));
    tree.prop("#size-cells", cells({0}));
    // QEMU emits the cpu-map before the CPU nodes; it must be ignored.
    tree.begin("cpu-map");
    tree.begin("socket0");
    tree.begin("cluster0");
    tree.begin("core0");
    tree.prop("cpu", cells({1}));
    tree.end();
    tree.end();
    tree.end();
    tree.end();
    for (u32 index = 0; index < shape.cpus; ++index)
        arm_cpu(tree, shape, index);
    tree.end();
    tree.end();
    return tree;
}

struct RiscvShape
{
    u32 harts{4};
    bool timebase{true};
    bool timebase64{};
    bool isa_only{};
    u32 per_hart_timebase{};
    u32 boot{};
};

static Tree riscv_tree(const RiscvShape &shape)
{
    Tree tree;
    tree.boot_cpu = shape.boot;
    tree.begin("");
    tree.prop("#address-cells", cells({2}));
    tree.prop("#size-cells", cells({2}));
    tree.begin("cpus");
    tree.prop("#address-cells", cells({1}));
    tree.prop("#size-cells", cells({0}));
    if (shape.timebase)
        tree.prop("timebase-frequency", shape.timebase64 ? cells({0, 10000000}) : cells({10000000}));
    for (u32 hart = 0; hart < shape.harts; ++hart)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "cpu@%x", hart);
        tree.begin(name);
        tree.prop("device_type", strings({"cpu"}));
        tree.prop("reg", cells({hart}));
        tree.prop("status", strings({"okay"}));
        if (!shape.isa_only)
            tree.prop("compatible", strings({"riscv"}));
        tree.prop("riscv,isa", strings({"rv64imafdc"}));
        if (shape.per_hart_timebase)
            tree.prop("timebase-frequency", cells({shape.per_hart_timebase + hart}));
        tree.begin("interrupt-controller");
        tree.prop("compatible", strings({"riscv,cpu-intc"}));
        tree.end();
        tree.end();
    }
    tree.end();
    tree.end();
    return tree;
}

static CpuTopologyMap parsed(const Bytes &blob, FdtCpuFlavor flavor)
{
    ++fdt_cases;
    CpuTopologyMap map{};
    const auto status = boot::parse_fdt_cpus(blob, flavor, map);
    if (!status)
        std::fprintf(stderr, "unexpected FDT failure: %.*s\n", static_cast<int>(status.message().size()),
                     status.message().data());
    assert(status);
    assert(map.count <= map.cpus.size());
    assert(map.up_reason.empty() == (map.method != CpuStartMethod::none));
    return map;
}

static CpuTopologyMap poisoned()
{
    CpuTopologyMap map{};
    map.count = 3;
    map.cpus[1] = {91, true, true};
    map.boot_index = 1;
    map.method = CpuStartMethod::riscv_msip;
    map.timebase_hz = 37;
    map.up_reason = "poison";
    return map;
}

static bool same(const CpuTopologyMap &a, const CpuTopologyMap &b)
{
    if (a.count != b.count || a.boot_index != b.boot_index || a.boot_known != b.boot_known || a.method != b.method ||
        a.psci_cpu_on != b.psci_cpu_on || a.timebase_hz != b.timebase_hz ||
        a.local_apic_address != b.local_apic_address || a.truncated != b.truncated || a.up_reason != b.up_reason)
        return false;
    for (usize i = 0; i < a.cpus.size(); ++i)
        if (a.cpus[i].hardware_id != b.cpus[i].hardware_id || a.cpus[i].enabled != b.cpus[i].enabled ||
            a.cpus[i].boot != b.cpus[i].boot)
            return false;
    return true;
}

static void fdt_rejects(const Bytes &blob, FdtCpuFlavor flavor, StatusCode expected = StatusCode::ok)
{
    auto map = poisoned();
    const auto before = map;
    const auto status = boot::parse_fdt_cpus(blob, flavor, map);
    assert(!status);
    if (expected != StatusCode::ok)
        assert(status.code() == expected);
    assert(same(map, before));
}

static void fdt_fails(const Bytes &blob, FdtCpuFlavor flavor, StatusCode expected = StatusCode::ok)
{
    ++fdt_cases;
    fdt_rejects(blob, flavor, expected);
}

static void fdt_checks()
{
    // aarch64 virt shape with HVC PSCI 1.0 and the default 64-bit CPU_ON id.
    const auto hvc_blob = arm_tree({}).blob();
    auto map = parsed(hvc_blob, FdtCpuFlavor::arm);
    assert(map.count == 4 && map.enabled_count() == 4 && !map.truncated);
    assert(map.method == CpuStartMethod::psci_hvc && map.psci_cpu_on == 0xC4000003);
    assert(map.boot_known && map.boot_index == 0 && map.cpus[0].boot && !map.cpus[1].boot);
    assert(map.cpus[3].hardware_id == 3 && map.timebase_hz == 0 && map.local_apic_address == 0);
    boot::FdtMemoryMap memory{};
    assert(boot::parse_fdt_memory(hvc_blob, memory) && memory.ram_count == 1); // Same blob, both parsers.

    // EL2 shape: SMC conduit and an explicit cpu_on id.
    map = parsed(arm_tree({.conduit = "smc", .cpu_on = true}).blob(), FdtCpuFlavor::arm);
    assert(map.method == CpuStartMethod::psci_smc && map.psci_cpu_on == 0x84000003);

    // -smp 1 shape: QEMU omits enable-method for a single CPU.
    map = parsed(arm_tree({.cpus = 1, .enable_method = false}).blob(), FdtCpuFlavor::arm);
    assert(map.count == 1 && map.method == CpuStartMethod::none && map.up_reason == "single-cpu" && map.boot_known);

    // A disabled CPU is recorded but needs no start method.
    map = parsed(arm_tree({.cpus = 3, .disabled = 2}).blob(), FdtCpuFlavor::arm);
    assert(map.count == 3 && map.enabled_count() == 2 && !map.cpus[2].enabled);
    assert(map.method == CpuStartMethod::psci_hvc);
    map = parsed(arm_tree({.cpus = 2, .disabled = 1}).blob(), FdtCpuFlavor::arm);
    assert(map.up_reason == "single-cpu");

    // Two-cell reg carries MPIDR Aff3 in the high cell.
    map = parsed(arm_tree({.cpus = 2, .two_cells = true, .aff3 = 1}).blob(), FdtCpuFlavor::arm);
    assert(map.cpus[1].hardware_id == 0x100000001ULL && map.method == CpuStartMethod::psci_hvc);

    // Precise UP reasons.
    assert(parsed(arm_tree({.psci = {}}).blob(), FdtCpuFlavor::arm).up_reason == "psci-absent");
    assert(parsed(arm_tree({.psci = {"arm,psci"}}).blob(), FdtCpuFlavor::arm).up_reason == "psci-0.1-unsupported");
    assert(parsed(arm_tree({.conduit = ""}).blob(), FdtCpuFlavor::arm).up_reason == "psci-method-missing");
    assert(parsed(arm_tree({.conduit = "svc"}).blob(), FdtCpuFlavor::arm).up_reason == "psci-method-unsupported");
    assert(parsed(arm_tree({.secondary_method = "spin-table"}).blob(), FdtCpuFlavor::arm).up_reason ==
           "enable-method-not-psci");
    assert(parsed(arm_tree({.enable_method = false}).blob(), FdtCpuFlavor::arm).up_reason == "enable-method-not-psci");
    map = parsed(arm_tree({.psci = {"arm,psci-0.2"}}).blob(), FdtCpuFlavor::arm);
    assert(map.method == CpuStartMethod::psci_hvc);

    // A boot_cpuid_phys that names no CPU leaves the boot CPU to the caller.
    map = parsed(arm_tree({.boot = 9}).blob(), FdtCpuFlavor::arm);
    assert(!map.boot_known && map.method == CpuStartMethod::psci_hvc);
    for (usize i = 0; i < map.count; ++i)
        assert(!map.cpus[i].boot);
    assert(boot::mark_boot_cpu(map, 2) && map.boot_known && map.boot_index == 2 && map.cpus[2].boot);
    assert(map.method == CpuStartMethod::psci_hvc);
    assert(boot::mark_boot_cpu(map, 1) && map.boot_index == 1 && !map.cpus[2].boot);
    assert(!boot::mark_boot_cpu(map, 77) && !map.boot_known && map.method == CpuStartMethod::none);
    assert(map.up_reason == "boot-cpu-not-described");
    map = parsed(arm_tree({.cpus = 3, .disabled = 2}).blob(), FdtCpuFlavor::arm);
    assert(!boot::mark_boot_cpu(map, 2) && map.method == CpuStartMethod::none); // A disabled CPU cannot be running.

    // More CPUs than max_cpus: bounded, reported, and the boot CPU is kept.
    const u32 many = smp::max_cpus + 4;
    map = parsed(arm_tree({.cpus = many}).blob(), FdtCpuFlavor::arm);
    assert(map.truncated && map.count == smp::max_cpus && map.boot_index == 0);
    map = parsed(arm_tree({.cpus = many, .boot = ((many - 1) / 4) << 8 | (many - 1) % 4}).blob(), FdtCpuFlavor::arm);
    assert(map.truncated && map.count == smp::max_cpus && map.boot_known && map.boot_index == smp::max_cpus - 1);
    assert(map.cpus[map.boot_index].hardware_id == (((many - 1) / 4) << 8 | (many - 1) % 4));

    // RISC-V harts with a /cpus timebase.
    map = parsed(riscv_tree({}).blob(), FdtCpuFlavor::riscv);
    assert(map.count == 4 && map.method == CpuStartMethod::riscv_msip && map.timebase_hz == 10000000);
    assert(map.boot_known && map.boot_index == 0 && map.cpus[2].hardware_id == 2);
    map = parsed(riscv_tree({.timebase64 = true, .isa_only = true}).blob(), FdtCpuFlavor::riscv);
    assert(map.timebase_hz == 10000000 && map.method == CpuStartMethod::riscv_msip);
    map = parsed(riscv_tree({.harts = 1}).blob(), FdtCpuFlavor::riscv);
    assert(map.up_reason == "single-cpu" && map.timebase_hz == 10000000);
    map = parsed(riscv_tree({.harts = 1, .timebase = false, .per_hart_timebase = 1000000}).blob(), FdtCpuFlavor::riscv);
    assert(map.timebase_hz == 1000000);
    map = parsed(riscv_tree({.timebase = false}).blob(), FdtCpuFlavor::riscv);
    assert(map.timebase_hz == 0 && map.method == CpuStartMethod::riscv_msip);
    map = parsed(riscv_tree({.boot = 3}).blob(), FdtCpuFlavor::riscv);
    assert(map.boot_index == 3);
    fdt_fails(riscv_tree({.per_hart_timebase = 1000000}).blob(), FdtCpuFlavor::riscv, StatusCode::unsupported);

    // Flavor mismatches are explicit.
    fdt_fails(hvc_blob, FdtCpuFlavor::riscv, StatusCode::unsupported);
    fdt_fails(riscv_tree({}).blob(), FdtCpuFlavor::arm, StatusCode::unsupported);

    // Structural failures.
    auto cpus_with = [](auto &&cpu_props, u32 address_cells = 1, u32 size_cells = 0) {
        Tree tree;
        tree.begin("");
        tree.begin("cpus");
        tree.prop("#address-cells", cells({address_cells}));
        tree.prop("#size-cells", cells({size_cells}));
        cpu_props(tree);
        tree.end();
        tree.end();
        return tree.blob();
    };
    auto cpu = [](Tree &tree, const Bytes &reg) {
        tree.begin("cpu");
        tree.prop("device_type", strings({"cpu"}));
        tree.prop("reg", reg);
        tree.end();
    };
    fdt_fails(cpus_with([&](Tree &t) { cpu(t, cells({0})); }, 1, 1), FdtCpuFlavor::arm, StatusCode::unsupported);
    fdt_fails(cpus_with([&](Tree &t) { cpu(t, cells({0})); }, 3), FdtCpuFlavor::arm, StatusCode::unsupported);
    fdt_fails(cpus_with([&](Tree &t) { cpu(t, cells({0, 1})); }), FdtCpuFlavor::arm, StatusCode::unsupported);
    fdt_fails(cpus_with([&](Tree &t) { cpu(t, cells({0})); }, 2), FdtCpuFlavor::arm, StatusCode::invalid_argument);
    fdt_fails(cpus_with([&](Tree &t) { cpu(t, {}); }), FdtCpuFlavor::arm, StatusCode::invalid_argument);
    fdt_fails(cpus_with([&](Tree &t) { cpu(t, cells({0})); cpu(t, cells({0})); }), FdtCpuFlavor::arm,
              StatusCode::invalid_argument);
    fdt_fails(cpus_with([&](Tree &t) { cpu(t, cells({0x01000000})); }), FdtCpuFlavor::arm, StatusCode::invalid_argument);
    fdt_fails(cpus_with([&](Tree &t) { cpu(t, cells({0x100, 0})); }, 2), FdtCpuFlavor::arm,
              StatusCode::invalid_argument);
    fdt_fails(cpus_with([&](Tree &t) {
                  t.begin("cpu");
                  t.prop("device_type", strings({"cpu"}));
                  t.prop("reg", cells({0}));
                  t.prop("reg", cells({1}));
                  t.end();
              }),
              FdtCpuFlavor::arm, StatusCode::invalid_argument);
    fdt_fails(cpus_with([&](Tree &t) {
                  t.begin("cpu");
                  t.prop("device_type", strings({"cpu"}));
                  t.prop("reg", cells({0}));
                  t.prop("enable-method", Bytes{std::byte{'p'}});
                  t.end();
              }),
              FdtCpuFlavor::arm, StatusCode::invalid_argument);
    fdt_fails(cpus_with([&](Tree &t) {
                  t.prop("timebase-frequency", cells({0}));
                  cpu(t, cells({0}));
              }),
              FdtCpuFlavor::arm, StatusCode::invalid_argument);
    // Children of /cpus without device_type "cpu" are not CPUs.
    fdt_fails(cpus_with([&](Tree &t) {
                  t.begin("cpu-map");
                  t.prop("reg", cells({0}));
                  t.end();
              }),
              FdtCpuFlavor::arm, StatusCode::not_found);
    Tree empty;
    empty.begin("");
    empty.end();
    fdt_fails(empty.blob(), FdtCpuFlavor::arm, StatusCode::not_found);
    {
        // A second enabled PSCI node is ambiguous; a disabled one is ignored.
        Tree twice;
        twice.begin("");
        for (auto name : {"psci", "psci-copy"})
        {
            twice.begin(name);
            twice.prop("compatible", strings({"arm,psci-0.2"}));
            twice.prop("method", strings({"hvc"}));
            twice.end();
        }
        twice.begin("cpus");
        twice.prop("#address-cells", cells({1}));
        twice.prop("#size-cells", cells({0}));
        twice.end();
        twice.end();
        fdt_fails(twice.blob(), FdtCpuFlavor::arm, StatusCode::invalid_argument);
        Tree disabled_copy;
        disabled_copy.begin("");
        disabled_copy.begin("psci-copy");
        disabled_copy.prop("compatible", strings({"arm,psci-0.2"}));
        disabled_copy.prop("method", strings({"smc"}));
        disabled_copy.prop("status", strings({"disabled"}));
        disabled_copy.end();
        disabled_copy.begin("psci");
        disabled_copy.prop("compatible", strings({"arm,psci-0.2"}));
        disabled_copy.prop("method", strings({"hvc"}));
        disabled_copy.end();
        disabled_copy.begin("cpus");
        disabled_copy.prop("#address-cells", cells({1}));
        disabled_copy.prop("#size-cells", cells({0}));
        for (u32 index = 0; index < 2; ++index)
            arm_cpu(disabled_copy, ArmShape{}, index);
        disabled_copy.end();
        disabled_copy.end();
        assert(parsed(disabled_copy.blob(), FdtCpuFlavor::arm).method == CpuStartMethod::psci_hvc);
    }

    // Every truncated prefix and a hostile-byte corpus: sanitizer coverage and
    // failure-atomic publication.
    for (usize size = 0; size < hvc_blob.size(); ++size, ++prefixes)
        fdt_rejects(Bytes(hvc_blob.begin(), hvc_blob.begin() + size), FdtCpuFlavor::arm);
    u64 random = 0x5eed1234;
    for (unsigned i = 0; i < 10000; ++i)
    {
        auto bytes = (i & 1) ? hvc_blob : riscv_tree({}).blob();
        const auto flavor = (i & 1) ? FdtCpuFlavor::arm : FdtCpuFlavor::riscv;
        random = random * 6364136223846793005ULL + 1;
        bytes[random % bytes.size()] ^= std::byte((random >> 32) | 1);
        auto result = poisoned();
        const auto before = result;
        if (!boot::parse_fdt_cpus(bytes, flavor, result))
        {
            assert(same(result, before));
            continue;
        }
        assert(result.count && result.count <= result.cpus.size());
        assert(result.up_reason.empty() == (result.method != CpuStartMethod::none));
        assert(!result.boot_known || (result.boot_index < result.count && result.cpus[result.boot_index].boot));
    }
}

// Synthetic physical memory: a few disjoint regions, read only through the
// PhysicalReader window so lengths are never trusted beyond what exists.
struct Region
{
    u64 base{};
    Bytes bytes;
};
struct Memory
{
    std::vector<Region> regions;
    unsigned reads{};
    Region &at(u64 base, usize size)
    {
        regions.push_back({base, Bytes(size)});
        return regions.back();
    }
    void put(u64 address, const Bytes &data)
    {
        for (auto &region : regions)
            if (address >= region.base && address - region.base + data.size() <= region.bytes.size())
            {
                std::memcpy(region.bytes.data() + (address - region.base), data.data(), data.size());
                return;
            }
        assert(false && "synthetic write outside every region");
    }
    std::span<std::byte> view(u64 address, usize size)
    {
        for (auto &region : regions)
            if (address >= region.base && address - region.base <= region.bytes.size() &&
                size <= region.bytes.size() - (address - region.base))
                return {region.bytes.data() + (address - region.base), size};
        return {};
    }
    static std::span<const std::byte> window(void *context, u64 physical, usize size)
    {
        auto &memory = *static_cast<Memory *>(context);
        ++memory.reads;
        return memory.view(physical, size);
    }
    boot::PhysicalReader reader() { return {&Memory::window, this}; }
};

static void le(Bytes &out, u64 value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i)
        out.push_back(std::byte(value >> (8 * i)));
}
static void text(Bytes &out, std::string_view value)
{
    for (auto character : value)
        out.push_back(std::byte(character));
}
static void fix_checksum(Bytes &out, usize position, usize begin = 0, usize end = 0)
{
    if (!end)
        end = out.size();
    u8 sum = 0;
    out[position] = std::byte{0};
    for (usize i = begin; i < end; ++i)
        sum = static_cast<u8>(sum + std::to_integer<u8>(out[i]));
    out[position] = std::byte(static_cast<u8>(-sum));
}
static Bytes rsdp(u8 revision, u32 rsdt, u64 xsdt = 0)
{
    Bytes out;
    text(out, "RSD PTR ");
    le(out, 0, 1);
    text(out, "OKTEST");
    le(out, revision, 1);
    le(out, rsdt, 4);
    if (revision >= 2)
    {
        le(out, 36, 4);
        le(out, xsdt, 8);
        le(out, 0, 4);
        fix_checksum(out, 8, 0, 20);
        fix_checksum(out, 32);
    }
    else
        fix_checksum(out, 8);
    return out;
}
static Bytes sdt(std::string_view signature, const Bytes &body)
{
    Bytes out;
    text(out, signature);
    le(out, 36 + body.size(), 4);
    le(out, 1, 1);
    le(out, 0, 1);
    text(out, "OKTESTOKTABLE "); // OEM id and OEM table id.
    le(out, 1, 4);
    text(out, "OKTS");
    le(out, 1, 4);
    out.insert(out.end(), body.begin(), body.end());
    fix_checksum(out, 9);
    return out;
}
static Bytes root_table(bool extended, std::initializer_list<u64> entries)
{
    Bytes body;
    for (auto entry : entries)
        le(body, entry, extended ? 8 : 4);
    return sdt(extended ? "XSDT" : "RSDT", body);
}
static void lapic(Bytes &out, u8 uid, u8 id, u32 flags)
{
    le(out, 0, 1);
    le(out, 8, 1);
    le(out, uid, 1);
    le(out, id, 1);
    le(out, flags, 4);
}
static void x2apic(Bytes &out, u32 id, u32 flags, u32 uid)
{
    le(out, 9, 1);
    le(out, 16, 1);
    le(out, 0, 2);
    le(out, id, 4);
    le(out, flags, 4);
    le(out, uid, 4);
}
static Bytes madt(const Bytes &entries, u32 local_apic = 0xfee00000)
{
    Bytes body;
    le(body, local_apic, 4);
    le(body, 1, 4); // PCAT_COMPAT
    body.insert(body.end(), entries.begin(), entries.end());
    return sdt("APIC", body);
}
static Bytes enabled_lapics(unsigned count)
{
    Bytes entries;
    for (unsigned i = 0; i < count; ++i)
        lapic(entries, static_cast<u8>(i), static_cast<u8>(i), 1);
    return entries;
}

constexpr u64 ebda = 0x9fc00;
constexpr u64 tables = 0x7fe0000;
constexpr u64 high_tables = 0x100000000ULL;

// Low memory with the EBDA pointer, a table area and a 64-bit table area.
static Memory machine()
{
    Memory memory;
    memory.at(0, 0x100000);
    memory.at(tables, 0x10000);
    memory.at(high_tables, 0x10000);
    memory.put(0x40e, {std::byte(0xc0), std::byte(0x9f)}); // Segment 0x9fc0.
    return memory;
}
// RSDP v1 in the EBDA, RSDT, a FACP stand-in and the MADT.
static Memory rsdt_machine(const Bytes &madt_table)
{
    auto memory = machine();
    memory.put(ebda + 0x40, rsdp(0, tables));
    memory.put(tables, root_table(false, {tables + 0x100, tables + 0x200}));
    memory.put(tables + 0x100, sdt("FACP", Bytes(16)));
    memory.put(tables + 0x200, madt_table);
    return memory;
}

static CpuTopologyMap acpi_parsed(Memory &memory)
{
    ++acpi_cases;
    u64 found = 0;
    auto status = boot::find_acpi_rsdp(memory.reader(), found);
    assert(status);
    CpuTopologyMap map{};
    status = boot::parse_acpi_madt(memory.reader(), found, map);
    if (!status)
        std::fprintf(stderr, "unexpected ACPI failure: %.*s\n", static_cast<int>(status.message().size()),
                     status.message().data());
    assert(status);
    assert(map.count <= map.cpus.size() && !map.boot_known);
    assert(map.up_reason.empty() == (map.method != CpuStartMethod::none));
    return map;
}

static void acpi_fails(Memory &memory, u64 rsdp_address, StatusCode expected)
{
    ++acpi_cases;
    auto map = poisoned();
    const auto before = map;
    const auto status = boot::parse_acpi_madt(memory.reader(), rsdp_address, map);
    if (status || status.code() != expected)
        std::fprintf(stderr, "ACPI expected code %u, got %u (%.*s)\n", static_cast<unsigned>(expected),
                     static_cast<unsigned>(status.code()), static_cast<int>(status.message().size()),
                     status.message().data());
    assert(!status && status.code() == expected);
    assert(same(map, before));
}

static void acpi_checks()
{
    // RSDP v1 in the EBDA, RSDT, four enabled local APICs.
    auto memory = rsdt_machine(madt(enabled_lapics(4)));
    u64 found = 0;
    assert(boot::find_acpi_rsdp(memory.reader(), found) && found == ebda + 0x40);
    auto map = acpi_parsed(memory);
    assert(map.count == 4 && map.enabled_count() == 4 && map.method == CpuStartMethod::x86_sipi);
    assert(map.local_apic_address == 0xfee00000 && !map.truncated && map.cpus[3].hardware_id == 3);
    assert(boot::mark_boot_cpu(map, 0) && map.boot_known && map.boot_index == 0 && map.cpus[0].boot);
    assert(map.method == CpuStartMethod::x86_sipi);

    // RSDP v2 in the BIOS area after a stale signature; XSDT in 64-bit space is
    // preferred over an RSDT that leads to a different MADT.
    memory = machine();
    Bytes stale = rsdp(0, tables);
    stale[8] ^= std::byte{1};
    memory.put(0xf0000, stale);
    Bytes damaged_v2 = rsdp(2, tables, high_tables);
    damaged_v2[33] ^= std::byte{1}; // Valid v1 checksum, broken extended checksum.
    memory.put(0xf0010, damaged_v2);
    memory.put(0xf0050, rsdp(2, tables, high_tables));
    memory.put(tables, root_table(false, {tables + 0x200}));
    memory.put(tables + 0x200, madt(enabled_lapics(1)));
    memory.put(high_tables, root_table(true, {0, high_tables + 0x100, high_tables + 0x200}));
    memory.put(high_tables + 0x100, sdt("FACP", Bytes(16)));
    Bytes entries;
    x2apic(entries, 0, 1, 0);
    x2apic(entries, 0x100, 1, 1);
    x2apic(entries, 0x101, 2, 2);  // Online capable, not enabled.
    x2apic(entries, 0x102, 0, 3);  // Unusable: not recorded.
    x2apic(entries, 0xffffffff, 1, 4);
    lapic(entries, 5, 0xff, 1);    // Broadcast id: not a CPU.
    lapic(entries, 6, 7, 1);
    le(entries, 5, 1); // Local APIC address override.
    le(entries, 12, 1);
    le(entries, 0, 2);
    le(entries, 0xfee00000ULL | (1ULL << 32), 8);
    le(entries, 4, 1); // Local APIC NMI: skipped by length.
    le(entries, 6, 1);
    le(entries, 0xff, 1);
    le(entries, 0, 2);
    le(entries, 1, 1);
    memory.put(high_tables + 0x200, madt(entries));
    assert(boot::find_acpi_rsdp(memory.reader(), found) && found == 0xf0050);
    map = acpi_parsed(memory);
    assert(map.count == 4 && map.enabled_count() == 3 && map.method == CpuStartMethod::x86_sipi);
    assert(map.cpus[1].hardware_id == 0x100 && !map.cpus[2].enabled && map.cpus[2].hardware_id == 0x101);
    assert(map.cpus[3].hardware_id == 7 && map.local_apic_address == (0xfee00000ULL | (1ULL << 32)));
    assert(boot::mark_boot_cpu(map, 0x100) && map.boot_index == 1);
    assert(!boot::mark_boot_cpu(map, 0x101) && map.method == CpuStartMethod::none &&
           map.up_reason == "boot-cpu-not-described");

    // Revision 2 without an XSDT address falls back to the RSDT.
    memory = machine();
    memory.put(0xe0000, rsdp(2, tables, 0));
    memory.put(tables, root_table(false, {tables + 0x200}));
    memory.put(tables + 0x200, madt(enabled_lapics(2)));
    map = acpi_parsed(memory);
    assert(map.count == 2 && map.method == CpuStartMethod::x86_sipi);

    // An EBDA segment below 0x400 is ignored; the BIOS area is still scanned.
    memory = machine();
    memory.put(0x40e, {std::byte{0x10}, std::byte{0}});
    memory.put(0x100 + 0x40, rsdp(0, tables));
    assert(boot::find_acpi_rsdp(memory.reader(), found).code() == StatusCode::not_found);
    memory.put(0xfffe0, rsdp(0, tables)); // Last 16-byte slot that holds 20 bytes is 0xfffe0.
    assert(boot::find_acpi_rsdp(memory.reader(), found) && found == 0xfffe0);
    acpi_cases += 2;

    // Disabled CPUs and the single-CPU result.
    Bytes one_enabled;
    lapic(one_enabled, 0, 0, 1);
    lapic(one_enabled, 1, 1, 0);
    lapic(one_enabled, 2, 2, 2);
    memory = rsdt_machine(madt(one_enabled));
    map = acpi_parsed(memory);
    assert(map.count == 2 && map.enabled_count() == 1 && map.up_reason == "single-cpu" && !map.cpus[1].enabled);
    assert(map.cpus[1].hardware_id == 2);

    // More CPUs than max_cpus.
    memory = rsdt_machine(madt(enabled_lapics(smp::max_cpus + 5)));
    map = acpi_parsed(memory);
    assert(map.truncated && map.count == smp::max_cpus && map.method == CpuStartMethod::x86_sipi);

    // Failures: bad checksums, bad lengths, truncation, ambiguity.
    const u64 at = ebda + 0x40;
    memory = rsdt_machine(madt(enabled_lapics(2)));
    {
        auto bad = rsdp(0, tables);
        bad[8] ^= std::byte{1};
        memory.put(at, bad);
        acpi_fails(memory, at, StatusCode::invalid_argument);
        assert(boot::find_acpi_rsdp(memory.reader(), found).code() == StatusCode::not_found);
        acpi_cases += 1;
        memory.put(at, rsdp(0, 0));
        acpi_fails(memory, at, StatusCode::invalid_argument);
        acpi_fails(memory, 0xfffff, StatusCode::fault); // RSDP straddles the end of readable memory.
        acpi_fails(memory, ~u64{0} - 4, StatusCode::fault); // Wraps.
        auto v2 = rsdp(2, tables, high_tables);
        v2[20] = std::byte{200}; // Extended length now disagrees with the checksum.
        fix_checksum(v2, 8, 0, 20);
        memory.put(at, v2);
        acpi_fails(memory, at, StatusCode::invalid_argument);
        v2 = rsdp(2, tables, high_tables);
        v2[20] = std::byte{20}; // Extended length shorter than the v2 structure.
        fix_checksum(v2, 8, 0, 20);
        memory.put(at, v2);
        acpi_fails(memory, at, StatusCode::invalid_argument);
    }
    auto with_root = [&](const Bytes &root, const Bytes &madt_table, u64 madt_at = tables + 0x200) {
        auto result = machine();
        result.put(at, rsdp(0, tables));
        result.put(tables, root);
        result.put(madt_at, madt_table);
        return result;
    };
    const auto good_root = root_table(false, {tables + 0x200});
    auto good_madt = madt(enabled_lapics(2));
    {
        auto root = good_root;
        root[9] ^= std::byte{1};
        memory = with_root(root, good_madt);
        acpi_fails(memory, at, StatusCode::invalid_argument); // RSDT checksum.
        root = good_root;
        root[0] = std::byte{'X'};
        fix_checksum(root, 9);
        memory = with_root(root, good_madt);
        acpi_fails(memory, at, StatusCode::invalid_argument); // Signature.
        root = good_root;
        root.push_back(std::byte{0});
        root[4] = std::byte(root.size());
        fix_checksum(root, 9);
        memory = with_root(root, good_madt);
        acpi_fails(memory, at, StatusCode::invalid_argument); // Partial entry.
        root = good_root;
        root[4] = std::byte{35};
        fix_checksum(root, 9);
        memory = with_root(root, good_madt);
        acpi_fails(memory, at, StatusCode::invalid_argument); // Shorter than its header.
        root = good_root;
        root[6] = std::byte{0x10}; // 1 MiB: beyond the table capacity.
        fix_checksum(root, 9);
        memory = with_root(root, good_madt);
        acpi_fails(memory, at, StatusCode::overflow);
        root = good_root;
        root[6] = std::byte{0x02}; // 128 KiB: within capacity, beyond the readable table area.
        fix_checksum(root, 9);
        memory = with_root(root, good_madt);
        acpi_fails(memory, at, StatusCode::fault);
        memory = with_root(root_table(false, {tables + 0x100}), good_madt, tables + 0x200);
        memory.put(tables + 0x100, sdt("FACP", {}));
        acpi_fails(memory, at, StatusCode::not_found); // No MADT.
        memory = with_root(root_table(false, {0x50000000}), good_madt);
        acpi_fails(memory, at, StatusCode::fault); // Entry outside readable memory.
        memory = with_root(root_table(false, {tables + 0x200, tables + 0x400}), good_madt);
        memory.put(tables + 0x400, good_madt);
        acpi_fails(memory, at, StatusCode::invalid_argument); // Two MADTs.
        // A MADT that overlaps its root table: the root's own bytes cannot
        // double as processor entries.
        auto overlapping = root_table(false, {tables + 0x28});
        overlapping.resize(0x28 + good_madt.size());
        std::memcpy(overlapping.data() + 0x28, good_madt.data(), good_madt.size());
        overlapping[4] = std::byte(overlapping.size());
        overlapping[5] = std::byte(overlapping.size() >> 8);
        fix_checksum(overlapping, 9);
        memory = with_root(overlapping, good_madt);
        acpi_fails(memory, at, StatusCode::invalid_argument);
    }
    auto madt_variant = [&](auto &&edit) {
        auto table = madt(enabled_lapics(2));
        edit(table);
        table[4] = std::byte(table.size());
        table[5] = std::byte(table.size() >> 8);
        fix_checksum(table, 9);
        return with_root(good_root, table);
    };
    memory = madt_variant([](Bytes &t) { t.resize(40); });
    acpi_fails(memory, at, StatusCode::invalid_argument); // Shorter than the MADT header.
    memory = madt_variant([](Bytes &t) { t.push_back(std::byte{0}); });
    acpi_fails(memory, at, StatusCode::invalid_argument); // One-byte trailing entry.
    memory = madt_variant([](Bytes &t) { t[44 + 1] = std::byte{0}; });
    acpi_fails(memory, at, StatusCode::invalid_argument); // Zero entry length.
    memory = madt_variant([](Bytes &t) { t[44 + 8 + 1] = std::byte{9}; });
    acpi_fails(memory, at, StatusCode::invalid_argument); // Entry runs past the table.
    memory = madt_variant([](Bytes &t) { t.resize(t.size() - 2); });
    acpi_fails(memory, at, StatusCode::invalid_argument); // Truncated final entry.
    memory = madt_variant([](Bytes &t) {
        t[44 + 1] = std::byte{6};
        t.resize(t.size() - 2);
    });
    acpi_fails(memory, at, StatusCode::invalid_argument); // Short local APIC entry.
    memory = madt_variant([](Bytes &t) {
        Bytes short_x2apic{std::byte{9}, std::byte{12}};
        short_x2apic.resize(12);
        t.insert(t.end(), short_x2apic.begin(), short_x2apic.end());
    });
    acpi_fails(memory, at, StatusCode::invalid_argument); // Short x2APIC entry.
    memory = madt_variant([](Bytes &t) {
        for (unsigned i = 0; i < 2; ++i)
        {
            le(t, 5, 1);
            le(t, 12, 1);
            le(t, 0, 2);
            le(t, 0xfee00000, 8);
        }
    });
    acpi_fails(memory, at, StatusCode::invalid_argument); // Two address overrides.
    memory = madt_variant([](Bytes &t) { lapic(t, 9, 1, 1); });
    acpi_fails(memory, at, StatusCode::invalid_argument); // Duplicate APIC id.
    memory = madt_variant([](Bytes &t) { x2apic(t, 1, 1, 9); });
    acpi_fails(memory, at, StatusCode::invalid_argument); // Duplicate across entry types.
    memory = madt_variant([](Bytes &t) { std::memset(t.data() + 36, 0, 4); });
    acpi_fails(memory, at, StatusCode::invalid_argument); // No local APIC address.
    memory = madt_variant([](Bytes &t) { t.resize(44); });
    acpi_fails(memory, at, StatusCode::not_found); // No processors.
    {
        auto table = madt(enabled_lapics(2));
        table[10] ^= std::byte{1};
        memory = with_root(good_root, table);
        acpi_fails(memory, at, StatusCode::invalid_argument); // MADT checksum.
        table = madt(enabled_lapics(2));
        table[4] = std::byte{0};
        table[5] = std::byte{1}; // 256 bytes: runs past the table area.
        fix_checksum(table, 9);
        memory = with_root(good_root, table, tables + 0x10000 - 0x80);
        memory.put(tables, root_table(false, {tables + 0x10000 - 0x80}));
        acpi_fails(memory, at, StatusCode::fault);
    }

    // No reader at all.
    CpuTopologyMap map_none{};
    assert(boot::find_acpi_rsdp({}, found).code() == StatusCode::not_found);
    assert(boot::parse_acpi_madt({}, at, map_none).code() == StatusCode::fault);
    acpi_cases += 2;

    // Hostile bytes inside a checksummed MADT exercise entry parsing.
    const auto base_madt = madt(enabled_lapics(4));
    u64 random = 0xacb1;
    for (unsigned i = 0; i < 10000; ++i)
    {
        auto table = base_madt;
        random = random * 6364136223846793005ULL + 1;
        const usize offset = 36 + random % (table.size() - 36);
        table[offset] ^= std::byte((random >> 32) | 1);
        fix_checksum(table, 9);
        memory = with_root(good_root, table);
        auto result = poisoned();
        const auto before = result;
        if (!boot::parse_acpi_madt(memory.reader(), at, result))
        {
            assert(same(result, before));
            continue;
        }
        assert(result.count && result.count <= result.cpus.size() && result.local_apic_address);
        assert(result.up_reason.empty() == (result.method != CpuStartMethod::none));
    }
}

static const char *method_name(CpuStartMethod method)
{
    switch (method)
    {
    case CpuStartMethod::none: return "none";
    case CpuStartMethod::x86_sipi: return "x86_sipi";
    case CpuStartMethod::psci_hvc: return "psci_hvc";
    case CpuStartMethod::psci_smc: return "psci_smc";
    case CpuStartMethod::riscv_msip: return "riscv_msip";
    }
    return "?";
}

// Each QEMU DTB case is: flavor path expected-count expected-method.
static int dumped(int argc, char **argv)
{
    if (argc % 4 != 1)
    {
        std::fputs("usage: cpu-topology [arm|riscv DTB COUNT METHOD]...\n", stderr);
        return 2;
    }
    for (int i = 1; i < argc; i += 4)
    {
        const std::string_view flavor_name = argv[i];
        const auto flavor = flavor_name == "riscv" ? FdtCpuFlavor::riscv : FdtCpuFlavor::arm;
        std::ifstream stream(argv[i + 1], std::ios::binary);
        assert(stream);
        const std::vector<char> characters{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        CpuTopologyMap map{};
        const auto status = boot::parse_fdt_cpus(
            {reinterpret_cast<const std::byte *>(characters.data()), characters.size()}, flavor, map);
        if (!status)
        {
            std::fprintf(stderr, "%s: %.*s\n", argv[i + 1], static_cast<int>(status.message().size()),
                         status.message().data());
            return 1;
        }
        std::printf("QEMU DTB %s: cpus=%zu enabled=%zu boot=%zu%s method=%s cpu_on=0x%x timebase=%llu up=%.*s\n",
                    argv[i + 1], map.count, map.enabled_count(), map.boot_index, map.boot_known ? "" : "(unknown)",
                    method_name(map.method), map.psci_cpu_on, static_cast<unsigned long long>(map.timebase_hz),
                    static_cast<int>(map.up_reason.size()), map.up_reason.data());
        if (map.count != std::strtoul(argv[i + 2], nullptr, 10) || std::string_view{method_name(map.method)} != argv[i + 3] ||
            !map.boot_known)
        {
            std::fprintf(stderr, "%s: expected %s CPUs with %s and a known boot CPU\n", argv[i + 1], argv[i + 2],
                         argv[i + 3]);
            return 1;
        }
        if ((map.method == CpuStartMethod::psci_hvc || map.method == CpuStartMethod::psci_smc) &&
            map.psci_cpu_on != 0xC4000003)
            return 1;
        if (flavor == FdtCpuFlavor::riscv && !map.timebase_hz)
            return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    fdt_checks();
    acpi_checks();
    if (const int status = dumped(argc, argv); status)
        return status;
    std::printf("PASS cpu topology discovery: fdt_cases=%u acpi_cases=%u fdt_prefixes=%u corpus=20000 qemu_dtbs=%d\n",
                fdt_cases, acpi_cases, prefixes, (argc - 1) / 4);
}
