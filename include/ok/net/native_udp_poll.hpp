#pragma once

#include "ok/net/native_udp_executor.hpp"
#include "ok/posix/posix.hpp"

namespace ok::net
{

// Passive copied readiness only. Credit is a hint, never a TX reservation.
[[nodiscard]] inline Result<posix::DescriptorReadiness> native_udp_descriptor_readiness(
    const Result<NativeUdpReceiveReadiness> &joined, bool readable, bool writable,
    posix::ReadyEvents requested)
{
    if (!joined)
    {
        if (joined.status().code() == StatusCode::bad_descriptor)
            return posix::DescriptorReadiness{.events = posix::ReadyEvents::invalid};
        return joined.status();
    }
    const auto &snapshot = joined.value();
    posix::DescriptorReadiness ready{};
    if (readable && snapshot.receive.readable())
        ready.events |= requested & posix::ReadyEvents::readable;
    const bool terminal = snapshot.receive.terminal || snapshot.executor.failed;
    if (terminal) ready.events |= posix::ReadyEvents::error;
    if (writable && !terminal && snapshot.executor.active && snapshot.executor.transmit_credit)
        ready.events |= requested & posix::ReadyEvents::writable;
    return ready;
}

} // namespace ok::net
