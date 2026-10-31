#include "ok/boot/acpi.hpp"

#include "ok/boot/cpu_topology.hpp"

#include <string_view>

namespace ok::boot
{
namespace
{

constexpr usize sdt_header_bytes = 36;
constexpr usize madt_header_bytes = 44;
constexpr u64 ebda_segment_pointer = 0x40e;
constexpr usize ebda_scan_bytes = 1024;
constexpr u64 bios_area = 0xe0000;
constexpr usize bios_area_bytes = 0x20000;

u16 le16(const std::byte *p)
{
    return static_cast<u16>(std::to_integer<u16>(p[0]) | (std::to_integer<u16>(p[1]) << 8));
}
u32 le32(const std::byte *p)
{
    return u32{le16(p)} | (u32{le16(p + 2)} << 16);
}
u64 le64(const std::byte *p)
{
    return u64{le32(p)} | (u64{le32(p + 4)} << 32);
}
// Both ranges are nonempty and were read, so their last bytes do not wrap.
bool overlaps(u64 a, u64 an, u64 b, u64 bn)
{
    return a <= b + (bn - 1) && b <= a + (an - 1);
}
bool signature(std::span<const std::byte> bytes, std::string_view expected)
{
    for (usize i = 0; i < expected.size(); ++i)
        if (bytes[i] != std::byte(expected[i]))
            return false;
    return true;
}
bool checksum(std::span<const std::byte> bytes)
{
    u8 sum = 0;
    for (auto byte : bytes)
        sum = static_cast<u8>(sum + std::to_integer<u8>(byte));
    return sum == 0;
}
Status unreadable()
{
    return Status::fault("ACPI structure is outside the readable physical window");
}
Status invalid(std::string_view message)
{
    return Status::invalid_argument(message);
}

// An empty result means the range wraps or the reader cannot supply all of it.
std::span<const std::byte> read(PhysicalReader reader, u64 physical, usize size)
{
    if (!reader.window || !size || size - 1 > ~u64{0} - physical)
        return {};
    const auto window = reader.window(reader.context, physical, size);
    if (!window.data() || window.size() != size)
        return {};
    return window;
}

struct Rsdp
{
    u8 revision{};
    u32 length{20};
    u32 rsdt{};
    u64 xsdt{};
};

Status read_rsdp(PhysicalReader reader, u64 address, Rsdp &output)
{
    const auto head = read(reader, address, 20);
    if (head.empty())
        return unreadable();
    if (!signature(head, "RSD PTR ") || !checksum(head))
        return invalid("ACPI RSDP signature or checksum is invalid");
    Rsdp rsdp{std::to_integer<u8>(head[15]), 20, le32(head.data() + 16), 0};
    if (rsdp.revision >= 2)
    {
        const auto extended = read(reader, address, 36);
        if (extended.empty())
            return unreadable();
        rsdp.length = le32(extended.data() + 20);
        if (rsdp.length < 36 || rsdp.length > 4096)
            return invalid("ACPI RSDP length is invalid");
        const auto full = read(reader, address, rsdp.length);
        if (full.empty())
            return unreadable();
        if (!checksum(full))
            return invalid("ACPI RSDP extended checksum is invalid");
        rsdp.xsdt = le64(extended.data() + 24);
    }
    output = rsdp;
    return Status::success();
}

bool scan(PhysicalReader reader, u64 start, usize size, u64 &rsdp)
{
    const auto area = read(reader, start, size);
    for (usize offset = 0; offset + 20 <= area.size(); offset += 16)
    {
        Rsdp candidate;
        if (signature(area.subspan(offset), "RSD PTR ") && read_rsdp(reader, start + offset, candidate).ok())
        {
            rsdp = start + offset;
            return true;
        }
    }
    return false;
}

// Read a whole system description table after validating its header.
Status read_table(PhysicalReader reader, u64 address, std::span<const std::byte> &table)
{
    const auto header = read(reader, address, sdt_header_bytes);
    if (header.empty())
        return unreadable();
    const usize length = le32(header.data() + 4);
    if (length < sdt_header_bytes)
        return invalid("ACPI table length is shorter than its header");
    if (length > max_acpi_table_bytes)
        return Status::overflow("ACPI table capacity exceeded");
    const auto full = read(reader, address, length);
    if (full.empty())
        return unreadable();
    if (!checksum(full))
        return invalid("ACPI table checksum is invalid");
    table = full;
    return Status::success();
}

class MadtParser final
{
  public:
    Status run(std::span<const std::byte> madt)
    {
        if (madt.size() < madt_header_bytes)
            return invalid("ACPI MADT is truncated");
        map_.local_apic_address = le32(madt.data() + 36);
        bool override_seen = false;
        for (usize offset = madt_header_bytes; offset < madt.size();)
        {
            if (madt.size() - offset < 2)
                return invalid("ACPI MADT entry is truncated");
            const auto type = std::to_integer<u8>(madt[offset]);
            const usize length = std::to_integer<u8>(madt[offset + 1]);
            if (length < 2 || length > madt.size() - offset)
                return invalid("ACPI MADT entry length is invalid");
            const auto *entry = madt.data() + offset;
            if (type == 0) // Processor Local APIC
            {
                if (length < 8)
                    return invalid("ACPI MADT local APIC entry is truncated");
                const auto id = std::to_integer<u8>(entry[3]);
                if (id != 0xff)
                    if (auto status = add(id, le32(entry + 4)); !status.ok())
                        return status;
            }
            else if (type == 5) // Local APIC Address Override
            {
                if (length < 12)
                    return invalid("ACPI MADT local APIC override is truncated");
                if (override_seen)
                    return invalid("ACPI MADT overrides the local APIC address twice");
                override_seen = true;
                map_.local_apic_address = le64(entry + 4);
            }
            else if (type == 9) // Processor Local x2APIC
            {
                if (length < 16)
                    return invalid("ACPI MADT x2APIC entry is truncated");
                const auto id = le32(entry + 4);
                if (id != 0xffffffff)
                    if (auto status = add(id, le32(entry + 8), true); !status.ok())
                        return status;
            }
            offset += length;
        }
        if (!map_.local_apic_address)
            return invalid("ACPI MADT names no local APIC address");
        if (!map_.count)
            return Status::not_found("ACPI MADT describes no usable processors");
        if (map_.enabled_count() > 1)
            map_.method = CpuStartMethod::x86_sipi;
        else
            map_.up_reason = "single-cpu";
        return Status::success();
    }

    [[nodiscard]] const CpuTopologyMap &map() const
    {
        return map_;
    }

  private:
    // Flags bit 0 is Enabled; bit 1 (ACPI 6.3) is Online Capable. A processor
    // with neither flag is unusable and is not recorded.
    // Some firmware lists a CPU below the xAPIC limit both as a Local APIC and
    // as an x2APIC entry; like Linux, the x2APIC duplicate only adds its flag.
    Status add(u64 id, u32 flags, bool x2apic = false)
    {
        const bool enabled = flags & 1;
        if (!enabled && !(flags & 2))
            return Status::success();
        for (usize i = 0; i < map_.count; ++i)
            if (map_.cpus[i].hardware_id == id)
            {
                if (!x2apic || id >= 0xff || recorded_x2apic_[i])
                    return invalid("ACPI MADT describes an APIC id twice");
                map_.cpus[i].enabled = map_.cpus[i].enabled || enabled;
                recorded_x2apic_[i] = true;
                return Status::success();
            }
        if (map_.count == map_.cpus.size())
        {
            map_.truncated = true;
            return Status::success();
        }
        recorded_x2apic_[map_.count] = x2apic;
        map_.cpus[map_.count++] = {id, enabled, false};
        return Status::success();
    }

    CpuTopologyMap map_{};
    std::array<bool, smp::max_cpus> recorded_x2apic_{};
};

} // namespace

Status find_acpi_rsdp(PhysicalReader reader, u64 &rsdp)
{
    if (const auto pointer = read(reader, ebda_segment_pointer, 2); !pointer.empty())
    {
        const u64 ebda = u64{le16(pointer.data())} << 4;
        if (ebda >= 0x400 && scan(reader, ebda, ebda_scan_bytes, rsdp))
            return Status::success();
    }
    if (scan(reader, bios_area, bios_area_bytes, rsdp))
        return Status::success();
    return Status::not_found("ACPI RSDP not found");
}

Status parse_acpi_madt(PhysicalReader reader, u64 rsdp_address, CpuTopologyMap &output)
{
    Rsdp rsdp;
    if (auto status = read_rsdp(reader, rsdp_address, rsdp); !status.ok())
        return status;
    const bool extended = rsdp.revision >= 2 && rsdp.xsdt;
    const u64 root_address = extended ? rsdp.xsdt : rsdp.rsdt;
    if (!root_address)
        return invalid("ACPI RSDP names no root table");
    std::span<const std::byte> root;
    if (auto status = read_table(reader, root_address, root); !status.ok())
        return status;
    if (!signature(root, extended ? "XSDT" : "RSDT"))
        return invalid("ACPI root table signature is invalid");
    const usize entry_bytes = extended ? 8 : 4;
    if ((root.size() - sdt_header_bytes) % entry_bytes)
        return invalid("ACPI root table length is not a whole number of entries");
    std::span<const std::byte> madt;
    for (usize offset = sdt_header_bytes; offset < root.size(); offset += entry_bytes)
    {
        const u64 address = extended ? le64(root.data() + offset) : le32(root.data() + offset);
        if (!address)
            continue;
        const auto header = read(reader, address, sdt_header_bytes);
        if (header.empty())
            return unreadable();
        if (!signature(header, "APIC"))
            continue;
        if (!madt.empty())
            return invalid("ACPI describes more than one MADT");
        if (auto status = read_table(reader, address, madt); !status.ok())
            return status;
        if (overlaps(address, madt.size(), root_address, root.size()) ||
            overlaps(address, madt.size(), rsdp_address, rsdp.length))
            return invalid("ACPI MADT overlaps the root table or RSDP");
    }
    if (madt.empty())
        return Status::not_found("ACPI root table lists no MADT");
    MadtParser parser;
    if (auto status = parser.run(madt); !status.ok())
        return status;
    output = parser.map();
    return Status::success();
}

} // namespace ok::boot
