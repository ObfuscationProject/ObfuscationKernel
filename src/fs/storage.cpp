#include "ok/fs/storage.hpp"

#include <cstddef>

namespace ok::fs
{
namespace
{

u32 read_le32(std::span<const std::byte> bytes, usize offset)
{
    u32 value = 0;
    for (usize i = 0; i < 4; ++i)
    {
        value |= static_cast<u32>(std::to_integer<u8>(bytes[offset + i])) << (i * 8);
    }
    return value;
}

} // namespace

Status BlockCache::attach(driver::BlockDevice &device)
{
    smp::ScopedSpinLock guard(lock_);
    device_ = &device;
    entries_ = {};
    stats_ = {};
    next_victim_ = 0;
    return Status::success();
}

BlockCacheStats BlockCache::stats() const
{
    smp::ScopedSpinLock guard(lock_);
    return stats_;
}

Status BlockCache::read_block(u64 block, std::span<std::byte> out)
{
    smp::ScopedSpinLock guard(lock_);
    if (device_ == nullptr)
    {
        return Status::not_initialized("block cache has no device");
    }
    if (out.size() != driver::block_sector_size)
    {
        ++stats_.errors;
        return Status::invalid_argument("block cache reads exactly one sector");
    }
    ++stats_.read_requests;
    const auto geometry = device_->geometry();
    if (block >= geometry.block_count)
    {
        ++stats_.errors;
        return Status::invalid_argument("block read is out of range");
    }
    for (auto &entry : entries_)
    {
        if (entry.valid && entry.block == block)
        {
            ++stats_.hits;
            stats_.bytes_read += out.size();
            for (usize i = 0; i < out.size(); ++i)
            {
                out[i] = entry.data[i];
            }
            return Status::success();
        }
    }
    ++stats_.misses;
    auto &victim = entries_[next_victim_++ % entries_.size()];
    if (auto status = device_->read_blocks(block, victim.data); !status.ok())
    {
        ++stats_.errors;
        return status;
    }
    ++stats_.device_reads;
    stats_.bytes_read += out.size();
    victim.valid = true;
    victim.block = block;
    for (usize i = 0; i < out.size(); ++i)
    {
        out[i] = victim.data[i];
    }
    return Status::success();
}

Status BlockCache::write_block(u64 block, std::span<const std::byte> in)
{
    smp::ScopedSpinLock guard(lock_);
    if (device_ == nullptr)
    {
        return Status::not_initialized("block cache has no device");
    }
    if (in.size() != driver::block_sector_size)
    {
        ++stats_.errors;
        return Status::invalid_argument("block cache writes exactly one sector");
    }
    ++stats_.write_requests;
    const auto geometry = device_->geometry();
    if (block >= geometry.block_count)
    {
        ++stats_.errors;
        return Status::invalid_argument("block write is out of range");
    }
    if (auto status = device_->write_blocks(block, in); !status.ok())
    {
        ++stats_.errors;
        return status;
    }
    ++stats_.device_writes;
    stats_.bytes_written += in.size();
    for (auto &entry : entries_)
    {
        if (entry.valid && entry.block == block)
        {
            for (usize i = 0; i < in.size(); ++i)
            {
                entry.data[i] = in[i];
            }
            return Status::success();
        }
    }
    return Status::success();
}

Status PartitionTable::parse_mbr(std::span<const std::byte> sector)
{
    if (sector.size() < driver::block_sector_size)
    {
        return Status::invalid_argument("MBR sector is too small");
    }
    if (std::to_integer<u8>(sector[510]) != 0x55 || std::to_integer<u8>(sector[511]) != 0xaa)
    {
        return Status::invalid_argument("MBR signature is invalid");
    }
    partitions_ = {};
    for (usize i = 0; i < 4; ++i)
    {
        const usize offset = 446 + i * 16;
        const u8 type = std::to_integer<u8>(sector[offset + 4]);
        const u32 first_lba = read_le32(sector, offset + 8);
        const u32 count = read_le32(sector, offset + 12);
        if (type == 0 || count == 0)
        {
            continue;
        }
        if (auto status = partitions_.push_back(
                Partition{.type = type, .first_lba = first_lba, .block_count = count});
            !status.ok())
        {
            return status;
        }
    }
    return partitions_.empty() ? Status::not_found("MBR has no partitions") : Status::success();
}

const Partition *PartitionTable::partition(usize index) const
{
    return index < partitions_.size() ? &partitions_[index] : nullptr;
}

} // namespace ok::fs
