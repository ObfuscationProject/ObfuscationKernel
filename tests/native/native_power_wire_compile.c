/* Compile-only native register scalars; no user-memory record or trap execution. */
#include "ok/uapi/syscall.h"
#include <limits.h>

_Static_assert(OK_UAPI_VERSION_MAJOR == 0 && OK_UAPI_VERSION_MINOR == 16 && OK_UAPI_VERSION_PATCH == 0,
               "native power requires the explicit UAPI0.15.0 SDK");
_Static_assert(OK_NATIVE_POWER_VERSION == 1, "native power version");
_Static_assert(OK_POWER_TOKEN_MAX == 0x3fffffffU && OK_POWER_TOKEN_MAX <= INT32_MAX,
               "power token must remain positive in every native signed return word");
_Static_assert(sizeof(long) == sizeof(uintptr_t) && sizeof(uint32_t) == 4, "native scalar widths");
_Static_assert(OK_SYS_POWER_PREPARE == 1032ULL && OK_SYS_POWER_COMMIT == 1033ULL, "native power numbers");

__attribute__((used, section(".ok_power_wire_c")))
const uint32_t ok_power_wire_c[] = {
    OK_UAPI_VERSION_MAJOR, OK_UAPI_VERSION_MINOR, OK_UAPI_VERSION_PATCH, OK_NATIVE_POWER_VERSION,
    OK_POWER_HALT, OK_POWER_REBOOT, OK_POWER_POWEROFF, OK_POWER_TOKEN_MAX,
    OK_SYS_POWER_PREPARE, OK_SYS_POWER_COMMIT, sizeof(uintptr_t) * CHAR_BIT,
    (uint32_t)(int32_t)-OK_EPERM, (uint32_t)(int32_t)-OK_EINVAL, (uint32_t)(int32_t)-OK_EOPNOTSUPP,
};

long compile_power_scalar_result(uint32_t token)
{
    return token && token <= OK_POWER_TOKEN_MAX ? (long)token : -OK_EINVAL;
}
