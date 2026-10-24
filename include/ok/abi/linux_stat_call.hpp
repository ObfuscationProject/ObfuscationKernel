#pragma once

#include "ok/abi/linux_stat.hpp"
#include "ok/syscall/linux.hpp"
#include <bit>

namespace ok::abi::linux_user
{
enum class StatCall : u8 { stat, lstat, fstat, newfstatat, statx };

struct StatRequest
{
    StatLayout layout{StatLayout::statx};
    i32 fd{posix::at_FDCWD};
    u64 path{}, output{};
    u32 flags{};
    bool direct_fd{};
};

// Arguments are already captured once by NativeBridge, including O32's fifth
// stack word. This layer returns generic errno; target completion maps it once.
template <Profile Target>
struct StatCallAdapter final
{
    static constexpr usize argument_count(StatCall call)
    {
        if (call == StatCall::statx) return 5;
        if (call == StatCall::newfstatat) return 4;
        return 2;
    }
    static Result<StatRequest> decode(StatCall call, std::span<const u64> arguments)
    {
        if (call != StatCall::stat && call != StatCall::lstat && call != StatCall::fstat &&
            call != StatCall::newfstatat && call != StatCall::statx)
            return Status::unsupported("unknown Linux stat call");
        if (call != StatCall::statx && !StatCodec<Target>::has_kernel_stat)
            return Status::unsupported("legacy Linux stat call unavailable");
        if ((call == StatCall::stat || call == StatCall::lstat) && Target != Profile::x86_64)
            return Status::unsupported("legacy Linux pathname stat unavailable");
        if (arguments.size() != argument_count(call))
            return Status::fault("Linux stat argument frame is incomplete");
        auto word = [&](usize at) { return arguments[at] & Codec<Target>::word_mask; };
        StatRequest request;
        request.layout = call == StatCall::statx ? StatLayout::statx : StatLayout::kernel_stat;
        if (call == StatCall::fstat)
        {
            request.fd = std::bit_cast<i32>(static_cast<u32>(arguments[0]));
            request.output = word(1); request.direct_fd = true;
            return request;
        }
        if (call == StatCall::stat || call == StatCall::lstat)
        {
            request.path = word(0); request.output = word(1);
            request.flags = call == StatCall::lstat ? posix::at_SYMLINK_NOFOLLOW : 0;
            return request;
        }
        request.fd = std::bit_cast<i32>(static_cast<u32>(arguments[0]));
        request.path = word(1);
        const u32 flags = static_cast<u32>(arguments[call == StatCall::statx ? 2 : 3]);
        constexpr u32 automount = 0x800, force = 0x2000, dont = 0x4000;
        const u32 allowed = posix::at_EMPTY_PATH | posix::at_SYMLINK_NOFOLLOW | automount |
            (call == StatCall::statx ? force | dont : 0);
        if ((flags & ~allowed) || ((flags & (force | dont)) == (force | dont)))
            return Status::invalid_argument("Linux stat flags unavailable");
        if (call == StatCall::statx && (static_cast<u32>(arguments[3]) & 0x80000000u))
            return Status::invalid_argument("Linux statx reserved mask bit");
        request.output = word(call == StatCall::statx ? 4 : 2);
        request.flags = flags & (posix::at_EMPTY_PATH | posix::at_SYMLINK_NOFOLLOW);
        return request;
    }

    // Callbacks are statically bound to the checked pathname copier, common
    // service query, whole-range admission and guarded user-copy boundary.
    // Query maps a bad live fd explicitly; provider EINVAL remains EINVAL.
    template <typename ReadPath, typename Query, typename Admit, typename Copy>
        requires requires(ReadPath read, Query query, Admit admit, Copy copy,
                          u64 address, FixedString<256> &path, StatRequest request,
                          std::span<const u8> bytes) {
            { read(address, path) } -> std::same_as<Status>;
            { query(request, path.view()) } -> std::same_as<Result<posix::FileStatus>>;
            { admit(address, bytes.size()) } -> std::same_as<bool>;
            { copy(address, bytes) } -> std::same_as<Status>;
        }
    static i64 execute(StatCall call, std::span<const u64> arguments,
                       ReadPath read, Query query, Admit admit, Copy copy)
    {
        auto decoded = decode(call, arguments);
        if (!decoded) return -syscall::ErrnoMapper::errno_for(decoded.status());
        const auto &request = decoded.value();
        FixedString<256> path;
        if (!request.direct_fd)
        {
            if (!request.path)
            {
                if (!(request.flags & posix::at_EMPTY_PATH)) return -14;
            }
            else if (auto status = read(request.path, path); !status)
                return -syscall::ErrnoMapper::errno_for(status);
            if (path.empty() && !(request.flags & posix::at_EMPTY_PATH)) return -2;
        }
        auto value = query(request, path.view());
        if (!value) return error(value.status());
        std::array<u8, 256> bytes{};
        const usize length = StatCodec<Target>::size(request.layout);
        auto encoded = StatCodec<Target>::encode(value.value(), request.layout, {bytes.data(), length});
        if (!encoded) return error(encoded.status());
        // Pointer arithmetic is bounded in the target word domain even in
        // hosted tests with a wider native pointer. No byte is copied early.
        if (!request.output || length - 1 > Codec<Target>::word_mask - request.output ||
            !admit(request.output, length)) return -14;
        const auto copied = copy(request.output, {bytes.data(), length});
        return copied ? 0 : -syscall::ErrnoMapper::errno_for(copied);
    }
  private:
    static i64 error(Status status)
    {
        return status.code() == StatusCode::unsupported ? -95 : -syscall::ErrnoMapper::errno_for(status);
    }
};
} // namespace ok::abi::linux_user
