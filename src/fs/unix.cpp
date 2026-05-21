#include "ok/fs/unix.hpp"

#include <cstddef>

namespace ok::fs
{

Status MountNamespace::mount(std::string_view mount_path, SuperBlock superblock)
{
    if (mount_path.empty() || mount_path[0] != '/')
    {
        return Status::invalid_argument("mount path must be absolute");
    }
    if (lookup_mount(mount_path) != nullptr)
    {
        return Status::already_exists("mount path is already mounted");
    }
    Mount mount{};
    if (auto status = mount.path.assign(mount_path); !status.ok())
    {
        return status;
    }
    mount.superblock = superblock;
    return mounts_.push_back(mount);
}

const Mount *MountNamespace::lookup_mount(std::string_view mount_path) const
{
    for (const auto &mount : mounts_)
    {
        if (mount.path.view() == mount_path)
        {
            return &mount;
        }
    }
    return nullptr;
}

Result<usize> DeviceNode::read(std::span<std::byte> out)
{
    switch (kind_)
    {
    case DeviceKind::null:
    case DeviceKind::console:
    case DeviceKind::tty:
    case DeviceKind::block:
        return usize{0};
    case DeviceKind::zero:
        for (auto &byte : out)
        {
            byte = std::byte{0};
        }
        return out.size();
    case DeviceKind::random:
        for (auto &byte : out)
        {
            random_state_ ^= random_state_ << 13;
            random_state_ ^= random_state_ >> 7;
            random_state_ ^= random_state_ << 17;
            byte = static_cast<std::byte>(random_state_ & 0xffu);
        }
        return out.size();
    }
    return Status::unsupported("device read is not implemented");
}

Result<usize> DeviceNode::write(std::span<const std::byte> in)
{
    switch (kind_)
    {
    case DeviceKind::null:
    case DeviceKind::console:
    case DeviceKind::tty:
    case DeviceKind::block:
        bytes_written_ += in.size();
        return in.size();
    case DeviceKind::zero:
    case DeviceKind::random:
        return Status::invalid_argument("device is not writable");
    }
    return Status::unsupported("device write is not implemented");
}

Result<u32> DeviceNode::poll()
{
    constexpr u32 readable = 0x01;
    constexpr u32 writable = 0x04;
    switch (kind_)
    {
    case DeviceKind::null:
    case DeviceKind::console:
    case DeviceKind::tty:
    case DeviceKind::block:
        return writable;
    case DeviceKind::zero:
    case DeviceKind::random:
        return readable;
    }
    return 0u;
}

Result<usize> Pipe::read(std::span<std::byte> out)
{
    if (size_ == 0)
    {
        return non_blocking_ ? Result<usize>{Status::would_block("pipe is empty")} : Result<usize>{usize{0}};
    }
    usize count = 0;
    while (count < out.size() && size_ > 0)
    {
        out[count++] = data_[head_];
        head_ = (head_ + 1) % data_.size();
        --size_;
    }
    return count;
}

Result<usize> Pipe::write(std::span<const std::byte> in)
{
    if (size_ == data_.size())
    {
        return non_blocking_ ? Result<usize>{Status::would_block("pipe is full")} : Result<usize>{usize{0}};
    }
    usize count = 0;
    while (count < in.size() && size_ < data_.size())
    {
        data_[tail_] = in[count++];
        tail_ = (tail_ + 1) % data_.size();
        ++size_;
    }
    return count;
}

Result<u32> Pipe::poll()
{
    constexpr u32 readable = 0x01;
    constexpr u32 writable = 0x04;
    return static_cast<u32>((size_ > 0 ? readable : 0u) | (size_ < data_.size() ? writable : 0u));
}

Result<usize> TtyDevice::read(std::span<std::byte>)
{
    return Status::would_block("TTY input buffer is empty");
}

Result<usize> TtyDevice::write(std::span<const std::byte> in)
{
    bytes_written_ += in.size();
    return in.size();
}

Result<u32> TtyDevice::poll()
{
    return 0x04u;
}

Result<i64> TtyDevice::ioctl(u64 request, u64 argument)
{
    constexpr u64 tcgets = 0x5401;
    constexpr u64 tcsets = 0x5402;
    if (request == tcgets)
    {
        return echo_ ? 1 : 0;
    }
    if (request == tcsets)
    {
        echo_ = argument != 0;
        return 0;
    }
    return Status::unsupported("TTY ioctl request is not implemented");
}

Status UnixVfsModel::initialize(VirtualFileSystem &vfs)
{
    vfs_ = &vfs;
    SuperBlock root{};
    SuperBlock dev{};
    static_cast<void>(root.filesystem.assign("ramfs"));
    static_cast<void>(dev.filesystem.assign("devfs"));
    if (auto status = mounts_.mount("/", root); !status.ok())
    {
        return status;
    }
    if (auto status = mounts_.mount("/dev", dev); !status.ok())
    {
        return status;
    }
    static_cast<void>(vfs.create("/dev/null", NodeType::device));
    static_cast<void>(vfs.create("/dev/zero", NodeType::device));
    static_cast<void>(vfs.create("/dev/console", NodeType::device));
    static_cast<void>(vfs.create("/dev/tty0", NodeType::device));
    static_cast<void>(vfs.create("/proc", NodeType::directory));
    return Status::success();
}

Status UnixVfsModel::validate_mounts()
{
    if (mounts_.lookup_mount("/") == nullptr || mounts_.lookup_mount("/dev") == nullptr || vfs_ == nullptr)
    {
        return Status::fault("mount namespace validation failed");
    }
    return Status::success();
}

Status UnixVfsModel::validate_files()
{
    if (vfs_ == nullptr)
    {
        return Status::not_initialized("VFS model is not initialized");
    }
    static_cast<void>(vfs_->unlink("/tmp/roadmap-file"));
    if (auto status = vfs_->create("/tmp/roadmap-file", NodeType::regular); !status.ok())
    {
        return status;
    }
    constexpr std::string_view text{"hello"};
    if (auto status =
            vfs_->write_file("/tmp/roadmap-file",
                             std::span<const std::byte>{reinterpret_cast<const std::byte *>(text.data()), text.size()});
        !status.ok())
    {
        return status;
    }
    auto read = vfs_->read_file("/tmp/roadmap-file");
    if (!read || read.value().size != text.size())
    {
        return Status::fault("regular file validation failed");
    }
    return vfs_->write_file("/tmp/roadmap-file", std::span<const std::byte>{});
}

Status UnixVfsModel::validate_directories()
{
    if (vfs_ == nullptr)
    {
        return Status::not_initialized("VFS model is not initialized");
    }
    static_cast<void>(vfs_->create("/tmp/roadmap-dir", NodeType::directory));
    auto listing = vfs_->list("/tmp");
    if (!listing || listing.value().count == 0)
    {
        return Status::fault("directory listing validation failed");
    }
    return Status::success();
}

Status UnixVfsModel::validate_symlink()
{
    if (vfs_ == nullptr)
    {
        return Status::not_initialized("VFS model is not initialized");
    }
    static_cast<void>(vfs_->unlink("/tmp/roadmap-link"));
    if (auto status = vfs_->create("/tmp/roadmap-link", NodeType::symlink); !status.ok())
    {
        return status;
    }
    constexpr std::string_view target{"/tmp/roadmap-file"};
    if (auto status =
            vfs_->write_file("/tmp/roadmap-link",
                             std::span<const std::byte>{reinterpret_cast<const std::byte *>(target.data()), target.size()});
        !status.ok())
    {
        return status;
    }
    auto read = vfs_->read_file("/tmp/roadmap-link");
    return read && read.value().size == target.size() ? Status::success()
                                                      : Status::fault("symlink validation failed");
}

Status UnixVfsModel::validate_devices()
{
    std::array<std::byte, 8> zeros{};
    auto zero_count = zero_.read(zeros);
    if (!zero_count || zero_count.value() != zeros.size())
    {
        return Status::fault("zero device validation failed");
    }
    constexpr std::string_view text{"console"};
    auto console_count =
        console_.write(std::span<const std::byte>{reinterpret_cast<const std::byte *>(text.data()), text.size()});
    if (!console_count || console_count.value() != text.size())
    {
        return Status::fault("console device validation failed");
    }
    return Status::success();
}

Status UnixVfsModel::validate_pipe()
{
    pipe_.set_non_blocking(true);
    constexpr std::string_view text{"pipe"};
    auto written = pipe_.write(std::span<const std::byte>{reinterpret_cast<const std::byte *>(text.data()), text.size()});
    if (!written || written.value() != text.size())
    {
        return Status::fault("pipe write validation failed");
    }
    std::array<std::byte, 8> out{};
    auto read = pipe_.read(out);
    if (!read || read.value() != text.size())
    {
        return Status::fault("pipe read validation failed");
    }
    auto empty = pipe_.read(out);
    return !empty && empty.status().code() == StatusCode::would_block ? Status::success()
                                                                      : Status::fault("pipe non-blocking validation failed");
}

Status UnixVfsModel::validate_tty()
{
    constexpr std::string_view text{"tty"};
    auto written = tty_.write(std::span<const std::byte>{reinterpret_cast<const std::byte *>(text.data()), text.size()});
    if (!written || written.value() != text.size())
    {
        return Status::fault("TTY write validation failed");
    }
    auto result = tty_.ioctl(0x5402, 0);
    if (!result || tty_.echo())
    {
        return Status::fault("TTY ioctl validation failed");
    }
    return Status::success();
}

} // namespace ok::fs
