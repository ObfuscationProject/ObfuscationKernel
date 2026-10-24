#pragma once

#include "ok/abi/linux_user.hpp"
#include "ok/core/fixed.hpp"
#include "ok/fs/mode_change.hpp"
#include "ok/syscall/linux.hpp"
#include <bit>
#include <concepts>

namespace ok::abi::linux_user
{

enum class ChmodCall : u8 { chmod, fchmod, fchmodat, fchmodat2 };

struct ChmodRequest
{
    i32 fd{-100};
    u64 path{};
    u32 permissions{};
    u32 flags{};
    bool direct_fd{};
};

// NativeBridge supplies the complete captured register frame. O32 uses at most
// four registers here; the old three-argument fchmodat has no flags argument.
template <Profile Target>
struct ChmodCallAdapter final
{
    static constexpr u32 nofollow = 0x100;
    static constexpr u32 empty_path = 0x1000;
    static constexpr bool has_chmod = Target != Profile::aarch64 && Target != Profile::rv32 &&
                                      Target != Profile::rv64 && Target != Profile::loongarch64;

    [[nodiscard]] static constexpr usize argument_count(ChmodCall call)
    {
        switch (call)
        {
        case ChmodCall::chmod: case ChmodCall::fchmod: return 2;
        case ChmodCall::fchmodat: return 3;
        case ChmodCall::fchmodat2: return 4;
        }
        return 0;
    }

    [[nodiscard]] static Result<ChmodRequest> decode(ChmodCall call, std::span<const u64> arguments)
    {
        const usize count = argument_count(call);
        if (!count || (call == ChmodCall::chmod && !has_chmod))
            return Status::unsupported("Linux chmod call absent from target profile");
        if (arguments.size() != count)
            return Status::fault("Linux chmod argument frame is incomplete");
        ChmodRequest request;
        if (call == ChmodCall::chmod)
        {
            request.path = arguments[0] & Codec<Target>::word_mask;
            request.permissions = static_cast<u32>(arguments[1]) & 07777u;
        }
        else
        {
            request.fd = std::bit_cast<i32>(static_cast<u32>(arguments[0]));
            request.direct_fd = call == ChmodCall::fchmod;
            request.permissions = static_cast<u32>(arguments[request.direct_fd ? 1 : 2]) & 07777u;
            if (!request.direct_fd)
                request.path = arguments[1] & Codec<Target>::word_mask;
            if (call == ChmodCall::fchmodat2)
            {
                request.flags = static_cast<u32>(arguments[3]);
                if (request.flags & ~(nofollow | empty_path))
                    return Status::invalid_argument("unknown Linux fchmodat2 flags");
            }
        }
        return request;
    }

    template <typename ReadPath, typename Change>
        requires requires(ReadPath read, Change change, u64 address,
                          FixedString<256> &path, ChmodRequest request) {
            { read(address, path) } -> std::same_as<Status>;
            { change(request, path.view()) } -> std::same_as<fs::ModeChangeResult>;
        }
    static i64 execute(ChmodCall call, std::span<const u64> arguments, ReadPath read, Change change)
    {
        auto decoded = decode(call, arguments);
        if (!decoded)
            return -syscall::ErrnoMapper::errno_for(decoded.status());
        const auto &request = decoded.value();
        FixedString<256> path;
        if (!request.direct_fd)
        {
            // Linux chmod does not have statx's NULL-as-empty shortcut.
            if (!request.path)
                return -14;
            if (auto status = read(request.path, path); !status)
                return -syscall::ErrnoMapper::errno_for(status);
            if (path.empty() && !(request.flags & empty_path))
                return -2;
        }
        const auto result = change(request, path.view());
        switch (result.kind())
        {
        case fs::ModeChangeResult::Kind::applied: return 0;
        case fs::ModeChangeResult::Kind::read_only: return -30;
        case fs::ModeChangeResult::Kind::unavailable: return -95;
        case fs::ModeChangeResult::Kind::failed:
            return -syscall::ErrnoMapper::errno_for(result.status());
        }
        return -5;
    }
};

} // namespace ok::abi::linux_user
