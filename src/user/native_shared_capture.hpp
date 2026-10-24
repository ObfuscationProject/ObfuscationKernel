#pragma once

#include "native_syscall_origin.hpp"
#include "ok/user/native_vfs_stat_continuation.hpp"

namespace ok::user::native
{
template <abi::linux_user::Profile Selected>
class SharedNativeCapture final : private NonCopyable
{
  public:
    using Origins = SyscallOriginTable<Selected>;
    using Vfs = VfsStatContinuation<Selected>;
    explicit constexpr SharedNativeCapture(memory::KernelEntryStackPool &pool) : backend_(pool) {}
    Result<arch::execution::Trap> enter(Origins &origins, Vfs &vfs, sched::ProcessControlBlock &process,
                                       sched::ThreadControlBlock &thread,
                                       const arch::execution::KernelEntryStackView &view,
                                       sched::SchedulerState &scheduler)
    {
        auto restart_stage = origins.stage_capture(process, thread, view, scheduler);
        if (!restart_stage) return restart_stage.status();
        auto vfs_stage = vfs.stage_capture(process, thread, view, scheduler);
        if (!vfs_stage) return vfs_stage.status();
        auto observation = backend_.enter(scheduler, process, thread, view);
        if (!observation) return observation.status();
        auto status = origins.observe(restart_stage.value(), observation.value(), process, thread, view, scheduler);
        if (status) status = vfs.observe(vfs_stage.value(), observation.value(), process, thread, view, scheduler);
        if (!status)
        {
            origins.revoke(thread);
            vfs.revoke(thread);
            return status;
        }
        return observation.value().trap();
    }
  private:
    BackendCapture backend_;
};
} // namespace ok::user::native
