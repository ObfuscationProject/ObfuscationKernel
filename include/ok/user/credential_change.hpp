#pragma once

#include "ok/user/user.hpp"

namespace ok::user
{

inline constexpr u32 unchanged_id = 0xffffffffU;
enum class IdDomain : u8 { user, group };
enum class IdChange : u8 { single, real_effective, real_effective_saved };

struct CredentialChange
{
    IdDomain domain{IdDomain::user};
    IdChange operation{IdChange::single};
    u32 real{unchanged_id};
    u32 effective{unchanged_id};
    u32 saved{unchanged_id};
};

// Authorization always uses the immutable old tuple. No caller-owned candidate
// is published on failure, and no userspace request contains kernel_space.
[[nodiscard]] inline Result<Credentials> change_credentials(Credentials old, CredentialChange request)
{
    if (old.kernel_space)
        return Status::permission("user identity mutation cannot alter kernel credentials");
    if ((request.domain != IdDomain::user && request.domain != IdDomain::group) ||
        (request.operation != IdChange::single && request.operation != IdChange::real_effective &&
         request.operation != IdChange::real_effective_saved))
        return Status::invalid_argument("invalid identity transition");
    const bool group = request.domain == IdDomain::group;
    const u32 real = group ? old.gid : old.uid;
    const u32 effective = group ? old.egid : old.euid;
    const u32 saved = group ? old.sgid : old.suid;
    const bool privileged = old.euid == root_uid;
    u32 next_real = real, next_effective = effective, next_saved = saved;
    auto known = [&](u32 id) { return id == real || id == effective || id == saved; };
    if (request.operation == IdChange::single)
    {
        if (request.real == unchanged_id)
            return Status::invalid_argument("single identity setter requires a valid ID");
        if (!privileged && request.real != real && request.real != saved)
            return Status::permission("single identity setter requires real or saved ID");
        next_effective = request.real;
        if (privileged) next_real = next_saved = request.real;
    }
    else if (request.operation == IdChange::real_effective)
    {
        if (!privileged &&
            ((request.real != unchanged_id && request.real != real && request.real != effective) ||
             (request.effective != unchanged_id && !known(request.effective))))
            return Status::permission("real/effective identity transition denied");
        if (request.real != unchanged_id) next_real = request.real;
        if (request.effective != unchanged_id) next_effective = request.effective;
        if (request.real != unchanged_id || (request.effective != unchanged_id && request.effective != real))
            next_saved = next_effective;
    }
    else
    {
        if (!privileged && ((request.real != unchanged_id && !known(request.real)) ||
                            (request.effective != unchanged_id && !known(request.effective)) ||
                            (request.saved != unchanged_id && !known(request.saved))))
            return Status::permission("real/effective/saved identity transition denied");
        if (request.real != unchanged_id) next_real = request.real;
        if (request.effective != unchanged_id) next_effective = request.effective;
        if (request.saved != unchanged_id) next_saved = request.saved;
    }
    auto next = old;
    if (group) { next.gid = next_real; next.egid = next_effective; next.sgid = next_saved; }
    else { next.uid = next_real; next.euid = next_effective; next.suid = next_saved; }
    return next;
}

} // namespace ok::user
