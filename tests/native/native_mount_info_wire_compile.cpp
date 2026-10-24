/* Emitted native C++ layout and constants; compile-only evidence. */
#include <ok/uapi/syscall.h>
#include <cstddef>
#include "ok/user/native_mount_info.hpp"
#include "ok/core/module.hpp"
static_assert(sizeof(struct ok_mount_info) == 288, "fixed mount inventory wire");
static_assert(sizeof(struct ok_mount_info) <= 1024, "bounded mount output");
__attribute__((used, section(".ok_mount_wire_cpp")))
extern const uint32_t ok_mount_wire_cpp[] = {
    OK_UAPI_VERSION_MAJOR,
    OK_UAPI_VERSION_MINOR,
    OK_UAPI_VERSION_PATCH,
    OK_NATIVE_MOUNT_INFO_VERSION,
    OK_SYS_MOUNT_INFO,
    sizeof(struct ok_mount_info),
    offsetof(struct ok_mount_info, version),
    offsetof(struct ok_mount_info, size),
    offsetof(struct ok_mount_info, generation),
    offsetof(struct ok_mount_info, slot),
    offsetof(struct ok_mount_info, capacity),
    offsetof(struct ok_mount_info, state),
    offsetof(struct ok_mount_info, parent_id),
    offsetof(struct ok_mount_info, path_length),
    offsetof(struct ok_mount_info, path),
    OK_MOUNT_INFO_CAPACITY,
    OK_MOUNT_INFO_PATH_CAPACITY,
    OK_MOUNT_STATE_UNUSED,
    OK_MOUNT_STATE_UNDERLYING_RAM,
    OK_MOUNT_STATE_BORROWED,
    OK_MOUNT_STATE_OWNED_PROVIDER,
    OK_MOUNT_STATE_OWNED_RAM,
    OK_MOUNT_STATE_DETACHED,
    alignof(struct ok_mount_info),
    sizeof(uint32_t),
    sizeof(uintptr_t) * 8,
    UINT32_C(0xffffffff),
    (uint32_t)-22,
    (uint32_t)-38,
    (uint32_t)-14
};

static_assert(ok::kernel_module_abi_version == 74);
extern ok::Status mount_compile_snapshot(ok::u32, ok::fs::MountSnapshot &);
extern bool mount_compile_range(ok::u64, ok::usize);
extern bool mount_compile_copy(ok::u64, std::span<const ok::u8>);
ok::i64 mount_compile_dispatch(const std::array<ok::u64, 6> &arguments)
{
    return ok::user::native_mount_info::dispatch(true, arguments, mount_compile_snapshot,
                                                mount_compile_range, mount_compile_copy);
}
