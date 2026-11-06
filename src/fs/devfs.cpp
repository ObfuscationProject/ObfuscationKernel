#include "ok/fs/devfs.hpp"

namespace ok::fs
{
namespace
{
using Kind = DeviceFileSystem::Kind;
constexpr std::array<std::string_view, DeviceFileSystem::device_count> device_names{"null", "zero", "full",
                                                                                   "tty", "console", "ptmx"};
constexpr std::string_view pts_digits{"0123456789"};
constexpr u64 directory_inode = 1, pts_directory_inode = 2;

constexpr TerminalNodeKind terminal_kind(Kind kind)
{
    switch (kind)
    {
    case Kind::controlling:
        return TerminalNodeKind::controlling;
    case Kind::console:
        return TerminalNodeKind::console;
    case Kind::multiplexer:
        return TerminalNodeKind::multiplexer;
    default:
        return TerminalNodeKind::none;
    }
}
// Linux device numbers; the memory devices keep their minor as the inode.
constexpr FilesystemDevice device_number(Kind kind)
{
    switch (kind)
    {
    case Kind::null:
        return {1, 3};
    case Kind::zero:
        return {1, 5};
    case Kind::full:
        return {1, 7};
    default:
        return terminal_device(terminal_kind(kind));
    }
}
constexpr u64 device_inode(Kind kind)
{
    return terminal_kind(kind) == TerminalNodeKind::none ? device_number(kind).minor : terminal_inode(terminal_kind(kind));
}
// Linux devtmpfs modes: /dev/console is 0600, the others 0666.
constexpr u32 device_mode(Kind kind)
{
    return kind == Kind::console ? 0600u : 0666u;
}

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
Metadata device_metadata(u64 inode, FilesystemDevice number, u32 mode, u32 uid, u32 gid)
{
    Metadata result{.type = NodeType::device, .size = 0, .mode = mode_for(NodeType::device, mode), .uid = uid,
                    .gid = gid, .link_count = 1, .block_size = metadata_block_size, .blocks = 0};
    kernel_attributes(result.attributes, inode);
    result.attributes.special = number;
    result.attributes.mark(Attribute::special_device);
    return result;
}
Metadata directory_metadata(u64 inode)
{
    Metadata result{.type = NodeType::directory, .size = 0, .mode = mode_for(NodeType::directory, 0755u), .uid = 0,
                    .gid = 0, .link_count = 2, .block_size = metadata_block_size, .blocks = 0};
    kernel_attributes(result.attributes, inode);
    return result;
}
// Strict decimal: no sign, no leading zero, below the capacity.
bool pts_index(std::string_view name, u16 &index)
{
    if (name.size() != 1 || name[0] < '0' || name[0] > '9') return false;
    const auto value = static_cast<usize>(name[0] - '0');
    if (value >= pts_capacity) return false;
    index = static_cast<u16>(value);
    return true;
}
static_assert(pts_capacity <= 10, "pts names are one decimal digit");
} // namespace

Status DeviceFileSystem::attach_terminals(VirtualFileSystem &vfs)
{
    auto *terminals = vfs.terminal_namespace();
    if (!terminals) return Status::not_initialized("VFS has no terminal namespace");
    return attach_terminals(*terminals);
}

Status DeviceFileSystem::attach_terminals(TerminalNamespace &terminals)
{
    if (terminals_ == &terminals) return Status::success();
    if (terminals_) return Status::busy("device filesystem already has a terminal namespace");
    if (auto status = terminals.bind_provider(this); !status) return status;
    terminals_ = &terminals;
    return Status::success();
}

std::string_view DeviceFileSystem::Device::name() const
{
    return device_names[static_cast<usize>(kind_)];
}

Metadata DeviceFileSystem::Device::metadata() const
{
    return device_metadata(device_inode(kind_), device_number(kind_), device_mode(kind_), 0, 0);
}

TerminalNode DeviceFileSystem::Device::terminal_node() const
{
    return {.kind = terminal_kind(kind_)};
}

Result<FileBuffer> DeviceFileSystem::Device::read(usize, usize count) const
{
    if (terminal_kind(kind_) != TerminalNodeKind::none)
        return Status::unsupported("terminal nodes open through the terminal service");
    FileBuffer bytes{};
    if (kind_ != Kind::null)
        bytes.size = count < bytes.data.size() ? count : bytes.data.size();
    return bytes;
}

Status DeviceFileSystem::Device::write(usize, std::span<const std::byte>)
{
    if (terminal_kind(kind_) != TerminalNodeKind::none)
        return Status::unsupported("terminal nodes open through the terminal service");
    return kind_ == Kind::full ? Status::no_space("device is full") : Status::success();
}

Metadata DeviceFileSystem::Directory::metadata() const
{
    return directory_metadata(directory_inode);
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
    auto &entry = listing.entries[listing.count++];
    if (auto status = entry.name.assign(owner_.pts_root_.name()); !status) return status;
    entry.metadata = owner_.pts_root_.metadata();
    return result;
}

Node *DeviceFileSystem::Directory::lookup(std::string_view child)
{
    for (auto &device : owner_.devices_)
        if (device.name() == child) return &device;
    if (child == owner_.pts_root_.name()) return &owner_.pts_root_;
    return nullptr;
}

Metadata DeviceFileSystem::PtsDirectory::metadata() const
{
    return directory_metadata(pts_directory_inode);
}

Result<DirectoryListing> DeviceFileSystem::PtsDirectory::list(DirectoryReadIntent) const
{
    Result<DirectoryListing> result{std::in_place};
    auto &listing = result.value();
    for (const auto &device : owner_.pts_)
    {
        if (!device.view().present) continue;
        auto &entry = listing.entries[listing.count++];
        if (auto status = entry.name.assign(device.name()); !status) return status;
        entry.metadata = device.metadata();
    }
    return result;
}

Node *DeviceFileSystem::PtsDirectory::lookup(std::string_view child)
{
    if (child == "..") return &owner_.root_;
    u16 index{};
    if (!pts_index(child, index) || !owner_.pts_[index].view().present) return nullptr;
    return &owner_.pts_[index];
}

std::string_view DeviceFileSystem::PtsDevice::name() const
{
    return pts_digits.substr(index_, 1);
}

PtsEntryView DeviceFileSystem::PtsDevice::view() const
{
    return owner_.terminals_ ? owner_.terminals_->entry(index_) : PtsEntryView{};
}

Metadata DeviceFileSystem::PtsDevice::metadata() const
{
    const auto entry = view();
    return device_metadata(terminal_inode(TerminalNodeKind::pts_slave, index_),
                           terminal_device(TerminalNodeKind::pts_slave, index_), entry.mode, entry.uid, entry.gid);
}

// A reference kept across the master's last close is stale: ENOENT.
Result<Metadata> DeviceFileSystem::PtsDevice::query_metadata() const
{
    if (!view().present) return Status::not_found("pts entry was removed");
    return metadata();
}

TerminalNode DeviceFileSystem::PtsDevice::terminal_node() const
{
    return {.kind = TerminalNodeKind::pts_slave, .index = index_, .generation = view().generation};
}

Result<FileBuffer> DeviceFileSystem::PtsDevice::read(usize, usize) const
{
    return Status::unsupported("terminal nodes open through the terminal service");
}

Status DeviceFileSystem::PtsDevice::write(usize, std::span<const std::byte>)
{
    return Status::unsupported("terminal nodes open through the terminal service");
}
} // namespace ok::fs
