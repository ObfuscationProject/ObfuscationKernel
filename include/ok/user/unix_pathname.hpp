#pragma once
#include "ok/abi/linux_unix_pathname.hpp"
#include "ok/user/unix_original_ofd.hpp"
#include "ok/posix/posix.hpp"
#include "ok/syscall/linux.hpp"

namespace ok::user::native
{
// Immediate boot-CPU operation. Pin the original descriptor before copying
// address/capacity/stack arguments. Only accept needs a prepared publication.
template <abi::linux_user::Profile Selected, bool Native = false,
          typename FillArguments, typename CopyIn, typename CopyOut, typename Writable>
i64 unix_pathname(posix::PosixService &posix, sched::SchedulerState &scheduler,
    const sched::ProcessControlBlock &process, abi::linux_user::UnixPathCall operation,
    std::array<u64, 6> arguments, FillArguments &&fill_arguments,
    CopyIn &&copy_in, CopyOut &&copy_out, Writable &&writable)
{
    using Call = abi::linux_user::UnixPathCall;
    using C = abi::linux_user::UnixPathCodec<Selected, Native>;
    auto failure = [&](Status status) -> i64 {
        if (status.code() == StatusCode::unsupported) return -95;
        if (operation == Call::bind && status.code() == StatusCode::already_exists) return -98;
        if (operation == Call::connect && status.code() == StatusCode::busy) return -106;
        return -syscall::ErrnoMapper::errno_for(status);
    };
    if (operation == Call::socket)
    {
        if (arguments[0] != 1) return -97;
        if (arguments[2]) return -93;
        auto flags = C::decode_type(arguments[1]);
        if (!flags) return failure(flags.status());
        auto result = posix.unix_socket(flags.value());
        return result ? static_cast<i64>(result.value()) : failure(result.status());
    }
    UnixOriginalDescription original{scheduler};
    const auto selected = original.acquire(process, arguments[0]);
    if (!selected) return selected.code() == StatusCode::invalid_argument ? -88 : failure(selected);
    auto filled = fill_arguments(arguments);
    if (!filled) return failure(filled.status());
    arguments = filled.value();
    if (!original.live()) return -9;
    const auto id = original.description();
    if (operation == Call::bind || operation == Call::connect)
    {
        if (arguments[2] < 3 || arguments[2] > C::address_capacity) return -22;
        const auto size = static_cast<usize>(arguments[2]);
        if (!C::range(arguments[1], size)) return -14;
        std::array<u8, C::address_capacity> bytes{};
        if (auto copied = copy_in(arguments[1], std::span<u8>{bytes.data(), size}); !copied) return failure(copied);
        if (!original.live()) return -9;
        auto path = C::decode_address({bytes.data(), size});
        if (!path) return failure(path.status());
        if (operation == Call::bind)
        {
            const auto result = posix.bind_unix_description(id, path.value().view());
            return result ? 0 : failure(result);
        }
        const auto result = posix.connect_unix_description(id, path.value().view());
        return result ? (result.value() ? 0 : -111) : failure(result.status());
    }
    if (operation == Call::listen)
    {
        // Negative backlog is zero; the scheduler uses a bounded minimum one.
        const auto backlog = static_cast<i32>(arguments[1]);
        const auto result = posix.listen_unix_description(id, backlog > 0 ? static_cast<usize>(backlog) : 0);
        return result ? 0 : failure(result);
    }
    if (operation == Call::accept || operation == Call::accept4)
    {
        const u64 flags = operation == Call::accept4 ? arguments[3] : 0;
        if (flags & ~(u64{C::nonblock} | C::cloexec)) return -22;
        u32 capacity{};
        if (arguments[1])
        {
            if (!C::range(arguments[2], 4)) return -14;
            std::array<u8, 4> bytes{};
            if (auto copied = copy_in(arguments[2], std::span<u8>{bytes}); !copied) return failure(copied);
            if (!original.live()) return -9;
            capacity = static_cast<u32>(C::load(bytes));
            const usize copied = capacity < 2 ? capacity : 2;
            if (!writable(arguments[2], 4) || (copied && (!C::range(arguments[1], copied) || !writable(arguments[1], copied)))) return -14;
        }
        if (!original.live()) return -9;
        auto prepared = posix.prepare_unix_accept(id, ((flags & C::nonblock) ? posix::o_NONBLOCK : 0u) |
                                                     ((flags & C::cloexec) ? posix::o_CLOEXEC : 0u));
        if (!prepared) return failure(prepared.status());
        if (arguments[1])
        {
            // This bounded API reports an unnamed peer. SO_PEERCRED supplies
            // actual peer identity independently of pathname text.
            std::array<u8, 2> address{}; C::store(address, 1);
            const usize copied = capacity < address.size() ? capacity : address.size();
            if (copied)
                if (auto output = copy_out(arguments[1], std::span<const u8>{address.data(), copied}); !output) return failure(output);
            if (!original.live()) return -9;
            std::array<u8, 4> length{}; C::store(length, address.size());
            if (auto output = copy_out(arguments[2], std::span<const u8>{length}); !output) return failure(output);
            if (!original.live()) return -9;
        }
        const auto result = prepared.value().commit();
        return result ? static_cast<i64>(result.value()) : failure(result.status());
    }
    if (operation == Call::getsockopt)
    {
        if (arguments[1] != C::sol_socket) return -95;
        const bool credentials = arguments[2] == C::peercred;
        if (!credentials && arguments[2] != C::so_type && arguments[2] != C::so_error) return -95;
        if (!C::range(arguments[4], 4)) return -14;
        std::array<u8, 4> input{};
        if (auto copied = copy_in(arguments[4], std::span<u8>{input}); !copied) return failure(copied);
        if (!original.live()) return -9;
        const usize length = credentials ? 12 : 4;
        const u32 capacity = static_cast<u32>(C::load(input));
        const usize copied = capacity < length ? capacity : length;
        if (!writable(arguments[4], 4) || (copied && (!C::range(arguments[3], copied) || !writable(arguments[3], copied)))) return -14;
        if (!original.live()) return -9;
        std::array<u8, 12> output{};
        if (credentials)
        {
            auto peer = posix.unix_peer_credentials(id);
            if (!peer) return peer.status().code() == StatusCode::not_found ? -107 : failure(peer.status());
            if (peer.value().pid > static_cast<u64>(__INT32_MAX__)) return -75;
            C::store(std::span<u8>{output}.subspan(0, 4), peer.value().pid);
            C::store(std::span<u8>{output}.subspan(4, 4), peer.value().uid);
            C::store(std::span<u8>{output}.subspan(8, 4), peer.value().gid);
        }
        else C::store(std::span<u8>{output}.first(4), arguments[2] == C::so_type ? C::stream : 0);
        if (copied)
            if (auto published = copy_out(arguments[3], std::span<const u8>{output.data(), copied}); !published) return failure(published);
        if (!original.live()) return -9;
        std::array<u8, 4> size{}; C::store(size, copied);
        if (auto published = copy_out(arguments[4], std::span<const u8>{size}); !published) return failure(published);
        return original.live() ? 0 : -9;
    }
    return -95;
}
}
