#include "ok/posix/posix.hpp"

namespace ok::posix
{
namespace
{

inline constexpr u32 at_REMOVEDIR = 0x0200;

usize align_up(usize value, usize alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

u8 dirent_type(fs::NodeType type)
{
    switch (type)
    {
    case fs::NodeType::directory:
        return 4;
    case fs::NodeType::regular:
        return 8;
    case fs::NodeType::device:
        return 2;
    case fs::NodeType::symlink:
        return 10;
    }
    return 0;
}

void write_u16(std::span<std::byte> out, usize offset, u16 value)
{
    out[offset] = static_cast<std::byte>(value & 0xffu);
    out[offset + 1] = static_cast<std::byte>((value >> 8) & 0xffu);
}

void write_u64(std::span<std::byte> out, usize offset, u64 value)
{
    for (usize i = 0; i < sizeof(value); ++i)
    {
        out[offset + i] = static_cast<std::byte>((value >> (i * 8)) & 0xffu);
    }
}

void write_i64(std::span<std::byte> out, usize offset, i64 value)
{
    write_u64(out, offset, static_cast<u64>(value));
}

} // namespace

Status PosixService::require_parent_directory_access(std::string_view normalized_path, u32 access) const
{
    if (vfs_ == nullptr)
    {
        return Status::not_initialized("POSIX service not initialized");
    }
    if (normalized_path.empty() || normalized_path.front() != '/')
    {
        return Status::invalid_argument("path is not normalized");
    }
    if (normalized_path == "/")
    {
        return Status::invalid_argument("root has no parent directory");
    }
    usize slash = 0;
    for (usize i = 0; i < normalized_path.size(); ++i)
    {
        if (normalized_path[i] == '/')
        {
            slash = i;
        }
    }
    FixedString<96> parent;
    if (slash == 0)
    {
        if (auto status = parent.assign("/"); !status.ok())
        {
            return status;
        }
    }
    else if (auto status = parent.assign(normalized_path.substr(0, slash)); !status.ok())
    {
        return status;
    }
    auto metadata = vfs_->stat(parent.view());
    if (!metadata)
    {
        return metadata.status();
    }
    if (metadata.value().type != fs::NodeType::directory)
    {
        return Status::not_found("parent directory not found");
    }
    return fs::require_access(metadata.value(), credentials(), access);
}

Status PosixService::mkdir(std::string_view path, u32 mode)
{
    return mkdirat(at_FDCWD, path, mode);
}

Status PosixService::mkdirat(Fd dirfd, std::string_view path, u32 mode)
{
    if (!initialized_ || vfs_ == nullptr)
    {
        return Status::not_initialized("POSIX service not initialized");
    }
    auto normalized = resolve_path(dirfd, path);
    if (!normalized)
    {
        return normalized.status();
    }
    if (auto status = require_parent_directory_access(normalized.value(), fs::access_write | fs::access_execute);
        !status.ok())
    {
        return status;
    }
    if (auto status = vfs_->create(normalized.value(), fs::NodeType::directory); !status.ok())
    {
        return status;
    }
    const auto directory_mode = mode & ~file_mode_mask_;
    if (auto status = vfs_->chmod(normalized.value(), directory_mode); !status.ok())
    {
        return status;
    }
    return vfs_->chown(normalized.value(), current_euid_, current_egid_);
}

Status PosixService::unlink(std::string_view path)
{
    return unlinkat(at_FDCWD, path);
}

Status PosixService::unlinkat(Fd dirfd, std::string_view path, u32 flags)
{
    if ((flags & at_REMOVEDIR) != 0)
    {
        auto normalized = resolve_path(dirfd, path);
        return normalized ? rmdir(normalized.value()) : normalized.status();
    }
    if (!initialized_ || vfs_ == nullptr)
    {
        return Status::not_initialized("POSIX service not initialized");
    }
    auto normalized = resolve_path(dirfd, path);
    if (!normalized)
    {
        return normalized.status();
    }
    if (auto status = require_parent_directory_access(normalized.value(), fs::access_write | fs::access_execute);
        !status.ok())
    {
        return status;
    }
    return vfs_->unlink(normalized.value());
}

Status PosixService::rmdir(std::string_view path)
{
    if (!initialized_ || vfs_ == nullptr)
    {
        return Status::not_initialized("POSIX service not initialized");
    }
    auto normalized = normalize_path(path);
    if (!normalized)
    {
        return normalized.status();
    }
    if (auto status = require_parent_directory_access(normalized.value(), fs::access_write | fs::access_execute);
        !status.ok())
    {
        return status;
    }
    return vfs_->rmdir(normalized.value());
}

Status PosixService::chdir(std::string_view path)
{
    if (!initialized_ || vfs_ == nullptr)
    {
        return Status::not_initialized("POSIX service not initialized");
    }
    auto normalized = normalize_path(path);
    if (!normalized)
    {
        return normalized.status();
    }
    auto *node = vfs_->lookup(normalized.value());
    if (node == nullptr || node->metadata().type != fs::NodeType::directory)
    {
        return Status::not_found("directory not found");
    }
    if (auto status = fs::require_access(node->metadata(), credentials(), fs::access_execute); !status.ok())
    {
        return status;
    }
    return cwd_.assign(normalized.value());
}

Status PosixService::fchdir(Fd fd)
{
    auto *entry = descriptor(fd);
    if (entry == nullptr || !entry->used || entry->node == nullptr ||
        entry->node->metadata().type != fs::NodeType::directory)
    {
        return Status::invalid_argument("file descriptor is not a directory");
    }
    if (auto status = fs::require_access(entry->node->metadata(), credentials(), fs::access_execute); !status.ok())
    {
        return status;
    }
    return cwd_.assign(entry->path.view());
}

Status PosixService::access(std::string_view path, u32 mode)
{
    return faccessat(at_FDCWD, path, mode);
}

Status PosixService::faccessat(Fd dirfd, std::string_view path, u32 mode, u32)
{
    if ((mode & ~(r_OK | w_OK | x_OK)) != 0)
    {
        return Status::invalid_argument("invalid access mode");
    }
    auto metadata = statat(dirfd, path);
    if (!metadata)
    {
        return metadata.status();
    }
    return fs::require_access(
        fs::Metadata{
            .type = metadata.value().type,
            .size = metadata.value().size,
            .mode = metadata.value().mode,
            .uid = metadata.value().uid,
            .gid = metadata.value().gid,
            .link_count = metadata.value().link_count,
            .block_size = metadata.value().block_size,
            .blocks = metadata.value().blocks,
        },
        credentials(), mode);
}

Status PosixService::chmod(std::string_view path, u32 mode)
{
    if (!initialized_ || vfs_ == nullptr)
    {
        return Status::not_initialized("POSIX service not initialized");
    }
    auto normalized = normalize_path(path);
    if (!normalized)
    {
        return normalized.status();
    }
    auto metadata = vfs_->stat(normalized.value());
    if (!metadata)
    {
        return metadata.status();
    }
    if (current_euid_ != 0 && current_euid_ != metadata.value().uid)
    {
        return Status::denied("only the owner can chmod a filesystem node");
    }
    return vfs_->chmod(normalized.value(), mode);
}

Status PosixService::chown(std::string_view path, u32 uid, u32 gid)
{
    if (!initialized_ || vfs_ == nullptr)
    {
        return Status::not_initialized("POSIX service not initialized");
    }
    if (current_euid_ != 0)
    {
        return Status::denied("only root can chown a filesystem node");
    }
    auto normalized = normalize_path(path);
    if (!normalized)
    {
        return normalized.status();
    }
    return vfs_->chown(normalized.value(), uid, gid);
}

Result<FileStatus> PosixService::stat(std::string_view path)
{
    return statat(at_FDCWD, path);
}

Result<FileStatus> PosixService::statat(Fd dirfd, std::string_view path, u32 flags)
{
    if (!initialized_ || vfs_ == nullptr)
    {
        return Status::not_initialized("POSIX service not initialized");
    }
    if (path.empty() && (flags & at_EMPTY_PATH) != 0)
    {
        return fstat(dirfd);
    }
    auto normalized = resolve_path(dirfd, path);
    if (!normalized)
    {
        return normalized.status();
    }
    auto metadata = vfs_->stat(normalized.value());
    if (!metadata)
    {
        return metadata.status();
    }
    return FileStatus{
        .type = metadata.value().type,
        .size = metadata.value().size,
        .mode = metadata.value().mode,
        .uid = metadata.value().uid,
        .gid = metadata.value().gid,
        .link_count = metadata.value().link_count,
        .block_size = metadata.value().block_size,
        .blocks = metadata.value().blocks,
    };
}

Result<usize> PosixService::getdents64(Fd fd, std::span<std::byte> out)
{
    auto *entry = descriptor(fd);
    if (entry == nullptr || !entry->used || entry->node == nullptr ||
        entry->node->metadata().type != fs::NodeType::directory)
    {
        return Status::invalid_argument("file descriptor is not a directory");
    }
    if (auto status = fs::require_access(entry->node->metadata(), credentials(), fs::access_read); !status.ok())
    {
        return status;
    }
    auto listing = vfs_->list(entry->path.view());
    if (!listing)
    {
        return listing.status();
    }

    usize written = 0;
    usize index = entry->offset;
    while (index < listing.value().count)
    {
        const auto &source = listing.value().entries[index];
        const auto name = source.name.view();
        const usize reclen = align_up(19 + name.size() + 1, 8);
        if (written + reclen > out.size())
        {
            break;
        }
        write_u64(out, written + 0, static_cast<u64>(index + 1));
        write_i64(out, written + 8, static_cast<i64>(index + 1));
        write_u16(out, written + 16, static_cast<u16>(reclen));
        out[written + 18] = static_cast<std::byte>(dirent_type(source.metadata.type));
        for (usize i = 0; i < name.size(); ++i)
        {
            out[written + 19 + i] = static_cast<std::byte>(name[i]);
        }
        out[written + 19 + name.size()] = static_cast<std::byte>(0);
        for (usize i = 20 + name.size(); i < reclen; ++i)
        {
            out[written + i] = static_cast<std::byte>(0);
        }
        written += reclen;
        ++index;
    }
    entry->offset = index;
    if (written == 0 && entry->offset < listing.value().count)
    {
        return Status::overflow("getdents64 buffer too small");
    }
    return written;
}

} // namespace ok::posix
