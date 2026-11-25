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
        const auto handle = description.unix_endpoint;
        const auto &endpoint = unix_endpoints_[handle.slot];
        if (!endpoint.used || endpoint.generation != handle.generation || endpoint.description != invalid_description)
            return Status::bad_descriptor("pathname UNIX description has no reserved endpoint");
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
        for (const auto &existing : descriptions_)
            if (existing.used && existing.native_shared_file == description.native_shared_file)
                return Status::already_exists("shared file owner already belongs to an open description");
    }
    if (description.native_tcp_handle)
    {
        if (!native_tcp_service_ || !native_tcp_service_->valid(description.native_tcp_handle))
            return Status::bad_descriptor("TCP description has no canonical stream owner");
        if (description.native_udp_handle || description.native_shared_file || description.unix_endpoint || description.node || description.pipe != invalid_description ||
            description.unix_write_pipe != invalid_description || description.console || description.input_console)
            return Status::invalid_argument("TCP description cannot combine providers");
        for (const auto &existing : descriptions_)
            if (existing.used && existing.native_tcp_handle == description.native_tcp_handle)
                return Status::already_exists("TCP owner already belongs to an open description");
    }
    if (description.native_udp_handle && !native_udp_service_)
        return Status::not_initialized("native UDP description has no endpoint owner");
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
Status SchedulerState::install_descriptor(ProcessResources &resources, i32 fd, u16 description, bool close_on_exec)
{
    smp::check_exclusive("SchedulerState::install_descriptor");
    smp::check_drained(resources_owner(resources), "SchedulerState::install_descriptor");
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission is closed");
    auto *slot = resources.files.entry(fd);
    auto *open = open_description(description);
    if (slot == nullptr || open == nullptr)
        return Status::invalid_argument("invalid descriptor binding");
    if (slot->reservation)
        return Status::busy("descriptor belongs to a prepared operation");
    if (open->reservation)
        return Status::busy("canonical description belongs to a prepared operation");
    auto token = next_descriptor_token(); if (!token) return token.status();
    // Retain first: dup2 may replace the last other reference to this description.
    if (auto status = retain_description(description); !status.ok())
        return status;
    if (slot->used)
        static_cast<void>(close_descriptor(resources, fd));
    *slot = FileDescriptor{.used = true, .close_on_exec = close_on_exec, .description = description,
        .installation = token.value()};
    return Status::success();
}
Status SchedulerState::retain_description(u16 id)
{
    smp::check_exclusive("SchedulerState::retain_description");
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission is closed");
    auto *open = open_description(id);
    if (!open)
        return Status::bad_descriptor("invalid open description");
    if (open->reservation)
        return Status::busy("unpublished description cannot be externally retained");
    if (open->references == ~usize{0})
        return Status::overflow("open description reference limit exceeded");
    if (open->references == 0 && open->node)
        if (auto status = open->node->retain(); !status.ok())
            return status;
    ++open->references;
    return Status::success();
}
Status SchedulerState::release_description(u16 id)
{
    smp::check_exclusive("SchedulerState::release_description");
    auto *open = open_description(id);
    if (!open || !open->references)
        return Status::bad_descriptor("open description has no reference to release");
    Status result = Status::success();
    --open->references;
    const auto pipe = open->pipe;
    const auto unix_write_pipe = open->unix_write_pipe;
    if (open->references == 0 && id >= 3)
    {
        const auto node = open->node;
        const auto endpoint = open->native_udp_handle;
        const auto stream = open->native_tcp_handle;
        const auto shared_file = open->native_shared_file;
        const auto unix_endpoint = open->unix_endpoint;
        const auto pty_endpoint = open->pty_endpoint;
        // Consume the exact external edge before a fallible provider callback.
        // A callback cannot resurrect this old OFD or erase a newly reused slot.
        *open = {};
        if (pty_endpoint)
        {
            // One open per final release. The master's last open hangs a ptmx
            // slave up and queues SIGHUP and SIGCONT for the slave session's
            // leader (Linux tty_signal_session_leader) inside the table.
            const auto retired = pty_transport_.retire(pty_endpoint);
            if (!retired) result = retired.status();
            // Linux devpts_pty_kill: /dev/pts/N goes with the master's last open.
            else if (pty_endpoint.side == private_pty::Side::master && retired.value().master_retired)
            {
                remember_hangup(retired.value().hangup_leader, retired.value().hangup_foreground,
                                static_cast<u16>(pty_endpoint.slot + 1));
                terminal_namespace_.remove(pty_endpoint.slot, pty_endpoint.generation);
                wake_waiters(ThreadWaitReason::job_control,
                             job_control_key({static_cast<u16>(pty_endpoint.slot + 1), pty_endpoint.generation}));
            }
            wake_pty(pty_endpoint, private_pty::wake_all);
        }
        if (unix_endpoint)
            result = release_unix_endpoint(unix_endpoint);
        if (node)
        {
            const auto retired = node->release();
            if (result && !retired) result = retired;
        }
        if (shared_file)
        {
            const auto retired = shared_memory_files_ ? shared_memory_files_->release_description(shared_file)
                : Status::not_initialized("shared file owner is unavailable");
            if (result && !retired) result = retired;
        }
        if (stream)
        {
            const auto retired = native_tcp_service_ ? native_tcp_service_->close(stream)
                : Status::not_initialized("TCP endpoint owner is unavailable");
            if (result && !retired) result = retired;
        }
        if (endpoint)
        {
            const auto retired = native_udp_service_ ? native_udp_service_->close(endpoint)
                                                      : Status::not_initialized("UDP endpoint owner is unavailable");
            if (result && !retired) result = retired;
        }
    }
    auto wake_and_retire = [&](u16 index) {
        if (index >= pipes_.size()) return;
        wake_waiters(ThreadWaitReason::pipe_read, index);
        wake_waiters(ThreadWaitReason::pipe_write, index);
        if (!pipe_has_endpoint(index, false) && !pipe_has_endpoint(index, true))
        {
            const auto retired = retire_pipe(index);
            if (result && !retired) result = retired;
        }
    };
    if (pipe < pipes_.size())
    {
        wake_and_retire(pipe);
        if (unix_write_pipe != pipe)
            wake_and_retire(unix_write_pipe);
    }
    if (!collecting_unix_rights_)
    {
        const auto collected = collect_unix_rights();
        if (result && !collected) result = collected;
    }
    return result;
}
Status SchedulerState::close_descriptor(ProcessResources &resources, i32 fd)
{
    smp::check_exclusive("SchedulerState::close_descriptor");
    smp::check_drained(resources_owner(resources), "SchedulerState::close_descriptor");
    auto *slot = resources.files.entry(fd);
    if (slot == nullptr || !slot->used)
        return Status::invalid_argument("bad file descriptor");
    const auto description = slot->description;
    *slot = {};
    const auto *open = open_description(description);
    if (open && open->native_tcp_handle && native_tcp_service_)
        native_tcp_service_->descriptor_closed(&resources, fd, description);
    return release_description(description);
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

bool SchedulerState::pipe_has_endpoint(u16 pipe, bool writable) const
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
Result<PipeReadiness> SchedulerState::pipe_readiness(u16 pipe) const
{
    if (pipe >= pipes_.size() || !pipes_[pipe].used)
        return Status::bad_descriptor("invalid pipe");
    return PipeReadiness{.buffered = pipes_[pipe].size, .available = pipe_capacity - pipes_[pipe].size,
                         .readers = pipe_has_endpoint(pipe, false), .writers = pipe_has_endpoint(pipe, true)};
}
Result<std::array<i32, 2>> SchedulerState::create_pipe(ProcessResources &resources, bool close_on_exec, u32 status_flags)
{
    if (status_flags & 0x3u)
        return Status::invalid_argument("pipe status flags contain an access mode");
    // Global pipe/file resources precede descriptor-slot admission, as in
    // Linux create_pipe_files followed by get_unused_fd_flags. No state is
    // published until every bounded capacity check succeeds.
    usize count = 0;
    for (usize i = 3; i < descriptions_.size(); ++i)
        if (!descriptions_[i].used)
            ++count;
    if (count < 2)
        return Status::system_file_limit("pipe needs two open descriptions");
    u16 pipe = invalid_description;
    for (usize i = 0; i < pipes_.size(); ++i)
        if (!pipes_[i].used)
        {
            pipe = static_cast<u16>(i);
            break;
        }
    if (pipe == invalid_description)
        return Status::system_file_limit("pipe table full");
    std::array<i32, 2> fds{};
    count = 0;
    for (i32 fd = 0; fd < static_cast<i32>(max_process_fds) && count != 2; ++fd)
        if (!resources.files.contains(fd))
            fds[count++] = fd;
    if (count != 2)
        return Status::process_file_limit("pipe needs two free descriptors");
    pipes_[pipe] = PipeBuffer{.used = true};
    auto reader = allocate_description(OpenFileDescription{.used = true, .readable = true, .flags = status_flags, .pipe = pipe});
    auto writer = allocate_description(OpenFileDescription{.used = true, .writable = true, .flags = status_flags | 1u, .pipe = pipe});
    static_cast<void>(install_descriptor(resources, fds[0], reader.value(), close_on_exec));
    static_cast<void>(install_descriptor(resources, fds[1], writer.value(), close_on_exec));
    return fds;
}
Result<std::array<i32, 2>> SchedulerState::create_unix_socketpair(ProcessResources &resources,
                                                                    bool close_on_exec, u32 status_flags)
{
    if (status_flags & 0x3u)
        return Status::invalid_argument("socket status flags contain an access mode");
    usize free_descriptions = 0;
    for (usize i = 3; i < descriptions_.size(); ++i)
        if (!descriptions_[i].used) ++free_descriptions;
    if (free_descriptions < 2)
        return Status::system_file_limit("socket pair needs two open descriptions");
    std::array<u16, 2> buffers{invalid_description, invalid_description};
    usize found = 0;
    for (usize i = 0; i < pipes_.size() && found != 2; ++i)
        if (!pipes_[i].used) buffers[found++] = static_cast<u16>(i);
    if (found != 2)
        return Status::system_file_limit("socket pair needs two stream buffers");
    std::array<i32, 2> fds{};
    found = 0;
    for (i32 fd = 0; fd < static_cast<i32>(max_process_fds) && found != 2; ++fd)
        if (!resources.files.contains(fd)) fds[found++] = fd;
    if (found != 2)
        return Status::process_file_limit("socket pair needs two free descriptors");

    pipes_[buffers[0]] = PipeBuffer{.used = true};
    pipes_[buffers[1]] = PipeBuffer{.used = true};
    auto first = allocate_description(OpenFileDescription{.used = true, .readable = true, .writable = true,
        .flags = 2u | status_flags, .pipe = buffers[0], .unix_write_pipe = buffers[1]});
    auto second = allocate_description(OpenFileDescription{.used = true, .readable = true, .writable = true,
        .flags = 2u | status_flags, .pipe = buffers[1], .unix_write_pipe = buffers[0]});
    if (!first || !second)
    {
        if (first) *open_description(first.value()) = {};
        if (second) *open_description(second.value()) = {};
        pipes_[buffers[0]] = {};
        pipes_[buffers[1]] = {};
        return !first ? first.status() : second.status();
    }
    if (auto status = install_descriptor(resources, fds[0], first.value(), close_on_exec); !status)
    {
        *open_description(first.value()) = {};
        *open_description(second.value()) = {};
        pipes_[buffers[0]] = {};
        pipes_[buffers[1]] = {};
        return status;
    }
    if (auto status = install_descriptor(resources, fds[1], second.value(), close_on_exec); !status)
    {
        static_cast<void>(close_descriptor(resources, fds[0]));
        *open_description(second.value()) = {};
        pipes_[buffers[0]] = {};
        pipes_[buffers[1]] = {};
        return status;
    }
    return fds;
}
Result<usize> SchedulerState::read_pipe(u16 pipe, std::span<std::byte> out, bool nonblocking)
{
    smp::check_exclusive("SchedulerState::read_pipe");
    if (pipe >= pipes_.size() || !pipes_[pipe].used)
        return Status::invalid_argument("invalid pipe");
    if (out.empty())
        return usize{0};
    auto &buffer = pipes_[pipe];
    if (buffer.size == 0)
    {
        if (!pipe_has_endpoint(pipe, true))
            return usize{0};
        if (!nonblocking)
            static_cast<void>(block_current(ThreadWaitReason::pipe_read, pipe));
        return Status::would_block("pipe has no buffered data");
    }
    if (buffer.revision == ~u64{0})
        return Status::overflow("stream revision cannot wrap");
    u16 record = invalid_description;
    const auto count = unix_read_count(pipe, out.size(), record);
    for (usize i = 0; i < count; ++i)
        out[i] = buffer.data[(buffer.head + i) % pipe_capacity];
    consume_unix_bytes(pipe, count, record);
    return count;
}
Result<usize> SchedulerState::write_pipe(u16 pipe, std::span<const std::byte> in, bool nonblocking,
                                          bool atomic_request)
{
    auto result = write_pipe_fragment(pipe, in, atomic_request && in.size() <= pipe_capacity);
    if (!result && result.status().code() == StatusCode::would_block && !nonblocking)
        static_cast<void>(block_current(ThreadWaitReason::pipe_write, pipe));
    return result;
}
Result<usize> SchedulerState::write_pipe_fragment(u16 pipe, std::span<const std::byte> in, bool atomic_request)
{
    smp::check_exclusive("SchedulerState::write_pipe_fragment");
    if (pipe >= pipes_.size() || !pipes_[pipe].used)
        return Status::invalid_argument("invalid pipe");
    if (in.empty())
        return usize{0};
    if (!pipe_has_endpoint(pipe, false))
        return Status::broken_pipe("pipe has no readers");
    auto &buffer = pipes_[pipe];
    if (buffer.revision == ~u64{0})
        return Status::overflow("stream revision cannot wrap");
    const auto available = pipe_capacity - buffer.size;
    if (available == 0 || (atomic_request && in.size() > available))
        return Status::would_block("pipe buffer is full");
    const auto count = in.size() < available ? in.size() : available;
    for (usize i = 0; i < count; ++i)
        buffer.data[(buffer.head + buffer.size + i) % pipe_capacity] = in[i];
    buffer.size += count;
    ++buffer.revision;
    wake_waiters(ThreadWaitReason::pipe_read, pipe);
    return count;
}
#include "unix_rights.inc"
#include "unix_path.inc"
#include "descriptor_reservation.inc"
#include "pty_pair.inc"
#include "terminal_open.inc"

} // namespace ok::sched
