#include "ok/fs/devfs.hpp"

namespace ok::fs
{
namespace
{
constexpr std::array<std::string_view, DeviceFileSystem::device_count> device_names{"null", "zero", "full"};
// Linux device numbers, reported as the node's inode for a stable identity.
constexpr std::array<u64, DeviceFileSystem::device_count> device_inodes{3, 5, 7};
constexpr u64 directory_inode = 1;
// Fixed epoch timestamps with kernel provenance; the VFS supplies the device.
void kernel_attributes(FileAttributes &attributes, u64 inode)
{
    attributes.inode = inode;
    attributes.mark(Attribute::inode);
    attributes.mark(Attribute::blocks512);
    for (auto *time : {&attributes.access, &attributes.modification, &attributes.change})
        *time = {.seconds = 0, .nanoseconds = 0, .source = TimestampSource::kernel_fixed};
    attributes.mark(Attribute::access_time);
    attributes.mark(Attribute::modification_time);
    attributes.mark(Attribute::change_time);
}
} // namespace

std::string_view DeviceFileSystem::Device::name() const
{
    return device_names[static_cast<usize>(kind_)];
}

Metadata DeviceFileSystem::Device::metadata() const
{
    Metadata result{.type = NodeType::device, .size = 0, .mode = mode_for(NodeType::device, 0666u), .uid = 0,
                    .gid = 0, .link_count = 1, .block_size = metadata_block_size, .blocks = 0};
    kernel_attributes(result.attributes, device_inodes[static_cast<usize>(kind_)]);
    return result;
}

Result<FileBuffer> DeviceFileSystem::Device::read(usize, usize count) const
{
    FileBuffer bytes{};
    if (kind_ != Kind::null)
        bytes.size = count < bytes.data.size() ? count : bytes.data.size();
    return bytes;
}

Status DeviceFileSystem::Device::write(usize, std::span<const std::byte>)
{
    return kind_ == Kind::full ? Status::no_space("device is full") : Status::success();
}

Metadata DeviceFileSystem::Directory::metadata() const
{
    Metadata result{.type = NodeType::directory, .size = 0, .mode = mode_for(NodeType::directory, 0755u), .uid = 0,
                    .gid = 0, .link_count = 2, .block_size = metadata_block_size, .blocks = 0};
    kernel_attributes(result.attributes, directory_inode);
    return result;
}

Result<DirectoryListing> DeviceFileSystem::Directory::list(DirectoryReadIntent) const
{
    Result<DirectoryListing> result{std::in_place};
    auto &listing = result.value();
    for (const auto &device : owner_.devices_)
    {
        auto &entry = listing.entries[listing.count++];
        if (auto status = entry.name.assign(device.name()); !status) return status;
        entry.metadata = device.metadata();
    }
    return result;
}

Node *DeviceFileSystem::Directory::lookup(std::string_view child)
{
    for (auto &device : owner_.devices_)
        if (device.name() == child) return &device;
    return nullptr;
}
} // namespace ok::fs
