#include "ok/posix/posix.hpp"

namespace ok::posix
{

Status PosixService::initialize(fs::VirtualFileSystem &vfs, driver::ConsoleDriver &console, sched::Scheduler &scheduler)
{
    vfs_ = &vfs;
    console_ = &console;
    scheduler_ = &scheduler;
    initialized_ = true;
    files_ = {};
    mappings_ = {};
    files_[0] = FileDescriptor{.used = true, .readable = true, .path = FixedString<96>{"stdin"}};
    files_[1] = FileDescriptor{.used = true, .writable = true, .console = true, .path = FixedString<96>{"stdout"}};
    files_[2] = FileDescriptor{.used = true, .writable = true, .console = true, .path = FixedString<96>{"stderr"}};
    static_cast<void>(cwd_.assign("/"));
    monotonic_ticks_ = 0;
    program_break_ = program_break_base_;
    next_mapping_address_ = 0x2000'0000;
    fs_base_ = 0;
    gs_base_ = 0;
    clear_tid_address_ = 0;
    last_exit_code_ = 0;
    return Status::success();
}

sched::ProcessId PosixService::getpid() const
{
    return scheduler_ == nullptr ? 0 : scheduler_->current_pid();
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

Result<Fd> PosixService::allocate_fd(Fd minimum)
{
    const usize start = minimum < 0 ? 0 : static_cast<usize>(minimum);
    for (usize i = start; i < files_.size(); ++i)
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

Result<std::string_view> PosixService::resolve_path(Fd dirfd, std::string_view path)
{
    if (path.empty())
    {
        if (dirfd == at_FDCWD)
        {
            return cwd_.view();
        }
        auto *entry = descriptor(dirfd);
        if (entry == nullptr || !entry->used)
        {
            return Status::invalid_argument("bad directory file descriptor");
        }
        return entry->path.view();
    }
    if (path[0] == '/' || dirfd == at_FDCWD)
    {
        return normalize_path(path);
    }
    auto *entry = descriptor(dirfd);
    if (entry == nullptr || !entry->used || entry->node == nullptr ||
        entry->node->metadata().type != fs::NodeType::directory)
    {
        return Status::invalid_argument("bad directory file descriptor");
    }
    normalized_path_.clear();
    if (auto status = normalized_path_.append(entry->path.view()); !status.ok())
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

Status PosixService::copy_descriptor(Fd source, Fd target, u32 flags)
{
    auto *source_entry = descriptor(source);
    auto *target_entry = descriptor(target);
    if (source_entry == nullptr || target_entry == nullptr || !source_entry->used)
    {
        return Status::invalid_argument("bad file descriptor");
    }
    *target_entry = *source_entry;
    target_entry->close_on_exec = (flags & o_CLOEXEC) != 0;
    return Status::success();
}

} // namespace ok::posix
