#include "ok/boot/fdt.hpp"

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
    bool children{};
    bool dynamic{};
    bool no_map{};
    bool reusable{};
    bool memory_named{};
};

class Parser final
{
  public:
    explicit Parser(std::span<const std::byte> input) : blob_(input) {}

    Status run(FdtMemoryMap &output)
    {
        if (auto status = header(); !status.ok())
            return status;
        if (auto status = reserves(); !status.ok())
            return status;
        usize position = structure_;
        const usize end = structure_ + structure_size_;
        usize depth = 0;
        bool root_seen = false, root_closed = false;
        while (position < end)
        {
            if (end - position < 4)
                return malformed();
            const auto token = be32(blob_.data() + position);
            position += 4;
            if (token == 4) // FDT_NOP
                continue;
            if (token == 1) // FDT_BEGIN_NODE
            {
                if (root_closed || depth == stack_.size())
                    return root_closed ? malformed() : Status::overflow("FDT nesting capacity exceeded");
                const usize start = position;
                while (position < end && blob_[position] != std::byte{0})
                    ++position;
                if (position == end || position - start > 255)
                    return malformed();
                const std::string_view name{reinterpret_cast<const char *>(blob_.data() + start), position - start};
                position = (position + 4) & ~usize{3};
                if (position > end || name.find('/') != std::string_view::npos)
                    return malformed();
                Node node;
                node.memory_named = memory_name(name);
                if (depth == 0)
                {
                    if (root_seen || !name.empty())
                        return malformed();
                    root_seen = true;
                    node.kind = Kind::root;
                }
                else
                {
                    if (name.empty())
                        return malformed();
                    auto &parent = stack_[depth - 1];
                    if (auto status = validate_parent(parent); !status.ok())
                        return status;
                    parent.children = true;
                    node.enabled = parent.enabled;
                    if (depth == 1 && name == "reserved-memory")
                        node.kind = Kind::reserved_root;
                    else if (depth == 1 && memory_name(name))
                        node.kind = Kind::memory;
                    else if (parent.kind == Kind::reserved_root)
                        node.kind = Kind::reservation;
                    else if (parent.kind == Kind::reservation && parent.enabled)
                        return Status::unsupported("nested reserved-memory buses require translation");
                }
                stack_[depth++] = node;
            }
            else if (token == 2) // FDT_END_NODE
            {
                if (depth == 0)
                    return malformed();
                if (auto status = finish(stack_[depth - 1]); !status.ok())
                    return status;
                --depth;
                if (!depth)
                    root_closed = true;
            }
            else if (token == 3) // FDT_PROP
            {
                if (!depth || stack_[depth - 1].children || end - position < 8)
                    return malformed();
                const auto size = be32(blob_.data() + position);
                const auto name_offset = be32(blob_.data() + position + 4);
                position += 8;
                if (size > end - position || name_offset >= strings_size_)
                    return malformed();
                usize name_end = name_offset;
                while (name_end < strings_size_ && blob_[strings_ + name_end] != std::byte{0})
                    ++name_end;
                if (name_end == strings_size_ || name_end == name_offset)
                    return malformed();
                const std::string_view name{reinterpret_cast<const char *>(blob_.data() + strings_ + name_offset),
                                            name_end - name_offset};
                if (auto status = property(stack_[depth - 1], depth, name, position, size); !status.ok())
                    return status;
                position = (position + size + 3) & ~usize{3};
                if (position > end)
                    return malformed();
            }
            else if (token == 9) // FDT_END must terminate the advertised structure block.
            {
                if (!root_seen || !root_closed || depth || position != end)
                    return malformed();
                if (!map_.ram_count)
                    return Status::not_found("FDT contains no enabled RAM ranges");
                output = map_;
                return Status::success();
            }
            else
                return malformed();
        }
        return malformed();
    }

  private:
    Status header()
    {
        if (blob_.size() < 40 || be32(blob_.data()) != 0xd00dfeed)
            return malformed();
        const auto *p = blob_.data();
        const auto total = be32(p + 4), version = be32(p + 20), compatible = be32(p + 24);
        if (total < 40 || total > blob_.size())
            return malformed();
        if (total > max_fdt_bytes)
            return Status::overflow("FDT blob capacity exceeded");
        if (version != 17 || compatible > 17)
            return Status::unsupported("FDT version is not supported");
        blob_ = blob_.first(total);
        structure_ = be32(p + 8);
        strings_ = be32(p + 12);
        reserve_ = be32(p + 16);
        strings_size_ = be32(p + 32);
        structure_size_ = be32(p + 36);
        auto bounded = [total](usize start, usize size) { return start >= 40 && start <= total && size <= total - start; };
        if ((structure_ & 3) || (reserve_ & 7) || (structure_size_ & 3) || structure_size_ < 12 ||
            !bounded(structure_, structure_size_) || !bounded(strings_, strings_size_) || !bounded(reserve_, 16) ||
            overlaps(structure_, structure_size_, strings_, strings_size_))
            return malformed();
        map_.blob_size = total;
        map_.boot_cpu_id = be32(p + 28);
        return Status::success();
    }

    Status add_reservation(PhysicalRange range, ReservationSource source, bool no_map = false, bool reusable = false)
    {
        if (auto status = validate_range(range); !status.ok())
            return status;
        if (map_.reservation_count == map_.reservations.size())
            return Status::overflow("FDT reservation capacity exceeded");
        map_.reservations[map_.reservation_count++] = {range, source, no_map, reusable};
        return Status::success();
    }

    Status reserves()
    {
        for (usize position = reserve_; position <= blob_.size() - 16; position += 16)
        {
            if (overlaps(position, 16, structure_, structure_size_) || overlaps(position, 16, strings_, strings_size_))
                return malformed();
            PhysicalRange range{be64(blob_.data() + position), be64(blob_.data() + position + 8)};
            if (!range.address && !range.size)
                return Status::success();
            if (auto status = add_reservation(range, ReservationSource::reserve_table); !status.ok())
                return status;
        }
        return malformed();
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

    Status property(Node &node, usize depth, std::string_view name, usize offset, usize size)
    {
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
                if (depth != 2 || node.kind == Kind::reserved_root)
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

    Status finish(const Node &node)
    {
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

    std::span<const std::byte> blob_;
    FdtMemoryMap map_{};
    std::array<Node, max_fdt_depth> stack_{};
    usize structure_{}, structure_size_{}, strings_{}, strings_size_{}, reserve_{};
};

} // namespace

Status parse_fdt_memory(std::span<const std::byte> blob, FdtMemoryMap &output)
{
    return Parser(blob).run(output);
}

} // namespace ok::boot
