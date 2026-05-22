#include "ok/fs/fat.hpp"

namespace ok::fs
{
namespace
{

u16 read_le16(std::span<const std::byte> data, usize offset)
{
    return static_cast<u16>(std::to_integer<u16>(data[offset]) | (std::to_integer<u16>(data[offset + 1]) << 8));
}

u32 read_le32(std::span<const std::byte> data, usize offset)
{
    return static_cast<u32>(read_le16(data, offset)) | (static_cast<u32>(read_le16(data, offset + 2)) << 16);
}

u64 read_le64(std::span<const std::byte> data, usize offset)
{
    return static_cast<u64>(read_le32(data, offset)) | (static_cast<u64>(read_le32(data, offset + 4)) << 32);
}

bool bytes_equal(std::span<const std::byte> data, usize offset, std::string_view text)
{
    if (offset + text.size() > data.size())
    {
        return false;
    }
    for (usize i = 0; i < text.size(); ++i)
    {
        if (data[offset + i] != static_cast<std::byte>(text[i]))
        {
            return false;
        }
    }
    return true;
}

bool valid_sector_size(u32 value)
{
    return value == 512 || value == 1024 || value == 2048 || value == 4096;
}

bool valid_power_of_two(u32 value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

Status copy_sector(std::span<std::byte> out, std::span<const std::byte> in)
{
    if (out.size() != fat_boot_sector_size || in.size() < fat_boot_sector_size)
    {
        return Status::invalid_argument("bad FAT sector size");
    }
    for (usize i = 0; i < fat_boot_sector_size; ++i)
    {
        out[i] = in[i];
    }
    return Status::success();
}

} // namespace

Status FatVolume::mount(std::span<const std::byte> image)
{
    if (image.size() < fat_boot_sector_size)
    {
        return Status::invalid_argument("short FAT image");
    }
    if (auto status = parse_boot_sector(image.first(fat_boot_sector_size)); !status.ok())
    {
        return status;
    }

    image_ = image;
    device_ = nullptr;
    mounted_ = true;
    return Status::success();
}

Status FatVolume::mount(driver::BlockDevice &device)
{
    const auto geometry = device.geometry();
    if (geometry.block_size != driver::block_sector_size)
    {
        return Status::unsupported("FAT requires 512 byte sectors");
    }
    if (geometry.block_count == 0)
    {
        return Status::invalid_argument("short FAT block device");
    }
    if (auto status = device.read_blocks(0, std::span<std::byte>(boot_sector_.data(), boot_sector_.size()));
        !status.ok())
    {
        return status;
    }
    if (auto status = parse_boot_sector(std::span<const std::byte>(boot_sector_.data(), boot_sector_.size()));
        !status.ok())
    {
        return status;
    }

    image_ = {};
    device_ = &device;
    mounted_ = true;
    return Status::success();
}

Status FatVolume::parse_boot_sector(std::span<const std::byte> sector)
{
    if (sector.size() < fat_boot_sector_size)
    {
        return Status::invalid_argument("short FAT boot sector");
    }
    if (std::to_integer<u8>(sector[510]) != 0x55 || std::to_integer<u8>(sector[511]) != 0xaa)
    {
        return Status::invalid_argument("bad FAT signature");
    }
    if (bytes_equal(sector, 3, "EXFAT   "))
    {
        return parse_exfat(sector);
    }
    return parse_fat32(sector);
}

Status FatVolume::parse_fat32(std::span<const std::byte> sector)
{
    const auto bytes_per_sector = read_le16(sector, 0x0b);
    const auto sectors_per_cluster = std::to_integer<u8>(sector[0x0d]);
    const auto reserved = read_le16(sector, 0x0e);
    const auto fats = std::to_integer<u8>(sector[0x10]);
    const auto root_entries = read_le16(sector, 0x11);
    const auto total16 = read_le16(sector, 0x13);
    const auto sectors_per_fat16 = read_le16(sector, 0x16);
    const auto total32 = read_le32(sector, 0x20);
    const auto sectors_per_fat32 = read_le32(sector, 0x24);
    const auto root_cluster = read_le32(sector, 0x2c);

    if (!valid_sector_size(bytes_per_sector) || !valid_power_of_two(sectors_per_cluster) || reserved == 0 ||
        fats == 0 || root_entries != 0 || sectors_per_fat16 != 0 || sectors_per_fat32 == 0 ||
        root_cluster < 2)
    {
        return Status::invalid_argument("bad FAT32 fields");
    }
    const auto total_sectors = total16 != 0 ? static_cast<u64>(total16) : static_cast<u64>(total32);
    if (total_sectors == 0 || total_sectors <= reserved + static_cast<u64>(fats) * sectors_per_fat32)
    {
        return Status::invalid_argument("bad FAT32 sector counts");
    }

    info_ = FatMountInfo{};
    info_.variant = FatVariant::fat32;
    info_.bytes_per_sector = bytes_per_sector;
    info_.sectors_per_cluster = sectors_per_cluster;
    info_.reserved_sector_count = reserved;
    info_.fat_count = fats;
    info_.total_sectors = total_sectors;
    info_.sectors_per_fat = sectors_per_fat32;
    info_.fat_offset = reserved;
    info_.cluster_heap_offset = reserved + fats * sectors_per_fat32;
    info_.cluster_count =
        static_cast<u32>((total_sectors - info_.cluster_heap_offset) / sectors_per_cluster);
    info_.root_cluster = root_cluster;

    const auto *label = reinterpret_cast<const char *>(sector.data() + 0x47);
    usize label_size = 0;
    while (label_size < 11 && label[label_size] != '\0' && label[label_size] != ' ')
    {
        ++label_size;
    }
    return info_.volume_label.assign(std::string_view{label, label_size});
}

Status FatVolume::parse_exfat(std::span<const std::byte> sector)
{
    const auto partition_offset = read_le64(sector, 0x40);
    const auto total_sectors = read_le64(sector, 0x48);
    const auto fat_offset = read_le32(sector, 0x50);
    const auto fat_length = read_le32(sector, 0x54);
    const auto cluster_heap_offset = read_le32(sector, 0x58);
    const auto cluster_count = read_le32(sector, 0x5c);
    const auto root_cluster = read_le32(sector, 0x60);
    const auto bytes_per_sector_shift = std::to_integer<u8>(sector[0x6c]);
    const auto sectors_per_cluster_shift = std::to_integer<u8>(sector[0x6d]);
    const auto fats = std::to_integer<u8>(sector[0x6e]);

    if (bytes_per_sector_shift < 9 || bytes_per_sector_shift > 12 || sectors_per_cluster_shift > 25 ||
        fats == 0 || fat_offset == 0 || fat_length == 0 || cluster_heap_offset == 0 || cluster_count == 0 ||
        root_cluster < 2 || total_sectors == 0)
    {
        return Status::invalid_argument("bad exFAT fields");
    }

    const auto bytes_per_sector = 1u << bytes_per_sector_shift;
    const auto sectors_per_cluster = 1u << sectors_per_cluster_shift;
    if (cluster_heap_offset + static_cast<u64>(cluster_count) * sectors_per_cluster > total_sectors)
    {
        return Status::invalid_argument("bad exFAT cluster heap");
    }
    static_cast<void>(partition_offset);

    info_ = FatMountInfo{};
    info_.variant = FatVariant::exfat;
    info_.bytes_per_sector = bytes_per_sector;
    info_.sectors_per_cluster = sectors_per_cluster;
    info_.reserved_sector_count = fat_offset;
    info_.fat_count = fats;
    info_.total_sectors = total_sectors;
    info_.sectors_per_fat = fat_length;
    info_.fat_offset = fat_offset;
    info_.cluster_heap_offset = cluster_heap_offset;
    info_.cluster_count = cluster_count;
    info_.root_cluster = root_cluster;
    return Status::success();
}

Result<FatMountInfo> FatVolume::info() const
{
    if (!mounted_)
    {
        return Status::not_initialized("FAT not mounted");
    }
    return info_;
}

Status FatVolume::read_sector(u64 sector, std::span<std::byte> out) const
{
    if (!mounted_)
    {
        return Status::not_initialized("FAT not mounted");
    }
    if (out.size() != fat_boot_sector_size)
    {
        return Status::invalid_argument("bad FAT read size");
    }
    if (device_ != nullptr)
    {
        return device_->read_blocks(sector, out);
    }
    const auto offset = static_cast<usize>(sector) * fat_boot_sector_size;
    if (offset + fat_boot_sector_size > image_.size())
    {
        return Status::invalid_argument("FAT read out of range");
    }
    return copy_sector(out, image_.subspan(offset, fat_boot_sector_size));
}

} // namespace ok::fs
