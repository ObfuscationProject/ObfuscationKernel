#pragma once

#include "ok/sched/scheduler.hpp"
#include "ok/syscall/syscall.hpp"
#include "ok/user/credential_change.hpp"

namespace ok::user::native_credentials
{

struct Call
{
    CredentialChange change{};
    bool getter{};
    usize arguments{};
};

[[nodiscard]] inline std::optional<Call> select(syscall::Number number)
{
    using N = syscall::Number;
    switch (number)
    {
    case N::setuid: return Call{{IdDomain::user, IdChange::single}, false, 1};
    case N::setgid: return Call{{IdDomain::group, IdChange::single}, false, 1};
    case N::setreuid: return Call{{IdDomain::user, IdChange::real_effective}, false, 2};
    case N::setregid: return Call{{IdDomain::group, IdChange::real_effective}, false, 2};
    case N::setresuid: return Call{{IdDomain::user, IdChange::real_effective_saved}, false, 3};
    case N::setresgid: return Call{{IdDomain::group, IdChange::real_effective_saved}, false, 3};
    case N::getresuid: return Call{{IdDomain::user, IdChange::real_effective_saved}, true, 3};
    case N::getresgid: return Call{{IdDomain::group, IdChange::real_effective_saved}, true, 3};
    default: return std::nullopt;
    }
}

// The runtime authenticates the running caller before reaching this serialized
// PCB boundary. CopyOut owns checked user memory access; no pointer is retained.
template <typename Word, typename CopyOut>
i64 dispatch(sched::ProcessControlBlock &process, Call call, const std::array<Word, 6> &arguments,
             CopyOut &&copy_out)
{
    const auto old = process.credentials();
    if (call.getter)
    {
        const bool group = call.change.domain == IdDomain::group;
        const u32 ids[]{group ? old.gid : old.uid, group ? old.egid : old.euid, group ? old.sgid : old.suid};
        for (usize field = 0; field < 3; ++field)
            if (!copy_out(arguments[field],
                          std::span<const u8>{reinterpret_cast<const u8 *>(&ids[field]), sizeof(u32)}))
                return -14;
        return 0;
    }
    usize live = 0;
    for (const auto &thread : process.threads())
        if (thread.state != sched::ProcessState::exited) ++live;
    // PCB-wide credentials cannot yet synchronize identity changes across TCBs.
    if (live != 1) return -95;
    call.change.real = static_cast<u32>(arguments[0]);
    if (call.arguments > 1) call.change.effective = static_cast<u32>(arguments[1]);
    if (call.arguments > 2) call.change.saved = static_cast<u32>(arguments[2]);
    auto result = change_credentials(old, call.change);
    if (!result) return result.status().code() == StatusCode::permission ? -1 : -22;
    process.set_credentials(result.value());
    return 0;
}

} // namespace ok::user::native_credentials
