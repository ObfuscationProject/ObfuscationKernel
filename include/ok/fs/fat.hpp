#pragma once

#include "ok/core/fixed.hpp"
#include "ok/core/types.hpp"
#include "ok/driver/driver.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace ok::fs
{

inline constexpr usize fat_boot_sector_size = driver::block_sector_size;
inline constexpr usize fat_volume_label_capacity = 12;

enum class FatVariant : u8
{
    fat32,
    exfat,
};

struct FatMountInfo
{
    FatVariant variant{FatVariant::fat32};
    u32 bytes_per_sector{driver::block_sector_size};
    u32 sectors_per_cluster{0};
    u32 reserved_sector_count{0};
    u32 fat_count{0};
    u64 total_sectors{0};
    u32 sectors_per_fat{0};
    u32 fat_offset{0};
    u32 cluster_heap_offset{0};
    u32 cluster_count{0};
    u32 root_cluster{0};
    FixedString<fat_volume_label_capacity> volume_label{};
};

class FatVolume final
{
  public:
    Status mount(std::span<const std::byte> image);
    Status mount(driver::BlockDevice &device);
    [[nodiscard]] Result<FatMountInfo> info() const;
    Status read_sector(u64 sector, std::span<std::byte> out) const;
    [[nodiscard]] bool mounted() const
    {
        return mounted_;
    }

  private:
    Status parse_boot_sector(std::span<const std::byte> sector);
    Status parse_fat32(std::span<const std::byte> sector);
    Status parse_exfat(std::span<const std::byte> sector);

    bool mounted_{false};
    std::span<const std::byte> image_{};
    driver::BlockDevice *device_{nullptr};
    FatMountInfo info_{};
    std::array<std::byte, fat_boot_sector_size> boot_sector_{};
};

} // namespace ok::fs
