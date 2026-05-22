#include "ok/driver/driver.hpp"

namespace ok::driver
{

Status NullBlockDriver::probe()
{
    return Status::success();
}

Status NullBlockDriver::start()
{
    started_ = true;
    stats_ = {};
    return Status::success();
}

Status NullBlockDriver::stop()
{
    started_ = false;
    return Status::success();
}

BlockGeometry NullBlockDriver::geometry() const
{
    return BlockGeometry{.block_count = 0, .block_size = block_sector_size, .writable = true};
}

Status NullBlockDriver::read_blocks(u64, std::span<std::byte> out)
{
    if (!started_)
    {
        ++stats_.errors;
        return Status::not_initialized("null block driver not started");
    }
    ++stats_.read_operations;
    stats_.bytes_read += out.size();
    for (auto &byte : out)
    {
        byte = std::byte{0};
    }
    return Status::success();
}

Status NullBlockDriver::write_blocks(u64, std::span<const std::byte> in)
{
    if (!started_)
    {
        ++stats_.errors;
        return Status::not_initialized("null block driver not started");
    }
    ++stats_.write_operations;
    stats_.bytes_written += in.size();
    return Status::success();
}

Status NullBlockDriver::read(uptr offset, std::span<std::byte> out)
{
    return read_blocks(offset / block_sector_size, out);
}

Status NullBlockDriver::write(uptr offset, std::span<const std::byte> in)
{
    return write_blocks(offset / block_sector_size, in);
}

Status RamBlockDriver::probe()
{
    return Status::success();
}

Status RamBlockDriver::start()
{
    started_ = true;
    stats_ = {};
    return Status::success();
}

Status RamBlockDriver::stop()
{
    started_ = false;
    return Status::success();
}

BlockGeometry RamBlockDriver::geometry() const
{
    return BlockGeometry{
        .block_count = ram_block_sector_count,
        .block_size = block_sector_size,
        .writable = true,
    };
}

Status RamBlockDriver::check_transfer(u64 first_block, usize byte_count) const
{
    if (!started_)
    {
        return Status::not_initialized("RAM block driver not started");
    }
    if ((byte_count % block_sector_size) != 0)
    {
        return Status::invalid_argument("block transfer size is not sector aligned");
    }
    const auto block_count = static_cast<u64>(byte_count / block_sector_size);
    if (first_block > ram_block_sector_count || block_count > ram_block_sector_count - first_block)
    {
        return Status::invalid_argument("block transfer is out of range");
    }
    return Status::success();
}

Status RamBlockDriver::read_blocks(u64 first_block, std::span<std::byte> out)
{
    if (auto status = check_transfer(first_block, out.size()); !status.ok())
    {
        ++stats_.errors;
        return status;
    }
    ++stats_.read_operations;
    stats_.bytes_read += out.size();
    const auto offset = static_cast<usize>(first_block) * block_sector_size;
    for (usize i = 0; i < out.size(); ++i)
    {
        out[i] = storage_[offset + i];
    }
    return Status::success();
}

Status RamBlockDriver::write_blocks(u64 first_block, std::span<const std::byte> in)
{
    if (auto status = check_transfer(first_block, in.size()); !status.ok())
    {
        ++stats_.errors;
        return status;
    }
    ++stats_.write_operations;
    stats_.bytes_written += in.size();
    const auto offset = static_cast<usize>(first_block) * block_sector_size;
    for (usize i = 0; i < in.size(); ++i)
    {
        storage_[offset + i] = in[i];
    }
    return Status::success();
}

Status RamBlockDriver::clear()
{
    if (!started_)
    {
        return Status::not_initialized("RAM block driver not started");
    }
    for (auto &byte : storage_)
    {
        byte = std::byte{0};
    }
    return Status::success();
}

Status VirtioBlockDriver::probe()
{
    return Status::success();
}

Status VirtioBlockDriver::start()
{
    started_ = true;
    stats_ = {};
    return Status::success();
}

Status VirtioBlockDriver::stop()
{
    started_ = false;
    bound_ = false;
    return Status::success();
}

Status VirtioBlockDriver::bind(const PciDevice &device)
{
    if (!started_)
    {
        return Status::not_initialized("virtio block driver not started");
    }
    if (device.id.vendor_id != 0x1af4 || device.id.class_code != 0x01)
    {
        return Status::invalid_argument("PCI device is not a virtio block device");
    }
    device_ = device;
    bound_ = true;
    return Status::success();
}

BlockGeometry VirtioBlockDriver::geometry() const
{
    return BlockGeometry{
        .block_count = virtio_block_sector_count,
        .block_size = block_sector_size,
        .writable = true,
    };
}

Status VirtioBlockDriver::check_transfer(u64 first_block, usize byte_count) const
{
    if (!started_)
    {
        return Status::not_initialized("virtio block driver not started");
    }
    if (!bound_)
    {
        return Status::not_initialized("virtio block driver has no PCI device");
    }
    if ((byte_count % block_sector_size) != 0)
    {
        return Status::invalid_argument("block transfer size is not sector aligned");
    }
    const auto block_count = static_cast<u64>(byte_count / block_sector_size);
    if (first_block > virtio_block_sector_count || block_count > virtio_block_sector_count - first_block)
    {
        return Status::invalid_argument("block transfer is out of range");
    }
    return Status::success();
}

Status VirtioBlockDriver::read_blocks(u64 first_block, std::span<std::byte> out)
{
    if (auto status = check_transfer(first_block, out.size()); !status.ok())
    {
        ++stats_.errors;
        return status;
    }
    ++stats_.read_operations;
    stats_.bytes_read += out.size();
    const auto offset = static_cast<usize>(first_block) * block_sector_size;
    for (usize i = 0; i < out.size(); ++i)
    {
        out[i] = storage_[offset + i];
    }
    return Status::success();
}

Status VirtioBlockDriver::write_blocks(u64 first_block, std::span<const std::byte> in)
{
    if (auto status = check_transfer(first_block, in.size()); !status.ok())
    {
        ++stats_.errors;
        return status;
    }
    ++stats_.write_operations;
    stats_.bytes_written += in.size();
    const auto offset = static_cast<usize>(first_block) * block_sector_size;
    for (usize i = 0; i < in.size(); ++i)
    {
        storage_[offset + i] = in[i];
    }
    return Status::success();
}

Status VirtioBlockDriver::clear()
{
    if (!started_)
    {
        return Status::not_initialized("virtio block driver not started");
    }
    for (auto &byte : storage_)
    {
        byte = std::byte{0};
    }
    return Status::success();
}

} // namespace ok::driver
