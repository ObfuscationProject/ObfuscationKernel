#pragma once

#include "ok/abi/linux_user.hpp"
#include "ok/posix/posix.hpp"

namespace ok::abi::linux_user
{
struct OpenRequest
{
    u32 native_flags{};
    bool large_file{};
    bool no_controlling_terminal{};
};

// Explicit Linux-to-native flag conversion. Never forward Linux flags as native
// bits: even O_APPEND/O_DIRECTORY/O_NOFOLLOW differ between these contracts.
template <Profile Selected>
    requires SupportedProfile<Selected>
class OpenCodec final
{
    static constexpr bool mips = Selected == Profile::mips || Selected == Profile::mips64;
    static constexpr bool arm = Selected == Profile::arm32 || Selected == Profile::aarch64;
    static constexpr bool ppc = Selected == Profile::ppc;

  public:
    static constexpr u32 create = mips ? 0400 : 0100;
    static constexpr u32 exclusive = mips ? 02000 : 0200;
    static constexpr u32 no_ctty = mips ? 04000 : 0400;
    static constexpr u32 truncate = 01000;
    static constexpr u32 append = mips ? 0010 : 02000;
    static constexpr u32 nonblock = mips ? 0200 : 04000;
    static constexpr u32 directory = arm || ppc ? 040000 : 0200000;
    static constexpr u32 nofollow = arm || ppc ? 0100000 : 0400000;
    static constexpr u32 cloexec = 02000000;
    static constexpr u32 largefile = mips ? 020000 : arm ? 0400000 : ppc ? 0200000 : 0100000;
    static constexpr u32 asynchronous = mips ? 010000 : 020000;
    static constexpr u32 direct = mips ? 0100000 : arm ? 0200000 : ppc ? 0400000 : 040000;
    static constexpr u32 noatime = 01000000;
    static constexpr u32 admitted =
        3 | create | exclusive | no_ctty | truncate | append | nonblock | directory | nofollow | cloexec | largefile;

    // Linux fs/open.c forces O_LARGEFILE for native 64-bit openat. The
    // 32-bit profiles require the caller's bit before opening a large file.
    static constexpr u64 max_regular_size(const OpenRequest &request)
    {
        return Codec<Selected>::profile.word_bytes == 8 || request.large_file ? ~u64{0} : 0x7fff'ffffu;
    }

    static constexpr u32 status_flags(const sched::OpenFileDescription &description)
    {
        u32 flags = description.flags & 3;
        if (description.flags & posix::o_APPEND) flags |= append;
        if (description.flags & posix::o_NONBLOCK) flags |= nonblock;
        if (description.flags & posix::o_DIRECTORY) flags |= directory;
        if (description.flags & posix::o_NOFOLLOW) flags |= nofollow;
        if (description.large_file) flags |= largefile;
        return flags;
    }

    // Canonical common-service errno -> target pathname errno. Values match
    // the pinned musl errno headers and Linux 6.13.4 UAPI (MIPS is distinct).
    static constexpr u32 pathname_errno(u32 value)
    {
        if constexpr (mips)
        {
            if (value == 36)
                return 78; // ENAMETOOLONG
            if (value == 39)
                return 93; // ENOTEMPTY
            if (value == 40)
                return 90; // ELOOP
        }
        return value;
    }

    static Result<OpenRequest> decode(u64 flags)
    {
        if ((flags & ~u64{admitted}) != 0 || (flags & 3) == 3)
            return Status::invalid_argument("Linux open flags are unsupported or access mode is invalid");
        u32 native = static_cast<u32>(flags & 3);
        const struct
        {
            u32 linux_flag;
            u32 native_flag;
        } pairs[] = {{create, posix::o_CREAT},      {exclusive, posix::o_EXCL},    {truncate, posix::o_TRUNC},
                     {append, posix::o_APPEND},     {nonblock, posix::o_NONBLOCK}, {directory, posix::o_DIRECTORY},
                     {nofollow, posix::o_NOFOLLOW}, {cloexec, posix::o_CLOEXEC}};
        for (const auto pair : pairs)
            if (flags & pair.linux_flag)
                native |= pair.native_flag;
        return OpenRequest{native, (flags & largefile) != 0, (flags & no_ctty) != 0};
    }
};
} // namespace ok::abi::linux_user
