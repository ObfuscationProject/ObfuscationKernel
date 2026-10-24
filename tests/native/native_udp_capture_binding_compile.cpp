#include "../../src/user/native_shared_capture.hpp"
#include <type_traits>

using namespace ok;
using Origins = user::native::SyscallOriginTable<abi::linux_user::configured_native_profile()>;
static_assert(!Origins::supported_binding<Origins::NativeUdpReceiveBinding>);
static_assert(!std::is_default_constructible_v<Origins::NativeUdpCapture>);
static_assert(!std::is_constructible_v<Origins::NativeUdpCapture, const Origins *, Origins::Identity>);
static_assert(std::is_nothrow_copy_constructible_v<Origins::NativeUdpCapture>);

void qualify_native_udp_capture_binding(Origins &origins, const Origins::NativeUdpCapture &capture,
    sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread,
    const Origins::NativeUdpReceiveBinding &before, const Origins::NativeUdpReceiveBinding &after)
{
    static_cast<void>(origins.native_udp_capture(process, thread));
    static_cast<void>(origins.bind_native_udp(capture, thread.native_frame, before));
    static_cast<void>(origins.validate_native_udp(capture, thread.native_frame, before, Origins::NativeUdpWaitState::pending));
    static_cast<void>(origins.validate_native_udp(capture, thread.native_frame, before, Origins::NativeUdpWaitState::ready));
    static_cast<void>(origins.rearm_native_udp(capture, thread.native_frame, before, after));
    origins.retire_native_udp(capture, after);
    static_cast<void>(origins.identity(process, thread));
}
