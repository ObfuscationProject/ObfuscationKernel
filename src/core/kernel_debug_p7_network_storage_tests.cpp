#include "kernel_roadmap_tests.hpp"

#include "ok/fs/ext4.hpp"
#include "ok/fs/storage.hpp"
#include "ok/syscall/linux.hpp"

#include <array>
#include <span>

namespace ok
{
namespace
{

std::span<const std::byte> as_bytes(std::string_view text)
{
    return {reinterpret_cast<const std::byte *>(text.data()), text.size()};
}

void write_le16(std::span<std::byte> out, usize offset, u16 value)
{
    out[offset] = static_cast<std::byte>(value & 0xffu);
    out[offset + 1] = static_cast<std::byte>((value >> 8) & 0xffu);
}

void write_le32(std::span<std::byte> out, usize offset, u32 value)
{
    write_le16(out, offset, static_cast<u16>(value & 0xffffu));
    write_le16(out, offset + 2, static_cast<u16>((value >> 16) & 0xffffu));
}

std::array<std::byte, 4096> make_ext4_test_image(bool valid_magic)
{
    std::array<std::byte, 4096> image{};
    auto bytes = std::span<std::byte>(image.data(), image.size());
    const auto base = fs::ext4_superblock_offset;
    write_le32(bytes, base + 0x00, 128);
    write_le32(bytes, base + 0x04, 4);
    write_le32(bytes, base + 0x0c, 2);
    write_le32(bytes, base + 0x18, 0);
    write_le16(bytes, base + 0x38, valid_magic ? fs::ext4_superblock_magic : 0);
    write_le16(bytes, base + 0x58, 256);
    write_le32(bytes, base + 0x60, 0x40);

    constexpr std::string_view name{"P7EXT4"};
    for (usize i = 0; i < name.size(); ++i)
    {
        bytes[base + 0x78 + i] = static_cast<std::byte>(name[i]);
    }
    bytes[1024] = std::byte{0x7e};
    return image;
}

Status test_network_device_and_protocols(Kernel &kernel)
{
    net::VirtioNetDevice netdev;
    std::array<std::byte, 64> frame{};
    if (netdev.transmit(frame).code() != StatusCode::not_initialized)
    {
        return Status::fault("virtio-net transmit before start was not rejected");
    }
    if (auto status = netdev.probe(); !status.ok())
    {
        return status;
    }
    constexpr net::EthernetAddress mac{{0x02, 0, 0, 0, 0, 2}};
    if (auto status = netdev.start(mac); !status.ok())
    {
        return status;
    }
    if (!netdev.started() || netdev.mac().octets != mac.octets)
    {
        return Status::fault("virtio-net MAC assignment validation failed");
    }
    if (netdev.transmit(std::span<const std::byte>{}).code() != StatusCode::invalid_argument)
    {
        return Status::fault("virtio-net accepted an empty frame");
    }
    if (auto status = netdev.transmit(frame); !status.ok())
    {
        return status;
    }
    if (auto status = netdev.receive(frame); !status.ok())
    {
        return status;
    }
    if (netdev.tx_count() != 1 || netdev.rx_count() != 1)
    {
        return Status::fault("virtio-net packet counters failed");
    }

    net::ArpCache arp;
    if (auto status = arp.learn(kernel.network().local_address(), netdev.mac()); !status.ok())
    {
        return status;
    }
    constexpr net::EthernetAddress updated_mac{{0x02, 0, 0, 0, 0, 3}};
    if (auto status = arp.learn(kernel.network().local_address(), updated_mac); !status.ok())
    {
        return status;
    }
    auto resolved = arp.lookup(kernel.network().local_address());
    if (!resolved || resolved.value().octets != updated_mac.octets || arp.size() != 1 ||
        arp.lookup(net::Ipv4Address{{10, 0, 0, 1}}).status().code() != StatusCode::not_found)
    {
        return Status::fault("ARP cache request/reply validation failed");
    }

    constexpr std::string_view ping{"ping"};
    auto echo = kernel.network().send_icmp_echo(kernel.network().local_address(), 0x1234, 7, as_bytes(ping));
    if (!echo || echo.value().identifier != 0x1234 || echo.value().sequence != 7 ||
        echo.value().payload_size != ping.size())
    {
        return Status::fault("ICMP echo loopback validation failed");
    }
    if (kernel.network()
            .send_icmp_echo(net::Ipv4Address{{192, 0, 2, 1}}, 1, 1, as_bytes(ping))
            .status()
            .code() != StatusCode::unsupported)
    {
        return Status::fault("ICMP non-loopback route was not rejected");
    }

    return netdev.stop();
}

Status test_socket_table(Kernel &kernel)
{
    net::SocketTable sockets;
    if (sockets.socket(net::SocketType::udp).status().code() != StatusCode::not_initialized)
    {
        return Status::fault("socket allocation before initialization was not rejected");
    }
    if (auto status = sockets.initialize(kernel.network()); !status.ok())
    {
        return status;
    }
    if (sockets.poll(99).status().code() != StatusCode::not_found)
    {
        return Status::fault("invalid socket poll was not rejected");
    }

    auto udp = sockets.socket(net::SocketType::udp);
    if (!udp)
    {
        return udp.status();
    }
    if (syscall::ErrnoMapper::errno_for(sockets.listen(udp.value())) != syscall::linux_EINVAL)
    {
        return Status::fault("invalid socket operation errno mapping failed");
    }
    if (auto status = sockets.bind(udp.value(), net::UdpEndpoint{.address = kernel.network().local_address(), .port = 41000});
        !status.ok())
    {
        return status;
    }
    if (auto status =
            sockets.connect(udp.value(), net::UdpEndpoint{.address = kernel.network().local_address(), .port = 41001});
        !status.ok())
    {
        return status;
    }
    constexpr std::string_view payload{"sock"};
    auto written = sockets.write(udp.value(), as_bytes(payload));
    if (!written || written.value() != payload.size())
    {
        return Status::fault("UDP socket FD write validation failed");
    }
    auto ready = sockets.poll(udp.value());
    if (!ready || (ready.value() & 0x01u) == 0 || (ready.value() & 0x04u) == 0)
    {
        return Status::fault("UDP socket poll validation failed");
    }
    std::array<std::byte, 16> socket_read{};
    auto read = sockets.read(udp.value(), socket_read);
    if (!read || read.value() != payload.size() || socket_read[0] != std::byte{'s'})
    {
        return Status::fault("UDP socket FD read validation failed");
    }
    if (sockets.recvfrom(udp.value()).status().code() != StatusCode::would_block)
    {
        return Status::fault("empty UDP receive queue did not report would_block");
    }

    auto tcp_listener = sockets.socket(net::SocketType::tcp);
    auto tcp_client = sockets.socket(net::SocketType::tcp);
    if (!tcp_listener || !tcp_client)
    {
        return Status::fault("TCP socket allocation failed");
    }
    if (auto status = sockets.bind(tcp_listener.value(),
                                   net::UdpEndpoint{.address = kernel.network().local_address(), .port = 42000});
        !status.ok())
    {
        return status;
    }
    if (auto status = sockets.listen(tcp_listener.value()); !status.ok())
    {
        return status;
    }
    if (auto status = sockets.connect(tcp_client.value(),
                                      net::UdpEndpoint{.address = kernel.network().local_address(), .port = 42000});
        !status.ok())
    {
        return status;
    }
    auto accepted = sockets.accept(tcp_listener.value());
    if (!accepted || sockets.find(accepted.value()) == nullptr)
    {
        return Status::fault("TCP accept validation failed");
    }
    if (sockets.sendto(tcp_client.value(), as_bytes(payload),
                       net::UdpEndpoint{.address = kernel.network().local_address(), .port = 42000})
            .status()
            .code() != StatusCode::invalid_argument)
    {
        return Status::fault("invalid TCP sendto operation was not rejected");
    }
    if (auto status = sockets.shutdown(tcp_client.value()); !status.ok())
    {
        return status;
    }

    return Status::success();
}

Status test_storage(Kernel &kernel)
{
    fs::BlockCache detached;
    std::array<std::byte, driver::block_sector_size> read_block{};
    if (detached.read_block(0, read_block).code() != StatusCode::not_initialized)
    {
        return Status::fault("detached block cache read was not rejected");
    }

    fs::BlockCache cache;
    if (auto status = cache.attach(kernel.disk()); !status.ok())
    {
        return status;
    }
    std::array<std::byte, driver::block_sector_size> block{};
    block[0] = std::byte{0x5a};
    if (auto status = cache.write_block(4, block); !status.ok())
    {
        return status;
    }
    if (auto status = cache.read_block(4, read_block); !status.ok())
    {
        return status;
    }
    if (auto status = cache.read_block(4, read_block); !status.ok())
    {
        return status;
    }
    if (cache.stats().misses != 1 || cache.stats().hits != 1 || read_block[0] != std::byte{0x5a})
    {
        return Status::fault("block cache hit/miss validation failed");
    }
    if (cache.read_block(kernel.disk().geometry().block_count, read_block).code() != StatusCode::invalid_argument ||
        cache.write_block(kernel.disk().geometry().block_count, block).code() != StatusCode::invalid_argument)
    {
        return Status::fault("out-of-range block access was not rejected");
    }

    std::array<std::byte, driver::block_sector_size> mbr{};
    fs::PartitionTable partitions;
    if (partitions.parse_mbr(mbr).code() != StatusCode::invalid_argument)
    {
        return Status::fault("invalid MBR signature was not rejected");
    }
    mbr[510] = std::byte{0x55};
    mbr[511] = std::byte{0xaa};
    if (partitions.parse_mbr(mbr).code() != StatusCode::not_found)
    {
        return Status::fault("empty MBR partition table was not rejected");
    }
    mbr[446 + 4] = std::byte{0x83};
    write_le32(mbr, 446 + 8, 2048);
    write_le32(mbr, 446 + 12, 128);
    if (auto status = partitions.parse_mbr(mbr); !status.ok())
    {
        return status;
    }
    if (partitions.partition_count() != 1 || partitions.partition(0) == nullptr ||
        partitions.partition(0)->first_lba != 2048 || partitions.partition(1) != nullptr)
    {
        return Status::fault("partition table validation failed");
    }

    auto ext4_image = make_ext4_test_image(true);
    fs::Ext4Volume volume;
    if (auto status = volume.mount(ext4_image); !status.ok())
    {
        return status;
    }
    auto info = volume.info();
    if (!info || info.value().block_size != 1024 || info.value().inode_size != 256 ||
        info.value().volume_name.view() != "P7EXT4" || !info.value().has_extents)
    {
        return Status::fault("EXT4 superblock validation failed");
    }
    std::array<std::byte, 1024> ext4_block{};
    if (auto status = volume.read_block(1, ext4_block); !status.ok())
    {
        return status;
    }
    if (ext4_block[0] != std::byte{0x7e} ||
        volume.read_block(4, ext4_block).code() != StatusCode::invalid_argument)
    {
        return Status::fault("EXT4 read-only block validation failed");
    }
    auto corrupt_ext4_image = make_ext4_test_image(false);
    fs::Ext4Volume corrupt;
    if (corrupt.mount(corrupt_ext4_image).code() != StatusCode::invalid_argument)
    {
        return Status::fault("corrupted EXT4 superblock was not rejected");
    }

    return Status::success();
}

} // namespace

Status run_network_storage_roadmap_tests(Kernel &kernel, KernelTestReport &report)
{
    if (auto status = test_network_device_and_protocols(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = test_socket_table(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = test_storage(kernel); !status.ok())
    {
        return status;
    }

    report.netdev = true;
    report.sockets = true;
    report.block = true;
    report.ext4_readonly = true;
    return Status::success();
}

} // namespace ok
