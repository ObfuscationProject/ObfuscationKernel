#include "ok/posix/posix.hpp"

namespace ok::posix
{
namespace
{

bool is_readable(u32 flags)
{
    const auto access = flags & 0x3u;
    return access == o_RDONLY || access == o_RDWR;
}

bool is_writable(u32 flags)
{
    const auto access = flags & 0x3u;
    return access == o_WRONLY || access == o_RDWR;
}

u32 access_for_open(u32 flags)
{
    u32 access = 0;
    if (is_readable(flags))
    {
        access |= fs::access_read;
    }
    if (is_writable(flags))
    {
        access |= fs::access_write;
    }
    return access;
}

} // namespace

Result<Fd> PosixService::open(std::string_view path, u32 flags, u32 mode)
{
    return openat(at_FDCWD, path, flags, mode);
}

Result<Fd> PosixService::openat(Fd dirfd, std::string_view path, u32 flags, u32 mode)
{
    if (!initialized_ || vfs_ == nullptr)
    {
        return Status::not_initialized("POSIX service not initialized");
    }
    auto normalized_result = resolve_path(dirfd, path);
    if (!normalized_result)
    {
        return normalized_result.status();
    }
    const auto normalized = normalized_result.value();
    auto *node = vfs_->lookup(normalized);
    bool created = false;
    if (node == nullptr)
    {
        if ((flags & o_CREAT) == 0)
        {
            return Status::not_found("path not found");
        }
        if (auto status = require_parent_directory_access(normalized, fs::access_write | fs::access_execute);
            !status.ok())
        {
            return status;
        }
        if (auto status = vfs_->create(normalized, fs::NodeType::regular); !status.ok())
        {
            return status;
        }
        const auto create_mode = mode & ~file_mode_mask_;
        if (auto status = vfs_->chmod(normalized, create_mode); !status.ok())
        {
            return status;
        }
        if (auto status = vfs_->chown(normalized, current_euid_, current_egid_); !status.ok())
        {
            return status;
        }
        node = vfs_->lookup(normalized);
        created = true;
    }
    if (node == nullptr)
    {
        return Status::fault("created path is not visible");
    }

    auto metadata = node->metadata();
    if ((flags & o_DIRECTORY) != 0 && metadata.type != fs::NodeType::directory)
    {
        return Status::invalid_argument("path is not a directory");
    }
    if (metadata.type == fs::NodeType::directory && is_writable(flags))
    {
        return Status::invalid_argument("directory is not writable");
    }
    u32 required_access = access_for_open(flags);
    if ((flags & o_TRUNC) != 0)
    {
        required_access |= fs::access_write;
    }
    if (!created)
    {
        if (auto status = fs::require_access(metadata, credentials(), required_access); !status.ok())
        {
            return status;
        }
    }
    if ((flags & o_TRUNC) != 0)
    {
        if (metadata.type != fs::NodeType::regular && metadata.type != fs::NodeType::device)
        {
            return Status::invalid_argument("cannot truncate non-regular file");
        }
        std::span<const std::byte> empty{};
        if (auto status = node->write(0, empty); !status.ok())
        {
            return status;
        }
        metadata = node->metadata();
    }

    auto fd = allocate_fd();
    if (!fd)
    {
        return fd.status();
    }
    auto &entry = files_[static_cast<usize>(fd.value())];
    entry.used = true;
    entry.readable = is_readable(flags);
    entry.writable = is_writable(flags);
    entry.console = false;
    entry.close_on_exec = (flags & o_CLOEXEC) != 0;
    entry.flags = flags;
    entry.offset = (flags & o_APPEND) != 0 ? node->metadata().size : 0;
    entry.node = node;
    if (auto status = entry.path.assign(normalized); !status.ok())
    {
        entry = {};
        return status;
    }
    return fd.value();
}

Status PosixService::close(Fd fd)
{
    if (fd >= 0 && fd <= 2)
    {
        return Status::success();
    }
    auto *entry = descriptor(fd);
    if (entry == nullptr || !entry->used)
    {
        return Status::invalid_argument("bad file descriptor");
    }
    *entry = {};
    return Status::success();
}

Status PosixService::close_range(Fd first, Fd last)
{
    if (first < 0 || last < first)
    {
        return Status::invalid_argument("invalid close range");
    }
    const auto capped = static_cast<usize>(last) >= files_.size() ? files_.size() - 1 : static_cast<usize>(last);
    for (usize fd = static_cast<usize>(first); fd <= capped; ++fd)
    {
        if (fd > 2 && files_[fd].used)
        {
            files_[fd] = {};
        }
    }
    return Status::success();
}

Result<Fd> PosixService::duplicate(Fd fd, Fd minimum)
{
    auto *entry = descriptor(fd);
    if (entry == nullptr || !entry->used)
    {
        return Status::invalid_argument("bad file descriptor");
    }
    auto target = allocate_fd(minimum);
    if (!target)
    {
        return target.status();
    }
    if (auto status = copy_descriptor(fd, target.value(), 0); !status.ok())
    {
        return status;
    }
    return target.value();
}

Result<Fd> PosixService::duplicate_to(Fd fd, Fd target, u32 flags)
{
    if (fd == target)
    {
        return target;
    }
    if (target < 0 || static_cast<usize>(target) >= files_.size())
    {
        return Status::invalid_argument("target file descriptor out of range");
    }
    auto *entry = descriptor(fd);
    if (entry == nullptr || !entry->used)
    {
        return Status::invalid_argument("bad file descriptor");
    }
    if (auto status = close(target); !status.ok())
    {
        return status;
    }
    if (auto status = copy_descriptor(fd, target, flags); !status.ok())
    {
        return status;
    }
    return target;
}

Result<usize> PosixService::read(Fd fd, std::span<std::byte> out)
{
    auto *entry = descriptor(fd);
    if (entry == nullptr || !entry->used || !entry->readable)
    {
        return Status::invalid_argument("file descriptor is not readable");
    }
    if (fd == 0)
    {
        return Status::would_block("stdin has no buffered data");
    }
    if (entry->node == nullptr)
    {
        return Status::invalid_argument("file descriptor has no backing node");
    }
    auto data = entry->node->read(entry->offset, out.size());
    if (!data)
    {
        return data.status();
    }
    const auto count = data.value().size < out.size() ? data.value().size : out.size();
    for (usize i = 0; i < count; ++i)
    {
        out[i] = data.value().data[i];
    }
    entry->offset += count;
    return count;
}

Result<usize> PosixService::write(Fd fd, std::span<const std::byte> in)
{
    auto *entry = descriptor(fd);
    if (entry == nullptr || !entry->used || !entry->writable)
    {
        return Status::invalid_argument("file descriptor is not writable");
    }
    if (entry->console)
    {
        const auto *text = reinterpret_cast<const char *>(in.data());
        if (auto status = console_->write(std::string_view{text, in.size()}); !status.ok())
        {
            return status;
        }
        return in.size();
    }
    if (entry->node == nullptr)
    {
        return Status::invalid_argument("file descriptor has no backing node");
    }
    if ((entry->flags & o_APPEND) != 0)
    {
        entry->offset = entry->node->metadata().size;
    }
    if (auto status = entry->node->write(entry->offset, in); !status.ok())
    {
        return status;
    }
    entry->offset += in.size();
    return in.size();
}

Result<usize> PosixService::pread(Fd fd, std::span<std::byte> out, usize offset)
{
    auto *entry = descriptor(fd);
    if (entry == nullptr || !entry->used)
    {
        return Status::invalid_argument("bad file descriptor");
    }
    const auto saved = entry->offset;
    entry->offset = offset;
    auto result = read(fd, out);
    entry->offset = saved;
    return result;
}

Result<usize> PosixService::pwrite(Fd fd, std::span<const std::byte> in, usize offset)
{
    auto *entry = descriptor(fd);
    if (entry == nullptr || !entry->used)
    {
        return Status::invalid_argument("bad file descriptor");
    }
    const auto saved = entry->offset;
    entry->offset = offset;
    auto result = write(fd, in);
    entry->offset = saved;
    return result;
}

Result<usize> PosixService::readv(Fd fd, std::span<const IoVector> vectors)
{
    usize total = 0;
    for (const auto &vector : vectors)
    {
        if (vector.base == 0 && vector.length != 0)
        {
            return total == 0 ? Result<usize>{Status::invalid_argument("readv buffer is null")} : Result<usize>{total};
        }
        auto result = read(fd, std::span<std::byte>{reinterpret_cast<std::byte *>(vector.base), vector.length});
        if (!result)
        {
            return total == 0 ? result : Result<usize>{total};
        }
        total += result.value();
        if (result.value() != vector.length)
        {
            break;
        }
    }
    return total;
}

Result<usize> PosixService::writev(Fd fd, std::span<const IoVector> vectors)
{
    usize total = 0;
    for (const auto &vector : vectors)
    {
        if (vector.base == 0 && vector.length != 0)
        {
            return total == 0 ? Result<usize>{Status::invalid_argument("writev buffer is null")} : Result<usize>{total};
        }
        auto result = write(fd, std::span<const std::byte>{reinterpret_cast<const std::byte *>(vector.base),
                                                           vector.length});
        if (!result)
        {
            return total == 0 ? result : Result<usize>{total};
        }
        total += result.value();
    }
    return total;
}

Result<usize> PosixService::seek(Fd fd, i64 offset, SeekWhence whence)
{
    auto *entry = descriptor(fd);
    if (entry == nullptr || !entry->used || entry->console)
    {
        return Status::invalid_argument("file descriptor is not seekable");
    }
    const auto metadata = entry->node == nullptr ? fs::Metadata{} : entry->node->metadata();
    i64 base = 0;
    switch (whence)
    {
    case SeekWhence::set:
        base = 0;
        break;
    case SeekWhence::current:
        base = static_cast<i64>(entry->offset);
        break;
    case SeekWhence::end:
        base = static_cast<i64>(metadata.size);
        break;
    }
    const i64 next = base + offset;
    if (next < 0)
    {
        return Status::invalid_argument("negative seek offset");
    }
    entry->offset = static_cast<usize>(next);
    return entry->offset;
}

Result<i64> PosixService::fcntl(Fd fd, u32 command, u64 argument)
{
    auto *entry = descriptor(fd);
    if (entry == nullptr || !entry->used)
    {
        return Status::invalid_argument("bad file descriptor");
    }
    switch (command)
    {
    case f_DUPFD:
    {
        auto duplicated = duplicate(fd, static_cast<Fd>(argument));
        return duplicated ? Result<i64>{static_cast<i64>(duplicated.value())} : Result<i64>{duplicated.status()};
    }
    case f_GETFD:
        return entry->close_on_exec ? fd_CLOEXEC : 0;
    case f_SETFD:
        entry->close_on_exec = (argument & fd_CLOEXEC) != 0;
        return 0;
    case f_GETFL:
        return static_cast<i64>(entry->flags);
    case f_SETFL:
        entry->flags = (entry->flags & ~o_APPEND) | (static_cast<u32>(argument) & o_APPEND);
        return 0;
    default:
        return Status::unsupported("fcntl command is not implemented");
    }
}

Result<i64> PosixService::ioctl(Fd fd, u64, u64)
{
    auto *entry = descriptor(fd);
    if (entry == nullptr || !entry->used)
    {
        return Status::invalid_argument("bad file descriptor");
    }
    return Status::unsupported("ioctl request is not implemented for this descriptor");
}

Result<FileStatus> PosixService::fstat(Fd fd) const
{
    auto *entry = descriptor(fd);
    if (entry == nullptr || !entry->used)
    {
        return Status::invalid_argument("bad file descriptor");
    }
    if (entry->console || fd <= 2)
    {
        return FileStatus{
            .type = fs::NodeType::device,
            .size = 0,
            .mode = fs::mode_for(fs::NodeType::device, 0600u),
            .uid = fs::default_uid,
            .gid = fs::default_gid,
            .link_count = 1,
            .block_size = fs::metadata_block_size,
            .blocks = 0,
        };
    }
    if (entry->node == nullptr)
    {
        return Status::invalid_argument("file descriptor has no backing node");
    }
    const auto metadata = entry->node->metadata();
    return FileStatus{
        .type = metadata.type,
        .size = metadata.size,
        .mode = metadata.mode,
        .uid = metadata.uid,
        .gid = metadata.gid,
        .link_count = metadata.link_count,
        .block_size = metadata.block_size,
        .blocks = metadata.blocks,
    };
}

} // namespace ok::posix
