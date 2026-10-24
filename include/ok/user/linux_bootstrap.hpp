#pragma once

#include "ok/user/native_child_wait.hpp"
#include "ok/user/native_owned_stat_request.hpp"

#include "ok/abi/linux_native.hpp"
#include "ok/abi/linux_open.hpp"
#include "ok/abi/linux_seek.hpp"
#include "ok/abi/linux_pipe.hpp"
#include "ok/abi/linux_socketpair.hpp"
#include "ok/abi/linux_udp_call.hpp"
#include "ok/user/unix_message.hpp"
#include "ok/user/unix_pathname.hpp"
#include "ok/user/unix_pathname_operation.hpp"
#include "ok/abi/linux_memfd.hpp"
#include "ok/abi/linux_sleep.hpp"
#include "ok/abi/linux_poll.hpp"
#include "ok/abi/linux_select.hpp"
#include "ok/abi/linux_directory.hpp"
#include "ok/abi/linux_stat_call.hpp"
#include "ok/abi/linux_chmod_call.hpp"
#include "ok/abi/linux_access_call.hpp"
#include "ok/abi/linux_console_ioctl.hpp"
#include "ok/user/console_control_dispatch.hpp"
#include "ok/user/native_select.hpp"
#include "ok/user/native_futex.hpp"
#include "ok/user/linux_clone.hpp"
#include "ok/user/linux_robust.hpp"
#include "ok/user/linux_credentials.hpp"
#include "ok/user/native_signal_mask.hpp"
#include "ok/user/native_sleep.hpp"
#include "ok/arch/clock.hpp"
#include "ok/time/public_clock.hpp"
#include "ok/user/clock_set.hpp"
#include "ok/core/kernel.hpp"
#include "ok/syscall/linux.hpp"
#include "ok/uapi/syscall.h"

namespace ok::user::linux_bootstrap
{
namespace lu = abi::linux_user;
namespace hw = arch::execution;

// Copy a bounded NUL-terminated pathname one checked byte at a time. A short
// path may end at a mapping boundary; bytes beyond the terminator are untouched.
inline Status copy_pathname(const hw::AddressSpace &space, uptr address, FixedString<256> &path)
{
    path.clear();
    for (usize offset = 0; offset < 256; ++offset)
    {
        char value{};
        if (address > ~uptr{0} - offset || !hw::copy_from_user(space, &value, address + offset, 1))
            return Status::fault("Linux pathname is not readable");
        if (!value)
            return Status::success();
        if (!path.append(value))
            return Status::name_too_long("Linux pathname exceeds supported capacity");
    }
    return Status::name_too_long("Linux pathname exceeds supported capacity");
}

// The native callback is statically bound to the existing checked user-copy
// boundary. This adapter does not duplicate file/VM implementation or guess
// unsupported Linux syscall signatures. Integration must select it per image.
template <lu::Profile Profile, typename NativeCall, typename PrepareThread = RejectThreadPreparation>
i64 dispatch(Kernel &kernel, sched::ProcessId pid, hw::UserFrame &frame, bool &complete, NativeCall &&native,
             PrepareThread &&prepare = {})
{
    using Bridge = lu::NativeBridge<Profile>;
    using Codec = lu::Codec<Profile>;
    using Operation = lu::Operation;
    auto *process = kernel.scheduler().find(pid);
    if (!process || process->threads().empty())
        return -3;
    auto &space = process->resources().native_space;
    const auto unix_operation = native::linux_unix_path_call<Profile>(frame, kernel.scheduler(), *process);
    if (unix_operation != lu::UnixPathCall::none)
    {
        const auto *caller = kernel.scheduler().current_thread();
        if (!caller || caller->owner != pid || caller->state != sched::ProcessState::running) return -22;
        const auto first = Bridge::decode_call(frame, 4);
        if (!first) return -14;
        const usize count = unix_operation == lu::UnixPathCall::getsockopt ? 5 : 4;
        return native::unix_pathname<Profile>(kernel.posix(), kernel.scheduler(), *process,
            unix_operation, first.value().arguments,
            [&](std::array<u64, 6>) -> Result<std::array<u64, 6>> {
                auto copied = Bridge::decode_call_copying_stack(frame, count,
                    [&](uptr address, std::span<u8> bytes) { return hw::copy_from_user(space, bytes.data(), address, bytes.size()); });
                if (!copied) return copied.status();
                return copied.value().arguments;
            },
            [&](u64 address, std::span<u8> bytes) { return hw::copy_from_user(space, bytes.data(), static_cast<uptr>(address), bytes.size()); },
            [&](u64 address, std::span<const u8> bytes) { return hw::copy_to_user(space, static_cast<uptr>(address), bytes.data(), bytes.size()); },
            [&](u64 address, usize length) { return hw::user_range(space, static_cast<uptr>(address), length, true); });
    }
    const auto snapshot = Bridge::snapshot(frame);
    const u64 number = snapshot.integer[Codec::profile.number_register];
    using Sockets = lu::SocketPairCodec<Profile>;
    using Messages = lu::UnixMessageCodec<Profile>;
    using Memfd = lu::MemfdCodec<Profile>;
    const bool memfd_create = number == Memfd::number;
    const bool direct_socketpair = number == Sockets::numbers.direct;
    const bool direct_message = number == Messages::numbers.sendmsg || number == Messages::numbers.recvmsg;
    const bool legacy_socketcall = Sockets::numbers.legacy_socketcall &&
                                   number == Sockets::numbers.legacy_socketcall;
    auto is = [&](Operation operation) {
        const auto expected = Codec::syscall_number(operation);
        return expected && number == *expected;
    };
    std::optional<lu::StatCall> stat_call;
    if (is(Operation::statx)) stat_call = lu::StatCall::statx;
    if constexpr (lu::StatCodec<Profile>::has_kernel_stat)
    {
        if (is(Operation::fstat)) stat_call = lu::StatCall::fstat;
        else if (is(Operation::newfstatat)) stat_call = lu::StatCall::newfstatat;
        if constexpr (Profile == lu::Profile::x86_64)
        {
            if (is(Operation::stat)) stat_call = lu::StatCall::stat;
            else if (is(Operation::lstat)) stat_call = lu::StatCall::lstat;
        }
    }
    if (stat_call)
    {
        // Capture the saved frame, O32 stack word and pathname together before
        // another dispatcher branch can decode or reread user memory.
        auto captured = native::OwnedStatRequest<Profile>::capture(frame,
            [&](uptr address, std::span<u8> destination) -> Status {
                return hw::copy_from_user(space, destination.data(), address, destination.size());
            },
            [&](u64 address, FixedString<256> &path) -> Status {
                return copy_pathname(space, static_cast<uptr>(address), path);
            });
        if (!captured) return -syscall::ErrnoMapper::errno_for(captured.status());
        auto payload = captured.value().take();
        if (!payload) return -syscall::ErrnoMapper::errno_for(payload.status());
        return payload.value().execute_synchronous(
            [&](lu::StatRequest request, std::string_view path) -> Result<posix::FileStatus> {
                if (request.direct_fd || (path.empty() && request.fd != posix::at_FDCWD))
                {
                    const auto *slot = process->fd_table().entry(request.fd);
                    const auto *description = slot && slot->used
                        ? kernel.scheduler().open_description(slot->description) : nullptr;
                    if (!description || !description->used)
                        return Status::bad_descriptor("Linux fstat descriptor is not live");
                    return kernel.posix().fstat(request.fd);
                }
                return kernel.posix().statat(request.fd, path, request.flags);
            },
            [&](u64 address, usize length) -> bool {
                return hw::user_range(space, static_cast<uptr>(address), length, true);
            },
            [&](u64 address, std::span<const u8> bytes) -> Status {
                return hw::copy_to_user(space, static_cast<uptr>(address), bytes.data(), bytes.size());
            });
    }
    std::optional<lu::ChmodCall> chmod_call;
    if (is(Operation::chmod)) chmod_call = lu::ChmodCall::chmod;
    else if (is(Operation::fchmod)) chmod_call = lu::ChmodCall::fchmod;
    else if (is(Operation::fchmodat)) chmod_call = lu::ChmodCall::fchmodat;
    else if (is(Operation::fchmodat2)) chmod_call = lu::ChmodCall::fchmodat2;
    std::optional<lu::AccessCall> access_call;
    if (is(Operation::access)) access_call = lu::AccessCall::access;
    else if (is(Operation::faccessat)) access_call = lu::AccessCall::faccessat;
    else if (is(Operation::faccessat2)) access_call = lu::AccessCall::faccessat2;
    const auto credential_call = linux_credentials::select<Profile>(number);
    if (credential_call || access_call || is(Operation::ioctl))
    {
        // An exported adapter caller cannot select another PCB merely by
        // supplying its PID. Bind identity access before decoding user memory.
        const auto *thread = kernel.scheduler().current_thread();
        if (!thread || thread->owner != pid || thread->state != sched::ProcessState::running)
            return -22;
    }
    usize count = 0;
    if (credential_call) count = credential_call->arguments;
    else if (access_call) count = lu::AccessCallAdapter<Profile>::argument_count(*access_call);
    else if (direct_socketpair) count = 4;
    else if (direct_message) count = 3;
    else if (legacy_socketcall) count = 2;
    else if (chmod_call) count = lu::ChmodCallAdapter<Profile>::argument_count(*chmod_call);
    else if (is(Operation::fork) || is(Operation::getpid) || is(Operation::getppid) || is(Operation::gettid) ||
        is(Operation::getpgrp) || is(Operation::setsid) ||
        is(Operation::getuid) || is(Operation::geteuid) || is(Operation::getgid) || is(Operation::getegid) ||
        is(Operation::getuid32) || is(Operation::geteuid32) || is(Operation::getgid32) || is(Operation::getegid32))
        count = 0;
    else if (is(Operation::ioctl)) count = 3;
    else if (is(Operation::getpgid) || is(Operation::getsid)) count = 1;
    else if (is(Operation::setpgid)) count = 2;
    else if (is(Operation::pread64) || is(Operation::pwrite64))
        count = Codec::profile.word_bytes == 8 ? 4 : (Codec::profile.aligned_64bit_pairs ? 6 : 5);
    else if (is(Operation::ftruncate) || is(Operation::ftruncate64) ||
             is(Operation::truncate) || is(Operation::truncate64))
        count = Codec::profile.word_bytes == 8 ? 2 :
            ((is(Operation::ftruncate64) || is(Operation::truncate64)) ?
                 (Codec::profile.aligned_64bit_pairs ? 4 : 3) : 2);
    else if (is(Operation::select) || is(Operation::_newselect))
        count = lu::SelectCodec<Profile>::old_select && is(Operation::select) ? 1 : 5;
    else if (is(Operation::pselect6) || is(Operation::pselect6_time64)) count = 6;
    else if (is(Operation::futex) || is(Operation::futex_time64)) count = 4;
    else if (is(Operation::set_robust_list)) count = 2;
    else if (is(Operation::get_robust_list)) count = 3;
    else if (is(Operation::rt_sigprocmask)) count = 4;
    else if (is(Operation::poll)) count = 3;
    else if (is(Operation::getdents64)) count = 3;
    else if (is(Operation::clock_settime) || is(Operation::clock_settime32) || is(Operation::clock_settime64)) count = 2;
    else if (is(Operation::umask)) count = 1;
    else if (is(Operation::mkdir)) count = 2;
    else if (is(Operation::mkdirat)) count = 3;
    else if (is(Operation::unlink) || is(Operation::rmdir)) count = 1;
    else if (is(Operation::unlinkat)) count = 3;
    else if (is(Operation::rename)) count = 2;
    else if (is(Operation::renameat)) count = 4;
    else if (is(Operation::renameat2)) count = 5;
    else if (is(Operation::ppoll) || is(Operation::ppoll_time64)) count = 5;
    else if (is(Operation::lseek)) count = 3;
    else if (is(Operation::_llseek)) count = 5;
    else if (is(Operation::clone)) count = 2;
    else if (is(Operation::execve)) count = 3;
    else if (is(Operation::wait4)) count = 4;
    else if (is(Operation::waitid)) count = 5;
    else if (is(Operation::chdir) || is(Operation::fchdir)) count = 1;
    else if (is(Operation::getcwd) || (is(Operation::nanosleep) || is(Operation::nanosleep_time32))) count = 2;
    else if ((is(Operation::clock_nanosleep) || is(Operation::clock_nanosleep_time32)) || is(Operation::clock_nanosleep_time64)) count = 4;
    else if ((is(Operation::clock_getres) || is(Operation::clock_getres_time32)) || is(Operation::clock_getres_time64)) count = 2;
    else if (is(Operation::readlink)) count = 3;
    else if (is(Operation::readlinkat)) count = 4;
    else if (is(Operation::pipe)) count = lu::PipeCodec<Profile>::register_pair ? 0 : 1;
    else if (is(Operation::pipe2)) count = 2;
    else if (is(Operation::dup)) count = 1;
    else if (is(Operation::dup2)) count = 2;
    else if (is(Operation::dup3) || is(Operation::fcntl) || is(Operation::fcntl64)) count = 3;
    else if (is(Operation::read) || is(Operation::write) || is(Operation::readv) || is(Operation::writev) ||
        is(Operation::getrandom) ||
        is(Operation::mprotect))
        count = 3;
    else if (is(Operation::brk) || is(Operation::fsync) || is(Operation::fdatasync) || is(Operation::close) || is(Operation::exit) ||
             is(Operation::exit_group) || is(Operation::set_tid_address) || is(Operation::set_thread_area) ||
             is(Operation::arm_set_tls))
        count = 1;
    else if (is(Operation::munmap) || is(Operation::arch_prctl) || is(Operation::clock_gettime) ||
             is(Operation::clock_gettime32) || is(Operation::clock_gettime64))
        count = 2;
    else if (is(Operation::open)) count = 3;
    else if (is(Operation::creat) || memfd_create) count = 2;
    else if (is(Operation::openat))
        count = 4;
    else if (is(Operation::mmap2) || (Profile != lu::Profile::i386 && is(Operation::mmap)))
        count = 6;
    else
        return -38;
    auto decoded = Bridge::decode_call_copying_stack(frame, count, [&](uptr address, std::span<u8> destination) {
        return hw::copy_from_user(space, destination.data(), address, destination.size());
    });
    if (!decoded)
        return -14;
    const auto &a = decoded.value().arguments;
    if (is(Operation::ioctl))
    {
        using Ioctl = lu::ConsoleIoctlCodec<Profile>;
        std::optional<sched::ConsoleControlOperation> selected;
        if (const auto operation = Ioctl::operation(a[1]))
            switch (*operation)
            {
            case lu::ConsoleIoctlOperation::acquire: selected = sched::ConsoleControlOperation::acquire; break;
            case lu::ConsoleIoctlOperation::get_sid: selected = sched::ConsoleControlOperation::get_session; break;
            case lu::ConsoleIoctlOperation::get_pgrp: selected = sched::ConsoleControlOperation::get_foreground; break;
            case lu::ConsoleIoctlOperation::set_pgrp: selected = sched::ConsoleControlOperation::set_foreground; break;
            default: break;
            }
        const u64 argument = selected == sched::ConsoleControlOperation::acquire
            ? static_cast<u32>(Ioctl::scalar_argument(a[2])) : a[2] & Codec::word_mask;
        return console_control::dispatch<Codec::profile.big_endian>(
            Ioctl::scalar_argument(a[0]), selected, argument,
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
    }
    if (direct_message || (legacy_socketcall && (a[0] == 16 || a[0] == 17)))
    {
        const auto *caller = kernel.scheduler().current_thread();
        if (!caller || caller->owner != pid || caller->state != sched::ProcessState::running) return -22;
        std::array<u64, 3> arguments{};
        const bool receive = direct_message ? number == Messages::numbers.recvmsg : a[0] == 17;
        if (legacy_socketcall)
        {
            if (!Messages::range(a[1], 12)) return -14;
            std::array<u8, 12> bytes{};
            if (!hw::copy_from_user(space, bytes.data(), static_cast<uptr>(a[1]), bytes.size())) return -14;
            for (usize index = 0; index < arguments.size(); ++index)
                arguments[index] = Messages::load(std::span<const u8>{bytes}.subspan(index * 4, 4));
        }
        else for (usize index = 0; index < arguments.size(); ++index) arguments[index] = a[index];
        return native::unix_message<Profile>(kernel.posix(), receive, arguments[0], arguments[1], arguments[2],
            [&](u64 address, std::span<u8> bytes) {
                return hw::copy_from_user(space, bytes.data(), static_cast<uptr>(address), bytes.size());
            },
            [&](u64 address, std::span<const u8> bytes) {
                return hw::copy_to_user(space, static_cast<uptr>(address), bytes.data(), bytes.size());
            },
            [&](u64 address, usize length) { return hw::user_range(space, static_cast<uptr>(address), length, true); });
    }
    if (memfd_create)
    {
        auto flags = Memfd::decode_flags(static_cast<u32>(a[1]));
        if (!flags) return -syscall::ErrnoMapper::errno_for(flags.status());
        FixedString<256> name;
        if (auto copied = copy_pathname(space, static_cast<uptr>(a[0]), name); !copied)
            return -syscall::ErrnoMapper::errno_for(copied);
        auto created = kernel.posix().memfd_create(name.view(), flags.value());
        return created ? static_cast<i64>(created.value()) : -syscall::ErrnoMapper::errno_for(created.status());
    }
    if (direct_socketpair || legacy_socketcall)
    {
        std::array<u64, 4> arguments{};
        if (legacy_socketcall)
        {
            // Linux socketcall selector 8 is SYS_SOCKETPAIR. Other selectors
            // remain unsupported and do not acquire socket resources.
            if (a[0] != 8) return -38;
            std::array<u8, 16> bytes{};
            if (!hw::copy_from_user(space, bytes.data(), static_cast<uptr>(a[1]), bytes.size()))
                return -14;
            for (usize item = 0; item < arguments.size(); ++item)
                for (usize byte = 0; byte < 4; ++byte)
                {
                    const usize shift = Codec::profile.big_endian ? 3 - byte : byte;
                    arguments[item] |= static_cast<u64>(bytes[item * 4 + byte]) << (8 * shift);
                }
        }
        else
            for (usize item = 0; item < arguments.size(); ++item) arguments[item] = a[item];
        if (arguments[0] != 1) return -97; // AF_UNIX only.
        auto flags = Sockets::decode_type(static_cast<u32>(arguments[1]));
        if (!flags || arguments[1] > 0xffffffffu) return -22;
        if (arguments[2] != 0) return -93; // Protocol zero selects the stream default.
        auto fds = kernel.posix().unix_socketpair(flags.value());
        if (!fds) return -syscall::ErrnoMapper::errno_for(fds.status());
        const auto encoded = lu::PipeCodec<Profile>::encode(fds.value());
        if (!hw::copy_to_user(space, static_cast<uptr>(arguments[3]), encoded.data(), encoded.size()))
        {
            static_cast<void>(kernel.posix().close(fds.value()[0]));
            static_cast<void>(kernel.posix().close(fds.value()[1]));
            return -14;
        }
        return 0;
    }
    if (credential_call)
        return linux_credentials::dispatch<Profile>(*process, *credential_call, a,
            [&](u64 address, std::span<const u8> bytes) {
                return hw::copy_to_user(space, static_cast<uptr>(address), bytes.data(), bytes.size());
            });
    if (is(Operation::rt_sigprocmask))
    {
        auto *thread = kernel.scheduler().current_thread();
        if (!thread || thread->owner != pid || thread->state != sched::ProcessState::running) return -22;
        return native::update_signal_mask<Profile>(thread->native_signal_mask, static_cast<i32>(a[0]), a[1], a[2], a[3],
            [&](u64 address, std::span<std::byte> bytes) {
                return hw::copy_from_user(space, bytes.data(), static_cast<uptr>(address), bytes.size());
            },
            [&](u64 address, std::span<const std::byte> bytes) {
                return hw::copy_to_user(space, static_cast<uptr>(address), bytes.data(), bytes.size());
            });
    }
    if (is(Operation::set_robust_list) || is(Operation::get_robust_list))
        return robust_registration<Profile>(kernel, pid, is(Operation::set_robust_list), a);
    auto shared = [&](syscall::Number operation) {
        hw::Syscall call{static_cast<uptr>(operation), {}};
        for (usize i = 0; i < call.arguments.size(); ++i)
            call.arguments[i] = static_cast<uptr>(a[i]);
        return native(call, complete);
    };
    if (is(Operation::futex) || is(Operation::futex_time64))
    {
        const u32 operation = static_cast<u32>(a[1]);
        if (operation == 3 || operation == 131 || operation == 4 || operation == 132)
        {
            const bool compare = (operation & 127U) == 4;
            // REQUEUE's fourth input is a scalar, never a timeout. Only
            // CMP_REQUEUE needs input six; WAIT/WAKE keep four-input decoding.
            const auto tail = Bridge::decode_call_copying_stack(frame, compare ? 6 : 5,
                [&](uptr address, std::span<u8> destination) {
                    return hw::copy_from_user(space, destination.data(), address, destination.size());
                });
            if (!tail) return -14;
            const auto &arguments = tail.value().arguments;
            return native::futex_requeue(kernel, pid,
                {static_cast<uptr>(arguments[0]), static_cast<uptr>(arguments[4]),
                 static_cast<u32>(arguments[2]), static_cast<u32>(arguments[3]),
                 compare ? static_cast<u32>(arguments[5]) : 0U, compare, Codec::profile.big_endian,
                 operation & 128 ? native::FutexKeyDomain::private_address_space
                                 : native::FutexKeyDomain::anonymous_shared_operation});
        }
        if (operation == 5 || operation == 133)
        {
            // Only WAKE_OP consumes the tail. O32 must copy its stacked words;
            // existing four-input WAIT/WAKE never inspect that unused stack.
            const auto tail = Bridge::decode_call_copying_stack(frame, 6,
                [&](uptr address, std::span<u8> destination) {
                    return hw::copy_from_user(space, destination.data(), address, destination.size());
                });
            if (!tail) return -14;
            const auto &arguments = tail.value().arguments;
            return native::futex_wake_operation(kernel, pid,
                {static_cast<uptr>(arguments[0]), static_cast<uptr>(arguments[4]),
                 static_cast<u32>(arguments[2]), static_cast<u32>(arguments[3]),
                 static_cast<u32>(arguments[5]), Codec::profile.big_endian,
                 operation & 128 ? native::FutexKeyDomain::private_address_space
                                 : native::FutexKeyDomain::anonymous_shared_operation});
        }
        if (operation != 0 && operation != 1 && operation != 128 && operation != 129)
            return -38;
        native::FutexRequest request;
        request.operation = (operation & 1) == 0 ? native::FutexOperation::wait : native::FutexOperation::wake;
        request.domain = operation & 128 ? native::FutexKeyDomain::private_address_space
                                        : native::FutexKeyDomain::anonymous_shared_operation;
        request.address = static_cast<uptr>(a[0]);
        request.value = static_cast<u32>(a[2]);
        request.big_endian = Codec::profile.big_endian;
        if (request.operation == native::FutexOperation::wait && a[3])
        {
            const auto format = Codec::profile.word_bytes == 8 || is(Operation::futex_time64)
                                    ? lu::TimeFormat::kernel_timespec
                                    : lu::TimeFormat::legacy_timespec;
            std::array<u8, 16> bytes{};
            const auto size = Codec::timespec_size(format);
            if (!hw::copy_from_user(space, bytes.data(), static_cast<uptr>(a[3]), size))
                return -14;
            auto timeout = Codec::decode_timespec({bytes.data(), size}, format);
            if (!timeout || timeout.value().seconds < 0 || timeout.value().nanoseconds < 0 ||
                timeout.value().nanoseconds >= 1000000000)
                return -22;
            request.timeout = {timeout.value().seconds, timeout.value().nanoseconds};
            request.timed = true;
        }
        return native::futex(kernel, pid, request, complete);
    }
    if (chmod_call)
    {
        return lu::ChmodCallAdapter<Profile>::execute(*chmod_call, std::span{a}.first(count),
            [&](u64 address, FixedString<256> &path) -> Status {
                return copy_pathname(space, static_cast<uptr>(address), path);
            },
            [&](lu::ChmodRequest request, std::string_view path) -> fs::ModeChangeResult {
                if (request.direct_fd)
                    return kernel.posix().change_mode_fd(request.fd, request.permissions);
                return kernel.posix().change_mode_at(request.fd, path, request.permissions, request.flags);
            });
    }
    if (access_call)
    {
        return lu::AccessCallAdapter<Profile>::execute(*access_call, std::span{a}.first(count),
            [&](u64 address, FixedString<256> &path) -> Status {
                return copy_pathname(space, static_cast<uptr>(address), path);
            },
            [&](lu::AccessRequest request, std::string_view path) -> Status {
                return kernel.posix().faccessat(request.fd, path, request.mode, request.flags);
            });
    }
    if (is(Operation::umask))
    {
        hw::Syscall call{static_cast<uptr>(syscall::Number::umask), {static_cast<u32>(a[0])}};
        return native(call, complete);
    }
    if (is(Operation::mkdir) || is(Operation::mkdirat))
    {
        const bool relative = is(Operation::mkdirat);
        FixedString<256> path;
        const auto copied = copy_pathname(space, static_cast<uptr>(a[relative ? 1 : 0]), path);
        if (!copied) return -syscall::ErrnoMapper::errno_for(copied);
        const auto dirfd = relative ? static_cast<i32>(static_cast<u32>(a[0])) : posix::at_FDCWD;
        // Linux umode_t is unsigned short; mkdir admits only rwx and sticky.
        // Common creation applies umask and parent setgid inheritance once.
        const u32 mode = static_cast<u16>(a[relative ? 2 : 1]) & 01777u;
        const auto status = kernel.posix().mkdirat(dirfd, path.view(), mode);
        return status ? 0 : -syscall::ErrnoMapper::errno_for(status);
    }
    if (is(Operation::getdents64))
    {
        using Directory = lu::DirectoryCodec<Profile>;
        const auto fd = static_cast<i32>(static_cast<u32>(a[0]));
        const u32 requested = static_cast<u32>(a[2]);
        const usize capacity = requested < 4096 ? requested : 4096;
        usize written = 0;
        auto result = kernel.posix().enumerate_directory<posix::DirectoryPolicy::linux>(fd,
            [&](const fs::DirectoryEntry &entry, u64 cookie) -> Result<usize> {
                const usize size = Directory::record_size(entry.name.view().size());
                if (requested > 0x7fffffffU || size > capacity - written)
                    return Status::invalid_argument("Linux directory output count too small");
                const uptr address = static_cast<uptr>(a[1]);
                if (written > ~uptr{0} - address || !hw::user_range(space, address + written, size, true))
                    return Status::fault("Linux directory output range");
                std::array<u8, Directory::capacity> bytes{};
                auto encoded = Directory::encode(entry, cookie, bytes);
                if (!encoded) return encoded.status();
                if (!hw::copy_to_user(space, address + written, bytes.data(), size))
                    return Status::fault("Linux directory output copy");
                written += size;
                return size;
            });
        return result ? static_cast<i64>(result.value()) : -syscall::ErrnoMapper::errno_for(result.status());
    }
    if (is(Operation::pread64) || is(Operation::pwrite64))
    {
        constexpr usize first = Codec::profile.aligned_64bit_pairs ? 4 : 3;
        const auto offset = Codec::decode_argument64(decoded.value(), first);
        if (!offset || static_cast<i64>(offset.value()) < 0) return -22;
        const auto fd = static_cast<i32>(static_cast<u32>(a[0]));
        const auto *entry = process->fd_table().entry(fd);
        const auto *description = entry && entry->used ? kernel.scheduler().open_description(entry->description) : nullptr;
        if (!description || !description->used) return -9;
        if ((!description->node && !description->native_shared_file) || description->pipe != sched::invalid_description ||
            description->console || description->input_console) return -29;
        const bool writing = is(Operation::pwrite64);
        if (writing ? !description->writable : !description->readable) return -9;
        if (a[2] > (Codec::word_mask >> 1)) return -22;
        constexpr u64 limit = Codec::word_mask >> 1;
        if (offset.value() > limit) return -75;
        const auto size = a[2] < sched::pipe_capacity ? static_cast<usize>(a[2]) : sched::pipe_capacity;
        // Bound the admitted endpoint even when hosted models have wider usize.
        if (size > limit - offset.value()) return -75;
        std::array<std::byte, sched::pipe_capacity> buffer{};
        if (writing)
        {
            if (!hw::copy_from_user(space, buffer.data(), static_cast<uptr>(a[1]), size)) return -14;
            auto result = kernel.posix().pwrite_with_policy<posix::PositionedWritePolicy::linux_append>(
                fd, {buffer.data(), size}, static_cast<usize>(offset.value()), limit);
            return result ? static_cast<i64>(result.value()) : -syscall::ErrnoMapper::errno_for(result.status());
        }
        if (!hw::user_range(space, static_cast<uptr>(a[1]), size, true)) return -14;
        auto result = kernel.posix().pread(fd, {buffer.data(), size}, static_cast<usize>(offset.value()));
        if (!result) return -syscall::ErrnoMapper::errno_for(result.status());
        if (!hw::copy_to_user(space, static_cast<uptr>(a[1]), buffer.data(), result.value())) return -14;
        return static_cast<i64>(result.value());
    }
    if (is(Operation::ftruncate) || is(Operation::ftruncate64) ||
        is(Operation::truncate) || is(Operation::truncate64))
    {
        const bool wide = Codec::profile.word_bytes == 8 ||
            is(Operation::ftruncate64) || is(Operation::truncate64);
        u64 length = a[1];
        if (wide)
        {
            constexpr usize first = Codec::profile.aligned_64bit_pairs ? 2 : 1;
            const auto decoded_length = Codec::decode_argument64(decoded.value(), first);
            if (!decoded_length) return -22;
            length = decoded_length.value();
            if (length > 0x7fffffffffffffffULL) return -22;
        }
        else if (static_cast<i32>(static_cast<u32>(length)) < 0) return -22;
        if (length > static_cast<u64>(~usize{0}))
            return -static_cast<i64>(*Codec::errno_number(lu::Error::e_overflow));
        Status status;
        if (is(Operation::ftruncate) || is(Operation::ftruncate64))
            status = kernel.posix().ftruncate(static_cast<i32>(static_cast<u32>(a[0])),
                                              static_cast<usize>(length));
        else
        {
            FixedString<256> path;
            if (status = copy_pathname(space, static_cast<uptr>(a[0]), path); !status)
                return -syscall::ErrnoMapper::errno_for(status);
            status = kernel.posix().truncate(path.view(), static_cast<usize>(length));
        }
        return status ? 0 : -syscall::ErrnoMapper::errno_for(status);
    }
    if (is(Operation::select) || is(Operation::_newselect) || is(Operation::pselect6) || is(Operation::pselect6_time64))
    {
        using Select = lu::SelectCodec<Profile>;
        using Poll = lu::PollCodec<Profile>;
        const bool precise = is(Operation::pselect6) || is(Operation::pselect6_time64);
        std::array<u64, 5> arguments{a[0], a[1], a[2], a[3], a[4]};
        if constexpr (Select::old_select)
            if (is(Operation::select))
            {
                std::array<u8, Select::old_argument_bytes> bytes{};
                if (!hw::copy_from_user(space, bytes.data(), static_cast<uptr>(a[0]), bytes.size())) return -14;
                arguments = Select::old_arguments(bytes);
            }
        u64 mask_pointer{}, mask_size{};
        if (precise && a[5])
        {
            std::array<u8, Select::signal_argument_bytes> bytes{};
            if (!hw::copy_from_user(space, bytes.data(), static_cast<uptr>(a[5]), bytes.size())) return -14;
            mask_pointer = Poll::word(std::span{bytes}.first(Select::word_bytes));
            mask_size = Poll::word(std::span{bytes}.subspan(Select::word_bytes));
        }
        native::SelectRequest request{};
        auto &readiness = request.readiness;
        readiness.big_endian = Codec::profile.big_endian;
        request.sets.word_bytes = Select::word_bytes;
        const auto format = Codec::profile.word_bytes == 8 || is(Operation::pselect6_time64)
            ? lu::TimeFormat::kernel_timespec : lu::TimeFormat::legacy_timespec;
        const auto time_bytes = precise ? Codec::timespec_size(format) : 2 * Select::word_bytes;
        lu::Timespec interval{};
        if (arguments[4])
        {
            std::array<u8, 16> bytes{};
            if (!hw::copy_from_user(space, bytes.data(), static_cast<uptr>(arguments[4]), time_bytes)) return -14;
            auto time = precise ? Codec::decode_timespec({bytes.data(), time_bytes}, format)
                                : Select::timeval({bytes.data(), time_bytes});
            if (!time || time.value().seconds < 0) return -22;
            interval = time.value();
            readiness.immediate = !interval.seconds && !interval.nanoseconds;
            readiness.timed = !readiness.immediate;
        }
        if (readiness.timed)
        {
            auto now = arch::monotonic_nanoseconds();
            if (!now) return -syscall::ErrnoMapper::errno_for(now.status());
            const auto end = Poll::end_time(now.value(), interval);
            const auto deadline = lu::linux_sleep_deadline(0, {end.seconds, end.nanoseconds}, true);
            readiness.timeout = {static_cast<i64>(deadline / 1000000000), static_cast<i64>(deadline % 1000000000)};
            readiness.absolute = true;
            readiness.timeout_output = {static_cast<uptr>(arguments[4]), end,
                static_cast<u8>(time_bytes / 2), Codec::profile.big_endian, precise ? native::PollTimeoutFormat::timespec : native::PollTimeoutFormat::timeval};
        }
        native::PollSignalMaskScope mask_scope{kernel, pid, complete};
        if (mask_pointer)
        {
            using Mask = native::SignalMaskCodec<Profile>;
            if (mask_size != Mask::bytes) return -22;
            std::array<std::byte, Mask::bytes> mask{};
            if (!hw::copy_from_user(space, mask.data(), static_cast<uptr>(mask_pointer), mask.size())) return -14;
            if (const auto result = mask_scope.begin(Mask::sanitize(Mask::decode(mask))); result) return result;
        }
        auto failed_select = [&](i64 error) {
            if (readiness.timeout_output.address)
                if (auto now = arch::monotonic_nanoseconds(); now)
                    native::finish_poll_timeout(space, readiness.timeout_output, now.value());
            return error;
        };
        const auto count = std::bit_cast<i32>(static_cast<u32>(arguments[0]));
        if (count < 0) return failed_select(-22);
        // Linux clamps to its allocated fdtable, not RLIMIT or libc FD_SETSIZE.
        request.sets.count = static_cast<usize>(count) > sched::max_process_fds ? sched::max_process_fds : static_cast<usize>(count);
        const auto bytes_count = Select::mask_bytes(request.sets.count);
        for (usize set = 0; set < request.sets.output.size(); ++set)
        {
            request.sets.output[set] = static_cast<uptr>(arguments[set + 1]);
            if (!bytes_count || !request.sets.output[set]) continue;
            std::array<u8, Select::word_bytes> bytes{};
            if (!hw::copy_from_user(space, bytes.data(), request.sets.output[set], bytes_count)) return failed_select(-14);
            request.sets.interests[set] = static_cast<u32>(Poll::word(bytes)) & Select::valid_bits(request.sets.count);
        }
        return native::begin_select(kernel, pid, request, complete);
    }
    if (is(Operation::poll) || is(Operation::ppoll) || is(Operation::ppoll_time64))
    {
        using Poll = lu::PollCodec<Profile>;
        const bool precise = !is(Operation::poll);
        const auto format = Codec::profile.word_bytes == 8 || is(Operation::ppoll_time64)
            ? lu::TimeFormat::kernel_timespec : lu::TimeFormat::legacy_timespec;
        native::PollRequest request{};
        request.output = static_cast<uptr>(a[0]);
        request.big_endian = Codec::profile.big_endian;
        request.linux_events = true;
        request.read_normal = Poll::read_normal;
        request.write_normal = Poll::write_normal;
        lu::Timespec interval{};
        bool finite{};
        if (precise)
        {
            if (a[2])
            {
                std::array<u8, 16> bytes{};
                const auto size = Codec::timespec_size(format);
                if (!hw::copy_from_user(space, bytes.data(), static_cast<uptr>(a[2]), size)) return -14;
                auto time = Codec::decode_timespec({bytes.data(), size}, format);
                if (!time || time.value().seconds < 0) return -22;
                interval = time.value(); finite = true;
            }
        }
        else
        {
            const auto milliseconds = static_cast<i32>(static_cast<u32>(a[2]));
            if (milliseconds >= 0)
            {
                interval = {milliseconds / 1000, (milliseconds % 1000) * 1000000};
                finite = true;
            }
        }
        request.immediate = finite && !interval.seconds && !interval.nanoseconds;
        request.timed = finite && !request.immediate;
        if (request.timed)
        {
            auto now = arch::monotonic_nanoseconds();
            if (!now) return -syscall::ErrnoMapper::errno_for(now.status());
            const auto end = Poll::end_time(now.value(), interval);
            const auto deadline = lu::linux_sleep_deadline(0, {end.seconds, end.nanoseconds}, true);
            request.timeout = {static_cast<i64>(deadline / 1000000000), static_cast<i64>(deadline % 1000000000)};
            request.absolute = true;
            if (precise)
                request.timeout_output = {static_cast<uptr>(a[2]), end,
                    static_cast<u8>(Codec::timespec_size(format) / 2), Codec::profile.big_endian};
        }
        // Linux validates timeout, then mask, then nfds and descriptor input.
        native::PollSignalMaskScope mask_scope{kernel, pid, complete};
        if (precise && a[3])
        {
            using Mask = native::SignalMaskCodec<Profile>;
            if (a[4] != Mask::bytes) return -22;
            std::array<std::byte, Mask::bytes> mask{};
            if (!hw::copy_from_user(space, mask.data(), static_cast<uptr>(a[3]), mask.size())) return -14;
            if (const auto result = mask_scope.begin(Mask::sanitize(Mask::decode(mask))); result) return result;
        }
        auto failed_poll = [&](i64 error) {
            if (request.timeout_output.address)
                if (auto now = arch::monotonic_nanoseconds(); now)
                    native::finish_poll_timeout(space, request.timeout_output, now.value());
            return error;
        };
        // The Linux syscall takes unsigned int even where musl nfds_t is long.
        const auto nfds = static_cast<u32>(a[1]);
        if (nfds > sched::max_process_fds) return failed_poll(-22);
        std::array<u8, Poll::record_size * sched::max_process_fds> bytes{};
        if (!hw::copy_from_user(space, bytes.data(), request.output, nfds * Poll::record_size))
            return failed_poll(-14);
        std::array<ok_pollfd, sched::max_process_fds> descriptors{};
        for (usize i = 0; i < nfds; ++i)
            descriptors[i] = Poll::decode(std::span<const u8, Poll::record_size>{bytes.data() + i * Poll::record_size, Poll::record_size});
        request.descriptors = {descriptors.data(), nfds};
        return native::begin_poll(kernel, pid, request, complete);
    }
    if ((is(Operation::nanosleep) || is(Operation::nanosleep_time32)) || (is(Operation::clock_nanosleep) || is(Operation::clock_nanosleep_time32)) || is(Operation::clock_nanosleep_time64))
    {
        const bool clock_sleep = !(is(Operation::nanosleep) || is(Operation::nanosleep_time32));
        const i32 clock = clock_sleep ? static_cast<i32>(static_cast<u32>(a[0])) : 1;
        const bool absolute = clock_sleep && (static_cast<u32>(a[1]) & 1U);
        // Relative realtime sleep measures elapsed time. Absolute realtime
        // timers require a separate wall-clock adjustment and wake contract.
        if (clock != 1 && (clock != 0 || absolute)) return -22;
        const auto format = Codec::profile.word_bytes == 8 || is(Operation::clock_nanosleep_time64)
            ? lu::TimeFormat::kernel_timespec : lu::TimeFormat::legacy_timespec;
        std::array<u8, 16> bytes{};
        const auto size = Codec::timespec_size(format);
        if (!hw::copy_from_user(space, bytes.data(), static_cast<uptr>(a[clock_sleep ? 2 : 0]), size))
            return -14;
        auto request = Codec::decode_timespec({bytes.data(), size}, format);
        if (!request || request.value().seconds < 0) return -22;
        auto now = arch::monotonic_nanoseconds();
        if (!now) return -syscall::ErrnoMapper::errno_for(now.status());
        const auto deadline = lu::linux_sleep_deadline(now.value(), request.value(), absolute);
        const posix::ClockTime target{static_cast<i64>(deadline / 1000000000),
                                      static_cast<i64>(deadline % 1000000000)};
        auto status = native::begin_sleep(kernel, pid, now.value(), target, true, complete);
        return status ? 0 : -syscall::ErrnoMapper::errno_for(status);
    }
    if (is(Operation::lseek) || is(Operation::_llseek))
    {
        using Seek = lu::SeekCodec<Profile>;
        const bool wide = is(Operation::_llseek);
        const auto fd = static_cast<i32>(static_cast<u32>(a[0]));
        const auto whence = static_cast<u32>(a[wide ? 4 : 2]);
        const auto *slot = process->resources().files.entry(fd);
        const auto *description = slot && slot->used ? kernel.scheduler().open_description(slot->description) : nullptr;
        if (!description) return -9;
        // Linux checks fd, then SEEK_MAX, then the endpoint's seek capability.
        if (whence > 4) return -22;
        if (description->console || description->input_console ||
            (!description->node && !description->native_shared_file) ||
            description->pipe != sched::invalid_description) return -29;
        // SEEK_DATA/HOLE require filesystem extent semantics outside this slice.
        if (whence > 2) return -22;
        if (whence == 2 && description->node)
        {
            auto metadata = description->node->query_metadata();
            if (!metadata) return -syscall::ErrnoMapper::errno_for(metadata.status());
            // Directory cookies have no byte-size-relative end in this profile.
            if (metadata.value().type == fs::NodeType::directory) return -22;
        }
        const i64 offset = wide ? Seek::wide_offset(a[1], a[2]) : Seek::word_offset(a[1]);
        auto result = kernel.posix().seek(fd, offset, static_cast<posix::SeekWhence>(whence));
        if (!result) return -syscall::ErrnoMapper::errno_for(result.status());
        if (!wide)
        {
            // Raw lseek returns native-word off_t. Linux applies this conversion
            // after vfs_llseek; the common service may impose a smaller capacity.
            if constexpr (Codec::profile.word_bytes == 4)
                if (result.value() > 0x7fffffffu) return -75;
            return static_cast<i64>(result.value());
        }
        // Linux commits the shared position before copying the result. EFAULT
        // must retain that position; do not preflight or undo the successful seek.
        const auto bytes = Seek::encode(result.value());
        return hw::copy_to_user(space, static_cast<uptr>(a[3]), bytes.data(), bytes.size()) ? 0 : -14;
    }
    if (is(Operation::pipe) || is(Operation::pipe2))
    {
        using Pipes = lu::PipeCodec<Profile>;
        const bool registers = Pipes::register_pair && is(Operation::pipe);
        auto flags = Pipes::decode_flags(is(Operation::pipe2) ? static_cast<u32>(a[1]) : 0);
        if (!flags) return -22;
        const auto output = static_cast<uptr>(a[0]);
        // Linux reserves resources before copyout. Common admission preserves
        // global ENFILE and per-process EMFILE precedence over output EFAULT.
        auto fds = kernel.posix().pipe(flags.value());
        if (!fds) return -syscall::ErrnoMapper::errno_for(fds.status());
        if (registers)
        {
            // Linux sysm_pipe/musl MIPS assembly uses v0/v1, not a user pointer.
            // Normal completion subsequently writes v0 and a3, preserving v1.
            frame.registers[3] = static_cast<uptr>(fds.value()[1]);
            return fds.value()[0];
        }
        const auto bytes = Pipes::encode(fds.value());
        if (!hw::copy_to_user(space, output, bytes.data(), bytes.size()))
        {
            static_cast<void>(kernel.posix().close(fds.value()[0]));
            static_cast<void>(kernel.posix().close(fds.value()[1]));
            return -14;
        }
        return 0;
    }
    if (is(Operation::dup) || is(Operation::dup2) || is(Operation::dup3) ||
        is(Operation::fcntl) || is(Operation::fcntl64))
    {
        using Flags = lu::OpenCodec<Profile>;
        const auto fd = static_cast<i32>(a[0]);
        const auto argument = static_cast<u32>(a[2]);
        const auto target = static_cast<i32>(a[1]);
        constexpr auto capacity = static_cast<u32>(posix::max_open_files);
        // Linux dup3 validates flags and equality before looking up oldfd.
        if (is(Operation::dup3) && ((argument & ~Flags::cloexec) || fd == target))
            return -22;
        const auto *slot = process->resources().files.entry(fd);
        const auto *description = slot && slot->used ? kernel.scheduler().open_description(slot->description) : nullptr;
        if (!description)
            return -9;
        auto result_value = [](const auto &result) -> i64 {
            return result ? static_cast<i64>(result.value()) : -syscall::ErrnoMapper::errno_for(result.status());
        };
        if (is(Operation::dup))
            return result_value(kernel.posix().duplicate(fd));
        if (is(Operation::dup2) || is(Operation::dup3))
        {
            if (static_cast<u32>(target) >= capacity) return -9;
            return result_value(kernel.posix().duplicate_to(fd, target,
                is(Operation::dup3) && (argument & Flags::cloexec) ? posix::o_CLOEXEC : 0));
        }
        const auto command = static_cast<u32>(a[1]);
        switch (command)
        {
        case 0: // F_DUPFD
        case 1030: // F_DUPFD_CLOEXEC
            if (argument >= capacity) return -22;
            return result_value(kernel.posix().duplicate(fd, static_cast<i32>(argument),
                command == 1030 ? posix::o_CLOEXEC : 0));
        case 1: // F_GETFD
        case 2: // F_SETFD: only the low FD_CLOEXEC bit has meaning.
            return result_value(kernel.posix().fcntl(fd, command, argument));
        case 3: // F_GETFL does not expose create/truncate/exclusive/CLOEXEC bits.
            return Flags::status_flags(*description);
        case 4: { // F_SETFL: access and creation flags are immutable/ignored.
            if (argument & (Flags::direct | Flags::noatime | Flags::asynchronous))
                return -22; // These selected mutable capabilities remain unsupported.
            const u32 flags = ((argument & Flags::append) ? posix::o_APPEND : 0) |
                              ((argument & Flags::nonblock) ? posix::o_NONBLOCK : 0);
            return result_value(kernel.posix().fcntl(fd, posix::f_SETFL, flags));
        }
        case Memfd::f_add_seals: {
            auto seals = Memfd::decode_seals(argument);
            return seals ? result_value(kernel.posix().fcntl(fd, posix::f_ADD_SEALS, seals.value()))
                         : -syscall::ErrnoMapper::errno_for(seals.status());
        }
        case Memfd::f_get_seals:
            return result_value(kernel.posix().fcntl(fd, posix::f_GET_SEALS, 0));
        default:
            return -22; // No pointer/locking command is interpreted as a scalar.
        }
    }
    if (is(Operation::getcwd))
    {
        auto current = kernel.posix().current_directory();
        if (!current)
            return -syscall::ErrnoMapper::errno_for(current.status());
        const auto path = current.value();
        const usize bytes = path.size() + 1;
        if (a[1] < bytes)
            return -34; // Linux ERANGE, including a zero syscall buffer size.
        // Validate/copy only the returned NUL-terminated path, not unused tail
        // capacity. Linux's raw syscall returns its size including the NUL.
        if (!hw::copy_to_user(space, static_cast<uptr>(a[0]), path.data(), bytes))
            return -14;
        return static_cast<i64>(bytes);
    }
    if (is(Operation::chdir)) return shared(syscall::Number::chdir);
    if (is(Operation::fchdir)) return shared(syscall::Number::fchdir);
    if (is(Operation::unlink)) return shared(syscall::Number::unlink);
    if (is(Operation::rmdir)) return shared(syscall::Number::rmdir);
    if (is(Operation::unlinkat)) return shared(syscall::Number::unlinkat);
    if (is(Operation::rename)) return shared(syscall::Number::rename);
    if (is(Operation::renameat)) return shared(syscall::Number::renameat);
    if (is(Operation::renameat2))
    {
        // The fifth Linux argument is an unsigned int, including the O32
        // stack word. Only zero has the ordinary renameat semantics below.
        const u32 flags = static_cast<u32>(a[4]);
        constexpr u32 no_replace = 1, exchange = 2, whiteout = 4;
        if (flags & ~(no_replace | exchange | whiteout)) return -22;
        if ((flags & exchange) && (flags & (no_replace | whiteout))) return -22;
        if (flags) return -95;
        hw::Syscall call{static_cast<uptr>(syscall::Number::renameat), {}};
        for (usize i = 0; i < 4; ++i) call.arguments[i] = static_cast<uptr>(a[i]);
        return native(call, complete);
    }
    if (is(Operation::readlink) || is(Operation::readlinkat))
    {
        const bool relative = is(Operation::readlinkat);
        const usize length_slot = relative ? 3 : 2;
        // Linux's syscall parameter is int even where musl accepts size_t.
        const i32 capacity = static_cast<i32>(a[length_slot]);
        if (capacity <= 0)
            return -22;
        hw::Syscall call{static_cast<uptr>(relative ? syscall::Number::readlinkat : syscall::Number::readlink), {}};
        for (usize i = 0; i < call.arguments.size(); ++i)
            call.arguments[i] = static_cast<uptr>(a[i]);
        call.arguments[length_slot] = static_cast<u32>(capacity);
        return native(call, complete);
    }
    if (is(Operation::getpid)) return shared(syscall::Number::getpid);
    if (is(Operation::getppid)) return shared(syscall::Number::getppid);
    std::optional<syscall::Number> session_operation;
    if (is(Operation::setpgid)) session_operation = syscall::Number::setpgid;
    else if (is(Operation::getpgid)) session_operation = syscall::Number::getpgid;
    else if (is(Operation::getpgrp)) session_operation = syscall::Number::getpgrp;
    else if (is(Operation::setsid)) session_operation = syscall::Number::setsid;
    else if (is(Operation::getsid)) session_operation = syscall::Number::getsid;
    if (session_operation)
    {
        hw::Syscall call{static_cast<uptr>(*session_operation), {}};
        // Linux pid_t is signed 32-bit even for LP64 syscalls. Ignore upper
        // register bits, then extend the selected value to the native word.
        // Unused arguments stay zero rather than inheriting register garbage.
        for (usize index = 0; index < count; ++index)
            call.arguments[index] = static_cast<uptr>(static_cast<i64>(static_cast<i32>(static_cast<u32>(a[index]))));
        return native(call, complete);
    }
    // Leaders and peers use the scheduler's common non-reused task-ID domain.
    if (is(Operation::gettid)) return shared(syscall::Number::gettid);
    auto identity = [&](Operation old_call, Operation wide_call, syscall::Number native_call) -> i64 {
        // The shared native boundary verifies the current caller before
        // reading canonical PCB identity. Legacy getters use Linux high2low.
        const auto value = shared(native_call);
        if (value < 0) return value;
        if (is(old_call) && Codec::syscall_number(wide_call))
            return value > 65535 ? 65534 : value;
        return value;
    };
    if (is(Operation::getuid) || is(Operation::getuid32))
        return identity(Operation::getuid, Operation::getuid32, syscall::Number::getuid);
    if (is(Operation::geteuid) || is(Operation::geteuid32))
        return identity(Operation::geteuid, Operation::geteuid32, syscall::Number::geteuid);
    if (is(Operation::getgid) || is(Operation::getgid32))
        return identity(Operation::getgid, Operation::getgid32, syscall::Number::getgid);
    if (is(Operation::getegid) || is(Operation::getegid32))
        return identity(Operation::getegid, Operation::getegid32, syscall::Number::getegid);
    if (is(Operation::fork)) return shared(syscall::Number::fork);
    if (is(Operation::clone))
    {
        // Native O32 entry rejects a stack whose 32-bit SP+32 is negative.
        // Its individual stack-word faults become zero, unlike generic copies.
        if constexpr (Profile == lu::Profile::mips)
            if ((static_cast<u32>(frame.sp) + u32{32}) & 0x80000000U) return -14;
        constexpr u64 child_exit_signal = Profile == lu::Profile::mips || Profile == lu::Profile::mips64 ? 18 : 17;
        const u32 flags = static_cast<u32>(a[0]);
        if (((flags & clone_flags::thread) && !(flags & clone_flags::sighand)) ||
            ((flags & clone_flags::sighand) && !(flags & clone_flags::vm))) return -22;
        if (flags == child_exit_signal)
        {
            if (a[1] != 0) return -22;
            return shared(syscall::Number::fork);
        }
        if ((flags & clone_flags::shared_group) != clone_flags::shared_group || (flags & ~clone_flags::supported))
            return -38;
        auto arguments = [&]() -> Result<lu::Call> {
            if constexpr (Profile != lu::Profile::mips) return Bridge::decode_call(frame, 5);
            else
            {
                auto result = Bridge::decode_call(frame, 4);
                if (!result) return result;
                std::array<u8, 4> word{};
                const u32 source = static_cast<u32>(frame.sp) + u32{16};
                if (hw::copy_from_user(space, word.data(), source, word.size()))
                    for (usize i = 0; i < 4; ++i)
                        result.value().arguments[4] |= static_cast<u64>(word[i]) << (8 * (Codec::profile.big_endian ? 3 - i : i));
                result.value().argument_count = 5;
                return result;
            }
        }();
        if (!arguments) return -14;
        return clone_thread<Profile>(kernel, pid, frame, arguments.value(), std::forward<PrepareThread>(prepare));
    }
    if (is(Operation::execve)) return shared(syscall::Number::execve);
    if (is(Operation::wait4))
    {
        // Linux pid_t/options remain 32-bit scalars on LP64.
        if ((static_cast<u32>(a[2]) & ~1U) || a[3]) return -22;
        const native::ChildWaitRequest request{static_cast<i32>(a[0]), static_cast<uptr>(a[1]),
            (static_cast<u32>(a[2]) & 1U) != 0, false, Codec::profile.big_endian};
        return native::begin_child_wait(kernel, pid, request, complete);
    }
    if (is(Operation::waitid))
    {
        // RV32 musl implements waitpid through waitid. This remains the
        // WEXITED/P_ALL/P_PID subset; no groups, rusage, stops or WNOWAIT.
        if constexpr (Profile != lu::Profile::rv32) return -38;
        else
        {
            const u32 kind = static_cast<u32>(a[0]), id = static_cast<u32>(a[1]);
            const u32 options = static_cast<u32>(a[3]);
            if (kind > 1 || (kind == 1 && (id == 0 || id > 0x7fffffffU)) ||
                (options & ~5U) || !(options & 4U) || a[4]) return -22;
            const native::ChildWaitRequest request{kind == 0 ? -1 : static_cast<i32>(id),
                static_cast<uptr>(a[2]), (options & 1U) != 0, true, Codec::profile.big_endian};
            return native::begin_child_wait(kernel, pid, request, complete);
        }
    }
    if (is(Operation::openat) || is(Operation::open) || is(Operation::creat))
    {
        using Open = lu::OpenCodec<Profile>;
        const bool relative = is(Operation::openat), create = is(Operation::creat);
        // Linux declares flags/dirfd as int and mode as umode_t on every ABI.
        const u32 flags = create ? Open::create | Open::truncate | 1U : static_cast<u32>(a[relative ? 2 : 1]);
        const u16 mode = static_cast<u16>(a[relative ? 3 : create ? 1 : 2]);
        const i32 dirfd = relative ? static_cast<i32>(static_cast<u32>(a[0])) : posix::at_FDCWD;
        auto request = Open::decode(flags);
        if (!request)
            return -22;
        FixedString<256> path;
        if (auto status = copy_pathname(space, static_cast<uptr>(a[relative ? 1 : 0]), path); !status.ok())
            return -syscall::ErrnoMapper::errno_for(status);
        // No controlling-terminal acquisition exists in the common descriptor
        // service, so O_NOCTTY imposes no additional action here.
        auto opened = kernel.posix().openat_limited(dirfd, path.view(), request.value().native_flags,
                                                    mode & 07777u, Open::max_regular_size(request.value()));
        return opened ? static_cast<i64>(opened.value()) : -syscall::ErrnoMapper::errno_for(opened.status());
    }
    if (is(Operation::read) || is(Operation::write) || is(Operation::readv) || is(Operation::writev) ||
        is(Operation::close))
    {
        const auto *slot = process->fd_table().entry(static_cast<i32>(a[0]));
        const auto *description = slot && slot->used ? kernel.scheduler().open_description(slot->description) : nullptr;
        if (!description || ((is(Operation::read) || is(Operation::readv)) && !description->readable) ||
            ((is(Operation::write) || is(Operation::writev)) && !description->writable))
            return -9;
    }
    if (is(Operation::read))
        return shared(syscall::Number::read);
    if (is(Operation::write))
        return shared(syscall::Number::write);
    if (is(Operation::readv))
        return shared(syscall::Number::readv);
    if (is(Operation::writev))
        return shared(syscall::Number::writev);
    if (is(Operation::getrandom))
        return shared(syscall::Number::getrandom);
    if (is(Operation::close))
        return shared(syscall::Number::close);
    if (is(Operation::fsync))
        return shared(syscall::Number::fsync);
    if (is(Operation::fdatasync))
        return shared(syscall::Number::fdatasync);
    if (is(Operation::exit))
        return shared(syscall::Number::exit);
    if (is(Operation::exit_group))
        return shared(syscall::Number::exit_group);
    if (is(Operation::mprotect))
        return shared(syscall::Number::mprotect);
    if (is(Operation::munmap))
        return shared(syscall::Number::munmap);
    if (is(Operation::brk))
    {
        // Linux's raw syscall returns the unchanged break on failure. Libc
        // translates a failed request; errno encoding must not touch this value.
        const auto result = shared(syscall::Number::brk);
        return result < 0 ? static_cast<i64>(process->resources().program_break) : result;
    }
    if (is(Operation::mmap) || is(Operation::mmap2))
    {
        constexpr u64 anonymous = Profile == lu::Profile::mips || Profile == lu::Profile::mips64 ? 0x800 : 0x20;
        constexpr u64 fixed = 0x10;
        const bool anonymous_mapping = (a[3] & anonymous) != 0;
        const u64 mapping_type = a[3] & 3;
        if ((mapping_type != 1 && mapping_type != 2) ||
            (a[3] & ~fixed) != (mapping_type | (anonymous_mapping ? anonymous : 0)) ||
            (anonymous_mapping && a[5] != 0))
            return -22;
        u64 byte_offset = a[5];
        if (is(Operation::mmap2))
        {
            if (byte_offset > (~uptr{0} >> 12)) return -22;
            byte_offset *= 4096;
        }
        if (byte_offset > ~uptr{0}) return -22;
        hw::Syscall call{static_cast<uptr>(syscall::Number::mmap), {}};
        for (usize i = 0; i < call.arguments.size(); ++i)
            call.arguments[i] = static_cast<uptr>(a[i]);
        call.arguments[3] = (mapping_type == 1 ? OK_MAP_SHARED : OK_MAP_PRIVATE) |
                            (anonymous_mapping ? OK_MAP_ANONYMOUS : 0) |
                            ((a[3] & fixed) ? OK_MAP_FIXED : 0);
        call.arguments[5] = static_cast<uptr>(byte_offset);
        return native(call, complete);
    }
    if (is(Operation::clock_settime) || is(Operation::clock_settime32) || is(Operation::clock_settime64))
    {
        if (static_cast<u32>(a[0]) != 0)
            return -22;
        const auto format = is(Operation::clock_settime64)
                                ? lu::TimeFormat::kernel_timespec : lu::TimeFormat::legacy_timespec;
        std::array<u8, 16> record{};
        const usize size = Codec::timespec_size(format);
        if (!hw::copy_from_user(space, record.data(), static_cast<uptr>(a[1]), size))
            return -14;
        auto value = Codec::decode_timespec({record.data(), size}, format);
        if (!value)
            return -22;
        const auto status = user::set_realtime_checked(
            {value.value().seconds, value.value().nanoseconds}, process->credentials(),
            [] { return arch::monotonic_sample(); },
            [](time::EpochTime epoch, time::MonotonicPoint point) { return arch::set_realtime(epoch, point); });
        return status ? 0 : -syscall::ErrnoMapper::errno_for(status);
    }
    if (is(Operation::clock_gettime) || is(Operation::clock_gettime32) || is(Operation::clock_gettime64) ||
        (is(Operation::clock_getres) || is(Operation::clock_getres_time32)) || is(Operation::clock_getres_time64))
    {
        const auto identity = static_cast<u32>(a[0]);
        if (identity > 1)
            return -22;
        const bool resolution = (is(Operation::clock_getres) || is(Operation::clock_getres_time32)) || is(Operation::clock_getres_time64);
        if (resolution && a[1] == 0) return 0;
        // NULL getres validates identity only; output requires a live provider.
        auto now = time::public_clock(identity, resolution);
        if (!now) return -5;
        const auto format = is(Operation::clock_gettime64) || is(Operation::clock_getres_time64)
            ? lu::TimeFormat::kernel_timespec : lu::TimeFormat::legacy_timespec;
        std::array<u8, 16> record{};
        const usize size = Codec::timespec_size(format);
        auto status = Codec::encode_timespec(
            {record.data(), size}, format,
            {now.value().seconds, now.value().nanoseconds});
        if (!status) return -75;
        return hw::copy_to_user(space, static_cast<uptr>(a[1]), record.data(), size) ? 0 : -14;
    }

    if (is(Operation::set_tid_address))
    {
        // Linux records the caller's pointer without accessing its mapping.
        auto *thread = kernel.scheduler().current_thread();
        if (!thread || thread->owner != pid || thread->state != sched::ProcessState::running)
            return -22;
        if (thread->tid > 0x7fffffffU) return -75;
        thread->tls.clear_tid_address = static_cast<uptr>(a[0]);
        return static_cast<i64>(thread->tid);
    }
    uptr pointer = static_cast<uptr>(a[0]);
    if constexpr (Profile == lu::Profile::x86_64)
        if (is(Operation::set_thread_area))
            return -38;
    if (is(Operation::arch_prctl))
    {
        if (a[0] != 0x1002)
            return -22;
        pointer = static_cast<uptr>(a[1]);
    }
    if constexpr (Profile == lu::Profile::i386)
    {
        if (!is(Operation::set_thread_area))
            return -38;
        std::array<u32, 4> descriptor{};
        if (!hw::copy_from_user(space, descriptor.data(), pointer, sizeof(descriptor)) ||
            !hw::user_range(space, pointer, sizeof(descriptor), true))
            return -14;
        if ((descriptor[0] != ~u32{0} && descriptor[0] != Bridge::i386_tls_entry) || descriptor[2] != 0xfffff ||
            descriptor[3] != 0x51)
            return -22;
        const auto base = descriptor[1];
        if (base != 0 && (base < hw::user_begin || base >= hw::user_end))
            return -22;
        descriptor[0] = Bridge::i386_tls_entry;
        if (!hw::copy_to_user(space, pointer, descriptor.data(), sizeof(descriptor)))
            return -14;
        pointer = base;
    }
    // Native n64 transports the entire Linux unsigned-long TLS value. The
    // emulated RDHWR returns this word; neither this call nor entry dereferences it.
    if constexpr (Profile != lu::Profile::mips64)
        if (pointer != 0 && (pointer < hw::user_begin || pointer >= hw::user_end))
            return -22;
    return Bridge::set_thread_pointer(frame, {pointer}, Bridge::thread_pointer_route) ? 0 : -22;
}

// Only errors produced by this bounded adapter/shared open/read-write/anonymous-VM
// path are covered. Other Linux operations require their own complete errno map.
template <lu::Profile Profile> Status complete(hw::UserFrame &frame, i64 result)
{
    using Codec = lu::Codec<Profile>;
    using Bridge = lu::NativeBridge<Profile>;
    using Operation = lu::Operation;
    if constexpr (Codec::profile.word_bytes == 4 &&
                  Codec::profile.error_convention == lu::ErrorConvention::negative)
    {
        // Scalar IDs are unsigned values with no Linux errno interpretation.
        // Preserve valid high uid32/gid32 values without weakening the generic
        // syscall return codec's reserved-negative-error-range contract.
        const auto number = Bridge::snapshot(frame).integer[Codec::profile.number_register];
        auto identity = [&](Operation legacy, Operation wide) {
            const auto selected = Codec::syscall_number(wide).has_value() ?
                Codec::syscall_number(wide) : Codec::syscall_number(legacy);
            return selected && number == *selected;
        };
        if (result >= 0 && static_cast<u64>(result) <= 0xffffffffULL &&
            (identity(Operation::getuid, Operation::getuid32) ||
             identity(Operation::geteuid, Operation::geteuid32) ||
             identity(Operation::getgid, Operation::getgid32) ||
             identity(Operation::getegid, Operation::getegid32)))
        {
            constexpr usize slot = Profile == lu::Profile::i386 ? 7 : Codec::profile.return_register;
            frame.registers[slot] = static_cast<uptr>(result);
            return Status::success();
        }
    }
    if (result < -4095)
        return Status::invalid_argument("Linux bootstrap result is outside the errno range");
    u32 error = result < 0 ? static_cast<u32>(-result) : 0;
    if (error == 3)
        error = *lu::Codec<Profile>::errno_number(lu::Error::e_srch);
    else if (error == 38)
        error = *lu::Codec<Profile>::errno_number(lu::Error::e_nosys);
    else if (error == 75)
        error = *lu::Codec<Profile>::errno_number(lu::Error::e_overflow);
    else if (error == 30)
        error = *lu::Codec<Profile>::errno_number(lu::Error::e_rofs);
    else if (error == 110)
        error = *lu::Codec<Profile>::errno_number(lu::Error::e_timedout);
    else if (error == 95)
        error = *lu::Codec<Profile>::errno_number(lu::Error::e_opnotsupp);
    if (error == 111) error = lu::tcp_errno_econnrefused[static_cast<usize>(Profile)];
    else if (error == 106) error = lu::tcp_errno_eisconn[static_cast<usize>(Profile)];
    else if (error == 107) error = lu::tcp_errno_enotconn[static_cast<usize>(Profile)];
    else if (error == 88) error = *lu::UdpConstants<Profile>::errno_number(lu::UdpErrno::e_notsock);
    else if (error == 98) error = *lu::UdpConstants<Profile>::errno_number(lu::UdpErrno::e_addrinuse);
    else if (error == 97) error = *lu::UdpConstants<Profile>::errno_number(lu::UdpErrno::e_afnosupport);
    else if (error == 93) error = *lu::UdpConstants<Profile>::errno_number(lu::UdpErrno::e_protonosupport);
    error = lu::OpenCodec<Profile>::pathname_errno(error);
    return lu::NativeBridge<Profile>::encode_return(frame, {error ? 0 : static_cast<u64>(result), error});
}
} // namespace ok::user::linux_bootstrap
