#include "ok/user/native_vfs_stat_continuation.hpp"

using namespace ok;
using Adapter = user::native::VfsStatContinuation<abi::linux_user::configured_native_profile()>;
using Frame = arch::execution::UserFrame;
void exercise(sched::SchedulerState &scheduler, fs::VirtualFileSystem &vfs, Adapter &adapter,
              sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread,
              const arch::execution::KernelEntryStackView &view)
{
    static_cast<void>(adapter.prepare(thread));
    static_cast<void>(adapter.enter(scheduler, process, thread, view));
    bool complete = true;
    const auto read = [](u64, FixedString<256> &path) { return path.assign("/tmp/file"); };
    const auto stack = [](uptr, std::span<u8> bytes) { for (auto &b : bytes) b = 0; return Status{}; };
    const auto admit = [](u64, usize) { return true; };
    const auto copy = [](u64, std::span<const u8>) { return Status{}; };
    static_cast<void>(adapter.begin(scheduler, vfs, process.pid(), thread.tid, read, stack, admit, copy, complete));
    const auto handle = adapter.pending(process.pid(), thread.tid);
    static_cast<void>(adapter.resume(scheduler, handle, admit, copy, complete));
    static_cast<void>(adapter.drain(scheduler, vfs));
    static_cast<void>(adapter.prune(scheduler));
    static_cast<void>(adapter.cancel(scheduler, handle));
    static_cast<void>(adapter.cancel_process(scheduler, process.pid()));
}
