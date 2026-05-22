#include "ok/core/shell.hpp"

#include "ok/core/kernel.hpp"
#include "shell_private.hpp"

namespace ok
{

using shell_detail::after_first_word;
using shell_detail::as_bytes;
using shell_detail::first_word;
using shell_detail::parse_unsigned;

Status KernelDebugShell::command_net(std::string_view args)
{
    if (kernel_ == nullptr)
    {
        return Status::not_initialized("shell has no kernel");
    }
    const auto subcommand = first_word(args);
    const auto rest = after_first_word(args);
    if (subcommand.empty() || subcommand == "status")
    {
        const auto stats = kernel_->network().stats();
        if (auto status = append("ip=127.0.0.1 udp_queue="); !status.ok())
        {
            return status;
        }
        if (auto status = append_unsigned(kernel_->network().udp_queued()); !status.ok())
        {
            return status;
        }
        if (auto status = append(" tcp_listeners="); !status.ok())
        {
            return status;
        }
        if (auto status = append_unsigned(kernel_->network().tcp_listener_count()); !status.ok())
        {
            return status;
        }
        if (auto status = append(" ipv4_tx="); !status.ok())
        {
            return status;
        }
        if (auto status = append_unsigned(stats.ipv4_tx); !status.ok())
        {
            return status;
        }
        if (auto status = append(" ipv4_rx="); !status.ok())
        {
            return status;
        }
        if (auto status = append_unsigned(stats.ipv4_rx); !status.ok())
        {
            return status;
        }
        return append("\n");
    }
    if (subcommand == "udp")
    {
        const auto port_text = first_word(rest);
        const auto payload = after_first_word(rest);
        auto port = parse_unsigned(port_text);
        if (!port)
        {
            return port.status();
        }
        if (port.value() > 0xffffu)
        {
            return Status::invalid_argument("UDP port out of range");
        }
        return kernel_->network().send_udp(
            net::UdpEndpoint{.address = kernel_->network().local_address(), .port = 30000},
            net::UdpEndpoint{.address = kernel_->network().local_address(), .port = static_cast<u16>(port.value())},
            as_bytes(payload));
    }
    if (subcommand == "recv")
    {
        auto datagram = kernel_->network().receive_udp();
        if (!datagram)
        {
            return datagram.status();
        }
        if (auto status = append("udp port="); !status.ok())
        {
            return status;
        }
        if (auto status = append_unsigned(datagram.value().destination.port); !status.ok())
        {
            return status;
        }
        if (auto status = append(" payload="); !status.ok())
        {
            return status;
        }
        for (usize i = 0; i < datagram.value().payload_size; ++i)
        {
            if (auto status = output_.append(static_cast<char>(datagram.value().payload[i])); !status.ok())
            {
                return status;
            }
        }
        return append("\n");
    }
    if (subcommand == "listen")
    {
        auto port = parse_unsigned(rest);
        if (!port)
        {
            return port.status();
        }
        if (port.value() > 0xffffu)
        {
            return Status::invalid_argument("TCP port out of range");
        }
        return kernel_->network().listen_tcp(static_cast<u16>(port.value()));
    }
    if (subcommand == "tcp")
    {
        auto port = parse_unsigned(rest);
        if (!port)
        {
            return port.status();
        }
        if (port.value() > 0xffffu)
        {
            return Status::invalid_argument("TCP port out of range");
        }
        auto connection = kernel_->network().connect_tcp(
            net::UdpEndpoint{.address = kernel_->network().local_address(), .port = static_cast<u16>(port.value())},
            40000);
        if (!connection)
        {
            return connection.status();
        }
        if (auto status = append("tcp state="); !status.ok())
        {
            return status;
        }
        if (auto status = append(net::tcp_state_name(connection.value().state)); !status.ok())
        {
            return status;
        }
        return append("\n");
    }
    return Status::invalid_argument("unknown net subcommand");
}

} // namespace ok
