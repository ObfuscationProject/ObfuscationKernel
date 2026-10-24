#include "ok/net/native_udp_sdk_profile.hpp"
#include "ok/fs/ext4_mounted_common_profile.hpp"
#include "ok/uapi/syscall.h"
static_assert(OK_UAPI_VERSION_MAJOR == 0 && OK_UAPI_VERSION_MINOR == 16 && OK_UAPI_VERSION_PATCH == 0);
extern "C" ok::u32 ok_native_udp_sdk_profile_contract()
{
    return ok::net::native_udp_module_abi;
}
