#pragma once

#include "ok/core/fixed.hpp"
#include "ok/fs/mode_change.hpp"
#include "ok/syscall/linux.hpp"
#include "ok/uapi/syscall.h"
#include <array>
#include <bit>
#include <concepts>
#include <optional>
#include <type_traits>

namespace ok::user::native_metadata
{
enum class Operation : u8 { chmod, fchmod, fchmodat, chown, fchown, fchownat };

[[nodiscard]] constexpr std::optional<Operation> select(syscall::Number number)
{
    using N = syscall::Number;
    switch (number)
    {
    case N::chmod: return Operation::chmod;
    case N::fchmod: return Operation::fchmod;
    case N::fchmodat: return Operation::fchmodat;
    case N::chown: return Operation::chown;
    case N::fchown: return Operation::fchown;
    case N::fchownat: return Operation::fchownat;
    default: return {};
    }
}

struct Request
{
    i32 fd{-100};
    u32 mode{};
    u32 uid{0xffffffffU};
    u32 gid{0xffffffffU};
    u32 flags{};
    bool ownership{};
    bool direct_fd{};
};

// Both zero-extended i32 bits and native sign extension are canonical. Other
// upper words must never silently select a different descriptor or directory.
template <typename Word>
[[nodiscard]] constexpr bool descriptor(Word word, i32 &output)
{
    static_assert(std::is_unsigned_v<Word> && sizeof(Word) >= sizeof(u32));
    const auto low = static_cast<u32>(word);
    output = std::bit_cast<i32>(low);
    return word == static_cast<Word>(low) || word == static_cast<Word>(output);
}

[[nodiscard]] inline i64 response(fs::ModeChangeResult result)
{
    switch (result.kind())
    {
    case fs::ModeChangeResult::Kind::applied: return 0;
    case fs::ModeChangeResult::Kind::read_only: return -30;
    case fs::ModeChangeResult::Kind::unavailable: return -95;
    case fs::ModeChangeResult::Kind::failed: return -syscall::ErrnoMapper::errno_for(result.status());
    }
    return -5;
}

// Copy exactly the bounded pathname at this boundary so overlong native paths
// use ENAMETOOLONG without changing the generic user-string copier contract.
template <typename Word, typename CopyByte>
Status copy_path(Word address, FixedString<256> &path, CopyByte copy)
{
    path.clear();
    for (usize index = 0; index < 256; ++index)
    {
        char value{};
        const auto offset = static_cast<Word>(index);
        if (address + offset < address || !copy(address + offset, value))
            return Status::fault("invalid native metadata path");
        if (!value) return Status::success();
        if (!path.append(value)) return Status::name_too_long("native metadata path exceeds limit");
    }
    return Status::name_too_long("native metadata path exceeds limit");
}

template <typename Word, typename ReadPath, typename Change>
    requires requires(ReadPath read, Change change, Word address, FixedString<256> &path, Request request) {
        { read(address, path) } -> std::same_as<Status>;
        { change(request, path.view()) } -> std::same_as<fs::ModeChangeResult>;
    }
i64 dispatch(bool native_personality, Operation operation, const std::array<Word, 6> &arguments,
             ReadPath read, Change change)
{
    static_assert(std::is_unsigned_v<Word> && sizeof(Word) >= sizeof(u32));
    if (!native_personality) return -38;
    Request request;
    request.ownership = operation == Operation::chown || operation == Operation::fchown || operation == Operation::fchownat;
    request.direct_fd = operation == Operation::fchmod || operation == Operation::fchown;
    const bool at = operation == Operation::fchmodat || operation == Operation::fchownat;
    const usize scalar = at ? 2 : 1;
    const usize count = at ? (request.ownership ? 5 : 4) : (request.ownership ? 3 : 2);
    for (usize index = count; index < arguments.size(); ++index)
        if (arguments[index]) return -22;
    if (arguments[scalar] > 0xffffffffU || (request.ownership && arguments[scalar + 1] > 0xffffffffU)) return -22;
    if ((at || request.direct_fd) && !descriptor(arguments[0], request.fd)) return -9;
    if (request.ownership)
    {
        request.uid = static_cast<u32>(arguments[scalar]);
        request.gid = static_cast<u32>(arguments[scalar + 1]);
    }
    else request.mode = static_cast<u32>(arguments[scalar]);
    if (at)
    {
        const Word flags = arguments[request.ownership ? 4 : 3];
        if (flags > 0xffffffffU || (flags & ~(Word{OK_AT_EMPTY_PATH} | Word{OK_AT_SYMLINK_NOFOLLOW}))) return -22;
        request.flags = static_cast<u32>(flags);
    }
    FixedString<256> path;
    if (!request.direct_fd)
    {
        const Word address = arguments[at ? 1 : 0];
        if (!address) return -14;
        if (auto status = read(address, path); !status) return -syscall::ErrnoMapper::errno_for(status);
        if (path.empty() && !(request.flags & OK_AT_EMPTY_PATH)) return -2;
    }
    return response(change(request, path.view()));
}
} // namespace ok::user::native_metadata
