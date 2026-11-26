#pragma once

#include "ok/net/native_tcp_stream.hpp"
#include "ok/posix/posix.hpp"

namespace ok::net
{

// Poll readiness of a TCP stream, following Linux tcp_poll:
// - a stream without an endpoint (fresh or only bound) is TCP_CLOSE, so it
//   reports POLLOUT and POLLHUP;
// - a listener is readable while a connection waits to be accepted;
// - during the handshake the stream is neither readable nor writable;
// - a received FIN makes it readable; a closed connection (finished, reset,
//   refused or timed out) has both directions shut down and so reports
//   POLLIN, POLLOUT and POLLHUP, plus POLLERR while its failure is not yet
//   reported (Linux sk_err).
[[nodiscard]] inline Result<posix::DescriptorReadiness> native_tcp_descriptor_readiness(
    const NativeTcpStreamService &service, TcpStreamHandle handle, posix::ReadyEvents requested)
{
    using E = posix::ReadyEvents;
    if (!service.valid(handle)) return posix::DescriptorReadiness{.events = E::invalid};
    auto snapshot = service.snapshot(handle); if (!snapshot) return snapshot.status();
    posix::DescriptorReadiness ready{};
    if (!snapshot.value().endpoint)
    {
        ready.events = (requested & E::writable) | E::hangup;
        return ready;
    }
    auto readiness = service.readiness(handle); if (!readiness) return readiness.status();
    const auto &r = readiness.value();
    if (snapshot.value().endpoint->kind == tcp::HandleKind::listener)
    {
        if (r.accept_count) ready.events |= requested & E::readable;
        if (r.terminal) ready.events |= E::error;
        return ready;
    }
    auto state = service.endpoints().state(*snapshot.value().endpoint); if (!state) return state.status();
    const auto &t = state.value().transport;
    const bool closed = state.value().terminal || t.state == tcp::State::closed || t.state == tcp::State::time_wait;
    const bool receive_shutdown = closed || t.eof, send_shutdown = closed || t.write_shutdown;
    if (closed || (receive_shutdown && send_shutdown)) ready.events |= E::hangup;
    if (receive_shutdown) ready.events |= requested & E::readable;
    if (t.state != tcp::State::syn_sent && t.state != tcp::State::syn_received)
    {
        if (t.receive_buffered) ready.events |= requested & E::readable;
        if (send_shutdown || r.writable_bytes) ready.events |= requested & E::writable;
    }
    if (state.value().terminal || t.error_pending) ready.events |= E::error;
    return ready;
}

} // namespace ok::net
