#pragma once

#include "ok/core/fixed.hpp"
#include "ok/core/types.hpp"
#include "ok/driver/driver.hpp"
#include "ok/smp/smp.hpp"

#include <array>
#include <span>

namespace ok::fs
{

inline constexpr usize block_cache_entries = 8;
inline constexpr usize max_partitions = 8;

struct BlockCacheStats
{
    u64 hits{0};
    u64 misses{0};
    u64 read_requests{0};
    u64 write_requests{0};
    u64 device_reads{0};
    u64 device_writes{0};
    u64 bytes_read{0};
    u64 bytes_written{0};
    u64 errors{0};
};

class BlockCache final
{
  public:
    Status attach(driver::BlockDevice &device);
    Status read_block(u64 block, std::span<std::byte> out);
    Status write_block(u64 block, std::span<const std::byte> in);
    [[nodiscard]] BlockCacheStats stats() const;

  private:
    struct Entry
    {
        bool valid{false};
        u64 block{0};
        std::array<std::byte, driver::block_sector_size> data{};
    };

    driver::BlockDevice *device_{nullptr};
    std::array<Entry, block_cache_entries> entries_{};
    BlockCacheStats stats_{};
    usize next_victim_{0};
    mutable smp::SpinLock lock_{};
};

struct Partition
{
    u8 type{0};
    u64 first_lba{0};
    u64 block_count{0};
};

class PartitionTable final
{
  public:
    Status parse_mbr(std::span<const std::byte> sector);
    [[nodiscard]] const Partition *partition(usize index) const;
    [[nodiscard]] usize partition_count() const
    {
        return partitions_.size();
    }

  private:
    StaticVector<Partition, max_partitions> partitions_;
};

} // namespace ok::fs
