// Freestanding object compilation only; no link or execution claim.
#include "ok/user/linux_bootstrap.hpp"
using namespace ok;
namespace lu = abi::linux_user;
constexpr auto profile = lu::configured_native_profile();
static_assert(lu::Codec<profile>::syscall_number(lu::Operation::faccessat).has_value());
static_assert(lu::Codec<profile>::syscall_number(lu::Operation::faccessat2).has_value());
static_assert(sizeof(uptr) == lu::Codec<profile>::profile.word_bytes);
static_assert(lu::AccessCallAdapter<profile>::argument_count(lu::AccessCall::faccessat) == 3);

i64 compile_access_dispatch(Kernel &kernel, sched::ProcessId pid,
                           arch::execution::UserFrame &frame, bool &done,
                           i64 (*native)(arch::execution::Syscall, bool &))
{
    return user::linux_bootstrap::dispatch<profile>(kernel, pid, frame, done, native);
}

Status compile_access_completion(arch::execution::UserFrame &frame, i64 result)
{
    return user::linux_bootstrap::complete<profile>(frame, result);
}
