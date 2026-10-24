#pragma once

#include "linux_udp_immediate_operation.hpp"
#include "ok/abi/linux_udp_call.hpp"

namespace ok::user::native::linux_udp
{
template <class Access>
concept CapturedArgumentAccess = requires(Access &access, u64 address, std::span<std::byte> bytes) {
    { access.live() } -> std::same_as<bool>;
    { access.raw_syscall() } -> std::same_as<u64>;
    { access.operation() } -> std::same_as<LinuxUdpImmediateOperation>;
    { access.read(address, bytes) } -> std::same_as<Status>;
};

// Must be called inside the enclosing operation's non-reentrant scratch scope.
// The access owns image authority; this function only returns copied values.
template <abi::linux_user::Profile Target, CapturedArgumentAccess Access>
[[nodiscard]] Result<abi::linux_user::UdpRequest> decode_captured(
    const arch::execution::UserFrame &frame, Access &access)
{
    namespace lu = abi::linux_user;
    using Codec = lu::Codec<Target>;
    using Decoder = lu::UdpCallDecoder<Target>;
    if (!access.live()) return Status::denied("Linux UDP captured argument authority expired");
    // No borrowed frame reads occur after a user-copy callback.
    const auto snapshot = lu::NativeBridge<Target>::snapshot(frame);
    const u64 stack = static_cast<u64>(frame.sp) & Codec::word_mask;
    const u64 number = snapshot.integer[Codec::profile.number_register];
    if (number > 0xffffffffU || number != access.raw_syscall() ||
        linux_udp_immediate_operation<Target>(static_cast<u32>(number)) != access.operation() ||
        access.operation() == LinuxUdpImmediateOperation::none || !access.live())
        return Status::denied("Linux UDP request is outside its exact captured operation");
    auto copy = [&](u64 address, std::span<u8> bytes) -> Status {
        if (!access.live()) return Status::denied("Linux UDP argument authority expired");
        const auto status = access.read(address, {reinterpret_cast<std::byte *>(bytes.data()), bytes.size()});
        if (!access.live()) return Status::denied("Linux UDP argument authority expired");
        return status;
    };
    if (access.operation() == LinuxUdpImmediateOperation::socketcall)
        return Decoder::decode_socketcall(snapshot, copy);
    std::array<u8, 24> copied_stack{};
    if constexpr (Target == lu::Profile::mips)
    {
        if (access.operation() == LinuxUdpImmediateOperation::sendto ||
            access.operation() == LinuxUdpImmediateOperation::recvfrom)
        {
            if (stack > Codec::word_mask - 23)
                return Status::fault("Linux UDP O32 argument interval wraps");
            if (auto status = copy(stack + 16, std::span<u8>{copied_stack}.subspan(16, 8)); !status)
                return status;
            return Decoder::decode_direct(snapshot, copied_stack);
        }
    }
    return Decoder::decode_direct(snapshot);
}
} // namespace ok::user::native::linux_udp
