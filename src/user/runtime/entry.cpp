// Native start/step loop, user entry and syscall dispatch.
#include "private.hpp"

extern "C" ok::Status ok_boot_filesystem(ok::fs::VirtualFileSystem &) __attribute__((weak));
extern "C" ok::Status ok_boot_entropy(ok::posix::PosixService &) __attribute__((weak));
namespace ok::user::native
{
inline namespace runtime_detail
{
namespace
{
SharedNativeCapture<lu::configured_native_profile()> shared_capture{entry_stacks};
} // namespace
u64 entry_syscalls{}, entry_timers{}, entry_faults{}, entry_rejections{};
namespace
{
sched::ProcessId last_pid{};
sched::ThreadId last_tid{};
} // namespace
struct RuntimeStepGuard
{
    RuntimeStepGuard() { runtime_step_active = true; }
    ~RuntimeStepGuard() { runtime_step_active = false; }
};
namespace
{
Status drain_console_ingress()
{
    if (ok_platform_console_poll == nullptr)
    {
#if defined(OK_KERNEL_FREESTANDING)
        return Status::unsupported("console ingress flush requires its platform reader");
#else
        return {}; // Hosted models have no external UART ingress.
#endif
    }
    for (usize count = 0; count < 1024; ++count)
        if (ok_platform_console_poll() < 0) return {};
    return Status::io_error("console ingress did not become quiescent");
}
} // namespace
namespace
{
// Only this boundary accepts untrusted user pointers. The existing syscall
// table receives kernel-owned buffers; calls without a complete marshaller are
// deliberately unavailable to native programs.
i64 dispatch(Kernel &kernel, sched::ProcessId pid, hw::Syscall call, bool &complete)
{
    auto *process = kernel.scheduler().find(pid);
    auto *caller = kernel.scheduler().current_thread();
    if (!process || !caller || caller->owner != pid || caller->state != sched::ProcessState::running)
        return -22;
    const auto calling_tid = caller->tid;
    if (const auto object = pty_wait_object(pid, calling_tid); object.pool)
        return resume_pty_io(kernel, *process, object, complete);
    if (const auto object = linux_udp_receive_object(pid, calling_tid); object.pool)
    {
        complete = false;
        if (auto status = resume_linux_udp_receive(kernel, *process, *caller, object); !status)
            static_cast<void>(linux_udp_receive_invariant(status));
        return 0; // The captured owner alone publishes or remains pending.
    }
    if (const auto object = native_udp_receive_object(pid, calling_tid); object.pool)
        return resume_native_udp_receive(kernel, *process, object, complete);
    if (const auto object = child_wait_object(pid, calling_tid); object.pool)
        return resume_child_wait(kernel, *process, object, complete);
    if (const auto object = pipe_read_object(pid, calling_tid); object.pool)
    {
        auto saved = pipe_read_objects.resolve(object);
        if (!saved) { runtime_failure = saved.status(); return error(saved.status()); }
        if (caller->wait_registration.ticket() == saved.value()->ticket)
            return resume_pipe_read(kernel, *process, object, complete);
        cancel_pipe_read(kernel, object);
    }
    if (const auto object = pipe_write_object(pid, calling_tid); object.pool)
    {
        auto saved = pipe_write_objects.resolve(object);
        if (!saved) { runtime_failure = saved.status(); return error(saved.status()); }
        if (caller->wait_registration.ticket() == saved.value()->ticket)
            return resume_pipe_write(kernel, *process, object, complete);
        cancel_pipe_write(kernel, object);
    }
    // Blocking may clear the current CPU slot; retain identity rather than
    // consulting the first TCB or the post-block current-thread accessor.
    auto blocked_caller = [&] {
        const auto *thread = kernel.scheduler().find_thread(pid, calling_tid);
        return thread && thread->state == sched::ProcessState::blocked;
    };
    auto &space = process->resources().native_space;
    using N = syscall::Number;
    const auto number = static_cast<N>(call.number);
    const auto &a = call.arguments;
    if (number == N::pty_pair)
    {
        if (process->resources().personality != Personality::native) return -38;
        auto result = native_pty::create<std::endian::native == std::endian::big>(a,
            [&](uptr output, usize size) { return hw::user_range(space, output, size, true); },
            [&](u32 flags) { return kernel.scheduler().prepare_pty_pair(pid, flags); },
            [&](uptr output, std::span<const u8> bytes) {
                return hw::copy_to_user(space, output, bytes.data(), bytes.size());
            },
            [](sched::PreparedPtyPair &operation) noexcept { operation.publish_checked(); },
            [&](Status failure) { if (runtime_failure) runtime_failure = failure; });
        return result ? static_cast<i64>(result.value()) : error(result.status());
    }
    if (number == N::ioctl)
        return console_control::dispatch<std::endian::native == std::endian::big>(
            static_cast<i32>(a[0]), console_control::native_operation(a[1]), a[2],
            [&](i32 fd) { return kernel.posix().console_control_descriptor(fd); },
            [&](i32 fd, sched::ConsoleControlOperation operation, i32 value) {
                return kernel.posix().console_control(fd, operation, value);
            },
            [&](u64 address, std::span<u8> bytes) {
                return hw::copy_from_user(space, bytes.data(), static_cast<uptr>(address), bytes.size());
            },
            [&](u64 address, usize bytes) { return hw::user_range(space, static_cast<uptr>(address), bytes, true); },
            [&](u64 address, std::span<const u8> bytes) {
                return hw::copy_to_user(space, static_cast<uptr>(address), bytes.data(), bytes.size());
            });
    if (caller->console_wait_epoch && !kernel.scheduler().console_authorized(pid, caller->console_wait_epoch))
    {
        caller->console_wait_epoch = 0;
        return -13;
    }
    if (const auto operation = native_metadata::select(number))
        return native_metadata::dispatch(process->resources().personality == Personality::native, *operation, a,
            [&](uptr address, FixedString<256> &path) {
                return native_metadata::copy_path(address, path, [&](uptr byte, char &value) {
                    return hw::copy_from_user(space, &value, byte, 1);
                });
            },
            [&](native_metadata::Request request, std::string_view path) -> fs::ModeChangeResult {
                if (request.ownership)
                    return request.direct_fd ? kernel.posix().change_owner_fd(request.fd, request.uid, request.gid)
                        : kernel.posix().change_owner_at(request.fd, path, request.uid, request.gid, request.flags);
                return request.direct_fd ? kernel.posix().change_mode_fd(request.fd, request.mode)
                    : kernel.posix().change_mode_at(request.fd, path, request.mode, request.flags);
            });
    if (number == N::mount_info)
        return native_mount_info::dispatch(process->resources().personality == Personality::native, a,
            [&](u32 slot, fs::MountSnapshot &snapshot) { return kernel.vfs().mount_snapshot(slot, snapshot); },
            [&](uptr address, usize bytes) { return hw::user_range(space, address, bytes, true); },
            [&](uptr address, std::span<const u8> bytes) {
                return hw::copy_to_user(space, address, bytes.data(), bytes.size());
            });
    // Per-process memory quotas; other resources keep the static service table.
    if ((number == N::getrlimit || number == N::prlimit64) && process->resources().personality == Personality::native)
        if (auto *limit = process->resources().memory_limits.find(number == N::prlimit64 ? a[1] : a[0]))
        {
            const bool prlimit = number == N::prlimit64, set = prlimit && a[2];
            const uptr previous = prlimit ? a[3] : a[1];
            sched::MemoryLimit requested{}, saved = *limit;
            if (prlimit && a[0] && a[0] != static_cast<uptr>(pid)) return -3; // Only the caller's own limits.
            if (!prlimit && !previous) return -14;
            if (set && !hw::copy_from_user(space, &requested, a[2], sizeof(requested))) return -14;
            if (previous && !hw::copy_to_user(space, previous, &saved, sizeof(saved))) return -14;
            return set ? sched::MemoryLimits::update(*limit, requested, process->credentials().euid == 0) : 0;
        }
    if (number == N::power_prepare || number == N::power_commit)
    {
        if (process->resources().personality != Personality::native) return -38;
        const auto power_error = [](Status status) -> i64 {
            if (status.code() == StatusCode::denied || status.code() == StatusCode::permission) return -1;
            return status.code() == StatusCode::unsupported ? -95 : error(status);
        };
        auto owner = shutdown_caller(kernel);
        if (!owner) return power_error(owner.status());
        for (usize index = 1; index < a.size(); ++index)
            if (a[index]) return -22;
        // Check the complete native word before narrowing on 64-bit profiles.
        if (!a[0] || a[0] > OK_POWER_TOKEN_MAX) return -22;
        if (number == N::power_prepare)
        {
            auto issued = prepare_native_power(kernel, static_cast<u32>(a[0]));
            return issued ? static_cast<i64>(issued.value()) : power_error(issued.status());
        }
        const auto status = commit_native_power(kernel, static_cast<u32>(a[0]));
        return status ? 0 : power_error(status);
    }
    if (number == N::sync_persistent)
    {
        if (process->resources().personality != Personality::native) return -38;
        for (usize index = 1; index < a.size(); ++index)
            if (a[index]) return -22;
        if (a[0] > 0x7fffffffU) return -9;
        const auto status = kernel.posix().sync_persistent(static_cast<posix::Fd>(a[0]));
        return status ? 0 : status.code() == StatusCode::unsupported ? -95 : error(status);
    }
    if (number == N::console_claim || number == N::console_revoke || number == N::console_flush)
    {
        if (process->resources().personality != Personality::native) return -38;
        for (usize index = 1; index < a.size(); ++index)
            if (a[index]) return -22;
        if (!a[0] || a[0] > sched::native_console_epoch_limit) return -22;
        if (number == N::console_claim)
        {
            auto issued = kernel.scheduler().claim_console(a[0]);
            if (!issued) return error(issued.status());
            if (auto status = drain_console_ingress(); !status)
            { kernel.scheduler().abort_console_input(); return error(status); }
            return issued.value();
        }
        const auto epoch = static_cast<u32>(a[0]);
        auto status = number == N::console_revoke ? kernel.scheduler().revoke_console(epoch)
                                                 : kernel.scheduler().flush_console(epoch);
        if (!status) return error(status);
        if (auto drained = drain_console_ingress(); !drained)
        { kernel.scheduler().abort_console_input(); return error(drained); }
        return 0;
    }
    if (const auto credential_call = native_credentials::select(number))
    {
        if (process->resources().personality != Personality::native) return -38;
        return native_credentials::dispatch(*process, *credential_call, a,
            [&](uptr address, std::span<const u8> bytes) {
                return hw::copy_to_user(space, address, bytes.data(), bytes.size());
            });
    }
    const auto unix_operation = native_unix_path_call(call, kernel.scheduler(), *process);
    if (process->resources().personality == Personality::native && unix_operation != lu::UnixPathCall::none)
        return unix_pathname<lu::configured_native_profile(), true>(kernel.posix(), kernel.scheduler(), *process,
            unix_operation, {a[0], a[1], a[2], a[3], a[4], a[5]},
            [](std::array<u64, 6> arguments) -> Result<std::array<u64, 6>> { return arguments; },
            [&](u64 address, std::span<u8> bytes) { return hw::copy_from_user(space, bytes.data(), address, bytes.size()); },
            [&](u64 address, std::span<const u8> bytes) { return hw::copy_to_user(space, address, bytes.data(), bytes.size()); },
            [&](u64 address, usize length) { return hw::user_range(space, address, length, true); });
    if (number == N::sendmsg || number == N::recvmsg)
    {
        if (process->resources().personality != Personality::native) return -38;
        using C = lu::UnixMessageCodec<lu::configured_native_profile(), true>;
        // The UAPI structs have the target layout only when the compiler's word
        // matches the profile; a hosted model of a 32-bit profile on a 64-bit
        // host decodes the target bytes through the codec alone.
        static_assert(sizeof(void *) != C::word_bytes ||
            (sizeof(ok_msghdr) == C::header_bytes && sizeof(ok_cmsghdr) == C::cmsg_header_bytes));
        static_assert(sizeof(void *) != C::word_bytes ||
            (offsetof(ok_msghdr, vectors) == C::vectors_offset && offsetof(ok_msghdr, flags) == C::flags_offset));
        return unix_message<lu::configured_native_profile(), true>(kernel.posix(), number == N::recvmsg,
            a[0], a[1], a[2],
            [&](u64 address, std::span<u8> bytes) { return hw::copy_from_user(space, bytes.data(), address, bytes.size()); },
            [&](u64 address, std::span<const u8> bytes) { return hw::copy_to_user(space, address, bytes.data(), bytes.size()); },
            [&](u64 address, usize length) { return hw::user_range(space, address, length, true); });
    }
    if (number == N::socketpair)
    {
        if (process->resources().personality != Personality::native) return -38;
        constexpr u64 allowed = OK_SOCK_STREAM | OK_SOCK_NONBLOCK | OK_SOCK_CLOEXEC;
        if (a[0] != OK_AF_UNIX || (a[1] & ~allowed) || (a[1] & 0xfu) != OK_SOCK_STREAM || a[2])
            return -22;
        auto fds = kernel.posix().unix_socketpair(static_cast<u32>(a[1]) &
                                                  (posix::o_NONBLOCK | posix::o_CLOEXEC));
        if (!fds) return error(fds.status());
        if (!hw::copy_to_user(space, a[3], fds.value().data(), sizeof(i32) * 2))
        {
            static_cast<void>(kernel.posix().close(fds.value()[0]));
            static_cast<void>(kernel.posix().close(fds.value()[1]));
            return -14;
        }
        return 0;
    }
    if (number == N::socket || number == N::bind || number == N::sendto || number == N::recvfrom)
        return native_udp_call(kernel, process, caller, space, number, a, complete);
    if (number == N::poll)
    {
        if (a[1] > sched::max_process_fds)
            return -22;
        if (!hw::user_range(space, a[0], a[1] * sizeof(ok_pollfd), true))
            return -14;
        std::array<ok_pollfd, sched::max_process_fds> descriptors{};
        if (!hw::copy_from_user(space, descriptors.data(), a[0], a[1] * sizeof(ok_pollfd)))
            return -14;
        const i32 milliseconds = static_cast<i32>(a[2]);
        PollRequest request{};
        request.output = a[0];
        request.descriptors = {descriptors.data(), a[1]};
        request.immediate = milliseconds == 0;
        request.timed = milliseconds > 0;
        request.timeout = {milliseconds / 1000, (milliseconds % 1000) * 1000000};
        return begin_poll(kernel, pid, request, complete);
    }
    if (number == N::clock_settime)
    {
        if (a[0] != 0)
            return -22;
        ok_timespec requested{};
        if (!hw::copy_from_user(space, &requested, a[1], sizeof(requested)))
            return -14;
        return error(user::set_realtime_checked(
            {requested.seconds, requested.nanoseconds}, process->credentials(),
            [] { return arch::monotonic_sample(); },
            [](time::EpochTime value, time::MonotonicPoint point) { return arch::set_realtime(value, point); }));
    }
    if (number == N::clock_gettime || number == N::clock_getres)
    {
        if (a[0] > 1)
            return -22;
        if (number == N::clock_getres && a[1] == 0)
            return 0;
        auto now = time::public_clock(static_cast<u32>(a[0]), number == N::clock_getres);
        if (!now)
            return error(now.status());
        const posix::ClockTime result{now.value().seconds, now.value().nanoseconds};
        return hw::copy_to_user(space, a[1], &result, sizeof(result)) ? 0 : -14;
    }
    if (number == N::time || number == N::gettimeofday)
    {
        if (number == N::gettimeofday && a[0] == 0)
            return 0;
        auto now = time::public_clock(0);
        if (!now)
            return error(now.status());
        if (number == N::time)
        {
            auto seconds = time::native_time_seconds<sizeof(uptr)>(now.value());
            if (!seconds)
                return error(seconds.status());
            const i64 value = seconds.value();
            if (a[0] && !hw::copy_to_user(space, a[0], &value, sizeof(value)))
                return -14;
            return value;
        }
        const ok_timeval result{now.value().seconds, now.value().nanoseconds / 1000};
        return hw::copy_to_user(space, a[0], &result, sizeof(result)) ? 0 : -14;
    }
    if (number == N::nanosleep || number == N::clock_nanosleep)
    {
        const bool clock_sleep = number == N::clock_nanosleep;
        if (clock_sleep && (a[0] != 1 || (a[1] & ~uptr{1}) != 0))
            return -22;
        posix::ClockTime request{};
        if (!hw::copy_from_user(space, &request, a[clock_sleep ? 2 : 0], sizeof(request)))
            return -14;
        const bool absolute = clock_sleep && a[1] == 1;
        auto now = arch::monotonic_nanoseconds();
        if (!now)
            return error(now.status());
        return error(begin_sleep(kernel, pid, now.value(), request, absolute, complete));
    }
    if (number == N::brk)
    {
        auto adjusted = native_brk(process->resources(), a[0]);
        return adjusted ? static_cast<i64>(adjusted.value()) : error(adjusted.status());
    }
    if (number == N::memfd_create)
    {
        FixedString<256> name;
        if (auto status = string_from_user(space, a[0], name); !status) return error(status);
        auto created = kernel.posix().memfd_create(name.view(), static_cast<u32>(a[1]));
        return created ? static_cast<i64>(created.value()) : error(created.status());
    }
    if (number == N::mmap)
    {
        fs::Node *source = nullptr;
        usize file_size = 0;
        if ((a[3] & posix::map_ANONYMOUS) == 0)
        {
            const auto fd = static_cast<__INTPTR_TYPE__>(a[4]);
            if (fd < 0 || fd >= static_cast<__INTPTR_TYPE__>(sched::max_process_fds))
                return -9;
            const auto *slot = process->resources().files.entry(static_cast<i32>(fd));
            const auto *description = slot && slot->used ? kernel.scheduler().open_description(slot->description) : nullptr;
            if (!description || !description->used || (!description->node && !description->native_shared_file))
                return -9;
            if (!description->readable)
                return -13;
            if (description->native_shared_file)
            {
                if ((a[2] & posix::prot_WRITE) && !description->writable) return -13;
                const auto handle = description->native_shared_file;
                auto metadata = shared_files.info(handle);
                if (!metadata) return error(metadata.status());
                const auto id = slot->description;
                if (auto status = kernel.scheduler().retain_description(id); !status) return error(status);
                auto mapped = native_mmap(process->resources(), a[0], a[1], a[2], a[3], a[4], a[5],
                    nullptr, metadata.value().size, handle);
                if (auto status = kernel.scheduler().release_description(id); !status)
                { runtime_failure = status; return error(status); }
                return mapped ? static_cast<i64>(mapped.value()) : error(mapped.status());
            }
            if (a[3] & posix::map_SHARED)
                return error(Status::unsupported("shared VFS mappings require a coherent page cache"));
            auto metadata = description->node->query_metadata();
            if (!metadata) return error(metadata.status());
            if (metadata.value().type != fs::NodeType::regular)
                return -19;
            source = description->node;
            file_size = metadata.value().size;
        }
        auto mapped = native_mmap(process->resources(), a[0], a[1], a[2], a[3], a[4], a[5], source, file_size);
        return mapped ? static_cast<i64>(mapped.value()) : error(mapped.status());
    }
    if (number == N::mprotect)
        return error(native_mprotect(process->resources(), a[0], a[1], a[2]));
    if (number == N::munmap)
        return error(native_munmap(process->resources(), a[0], a[1]));
    if (number == N::exit || number == N::exit_group)
    {
        if (number == N::exit && process->resources().personality == Personality::linux)
        {
            retire_native_udp_receives(kernel, pid, calling_tid);
            const auto linux_udp_retired = retire_linux_udp_receives(
                kernel, pid, calling_tid, LinuxUdpReceiveRetirement::thread_exit);
            if (!runtime_failure) { started = false; complete = false; return error(runtime_failure); }
            if (!linux_udp_retired) { runtime_failure = linux_udp_retired; started = false; complete = false; return error(linux_udp_retired); }
            auto *thread = kernel.scheduler().find_thread(pid, calling_tid);
            for (const auto &entry : futex_waits)
                if (entry.pid == pid && entry.tid == calling_tid)
                    if (auto status = cancel_futex(kernel, entry.object); !status)
                    { complete = false; return error(status); }
            if (auto status = retire_native_thread(*thread); !status)
            { runtime_failure = status; started = false; complete = false; return error(status); }
            if (!runtime_failure) { started = false; complete = false; return error(runtime_failure); }
            for (const auto &entry : poll_waits)
                if (entry.pid == pid && entry.tid == calling_tid) cancel_poll(kernel, entry.object);
            for (const auto &entry : pipe_read_waits)
                if (entry.pid == pid && entry.tid == calling_tid) cancel_pipe_read(kernel, entry.object);
            for (const auto &entry : pipe_write_waits)
                if (entry.pid == pid && entry.tid == calling_tid) cancel_pipe_write(kernel, entry.object);
            cancel_pty_waits(kernel, pid, calling_tid);
            for (const auto &entry : child_waits)
                if (entry.pid == pid && entry.tid == calling_tid) cancel_child_wait(kernel, entry.object);
            usize live = 0;
            for (const auto &peer : process->threads()) live += peer.state != sched::ProcessState::exited;
            static_cast<void>(retire_robust(kernel, *process, thread->tls, thread->tid));
            clear_child_tid(kernel, *process, thread->tls, live > 1);
            if (!runtime_failure) { started = false; complete = false; return error(runtime_failure); }
            auto retired = kernel.scheduler().exit_thread(pid, calling_tid);
            if (!retired)
            { runtime_failure = retired.status(); started = false; complete = false; return error(retired.status()); }
#ifdef OK_PRIVATE_POLL_MASK
            if (auto status = PrivateChildExit::thread_exited(kernel.scheduler(), pid, calling_tid,
                sched::ChildExitStatus::normal(static_cast<i32>(a[0])), private_signal_find); !status)
            { runtime_failure = status; started = false; complete = false; return error(status); }
#endif
            if (retired.value() == 0) finish(kernel, pid, static_cast<i32>(a[0]));
        }
        else finish(kernel, pid, static_cast<i32>(a[0]));
        complete = false;
        return 0;
    }
    if (number == N::fork)
        return fork_process(kernel, pid, process, calling_tid);
    if (number == N::execve)
        return exec_image(kernel, pid, process, calling_tid, space, a, complete);
    if (number == N::wait4)
    {
        if ((a[2] & ~uptr{1}) || a[3]) return -22;
        return begin_child_wait(kernel, pid, {static_cast<i32>(a[0]), a[1], (a[2] & 1) != 0}, complete);
    }

    if (number == N::pipe || number == N::pipe2)
    {
        if (!hw::user_range(space, a[0], sizeof(i32) * 2, true))
            return -14;
        auto fds = kernel.posix().pipe(number == N::pipe2 ? static_cast<u32>(a[1]) : 0);
        if (!fds)
            return error(fds.status());
        if (!hw::copy_to_user(space, a[0], fds.value().data(), sizeof(i32) * 2))
        {
            static_cast<void>(kernel.posix().close(fds.value()[0]));
            static_cast<void>(kernel.posix().close(fds.value()[1]));
            return -14;
        }
        return 0;
    }
    if (number == N::readv || number == N::writev)
    {
        using Vector = PipeVector;
        std::array<Vector, PipeVectors::capacity> vectors{};
        if (a[2] > vectors.size()) return -22;
        if (!hw::copy_from_user(space, vectors.data(), a[1], a[2] * sizeof(Vector))) return -14;
        const auto *slot = process->fd_table().entry(static_cast<i32>(a[0]));
        auto *description = slot && slot->used ? kernel.scheduler().open_description(slot->description) : nullptr;
        const bool writing = number == N::writev;
        if (!description || !(writing ? description->writable : description->readable)) return -9;
        usize total = 0;
        for (usize i = 0; i < a[2]; ++i)
        {
            if (vectors[i].length > static_cast<usize>(__INTPTR_MAX__) - total) return -22;
            total += vectors[i].length;
        }
        if (description->pty_endpoint)
        {
            const PipeVectors request{vectors, a[2]};
            const auto staged = writing || total < sched::private_pty::RawPtyTable<>::queue_capacity
                ? total : sched::private_pty::RawPtyTable<>::queue_capacity;
            return begin_pty_io(kernel, *process, calling_tid, static_cast<i32>(a[0]), slot->description,
                                a[1], staged, writing, complete, request);
        }
        if (!total) return 0;
        if (writing && description->native_shared_file)
            if (auto status = shared_files.check_write(description->native_shared_file, total); !status)
                return error(status);
        if (description->pipe != sched::invalid_description)
        {
            const PipeVectors request{vectors, a[2]};
            const usize limit = sched::pipe_capacity + (writing ? 1 : 0);
            const usize staged = total < limit ? total : limit;
            return writing ? begin_pipe_write(kernel, *process, calling_tid, static_cast<i32>(a[0]),
                slot->description, a[1], total, complete, request)
                : begin_pipe_read(kernel, *process, calling_tid, static_cast<i32>(a[0]),
                slot->description, a[1], staged, complete, request);
        }
        for (usize i = 0; i < a[2]; ++i)
            if (!hw::user_range(space, vectors[i].base, vectors[i].length, !writing)) return -14;
        std::array<std::byte, sched::pipe_capacity> data{};
        const usize limit = data.size();
        if (total > limit) total = limit;
        usize position = 0;
        if (writing)
            for (usize i = 0; i < a[2] && position < total; ++i)
            {
                auto length = vectors[i].length < total - position ? vectors[i].length : total - position;
                if (!hw::copy_from_user(space, data.data() + position, vectors[i].base, length)) return -14;
                position += length;
            }
        Result<usize> result = usize{0};
        if (writing && description->console)
        {
            emit({reinterpret_cast<const char *>(data.data()), total});
            result = kernel.posix().write(static_cast<i32>(a[0]), {data.data(), total});
        }
        else if (!writing && description->input_console)
        {
            const auto epoch = caller->console_wait_epoch ? caller->console_wait_epoch : process->console_epoch();
            result = kernel.scheduler().read_console(pid, epoch, {data.data(), total});
            if (!result && result.status().code() == StatusCode::would_block &&
                !(description->flags & posix::o_NONBLOCK))
            {
                caller->console_wait_epoch = epoch;
                if (auto status = kernel.scheduler().block_current(sched::ThreadWaitReason::console_read, epoch); !status)
                { caller->console_wait_epoch = 0; return error(status); }
            }
        }
        else if (writing)
            result = kernel.posix().write(static_cast<i32>(a[0]), {data.data(), total});
        else if (description->native_shared_file)
            result = kernel.posix().pread(static_cast<i32>(a[0]), {data.data(), total}, description->offset);
        else result = kernel.posix().read(static_cast<i32>(a[0]), {data.data(), total});
        if (!result)
        {
            if (result.status().code() == StatusCode::would_block &&
                blocked_caller()) complete = false;
            return error(result.status());
        }
        if (!writing)
        {
            position = 0;
            for (usize i = 0; i < a[2] && position < result.value(); ++i)
            {
                auto length = vectors[i].length < result.value() - position ? vectors[i].length : result.value() - position;
                if (!hw::copy_to_user(space, vectors[i].base, data.data() + position, length)) return -14;
                position += length;
            }
            if (description->native_shared_file) description->offset += result.value();
        }
        return static_cast<i64>(result.value());
    }
    if (number == N::rename || number == N::link || number == N::renameat || number == N::linkat)
    {
        const bool relative = number == N::renameat || number == N::linkat;
        FixedString<256> source, destination;
        if (auto status = string_from_user(space, a[relative ? 1 : 0], source); !status.ok())
            return error(status);
        if (auto status = string_from_user(space, a[relative ? 3 : 1], destination); !status.ok())
            return error(status);
        Status result;
        if (number == N::rename)
            result = kernel.posix().rename(source.view(), destination.view());
        else if (number == N::link)
            result = kernel.posix().link(source.view(), destination.view());
        else if (number == N::renameat)
            result = kernel.posix().renameat(static_cast<i32>(a[0]), source.view(), static_cast<i32>(a[2]),
                                            destination.view());
        else
            result = kernel.posix().linkat(static_cast<i32>(a[0]), source.view(), static_cast<i32>(a[2]),
                                          destination.view(), static_cast<u32>(a[4]));
        return result.ok() ? 0 : error(result);
    }
    if (number == N::symlink || number == N::symlinkat)
    {
        FixedString<256> target, path;
        if (auto status = string_from_user(space, a[0], target); !status.ok())
            return error(status);
        if (auto status = string_from_user(space, a[number == N::symlink ? 1 : 2], path); !status.ok())
            return error(status);
        return error(kernel.posix().symlinkat(target.view(),
            number == N::symlink ? posix::at_FDCWD : static_cast<i32>(a[1]), path.view()));
    }
    if ((number == N::pread64 || number == N::pwrite64) && static_cast<__INTPTR_TYPE__>(a[3]) < 0)
        return -22;
    if ((number == N::truncate || number == N::ftruncate) && static_cast<__INTPTR_TYPE__>(a[1]) < 0)
        return -22;
    syscall::Request request{.number = number, .caller = pid};
    const auto *fd = process->fd_table().entry(static_cast<i32>(a[0]));
    const auto *description = fd && fd->used ? kernel.scheduler().open_description(fd->description) : nullptr;
    if ((number == N::read || number == N::write) && description && description->pty_endpoint)
    {
        const bool writing = number == N::write;
        const auto count = writing || a[2] < sched::private_pty::RawPtyTable<>::queue_capacity
            ? a[2] : sched::private_pty::RawPtyTable<>::queue_capacity;
        return begin_pty_io(kernel, *process, calling_tid, static_cast<i32>(a[0]), fd->description,
                            a[1], count, writing, complete);
    }
    if (number == N::write && description && description->writable && description->pipe != sched::invalid_description)
        return begin_pipe_write(kernel, *process, calling_tid, static_cast<i32>(a[0]), fd->description, a[1],
            a[2], complete);
    if (number == N::read && description && description->readable && description->pipe != sched::invalid_description)
        return begin_pipe_read(kernel, *process, calling_tid, static_cast<i32>(a[0]), fd->description, a[1],
            a[2] < sched::pipe_capacity ? a[2] : sched::pipe_capacity, complete);
    for (usize i = 0; i < 6; ++i)
        request.args[i] = a[i];
    if (number == N::lseek)
        request.args[1] = static_cast<u64>(static_cast<__INTPTR_TYPE__>(a[1]));
    alignas(16) std::array<std::byte, sched::pipe_capacity + 1> buffer{};
    FixedString<256> path;
    int path_arg = -1, output_arg = -1, input_arg = -1;
    usize output_size = 0, input_size = 0;
    bool variable_output = false;
    switch (number)
    {
    case N::read:
    case N::pread64:
    case N::getdents:
    case N::getdents64:
        output_arg = 1;
        output_size = a[2] < sched::pipe_capacity ? a[2] : sched::pipe_capacity;
        request.args[2] = output_size;
        variable_output = true;
        break;
    case N::write:
    case N::pwrite64:
    {
        if (description && description->writable && description->native_shared_file)
            if (auto status = shared_files.check_write(description->native_shared_file, a[2]); !status)
                return error(status);
        input_arg = 1;
        const usize limit = number == N::write && description && description->pipe != sched::invalid_description
                                ? buffer.size() : sched::pipe_capacity;
        input_size = a[2] < limit ? a[2] : limit;
        request.args[2] = input_size;
        break;
    }
    case N::truncate:
    case N::open:
    case N::creat:
    case N::mkdir:
    case N::unlink:
    case N::rmdir:
    case N::chdir:
    case N::access:
        path_arg = 0;
        break;
    case N::openat:
    case N::mkdirat:
    case N::unlinkat:
    case N::faccessat:
    case N::faccessat2:
        path_arg = 1;
        break;
    case N::stat:
    case N::lstat:
        path_arg = 0;
        output_arg = 1;
        output_size = sizeof(ok_stat);
        break;
    case N::fstat:
        output_arg = 1;
        output_size = sizeof(ok_stat);
        break;
    case N::newfstatat:
        path_arg = 1;
        output_arg = 2;
        output_size = sizeof(ok_stat);
        break;
    case N::fstat_v2:
        output_arg = 1;
        output_size = sizeof(ok_stat_v2);
        break;
    case N::newfstatat_v2:
        path_arg = 1;
        output_arg = 2;
        output_size = sizeof(ok_stat_v2);
        break;
    case N::readlink:
    case N::readlinkat:
    {
        const bool relative = number == N::readlinkat;
        path_arg = relative ? 1 : 0;
        output_arg = relative ? 2 : 1;
        const usize size_arg = relative ? 3 : 2;
        if (a[size_arg] == 0)
            return -22;
        output_size = a[size_arg] < buffer.size() ? a[size_arg] : buffer.size();
        request.args[size_arg] = output_size;
        variable_output = true;
        break;
    }
    case N::getcwd:
        output_arg = 0;
        output_size = a[1] < buffer.size() ? a[1] : buffer.size();
        request.args[1] = output_size;
        break;
    case N::uname:
        output_arg = 0;
        output_size = sizeof(posix::UnameInfo);
        break;
    case N::getrlimit:
        output_arg = 1;
        output_size = sizeof(posix::ResourceLimit);
        break;
    case N::sysinfo:
        output_arg = 0;
        output_size = sizeof(posix::SystemInfo);
        break;
    case N::getrandom:
        output_arg = 0;
        output_size = a[1] < buffer.size() ? a[1] : buffer.size();
        request.args[1] = output_size;
        variable_output = true;
        break;
    case N::kill:
#ifdef OK_PRIVATE_POLL_MASK
        return native_process_kill(kernel, *process, a[0], a[1]);
#else
        return -95;
#endif
    case N::fsync:
    case N::fdatasync:
    case N::ftruncate:
    case N::close:
    case N::close_range:
    case N::dup:
    case N::dup2:
    case N::dup3:
    case N::lseek:
    case N::getpid:
    case N::getppid:
    case N::setpgid:
    case N::getpgid:
    case N::getpgrp:
    case N::getsid:
    case N::setsid:
    case N::gettid:
    case N::getuid:
    case N::getgid:
    case N::geteuid:
    case N::getegid:
    case N::umask:
    case N::fcntl:
    case N::fchdir:
    case N::sched_yield:
        break;
    default:
        return -38;
    }
    if (path_arg >= 0)
    {
        if (auto status = string_from_user(space, a[path_arg], path); !status.ok())
            return error(status);
        request.args[path_arg] = reinterpret_cast<uptr>(path.c_str());
    }
    if (input_arg >= 0)
    {
        if (!hw::copy_from_user(space, buffer.data(), a[input_arg], input_size))
            return -14;
        request.args[input_arg] = reinterpret_cast<uptr>(buffer.data());
    }
    if (output_arg >= 0)
    {
        // readlink has no side effects: validate only the bytes it returns,
        // so unused capacity may extend beyond the caller's mapped buffer.
        if (number != N::readlink && number != N::readlinkat &&
            !hw::user_range(space, a[output_arg], output_size, true))
            return -14;
        request.args[output_arg] = reinterpret_cast<uptr>(buffer.data());
    }
    if (number == N::write && description && description->console && description->writable)
    {
        emit({reinterpret_cast<const char *>(buffer.data()), input_size});
        static_cast<void>(kernel.console().write({reinterpret_cast<const char *>(buffer.data()), input_size}));
        return static_cast<i64>(input_size);
    }
    if (number == N::read && description && description->input_console)
    {
        if (!description->readable) return -9;
        const auto epoch = caller->console_wait_epoch ? caller->console_wait_epoch : process->console_epoch();
        auto read = kernel.scheduler().read_console(pid, epoch, {buffer.data(), output_size});
        if (!read && read.status().code() == StatusCode::would_block)
        {
            if (description->flags & posix::o_NONBLOCK)
                return -11;
            caller->console_wait_epoch = epoch;
            auto status = kernel.scheduler().block_current(sched::ThreadWaitReason::console_read, epoch);
            if (!status)
            { caller->console_wait_epoch = 0; return error(status); }
            complete = false;
            return -11;
        }
        if (!read) return error(read.status());
        const auto count = read.value();
        if (!hw::copy_to_user(space, a[1], buffer.data(), count))
            return -14;
        return static_cast<i64>(count);
    }
    if (description && description->native_shared_file && (number == N::read || number == N::pread64))
    {
        auto read = kernel.posix().pread(static_cast<i32>(a[0]), {buffer.data(), output_size},
            number == N::read ? description->offset : static_cast<usize>(a[3]));
        if (!read) return error(read.status());
        if (!hw::copy_to_user(space, a[1], buffer.data(), read.value())) return -14;
        if (number == N::read)
        {
            auto *open = kernel.scheduler().open_description(fd->description);
            if (!open || open->native_shared_file != description->native_shared_file)
                return error(Status::fault("shared file read publication lost its canonical description"));
            open->offset += read.value();
        }
        return static_cast<i64>(read.value());
    }
    auto result = kernel.syscalls().dispatch(request);
    if (!result.status.ok())
    {
        if (result.status.code() == StatusCode::would_block &&
            blocked_caller())
            complete = false;
        return error(result.status);
    }
    if (output_arg >= 0)
    {
        if (variable_output)
            output_size = static_cast<usize>(result.value);
        if (output_size > buffer.size() || !hw::copy_to_user(space, a[output_arg], buffer.data(), output_size))
            return -14;
    }
    // A file or device read fills the request one buffer at a time, as on
    // Linux, until a chunk comes back short. Pipes, PTYs and the console
    // returned above with their own semantics.
    if (number == N::read && description && description->node && output_size == sched::pipe_capacity)
    {
        usize total = output_size;
        while (total < a[2])
        {
            const usize wanted = a[2] - total < sched::pipe_capacity ? a[2] - total : sched::pipe_capacity;
            request.args[1] = reinterpret_cast<uptr>(buffer.data());
            request.args[2] = wanted;
            const auto more = kernel.syscalls().dispatch(request);
            if (!more.status.ok() || more.value <= 0) break;
            const auto chunk = static_cast<usize>(more.value);
            if (chunk > wanted || !hw::copy_to_user(space, a[1] + total, buffer.data(), chunk)) break;
            total += chunk;
            if (chunk < wanted) break;
        }
        return static_cast<i64>(total);
    }
    return result.value;
}
} // namespace
} // namespace runtime_detail
#if defined(OK_ENABLE_TEST_POINTS) && defined(OK_PRIVATE_FUTEX_PEER_FIXTURE)
#include "../../../tests/native/private_futex_peer_fixture.hpp"
#endif
Status start(Kernel &kernel, fs::BootArchive archive)
{
#ifdef OK_KERNEL_FREESTANDING
    // Root selection and native services run on the original boot stack.
    // Bounds are a current-SP check, not faulting guards or a usage budget.
    if (auto status = arch::require_service_stack(); !status) return status;
#endif
    if (shutdown_admission_closed || native_shutdown_pending())
        return Status::busy("native shutdown has closed runtime admission");
    if (started || original_shutdown_owner.runtime)
        return Status::already_exists("native runtime already bound its original init");
    if (!runtime_failure) return runtime_failure;
    if (auto status = linux_udp_receive_start_status(); !status) return status;
    if (!hw::supported())
        return Status::unsupported("native execution backend is unavailable");
    if (!archive.data || !archive.initial_program)
        return Status::invalid_argument("missing boot archive");
    if (auto status = fs::mount_archive(kernel.vfs(), {archive.data, archive.size}); !status.ok())
        return status;
    if (ok_boot_filesystem != nullptr)
        if (auto status = ok_boot_filesystem(kernel.vfs()); !status.ok())
            return status;
    if (ok_boot_entropy != nullptr)
        if (auto status = ok_boot_entropy(kernel.posix()); !status.ok())
            return status;
    // Root selection can cover the directory retained by early diagnostics.
    // Kernel GUI/services must start in the selected root as well as /init.
    if (auto status = kernel.posix().activate(0); !status.ok())
        return status;
    if (auto status = kernel.posix().chdir("/"); !status.ok())
        return status;
    if (auto status = hw::initialize(); !status.ok())
        return status;
    // Allocation becomes possible only after supervisor mappings are established.
    if (auto status = activate_pool(&kernel); !status) return status;
    if (auto status = shared_files.bind(page_pool); !status) return status;
    if (auto status = kernel.scheduler().set_shared_memory_files(shared_files); !status) return status;
    if (auto status = entry_stacks.activate(entry_stack_arena); !status) return status;
    emit_pool_stats();
    emit_entry_stack_stats();
#ifdef OK_ENABLE_TEST_POINTS
    if (auto status = check_runtime_objects(); !status) return status;
    emit_pool_stats();
#endif
    if (auto status = arch::initialize_clock(); !status.ok())
        return status;
    // A missing RTC does not prevent boot. The first anchor attempt is terminal
    // for this boot, and fork/exec never repeat it or substitute monotonic time.
    const auto realtime_status = arch::initialize_realtime();
    const auto realtime_info = arch::realtime_info();
    emit("OK_REALTIME scope=runtime state="); emit_number(static_cast<u32>(realtime_info.state));
    emit(" source="); emit_number(static_cast<u32>(realtime_info.source));
    emit(" source_resolution_ns="); emit_number(realtime_info.source_resolution_ns);
    emit(" acquisition_span_ns="); emit_number(realtime_info.acquisition_span_ns);
    emit(" accuracy=unknown available="); emit_number(realtime_status ? 1 : 0); emit("\n");
    auto pid = install_init(kernel, archive);
    if (!pid) return pid.status();
    auto *process = kernel.scheduler().find(pid.value());
    static_cast<void>(kernel.scheduler().set_runnable(pid.value()));
#if defined(OK_ENABLE_TEST_POINTS) && defined(OK_PRIVATE_FUTEX_PEER_FIXTURE)
    if (auto status = setup_peer_fixture(kernel, pid.value()); !status) return status;
#endif
    started = true;
    runtime_kernel = &kernel;
#ifdef OK_NATIVE_COW_FORK
    hw::set_write_resolver(resolve_native_write);
#endif
    // This static singleton has one boot lifetime and no reconstruction/reset API.
    original_shutdown_owner = {&original_shutdown_owner, 1, pid.value(), process->threads()[0].tid};
    emit("OK_NATIVE scope=hardware init=loaded\n");
    return Status::success();
}
bool step(Kernel &kernel)
{
    if (shutdown_admission_closed || native_shutdown_pending()) return false;
    if (runtime_step_active)
    { runtime_failure = Status::busy("native step is not reentrant"); started = false; return false; }
    RuntimeStepGuard step_guard;
#ifdef OK_KERNEL_FREESTANDING
    if (auto status = arch::require_service_stack(); !status)
    { runtime_failure = status; started = false; return false; }
#endif
    // Retry retained owner/pin cleanup on the serialized boot-CPU service path.
    // A failed retry remains discoverable and cannot consume a discharged edge.
    if (auto retired = retry_quarantined_retirement(); !retired) runtime_failure = retired;
    if (auto retired = shared_files.retry_retirement(); !retired) runtime_failure = retired;
    // The outer loop also handles GUI/services when no native task is ready.
    if (!runtime_failure) { started = false; return false; }
    if (!kernel.posix().activate(0))
        return false;
    if (!started)
        return false;
#ifdef OK_PRIVATE_POLL_MASK
    private_process_signal_fatal(kernel);
    if (!runtime_failure) { started = false; return false; }
#endif
#if defined(OK_ENABLE_TEST_POINTS) && defined(OK_PRIVATE_FUTEX_PEER_FIXTURE)
    if (auto status = checkpoint_peer_fixture(kernel); !status)
    { runtime_failure = status; started = false; return false; }
    if (!started) return false;
#endif
    auto now = arch::monotonic_nanoseconds();
    if (!now)
    {
        runtime_failure = now.status();
        started = false;
        emit("OK_TEST_FAIL native-clock=unavailable\n");
        return false;
    }
    kernel.posix().set_monotonic_time(now.value(), arch::clock_resolution_ns());
    if (auto status = prune_departed_linux_udp_receives(kernel); !status)
    { runtime_failure = status; started = false; return false; }
#ifdef OK_PRIVATE_POLL_MASK
    if (!private_child_reap(kernel)) return false;
#else
    static_cast<void>(kernel.scheduler().reap_orphans());
#endif
    if (!runtime_failure) { started = false; return false; }
    if (auto status = scan_native_udp_receives(kernel); !status)
    { runtime_failure = status; started = false; return false; }
    if (auto status = scan_linux_udp_receives(kernel); !status)
    { runtime_failure = status; started = false; return false; }
    prune_pipe_reads(kernel);
    prune_pipe_writes(kernel);
    prune_pty_waits(kernel);
    prune_child_waits(kernel);
    if (!runtime_failure) { started = false; return false; }
    bool waiting_deadline = expire_futex(kernel, now.value());
#ifdef OK_PRIVATE_POLL_MASK
    waiting_deadline |= private_signal_wait_pending(kernel);
#endif
    for (const auto &entry : poll_waits)
    {
        if (entry.pid == 0)
            continue;
        const auto object = entry.object;
        auto resolved = poll_objects.resolve(object);
        if (!resolved)
        {
            runtime_failure = resolved.status();
            started = false;
            return false;
        }
        auto &wait = *resolved.value();
        auto *candidate = kernel.scheduler().find(wait.pid);
        auto *owner = kernel.scheduler().find_thread(wait.pid, wait.tid);
        if (candidate == nullptr || owner == nullptr || !owner->native_active ||
            !owner->wait_registration.pending(wait.ticket) ||
            owner->wait_reason != sched::ThreadWaitReason::poll ||
            candidate->state() == sched::ProcessState::zombie || candidate->state() == sched::ProcessState::exited)
        {
            cancel_poll(kernel, object);
            continue;
        }
        auto ready = poll_ready(kernel, wait);
        const bool expired = wait.timeout.complete(now.value(), wait.timeout.generation());
        if (!ready || ready.value() != 0 || expired)
        {
            const auto cause = ready && ready.value() == 0 && expired
                ? sched::WaitCompletion::timeout : sched::WaitCompletion::ready;
            if (!kernel.scheduler().complete_wait(wait.ticket, cause))
            {
                cancel_poll(kernel, object);
                continue;
            }
            i64 result = ready ? static_cast<i64>(ready.value()) : error(ready.status());
            if (ready && !poll_output(candidate->resources().native_space, wait))
                result = -14;
            finish_poll_timeout(candidate->resources().native_space, wait.timeout_output, now.value());
            auto &thread = *owner;
            thread.syscall_pending = false;
            if (auto status = set_result(*candidate, thread.native_frame, result); !status.ok())
            {
                runtime_failure = status;
                finish(kernel, candidate->pid(), 127);
                continue;
            }
            cancel_poll(kernel, object, cause == sched::WaitCompletion::timeout ? "timeout" : "ready", result);
        }
        else
            waiting_deadline = true;
    }
    for (auto &candidate : kernel.scheduler().processes())
        for (auto &thread : candidate.threads())
            if (thread.native_active && thread.native_deadline.armed())
            {
                if (thread.wait_reason == sched::ThreadWaitReason::deadline &&
                    thread.native_deadline.complete(now.value(), thread.wait_key) &&
                    kernel.scheduler().complete_wait(thread.wait_registration.ticket(), sched::WaitCompletion::timeout))
                {
                    thread.syscall_pending = false;
                    if (auto status = set_result(candidate, thread.native_frame, 0); !status.ok())
                    {
                        runtime_failure = status;
                        finish(kernel, candidate.pid(), 127);
                        continue;
                    }
                }
                else
                    waiting_deadline = true;
            }
    if (!runtime_failure || !started) { started = false; return false; }
    struct Candidate { sched::ProcessId pid{}; sched::ThreadId tid{}; };
    auto before = [](Candidate left, Candidate right) {
        return left.pid < right.pid || (left.pid == right.pid && left.tid < right.tid);
    };
    Candidate chosen{}, first{};
    for (const auto &candidate : kernel.scheduler().processes())
    {
        if (candidate.state() != sched::ProcessState::runnable && candidate.state() != sched::ProcessState::running &&
            candidate.state() != sched::ProcessState::blocked)
            continue;
        for (const auto &thread : candidate.threads())
        {
            bool interruptible = false;
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64) || defined(__mips__) || defined(__powerpc__) || defined(__PPC__))
            if (linux_udp_receive_object(candidate.pid(), thread.tid).pool)
                interruptible = private_linux_udp_signal_ready(kernel, candidate, thread);
            else
                interruptible = private_signal_wait_ready(kernel, candidate, thread, now.value()) ||
                            private_sleep_signal_ready(kernel, candidate, thread) ||
                            private_pipe_signal_ready(kernel, candidate, thread) ||
                            private_pipe_write_signal_ready(kernel, candidate, thread) ||
                            private_futex_signal_ready(kernel, candidate, thread) ||
                            private_poll_signal_ready(kernel, candidate, thread) ||
                            private_child_signal_ready(kernel, candidate, thread);
#endif
            if (!thread.native_active ||
                (thread.state != sched::ProcessState::runnable && thread.state != sched::ProcessState::running && !interruptible))
                continue;
            Candidate entry{candidate.pid(), thread.tid};
            if (!first.pid || before(entry, first)) first = entry;
            if (before({last_pid, last_tid}, entry) && (!chosen.pid || before(entry, chosen))) chosen = entry;
        }
    }
    if (!runtime_failure || !started) { started = false; return false; }
    if (!chosen.pid)
        chosen = first;
    if (!chosen.pid)
        return waiting_deadline;
    last_pid = chosen.pid;
    last_tid = chosen.tid;
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64) || defined(__mips__) || defined(__powerpc__) || defined(__PPC__))
    if (auto *selected = kernel.scheduler().find_thread(chosen.pid, chosen.tid); selected)
    {
        auto *owner = kernel.scheduler().find(chosen.pid);
        if (!owner) return false;
        // Also inspect runnable continuations whose readiness wake lost its
        // data to a peer. A wake alone has not completed the original read.
        if (linux_udp_receive_object(chosen.pid, chosen.tid).pool)
        {
            if (private_linux_udp_signal_ready(kernel, *owner, *selected) &&
                !private_linux_udp_signal_complete(kernel, *owner, *selected)) return runtime_failure.ok();
        }
        else if (private_signal_wait_ready(kernel, *owner, *selected, now.value()))
        {
            if (!private_signal_wait_complete(kernel, *owner, *selected, now.value())) return runtime_failure.ok();
        }
        else if (private_pipe_signal_ready(kernel, *owner, *selected))
        {
            if (!private_pipe_signal_complete(kernel, *owner, *selected)) return runtime_failure.ok();
        }
        else if (private_pipe_write_signal_ready(kernel, *owner, *selected))
        {
            if (!private_pipe_write_signal_complete(kernel, *owner, *selected)) return runtime_failure.ok();
        }
        else if (private_child_signal_ready(kernel, *owner, *selected))
        {
            if (!private_child_signal_complete(kernel, *owner, *selected)) return runtime_failure.ok();
        }
        else if (private_futex_signal_ready(kernel, *owner, *selected))
        {
            if (!private_futex_signal_complete(kernel, *owner, *selected)) return runtime_failure.ok();
        }
        else if (private_poll_signal_ready(kernel, *owner, *selected))
        {
            if (!private_poll_signal_complete(kernel, *owner, *selected)) return runtime_failure.ok();
        }
        else if (selected->state == sched::ProcessState::blocked &&
                 !private_sleep_signal_complete(kernel, *owner, *selected)) return runtime_failure.ok();
    }
#endif
    if (!kernel.scheduler().activate_thread(chosen.pid, chosen.tid))
        return false;
    auto *process = kernel.scheduler().find(chosen.pid);
    auto &thread = *kernel.scheduler().current_thread();
    if (linux_udp_receive_object(chosen.pid, chosen.tid).pool && !thread.syscall_pending)
    {
        static_cast<void>(linux_udp_receive_invariant(
            Status::fault("Linux UDP receive owner lost its pending syscall before entry")));
        return false;
    }
    bool linux_udp_delay_slot = false;
    if (!thread.syscall_pending)
    {
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64) || defined(__mips__) || defined(__powerpc__) || defined(__PPC__))
        if (!private_signal_deliver(kernel, *process, thread))
        {
            static_cast<void>(kernel.posix().activate(0));
            return true;
        }
#endif
        if (thread.tls.set_tid_address)
        {
            const uptr destination = thread.tls.set_tid_address;
            thread.tls.set_tid_address = 0;
            std::array<u8, 4> bytes{};
            constexpr bool big_endian = lu::Codec<lu::configured_native_profile()>::profile.big_endian;
            for (usize i = 0; i < bytes.size(); ++i)
                bytes[i] = static_cast<u8>(thread.tid >> (8 * (big_endian ? 3 - i : i)));
            static_cast<void>(hw::copy_to_user(process->resources().native_space, destination, bytes.data(), bytes.size()));
        }
        if (auto status = validate_entry_stack(entry_stacks, thread); !status)
        { runtime_failure = status; started = false; return false; }
        auto active = entry_stacks.activate_entry(thread.entry_stack, {chosen.pid, chosen.tid}, 0);
        if (!active) { runtime_failure = active.status(); started = false; return false; }
        const auto view = active.value();
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
        if (!private_n64_begin(*process, thread, view))
        {
            static_cast<void>(entry_stacks.deactivate_entry(thread.entry_stack, {chosen.pid, chosen.tid}, 0));
            runtime_failure = Status::fault("private n64 entry readiness missing");
            started = false;
            return false;
        }
#endif
#ifdef OK_PRIVATE_MIPS_O32_SIGNAL
        if (!private_mips_begin(thread))
        {
            static_cast<void>(entry_stacks.deactivate_entry(thread.entry_stack, {chosen.pid, chosen.tid}, 0));
            runtime_failure = Status::fault("private MIPS entry readiness missing");
            started = false;
            return false;
        }
#endif
#ifdef OK_PRIVATE_PPC32_SIGNAL
        if (process->resources().personality == Personality::linux && !private_ppc_begin(*process, thread, view))
        {
            static_cast<void>(entry_stacks.deactivate_entry(thread.entry_stack, {chosen.pid, chosen.tid}, 0));
            runtime_failure = Status::fault("private PPC entry readiness missing");
            started = false; return false;
        }
#endif
        auto trap = shared_capture.enter(syscall_origins, vfs_continuations, *process, thread, view, kernel.scheduler());
        const auto capture = trap ? trap.value().kernel_stack_pointer : 0;
        const bool captured = capture != 0 && view.contains(capture);
        const bool needs_capture = trap && (trap.value().kind == hw::TrapKind::syscall ||
                                           trap.value().kind == hw::TrapKind::timer);
        const bool bad_capture = (capture && !captured) || (needs_capture && !captured);
#if defined(OK_PRIVATE_MIPS64_N64_SIGNAL) || defined(OK_PRIVATE_MIPS_O32_SIGNAL)
        // A copy-on-write fault resumes the same frame. Its capture keeps the
        // handler readiness; without one the thread stays unverified.
        const bool resumable_fault = captured && trap.value().kind == hw::TrapKind::page_fault;
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
        // The observation expires with view deactivation. Consume it first.
        const bool n64_capture_valid = !needs_capture || (!bad_capture && private_n64_capture(*process, thread, view));
        const bool n64_fault_captured = resumable_fault && private_n64_capture(*process, thread, view);
        if ((!needs_capture && !n64_fault_captured) || !n64_capture_valid) private_n64_revoke(thread);
#endif
#ifdef OK_PRIVATE_PPC32_SIGNAL
        const bool ppc_linux = process->resources().personality == Personality::linux;
        const bool ppc_capture_valid = !ppc_linux || (trap && !bad_capture && private_ppc_capture(*process, thread, view));
        if (ppc_linux && !ppc_capture_valid) private_ppc_revoke(thread);
#endif
        // Backends have restored the service stack and detached hardware/global
        // pointers before returning. Never dispatch or free an active lease.
        const auto deactivated = entry_stacks.deactivate_entry(thread.entry_stack, {chosen.pid, chosen.tid}, 0);
        if (!deactivated || bad_capture)
        {
            syscall_origins.revoke(thread);
            vfs_continuations.revoke(thread);
            runtime_failure = !deactivated ? deactivated : Status::fault("missing owned privileged capture");
            started = false;
            emit("OK_TEST_FAIL native-entry-stack=ownership\n");
            return false;
        }
#ifdef OK_PRIVATE_PPC32_SIGNAL
        if (!ppc_capture_valid)
        {
            syscall_origins.revoke(thread);
            vfs_continuations.revoke(thread);
            runtime_failure = Status::fault("private PPC capture readiness missing");
            started = false; return false;
        }
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
        if (!n64_capture_valid)
        {
            syscall_origins.revoke(thread);
            vfs_continuations.revoke(thread);
            runtime_failure = Status::fault("private n64 capture readiness missing");
            started = false;
            return false;
        }
#endif
#ifdef OK_PRIVATE_MIPS_O32_SIGNAL
        if (needs_capture)
        {
            if (!private_mips_capture(thread))
            {
                syscall_origins.revoke(thread);
            vfs_continuations.revoke(thread);
                runtime_failure = Status::fault("private MIPS capture readiness missing");
                started = false;
                return false;
            }
        }
        else if (!resumable_fault || !private_mips_capture(thread)) private_mips_revoke(thread);
#endif
        if (captured)
        {
            if (trap.value().kind == hw::TrapKind::syscall) ++entry_syscalls;
            else if (trap.value().kind == hw::TrapKind::timer) ++entry_timers;
            else ++entry_faults;
        }
        else ++entry_rejections;
        if (!trap)
        {
            finish(kernel, chosen.pid, 127);
            emit("OK_NATIVE scope=hardware enter=failed\n");
        }
        else if (trap.value().kind == hw::TrapKind::syscall)
        {
#if defined(OK_ENABLE_TEST_POINTS) && defined(OK_PRIVATE_FUTEX_PEER_FIXTURE)
            if (auto status = record_peer_trap(kernel, chosen.pid, chosen.tid, thread.native_frame); !status)
            { runtime_failure = status; started = false; return false; }
#endif
            linux_udp_delay_slot = linux_udp_delay_slot_excluded<lu::configured_native_profile()>(trap.value());
            thread.syscall_pending = true;
        }
        else if (trap.value().kind == hw::TrapKind::timer)
        {
            ++thread.cpu_time_ticks;
            static bool reported = false;
            if (!reported)
            {
                emit("OK_NATIVE scope=hardware timer=preempted\n");
                reported = true;
            }
        }
#ifdef OK_NATIVE_COW_FORK
        else if (trap.value().kind == hw::TrapKind::page_fault &&
                 resolve_native_write(process->resources().native_space, trap.value().fault_address)) {}
#endif
        else
        {
            emit("OK_NATIVE scope=hardware fault=contained\n");
            finish(kernel, chosen.pid, 0, 11);
        }
    }
    if (thread.native_active && thread.syscall_pending)
    {
        bool complete = true;
        i64 result{};
#ifdef OK_PRIVATE_PPC32_SIGNAL
        sched::ThreadId ppc_prepared_clone{};
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
        sched::ThreadId n64_prepared_clone{};
#endif
        // Captured Linux ownership precedes signal-call/frame decoding and all
        // synchronous continuations. Resume never rereads syscall arguments.
        if (const auto object = linux_udp_receive_object(chosen.pid, chosen.tid); object.pool)
        {
            complete = false;
            if (auto status = resume_linux_udp_receive(kernel, *process, thread, object); !status)
                static_cast<void>(linux_udp_receive_invariant(status));
        }
        else
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__arm__) || defined(__riscv) || defined(__loongarch64) || defined(__mips__) || defined(__powerpc__) || defined(__PPC__))
        if (!private_signal_call(kernel, *process, thread, complete, result))
#endif
        {
        if (const auto object = native_udp_receive_object(chosen.pid, chosen.tid); object.pool)
            result = resume_native_udp_receive(kernel, *process, object, complete);
        else if (const auto object = pty_wait_object(chosen.pid, chosen.tid); object.pool)
            result = resume_pty_io(kernel, *process, object, complete);
        else
        if (const auto object = child_wait_object(chosen.pid, chosen.tid); object.pool)
            result = resume_child_wait(kernel, *process, object, complete);
        else if (const auto object = pipe_read_object(chosen.pid, chosen.tid); object.pool)
            result = resume_pipe_read(kernel, *process, object, complete);
        else if (const auto object = pipe_write_object(chosen.pid, chosen.tid); object.pool)
            result = resume_pipe_write(kernel, *process, object, complete);
        else if ((process->resources().personality == Personality::native &&
                      native_unix_path_call(hw::syscall(thread.native_frame), kernel.scheduler(), *process) != lu::UnixPathCall::none) ||
                 (process->resources().personality == Personality::linux &&
                      linux_unix_path_call<lu::configured_native_profile()>(thread.native_frame, kernel.scheduler(), *process) != lu::UnixPathCall::none))
        {
            if (linux_udp_delay_slot) result = -95;
            else result = process->resources().personality == Personality::linux
                ? linux_bootstrap::dispatch<lu::configured_native_profile()>(kernel, chosen.pid, thread.native_frame, complete,
                    [&](hw::Syscall call, bool &done) { return dispatch(kernel, chosen.pid, call, done); })
                : dispatch(kernel, chosen.pid, hw::syscall(thread.native_frame), complete);
        }
        else if (syscall_origins.has_tcp_immediate(thread) || tcp_immediate_frame(kernel, *process, thread))
        {
            if (linux_udp_delay_slot) result = -95;
            else
            {
                complete = false; result = 0;
                if (const auto status = tcp_immediate_runtime.dispatch(kernel, *process, thread); !status)
                { runtime_failure = status; started = false; }
            }
        }
        else if (process->resources().personality == Personality::linux && linux_udp_frame(thread.native_frame))
        {
            if (linux_udp_delay_slot)
            {
                // BD has a recognized backend continuation but no immediate IO
                // capture. Complete only this unsupported call, with no UDP effects.
                result = -95; // Generic EOPNOTSUPP; bootstrap maps target errno.
            }
            else
            {
                // Publish once while live. Generic completion must not run
                // after access reset or final original-OFD release.
                complete = false;
                result = 0;
                if (const auto status = dispatch_linux_udp(kernel, *process, thread); !status)
                    static_cast<void>(linux_udp_receive_invariant(status));
            }
        }
        else
        result = process->resources().personality == Personality::linux
            ? linux_bootstrap::dispatch<lu::configured_native_profile()>(kernel, chosen.pid, thread.native_frame, complete,
                [&](hw::Syscall call, bool &done) { return dispatch(kernel, chosen.pid, call, done); },
                [&](sched::ThreadControlBlock &created) {
                    const auto prepared = prepare_native_thread(created);
                    if (!prepared) return prepared;
#ifdef OK_PRIVATE_PPC32_SIGNAL
                    if (!private_ppc_inherit(*process, thread, *process, created, true) ||
                        !private_ppc_state(*process, created)->stage_relocation(private_ppc_identity(*process, created),
                            created.native_frame, private_ppc_backend::capability()))
                    {
                        const auto retired = retire_native_thread(created);
                        if (!retired && created.entry_stack.pool)
                        {
                            stranded_entry_stacks[created.entry_stack.slot] = {{created.owner, created.tid}, created.entry_stack};
                            created.entry_stack = {};
                            created.kernel_stack_top = 0;
                            runtime_failure = retired;
                            started = false;
                        }
                        return Status::fault("private PPC clone signal context inheritance failed");
                    }
                    ppc_prepared_clone = created.tid;
#endif
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
                    if (!private_n64_inherit(*process, thread, *process, created) ||
                        !private_n64_state(*process, created)->stage_relocation(private_n64_identity(*process, created),
                            created.native_frame, private_n64_backend::capability()))
                    {
                        const auto retired = retire_native_thread(created);
                        if (!retired && created.entry_stack.pool)
                        {
                            stranded_entry_stacks[created.entry_stack.slot] = {{created.owner, created.tid}, created.entry_stack};
                            created.entry_stack = {};
                            created.kernel_stack_top = 0;
                            runtime_failure = retired;
                            started = false;
                        }
                        return Status::fault("private n64 clone signal context inheritance failed");
                    }
                    n64_prepared_clone = created.tid;
#endif
#ifdef OK_PRIVATE_MIPS_O32_SIGNAL
                    if (!private_mips_inherit(thread, created))
                    {
                        // The scheduler has not published this child yet.
                        const auto retired = retire_native_thread(created);
                        if (!retired && created.entry_stack.pool)
                        {
                            stranded_entry_stacks[created.entry_stack.slot] = {{created.owner, created.tid}, created.entry_stack};
                            created.entry_stack = {};
                            created.kernel_stack_top = 0;
                            runtime_failure = retired;
                            started = false;
                        }
                        return Status::fault("private MIPS clone signal context inheritance failed");
                    }
#endif
                    return Status::success();
                })
            : dispatch(kernel, chosen.pid, hw::syscall(thread.native_frame), complete);
        }
        process = kernel.scheduler().find(chosen.pid);
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
        if (n64_prepared_clone)
        {
            auto *child = kernel.scheduler().find_thread(chosen.pid, n64_prepared_clone);
            auto *state = process && child ? private_n64_state(*process, *child) : nullptr;
            if (result != static_cast<i64>(n64_prepared_clone) || !state ||
                !state->commit_relocation(private_n64_identity(*process, *child), child->native_frame,
                                          private_n64_backend::capability()))
            {
                runtime_failure = Status::fault("private n64 clone canonical publication failed");
                started = false; return false;
            }
        }
#endif
#ifdef OK_PRIVATE_PPC32_SIGNAL
        if (ppc_prepared_clone)
        {
            auto *child = kernel.scheduler().find_thread(chosen.pid, ppc_prepared_clone);
            auto *state = process && child ? private_ppc_state(*process, *child) : nullptr;
            if (result != static_cast<i64>(ppc_prepared_clone) || !state ||
                !state->commit_relocation(private_ppc_identity(*process, *child), child->native_frame,
                                          private_ppc_backend::capability()))
            {
                runtime_failure = Status::fault("private PPC clone canonical publication failed");
                started = false; return false;
            }
        }
#endif
        auto *resumed = kernel.scheduler().find_thread(chosen.pid, chosen.tid);
        if (process && resumed && resumed->native_active && complete)
        {
            auto &current = *resumed;
            current.syscall_pending = false;
            current.console_wait_epoch = 0;
            if (auto status = set_result(*process, current.native_frame, result); !status.ok())
            {
                runtime_failure = status;
                finish(kernel, chosen.pid, 127);
            }
        }
    }
    static_cast<void>(kernel.posix().activate(0));
    // The committing syscall's result is published before leaving all step locals.
    if (native_shutdown_pending()) return false;
    if (!runtime_failure || !started) { started = false; return false; }
    if (auto status = prune_departed_linux_udp_receives(kernel); !status)
    { runtime_failure = status; started = false; return false; }
#ifdef OK_PRIVATE_POLL_MASK
    if (!private_child_reap(kernel)) return false;
#else
    static_cast<void>(kernel.scheduler().reap_orphans());
#endif
    if (!runtime_failure) { started = false; return false; }
    return true;
}
} // namespace ok::user::native
