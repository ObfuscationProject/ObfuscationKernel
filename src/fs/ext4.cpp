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

    const auto base = ext4_superblock_offset;
    if (read_le16(image, base + 0x38) != ext4_superblock_magic)
    {
        return Status::invalid_argument("ext4 superblock magic mismatch");
    }

    const auto log_block_size = read_le32(image, base + 0x18);
    if (log_block_size > 16)
    {
        return Status::invalid_argument("ext4 block size exponent is invalid");
    }

    info_ = Ext4MountInfo{};
    info_.inode_count = read_le32(image, base + 0x00);
    info_.block_count_low = read_le32(image, base + 0x04);
    info_.free_block_count_low = read_le32(image, base + 0x0c);
    info_.block_size = 1024u << log_block_size;
    info_.inode_size = read_le16(image, base + 0x58);
    info_.has_extents = (read_le32(image, base + 0x60) & 0x40u) != 0;

    const auto *label = reinterpret_cast<const char *>(image.data() + base + 0x78);
    usize label_size = 0;
    while (label_size < 16 && label[label_size] != '\0')
    {
        ++label_size;
    }
    if (auto status = info_.volume_name.assign(std::string_view{label, label_size}); !status.ok())
    {
        return status;
    }

    image_ = image;
    mounted_ = true;
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
    const auto offset = static_cast<usize>(block) * info_.block_size;
    if (offset + info_.block_size > image_.size())
    {
        return Status::invalid_argument("ext4 block read is out of range");
    }
    copy_bytes(out.first(info_.block_size), image_.subspan(offset, info_.block_size));
    return Status::success();
}

} // namespace ok::fs
