#include "ok/boot/fdt.hpp"

#include "ok/boot/cpu_topology.hpp"

#include <string_view>

namespace ok::boot
{
namespace
{

u32 be32(const std::byte *p)
{
    return (std::to_integer<u32>(p[0]) << 24) | (std::to_integer<u32>(p[1]) << 16) |
           (std::to_integer<u32>(p[2]) << 8) | std::to_integer<u32>(p[3]);
}
u64 be64(const std::byte *p)
{
    return (u64{be32(p)} << 32) | be32(p + 4);
}
bool overlaps(usize a, usize an, usize b, usize bn)
{
    return an && bn && a < b + bn && b < a + an;
}
Status malformed()
{
    return Status::invalid_argument("malformed or truncated FDT");
}
Status validate_range(PhysicalRange range)
{
    if (!range.size)
        return Status::invalid_argument("empty firmware memory range");
    if (range.size > ~u64{0} - range.address)
        return Status::overflow("firmware memory range wraps physical address space");
    return Status::success();
}
bool memory_name(std::string_view name)
{
    return name == "memory" || name.starts_with("memory@");
}
Result<std::string_view> string_value(std::span<const std::byte> value)
{
    if (value.empty() || value.back() != std::byte{0})
        return malformed();
    std::string_view result{reinterpret_cast<const char *>(value.data()), value.size() - 1};
    if (result.find('\0') != std::string_view::npos)
        return malformed();
    return result;
}
// A NUL-separated string list such as compatible or enable-method.
Result<bool> list_contains(std::span<const std::byte> value, std::string_view wanted)
{
    if (value.empty() || value.back() != std::byte{0})
        return malformed();
    std::string_view rest{reinterpret_cast<const char *>(value.data()), value.size()};
    bool found = false;
    while (!rest.empty())
    {
        const auto length = rest.find('\0');
        found = found || rest.substr(0, length) == wanted;
        rest.remove_prefix(length + 1);
    }
    return found;
}
Status set_once(bool &seen)
{
    if (seen)
        return malformed();
    seen = true;
    return Status::success();
}

// Header fields of a validated FDT v17 blob; offsets are relative to blob.
struct Layout
{
    std::span<const std::byte> blob;
    usize structure{}, structure_size{}, strings{}, strings_size{}, reserve{};
    u32 total{};
    u32 boot_cpu{};
};

Status read_layout(std::span<const std::byte> blob, Layout &layout)
{
    if (blob.size() < 40 || be32(blob.data()) != 0xd00dfeed)
        return malformed();
    const auto *p = blob.data();
    const auto total = be32(p + 4), version = be32(p + 20), compatible = be32(p + 24);
    if (total < 40 || total > blob.size())
        return malformed();
    if (total > max_fdt_bytes)
        return Status::overflow("FDT blob capacity exceeded");
    if (version != 17 || compatible > 17)
        return Status::unsupported("FDT version is not supported");
    Layout result{blob.first(total), be32(p + 8), be32(p + 36), be32(p + 12), be32(p + 32), be32(p + 16), total,
                  be32(p + 28)};
    auto bounded = [total](usize start, usize size) { return start >= 40 && start <= total && size <= total - start; };
    if ((result.structure & 3) || (result.reserve & 7) || (result.structure_size & 3) || result.structure_size < 12 ||
        !bounded(result.structure, result.structure_size) || !bounded(result.strings, result.strings_size) ||
        !bounded(result.reserve, 16) ||
        overlaps(result.structure, result.structure_size, result.strings, result.strings_size))
        return malformed();
    layout = result;
    return Status::success();
}

// Visit each memory reservation table entry up to its terminator.
template <typename Each> Status walk_reservations(const Layout &layout, Each &&each)
{
    for (usize position = layout.reserve; position <= layout.blob.size() - 16; position += 16)
    {
        if (overlaps(position, 16, layout.structure, layout.structure_size) ||
            overlaps(position, 16, layout.strings, layout.strings_size))
            return malformed();
        PhysicalRange range{be64(layout.blob.data() + position), be64(layout.blob.data() + position + 8)};
        if (!range.address && !range.size)
            return Status::success();
        if (auto status = each(range); !status.ok())
            return status;
    }
    return malformed();
}

// Walk the structure block, enforcing token, name, nesting and property
// placement rules. The visitor sees nodes by level (root is 0) and properties
// as blob offsets; it returns the first failure it wants to publish.
template <typename Visitor> Status walk(const Layout &layout, Visitor &visitor)
{
    const auto blob = layout.blob;
    usize position = layout.structure;
    const usize end = layout.structure + layout.structure_size;
    std::array<bool, max_fdt_depth> children{};
    usize depth = 0;
    bool root_seen = false, root_closed = false;
    while (position < end)
    {
        if (end - position < 4)
            return malformed();
        const auto token = be32(blob.data() + position);
        position += 4;
        if (token == 4) // FDT_NOP
            continue;
        if (token == 1) // FDT_BEGIN_NODE
        {
            if (root_closed || depth == children.size())
                return root_closed ? malformed() : Status::overflow("FDT nesting capacity exceeded");
            const usize start = position;
            while (position < end && blob[position] != std::byte{0})
                ++position;
            if (position == end || position - start > 255)
                return malformed();
            const std::string_view name{reinterpret_cast<const char *>(blob.data() + start), position - start};
            position = (position + 4) & ~usize{3};
            if (position > end || name.find('/') != std::string_view::npos)
                return malformed();
            if (depth == 0)
            {
                if (root_seen || !name.empty())
                    return malformed();
                root_seen = true;
            }
            else if (name.empty())
                return malformed();
            if (auto status = visitor.begin(depth, name); !status.ok())
                return status;
            if (depth)
                children[depth - 1] = true;
            children[depth++] = false;
        }
        else if (token == 2) // FDT_END_NODE
        {
            if (depth == 0)
                return malformed();
            if (auto status = visitor.end(depth - 1); !status.ok())
                return status;
            --depth;
            if (!depth)
                root_closed = true;
        }
        else if (token == 3) // FDT_PROP
        {
            if (!depth || children[depth - 1] || end - position < 8)
                return malformed();
            const auto size = be32(blob.data() + position);
            const auto name_offset = be32(blob.data() + position + 4);
            position += 8;
            if (size > end - position || name_offset >= layout.strings_size)
                return malformed();
            usize name_end = name_offset;
            while (name_end < layout.strings_size && blob[layout.strings + name_end] != std::byte{0})
                ++name_end;
            if (name_end == layout.strings_size || name_end == name_offset)
                return malformed();
            const std::string_view name{reinterpret_cast<const char *>(blob.data() + layout.strings + name_offset),
                                        name_end - name_offset};
            if (auto status = visitor.property(depth - 1, name, position, size); !status.ok())
                return status;
            position = (position + size + 3) & ~usize{3};
            if (position > end)
                return malformed();
        }
        else if (token == 9) // FDT_END must terminate the advertised structure block.
        {
            if (!root_seen || !root_closed || depth || position != end)
                return malformed();
            return visitor.complete();
        }
        else
            return malformed();
    }
    return malformed();
}

enum class Kind : u8 { root, memory, reserved_root, reservation, other };
struct Node
{
    Kind kind{Kind::other};
    u32 address_cells{2};
    u32 size_cells{1};
    usize reg_offset{};
    usize reg_size{};
    bool address_seen{};
    bool size_seen{};
    bool reg_seen{};
    bool ranges_seen{};
    bool ranges_nonempty{};
    bool status_seen{};
    bool type_seen{};
    bool enabled{true};
    bool dynamic{};
    bool no_map{};
    bool reusable{};
    bool memory_named{};
};

class MemoryVisitor final
{
  public:
    explicit MemoryVisitor(const Layout &layout) : blob_(layout.blob)
    {
        map_.blob_size = layout.total;
        map_.boot_cpu_id = layout.boot_cpu;
    }

    [[nodiscard]] const FdtMemoryMap &map() const
    {
        return map_;
    }

    Status reserve(PhysicalRange range)
    {
        return add_reservation(range, ReservationSource::reserve_table);
    }

    Status begin(usize level, std::string_view name)
    {
        Node node;
        node.memory_named = memory_name(name);
        if (level == 0)
            node.kind = Kind::root;
        else
        {
            const auto &parent = stack_[level - 1];
            if (auto status = validate_parent(parent); !status.ok())
                return status;
            node.enabled = parent.enabled;
            if (level == 1 && name == "reserved-memory")
                node.kind = Kind::reserved_root;
            else if (level == 1 && memory_name(name))
                node.kind = Kind::memory;
            else if (parent.kind == Kind::reserved_root)
                node.kind = Kind::reservation;
            else if (parent.kind == Kind::reservation && parent.enabled)
                return Status::unsupported("nested reserved-memory buses require translation");
        }
        stack_[level] = node;
        return Status::success();
    }

    Status property(usize level, std::string_view name, usize offset, usize size)
    {
        auto &node = stack_[level];
        const auto value = blob_.subspan(offset, size);
        if (name == "#address-cells" || name == "#size-cells")
        {
            auto &seen = name == "#address-cells" ? node.address_seen : node.size_seen;
            if (seen || size != 4)
                return malformed();
            seen = true;
            (name == "#address-cells" ? node.address_cells : node.size_cells) = be32(value.data());
        }
        else if (name == "reg")
        {
            if (node.reg_seen)
                return malformed();
            node.reg_seen = true;
            node.reg_offset = offset;
            node.reg_size = size;
        }
        else if (name == "ranges")
        {
            if (node.ranges_seen)
                return malformed();
            node.ranges_seen = true;
            node.ranges_nonempty = size != 0;
        }
        else if (name == "status")
        {
            if (node.status_seen)
                return malformed();
            node.status_seen = true;
            auto text = string_value(value);
            if (!text)
                return text.status();
            node.enabled = node.enabled && (text.value() == "ok" || text.value() == "okay");
        }
        else if (name == "device_type")
        {
            if (node.type_seen)
                return malformed();
            node.type_seen = true;
            auto text = string_value(value);
            if (!text)
                return text.status();
            if (text.value() == "memory")
            {
                if (level != 1 || node.kind == Kind::reserved_root)
                    return Status::unsupported("RAM on a nested bus requires address translation");
                node.kind = Kind::memory;
            }
            else if (node.kind == Kind::memory)
                return malformed();
        }
        else if (node.kind == Kind::reservation && (name == "size" || name == "alignment" || name == "alloc-ranges"))
            node.dynamic = true;
        else if (node.kind == Kind::reservation && (name == "no-map" || name == "reusable"))
        {
            auto &flag = name == "no-map" ? node.no_map : node.reusable;
            if (size || flag)
                return malformed();
            flag = true;
            if (node.no_map && node.reusable)
                return malformed();
        }
        return Status::success();
    }

    Status end(usize level)
    {
        const auto &node = stack_[level];
        if (auto status = validate_parent(node); !status.ok())
            return status;
        if (node.enabled && node.kind == Kind::other && node.memory_named)
            return Status::unsupported("RAM on a nested bus requires address translation");
        if (!node.enabled || (node.kind != Kind::memory && node.kind != Kind::reservation))
            return Status::success();
        if (node.ranges_nonempty || (node.kind == Kind::reservation && !node.reg_seen && node.dynamic))
            return Status::unsupported("memory range requires translation or dynamic allocation");
        if (!node.reg_seen || !node.reg_size)
            return malformed();
        const auto addresses = stack_[0].address_cells, sizes = stack_[0].size_cells;
        const usize stride = (addresses + sizes) * 4;
        if (node.reg_size % stride)
            return malformed();
        for (usize position = 0; position < node.reg_size; position += stride)
        {
            const auto *p = blob_.data() + node.reg_offset + position;
            PhysicalRange range{addresses == 2 ? be64(p) : be32(p),
                                sizes == 2 ? be64(p + addresses * 4) : be32(p + addresses * 4)};
            if (auto status = validate_range(range); !status.ok())
                return status;
            if (node.kind == Kind::reservation)
            {
                if (auto status = add_reservation(range, ReservationSource::reserved_memory, node.no_map, node.reusable);
                    !status.ok())
                    return status;
            }
            else
            {
                if (map_.ram_count == map_.ram.size())
                    return Status::overflow("FDT RAM range capacity exceeded");
                map_.ram[map_.ram_count++] = range;
            }
        }
        return Status::success();
    }

    Status complete() const
    {
        if (!map_.ram_count)
            return Status::not_found("FDT contains no enabled RAM ranges");
        return Status::success();
    }

  private:
    Status add_reservation(PhysicalRange range, ReservationSource source, bool no_map = false, bool reusable = false)
    {
        if (auto status = validate_range(range); !status.ok())
            return status;
        if (map_.reservation_count == map_.reservations.size())
            return Status::overflow("FDT reservation capacity exceeded");
        map_.reservations[map_.reservation_count++] = {range, source, no_map, reusable};
        return Status::success();
    }

    Status validate_parent(const Node &node) const
    {
        if (!node.enabled)
            return Status::success();
        if (node.kind == Kind::root || node.kind == Kind::reserved_root)
        {
            if ((node.address_cells != 1 && node.address_cells != 2) ||
                (node.size_cells != 1 && node.size_cells != 2))
                return Status::unsupported("FDT RAM requires one or two address and size cells");
            if (node.kind == Kind::reserved_root &&
                (!node.address_seen || !node.size_seen || !node.ranges_seen || node.ranges_nonempty ||
                 node.address_cells != stack_[0].address_cells || node.size_cells != stack_[0].size_cells))
                return Status::unsupported("reserved-memory requires explicit root cells and empty ranges");
        }
        return Status::success();
    }

    std::span<const std::byte> blob_;
    FdtMemoryMap map_{};
    std::array<Node, max_fdt_depth> stack_{};
};

enum class CpuKind : u8 { other, cpus, cpu_child, root_child };
enum class Conduit : u8 { missing, hvc, smc, other };
struct CpuNode
{
    CpuKind kind{CpuKind::other};
    Conduit conduit{Conduit::missing};
    u32 address_cells{2};
    u32 size_cells{1};
    u32 cpu_on{};
    u64 timebase{};
    usize reg_offset{};
    usize reg_size{};
    bool address_seen{};
    bool size_seen{};
    bool reg_seen{};
    bool status_seen{};
    bool enabled{true};
    bool type_seen{};
    bool cpu{};
    bool enable_method_seen{};
    bool psci_enable{};
    bool compatible_seen{};
    bool riscv_compatible{};
    bool riscv_isa{};
    bool psci_02{};
    bool psci_01{};
    bool method_seen{};
    bool cpu_on_seen{};
    bool timebase_seen{};
};

constexpr u32 psci_0_2_cpu_on_64 = 0xC4000003;

class CpuVisitor final
{
  public:
    CpuVisitor(const Layout &layout, FdtCpuFlavor flavor) : blob_(layout.blob), boot_cpu_(layout.boot_cpu), flavor_(flavor)
    {
    }

    Status begin(usize level, std::string_view name)
    {
        CpuNode node;
        if (level == 1 && name == "cpus")
        {
            if (cpus_seen_)
                return malformed();
            cpus_seen_ = true;
            node.kind = CpuKind::cpus;
        }
        else if (level == 1)
            node.kind = CpuKind::root_child;
        else if (level == 2 && stack_[1].kind == CpuKind::cpus)
            node.kind = CpuKind::cpu_child;
        stack_[level] = node;
        return Status::success();
    }

    Status property(usize level, std::string_view name, usize offset, usize size)
    {
        auto &node = stack_[level];
        const auto value = blob_.subspan(offset, size);
        if (node.kind == CpuKind::cpus && (name == "#address-cells" || name == "#size-cells"))
        {
            const bool address = name == "#address-cells";
            if (auto status = set_once(address ? node.address_seen : node.size_seen); !status.ok())
                return status;
            if (size != 4)
                return malformed();
            (address ? node.address_cells : node.size_cells) = be32(value.data());
        }
        else if ((node.kind == CpuKind::cpus || node.kind == CpuKind::cpu_child) && name == "timebase-frequency")
        {
            if (auto status = set_once(node.timebase_seen); !status.ok())
                return status;
            if (size != 4 && size != 8)
                return malformed();
            node.timebase = size == 4 ? be32(value.data()) : be64(value.data());
            if (!node.timebase)
                return Status::invalid_argument("FDT timebase-frequency is zero");
        }
        else if ((node.kind == CpuKind::cpu_child || node.kind == CpuKind::root_child) && name == "status")
        {
            if (auto status = set_once(node.status_seen); !status.ok())
                return status;
            auto text = string_value(value);
            if (!text)
                return text.status();
            node.enabled = text.value() == "ok" || text.value() == "okay";
        }
        else if ((node.kind == CpuKind::cpu_child || node.kind == CpuKind::root_child) && name == "compatible")
        {
            if (auto status = set_once(node.compatible_seen); !status.ok())
                return status;
            auto riscv = list_contains(value, "riscv");
            if (!riscv)
                return riscv.status();
            // The list is now known to be well formed. Plain "arm,psci" is PSCI 0.1,
            // whose function ids are firmware specific.
            node.riscv_compatible = riscv.value();
            node.psci_02 = list_contains(value, "arm,psci-1.0").value() || list_contains(value, "arm,psci-0.2").value();
            node.psci_01 = list_contains(value, "arm,psci").value();
        }
        else if (node.kind == CpuKind::cpu_child && name == "device_type")
        {
            if (auto status = set_once(node.type_seen); !status.ok())
                return status;
            auto text = string_value(value);
            if (!text)
                return text.status();
            node.cpu = text.value() == "cpu";
        }
        else if (node.kind == CpuKind::cpu_child && name == "reg")
        {
            if (auto status = set_once(node.reg_seen); !status.ok())
                return status;
            node.reg_offset = offset;
            node.reg_size = size;
        }
        else if (node.kind == CpuKind::cpu_child && name == "enable-method")
        {
            if (auto status = set_once(node.enable_method_seen); !status.ok())
                return status;
            auto psci = list_contains(value, "psci");
            if (!psci)
                return psci.status();
            node.psci_enable = psci.value();
        }
        else if (node.kind == CpuKind::cpu_child && name == "riscv,isa")
        {
            if (auto status = set_once(node.riscv_isa); !status.ok())
                return status;
        }
        else if (node.kind == CpuKind::root_child && name == "method")
        {
            if (auto status = set_once(node.method_seen); !status.ok())
                return status;
            auto text = string_value(value);
            if (!text)
                return text.status();
            node.conduit = text.value() == "hvc" ? Conduit::hvc : text.value() == "smc" ? Conduit::smc : Conduit::other;
        }
        else if (node.kind == CpuKind::root_child && name == "cpu_on")
        {
            if (auto status = set_once(node.cpu_on_seen); !status.ok())
                return status;
            if (size != 4)
                return malformed();
            node.cpu_on = be32(value.data());
        }
        return Status::success();
    }

    Status end(usize level)
    {
        const auto &node = stack_[level];
        if (node.kind == CpuKind::cpus)
            return note_timebase(node);
        if (node.kind == CpuKind::root_child && (node.psci_02 || node.psci_01) && node.enabled)
        {
            if (psci_seen_)
                return Status::invalid_argument("FDT describes more than one PSCI node");
            psci_seen_ = true;
            psci_ = node;
            return Status::success();
        }
        if (node.kind != CpuKind::cpu_child || !node.cpu)
            return Status::success();
        return add_cpu(node, stack_[1]);
    }

    Status complete()
    {
        if (!map_.count)
            return Status::not_found("FDT describes no CPU nodes");
        for (usize i = 0; i < map_.count; ++i)
            if (map_.cpus[i].enabled && map_.cpus[i].hardware_id == boot_cpu_)
            {
                map_.cpus[i].boot = true;
                map_.boot_index = i;
                map_.boot_known = true;
            }
        map_.timebase_hz = timebase_;
        map_.up_reason = select_method();
        if (!map_.up_reason.empty())
            map_.method = CpuStartMethod::none;
        return Status::success();
    }

    [[nodiscard]] const CpuTopologyMap &map() const
    {
        return map_;
    }

  private:
    Status note_timebase(const CpuNode &node)
    {
        if (!node.timebase_seen)
            return Status::success();
        if (timebase_ && timebase_ != node.timebase)
            return Status::unsupported("FDT CPUs disagree on timebase-frequency");
        timebase_ = node.timebase;
        return Status::success();
    }

    Status add_cpu(const CpuNode &node, const CpuNode &cpus)
    {
        if ((cpus.address_cells != 1 && cpus.address_cells != 2) || cpus.size_cells != 0)
            return Status::unsupported("FDT /cpus requires one or two address cells and zero size cells");
        const usize stride = cpus.address_cells * 4;
        if (!node.reg_seen || !node.reg_size || node.reg_size % stride)
            return malformed();
        if (node.reg_size != stride)
            return Status::unsupported("FDT CPU nodes describing several threads are not supported");
        const auto *p = blob_.data() + node.reg_offset;
        const bool riscv = node.riscv_compatible || node.riscv_isa;
        if (riscv != (flavor_ == FdtCpuFlavor::riscv))
            return Status::unsupported("FDT CPU node does not match the requested CPU flavor");
        const u64 id = cpus.address_cells == 2 ? be64(p) : be32(p);
        // Arm reg holds MPIDR Aff3 in bits 39:32 and Aff2..Aff0 in bits 23:0.
        if (flavor_ == FdtCpuFlavor::arm && (id & ~u64{0xff00ffffff}))
            return Status::invalid_argument("FDT CPU reg is not an MPIDR affinity value");
        if (auto status = note_timebase(node); !status.ok())
            return status;
        for (usize i = 0; i < map_.count; ++i)
            if (map_.cpus[i].hardware_id == id)
                return Status::invalid_argument("FDT describes a CPU id twice");
        const bool enabled = node.enabled;
        usize slot = map_.count;
        if (slot == map_.cpus.size())
        {
            map_.truncated = true;
            // Keep the boot CPU even when it is described after max_cpus others.
            if (!enabled || id != boot_cpu_ || boot_recorded_)
                return Status::success();
            slot = map_.cpus.size() - 1;
        }
        else
            ++map_.count;
        boot_recorded_ = boot_recorded_ || (enabled && id == boot_cpu_);
        map_.cpus[slot] = {id, enabled, false, flavor_ == FdtCpuFlavor::riscv || node.psci_enable};
        psci_enable_[slot] = node.psci_enable;
        return Status::success();
    }

    std::string_view select_method()
    {
        if (map_.enabled_count() < 2)
            return "single-cpu";
        if (flavor_ == FdtCpuFlavor::riscv)
        {
            map_.method = CpuStartMethod::riscv_msip;
            return {};
        }
        if (!psci_seen_)
            return "psci-absent";
        if (!psci_.psci_02)
            return "psci-0.1-unsupported";
        if (psci_.conduit == Conduit::missing)
            return "psci-method-missing";
        if (psci_.conduit == Conduit::other)
            return "psci-method-unsupported";
        for (usize i = 0; i < map_.count; ++i)
            if (map_.cpus[i].enabled && !map_.cpus[i].boot && !psci_enable_[i])
                return "enable-method-not-psci";
        map_.method = psci_.conduit == Conduit::hvc ? CpuStartMethod::psci_hvc : CpuStartMethod::psci_smc;
        map_.psci_cpu_on = psci_.cpu_on_seen ? psci_.cpu_on : psci_0_2_cpu_on_64;
        return {};
    }

    std::span<const std::byte> blob_;
    u32 boot_cpu_{};
    FdtCpuFlavor flavor_{};
    CpuTopologyMap map_{};
    std::array<bool, smp::max_cpus> psci_enable_{};
    std::array<CpuNode, max_fdt_depth> stack_{};
    CpuNode psci_{};
    u64 timebase_{};
    bool cpus_seen_{};
    bool psci_seen_{};
    bool boot_recorded_{};
};

} // namespace

Status parse_fdt_memory(std::span<const std::byte> blob, FdtMemoryMap &output)
{
    Layout layout;
    if (auto status = read_layout(blob, layout); !status.ok())
        return status;
    MemoryVisitor visitor(layout);
    if (auto status = walk_reservations(layout, [&visitor](PhysicalRange range) { return visitor.reserve(range); });
        !status.ok())
        return status;
    if (auto status = walk(layout, visitor); !status.ok())
        return status;
    output = visitor.map();
    return Status::success();
}

Status parse_fdt_cpus(std::span<const std::byte> blob, FdtCpuFlavor flavor, CpuTopologyMap &output)
{
    Layout layout;
    if (auto status = read_layout(blob, layout); !status.ok())
        return status;
    if (auto status = walk_reservations(layout, [](PhysicalRange) { return Status::success(); }); !status.ok())
        return status;
    CpuVisitor visitor(layout, flavor);
    if (auto status = walk(layout, visitor); !status.ok())
        return status;
    output = visitor.map();
    return Status::success();
}

} // namespace ok::boot
