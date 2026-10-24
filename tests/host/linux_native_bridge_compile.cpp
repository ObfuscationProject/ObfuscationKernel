#include "ok/abi/linux_native.hpp"
#include <cstddef>

namespace ok::abi::linux_user
{
using Bridge = ConfiguredNativeBridge;
using Frame = arch::execution::UserFrame;
static_assert(sizeof(uptr) == Bridge::profile.word_bytes);
static_assert(offsetof(Frame, pc) == 32 * sizeof(uptr));
static_assert(offsetof(Frame, sp) == 33 * sizeof(uptr));
static_assert(offsetof(Frame, flags) == 34 * sizeof(uptr));
static_assert(offsetof(Frame, fp_state) == (sizeof(uptr) == 4 ? 160 : 304));
static_assert(configured_native_profile() != Profile::ppc || offsetof(Frame, fp_state) + 280 == 440);

// This isolated object is never linked into a kernel or executed as a guest.
Status compile_native_bridge(Frame &frame, std::span<const u8> copied)
{
    auto call = Bridge::decode_call(frame, 6, copied);
    if (!call)
        return call.status();
    auto no_user_access = [](uptr, std::span<u8>) { return Status::fault("compile-only user copy"); };
    static_cast<void>(Bridge::decode_call_copying_stack(frame, 6, no_user_access));
    if (auto status = Bridge::encode_return(frame, Return{0, 5}); !status.ok())
        return status;
    auto result = Bridge::decode_return(frame);
    if (!result)
        return result.status();
    return Bridge::set_thread_pointer(frame, ThreadPointer{call.value().arguments[0]}, Bridge::thread_pointer_route);
}
} // namespace ok::abi::linux_user
