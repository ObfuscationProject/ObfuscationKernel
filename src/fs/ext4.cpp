#include "ok/fs/ext4.hpp"

namespace ok::fs
{
namespace
{

u16 read_le16(std::span<const std::byte> image, usize offset)
{
    return static_cast<u16>(std::to_integer<u16>(image[offset]) | (std::to_integer<u16>(image[offset + 1]) << 8));
}

u32 read_le32(std::span<const std::byte> image, usize offset)
{
    return static_cast<u32>(read_le16(image, offset)) | (static_cast<u32>(read_le16(image, offset + 2)) << 16);
}

void copy_bytes(std::span<std::byte> destination, std::span<const std::byte> source)
{
    const auto count = destination.size() < source.size() ? destination.size() : source.size();
    for (usize i = 0; i < count; ++i)
    {
        destination[i] = source[i];
    }
}

} // namespace

Status Ext4Volume::mount(std::span<const std::byte> image)
{
    if (image.size() < ext4_superblock_offset + 1024)
    {
        return Status::invalid_argument("image is too small for an ext4 superblock");
    }

    Ext4Volume candidate;
    if (auto status = candidate.parse_superblock(image.subspan(ext4_superblock_offset, 1024)); !status.ok())
    {
        return status;
    }
    if (candidate.info_.block_count_low > image.size() / candidate.info_.block_size)
    {
        return Status::invalid_argument("ext4 filesystem exceeds the backing image");
    }

    info_ = candidate.info_;
    image_ = image;
    device_ = nullptr;
    mounted_ = true;
    return Status::success();
}

Status Ext4Volume::mount(driver::BlockDevice &device)
{
    const auto geometry = device.geometry();
    if (geometry.block_size != driver::block_sector_size)
    {
        return Status::unsupported("EXT4 block-device mount currently requires 512 byte sectors");
    }
    if (geometry.block_count < 4)
    {
        return Status::invalid_argument("block device is too small for an ext4 superblock");
    }
    Ext4Volume candidate;
    if (auto status = device.read_blocks(ext4_superblock_offset / driver::block_sector_size,
                                         candidate.superblock_);
        !status.ok())
    {
        return status;
    }
    if (auto status = candidate.parse_superblock(candidate.superblock_);
        !status.ok())
    {
        return status;
    }
    if (candidate.info_.block_count_low >
        geometry.block_count / (candidate.info_.block_size / driver::block_sector_size))
    {
        return Status::invalid_argument("ext4 filesystem exceeds the backing device");
    }

    info_ = candidate.info_;
    superblock_ = candidate.superblock_;
    image_ = {};
    device_ = &device;
    mounted_ = true;
    return Status::success();
}

Status Ext4Volume::parse_superblock(std::span<const std::byte> superblock)
{
    if (superblock.size() < 1024)
    {
        return Status::invalid_argument("ext4 superblock buffer is too small");
    }
    if (read_le16(superblock, 0x38) != ext4_superblock_magic)
    {
        return Status::invalid_argument("ext4 superblock magic mismatch");
    }

    const auto log_block_size = read_le32(superblock, 0x18);
    if (log_block_size > 6)
    {
        return Status::invalid_argument("ext4 block size exponent is invalid");
    }

    info_ = Ext4MountInfo{};
    info_.inode_count = read_le32(superblock, 0x00);
    info_.block_count_low = read_le32(superblock, 0x04);
    info_.free_block_count_low = read_le32(superblock, 0x0c);
    info_.block_size = 1024u << log_block_size;
    info_.inode_size = read_le16(superblock, 0x58);
    info_.has_extents = (read_le32(superblock, 0x60) & 0x40u) != 0;
    if ((read_le32(superblock, 0x60) & 0x80u) || read_le32(superblock, 0x150))
    {
        return Status::unsupported("ext4 diagnostic volume requires 32-bit block counts");
    }
    if (!info_.block_count_low || info_.free_block_count_low > info_.block_count_low ||
        static_cast<u64>(info_.block_count_low) * info_.block_size < ext4_superblock_offset + 1024)
    {
        return Status::invalid_argument("ext4 block counts are invalid");
    }

    const auto *label = reinterpret_cast<const char *>(superblock.data() + 0x78);
    usize label_size = 0;
    while (label_size < 16 && label[label_size] != '\0')
    {
        ++label_size;
    }
    if (auto status = info_.volume_name.assign(std::string_view{label, label_size}); !status.ok())
    {
        return status;
    }
    return Status::success();
}

Result<Ext4MountInfo> Ext4Volume::info() const
{
    if (!mounted_)
    {
        return Status::not_initialized("ext4 volume is not mounted");
    }
    return info_;
}

Status Ext4Volume::read_block(u64 block, std::span<std::byte> out) const
{
    if (!mounted_)
    {
        return Status::not_initialized("ext4 volume is not mounted");
    }
    if (out.size() < info_.block_size)
    {
        return Status::invalid_argument("output buffer is smaller than ext4 block size");
    }
    if (block >= info_.block_count_low)
    {
        return Status::invalid_argument("ext4 block read is out of range");
    }
    if (device_ != nullptr)
    {
        if ((info_.block_size % driver::block_sector_size) != 0)
        {
            return Status::unsupported("ext4 block size is not sector aligned");
        }
        const auto sectors_per_block = info_.block_size / driver::block_sector_size;
        const auto geometry = device_->geometry();
        if (geometry.block_size != driver::block_sector_size ||
            block >= geometry.block_count / sectors_per_block)
        {
            return Status::invalid_argument("ext4 block read exceeds the backing device");
        }
        const auto sector = block * sectors_per_block;
        return device_->read_blocks(sector, out.first(info_.block_size));
    }
    if (block >= image_.size() / info_.block_size)
    {
        return Status::invalid_argument("ext4 block read is out of range");
    }
    const auto offset = static_cast<usize>(block) * info_.block_size;
    copy_bytes(out.first(info_.block_size), image_.subspan(offset, info_.block_size));
    return Status::success();
}

} // namespace ok::fs
