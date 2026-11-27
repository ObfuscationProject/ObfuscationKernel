#include "ok/posix/posix.hpp"
#include "ok/smp/kernel_section.hpp"
#include "ok/syscall/syscall.hpp"
namespace ok::posix
{
Status PosixService::initialize(fs::VirtualFileSystem &vfs, driver::ConsoleDriver &console, sched::Scheduler &scheduler)
{
    // The device filesystem reaches the scheduler's /dev/pts entries through
    // the VFS (DeviceFileSystem::attach_terminals).
    if (auto status = vfs.attach_terminal_namespace(scheduler.terminal_namespace()); !status) return status;
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
    if (const auto bound = caller(); bound.process) return bound.process->pid();
    return scheduler_ == nullptr ? 0 : scheduler_->current_pid();
}
sched::ThreadId PosixService::gettid() const
{
    if (const auto bound = caller(); bound.process) return bound.thread->tid;
    return scheduler_ == nullptr ? 0 : scheduler_->current_tid(smp::current_cpu());
}
void PosixService::bind_cpu_caller(smp::CpuId cpu, sched::ProcessControlBlock &process,
                                   sched::ThreadControlBlock &thread)
{
    // The pointers are taken under the kernel lock, by the caller's own CPU.
    smp::check_exclusive("PosixService::bind_cpu_caller");
    if (cpu != smp::current_cpu())
        smp::count_exclusive_violation("PosixService::bind_cpu_caller for another CPU");
    if (cpu < cpu_callers_.size()) cpu_callers_[cpu] = {&process, &thread};
}
void PosixService::unbind_cpu_caller(smp::CpuId cpu)
{
    // Before the binding's scope ends: only this CPU reads its entry.
    if (cpu < cpu_callers_.size()) cpu_callers_[cpu] = {};
}
PosixService::CpuCaller PosixService::caller() const
{
    const auto cpu = smp::current_cpu();
    if (cpu >= cpu_callers_.size()) return {};
    if (cpu_callers_[cpu].process) return cpu_callers_[cpu];
    // The thread this CPU runs: its slot never moves or frees while it does.
    if (scheduler_ == nullptr || cpu >= scheduler_->cpu_count()) return {};
    const auto &slot = scheduler_->cpu_slot(cpu);
    if (!slot.occupied() || !slot.process || !slot.thread || slot.process->pid() != slot.pid ||
        slot.thread->tid != slot.tid)
        return {};
    return {slot.process, slot.thread};
}
void PosixService::boot_fallback([[maybe_unused]] const char *site) const
{
    // A10: a section, or a dispatch for a user caller once the runtime armed
    // its checks, has a caller of its own that the boot state would replace.
    if constexpr (smp::lock_debug_enabled)
        if (smp::in_section() ||
            (smp::exclusive_armed.load(std::memory_order_relaxed) && syscall::dispatch_caller() != 0))
            smp::count_exclusive_violation(site);
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
    if (const auto bound = caller(); bound.process) return bound.process->resources();
    if (scheduler_ == nullptr)
        return unbound_resources_;
    auto *process = scheduler_->find(getpid());
    if (process != nullptr) return process->resources();
    boot_fallback("PosixService::resources boot fallback");
    return scheduler_->boot_resources();
}
const sched::ProcessResources &PosixService::resources() const
{
    if (const auto bound = caller(); bound.process) return bound.process->resources();
    if (scheduler_ == nullptr)
        return unbound_resources_;
    const auto *process = scheduler_->find(getpid());
    if (process != nullptr) return process->resources();
    boot_fallback("PosixService::resources boot fallback");
    return scheduler_->boot_resources();
}
sched::ThreadLocalState &PosixService::thread_local_state()
{
    if (const auto bound = caller(); bound.process) return bound.thread->tls;
    auto *process = scheduler_ == nullptr ? nullptr : scheduler_->find(getpid());
    if (process == nullptr || process->record_count() == 0)
    {
        boot_fallback("PosixService::thread_local_state boot fallback");
        return boot_tls_;
    }
    for (auto &thread : process->thread_records())
        if (thread.tid == scheduler_->current_tid(smp::current_cpu()))
            return thread.tls;
    return process->leader().tls;
}
user::Credentials PosixService::user_credentials() const
{
    if (const auto bound = caller(); bound.process) return bound.process->credentials();
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
sched::PinnedDescription PosixService::pin(Fd fd) const
{
    if (scheduler_ == nullptr) return {};
    // Only the caller's own thread closes a single-threaded process's slots.
    const auto bound = caller();
    const bool shared = !bound.process || bound.process->live_count() > 1;
    // The pin changes only the description's reference, never the table.
    return scheduler_->pin_descriptor(const_cast<sched::ProcessResources &>(resources()), fd, shared);
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
Result<HeldResolveContext> PosixService::cwd_context(fs::ResolveContext context)
{
    auto &owned = resources();
    for (usize attempt = 0; attempt < cwd_retain_attempts; ++attempt)
    {
        fs::PathHandle start{};
        {
            const sched::FilesLock::Lock::Guard files(owned.files_lock.lock());
            start = {owned.cwd_node, owned.cwd_mount_id};
        }
        context.start = start;
        if (!start.node)
            return HeldResolveContext{context, nullptr};
        const auto retained = start.node->retain();
        bool current{};
        {
            const sched::FilesLock::Lock::Guard files(owned.files_lock.lock());
            current = owned.cwd_node == start.node && owned.cwd_mount_id == start.mount_id;
        }
        if (retained && current)
            return HeldResolveContext{context, start.node};
        if (retained) static_cast<void>(start.node->release());
        else if (current) return retained;
    }
    return Status::busy("current directory kept changing during the path walk setup");
}

// The walk's start is retained for the walk (spec 3.7). A dirfd's node is
// retained through the pin, which keeps the description (and its node
// reference) live meanwhile. The cwd is read under the files lock and
// retained outside it (a provider's retain may enter its gate), then
// rechecked: a chdir in between gives the stale reference back and the read
// is repeated. Provider nodes live in their filesystem's pool while it is
// mounted, and a mount that holds a cwd cannot be detached.
Result<HeldResolveContext> PosixService::path_context(Fd dirfd, std::string_view path)
{
    if (!initialized_ || !vfs_)
        return Status::not_initialized("POSIX service not initialized");
    if (path.empty())
        return Status::not_found("empty pathname");
    fs::ResolveContext context{.credentials = credentials(), .check_search = true};
    if (path.front() == '/')
        return HeldResolveContext{context, nullptr};
    if (dirfd == at_FDCWD)
        return cwd_context(context);
    auto pinned = pin(dirfd);
    auto *entry = pinned.get();
    if (!entry || !entry->used || !entry->node)
        return Status::bad_descriptor("bad directory file descriptor");
    auto metadata = entry->node->query_metadata();
    if (!metadata)
        return metadata.status();
    if (metadata.value().type != fs::NodeType::directory)
        return Status::not_directory("file descriptor is not a directory");
    if (auto status = entry->node->retain(); !status)
        return status;
    context.start = {entry->node, entry->mount_id};
    return HeldResolveContext{context, entry->node};
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
    // The source stays pinned until the target holds its own reference.
    const auto pinned = pin(source);
    if (scheduler_ == nullptr || !pinned || !pinned->used)
        return Status::invalid_argument("bad file descriptor");
    return scheduler_->install_descriptor(resources(), target, pinned.id(), (flags & o_CLOEXEC) != 0);
}
} // namespace ok::posix
