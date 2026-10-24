#include <ok/net/tcp_wire.hpp>

extern "C" ok::Result<ok::net::wire::Tcp> tcp_wire_parse_object(
    const ok::net::wire::Ipv4Packet &packet)
{
    return ok::net::wire::tcp(packet);
}
extern "C" ok::Result<ok::usize> tcp_wire_build_object(std::span<std::byte> output,
    ok::net::wire::Ipv4 source, ok::net::wire::Ipv4 destination,
    ok::net::wire::TcpFields fields, ok::net::wire::Bytes options, ok::net::wire::Bytes payload)
{
    return ok::net::wire::build_tcp(output, source, destination, 1, 64, fields, options, payload);
}
extern "C" ok::Result<std::optional<ok::net::wire::TcpOption>> tcp_wire_option_object(
    ok::net::wire::TcpOptions &cursor)
{
    return cursor.next();
}
