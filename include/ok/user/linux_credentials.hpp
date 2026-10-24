#pragma once

#include "ok/abi/linux_user.hpp"
#include "ok/sched/scheduler.hpp"
#include "ok/user/credential_change.hpp"

namespace ok::user::linux_credentials
{

struct Call
{
    CredentialChange change{};
    bool getter{};
    bool legacy16{};
    usize arguments{};
};

template <abi::linux_user::Profile Profile>
[[nodiscard]] std::optional<Call> select(u64 number)
{
    using C = abi::linux_user::Codec<Profile>;
    using O = abi::linux_user::Operation;
    auto pair = [&](O narrow, O wide) -> std::optional<bool> {
        const auto old = C::syscall_number(narrow), modern = C::syscall_number(wide);
        if (modern && number == *modern) return false;
        if (old && number == *old) return modern.has_value();
        return std::nullopt;
    };
    struct Form { O narrow, wide; IdDomain domain; IdChange operation; bool getter; usize count; };
    constexpr Form forms[]{
        {O::setuid, O::setuid32, IdDomain::user, IdChange::single, false, 1},
        {O::setgid, O::setgid32, IdDomain::group, IdChange::single, false, 1},
        {O::setreuid, O::setreuid32, IdDomain::user, IdChange::real_effective, false, 2},
        {O::setregid, O::setregid32, IdDomain::group, IdChange::real_effective, false, 2},
        {O::setresuid, O::setresuid32, IdDomain::user, IdChange::real_effective_saved, false, 3},
        {O::setresgid, O::setresgid32, IdDomain::group, IdChange::real_effective_saved, false, 3},
        {O::getresuid, O::getresuid32, IdDomain::user, IdChange::real_effective_saved, true, 3},
        {O::getresgid, O::getresgid32, IdDomain::group, IdChange::real_effective_saved, true, 3},
    };
    for (const auto &form : forms)
        if (auto legacy = pair(form.narrow, form.wide))
            return Call{{form.domain, form.operation}, form.getter, *legacy, form.count};
    return std::nullopt;
}

template <abi::linux_user::Profile Profile, typename CopyOut>
i64 dispatch(sched::ProcessControlBlock &process, Call call, const std::array<u64, 6> &arguments, CopyOut &&copy_out)
{
    using C = abi::linux_user::Codec<Profile>;
    const auto old = process.credentials();
    if (call.getter)
    {
        const bool group = call.change.domain == IdDomain::group;
        const u32 ids[]{group ? old.gid : old.uid, group ? old.egid : old.euid, group ? old.sgid : old.suid};
        const usize width = call.legacy16 ? 2 : 4;
        for (usize field = 0; field < 3; ++field)
        {
            const u32 id = call.legacy16 && ids[field] > 65535 ? 65534 : ids[field];
            std::array<u8, 4> bytes{};
            for (usize index = 0; index < width; ++index)
                bytes[index] = static_cast<u8>(id >> (8 * (C::profile.big_endian ? width-index-1 : index)));
            // Linux commits each put_user before attempting the next pointer.
            if (!copy_out(arguments[field], std::span<const u8>{bytes.data(), width})) return -14;
        }
        return 0;
    }
    usize live = 0;
    for (const auto &thread : process.threads())
        if (thread.state != sched::ProcessState::exited) ++live;
    // PCB credentials are shared. Per-thread Linux changes and libc setxid
    // synchronization need a separate mechanism before admitting this case.
    if (live != 1) return -95;
    auto decode = [&](u64 argument) {
        if (!call.legacy16) return static_cast<u32>(argument);
        const auto narrow = static_cast<u16>(argument);
        return narrow == 65535 ? unchanged_id : static_cast<u32>(narrow);
    };
    call.change.real = decode(arguments[0]);
    if (call.arguments > 1) call.change.effective = decode(arguments[1]);
    if (call.arguments > 2) call.change.saved = decode(arguments[2]);
    auto result = change_credentials(old, call.change);
    if (!result) return result.status().code() == StatusCode::permission ? -1 : -22;
    process.set_credentials(result.value());
    return 0;
}

} // namespace ok::user::linux_credentials
