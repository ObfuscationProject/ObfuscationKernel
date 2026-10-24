#include "ok/user/native_signal_action.hpp"
using namespace ok;
using Codec=user::native::SignalActionCodec<abi::linux_user::Profile::ACTION_PROFILE>;
static_assert(Codec::bytes<=32);
static_assert(Codec::sanitize({.flags=~u64{0}}).flags==Codec::uapi_flags);
static_assert(Codec::encode({})[0]==std::byte{0});
extern "C" i64 exercise_internal_action(sched::SignalActionTable &table,i32 signal,u64 input,u64 output)
{
    return user::native::update_signal_action<abi::linux_user::Profile::ACTION_PROFILE>(table,signal,input,output,Codec::Mask::bytes,
        [](u64,std::span<std::byte> buffer){for(auto &byte:buffer)byte=std::byte{0};return true;},
        [](u64,std::span<const std::byte> buffer){return !buffer.empty();},[](u32) noexcept {});
}
extern "C" void exercise_action_lifetime(sched::SignalActionTable &source,sched::SignalActionTable &destination)
{
    destination=source.fork_copy();destination.reset_for_exec();
}
