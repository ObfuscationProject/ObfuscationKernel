// Freestanding object probe only; no link, SDK acceptance or execution claim.
#include "ok/user/linux_bootstrap.hpp"
using namespace ok;
namespace lu = abi::linux_user;
constexpr auto profile = lu::configured_native_profile();
static_assert(lu::Codec<profile>::syscall_number(lu::Operation::fchmod).has_value());
static_assert(lu::Codec<profile>::syscall_number(lu::Operation::fchmodat).has_value());
static_assert(lu::Codec<profile>::syscall_number(lu::Operation::fchmodat2).has_value());
static_assert(lu::Codec<profile>::syscall_number(lu::Operation::chmod).has_value() ==
              lu::ChmodCallAdapter<profile>::has_chmod);
static_assert(*lu::Codec<profile>::errno_number(lu::Error::e_rofs) == 30);
static_assert(sizeof(uptr) == lu::Codec<profile>::profile.word_bytes);

i64 compile_chmod_dispatch(Kernel &kernel, sched::ProcessId pid,
                          arch::execution::UserFrame &frame, bool &done,
                          i64 (*native)(arch::execution::Syscall, bool &))
{
    return user::linux_bootstrap::dispatch<profile>(kernel, pid, frame, done, native);
}

Status compile_chmod_completion(arch::execution::UserFrame &frame, i64 result)
{
    return user::linux_bootstrap::complete<profile>(frame, result);
}
