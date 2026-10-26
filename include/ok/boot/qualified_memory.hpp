#pragma once

#include "ok/boot/firmware_handoff.hpp"
#include "ok/core/fixed.hpp"

namespace ok::boot
{
inline constexpr usize max_qualified_ranges = 512;
inline constexpr usize max_loaded_reservations = 64;
using PhysicalRanges = StaticVector<PhysicalRange, max_qualified_ranges>;

enum class ManagedSelection : u8 { require_all, lowest_addresses };
enum class ExclusionReason : u8 { reserved, unmapped, address_width, page_edge, capacity };
struct ExcludedMemory { PhysicalRange range; ExclusionReason reason; };
struct LoadedReservation
{
    PhysicalRange range;
    // False permits device/MMIO exclusions; it does not assert RAM ownership.
    bool require_usable_ram{true};
};
struct ManagedMemoryPolicy
{
    PhysicalRange accessible;
    u64 address_ceiling{}; // Exclusive, explicit bound; not a pointer-sized value.
    u64 page_size{4096};
    u64 maximum_pages{};
    ManagedSelection selection{ManagedSelection::require_all};
    usize maximum_ranges{max_qualified_ranges};
};
class QualifiedBootMemory;
Status qualify_boot_memory(const FirmwareMemory &, std::span<const LoadedReservation>, const ManagedMemoryPolicy &,
                           QualifiedBootMemory &, QualifiedBootMemory &);

class QualifiedBootMemory
{
  public:
    [[nodiscard]] bool valid() const { return valid_; }
    [[nodiscard]] const FirmwareMemory &source() const { return source_; }
    [[nodiscard]] std::span<const PhysicalRange> managed() const { return {managed_.begin(), managed_.size()}; }
    [[nodiscard]] std::span<const ExcludedMemory> excluded() const { return {excluded_.begin(), excluded_.size()}; }
    [[nodiscard]] std::span<const LoadedReservation> loaded() const { return {loaded_.begin(), loaded_.size()}; }
    [[nodiscard]] u64 page_count() const { return page_count_; }
    [[nodiscard]] const ManagedMemoryPolicy &policy() const { return policy_; }

  private:
    friend Status qualify_boot_memory(const FirmwareMemory &, std::span<const LoadedReservation>,
                                      const ManagedMemoryPolicy &, QualifiedBootMemory &, QualifiedBootMemory &);
    bool valid_{};
    FirmwareMemory source_{};
    StaticVector<LoadedReservation, max_loaded_reservations> loaded_;
    PhysicalRanges managed_;
    StaticVector<ExcludedMemory, max_qualified_ranges> excluded_;
    u64 page_count_{};
    ManagedMemoryPolicy policy_{};
};

namespace qualified_detail
{
inline bool valid_range(PhysicalRange range)
{
    return range.size != 0 && range.address <= ~u64{0} - range.size;
}
inline u64 end(PhysicalRange range) { return range.address + range.size; }
inline Status append(PhysicalRanges &ranges, PhysicalRange range)
{
    if (!valid_range(range)) return Status::invalid_argument("invalid physical range");
    return ranges.push_back(range);
}
inline Status canonicalize(PhysicalRanges &ranges)
{
    for (usize i = 1; i < ranges.size(); ++i)
    {
        const auto value = ranges[i];
        usize j = i;
        while (j && ranges[j - 1].address > value.address) { ranges[j] = ranges[j - 1]; --j; }
        ranges[j] = value;
    }
    PhysicalRanges merged;
    for (const auto range : ranges)
    {
        if (merged.empty() || end(merged[merged.size() - 1]) < range.address)
        {
            if (auto status = append(merged, range); !status) return status;
        }
        else
        {
            auto &previous = merged[merged.size() - 1];
            if (end(range) > end(previous)) previous.size = end(range) - previous.address;
        }
    }
    ranges = merged;
    return Status::success();
}
inline bool contains(const PhysicalRanges &ranges, PhysicalRange object)
{
    for (const auto range : ranges)
        if (object.address >= range.address && end(object) <= end(range)) return true;
    return false;
}
} // namespace qualified_detail

// The caller owns stable firmware facts and separate workspace/output objects.
// This is pure qualification, never an authority to dereference physical RAM.
inline Status qualify_boot_memory(const FirmwareMemory &source, std::span<const LoadedReservation> loaded,
                                  const ManagedMemoryPolicy &policy, QualifiedBootMemory &workspace,
                                  QualifiedBootMemory &output)
{
    using namespace qualified_detail;
    if (&workspace == &output || &source == &workspace.source_ || &source == &output.source_)
        return Status::invalid_argument("boot memory workspace aliases an input or output");
    if ((policy.selection != ManagedSelection::require_all && policy.selection != ManagedSelection::lowest_addresses) ||
        !policy.maximum_ranges || policy.maximum_ranges > max_qualified_ranges ||
        !valid_range(policy.accessible) || !policy.address_ceiling || !policy.page_size ||
        (policy.page_size & (policy.page_size - 1)) || !policy.maximum_pages ||
        policy.maximum_pages > ~u64{0} / policy.page_size || loaded.size() > max_loaded_reservations ||
        !valid_range(source.source_storage))
        return Status::invalid_argument("invalid explicit boot memory policy or source storage");
    const auto loaded_address = reinterpret_cast<uptr>(loaded.data());
    const auto workspace_address = reinterpret_cast<uptr>(&workspace);
    const usize loaded_bytes = loaded.size_bytes(); // Count is bounded above.
    if (!loaded.empty() && (!loaded.data() || loaded_address > ~uptr{0} - loaded_bytes ||
        (loaded_address < workspace_address + sizeof(workspace) && workspace_address < loaded_address + loaded_bytes)))
        return Status::invalid_argument("loaded reservations alias mutable workspace or wrap");
    workspace.valid_ = false;
    workspace.managed_.clear(); workspace.excluded_.clear(); workspace.loaded_.clear(); workspace.page_count_ = 0;
    workspace.source_ = source;
    workspace.policy_ = policy;
    PhysicalRanges usable, reserved;
    if (source.source.format == FirmwareFormat::e820)
    {
        if (!source.e820.count || source.e820.count > source.e820.records.size())
            return Status::invalid_argument("invalid owned E820 count");
        for (usize i = 0; i < source.e820.count; ++i)
        {
            const auto &record = source.e820.records[i];
            if (!record.size) continue;
            if (auto status = append(record.type == 1 && (record.attributes & 1) ? usable : reserved, {record.address, record.size});
                !status) return status;
        }
    }
    else if (source.source.format == FirmwareFormat::fdt)
    {
        if (!source.fdt.ram_count || source.fdt.ram_count > source.fdt.ram.size() ||
            source.fdt.reservation_count > source.fdt.reservations.size())
            return Status::invalid_argument("invalid owned FDT counts");
        for (usize i = 0; i < source.fdt.ram_count; ++i)
            if (auto status = append(usable, source.fdt.ram[i]); !status) return status;
        for (usize i = 0; i < source.fdt.reservation_count; ++i)
            if (auto status = append(reserved, source.fdt.reservations[i].range); !status) return status;
    }
    else if (source.source.format == FirmwareFormat::board)
    {
        const auto &board = source.board;
        if (!board.ram_count || board.ram_count > board.ram.size() || board.reserved_count > board.reserved.size() ||
            board.device_count > board.devices.size())
            return Status::invalid_argument("invalid owned board table counts");
        for (usize i = 0; i < board.ram_count; ++i)
            if (auto status = append(usable, board.ram[i]); !status) return status;
        for (usize i = 0; i < board.reserved_count; ++i)
            if (auto status = append(reserved, board.reserved[i]); !status) return status;
        for (usize i = 0; i < board.device_count; ++i)
            if (auto status = append(reserved, board.devices[i]); !status) return status;
    }
    else return Status::unsupported("boot memory source has no supported format");
    if (auto status = canonicalize(usable); !status) return status;
    // Source storage is live firmware data, not just the decoded record bytes.
    if (auto status = append(reserved, source.source_storage); !status) return status;
    if (source.loader_storage_count > source.loader_storage.size())
        return Status::invalid_argument("invalid loader table count");
    for (usize i = 0; i < source.loader_storage_count; ++i)
        if (auto status = append(reserved, source.loader_storage[i]); !status) return status;
    for (const auto &object : loaded)
    {
        if (!valid_range(object.range) || (object.require_usable_ram && !contains(usable, object.range)))
            return Status::invalid_argument("loaded RAM object is outside firmware usable RAM");
        if (object.require_usable_ram)
            for (const auto claim : reserved)
                if (object.range.address < end(claim) && claim.address < end(object.range))
                    return Status::invalid_argument("loaded RAM overlaps a firmware reservation");
        if (auto status = workspace.loaded_.push_back(object); !status) return status;
    }
    // Loaded objects may overlap (for example the stack is inside the complete
    // linked image). Only firmware/source reservations conflict with RAM claims.
    for (const auto &object : loaded)
        if (auto status = append(reserved, object.range); !status) return status;
    if (auto status = canonicalize(reserved); !status) return status;
    auto exclude = [&](PhysicalRange range, ExclusionReason reason) -> Status {
        return range.size ? workspace.excluded_.push_back({range, reason}) : Status::success();
    };
    // Subtract complete byte ranges first. Rounding usable extents afterwards
    // excludes every page touched by a reservation, including partial edges.
    for (const auto reservation : reserved)
    {
        PhysicalRanges next;
        for (const auto range : usable)
        {
            const u64 first = range.address > reservation.address ? range.address : reservation.address;
            const u64 last = end(range) < end(reservation) ? end(range) : end(reservation);
            if (first >= last) { if (auto status = append(next, range); !status) return status; continue; }
            if (range.address < first)
                if (auto status = append(next, {range.address, first - range.address}); !status) return status;
            if (auto status = exclude({first, last - first}, ExclusionReason::reserved); !status) return status;
            if (last < end(range))
                if (auto status = append(next, {last, end(range) - last}); !status) return status;
        }
        usable = next;
    }
    auto restrict_to = [&](PhysicalRange window, ExclusionReason reason) -> Status {
        PhysicalRanges next;
        for (const auto range : usable)
        {
            const u64 first = range.address > window.address ? range.address : window.address;
            const u64 last = end(range) < end(window) ? end(range) : end(window);
            if (first >= last) { if (auto status = exclude(range, reason); !status) return status; continue; }
            if (auto status = exclude({range.address, first - range.address}, reason); !status) return status;
            if (auto status = exclude({last, end(range) - last}, reason); !status) return status;
            if (auto status = append(next, {first, last - first}); !status) return status;
        }
        usable = next;
        return Status::success();
    };
    if (auto status = restrict_to(policy.accessible, ExclusionReason::unmapped); !status) return status;
    if (auto status = restrict_to({0, policy.address_ceiling}, ExclusionReason::address_width); !status) return status;
    for (const auto range : usable)
    {
        const u64 mask = policy.page_size - 1;
        const u64 floor_end = end(range) & ~mask;
        if (range.address >= floor_end)
        {
            if (auto status = exclude(range, ExclusionReason::page_edge); !status) return status;
            continue;
        }
        // address < floor_end proves rounding cannot wrap, even at UINT64_MAX.
        const u64 aligned = (range.address + mask) & ~mask;
        if (aligned >= floor_end)
        {
            if (auto status = exclude(range, ExclusionReason::page_edge); !status) return status;
            continue;
        }
        if (auto status = exclude({range.address, aligned - range.address}, ExclusionReason::page_edge); !status) return status;
        if (auto status = exclude({floor_end, end(range) - floor_end}, ExclusionReason::page_edge); !status) return status;
        u64 pages = (floor_end - aligned) / policy.page_size;
        const u64 available = policy.maximum_pages - workspace.page_count_;
        if (pages > available)
        {
            if (policy.selection != ManagedSelection::lowest_addresses)
                return Status::no_memory("boot memory exceeds explicit managed capacity");
            const u64 retained_end = aligned + available * policy.page_size;
            if (auto status = exclude({retained_end, floor_end - retained_end}, ExclusionReason::capacity); !status) return status;
            pages = available;
        }
        if (pages && workspace.managed_.size() == policy.maximum_ranges)
        {
            if (policy.selection != ManagedSelection::lowest_addresses)
                return Status::no_memory("boot memory exceeds explicit range capacity");
            if (auto status = exclude({aligned, pages * policy.page_size}, ExclusionReason::capacity); !status) return status;
            pages = 0;
        }
        if (pages)
        {
            if (auto status = append(workspace.managed_, {aligned, pages * policy.page_size}); !status) return status;
            workspace.page_count_ += pages;
        }
    }
    if (!workspace.page_count_) return Status::no_memory("no qualified managed RAM remains");
    workspace.valid_ = true;
    output = workspace;
    return Status::success();
}
} // namespace ok::boot
