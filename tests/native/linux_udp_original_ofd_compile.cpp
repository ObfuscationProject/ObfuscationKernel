#include "../../src/user/linux_udp_original_ofd.hpp"
using namespace ok;
namespace udp = user::native::linux_udp;
struct CompileAccess { void reset() {} };
extern "C" Status linux_udp_original_ofd_probe(sched::SchedulerState &scheduler,
                                              sched::ProcessControlBlock &process,
                                              Status &failure, i32 fd)
{
    udp::OriginalUdpDescription original{scheduler, failure};
    CompileAccess access;
    udp::ImmediateLifetimeScope scope{access, original};
    auto result = original.acquire(process, fd);
    if (!result) return result.status();
    if (result.value().kind != udp::DescriptionKind::retained_udp)
        return Status::bad_descriptor();
    return scope.finish();
}
