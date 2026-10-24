#include "ok/arch/execution.hpp"
#include "ok/syscall/syscall.hpp"
#include "ok/uapi/syscall.h"
#include "ok/user/native_power_profile.hpp"
#include <limits>
#include <type_traits>

static_assert(static_cast<ok::u64>(ok::syscall::Number::power_prepare) == OK_SYS_POWER_PREPARE);
static_assert(static_cast<ok::u64>(ok::syscall::Number::power_commit) == OK_SYS_POWER_COMMIT);
static_assert(OK_POWER_TOKEN_MAX == ok::user::native_power_token_limit);
static_assert(OK_NATIVE_POWER_VERSION == ok::user::native_power_version);
static_assert(OK_POWER_TOKEN_MAX <= std::numeric_limits<ok::i32>::max());
static_assert(sizeof(ok::arch::execution::Syscall::number) == sizeof(ok::uptr));
static_assert(std::is_same_v<decltype(ok::arch::execution::Syscall::arguments)::value_type, ok::uptr>);
static_assert(ok::arch::execution::Syscall{}.arguments.size() == OK_UAPI_SYSCALL_ARG_COUNT);

extern "C"
{
__attribute__((used, section(".ok_power_wire_cpp")))
extern const ok::u32 ok_power_wire_cpp[] = {
    OK_UAPI_VERSION_MAJOR, OK_UAPI_VERSION_MINOR, OK_UAPI_VERSION_PATCH, OK_NATIVE_POWER_VERSION,
    OK_POWER_HALT, OK_POWER_REBOOT, OK_POWER_POWEROFF, OK_POWER_TOKEN_MAX,
    static_cast<ok::u32>(ok::syscall::Number::power_prepare), static_cast<ok::u32>(ok::syscall::Number::power_commit),
    sizeof(ok::uptr) * 8,
    static_cast<ok::u32>(-OK_EPERM), static_cast<ok::u32>(-OK_EINVAL), static_cast<ok::u32>(-OK_EOPNOTSUPP),
};
}

ok::arch::execution::Syscall compile_power_scalar_arguments(ok::u32 token)
{
    return {OK_SYS_POWER_COMMIT, {token, 0, 0, 0, 0, 0}};
}
