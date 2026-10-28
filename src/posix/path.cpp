#include "ok/posix/posix.hpp"
#include "ok/fs/mode_policy.hpp"

namespace ok::posix
{
namespace
{

inline constexpr u32 at_REMOVEDIR = 0x0200;

FileStatus file_status(const fs::Metadata &metadata)
{
    return FileStatus{
        .type = metadata.type,
        .size = metadata.size,
        .mode = metadata.mode,
        .uid = metadata.uid,
        .gid = metadata.gid,
        .link_count = metadata.link_count,
        .block_size = metadata.block_size,
        .blocks = metadata.blocks,
        .attributes = metadata.attributes,
    };
}

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
    case fs::NodeType::socket:
        return 12;
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

Status PosixService::mkdir(std::string_view path, u32 mode)
{
    return mkdirat(at_FDCWD, path, mode);
}

Status PosixService::mkdirat(Fd dirfd, std::string_view path, u32 mode)
{
    auto context = path_context(dirfd, path);
    if (!context)
        return context.status();
    return vfs_->create(context.value(), path, fs::NodeType::directory,
                        mode & ~resources().file_mode_mask, euid(), egid());
}

Status PosixService::symlinkat(std::string_view target, Fd dirfd, std::string_view path)
{
    auto context = path_context(dirfd, path);
    if (!context)
        return context.status();
    return vfs_->create(context.value(), path, fs::NodeType::symlink, 0777u, euid(), egid(), target);
}

Result<usize> PosixService::readlinkat(Fd dirfd, std::string_view path, std::span<std::byte> out)
{
    if (out.empty())
        return Status::invalid_argument("empty readlink buffer");
    auto resolved = resolve_node(dirfd, path, fs::FinalComponent::no_follow);
    if (!resolved)
        return resolved.status();
    auto metadata = resolved.value().node()->query_metadata();
    if (!metadata)
        return metadata.status();
    if (metadata.value().type != fs::NodeType::symlink)
        return Status::invalid_argument("path is not a symlink");
    auto data = resolved.value().node()->read(0, out.size());
    if (!data)
        return data.status();
    if (data.value().size > out.size())
        return Status::io_error("readlink provider exceeded output capacity");
    if (auto status = resolved.value().release(); !status.ok())
        return status;
    for (usize i = 0; i < data.value().size; ++i)
        out[i] = data.value().data[i];
    return data.value().size;
}

Status PosixService::link(std::string_view oldpath, std::string_view newpath)
{
    return linkat(at_FDCWD, oldpath, at_FDCWD, newpath);
}

Status PosixService::linkat(Fd olddirfd, std::string_view oldpath, Fd newdirfd, std::string_view newpath, u32 flags)
{
    if (flags)
        return Status::invalid_argument("linkat flags are unsupported");
    auto source = path_context(olddirfd, oldpath);
    if (!source)
        return source.status();
    auto target = path_context(newdirfd, newpath);
    if (!target)
        return target.status();
    return vfs_->link(source.value(), oldpath, target.value(), newpath);
}

Status PosixService::rename(std::string_view oldpath, std::string_view newpath)
{
    return renameat(at_FDCWD, oldpath, at_FDCWD, newpath);
}

Status PosixService::renameat(Fd olddirfd, std::string_view oldpath, Fd newdirfd, std::string_view newpath)
{
    auto source = path_context(olddirfd, oldpath);
    if (!source)
        return source.status();
    auto target = path_context(newdirfd, newpath);
    if (!target)
        return target.status();
    return vfs_->rename(source.value(), oldpath, target.value(), newpath);
}

Status PosixService::unlink(std::string_view path)
{
    return unlinkat(at_FDCWD, path);
}

Status PosixService::unlinkat(Fd dirfd, std::string_view path, u32 flags)
{
    if (flags & ~at_REMOVEDIR)
        return Status::invalid_argument("unsupported unlinkat flags");
    auto context = path_context(dirfd, path);
    if (!context)
        return context.status();
    return vfs_->remove(context.value(), path, (flags & at_REMOVEDIR) != 0);
}

Status PosixService::rmdir(std::string_view path)
{
    return unlinkat(at_FDCWD, path, at_REMOVEDIR);
}

Status PosixService::chdir(std::string_view path)
{
    auto resolved = resolve_node(at_FDCWD, path);
    if (!resolved)
        return resolved.status();
    auto *node = resolved.value().node();
    auto metadata = node->query_metadata();
    if (!metadata)
        return metadata.status();
    if (metadata.value().type != fs::NodeType::directory)
        return Status::not_directory("path is not a directory");
    if (auto status = fs::require_access(metadata.value(), credentials(), fs::access_execute); !status.ok())
        return status;
    if (auto status = node->retain(); !status.ok())
        return status;
    const auto mount = resolved.value().handle().mount_id;
    if (auto status = resolved.value().release(); !status.ok())
    {
        static_cast<void>(node->release());
        return status;
    }
    auto &owned = resources();
    auto *previous = owned.cwd_node;
    owned.cwd_node = node;
    owned.cwd_mount_id = mount;
    owned.cwd.clear();
    // Old cleanup consumes its pin even on failure; the new cwd already owns
    // its independent pin and cannot retain a dangling previous reference.
    return previous ? previous->release() : Status::success();
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
    auto metadata = entry->node->query_metadata();
    if (!metadata)
        return metadata.status();
    if (metadata.value().type != fs::NodeType::directory)
        return Status::not_directory("file descriptor is not a directory");
    if (auto status = fs::require_access(metadata.value(), credentials(), fs::access_execute); !status.ok())
    {
        return status;
    }
    if (auto status = entry->node->retain(); !status.ok())
        return status;
    auto &owned = resources();
    auto *previous = owned.cwd_node;
    owned.cwd_node = entry->node;
    owned.cwd_mount_id = entry->mount_id;
    // A detached directory remains an object reference; getcwd may fail.
    owned.cwd.clear();
    return previous ? previous->release() : Status::success();
}

Status PosixService::access(std::string_view path, u32 mode)
{
    return faccessat(at_FDCWD, path, mode);
}

Status PosixService::faccessat(Fd dirfd, std::string_view path, u32 mode, u32 flags)
{
    if (!initialized_ || !vfs_)
        return Status::not_initialized("POSIX service not initialized");
    if ((mode & ~(r_OK | w_OK | x_OK)) != 0)
        return Status::invalid_argument("invalid access mode");
    if (flags & ~(at_EACCESS | at_SYMLINK_NOFOLLOW | at_EMPTY_PATH))
        return Status::invalid_argument("unsupported access flags");
    const auto ids = user_credentials();
    const fs::Credentials selected = flags & at_EACCESS ? fs::Credentials{ids.euid, ids.egid}
                                                       : fs::Credentials{ids.uid, ids.gid};
    const auto permitted = [&](const fs::Metadata &metadata) {
        // Root can bypass DAC, but a regular file still needs an execute bit.
        if (!selected.uid && metadata.type == fs::NodeType::regular &&
            (mode & x_OK) && !(metadata.mode & 0111u))
            return Status::denied("regular file has no execute permission");
        return fs::require_access(metadata, selected, mode);
    };
    if (path.empty() && (flags & at_EMPTY_PATH))
    {
        if (dirfd != at_FDCWD)
        {
            const auto *entry = descriptor(dirfd);
            if (!entry || !entry->used)
                return Status::bad_descriptor("bad access descriptor");
            // Special descriptions do not yet carry qualified owner metadata.
            if (entry->pipe != sched::invalid_description || entry->native_udp_handle ||
                entry->console || entry->input_console)
                return mode ? Status::unsupported("special descriptor access metadata is unqualified")
                            : Status::success();
            auto status = fstat(dirfd);
            if (!status) return status.status();
            const auto &metadata = status.value();
            return permitted(fs::Metadata{.type = metadata.type, .mode = metadata.mode,
                                           .uid = metadata.uid, .gid = metadata.gid});
        }
        if (auto *cwd = resources().cwd_node)
        {
            auto metadata = vfs_->query_metadata(*cwd);
            return metadata ? permitted(metadata.value()) : metadata.status();
        }
        path = "/";
    }
    auto context = path_context(dirfd, path);
    if (!context) return context.status();
    // Path search and final DAC must observe the same captured ID pair.
    context.value().credentials = selected;
    auto resolved = vfs_->resolve(context.value(), path, flags & at_SYMLINK_NOFOLLOW
        ? fs::FinalComponent::no_follow : fs::FinalComponent::follow);
    if (!resolved) return resolved.status();
    auto metadata = vfs_->query_metadata(*resolved.value().node());
    const auto released = resolved.value().release();
    if (!metadata) return metadata.status();
    if (!released) return released;
    return permitted(metadata.value());
}

Status PosixService::chmod(std::string_view path, u32 mode)
{
    auto resolved = resolve_node(at_FDCWD, path);
    if (!resolved)
        return resolved.status();
    auto *node = resolved.value().node();
    auto metadata = node->query_metadata();
    if (!metadata)
        return metadata.status();
    const auto decision = fs::mode_change_policy(metadata.value().uid, metadata.value().gid,
                                                  mode, user_credentials());
    if (!decision.authorized)
        return Status::permission("only the owner can chmod a filesystem node");
    if (auto status = node->chmod(decision.permissions); !status.ok())
        return status;
    return resolved.value().release();
}

fs::ModeChangeResult PosixService::change_mode_fd(Fd fd, u32 mode)
{
    if (!initialized_ || !vfs_)
        return fs::ModeChangeResult::failure(Status::not_initialized("POSIX service not initialized"));
    auto *entry = descriptor(fd);
    if (!entry || !entry->used)
        return fs::ModeChangeResult::failure(Status::bad_descriptor("bad mode-change descriptor"));
    if (!entry->node)
        return fs::ModeChangeResult::unavailable();
    // The open description owns this node throughout the serialized operation.
    return entry->node->change_mode(mode, user_credentials());
}

fs::ModeChangeResult PosixService::change_mode_at(Fd dirfd, std::string_view path, u32 mode, u32 flags)
{
    if (!initialized_ || !vfs_)
        return fs::ModeChangeResult::failure(Status::not_initialized("POSIX service not initialized"));
    if (flags & ~(at_EMPTY_PATH | at_SYMLINK_NOFOLLOW))
        return fs::ModeChangeResult::failure(Status::invalid_argument("unsupported mode-change flags"));
    if (path.empty() && (flags & at_EMPTY_PATH))
    {
        if (dirfd != at_FDCWD)
            return change_mode_fd(dirfd, mode);
        // A held cwd remains the target after rename or namespace detachment.
        if (auto *cwd = resources().cwd_node)
            return cwd->change_mode(mode, user_credentials());
        path = "/";
    }
    auto resolved = resolve_node(dirfd, path, (flags & at_SYMLINK_NOFOLLOW) ? fs::FinalComponent::no_follow
                                                                         : fs::FinalComponent::follow);
    if (!resolved)
        return resolved.status().code() == StatusCode::unsupported ? fs::ModeChangeResult::unavailable()
                                                                   : fs::ModeChangeResult::failure(resolved.status());
    const auto result = resolved.value().node()->change_mode(mode, user_credentials());
    const auto released = resolved.value().release();
    if (result.ok() && !released)
        return fs::ModeChangeResult::failure(released);
    return result;
}

fs::ModeChangeResult PosixService::change_owner_fd(Fd fd, u32 uid, u32 gid)
{
    if (!initialized_ || !vfs_)
        return fs::ModeChangeResult::failure(Status::not_initialized("POSIX service not initialized"));
    auto *entry = descriptor(fd);
    if (!entry || !entry->used)
        return fs::ModeChangeResult::failure(Status::bad_descriptor("bad ownership-change descriptor"));
    if (euid() != user::root_uid)
        return fs::ModeChangeResult::failure(Status::permission("ownership change requires root"));
    if (!entry->node) return fs::ModeChangeResult::unavailable();
    return entry->node->change_owner(uid, gid, user_credentials());
}

fs::ModeChangeResult PosixService::change_owner_at(Fd dirfd, std::string_view path, u32 uid, u32 gid, u32 flags)
{
    if (!initialized_ || !vfs_)
        return fs::ModeChangeResult::failure(Status::not_initialized("POSIX service not initialized"));
    if (flags & ~(at_EMPTY_PATH | at_SYMLINK_NOFOLLOW))
        return fs::ModeChangeResult::failure(Status::invalid_argument("unsupported ownership-change flags"));
    if (path.empty() && (flags & at_EMPTY_PATH))
    {
        if (dirfd != at_FDCWD) return change_owner_fd(dirfd, uid, gid);
        if (auto *cwd = resources().cwd_node)
        {
            if (euid() != user::root_uid)
                return fs::ModeChangeResult::failure(Status::permission("ownership change requires root"));
            return cwd->change_owner(uid, gid, user_credentials());
        }
        path = "/";
    }
    auto resolved = resolve_node(dirfd, path, (flags & at_SYMLINK_NOFOLLOW) ? fs::FinalComponent::no_follow
                                                                         : fs::FinalComponent::follow);
    if (!resolved)
        return resolved.status().code() == StatusCode::unsupported ? fs::ModeChangeResult::unavailable()
                                                                   : fs::ModeChangeResult::failure(resolved.status());
    const auto result = euid() != user::root_uid ?
        fs::ModeChangeResult::failure(Status::permission("ownership change requires root")) :
        resolved.value().node()->change_owner(uid, gid, user_credentials());
    const auto released = resolved.value().release();
    if (result.ok() && !released) return fs::ModeChangeResult::failure(released);
    return result;
}

Status PosixService::utimensat(Fd dirfd, std::string_view path, fs::TimestampChange access,
                               fs::TimestampChange modification, u32 flags)
{
    if (!initialized_ || !vfs_)
        return Status::not_initialized("POSIX service not initialized");
    if (flags & ~(at_SYMLINK_NOFOLLOW | at_EMPTY_PATH))
        return Status::invalid_argument("unsupported timestamp-change flags");
    using Kind = fs::TimestampChange::Kind;
    if (access.kind == Kind::omit && modification.kind == Kind::omit)
        return Status::success();
    const auto apply = [&](fs::Node &node) -> Status {
        auto metadata = node.query_metadata();
        if (!metadata) return metadata.status();
        const bool owner = euid() == user::root_uid || euid() == metadata.value().uid;
        if (!owner)
        {
            if (access.kind == Kind::value || modification.kind == Kind::value)
                return Status::permission("explicit timestamps require the owner");
            if (auto status = fs::require_access(metadata.value(), {euid(), egid()}, fs::access_write); !status)
                return status;
        }
        return node.set_times(access, modification);
    };
    if (path.empty())
    {
        auto *description = descriptor(dirfd);
        if (!description || !description->used || !description->node)
            return Status::bad_descriptor("timestamp change needs an open file");
        return apply(*description->node);
    }
    auto resolved = resolve_node(dirfd, path, (flags & at_SYMLINK_NOFOLLOW) ? fs::FinalComponent::no_follow
                                                                             : fs::FinalComponent::follow);
    if (!resolved)
        return resolved.status();
    const auto status = apply(*resolved.value().node());
    const auto released = resolved.value().release();
    return status ? released : status;
}

Result<fs::FilesystemStatistics> PosixService::statfs(Fd dirfd, std::string_view path)
{
    if (!initialized_ || !vfs_)
        return Status::not_initialized("POSIX service not initialized");
    if (path.empty())
    {
        auto *description = descriptor(dirfd);
        if (!description || !description->used || !description->node)
            return Status::bad_descriptor("statfs needs an open file");
        return description->node->filesystem_statistics();
    }
    auto resolved = resolve_node(dirfd, path, fs::FinalComponent::follow);
    if (!resolved)
        return resolved.status();
    auto statistics = resolved.value().node()->filesystem_statistics();
    const auto released = resolved.value().release();
    if (statistics && !released) return released;
    return statistics;
}

Status PosixService::chown(std::string_view path, u32 uid, u32 gid)
{
    if (euid() != 0)
        return Status::denied("only root can chown a filesystem node");
    auto resolved = resolve_node(at_FDCWD, path);
    if (!resolved)
        return resolved.status();
    if (auto status = resolved.value().node()->chown(uid, gid); !status.ok())
        return status;
    return resolved.value().release();
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
    if (flags & ~(at_EMPTY_PATH | at_SYMLINK_NOFOLLOW))
        return Status::invalid_argument("unsupported statat flags");
    if (path.empty() && (flags & at_EMPTY_PATH) != 0)
    {
        if (dirfd != at_FDCWD)
            return fstat(dirfd);
        if (auto *cwd = resources().cwd_node)
        {
            // ProcessResources owns this identity in the serialized caller.
            // Empty-path status requires no search or reconstructed pathname.
            auto queried = vfs_->query_metadata(*cwd);
            if (!queried)
                return queried.status();
            return file_status(queried.value());
        }
        // A process without an explicit cwd pin still uses the namespace root.
        path = "/";
    }
    auto resolved = resolve_node(dirfd, path, (flags & at_SYMLINK_NOFOLLOW) ? fs::FinalComponent::no_follow
                                                                         : fs::FinalComponent::follow);
    if (!resolved)
        return resolved.status();
    auto queried = vfs_->query_metadata(*resolved.value().node());
    if (!queried)
        return queried.status();
    const auto metadata = queried.value();
    if (auto status = resolved.value().release(); !status.ok())
        return status;
    return file_status(metadata);
}

Result<usize> PosixService::getdents64(Fd fd, std::span<std::byte> out)
{
    usize written = 0;
    return enumerate_directory<DirectoryPolicy::native>(fd,
        [&](const fs::DirectoryEntry &source, u64 cookie) -> Result<usize> {
            const auto name = source.name.view();
            const usize reclen = align_up(19 + name.size() + 1, 8);
            if (reclen > out.size() - written)
                return Status::overflow("getdents64 buffer too small");
            write_native(out, written, cookie);
            write_native(out, written + 8, static_cast<i64>(cookie));
            write_native(out, written + 16, static_cast<u16>(reclen));
            out[written + 18] = static_cast<std::byte>(dirent_type(source.metadata.type));
            for (usize i = 0; i < name.size(); ++i)
                out[written + 19 + i] = static_cast<std::byte>(name[i]);
            for (usize i = 19 + name.size(); i < reclen; ++i)
                out[written + i] = std::byte{0};
            written += reclen;
            return reclen;
        });
}

} // namespace ok::posix
