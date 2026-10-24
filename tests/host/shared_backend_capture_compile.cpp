#include "../../src/user/native_shared_capture.hpp"
using namespace ok;
constexpr auto profile = abi::linux_user::configured_native_profile();
void qualify_shared_capture(user::native::SharedNativeCapture<profile> &capture,
                            user::native::SyscallOriginTable<profile> &origins,
                            user::native::VfsStatContinuation<profile> &vfs,
                            sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread,
                            const arch::execution::KernelEntryStackView &view, sched::SchedulerState &scheduler)
{
    static_cast<void>(capture.enter(origins, vfs, process, thread, view, scheduler));
}
