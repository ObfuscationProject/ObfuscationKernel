#pragma once

#include "ok/core/fixed.hpp"
#include "ok/core/types.hpp"
#include "ok/driver/driver.hpp"

#include <array>
#include <cstddef>
#include <span>

namespace ok::fs
{

inline constexpr usize ext4_superblock_offset = 1024;
inline constexpr u16 ext4_superblock_magic = 0xef53;
inline constexpr usize ext4_volume_name_capacity = 17;

struct Ext4MountInfo
{
    u32 inode_count{0};
    u32 block_count_low{0};
    u32 free_block_count_low{0};
    u32 block_size{1024};
    u16 inode_size{128};
    bool has_extents{false};
    FixedString<ext4_volume_name_capacity> volume_name{};
};

class Ext4Volume final
{
  public:
    Status mount(std::span<const std::byte> image);
    Status mount(driver::BlockDevice &device);
    [[nodiscard]] Result<Ext4MountInfo> info() const;
    Status read_block(u64 block, std::span<std::byte> out) const;
    [[nodiscard]] bool mounted() const
    {
        return mounted_;
    }

  private:
    Status parse_superblock(std::span<const std::byte> superblock);

    bool mounted_{false};
    std::span<const std::byte> image_{};
    driver::BlockDevice *device_{nullptr};
    Ext4MountInfo info_{};
    std::array<std::byte, 1024> superblock_{};
};

} // namespace ok::fs
