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

} // namespace

Status PosixService::initialize(fs::VirtualFileSystem &vfs, driver::ConsoleDriver &console, sched::Scheduler &scheduler)
{
    vfs_ = &vfs;
    console_ = &console;
    scheduler_ = &scheduler;
    initialized_ = true;
    files_ = {};
    files_[0] = FileDescriptor{.used = true, .readable = true, .path = FixedString<96>{"stdin"}};
    files_[1] = FileDescriptor{.used = true, .writable = true, .console = true, .path = FixedString<96>{"stdout"}};
    files_[2] = FileDescriptor{.used = true, .writable = true, .console = true, .path = FixedString<96>{"stderr"}};
    static_cast<void>(cwd_.assign("/"));
    monotonic_ticks_ = 0;
    return Status::success();
}

sched::ProcessId PosixService::getpid() const
{
    return scheduler_ == nullptr ? 0 : scheduler_->current_pid();
}

Result<Fd> PosixService::open(std::string_view path, u32 flags, u32 mode)
{
    if (!initialized_ || vfs_ == nullptr)
    {
        return Status::not_initialized("POSIX service not initialized");
    }
    auto normalized_result = normalize_path(path);
    if (!normalized_result)
    {
        return normalized_result.status();
    }
    const auto normalized = normalized_result.value();
    auto *node = vfs_->lookup(normalized);
    if (node == nullptr)
    {
        if ((flags & o_CREAT) == 0)
        {
            return Status::not_found("path not found");
        }
        if (auto status = vfs_->create(normalized, fs::NodeType::regular); !status.ok())
        {
            return status;
        }
        node = vfs_->lookup(normalized);
    }
    if (node == nullptr)
    {
        return Status::fault("created path is not visible");
    }
    if ((flags & o_TRUNC) != 0)
    {
        std::span<const std::byte> empty{};
        if (auto status = node->write(0, empty); !status.ok())
        {
            return status;
        }
        static_cast<void>(mode);
    }

    auto fd = allocate_fd();
    if (!fd)
    {
        return fd.status();
    }
    auto &entry = files_[static_cast<usize>(fd.value())];
    entry.used = true;
    entry.readable = is_readable(flags);
    entry.writable = is_writable(flags) || (flags & o_CREAT) != 0;
    entry.console = false;
    entry.offset = 0;
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
    if (auto status = entry->node->write(entry->offset, in); !status.ok())
    {
        return status;
    }
    entry->offset += in.size();
    return in.size();
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

Status PosixService::mkdir(std::string_view path, u32)
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
    return vfs_->create(normalized.value(), fs::NodeType::directory);
}

Status PosixService::unlink(std::string_view path)
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
    return vfs_->unlink(normalized.value());
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
    return cwd_.assign(normalized.value());
}

Result<FileStatus> PosixService::stat(std::string_view path)
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
    return FileStatus{.type = metadata.value().type, .size = metadata.value().size, .mode = metadata.value().mode};
}

ClockTime PosixService::clock_gettime() const
{
    const auto ticks = monotonic_ticks_ + (scheduler_ == nullptr ? 0 : scheduler_->process_count());
    return ClockTime{.seconds = static_cast<i64>(ticks / 1000), .nanoseconds = static_cast<i64>((ticks % 1000) * 1000000)};
}

UnameInfo PosixService::uname() const
{
    UnameInfo info{};
    static_cast<void>(info.machine.assign("kernel-profile"));
    return info;
}

usize PosixService::open_file_count() const
{
    usize count = 0;
    for (const auto &entry : files_)
    {
        if (entry.used)
        {
            ++count;
        }
    }
    return count;
}

PosixService::FileDescriptor *PosixService::descriptor(Fd fd)
{
    if (fd < 0 || static_cast<usize>(fd) >= files_.size())
    {
        return nullptr;
    }
    return &files_[static_cast<usize>(fd)];
}

const PosixService::FileDescriptor *PosixService::descriptor(Fd fd) const
{
    if (fd < 0 || static_cast<usize>(fd) >= files_.size())
    {
        return nullptr;
    }
    return &files_[static_cast<usize>(fd)];
}

Result<Fd> PosixService::allocate_fd()
{
    for (usize i = 3; i < files_.size(); ++i)
    {
        if (!files_[i].used)
        {
            return static_cast<Fd>(i);
        }
    }
    return Status::overflow("file descriptor table full");
}

Result<std::string_view> PosixService::normalize_path(std::string_view path)
{
    if (path.empty())
    {
        return cwd_.view();
    }
    if (path[0] == '/')
    {
        return path;
    }
    normalized_path_.clear();
    if (auto status = normalized_path_.append(cwd_.view()); !status.ok())
    {
        return status;
    }
    if (normalized_path_.view() != "/" && normalized_path_.view().back() != '/')
    {
        if (auto status = normalized_path_.append('/'); !status.ok())
        {
            return status;
        }
    }
    if (auto status = normalized_path_.append(path); !status.ok())
    {
        return status;
    }
    return normalized_path_.view();
}

} // namespace ok::posix
