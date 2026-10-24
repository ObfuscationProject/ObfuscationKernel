#include "ok/posix/posix.hpp"
namespace ok::posix
{
Status PosixService::initialize(fs::VirtualFileSystem &vfs, driver::ConsoleDriver &console, sched::Scheduler &scheduler)
{
    vfs_ = &vfs;
    console_ = &console;
    scheduler_ = &scheduler;
    initialized_ = true;
    monotonic_ticks_ = 0;
    clock_resolution_ = 1000000;
    boot_tls_ = {};
    boot_credentials_ = user::kernel_credentials();
    return Status::success();
}
sched::ProcessId PosixService::getpid() const
{
    return scheduler_ == nullptr ? 0 : scheduler_->current_pid();
}
sched::ThreadId PosixService::gettid() const
{
    return scheduler_ == nullptr ? 0 : scheduler_->current_tid(0);
}
sched::ProcessId PosixService::getppid() const
{
    const auto *process = scheduler_ == nullptr ? nullptr : scheduler_->find(getpid());
    return process == nullptr ? 0 : process->parent_pid();
}
Result<sched::ProcessId> PosixService::getpgid(i32 pid) const
{
    return scheduler_ ? scheduler_->process_group(getpid(), pid)
                      : Result<sched::ProcessId>(Status::not_initialized("POSIX service not initialized"));
}
Result<sched::ProcessId> PosixService::getsid(i32 pid) const
{
    return scheduler_ ? scheduler_->session_id(getpid(), pid)
                      : Result<sched::ProcessId>(Status::not_initialized("POSIX service not initialized"));
}
Result<sched::ProcessId> PosixService::setsid()
{
    return scheduler_ ? scheduler_->create_session(getpid())
                      : Result<sched::ProcessId>(Status::not_initialized("POSIX service not initialized"));
}
Status PosixService::setpgid(i32 pid, i32 group)
{
    return scheduler_ ? scheduler_->set_process_group(getpid(), pid, group)
                      : Status::not_initialized("POSIX service not initialized");
}
sched::ProcessResources &PosixService::resources()
{
    if (scheduler_ == nullptr)
        return unbound_resources_;
    auto *process = scheduler_->find(getpid());
    return process == nullptr ? scheduler_->boot_resources() : process->resources();
}
const sched::ProcessResources &PosixService::resources() const
{
    if (scheduler_ == nullptr)
        return unbound_resources_;
    const auto *process = scheduler_->find(getpid());
    return process == nullptr ? scheduler_->boot_resources() : process->resources();
}
sched::ThreadLocalState &PosixService::thread_local_state()
{
    auto *process = scheduler_ == nullptr ? nullptr : scheduler_->find(getpid());
    if (process == nullptr || process->threads().empty())
        return boot_tls_;
    for (auto &thread : process->threads())
        if (thread.tid == scheduler_->current_tid(0))
            return thread.tls;
    return process->threads()[0].tls;
}
user::Credentials PosixService::user_credentials() const
{
    const auto *process = scheduler_ == nullptr ? nullptr : scheduler_->find(getpid());
    return process == nullptr ? boot_credentials_ : process->credentials();
}
Status PosixService::set_identity(u32 uid, u32 gid)
{
    return set_credentials(user::Credentials{.uid = uid, .gid = gid, .euid = uid, .egid = gid,
                                            .kernel_space = false, .suid = uid, .sgid = gid});
}
Status PosixService::set_credentials(user::Credentials credentials)
{
    auto *process = scheduler_ == nullptr ? nullptr : scheduler_->find(getpid());
    if (process == nullptr)
        boot_credentials_ = credentials;
    else
        process->set_credentials(credentials);
    return Status::success();
}
Status PosixService::activate(sched::ProcessId pid)
{
    return scheduler_ == nullptr ? Status::not_initialized("POSIX service not initialized") : scheduler_->activate(pid);
}
Result<sched::ProcessId> PosixService::fork()
{
    return fork(getpid());
}
Result<sched::ProcessId> PosixService::fork(sched::ProcessId parent, bool publish)
{
    if (scheduler_ == nullptr)
        return Status::not_initialized("POSIX service not initialized");
    return scheduler_->fork_process(parent, publish);
}
Status PosixService::exec(sched::ProcessId pid, arch::CpuContext context, const sched::MemoryMap &memory)
{
    return scheduler_ == nullptr ? Status::not_initialized("POSIX service not initialized")
                                 : scheduler_->replace_image(pid, context, memory);
}
Status PosixService::exit(sched::ProcessId pid, i32 code)
{
    return scheduler_ == nullptr ? Status::not_initialized("POSIX service not initialized")
                                 : scheduler_->exit_process(pid, code);
}
Result<i32> PosixService::wait(sched::ProcessId parent, sched::ProcessId child)
{
    if (scheduler_ == nullptr)
        return Status::not_initialized("POSIX service not initialized");
    return scheduler_->wait_process(parent, child);
}
usize PosixService::open_file_count() const
{
    return resources().files.open_count();
}
PosixService::FileDescriptor *PosixService::descriptor(Fd fd)
{
    const auto *slot = resources().files.entry(fd);
    return scheduler_ == nullptr || slot == nullptr || !slot->used ? nullptr
                                                                   : scheduler_->open_description(slot->description);
}
const PosixService::FileDescriptor *PosixService::descriptor(Fd fd) const
{
    const auto *slot = resources().files.entry(fd);
    return scheduler_ == nullptr || slot == nullptr || !slot->used ? nullptr
                                                                   : scheduler_->open_description(slot->description);
}
Result<Fd> PosixService::allocate_fd(Fd minimum)
{
    if (minimum < 0)
        return Status::invalid_argument("negative minimum descriptor");
    for (usize i = static_cast<usize>(minimum); i < max_open_files; ++i)
        if (!resources().files.contains(static_cast<Fd>(i)))
            return static_cast<Fd>(i);
    return Status::process_file_limit("file descriptor table full");
}
Result<fs::ResolveContext> PosixService::path_context(Fd dirfd, std::string_view path)
{
    if (!initialized_ || !vfs_)
        return Status::not_initialized("POSIX service not initialized");
    if (path.empty())
        return Status::not_found("empty pathname");
    fs::ResolveContext context{.credentials = credentials(), .check_search = true};
    if (path.front() == '/')
        return context;
    if (dirfd == at_FDCWD)
    {
        context.start = {resources().cwd_node, resources().cwd_mount_id};
        return context;
    }
    auto *entry = descriptor(dirfd);
    if (!entry || !entry->used || !entry->node)
        return Status::bad_descriptor("bad directory file descriptor");
    auto metadata = entry->node->query_metadata();
    if (!metadata)
        return metadata.status();
    if (metadata.value().type != fs::NodeType::directory)
        return Status::not_directory("file descriptor is not a directory");
    context.start = {entry->node, entry->mount_id};
    return context;
}

Result<fs::PathReference> PosixService::resolve_node(Fd dirfd, std::string_view path, fs::FinalComponent policy)
{
    auto context = path_context(dirfd, path);
    if (!context)
        return context.status();
    return vfs_->resolve(context.value(), path, policy);
}

Status PosixService::copy_descriptor(Fd source, Fd target, u32 flags)
{
    auto *entry = resources().files.entry(source);
    if (scheduler_ == nullptr || entry == nullptr || !entry->used)
        return Status::invalid_argument("bad file descriptor");
    return scheduler_->install_descriptor(resources(), target, entry->description, (flags & o_CLOEXEC) != 0);
}
} // namespace ok::posix
