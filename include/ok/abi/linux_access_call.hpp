#pragma once

#include "ok/abi/linux_user.hpp"
#include "ok/core/fixed.hpp"
#include "ok/syscall/linux.hpp"
#include <bit>
#include <concepts>

namespace ok::abi::linux_user
{

enum class AccessCall : u8 { access, faccessat, faccessat2 };

struct AccessRequest
{
    i32 fd{-100};
    u64 path{};
    u32 mode{};
    u32 flags{};
};

template <Profile Target> struct AccessCallAdapter final
{
    static constexpr u32 nofollow = 0x100;
    static constexpr u32 effective_ids = 0x200;
    static constexpr u32 empty_path = 0x1000;

    [[nodiscard]] static constexpr usize argument_count(AccessCall call)
    {
        switch (call)
        {
        case AccessCall::access: return 2;
        case AccessCall::faccessat: return 3;
        case AccessCall::faccessat2: return 4;
        }
        return 0;
    }

    [[nodiscard]] static Result<AccessRequest> decode(AccessCall call, std::span<const u64> arguments)
    {
        const usize count = argument_count(call);
        if (!count || (call == AccessCall::access && !Codec<Target>::syscall_number(Operation::access)))
            return Status::unsupported("Linux access call absent from target profile");
        if (arguments.size() != count)
            return Status::fault("Linux access argument frame is incomplete");
        AccessRequest request;
        const bool relative = call != AccessCall::access;
        if (relative)
            request.fd = std::bit_cast<i32>(static_cast<u32>(arguments[0]));
        request.path = arguments[relative ? 1 : 0] & Codec<Target>::word_mask;
        request.mode = static_cast<u32>(arguments[relative ? 2 : 1]);
        if (call == AccessCall::faccessat2)
            request.flags = static_cast<u32>(arguments[3]);
        if (request.mode & ~7u)
            return Status::invalid_argument("invalid Linux access mode");
        if (request.flags & ~(nofollow | effective_ids | empty_path))
            return Status::invalid_argument("unknown Linux faccessat2 flags");
        return request;
    }

    template <typename ReadPath, typename Check>
        requires requires(ReadPath read, Check check, u64 address,
                          FixedString<256> &path, AccessRequest request) {
            { read(address, path) } -> std::same_as<Status>;
            { check(request, path.view()) } -> std::same_as<Status>;
        }
    static i64 execute(AccessCall call, std::span<const u64> arguments, ReadPath read, Check check)
    {
        auto decoded = decode(call, arguments);
        if (!decoded)
            return -syscall::ErrnoMapper::errno_for(decoded.status());
        const auto request = decoded.value();
        // AT_EMPTY_PATH permits an empty string, not a NULL pathname pointer.
        if (!request.path)
            return -14;
        FixedString<256> path;
        if (auto status = read(request.path, path); !status)
            return -syscall::ErrnoMapper::errno_for(status);
        if (path.empty() && !(request.flags & empty_path))
            return -2;
        const auto status = check(request, path.view());
        return status ? 0 : -syscall::ErrnoMapper::errno_for(status);
    }
};

} // namespace ok::abi::linux_user
