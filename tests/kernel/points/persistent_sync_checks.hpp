#pragma once

#include "ok/core/kernel.hpp"
#include "ok/uapi/syscall.h"

namespace ok::tests
{
// Refusal/model coverage only; mounted I/O and native execution are separate.
inline Status check_persistent_sync_refusal_model(Kernel &kernel)
{
    fs::RamNode volatile_node;
    if (!volatile_node.sync() || volatile_node.sync_persistent().code() != StatusCode::unsupported ||
        kernel.posix().sync_persistent(-1).code() != StatusCode::bad_descriptor)
        return Status::fault("persistent sync volatile/bad-fd refusal model failed");
    static_assert(OK_SYS_SYNC_PERSISTENT == static_cast<u64>(syscall::Number::sync_persistent));
    for (usize index = 1; index < 6; ++index)
    {
        syscall::Request request{.number = syscall::Number::sync_persistent};
        request.args[index] = 1;
        if (kernel.syscalls().dispatch(request).status.code() != StatusCode::invalid_argument)
            return Status::fault("persistent sync reserved argument refusal model failed");
    }
    return {};
}
} // namespace ok::tests
