#include <ok/net/tcp_transport.hpp>
using TcpConnection = ok::net::tcp::Connection<>;
extern "C" ok::Status tcp_active_object(TcpConnection &c, ok::net::tcp::Endpoint a,
    ok::net::tcp::Endpoint b, ok::u32 iss, ok::u64 now) { return c.active_open(a, b, iss, now); }
extern "C" ok::Status tcp_passive_object(TcpConnection &c, ok::net::tcp::Endpoint a,
    ok::u32 iss, ok::u64 now) { return c.passive_open(a, iss, now); }
extern "C" ok::Result<ok::net::tcp::Ingress> tcp_receive_object(TcpConnection &c,
    ok::net::wire::Bytes packet, ok::u64 now) { return c.receive(packet, now); }
extern "C" ok::Result<std::optional<ok::net::tcp::PreparedPacket>> tcp_prepare_object(TcpConnection &c,
    std::span<std::byte> output, ok::u64 now) { return c.prepare(output, now); }
extern "C" ok::Status tcp_commit_object(TcpConnection &c, ok::u64 token, bool accepted,
    ok::u64 now) { return c.complete_transmit(token, accepted, now); }
extern "C" ok::Result<ok::usize> tcp_write_object(TcpConnection &c, ok::net::wire::Bytes bytes) { return c.write(bytes); }
extern "C" ok::Result<ok::usize> tcp_read_object(TcpConnection &c, std::span<std::byte> bytes) { return c.read(bytes); }
extern "C" ok::Status tcp_shutdown_object(TcpConnection &c) { return c.shutdown_write(); }
extern "C" ok::Status tcp_tick_object(TcpConnection &c, ok::u64 now) { return c.tick(now); }

extern "C" ok::Status tcp_rearm_object(TcpConnection &c, ok::u32 iss, ok::u64 now) { return c.rearm_listener(iss, now); }
