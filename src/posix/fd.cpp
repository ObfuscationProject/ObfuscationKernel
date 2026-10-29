#include "ok/posix/posix.hpp"
#include "ok/user/native_shared_files.hpp"

namespace ok::posix
{
namespace
{

// The serialized caller pins a description before any creation/truncation.
// Publication transfers its lifetime to a descriptor; every failure drops it.
class PendingDescription final : private NonCopyable
{
  public:
    PendingDescription(sched::SchedulerState &owner, u16 id) : owner_(&owner), id_(id) {}
    ~PendingDescription() { static_cast<void>(owner_->release_description(id_)); }
  private:
    sched::SchedulerState *owner_;
    u16 id_;
};

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
    return openat_limited(dirfd, path, flags, mode, ~u64{0});
}

Result<Fd> PosixService::openat_limited(Fd dirfd, std::string_view path, u32 flags, u32 mode, u64 max_regular_size)
{
    if (!initialized_ || vfs_ == nullptr)
    {
        return Status::not_initialized("POSIX service not initialized");
    }
    constexpr u32 allowed = o_WRONLY | o_RDWR | o_APPEND | o_NONBLOCK | o_CREAT | o_EXCL |
                            o_NOFOLLOW | o_TRUNC | o_CLOEXEC | o_DIRECTORY;
    if ((flags & ~allowed) != 0 || (flags & 3u) == 3u ||
        (flags & (o_CREAT | o_DIRECTORY)) == (o_CREAT | o_DIRECTORY))
        return Status::invalid_argument("unsupported open flags or access mode");
    auto context = path_context(dirfd, path);
    if (!context)
        return context.status();
    auto fd = allocate_fd();
    if (!fd)
        return fd.status();
    auto description = scheduler_->allocate_description({});
    if (!description)
        return description.status();
    auto *reserved = scheduler_->open_description(description.value());
    if (auto status = scheduler_->retain_description(description.value()); !status.ok())
    {
        *reserved = {};
        return status;
    }
    PendingDescription pending(*scheduler_, description.value());
    bool created = false;
    const bool exclusive = (flags & (o_CREAT | o_EXCL)) == (o_CREAT | o_EXCL);
    const auto policy = (flags & o_NOFOLLOW) || exclusive ? fs::FinalComponent::no_follow : fs::FinalComponent::follow;
    auto open_node = [&]() -> Result<fs::PathReference> {
        auto existing = vfs_->resolve(context.value(), path, policy);
        if (existing)
        {
            if (exclusive)
                return Status::already_exists("exclusive open path exists");
            return existing;
        }
        if (existing.status().code() != StatusCode::not_found || !(flags & o_CREAT))
            return existing.status();
        auto fresh = vfs_->create_file(context.value(), path, mode & ~resources().file_mode_mask,
                                       euid(), egid(), policy == fs::FinalComponent::follow);
        if (fresh)
        {
            created = true;
            return fresh;
        }
        if (fresh.status().code() != StatusCode::already_exists || exclusive)
            return fresh.status();
        return vfs_->resolve(context.value(), path, policy);
    };
    auto resolved = open_node();
    if (!resolved)
        return resolved.status();
    auto *node = resolved.value().node();
    const auto mount_id = resolved.value().handle().mount_id;
    auto queried = node->query_metadata();
    if (!queried)
        return queried.status();
    auto metadata = queried.value();
    if (metadata.type == fs::NodeType::socket)
        return Status::unsupported("filesystem socket nodes require the socket interface");
    if (metadata.type == fs::NodeType::symlink)
        return Status::too_many_symlinks("open refuses the final symlink");
    if ((flags & o_DIRECTORY) != 0 && metadata.type != fs::NodeType::directory)
    {
        return Status::not_directory("path is not a directory");
    }
    if (metadata.type == fs::NodeType::directory && (is_writable(flags) || (flags & (o_CREAT | o_TRUNC))))
    {
        return Status::is_directory("directory cannot be opened for write/create/truncate");
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
    if (metadata.type == fs::NodeType::regular && metadata.size > max_regular_size)
        return Status::overflow("regular file exceeds selected open size policy");
    // Acquire the only node reference owned by this pending description before
    // truncation. The retained path independently protects namespace traversal.
    if (auto status = node->retain(); !status.ok())
        return status;
    reserved->node = node;
    if (!created && (flags & o_TRUNC) != 0)
    {
        if (metadata.type != fs::NodeType::regular && metadata.type != fs::NodeType::device)
        {
            return Status::invalid_argument("cannot truncate non-regular file");
        }
        if (auto status = node->truncate(0); !status.ok())
        {
            return status;
        }
    }

    // The reserved description keeps the node pinned. Finish fallible path
    // cleanup before publishing the fd; rollback consumes its remaining pin.
    if (auto status = resolved.value().release(); !status.ok())
        return status;

    FileDescriptor entry{};
    entry.references = 1;
    entry.used = true;
    entry.readable = is_readable(flags);
    entry.writable = is_writable(flags);
    entry.large_file = max_regular_size > 0x7fff'ffffu;
    entry.flags = flags & ~o_CLOEXEC;
    // Append changes each write's destination, never the initial read position.
    // write() obtains the current EOF immediately before every append write.
    entry.offset = 0;
    entry.node = node;
    entry.mount_id = mount_id;
    // Diagnostic only: identity is carried by the retained node and mount.
    static_cast<void>(entry.path.assign(path));
    *reserved = entry;
    if (auto status =
            scheduler_->install_descriptor(resources(), fd.value(), description.value(), (flags & o_CLOEXEC) != 0);
        !status.ok())
        return status;
    return fd.value();
}

Result<std::array<Fd, 2>> PosixService::pipe(u32 flags)
{
    if (scheduler_ == nullptr)
        return Status::not_initialized("POSIX service not initialized");
    if ((flags & ~(o_CLOEXEC | o_NONBLOCK)) != 0)
        return Status::invalid_argument("unsupported pipe flags");
    return scheduler_->create_pipe(resources(), (flags & o_CLOEXEC) != 0, flags & o_NONBLOCK);
}

Result<std::array<Fd, 2>> PosixService::unix_socketpair(u32 flags)
{
    if (scheduler_ == nullptr)
        return Status::not_initialized("POSIX service not initialized");
    if ((flags & ~(o_CLOEXEC | o_NONBLOCK)) != 0)
        return Status::invalid_argument("unsupported AF_UNIX socket flags");
    return scheduler_->create_unix_socketpair(resources(), (flags & o_CLOEXEC) != 0, flags & o_NONBLOCK);
}

Result<Fd> PosixService::unix_socket(u32 flags)
{
    if (!initialized_ || !scheduler_) return Status::not_initialized("UNIX socket owner is unavailable");
    if (flags & ~(o_CLOEXEC | o_NONBLOCK)) return Status::invalid_argument("unsupported UNIX socket flags");
    return scheduler_->create_unix_socket(resources(), (flags & o_CLOEXEC) != 0, flags & o_NONBLOCK);
}
Status PosixService::bind_unix_description(u16 description, std::string_view path)
{
    if (!initialized_ || !scheduler_ || !vfs_) return Status::not_initialized("UNIX binding owner is unavailable");
    if (auto admitted = scheduler_->validate_unix_bind(description); !admitted) return admitted;
    auto context = path_context(at_FDCWD, path);
    if (!context) return context.status();
    auto node = vfs_->create_socket(context.value(), path, 0777u & ~resources().file_mode_mask);
    if (!node) return node.status();
    // RAM creation has no provider callbacks. Admission above guarantees the
    // binding transfer performs no allocation or fallible post-publication work.
    return scheduler_->bind_unix_socket(description, std::move(node.value()));
}
Status PosixService::listen_unix_description(u16 description, usize backlog)
{
    if (!scheduler_) return Status::not_initialized("UNIX listener owner is unavailable");
    return scheduler_->listen_unix_socket(description, backlog, {getpid(), euid(), egid()});
}
Result<bool> PosixService::connect_unix_description(u16 description, std::string_view path)
{
    if (!initialized_ || !scheduler_ || !vfs_) return Status::not_initialized("UNIX connect owner is unavailable");
    auto context = path_context(at_FDCWD, path);
    if (!context) return context.status();
    auto node = vfs_->resolve_socket(context.value(), path);
    if (!node) return node.status();
    const auto connected = scheduler_->connect_unix_socket(description, node.value(), {getpid(), euid(), egid()});
    if (connected) return true;
    if (connected.code() == StatusCode::not_found) return false;
    return connected;
}
Result<sched::UnixAccept> PosixService::prepare_unix_accept(u16 description, u32 flags)
{
    if (!scheduler_) return Status::not_initialized("UNIX accept owner is unavailable");
    if (flags & ~(o_CLOEXEC | o_NONBLOCK)) return Status::invalid_argument("unsupported UNIX accept flags");
    return scheduler_->prepare_unix_accept(resources(), description, (flags & o_CLOEXEC) != 0, flags & o_NONBLOCK);
}
Result<sched::UnixPeerCredentials> PosixService::unix_peer_credentials(u16 description) const
{
    if (!scheduler_) return Status::not_initialized("UNIX credentials owner is unavailable");
    return scheduler_->unix_peer_credentials(description);
}

Result<usize> PosixService::send_unix_rights(Fd fd, std::span<const std::byte> payload,
    std::span<const Fd> descriptors, bool nonblocking)
{
    if (!scheduler_) return Status::not_initialized("POSIX scheduler is unavailable");
    return scheduler_->send_unix_rights(resources(), fd, payload, descriptors, nonblocking);
}
Result<sched::UnixReceive> PosixService::prepare_unix_receive(Fd fd, std::span<std::byte> preview,
    usize descriptor_capacity, bool peek, bool close_on_exec, bool nonblocking)
{
    if (!scheduler_) return Status::not_initialized("POSIX scheduler is unavailable");
    return scheduler_->prepare_unix_receive(resources(), fd, preview, descriptor_capacity, peek,
        close_on_exec, nonblocking);
}

Result<Fd> PosixService::install_native_udp(u32 handle, bool close_on_exec, bool nonblocking)
{
    if (!initialized_ || !scheduler_ || !handle)
        return Status::not_initialized("native UDP descriptor owner is unavailable");
    auto fd = allocate_fd();
    if (!fd) return fd.status();
    auto description = scheduler_->allocate_description(sched::OpenFileDescription{
        .used = true, .readable = true, .writable = true,
        .flags = o_RDWR | (nonblocking ? o_NONBLOCK : 0U), .native_udp_handle = handle});
    if (!description) return description.status();
    const auto published = scheduler_->install_descriptor(resources(), fd.value(),
                                                          description.value(), close_on_exec);
    if (!published)
    {
        // Zero-reference reservations still belong to the caller, including
        // the endpoint; the caller closes that endpoint on this failure.
        *scheduler_->open_description(description.value()) = {};
        return published;
    }
    return fd.value();
}

Result<Fd> PosixService::memfd_create(std::string_view name, u32 flags)
{
    if (!initialized_ || !scheduler_ || !scheduler_->shared_memory_files())
        return Status::not_initialized("shared file descriptor owner is unavailable");
    if ((flags & ~(mfd_CLOEXEC | mfd_ALLOW_SEALING)) || !(flags & mfd_ALLOW_SEALING))
        return Status::unsupported("shared file creation requires the bounded sealing flags");
    auto fd = allocate_fd();
    if (!fd) return fd.status();
    auto &service = *scheduler_->shared_memory_files();
    auto reservation = service.reserve(name, euid(), egid());
    if (!reservation) return reservation.status();
    auto handle = reservation.value().publish();
    if (!handle) return handle.status();
    auto description = scheduler_->allocate_description(sched::OpenFileDescription{
        .used = true, .readable = true, .writable = true, .large_file = true,
        .flags = o_RDWR, .native_shared_file = handle.value()});
    if (!description)
    {
        static_cast<void>(service.release_description(handle.value()));
        return description.status();
    }
    if (auto status = scheduler_->retain_description(description.value()); !status)
    {
        *scheduler_->open_description(description.value()) = {};
        static_cast<void>(service.release_description(handle.value()));
        return status;
    }
    PendingDescription pending(*scheduler_, description.value());
    if (auto status = scheduler_->install_descriptor(resources(), fd.value(), description.value(),
            (flags & mfd_CLOEXEC) != 0); !status)
        return status;
    return fd.value();
}

Result<DescriptorReadiness> PosixService::ready_events(Fd fd, ReadyEvents requested) const
{
    if (!scheduler_)
        return Status::not_initialized("POSIX service not initialized");
    const auto *slot = resources().files.entry(fd);
    return ready_description(slot && slot->used ? slot->description : sched::invalid_description, requested);
}
Result<DescriptorReadiness> PosixService::ready_description(u16 id, ReadyEvents requested) const
{
    return poll_description<ReadinessPolicy::native>(id, requested);
}
template <ReadinessPolicy Policy>
Result<DescriptorReadiness> PosixService::poll_description(u16 id, ReadyEvents requested) const
{
    if (!scheduler_)
        return Status::not_initialized("POSIX service not initialized");
    const auto *entry = scheduler_->open_description(id);
    if (!entry || !entry->references)
        return DescriptorReadiness{.events = ReadyEvents::invalid};
    DescriptorReadiness ready{};
    if (entry->pty_endpoint)
    {
        if constexpr (Policy == ReadinessPolicy::linux)
            return Status::unsupported("raw PTY Linux readiness is outside the native candidate");
        auto state = scheduler_->pty_description_readiness(id);
        if (!state) return state.status();
        if (state.value().readable) ready.events |= requested & ReadyEvents::readable;
        if (state.value().writable) ready.events |= requested & ReadyEvents::writable;
        if (state.value().hangup) ready.events |= ReadyEvents::hangup;
        if (state.value().error) ready.events |= ReadyEvents::error;
        return ready;
    }
    if (entry->native_shared_file)
    {
        auto *service = scheduler_->shared_memory_files();
        if (!service || !service->valid(entry->native_shared_file))
            return Status::bad_descriptor("shared file readiness lost its owner");
        ready.events = requested & (ReadyEvents::readable | ReadyEvents::writable);
        return ready;
    }
    if (scheduler_->is_unix_socket(id) && entry->pipe == sched::invalid_description)
    {
        auto state = scheduler_->unix_socket_readiness(id);
        if (!state) return state.status();
        if (state.value().readable) ready.events |= requested & ReadyEvents::readable;
        if (state.value().writable) ready.events |= requested & ReadyEvents::writable;
        if (state.value().hangup) ready.events |= ReadyEvents::hangup;
        return ready;
    }
    if (entry->pipe != sched::invalid_description)
    {
        auto incoming = scheduler_->pipe_readiness(entry->pipe);
        if (!incoming) return incoming.status();
        auto outgoing = entry->unix_write_pipe != sched::invalid_description
                          ? scheduler_->pipe_readiness(entry->unix_write_pipe) : incoming;
        if (!outgoing) return outgoing.status();
        const auto state = incoming.value();
        const auto write_state = outgoing.value();
        const bool unix_stream = entry->unix_write_pipe != sched::invalid_description;
        if (entry->readable)
        {
            if (state.buffered || ((Policy == ReadinessPolicy::native || unix_stream) && !state.writers))
                ready.events |= requested & ReadyEvents::readable;
            if (!state.writers)
                ready.events |= ReadyEvents::hangup;
        }
        if (entry->writable)
        {
            if (write_state.available && (Policy == ReadinessPolicy::linux || write_state.readers))
                ready.events |= requested & ReadyEvents::writable;
            // A clean AF_UNIX peer close is EOF/HUP, not the pipe POLLERR condition.
            if (!write_state.readers && (Policy == ReadinessPolicy::native || !unix_stream))
                ready.events |= ReadyEvents::error;
        }
        return ready;
    }
    if (entry->input_console)
    {
        ready.console_input = true;
        return ready;
    }
    if (entry->console)
    {
        ready.events = requested & ReadyEvents::writable;
        return ready;
    }
    if (entry->node)
    {
        auto metadata = entry->node->query_metadata();
        if (!metadata)
            return metadata.status();
        const auto type = metadata.value().type;
        if (type == fs::NodeType::regular || type == fs::NodeType::directory)
        {
            // Regular filesystem I/O completes synchronously, including EOF
            // and access-mode errors; poll readiness does not grant access.
            ready.events = requested & (ReadyEvents::readable | ReadyEvents::writable);
            return ready;
        }
    }
    return Status::unsupported("descriptor has no readiness provider");
}

template Result<DescriptorReadiness> PosixService::poll_description<ReadinessPolicy::native>(u16, ReadyEvents) const;
template Result<DescriptorReadiness> PosixService::poll_description<ReadinessPolicy::linux>(u16, ReadyEvents) const;

Status PosixService::close(Fd fd)
{
    return scheduler_ == nullptr ? Status::not_initialized("POSIX service not initialized")
                                 : scheduler_->close_descriptor(resources(), fd);
}

Status PosixService::close_range(Fd first, Fd last)
{
    if (first < 0 || last < first)
        return Status::invalid_argument("invalid close range");
    const auto capped = static_cast<usize>(last) >= max_open_files ? max_open_files - 1 : static_cast<usize>(last);
    for (usize fd = static_cast<usize>(first); fd <= capped; ++fd)
        if (resources().files.contains(static_cast<Fd>(fd)))
            static_cast<void>(close(static_cast<Fd>(fd)));
    return Status::success();
}

Result<Fd> PosixService::duplicate(Fd fd, Fd minimum, u32 flags)
{
    if (flags & ~o_CLOEXEC)
        return Status::invalid_argument("unsupported duplicate flags");
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
    if (auto status = copy_descriptor(fd, target.value(), flags); !status.ok())
    {
        return status;
    }
    return target.value();
}

Result<Fd> PosixService::duplicate_to(Fd fd, Fd target, u32 flags)
{
    if (target < 0 || static_cast<usize>(target) >= resources().files.entries().size())
    {
        return Status::invalid_argument("target file descriptor out of range");
    }
    auto *entry = descriptor(fd);
    if (entry == nullptr || !entry->used)
    {
        return Status::invalid_argument("bad file descriptor");
    }
    if (fd == target)
        return target;
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
    if (entry->native_shared_file)
    {
        auto *service = scheduler_->shared_memory_files();
        if (!service) return Status::not_initialized("shared file owner is unavailable");
        auto result = service->read(entry->native_shared_file, entry->offset, out);
        if (result) entry->offset += result.value();
        return result;
    }
    if (entry->pty_endpoint)
    {
        const auto *slot = resources().files.entry(fd);
        return scheduler_->read_pty_description(slot->description, out);
    }
    if (entry->pipe != sched::invalid_description)
        return scheduler_->read_pipe(entry->pipe, out, (entry->flags & o_NONBLOCK) != 0);
    if (entry->input_console)
    {
        return Status::would_block("stdin has no buffered data");
    }
    if (entry->node == nullptr)
    {
        return Status::invalid_argument("file descriptor has no backing node");
    }
    // Providers return at most one FileBuffer per call. As on Linux, a read
    // fills the request until a provider returns a short chunk (end of file).
    usize count = 0;
    while (count < out.size())
    {
        const usize wanted = out.size() - count;
        auto data = entry->node->read(entry->offset + count, wanted);
        if (!data)
        {
            if (count) break;
            return data.status();
        }
        const auto chunk = data.value().size < wanted ? data.value().size : wanted;
        for (usize i = 0; i < chunk; ++i)
        {
            out[count + i] = data.value().data[i];
        }
        count += chunk;
        if (chunk < (wanted < fs::max_file_data ? wanted : fs::max_file_data))
            break;
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
    if (entry->native_shared_file)
    {
        auto *service = scheduler_->shared_memory_files();
        if (!service) return Status::not_initialized("shared file owner is unavailable");
        if (in.empty()) return usize{0};
        usize offset = entry->offset;
        if (auto status = service->check_write(entry->native_shared_file, in.size()); !status) return status;
        if (entry->flags & o_APPEND)
        {
            auto metadata = service->info(entry->native_shared_file);
            if (!metadata) return metadata.status();
            offset = metadata.value().size;
        }
        auto result = service->write(entry->native_shared_file, offset, in);
        if (result) entry->offset = offset + result.value();
        return result;
    }
    if (entry->pty_endpoint)
    {
        const auto *slot = resources().files.entry(fd);
        return scheduler_->write_pty_description(slot->description, in);
    }
    if (entry->pipe != sched::invalid_description)
        return scheduler_->write_pipe(entry->unix_write_pipe != sched::invalid_description
                                          ? entry->unix_write_pipe : entry->pipe,
                                      in, (entry->flags & o_NONBLOCK) != 0,
                                      entry->unix_write_pipe == sched::invalid_description);
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
    usize offset = entry->offset;
    if ((entry->flags & o_APPEND) != 0)
    {
        auto metadata = entry->node->query_metadata();
        if (!metadata)
            return metadata.status();
        offset = metadata.value().size;
    }
    if (in.size() > ~usize{0} - offset)
        return Status::overflow("write offset overflow");
    if (auto status = entry->node->write(offset, in); !status.ok())
    {
        return status;
    }
    entry->offset = offset + in.size();
    return in.size();
}

Status PosixService::fsync(Fd fd)
{
    auto *entry = descriptor(fd);
    if (!entry || !entry->used)
        return Status::bad_descriptor("bad file descriptor");
    if (entry->native_shared_file)
        return Status::unsupported("volatile shared file synchronization is not admitted");
    if (!entry->node)
        return Status::invalid_argument("file descriptor does not support synchronization");
    return entry->node->sync();
}
Status PosixService::sync_persistent(Fd fd)
{
    auto *entry = descriptor(fd);
    if (!entry || !entry->used)
        return Status::bad_descriptor("bad file descriptor");
    if (entry->native_shared_file || !entry->node)
        return Status::unsupported("file descriptor has no persistent storage provider");
    return entry->node->sync_persistent();
}
Status PosixService::ftruncate(Fd fd, usize size)
{
    auto *entry = descriptor(fd);
    if (!entry || !entry->used || !entry->writable)
        return Status::bad_descriptor("file descriptor is not writable");
    if (entry->native_shared_file)
    {
        auto *service = scheduler_->shared_memory_files();
        return service ? service->initialize_size(entry->native_shared_file, size)
                       : Status::not_initialized("shared file owner is unavailable");
    }
    if (!entry->node)
        return Status::invalid_argument("file descriptor cannot be truncated");
    return entry->node->truncate(size);
}
Status PosixService::truncate(std::string_view path, usize size)
{
    auto fd = open(path, o_WRONLY);
    if (!fd)
        return fd.status();
    const auto status = ftruncate(fd.value(), size);
    const auto closed = close(fd.value());
    return status ? closed : status;
}
Result<usize> PosixService::pread(Fd fd, std::span<std::byte> out, usize offset)
{
    auto *entry = descriptor(fd);
    if (!entry || !entry->used)
        return Status::bad_descriptor("bad file descriptor");
    if (entry->native_shared_file)
    {
        if (!entry->readable) return Status::bad_descriptor("shared file descriptor is not readable");
        auto *service = scheduler_->shared_memory_files();
        return service ? service->read(entry->native_shared_file, offset, out)
                       : Result<usize>{Status::not_initialized("shared file owner is unavailable")};
    }
    if (entry->pty_endpoint || !entry->node || entry->pipe != sched::invalid_description || entry->input_console || entry->console)
        return Status::not_seekable("file descriptor is not seekable");
    if (!entry->readable)
        return Status::bad_descriptor("file descriptor is not readable");
    auto metadata = entry->node->query_metadata();
    if (!metadata)
        return metadata.status();
    if (metadata.value().type == fs::NodeType::directory)
        return Status::is_directory("directory cannot be read as a byte stream");
    if (out.empty())
        return usize{0};
    auto result = entry->node->read(offset, out.size());
    if (!result)
        return result.status();
    const auto count = result.value().size < out.size() ? result.value().size : out.size();
    for (usize i = 0; i < count; ++i)
        out[i] = result.value().data[i];
    return count;
}
Result<usize> PosixService::pwrite(Fd fd, std::span<const std::byte> in, usize offset)
{
    return pwrite_with_policy<PositionedWritePolicy::native>(fd, in, offset);
}
template <PositionedWritePolicy Policy>
Result<usize> PosixService::pwrite_with_policy(Fd fd, std::span<const std::byte> in, usize offset, u64 maximum_offset)
{
    auto *entry = descriptor(fd);
    if (!entry || !entry->used)
        return Status::bad_descriptor("bad file descriptor");
    if (entry->native_shared_file)
    {
        if (!entry->writable) return Status::bad_descriptor("shared file descriptor is not writable");
        auto *service = scheduler_->shared_memory_files();
        if (!service) return Status::not_initialized("shared file owner is unavailable");
        if (in.empty()) return usize{0};
        if (auto status = service->check_write(entry->native_shared_file, in.size()); !status) return status;
        if constexpr (Policy == PositionedWritePolicy::linux_append)
        {
            if (entry->flags & o_APPEND)
            {
                auto metadata = service->info(entry->native_shared_file);
                if (!metadata) return metadata.status();
                offset = metadata.value().size;
            }
            if (offset > maximum_offset || in.size() > maximum_offset - offset)
                return Status::overflow("positioned shared file write exceeds native offset capacity");
        }
        return service->write(entry->native_shared_file, offset, in);
    }
    if (entry->pty_endpoint || !entry->node || entry->pipe != sched::invalid_description || entry->console || entry->input_console)
        return Status::not_seekable("file descriptor is not seekable");
    if (!entry->writable)
        return Status::bad_descriptor("file descriptor is not writable");
    if (in.empty())
        return usize{0};
    if constexpr (Policy == PositionedWritePolicy::linux_append)
    {
        // Linux's positional writes honor O_APPEND without changing f_pos.
        if (entry->flags & o_APPEND)
        {
            auto metadata = entry->node->query_metadata();
            if (!metadata)
                return metadata.status();
            offset = metadata.value().size;
        }
        if (offset > maximum_offset || in.size() > maximum_offset - offset)
            return Status::overflow("positioned write exceeds native offset capacity");
    }
    if (in.size() > ~usize{0} - offset)
        return Status::overflow("positioned write offset overflow");
    if (auto status = entry->node->write(offset, in); !status.ok())
        return status;
    return in.size();
}
template Result<usize> PosixService::pwrite_with_policy<PositionedWritePolicy::native>(Fd, std::span<const std::byte>, usize, u64);
template Result<usize> PosixService::pwrite_with_policy<PositionedWritePolicy::linux_append>(Fd, std::span<const std::byte>, usize, u64);

Result<usize> PosixService::readv(Fd fd, std::span<const IoVector> vectors)
{
    const auto *stream = descriptor(fd);
    if (stream && stream->used && stream->readable && stream->unix_write_pipe != sched::invalid_description)
    {
        // One stream consumption preserves the first ancillary-record boundary
        // even when it falls exactly at an iovec boundary.
        std::array<std::byte, sched::pipe_capacity> gathered{};
        usize requested{};
        for (const auto &vector : vectors)
        {
            if (!vector.base && vector.length) return Status::invalid_argument("readv buffer is null");
            const auto room = gathered.size() - requested;
            requested += vector.length < room ? vector.length : room;
        }
        auto result = read(fd, {gathered.data(), requested});
        if (!result) return result.status();
        usize copied{};
        for (const auto &vector : vectors)
        {
            const auto remaining = result.value() - copied;
            const auto count = vector.length < remaining ? vector.length : remaining;
            auto *output = reinterpret_cast<std::byte *>(vector.base);
            for (usize index = 0; index < count; ++index) output[index] = gathered[copied + index];
            copied += count;
            if (copied == result.value()) break;
        }
        return result;
    }
    const auto *shared = descriptor(fd);
    if (shared && shared->used && shared->native_shared_file)
    {
        std::array<std::byte, sched::pipe_capacity> gathered{};
        usize requested{};
        for (const auto &vector : vectors)
        {
            if (!vector.base && vector.length) return Status::invalid_argument("shared file readv buffer is null");
            if (vector.length > ~uptr{0} - vector.base)
                return Status::overflow("shared file readv buffer range wraps");
            const auto room = gathered.size() - requested;
            requested += vector.length < room ? vector.length : room;
        }
        auto result = read(fd, {gathered.data(), requested});
        if (!result) return result.status();
        usize position{};
        for (const auto &vector : vectors)
        {
            const auto remaining = result.value() - position;
            const auto count = vector.length < remaining ? vector.length : remaining;
            auto *output = reinterpret_cast<std::byte *>(vector.base);
            for (usize index = 0; index < count; ++index) output[index] = gathered[position + index];
            position += count;
            if (position == result.value()) break;
        }
        return result;
    }
    usize total = 0;
    for (const auto &vector : vectors)
    {
        if (vector.base == 0 && vector.length != 0)
        {
            return total == 0 ? Result<usize>{Status::invalid_argument("readv buffer is null")} : Result<usize>{total};
        }
        const auto output = std::span<std::byte>{reinterpret_cast<std::byte *>(vector.base), vector.length};
        const auto *entry = descriptor(fd);
        // Once a vector has made progress, returning a short read must not
        // leave the caller blocked while reporting a positive byte count.
        auto result = total && entry && entry->readable && entry->pipe != sched::invalid_description
                          ? scheduler_->read_pipe(entry->pipe, output, true)
                          : read(fd, output);
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
    auto *entry = descriptor(fd);
    if (entry && entry->used && entry->native_shared_file)
    {
        std::array<std::byte, sched::pipe_capacity> gathered{};
        usize requested{};
        for (const auto &vector : vectors)
        {
            if (!vector.base && vector.length) return Status::invalid_argument("shared file writev buffer is null");
            if (vector.length > ~uptr{0} - vector.base)
                return Status::overflow("shared file writev buffer range wraps");
            const auto room = gathered.size() - requested;
            requested += vector.length < room ? vector.length : room;
        }
        auto *service = scheduler_->shared_memory_files();
        if (!service) return Status::not_initialized("shared file owner is unavailable");
        if (auto status = service->check_write(entry->native_shared_file, requested); !status) return status;
        usize position{};
        for (const auto &vector : vectors)
        {
            const auto remaining = requested - position;
            const auto count = vector.length < remaining ? vector.length : remaining;
            const auto *input = reinterpret_cast<const std::byte *>(vector.base);
            for (usize index = 0; index < count; ++index) gathered[position + index] = input[index];
            position += count;
            if (position == requested) break;
        }
        return write(fd, {gathered.data(), requested});
    }
    if (entry && entry->used && entry->writable && entry->pipe != sched::invalid_description)
    {
        // PIPE_BUF atomicity covers the entire vector, not each element.
        // One extra byte distinguishes a large, partially writable request.
        std::array<std::byte, sched::pipe_capacity + 1> gathered{};
        usize size = 0;
        for (const auto &vector : vectors)
        {
            if (!vector.base && vector.length)
                return Status::invalid_argument("writev buffer is null");
            const auto remaining = gathered.size() - size;
            const auto count = vector.length < remaining ? vector.length : remaining;
            const auto *source = reinterpret_cast<const std::byte *>(vector.base);
            for (usize i = 0; i < count; ++i)
                gathered[size + i] = source[i];
            size += count;
        }
        return write(fd, {gathered.data(), size});
    }
    usize total = 0;
    for (const auto &vector : vectors)
    {
        if (vector.base == 0 && vector.length != 0)
        {
            return total == 0 ? Result<usize>{Status::invalid_argument("writev buffer is null")} : Result<usize>{total};
        }
        auto result =
            write(fd, std::span<const std::byte>{reinterpret_cast<const std::byte *>(vector.base), vector.length});
        if (!result)
        {
            return total == 0 ? result : Result<usize>{total};
        }
        total += result.value();
        if (result.value() != vector.length)
            break;
    }
    return total;
}

Result<usize> PosixService::seek(Fd fd, i64 offset, SeekWhence whence)
{
    auto *entry = descriptor(fd);
    if (entry && entry->used && entry->pty_endpoint)
        return Status::not_seekable("raw PTY descriptions are not seekable");
    if (entry == nullptr || !entry->used || entry->console || entry->input_console ||
        entry->pipe != sched::invalid_description)
    {
        return Status::invalid_argument("file descriptor is not seekable");
    }
    if (!entry->node && !entry->native_shared_file)
        return Status::invalid_argument("file descriptor has no backing node");
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
    {
        usize size{};
        if (entry->native_shared_file)
        {
            auto *service = scheduler_->shared_memory_files();
            if (!service) return Status::not_initialized("shared file owner is unavailable");
            auto metadata = service->info(entry->native_shared_file);
            if (!metadata) return metadata.status();
            size = metadata.value().size;
        }
        else
        {
            auto metadata = entry->node->query_metadata();
            if (!metadata) return metadata.status();
            size = metadata.value().size;
        }
        if (!std::in_range<i64>(size))
            return Status::overflow("file size exceeds seek range");
        base = static_cast<i64>(size);
        break;
    }
    default:
        return Status::invalid_argument("invalid seek origin");
    }
    if (offset > 0 && base > __INT64_MAX__ - offset)
        return Status::overflow("seek offset overflow");
    const i64 next = base + offset;
    if (next < 0)
    {
        return Status::invalid_argument("negative seek offset");
    }
    if (static_cast<u64>(next) > static_cast<u64>(__INTPTR_MAX__))
        return Status::overflow("seek offset exceeds native off_t");
    entry->offset = static_cast<usize>(next);
    return entry->offset;
}

Result<i64> PosixService::fcntl(Fd fd, u32 command, u64 argument)
{
    auto *entry = descriptor(fd);
    if (entry == nullptr || !entry->used)
    {
        return Status::bad_descriptor("bad file descriptor");
    }
    switch (command)
    {
    case f_ADD_SEALS:
    case f_GET_SEALS: {
        if (!entry->native_shared_file) return Status::unsupported("seals require a shared file provider");
        auto *service = scheduler_->shared_memory_files();
        if (!service) return Status::not_initialized("shared file owner is unavailable");
        if (command == f_GET_SEALS)
        {
            auto metadata = service->info(entry->native_shared_file);
            return metadata ? Result<i64>{static_cast<i64>(metadata.value().seals)} : Result<i64>{metadata.status()};
        }
        if (!entry->writable) return Status::permission("shared file seals require write authority");
        if (argument > ~u32{0}) return Status::invalid_argument("shared file seal argument exceeds its word width");
        auto status = service->add_seals(entry->native_shared_file, static_cast<u32>(argument));
        return status ? Result<i64>{i64{0}} : Result<i64>{status};
    }
    case f_DUPFD: {
        auto duplicated = duplicate(fd, static_cast<Fd>(argument));
        return duplicated ? Result<i64>{static_cast<i64>(duplicated.value())} : Result<i64>{duplicated.status()};
    }
    case f_GETFD:
        return resources().files.entry(fd)->close_on_exec ? fd_CLOEXEC : 0;
    case f_SETFD:
        resources().files.entry(fd)->close_on_exec = (argument & fd_CLOEXEC) != 0;
        return 0;
    case f_GETFL:
        return static_cast<i64>(entry->flags);
    case f_SETFL:
        if (entry->native_udp_handle && (static_cast<u32>(argument) & o_APPEND))
            return Status::unsupported("native UDP does not support append status");
        entry->flags = (entry->flags & ~(o_APPEND | o_NONBLOCK)) |
                       (static_cast<u32>(argument) & (o_APPEND | o_NONBLOCK));
        return 0;
    default:
        return Status::unsupported("fcntl command is not implemented");
    }
}

Result<i64> PosixService::ioctl(Fd fd, u64, u64)
{
    auto *entry = descriptor(fd);
    if (entry == nullptr || !entry->used)
        return Status::bad_descriptor("bad file descriptor");
    // Raw generic requests never dereference caller pointers. User adapters
    // marshal the bounded terminal controls through the scalar seam below.
    return Status::not_terminal("ioctl request is not implemented for this descriptor");
}

Status PosixService::console_control_descriptor(Fd fd) const
{
    const auto *entry = descriptor(fd);
    if (!entry || !entry->used) return Status::bad_descriptor("bad console descriptor");
    if (!entry->console && !entry->input_console)
        return Status::not_terminal("descriptor is not a native console description");
    return {};
}

Result<i64> PosixService::console_control(Fd fd, sched::ConsoleControlOperation operation, i32 value)
{
    if (auto status = console_control_descriptor(fd); !status) return status;
    return scheduler_->console_control(operation, value);
}

Result<FileStatus> PosixService::fstat(Fd fd) const
{
    auto *entry = descriptor(fd);
    if (entry == nullptr || !entry->used)
    {
        return Status::invalid_argument("bad file descriptor");
    }
    if (entry->pty_endpoint)
        return FileStatus{.type = fs::NodeType::device,
            .mode = fs::mode_character_device | 0600u,
            .link_count = 1, .block_size = fs::metadata_block_size};
    if (entry->native_shared_file)
    {
        auto *service = scheduler_->shared_memory_files();
        if (!service) return Status::not_initialized("shared file owner is unavailable");
        auto metadata = service->info(entry->native_shared_file);
        if (!metadata) return metadata.status();
        fs::FileAttributes attributes{};
        attributes.inode = metadata.value().inode;
        attributes.mark(fs::Attribute::inode);
        attributes.mark(fs::Attribute::blocks512);
        return FileStatus{.type = fs::NodeType::regular, .size = metadata.value().size,
            .mode = fs::mode_for(fs::NodeType::regular, 0600u), .uid = metadata.value().uid, .gid = metadata.value().gid,
            .link_count = 0, .block_size = user::native::SharedMemoryFiles::page_size,
            .blocks = metadata.value().size / 512, .attributes = attributes,
            .provider = FileStatusProvider::volatile_shared_file};
    }
    const auto *slot = resources().files.entry(fd);
    if (slot && slot->used && scheduler_->is_unix_socket(slot->description))
        return FileStatus{.type = fs::NodeType::socket, .mode = fs::mode_socket | 0600u,
                          .link_count = 1, .block_size = fs::metadata_block_size};
    if (entry->pipe != sched::invalid_description)
    {
        if (entry->unix_write_pipe != sched::invalid_description)
            return FileStatus{.type = fs::NodeType::device, .mode = fs::mode_socket | 0600u,
                              .link_count = 1, .block_size = fs::metadata_block_size};
        return FileStatus{.type = fs::NodeType::device, .mode = fs::mode_fifo | 0600u};
    }
    if (entry->native_udp_handle)
        return FileStatus{.type = fs::NodeType::device, .mode = fs::mode_socket | 0600u,
                          .link_count = 1, .block_size = fs::metadata_block_size};
    if (entry->console || entry->input_console)
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
    auto queried = vfs_->query_metadata(*entry->node);
    if (!queried)
        return queried.status();
    const auto metadata = queried.value();
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

} // namespace ok::posix
