// SchedulerState open file descriptions: allocation, descriptor install and
// close, per-process resource drain, pipes, AF_UNIX sockets and PTY descriptions.
#include "ok/user/native_signal_config.hpp"
#include "ok/sched/scheduler.hpp"
#include "ok/net/native_udp.hpp"
#include "ok/net/native_tcp_stream.hpp"
#include "ok/user/native_shared_files.hpp"
#include "ok/fs/vfs.hpp"
#include "ok/fs/mode_policy.hpp"
#include "resource_drain.hpp"

namespace ok::sched
{

Result<u16> SchedulerState::allocate_description(OpenFileDescription description)
{
    smp::check_exclusive("SchedulerState::allocate_description");
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission is closed");
    if (description.reservation)
        return Status::invalid_argument("new description cannot import prepared reservation authority");
    if (description.unix_queue_references)
        return Status::invalid_argument("new description cannot import UNIX queue ownership");
    // Each bound PTY description owns one endpoint open, so only the prepared
    // raw pair and the PTY bind calls create them.
    if (description.pty_endpoint)
        return Status::invalid_argument("PTY descriptions are bound by a prepared pair or a PTY open");
    if (description.unix_endpoint)
    {
        if (description.node || description.native_udp_handle || description.native_tcp_handle ||
            description.native_shared_file || description.console || description.input_console)
            return Status::invalid_argument("pathname UNIX description cannot combine providers");
    }
    if (description.native_shared_file)
    {
        if (!shared_memory_files_ || !shared_memory_files_->valid(description.native_shared_file))
            return Status::bad_descriptor("shared file description has no canonical owner");
        auto metadata = shared_memory_files_->info(description.native_shared_file);
        if (!metadata || !metadata.value().description)
            return Status::bad_descriptor("shared file description lost its provider hold");
        if (description.node || description.native_udp_handle || description.native_tcp_handle || description.unix_endpoint ||
            description.pipe != invalid_description || description.unix_write_pipe != invalid_description ||
            description.console || description.input_console)
            return Status::invalid_argument("shared file description cannot combine providers");
    }
    if (description.native_tcp_handle)
    {
        if (!native_tcp_service_ || !native_tcp_service_->valid(description.native_tcp_handle))
            return Status::bad_descriptor("TCP description has no canonical stream owner");
        if (description.native_udp_handle || description.native_shared_file || description.unix_endpoint || description.node || description.pipe != invalid_description ||
            description.unix_write_pipe != invalid_description || description.console || description.input_console)
            return Status::invalid_argument("TCP description cannot combine providers");
    }
    if (description.native_udp_handle && !native_udp_service_)
        return Status::not_initialized("native UDP description has no endpoint owner");
    // The provider owners were asked above; the table itself under ofd_table.
    const OfdTableLock::Guard table(ofd_table_);
    if (description.unix_endpoint)
    {
        const auto &endpoint = unix_endpoints_[description.unix_endpoint.slot];
        if (!endpoint.used || endpoint.generation != description.unix_endpoint.generation ||
            endpoint.description != invalid_description)
            return Status::bad_descriptor("pathname UNIX description has no reserved endpoint");
    }
    for (const auto &existing : descriptions_)
    {
        if (description.native_shared_file && existing.used && existing.native_shared_file == description.native_shared_file)
            return Status::already_exists("shared file owner already belongs to an open description");
        if (description.native_tcp_handle && existing.used && existing.native_tcp_handle == description.native_tcp_handle)
            return Status::already_exists("TCP owner already belongs to an open description");
    }
    for (usize i = 3; i < descriptions_.size(); ++i)
        if (!descriptions_[i].used)
        {
            description.used = true;
            description.references = 0;
            descriptions_[i] = description;
            if (description.unix_endpoint)
                unix_endpoints_[description.unix_endpoint.slot].description = static_cast<u16>(i);
            return static_cast<u16>(i);
        }
    return Status::system_file_limit("open file description table full");
}
namespace
{
using OfdGuard = OfdTableLock::Guard;
using PipeGuard = PipeLock::Guard;
using FilesGuard = FilesLock::Lock::Guard;

// The descriptions one files-lock hold took out of their slots, released
// after the lock is dropped (spec 3.6). A pass over more slots than fit stops
// at its cursor, releases the batch and continues there (the Linux
// close_range shape); exec's close-on-exec pass and close_range use it.
template <usize Capacity> struct RetireList
{
    std::array<u16, Capacity> descriptions{};
    std::array<i32, Capacity> fds{};
    usize count{};
    [[nodiscard]] bool full() const { return count == Capacity; }
    void push(i32 fd, u16 description)
    {
        fds[count] = fd;
        descriptions[count++] = description;
    }
};
inline constexpr usize retire_batch = 32;
} // namespace

Status PinnedDescription::reset()
{
    auto *owner = std::exchange(owner_, nullptr);
    const auto id = std::exchange(id_, invalid_description);
    open_ = nullptr;
    if (!owner || !std::exchange(referenced_, false)) return {};
    return owner->release_description(id);
}
PinnedDescription::~PinnedDescription()
{
    // The pin may hold the last reference (a sibling closed the slot): a
    // failed provider, socket or PTY release is recorded like a close's.
    auto *owner = owner_;
    const auto id = id_;
    if (auto status = reset(); !status && owner) owner->record_resource_failure(status, -1, id, 0, false);
}

void SchedulerState::count_pipe_endpoints(const OpenFileDescription &description, int delta)
{
    smp::check_held(ofd_table_, "SchedulerState::count_pipe_endpoints");
    const auto adjust = [&](u16 pipe, bool writer) {
        if (pipe >= pipes_.size()) return;
        const PipeGuard guard(pipe_locks_[pipe]);
        auto &count = writer ? pipes_[pipe].writers : pipes_[pipe].readers;
        count = static_cast<u16>(count + delta);
    };
    if (description.readable) adjust(description.pipe, false);
    if (description.writable)
        adjust(description.unix_write_pipe != invalid_description ? description.unix_write_pipe : description.pipe, true);
}

Status SchedulerState::install_descriptor(ProcessResources &resources, i32 fd, u16 description, bool close_on_exec)
{
    smp::check_exclusive("SchedulerState::install_descriptor");
    smp::check_drained(resources_owner(resources), "SchedulerState::install_descriptor");
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission is closed");
    auto *slot = resources.files.entry(fd);
    if (slot == nullptr || open_description(description) == nullptr)
        return Status::invalid_argument("invalid descriptor binding");
    {
        const FilesGuard files(resources.files_lock.lock());
        if (slot->reservation)
            return Status::busy("descriptor belongs to a prepared operation");
    }
    {
        const OfdGuard table(ofd_table_);
        if (const auto *open = open_description(description); !open || open->reservation)
            return Status::busy("canonical description belongs to a prepared operation");
    }
    auto token = next_descriptor_token(); if (!token) return token.status();
    // Retain first: dup2 may replace the last other reference to this description.
    if (auto status = retain_description(description); !status.ok())
        return status;
    FileDescriptor previous{};
    {
        const FilesGuard files(resources.files_lock.lock());
        if (!slot->reservation)
        {
            previous = *slot;
            *slot = FileDescriptor{.used = true, .close_on_exec = close_on_exec, .description = description,
                .installation = token.value()};
        }
        else
            previous.reservation = slot->reservation;
    }
    if (previous.reservation)
    {
        record_resource_failure(release_description(description), fd, description, 0, false);
        return Status::busy("descriptor belongs to a prepared operation");
    }
    if (previous.used)
        static_cast<void>(retire_closed_slot(resources, fd, previous.description));
    return Status::success();
}
Status SchedulerState::retain_description(u16 id)
{
    smp::check_exclusive("SchedulerState::retain_description");
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission is closed");
    fs::Node *first{};
    {
        const OfdGuard table(ofd_table_);
        auto *open = open_description(id);
        if (!open)
            return Status::bad_descriptor("invalid open description");
        if (open->reservation)
            return Status::busy("unpublished description cannot be externally retained");
        if (open->references == ~usize{0})
            return Status::overflow("open description reference limit exceeded");
        if (open->references || !open->node)
        {
            if (!open->references++) count_pipe_endpoints(*open, +1);
            return Status::success();
        }
        first = open->node;
    }
    // The first reference of a node description retains the node, outside the
    // table lock: a provider's retain may enter its gate. A description
    // without references is reachable only by the operation that allocated
    // it, so nothing retains it meanwhile.
    if (auto status = first->retain(); !status.ok())
        return status;
    {
        const OfdGuard table(ofd_table_);
        auto *open = open_description(id);
        if (open && open->node == first)
        {
            if (!open->references++) count_pipe_endpoints(*open, +1);
            return Status::success();
        }
    }
    // The description changed meanwhile: give the node reference back,
    // again outside the table lock.
    smp::count_check(smp::KernelCheck::audit_repairs, "description changed during its first retain");
    record_resource_failure(first->release(), -1, id, 0, false);
    return Status::busy("open description changed during its first retain");
}
Status SchedulerState::release_description(u16 id)
{
    smp::check_exclusive("SchedulerState::release_description");
    // Locked phase: the reference, the endpoint counts and the slot.
    OpenFileDescription retired{};
    bool last{};
    u16 pipe = invalid_description, unix_write_pipe = invalid_description;
    {
        const OfdGuard table(ofd_table_);
        auto *open = open_description(id);
        if (!open || !open->references)
            return Status::bad_descriptor("open description has no reference to release");
        --open->references;
        // Only a last reference changes what the pipes' peers see (their
        // endpoint counts): a pin or a duplicate going wakes nobody.
        if (!open->references)
        {
            pipe = open->pipe;
            unix_write_pipe = open->unix_write_pipe;
            count_pipe_endpoints(*open, -1);
        }
        if (open->references == 0 && id >= 3)
        {
            // Consume the exact external edge before a fallible provider callback.
            // A callback cannot resurrect this old OFD or erase a newly reused slot.
            retired = *open;
            last = true;
            *open = {};
        }
    }
    Status result = Status::success();
    if (last)
    {
        const auto node = retired.node;
        const auto endpoint = retired.native_udp_handle;
        const auto stream = retired.native_tcp_handle;
        const auto shared_file = retired.native_shared_file;
        const auto unix_endpoint = retired.unix_endpoint;
        const auto pty_endpoint = retired.pty_endpoint;
        if (pty_endpoint)
        {
            // One open per final release. The master's last open hangs a ptmx
            // slave up and queues SIGHUP and SIGCONT for the slave session's
            // leader (Linux tty_signal_session_leader) inside the table.
            const auto retired_pty = pty_transport_.retire(pty_endpoint);
            if (!retired_pty) result = retired_pty.status();
            // Linux devpts_pty_kill: /dev/pts/N goes with the master's last open.
            else if (pty_endpoint.side == private_pty::Side::master && retired_pty.value().master_retired)
            {
                remember_hangup(retired_pty.value().hangup_leader, retired_pty.value().hangup_foreground,
                                static_cast<u16>(pty_endpoint.slot + 1));
                terminal_namespace_.remove(pty_endpoint.slot, pty_endpoint.generation);
                wake_waiters(ThreadWaitReason::job_control,
                             job_control_key({static_cast<u16>(pty_endpoint.slot + 1), pty_endpoint.generation}));
            }
            wake_pty(pty_endpoint, private_pty::wake_all);
        }
        // Unlocked phase: provider, socket path, shared-file, TCP and UDP
        // releases run with no scheduler lock held.
        if (unix_endpoint)
            result = release_unix_endpoint(unix_endpoint);
        if (node)
        {
            const auto released = node->release();
            if (result && !released) result = released;
        }
        if (shared_file)
        {
            const auto released = shared_memory_files_ ? shared_memory_files_->release_description(shared_file)
                : Status::not_initialized("shared file owner is unavailable");
            if (result && !released) result = released;
        }
        if (stream)
        {
            const auto released = native_tcp_service_ ? native_tcp_service_->close(stream)
                : Status::not_initialized("TCP endpoint owner is unavailable");
            if (result && !released) result = released;
        }
        if (endpoint)
        {
            const auto released = native_udp_service_ ? native_udp_service_->close(endpoint)
                                                       : Status::not_initialized("UDP endpoint owner is unavailable");
            if (result && !released) result = released;
        }
    }
    auto wake_and_retire = [&](u16 index) {
        if (index >= pipes_.size()) return;
        wake_waiters(ThreadWaitReason::pipe_read, index);
        wake_waiters(ThreadWaitReason::pipe_write, index);
        if (!pipe_has_endpoint(index, false) && !pipe_has_endpoint(index, true))
        {
            const auto released = retire_pipe(index);
            if (result && !released) result = released;
        }
    };
    if (pipe < pipes_.size())
    {
        wake_and_retire(pipe);
        if (unix_write_pipe != pipe)
            wake_and_retire(unix_write_pipe);
    }
    // The rights collection runs only while rights are in flight (spec 4.13).
    if (!collecting_unix_rights_ && rights_in_flight())
    {
        const auto collected = collect_unix_rights();
        if (result && !collected) result = collected;
    }
    return result;
}
Status SchedulerState::retire_closed_slot(ProcessResources &resources, i32 fd, u16 description)
{
    if (native_tcp_service_)
    {
        bool stream{};
        {
            const OfdGuard table(ofd_table_);
            const auto *open = open_description(description);
            stream = open && open->native_tcp_handle;
        }
        if (stream) native_tcp_service_->descriptor_closed(&resources, fd, description);
    }
    return release_description(description);
}
Status SchedulerState::close_descriptor(ProcessResources &resources, i32 fd)
{
    smp::check_exclusive("SchedulerState::close_descriptor");
    smp::check_drained(resources_owner(resources), "SchedulerState::close_descriptor");
    auto *slot = resources.files.entry(fd);
    u16 description = invalid_description;
    {
        const FilesGuard files(resources.files_lock.lock());
        if (slot == nullptr || !slot->used)
            return Status::invalid_argument("bad file descriptor");
        description = slot->description;
        *slot = {};
    }
    return retire_closed_slot(resources, fd, description);
}
template <typename Select>
Status SchedulerState::close_descriptors(ProcessResources &resources, i32 first, i32 last, Select select)
{
    smp::check_exclusive("SchedulerState::close_descriptors");
    if (first < 0) first = 0;
    if (last >= static_cast<i32>(max_process_fds)) last = static_cast<i32>(max_process_fds) - 1;
    Status result{};
    for (i32 cursor = first; cursor <= last;)
    {
        RetireList<retire_batch> batch{};
        {
            const FilesGuard files(resources.files_lock.lock());
            for (; cursor <= last && !batch.full(); ++cursor)
            {
                auto *slot = resources.files.entry(cursor);
                if (!slot || !slot->used || !select(*slot)) continue;
                batch.push(cursor, slot->description);
                *slot = {};
            }
        }
        for (usize index = 0; index < batch.count; ++index)
        {
            const auto released = retire_closed_slot(resources, batch.fds[index], batch.descriptions[index]);
            if (result && !released) result = released;
        }
    }
    return result;
}
Status SchedulerState::close_descriptor_range(ProcessResources &resources, i32 first, i32 last)
{
    return close_descriptors(resources, first, last, [](const FileDescriptor &) { return true; });
}
Status SchedulerState::close_on_exec_descriptors(ProcessResources &resources)
{
    return close_descriptors(resources, 0, static_cast<i32>(max_process_fds) - 1,
                             [](const FileDescriptor &slot) { return slot.close_on_exec; });
}
PinnedDescription SchedulerState::pin_descriptor(ProcessResources &resources, i32 fd, bool shared)
{
    u16 id = invalid_description;
    {
        const FilesGuard files(resources.files_lock.lock());
        const auto *slot = resources.files.entry(fd);
        if (!slot || !slot->used) return {};
        id = slot->description;
        if (!shared) return PinnedDescription{*this, id, open_description(id), false};
        // ofd_table nests inside files: the slot cannot change before the
        // reference is taken.
        const OfdGuard table(ofd_table_);
        auto *open = open_description(id);
        if (!open || !open->references || open->references == ~usize{0}) return {};
        ++open->references;
        return PinnedDescription{*this, id, open, true};
    }
}
Result<SchedulerState::DescriptorSlot> SchedulerState::reserve_descriptor_slot(ProcessResources &resources, i32 minimum)
{
    if (minimum < 0) return Status::invalid_argument("negative minimum descriptor");
    auto token = next_descriptor_token(); if (!token) return token.status();
    const FilesGuard files(resources.files_lock.lock());
    for (i32 fd = minimum; fd < static_cast<i32>(max_process_fds); ++fd)
        if (!resources.files.contains(fd))
        {
            resources.files.entry(fd)->reservation = token.value();
            return DescriptorSlot{fd, token.value()};
        }
    return Status::process_file_limit("file descriptor table full");
}
Status SchedulerState::install_reserved_descriptor(ProcessResources &resources, DescriptorSlot &reserved,
                                                   u16 description, bool close_on_exec)
{
    smp::check_exclusive("SchedulerState::install_reserved_descriptor");
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission is closed");
    auto *slot = resources.files.entry(reserved.fd);
    if (!reserved || !slot) return Status::invalid_argument("invalid descriptor reservation");
    {
        const OfdGuard table(ofd_table_);
        if (const auto *open = open_description(description); !open || open->reservation)
            return Status::busy("canonical description belongs to a prepared operation");
    }
    if (auto status = retain_description(description); !status) return status;
    {
        const FilesGuard files(resources.files_lock.lock());
        if (!slot->used && slot->reservation == reserved.token)
        {
            *slot = FileDescriptor{.used = true, .close_on_exec = close_on_exec, .description = description,
                .installation = reserved.token};
            reserved = {};
            return Status::success();
        }
    }
    record_resource_failure(release_description(description), reserved.fd, description, 0, false);
    return Status::busy("descriptor reservation was lost before publication");
}
void SchedulerState::cancel_descriptor_slot(ProcessResources &resources, DescriptorSlot &reserved)
{
    if (!reserved) return;
    const FilesGuard files(resources.files_lock.lock());
    if (auto *slot = resources.files.entry(reserved.fd); slot && !slot->used && slot->reservation == reserved.token)
        *slot = {};
    reserved = {};
}
Result<WaitTicket> SchedulerState::arm_pipe_wait(ThreadWaitReason reason, u16 pipe, usize needed)
{
    if (pipe >= pipes_.size()) return Status::invalid_argument("invalid pipe");
    const PipeGuard guard(pipe_locks_[pipe]);
    auto ticket = register_current_wait(reason, pipe);
    if (!ticket) return ticket;
    const auto &buffer = pipes_[pipe];
    const bool ready = !buffer.used || (reason == ThreadWaitReason::pipe_read
                                            ? buffer.size != 0 || !buffer.writers
                                            : pipe_capacity - buffer.size >= needed || !buffer.readers);
    if (ready)
        static_cast<void>(complete_wait(ticket.value(), WaitCompletion::ready, WakeSource::thread));
    return ticket;
}
void SchedulerState::release_resources(ProcessResources &resources)
{
    static_cast<void>(drain_resources(resources));
}
void SchedulerState::record_resource_failure(Status status, i32 fd, u16 description, usize mount_id, bool cwd)
{
    if (status) return;
    count_resource_event(resource_drain_report_.failed_releases);
    if (resource_drain_report_.first_failure)
    {
        resource_drain_report_.first_failure = status;
        resource_drain_report_.first_failed_fd = fd;
        resource_drain_report_.first_failed_description = description;
        resource_drain_report_.first_failed_mount_id = mount_id;
        resource_drain_report_.first_failed_cwd = cwd;
    }
}
Status SchedulerState::consume_resources(ProcessResources &resources)
{
    if (resources.unix_receive_operations || resources.prepared_descriptor_operations)
        return Status::busy("prepared descriptor operation still owns process resources");
    Status result{};
    for (i32 fd = 0; fd < static_cast<i32>(max_process_fds); ++fd)
        if (resources.files.contains(fd))
        {
            const auto description = resources.files.entry(fd)->description;
            const auto *open = open_description(description);
            const auto mount_id = open ? open->mount_id : 0;
            count_resource_event(resource_drain_report_.descriptors_consumed);
            const auto released = close_descriptor(resources, fd);
            record_resource_failure(released, fd, description, mount_id, false);
            if (result && !released) result = released;
        }
    resources.memory = {};
    resources.mappings = {};
    const auto cwd = resources.cwd_node;
    const auto mount_id = resources.cwd_mount_id;
    resources.cwd_node = nullptr;
    resources.cwd_mount_id = 0;
    if (cwd)
    {
        count_resource_event(resource_drain_report_.cwd_consumed);
        const auto released = cwd->release();
        record_resource_failure(released, -1, invalid_description, mount_id, true);
        if (result && !released) result = released;
    }
    return result;
}
Status SchedulerState::drain_resources(ProcessResources &resources)
{
    if (resource_drain_report_.active)
        return Status::busy("scheduler resource cleanup is already active");
    resource_drain_report_.active = true;
    ResourceDrainAdmission admission{resource_drain_report_.active};
    count_resource_event(resource_drain_report_.operations);
    return consume_resources(resources);
}

bool SchedulerState::pipe_endpoint_audit(u16 pipe, bool writable) const
{
    for (const auto &description : descriptions_)
        if (description.used && description.references != 0 &&
            (writable ? (description.unix_write_pipe != invalid_description
                             ? description.unix_write_pipe : description.pipe)
                      : description.pipe) == pipe &&
            (writable ? description.writable : description.readable))
            return true;
    return false;
}
// The caller holds the pipe lock, or reads under the kernel lock.
bool SchedulerState::pipe_has_endpoint(u16 pipe, bool writable) const
{
    if (pipe >= pipes_.size()) return false;
    const bool counted = (writable ? pipes_[pipe].writers : pipes_[pipe].readers) != 0;
    // The scan reads the table without ofd_table: valid while pipes stay
    // under the kernel lock (debug kernels only).
    if constexpr (smp::lock_debug_enabled)
        if (pipes_[pipe].used && pipe_endpoint_audit(pipe, writable) != counted)
        {
            smp::count_check(smp::KernelCheck::audit_repairs, "pipe endpoint count differs from the table");
            return !counted;
        }
    return counted;
}
usize SchedulerState::rights_in_flight() const
{
    usize total{};
    for (const auto &buffer : pipes_) total += buffer.rights_count;
    return total;
}
Result<PipeReadiness> SchedulerState::pipe_readiness(u16 pipe) const
{
    if (pipe >= pipes_.size())
        return Status::bad_descriptor("invalid pipe");
    const PipeGuard guard(pipe_locks_[pipe]);
    if (!pipes_[pipe].used)
        return Status::bad_descriptor("invalid pipe");
    return PipeReadiness{.buffered = pipes_[pipe].size, .available = pipe_capacity - pipes_[pipe].size,
                         .readers = pipe_has_endpoint(pipe, false), .writers = pipe_has_endpoint(pipe, true)};
}
Status SchedulerState::claim_pipes(std::span<u16> claimed)
{
    const OfdGuard table(ofd_table_);
    usize found{};
    for (usize i = 0; i < pipes_.size() && found != claimed.size(); ++i)
        if (!pipes_[i].used) claimed[found++] = static_cast<u16>(i);
    if (found != claimed.size())
        return Status::system_file_limit("pipe table full");
    for (const auto pipe : claimed)
    {
        const PipeGuard guard(pipe_locks_[pipe]);
        pipes_[pipe] = PipeBuffer{.used = true};
    }
    return {};
}
void SchedulerState::unclaim_pipe(u16 pipe)
{
    if (pipe >= pipes_.size()) return;
    const OfdGuard table(ofd_table_);
    const PipeGuard guard(pipe_locks_[pipe]);
    if (!pipes_[pipe].readers && !pipes_[pipe].writers && !pipes_[pipe].rights_count) pipes_[pipe] = {};
}
void SchedulerState::discard_description(u16 id)
{
    const OfdGuard table(ofd_table_);
    if (auto *open = open_description(id); open && !open->references) *open = {};
}
Result<std::array<i32, 2>> SchedulerState::create_pipe(ProcessResources &resources, bool close_on_exec, u32 status_flags)
{
    if (status_flags & 0x3u)
        return Status::invalid_argument("pipe status flags contain an access mode");
    // Global pipe/file resources precede descriptor-slot admission, as in
    // Linux create_pipe_files followed by get_unused_fd_flags. No state is
    // published until every bounded capacity check succeeds.
    {
        const OfdGuard table(ofd_table_);
        usize count = 0;
        for (usize i = 3; i < descriptions_.size(); ++i)
            if (!descriptions_[i].used)
                ++count;
        if (count < 2)
            return Status::system_file_limit("pipe needs two open descriptions");
    }
    std::array<u16, 1> pipe{invalid_description};
    if (auto status = claim_pipes(pipe); !status) return status;
    std::array<DescriptorSlot, 2> slots{};
    for (auto &slot : slots)
    {
        auto reserved = reserve_descriptor_slot(resources);
        if (!reserved)
        {
            for (auto &taken : slots) cancel_descriptor_slot(resources, taken);
            unclaim_pipe(pipe[0]);
            return reserved.status().code() == StatusCode::process_file_limit
                ? Status::process_file_limit("pipe needs two free descriptors") : reserved.status();
        }
        slot = reserved.value();
    }
    const std::array<i32, 2> fds{slots[0].fd, slots[1].fd};
    auto reader = allocate_description(OpenFileDescription{.used = true, .readable = true, .flags = status_flags, .pipe = pipe[0]});
    auto writer = allocate_description(OpenFileDescription{.used = true, .writable = true, .flags = status_flags | 1u, .pipe = pipe[0]});
    if (!reader || !writer)
    {
        if (reader) discard_description(reader.value());
        if (writer) discard_description(writer.value());
        for (auto &taken : slots) cancel_descriptor_slot(resources, taken);
        unclaim_pipe(pipe[0]);
        return !reader ? reader.status() : writer.status();
    }
    static_cast<void>(install_reserved_descriptor(resources, slots[0], reader.value(), close_on_exec));
    static_cast<void>(install_reserved_descriptor(resources, slots[1], writer.value(), close_on_exec));
    return fds;
}
Result<std::array<i32, 2>> SchedulerState::create_unix_socketpair(ProcessResources &resources,
                                                                    bool close_on_exec, u32 status_flags)
{
    if (status_flags & 0x3u)
        return Status::invalid_argument("socket status flags contain an access mode");
    {
        const OfdGuard table(ofd_table_);
        usize free_descriptions = 0;
        for (usize i = 3; i < descriptions_.size(); ++i)
            if (!descriptions_[i].used) ++free_descriptions;
        if (free_descriptions < 2)
            return Status::system_file_limit("socket pair needs two open descriptions");
    }
    std::array<u16, 2> buffers{invalid_description, invalid_description};
    if (auto status = claim_pipes(buffers); !status)
        return Status::system_file_limit("socket pair needs two stream buffers");
    const auto unclaim = [&] {
        unclaim_pipe(buffers[0]);
        unclaim_pipe(buffers[1]);
    };
    std::array<DescriptorSlot, 2> slots{};
    for (auto &slot : slots)
    {
        auto reserved = reserve_descriptor_slot(resources);
        if (!reserved)
        {
            for (auto &taken : slots) cancel_descriptor_slot(resources, taken);
            unclaim();
            return reserved.status().code() == StatusCode::process_file_limit
                ? Status::process_file_limit("socket pair needs two free descriptors") : reserved.status();
        }
        slot = reserved.value();
    }
    const std::array<i32, 2> fds{slots[0].fd, slots[1].fd};
    auto first = allocate_description(OpenFileDescription{.used = true, .readable = true, .writable = true,
        .flags = 2u | status_flags, .pipe = buffers[0], .unix_write_pipe = buffers[1]});
    auto second = allocate_description(OpenFileDescription{.used = true, .readable = true, .writable = true,
        .flags = 2u | status_flags, .pipe = buffers[1], .unix_write_pipe = buffers[0]});
    if (!first || !second)
    {
        if (first) discard_description(first.value());
        if (second) discard_description(second.value());
        for (auto &taken : slots) cancel_descriptor_slot(resources, taken);
        unclaim();
        return !first ? first.status() : second.status();
    }
    if (auto status = install_reserved_descriptor(resources, slots[0], first.value(), close_on_exec); !status)
    {
        discard_description(first.value());
        discard_description(second.value());
        for (auto &taken : slots) cancel_descriptor_slot(resources, taken);
        unclaim();
        return status;
    }
    if (auto status = install_reserved_descriptor(resources, slots[1], second.value(), close_on_exec); !status)
    {
        cancel_descriptor_slot(resources, slots[1]);
        static_cast<void>(close_descriptor(resources, fds[0]));
        discard_description(second.value());
        unclaim();
        return status;
    }
    return fds;
}
Result<usize> SchedulerState::read_pipe(u16 pipe, std::span<std::byte> out, bool nonblocking)
{
    smp::check_exclusive("SchedulerState::read_pipe");
    if (pipe >= pipes_.size())
        return Status::invalid_argument("invalid pipe");
    UnixRightsRecord detached{};
    usize count{};
    {
        const PipeGuard guard(pipe_locks_[pipe]);
        auto &buffer = pipes_[pipe];
        if (!buffer.used)
            return Status::invalid_argument("invalid pipe");
        if (out.empty())
            return usize{0};
        if (buffer.size == 0)
        {
            if (!pipe_has_endpoint(pipe, true))
                return usize{0};
            // A1: armed under the pipe lock that guards the emptiness.
            if (!nonblocking)
                static_cast<void>(block_current(ThreadWaitReason::pipe_read, pipe));
            return Status::would_block("pipe has no buffered data");
        }
        if (buffer.revision == ~u64{0})
            return Status::overflow("stream revision cannot wrap");
        u16 record = invalid_description;
        count = unix_read_count(pipe, out.size(), record);
        for (usize i = 0; i < count; ++i)
            out[i] = buffer.data[(buffer.head + i) % pipe_capacity];
        detached = consume_unix_bytes_locked(pipe, count, record);
    }
    wake_waiters(ThreadWaitReason::pipe_write, pipe);
    if (detached.used)
        record_resource_failure(release_unix_record(detached), -1, invalid_description, 0, false);
    return count;
}
Result<usize> SchedulerState::write_pipe(u16 pipe, std::span<const std::byte> in, bool nonblocking,
                                          bool atomic_request)
{
    const bool atomic = atomic_request && in.size() <= pipe_capacity;
    auto result = write_pipe_fragment(pipe, in, atomic);
    if (!result && result.status().code() == StatusCode::would_block && !nonblocking)
        static_cast<void>(arm_pipe_wait(ThreadWaitReason::pipe_write, pipe, atomic ? in.size() : 1));
    return result;
}
Result<usize> SchedulerState::write_pipe_fragment(u16 pipe, std::span<const std::byte> in, bool atomic_request)
{
    smp::check_exclusive("SchedulerState::write_pipe_fragment");
    if (pipe >= pipes_.size())
        return Status::invalid_argument("invalid pipe");
    usize count{};
    {
        const PipeGuard guard(pipe_locks_[pipe]);
        auto &buffer = pipes_[pipe];
        if (!buffer.used)
            return Status::invalid_argument("invalid pipe");
        if (in.empty())
            return usize{0};
        if (!pipe_has_endpoint(pipe, false))
            return Status::broken_pipe("pipe has no readers");
        if (buffer.revision == ~u64{0})
            return Status::overflow("stream revision cannot wrap");
        const auto available = pipe_capacity - buffer.size;
        if (available == 0 || (atomic_request && in.size() > available))
            return Status::would_block("pipe buffer is full");
        count = in.size() < available ? in.size() : available;
        for (usize i = 0; i < count; ++i)
            buffer.data[(buffer.head + buffer.size + i) % pipe_capacity] = in[i];
        buffer.size += count;
        ++buffer.revision;
    }
    // A2: the data is visible under the pipe lock before the channel is woken.
    wake_waiters(ThreadWaitReason::pipe_read, pipe);
    return count;
}
#include "unix_rights.inc"
#include "unix_path.inc"
#include "descriptor_reservation.inc"
#include "pty_pair.inc"
#include "terminal_open.inc"

} // namespace ok::sched
