#include "ok/user/linux_bootstrap.hpp"

ok::i64 compile_linux_bootstrap(ok::Kernel &kernel, ok::sched::ProcessId pid,
    ok::arch::execution::UserFrame &frame, bool &complete,
    ok::i64 (*native)(ok::arch::execution::Syscall, bool &))
{
    return ok::user::linux_bootstrap::dispatch<ok::abi::linux_user::configured_native_profile()>(
        kernel,pid,frame,complete,native);
}
