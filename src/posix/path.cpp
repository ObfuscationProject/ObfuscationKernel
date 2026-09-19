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

template <typename T> void write_native(std::span<std::byte> out, usize offset, T value)
{
    const auto *bytes = reinterpret_cast<const std::byte *>(&value);
    for (usize i = 0; i < sizeof(value); ++i)
        out[offset + i] = bytes[i];
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
    while (normalized_path.size() > 1 && normalized_path.back() == '/')
        normalized_path.remove_suffix(1);
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
        return Status::not_directory("parent is not a directory");
    }
    return fs::require_access(metadata.value(), credentials(), access);
}

Status PosixService::require_path_search(Fd dirfd, std::string_view path)
{
    FixedString<256> prefix;
    if (path.front() == '/')
        static_cast<void>(prefix.assign("/"));
    else if (dirfd == at_FDCWD)
    {
        auto cwd = current_directory();
        if (!cwd)
            return cwd.status();
        static_cast<void>(prefix.assign(cwd.value()));
    }
    else
    {
        auto *entry = descriptor(dirfd);
        if (!entry || !entry->node)
            return Status::bad_descriptor("bad directory file descriptor");
        auto base = vfs_->directory_path(*entry->node, entry->mount_id);
        if (!base)
            return base.status();
        static_cast<void>(prefix.assign(base.value().view()));
    }
    // Search starts at the supplied directory, not at its ancestors. An already
    // held cwd/dirfd remains usable if an ancestor later loses search permission.
    usize cursor = 0;
    while (cursor < path.size())
    {
        while (cursor < path.size() && path[cursor] == '/')
            ++cursor;
        if (cursor == path.size())
            break;
        auto parent = vfs_->stat(prefix.view());
        if (!parent)
            return parent.status();
        if (parent.value().type != fs::NodeType::directory)
            return Status::not_directory("path component is not a directory");
        if (auto status = fs::require_access(parent.value(), credentials(), fs::access_execute); !status.ok())
            return status;
        const auto begin = cursor;
        while (cursor < path.size() && path[cursor] != '/')
            ++cursor;
        if (prefix.view() != "/")
            if (auto status = prefix.append('/'); !status.ok())
                return status;
        if (auto status = prefix.append(path.substr(begin, cursor - begin)); !status.ok())
            return status;
    }
    return Status::success();
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
    const auto directory_mode = mode & ~resources().file_mode_mask;
    if (auto status = vfs_->chmod(normalized.value(), directory_mode); !status.ok())
    {
        return status;
    }
    return vfs_->chown(normalized.value(), euid(), egid());
}

Status PosixService::link(std::string_view oldpath, std::string_view newpath)
{
    return linkat(at_FDCWD, oldpath, at_FDCWD, newpath);
}

Status PosixService::linkat(Fd olddirfd, std::string_view oldpath, Fd newdirfd, std::string_view newpath, u32 flags)
{
    if (!initialized_ || !vfs_)
        return Status::not_initialized("POSIX service not initialized");
    if (flags)
        return Status::invalid_argument("linkat flags are unsupported");
    if (oldpath.empty() || newpath.empty())
        return Status::not_found("empty link path");
    if (auto status = require_path_search(olddirfd, oldpath); !status.ok())
        return status;
    if (auto status = require_path_search(newdirfd, newpath); !status.ok())
        return status;
    auto old_resolved = resolve_path(olddirfd, oldpath);
    if (!old_resolved)
        return old_resolved.status();
    // resolve_path uses one scratch buffer. Preserve the first path before
    // resolving a relative destination, including when both dirfds differ.
    FixedString<256> source;
    if (auto status = source.assign(old_resolved.value()); !status.ok())
        return status;
    auto new_resolved = resolve_path(newdirfd, newpath);
    if (!new_resolved)
        return new_resolved.status();
    if (auto status = require_parent_directory_access(source.view(), fs::access_execute); !status.ok())
        return status;
    if (auto status = require_parent_directory_access(new_resolved.value(), fs::access_write | fs::access_execute);
        !status.ok())
        return status;
    auto *node = vfs_->lookup(source.view());
    if (!node)
        return Status::not_found("link source not found");
    if (node->metadata().type == fs::NodeType::directory)
        return Status::permission("directory hard links are forbidden");
    return vfs_->link(source.view(), new_resolved.value());
}

Status PosixService::rename(std::string_view oldpath, std::string_view newpath)
{
    return renameat(at_FDCWD, oldpath, at_FDCWD, newpath);
}

Status PosixService::renameat(Fd olddirfd, std::string_view oldpath, Fd newdirfd, std::string_view newpath)
{
    if (!initialized_ || !vfs_)
        return Status::not_initialized("POSIX service not initialized");
    if (oldpath.empty() || newpath.empty())
        return Status::not_found("empty rename path");
    if (auto status = require_path_search(olddirfd, oldpath); !status.ok())
        return status;
    if (auto status = require_path_search(newdirfd, newpath); !status.ok())
        return status;
    auto old_resolved = resolve_path(olddirfd, oldpath);
    if (!old_resolved)
        return old_resolved.status();
    FixedString<256> source;
    if (auto status = source.assign(old_resolved.value()); !status.ok())
        return status;
    auto new_resolved = resolve_path(newdirfd, newpath);
    if (!new_resolved)
        return new_resolved.status();
    if (auto status = require_parent_directory_access(source.view(), fs::access_write | fs::access_execute);
        !status.ok())
        return status;
    if (auto status = require_parent_directory_access(new_resolved.value(), fs::access_write | fs::access_execute);
        !status.ok())
        return status;
    const auto sticky_access = [&](std::string_view path) -> Status {
        while (path.size() > 1 && path.back() == '/')
            path.remove_suffix(1);
        auto *node = vfs_->lookup(path);
        if (!node || euid() == 0)
            return Status::success();
        const auto slash = path.find_last_of('/');
        auto *parent = vfs_->lookup(slash == 0 ? "/" : path.substr(0, slash));
        if (!parent)
            return Status::not_found("rename parent not found");
        const auto metadata = parent->metadata();
        if ((metadata.mode & 01000u) && euid() != metadata.uid && euid() != node->metadata().uid)
            return Status::permission("sticky directory protects another owner's file");
        return Status::success();
    };
    if (auto status = sticky_access(source.view()); !status.ok())
        return status;
    if (auto status = sticky_access(new_resolved.value()); !status.ok())
        return status;
    return vfs_->rename(source.view(), new_resolved.value());
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
    usize mount_id = 0;
    auto *node = vfs_->lookup(normalized.value(), &mount_id);
    if (node == nullptr || node->metadata().type != fs::NodeType::directory)
    {
        return Status::not_found("directory not found");
    }
    if (auto status = fs::require_access(node->metadata(), credentials(), fs::access_execute); !status.ok())
    {
        return status;
    }
    auto canonical = vfs_->directory_path(*node, mount_id);
    if (!canonical)
        return canonical.status();
    if (auto status = node->retain(); !status.ok())
        return status;
    auto &owned = resources();
    if (owned.cwd_node)
        static_cast<void>(owned.cwd_node->release());
    owned.cwd_node = node;
    owned.cwd_mount_id = mount_id;
    return owned.cwd.assign(canonical.value().view());
}

Result<std::string_view> PosixService::current_directory()
{
    auto &owned = resources();
    if (!owned.cwd_node)
        return owned.cwd.view();
    auto path = vfs_->directory_path(*owned.cwd_node, owned.cwd_mount_id);
    if (!path)
        return path.status();
    if (auto status = owned.cwd.assign(path.value().view()); !status.ok())
        return status;
    return owned.cwd.view();
}

Status PosixService::fchdir(Fd fd)
{
    auto *entry = descriptor(fd);
    if (entry == nullptr || !entry->used || entry->node == nullptr)
        return Status::bad_descriptor("bad directory file descriptor");
    if (entry->node->metadata().type != fs::NodeType::directory)
        return Status::not_directory("file descriptor is not a directory");
    if (auto status = fs::require_access(entry->node->metadata(), credentials(), fs::access_execute); !status.ok())
    {
        return status;
    }
    if (auto status = entry->node->retain(); !status.ok())
        return status;
    auto &owned = resources();
    if (owned.cwd_node)
        static_cast<void>(owned.cwd_node->release());
    owned.cwd_node = entry->node;
    owned.cwd_mount_id = entry->mount_id;
    // A detached directory is still a valid descriptor, but getcwd and relative
    // operations must fail rather than resolve through a replacement pathname.
    owned.cwd.clear();
    return Status::success();
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
    if (euid() != 0 && euid() != metadata.value().uid)
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
    if (euid() != 0)
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
    auto listing = entry->node->list();
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
        write_native(out, written + 0, static_cast<u64>(index + 1));
        write_native(out, written + 8, static_cast<i64>(index + 1));
        write_native(out, written + 16, static_cast<u16>(reclen));
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
