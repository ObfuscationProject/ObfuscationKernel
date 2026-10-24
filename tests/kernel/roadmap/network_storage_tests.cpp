#include "roadmap_tests.hpp"
#include "block_cache_failure.hpp"

#include "ok/fs/ext4.hpp"
#include "ok/fs/fat.hpp"
#include "ok/fs/fat_fs.hpp"
#include "ok/fs/storage.hpp"
#include "ok/net/native_udp.hpp"
#include "ok/net/wire.hpp"
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

void write_le64(std::span<std::byte> out, usize offset, u64 value)
{
    write_le32(out, offset, static_cast<u32>(value & 0xffff'ffffull));
    write_le32(out, offset + 4, static_cast<u32>((value >> 32) & 0xffff'ffffull));
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

    constexpr std::string_view name{"OKEXT4"};
    for (usize i = 0; i < name.size(); ++i)
    {
        bytes[base + 0x78 + i] = static_cast<std::byte>(name[i]);
    }
    bytes[1024] = std::byte{0x7e};
    return image;
}

std::array<std::byte, fs::fat_boot_sector_size> make_fat32_test_image(bool valid_signature)
{
    std::array<std::byte, fs::fat_boot_sector_size> image{};
    auto bytes = std::span<std::byte>(image.data(), image.size());
    bytes[0] = std::byte{0xeb};
    bytes[1] = std::byte{0x58};
    bytes[2] = std::byte{0x90};
    write_le16(bytes, 0x0b, 512);
    bytes[0x0d] = std::byte{8};
    write_le16(bytes, 0x0e, 32);
    bytes[0x10] = std::byte{2};
    write_le16(bytes, 0x11, 0);
    write_le16(bytes, 0x13, 0);
    write_le16(bytes, 0x16, 0);
    write_le32(bytes, 0x20, 65536);
    write_le32(bytes, 0x24, 256);
    write_le32(bytes, 0x2c, 2);
    constexpr std::string_view label{"OKFAT32"};
    for (usize i = 0; i < label.size(); ++i)
    {
        bytes[0x47 + i] = static_cast<std::byte>(label[i]);
    }
    constexpr std::string_view type{"FAT32   "};
    for (usize i = 0; i < type.size(); ++i)
    {
        bytes[0x52 + i] = static_cast<std::byte>(type[i]);
    }
    bytes[510] = valid_signature ? std::byte{0x55} : std::byte{0};
    bytes[511] = valid_signature ? std::byte{0xaa} : std::byte{0};
    return image;
}

std::array<std::byte, fs::fat_boot_sector_size> make_exfat_test_image()
{
    std::array<std::byte, fs::fat_boot_sector_size> image{};
    auto bytes = std::span<std::byte>(image.data(), image.size());
    constexpr std::string_view name{"EXFAT   "};
    for (usize i = 0; i < name.size(); ++i)
    {
        bytes[3 + i] = static_cast<std::byte>(name[i]);
    }
    write_le64(bytes, 0x48, 131072);
    write_le32(bytes, 0x50, 128);
    write_le32(bytes, 0x54, 256);
    write_le32(bytes, 0x58, 384);
    write_le32(bytes, 0x5c, 1024);
    write_le32(bytes, 0x60, 2);
    bytes[0x6c] = std::byte{9};
    bytes[0x6d] = std::byte{3};
    bytes[0x6e] = std::byte{1};
    bytes[510] = std::byte{0x55};
    bytes[511] = std::byte{0xaa};
    return image;
}

// Sparse metadata fixture: geometry is declared, only the boot sector is stored.
class FatBootFixtureDevice final : public driver::BlockDevice
{
  public:
    FatBootFixtureDevice(std::span<const std::byte> boot, u64 sectors) : boot_(boot), sectors_(sectors) {}
    driver::BlockGeometry geometry() const override { return {sectors_, 512, false}; }
    Status read_blocks(u64 sector, std::span<std::byte> out) override
    {
        if (sector >= sectors_ || out.size() != 512) return Status::invalid_argument();
        for (usize i = 0; i < out.size(); ++i) out[i] = sector ? std::byte{} : boot_[i];
        return {};
    }
    Status write_blocks(u64, std::span<const std::byte>) override { return Status::denied(); }
    Status flush() override { return {}; }
  private:
    std::span<const std::byte> boot_;
    u64 sectors_;
};

// Sparse writable block model reached through kernel_main; no physical-device claim.
class FatWritableFixtureDevice final : public driver::BlockDevice
{
  public:
    Status initialize()
    {
        auto boot = make_fat32_test_image(true);
        boot[13] = std::byte{1};
        write_le32(boot, 32, 131072);
        write_le32(boot, 36, 1024);
        write_le16(boot, 48, 0xffff);
        write_le16(boot, 50, 0xffff);
        if (auto status = write_blocks(0, boot); !status) return status;
        std::array<std::byte, 512> table{};
        write_le32(table, 0, 0x0ffffff8);
        write_le32(table, 4, 0x0fffffff);
        write_le32(table, 8, 0x0fffffff);
        if (auto status = write_blocks(32, table); !status) return status;
        return write_blocks(1056, table);
    }
    driver::BlockGeometry geometry() const override { return {131072, 512, true}; }
    Status read_blocks(u64 sector, std::span<std::byte> out) override
    {
        if (sector >= 131072 || out.size() != 512) return Status::invalid_argument();
        for (usize i = 0; i < used_; ++i)
            if (sectors_[i] == sector)
            {
                for (usize j = 0; j < 512; ++j) out[j] = data_[i][j];
                return {};
            }
        for (auto &byte : out) byte = std::byte{};
        return {};
    }
    Status write_blocks(u64 sector, std::span<const std::byte> bytes) override
    {
        if (sector >= 131072 || bytes.size() != 512) return Status::invalid_argument();
        usize index = 0;
        while (index < used_ && sectors_[index] != sector) ++index;
        if (index == data_.size()) return Status::no_memory("FAT model sparse sector capacity");
        if (index == used_) sectors_[used_++] = sector;
        for (usize j = 0; j < 512; ++j) data_[index][j] = bytes[j];
        return {};
    }
    Status flush() override { ++flushes; return {}; }
    usize flushes{};
  private:
    std::array<u64, 32> sectors_{};
    std::array<std::array<std::byte, 512>, 32> data_{};
    usize used_{};
};

Status test_fat_mounted_block_model()
{
    static FatWritableFixtureDevice disk;
    static fs::FatFileSystem volume;
    static fs::FatFileSystem remounted;
    if (auto status = disk.initialize(); !status) return status;
    if (auto status = volume.mount(disk); !status) return status;
    auto *root = volume.root();
    if (!root || !root->create("directory", fs::NodeType::directory))
        return Status::fault("FAT mounted directory create failed");
    auto *directory = root->lookup("DIRECTORY");
    if (!directory || !directory->create("Long file.txt", fs::NodeType::regular))
        return Status::fault("FAT mounted file create failed");
    auto *file = directory->lookup("long FILE.txt");
    const auto payload = as_bytes("mounted FAT bytes");
    if (!file || !file->write(700, payload) || !file->truncate(800))
        return Status::fault("FAT mounted write/truncate failed");
    auto bytes = file->read(0, 800);
    if (!bytes || bytes.value().size != 800)
        return Status::fault("FAT mounted read failed");
    for (usize i = 0; i < 800; ++i)
        if (bytes.value().data[i] != (i >= 700 && i < 700 + payload.size() ? payload[i - 700] : std::byte{}))
            return Status::fault("FAT mounted zero gap/data mismatch");
    if (!root->create("removed", fs::NodeType::regular) || !root->remove("removed", false) ||
        root->lookup("removed") || !volume.sync() || !disk.flushes)
        return Status::fault("FAT mounted unlink/sync failed");
    if (auto status = remounted.mount(disk); !status) return status;
    directory = remounted.root()->lookup("directory");
    file = directory ? directory->lookup("Long file.txt") : nullptr;
    if (!file || file->metadata().size != 800)
        return Status::fault("FAT mounted persisted namespace failed");
    bytes = file->read(700, payload.size());
    if (!bytes || bytes.value().size != payload.size())
        return Status::fault("FAT mounted persisted read failed");
    for (usize i = 0; i < payload.size(); ++i)
        if (bytes.value().data[i] != payload[i])
            return Status::fault("FAT mounted persisted data mismatch");
    if (!directory->remove("Long file.txt", false) || !remounted.root()->remove("directory", true) || !remounted.sync())
        return Status::fault("FAT mounted directory removal failed");
    return {};
}

// Byte/model coverage reached by the Debug kernel; this does not exercise a NIC.
Status test_network_wire_bytes()
{
    namespace wire = net::wire;
    constexpr std::array<u8, 46> fixture{
        0x45,0x00,0x00,0x2e,0x12,0x34,0x40,0x00,0x40,0x11,0x3c,0x54,
        0xc0,0x00,0x02,0x01,0xc6,0x33,0x64,0x02,0x30,0x39,0xd4,0x31,
        0x00,0x1a,0x73,0x45,0x4f,0x62,0x66,0x75,0x73,0x63,0x61,0x74,
        0x69,0x6f,0x6e,0x4f,0x53,0x00,0x75,0x64,0x70,0xff};
    std::array<std::byte, 46> packet{};
    for (usize i = 0; i < packet.size(); ++i) packet[i] = std::byte{fixture[i]};
    auto ip = wire::ipv4(packet);
    if (!ip || ip.value().source != wire::Ipv4{192,0,2,1} ||
        ip.value().destination != wire::Ipv4{198,51,100,2} ||
        ip.value().identification != 0x1234 || ip.value().ttl != 64 || !ip.value().dont_fragment)
        return Status::fault("network wire IPv4 reference bytes failed");
    auto udp = wire::udp(ip.value());
    if (!udp || udp.value().source_port != 12345 || udp.value().destination_port != 54321 ||
        !udp.value().checksum_present || udp.value().payload.size() != 18)
        return Status::fault("network wire UDP reference bytes failed");
    std::array<std::byte, 64> output{};
    auto built = wire::build_udp(output, ip.value().source, ip.value().destination,
                                12345, 54321, 0x1234, 64, udp.value().payload);
    if (!built || built.value() != packet.size()) return Status::fault("network wire UDP build length failed");
    for (usize i = 0; i < packet.size(); ++i)
        if (output[i] != packet[i]) return Status::fault("network wire UDP build differs from reference bytes");
    for (usize length = 0; length < packet.size(); ++length)
        if (wire::ipv4(wire::Bytes{packet.data(), length}))
            return Status::fault("network wire accepted a truncated IPv4 packet");
    auto damaged = packet;
    damaged[45] ^= std::byte{1};
    auto damaged_ip = wire::ipv4(damaged);
    if (!damaged_ip || wire::udp(damaged_ip.value())) return Status::fault("network wire accepted corrupt UDP data");
    damaged[26] = damaged[27] = std::byte{};
    damaged_ip = wire::ipv4(damaged);
    if (!damaged_ip || !wire::udp(damaged_ip.value()) || wire::udp(damaged_ip.value()).value().checksum_present)
        return Status::fault("network wire omitted IPv4 UDP checksum handling failed");
    damaged = packet;
    damaged[6] = std::byte{0x60};
    damaged[10] = damaged[11] = std::byte{};
    wire::be16(damaged, 10, wire::checksum(wire::Bytes{damaged.data(), 20}));
    if (wire::ipv4(damaged).status().code() != StatusCode::unsupported)
        return Status::fault("network wire fragment was not rejected explicitly");
    const auto before = output;
    if (wire::build_udp(output, ip.value().source, ip.value().destination, 1, 2, 3, 64,
        wire::Bytes{output.data() + 8, 4}).status().code() != StatusCode::invalid_argument || output != before)
        return Status::fault("network wire aliased build changed output");
    auto empty = wire::build_udp(output, ip.value().source, ip.value().destination, 0, 2, 0, 64, {});
    if (!empty || empty.value() != 28)
        return Status::fault("network wire empty UDP build failed");
    auto empty_ip = wire::ipv4(output);
    if (!empty_ip || !wire::udp(empty_ip.value()) || !wire::udp(empty_ip.value()).value().payload.empty())
        return Status::fault("network wire empty UDP parse failed");
    std::array<std::byte, 14> ethernet{};
    wire::be16(ethernet, 12, 0x8100);
    if (wire::ethernet(ethernet).status().code() != StatusCode::unsupported)
        return Status::fault("network wire VLAN policy failed");
    wire::be16(ethernet, 12, 0x0800);
    if (!wire::ethernet(ethernet)) return Status::fault("network wire Ethernet type parse failed");
    if (wire::arp(wire::Bytes{packet.data(), 27}).status().code() != StatusCode::invalid_argument)
        return Status::fault("network wire ARP truncation was not rejected");
    return Status::success();
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
    if (kernel.network().send_icmp_echo(net::Ipv4Address{{192, 0, 2, 1}}, 1, 1, as_bytes(ping)).status().code() !=
        StatusCode::unsupported)
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
    if (auto status =
            sockets.bind(udp.value(), net::UdpEndpoint{.address = kernel.network().local_address(), .port = 41000});
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
    if (sockets
            .sendto(tcp_client.value(), as_bytes(payload),
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

Status test_native_udp_description_owner(Kernel &kernel)
{
    // Keep the alternate service off the small kernel test stack. The binding
    // is restored before this test returns successfully.
    static net::NativeUdpService alternate_service;
    auto &owner = kernel.native_udp();
    auto &scheduler = kernel.scheduler();
    sched::ProcessResources resources{};
    if (owner.active_sockets() || alternate_service.active_sockets())
        return Status::fault("native UDP ownership test started with live sockets");
    if (auto status = scheduler.set_native_udp_service(owner); !status.ok())
        return status;

    auto handle = owner.open();
    if (!handle) return handle.status();
    auto description = scheduler.allocate_description(sched::OpenFileDescription{
        .readable = true, .writable = true, .native_udp_handle = handle.value()});
    if (!description) return description.status();
    if (scheduler.set_native_udp_service(alternate_service).code() != StatusCode::busy ||
        owner.active_sockets() != 1 || alternate_service.active_sockets() != 0)
        return Status::fault("zero-reference UDP reservation allowed service replacement");

    if (auto status = scheduler.install_descriptor(resources, 10, description.value()); !status.ok())
        return status;
    if (auto status = scheduler.install_descriptor(resources, 11, description.value()); !status.ok())
        return status;
    if (auto status = scheduler.close_descriptor(resources, 10); !status.ok())
        return status;
    if (scheduler.set_native_udp_service(alternate_service).code() != StatusCode::busy ||
        owner.active_sockets() != 1 || !scheduler.open_description(description.value()))
        return Status::fault("shared UDP description lost its endpoint owner too early");
    if (auto status = scheduler.close_descriptor(resources, 11); !status.ok())
        return status;
    if (owner.active_sockets() || scheduler.open_description(description.value()) ||
        owner.peek(handle.value()).status().code() != StatusCode::bad_descriptor)
        return Status::fault("last UDP description reference did not retire its endpoint");

    if (auto status = scheduler.set_native_udp_service(alternate_service); !status.ok())
        return status;
    auto alternate_handle = alternate_service.open();
    if (!alternate_handle) return alternate_handle.status();
    auto alternate_description = scheduler.allocate_description(sched::OpenFileDescription{
        .readable = true, .writable = true, .native_udp_handle = alternate_handle.value()});
    if (!alternate_description) return alternate_description.status();
    if (auto status = scheduler.install_descriptor(resources, 12, alternate_description.value()); !status.ok())
        return status;
    if (auto status = scheduler.close_descriptor(resources, 12); !status.ok())
        return status;
    if (alternate_service.active_sockets() ||
        alternate_service.peek(alternate_handle.value()).status().code() != StatusCode::bad_descriptor)
        return Status::fault("rebound UDP service did not retire its own endpoint");
    return scheduler.set_native_udp_service(owner);
}

// Reached Debug association model; it executes no user syscall or NIC path.
Status test_native_udp_association_model()
{
    static net::NativeUdpService service;
    constexpr net::NativeUdpLink link{{2,1,2,3,4,5},{2,6,7,8,9,10},{192,0,2,2},{192,0,2,1}};
    if (auto status = service.configure(link); !status) return status;
    const auto opened = service.open();
    if (!opened) return opened.status();
    const auto endpoint = opened.value();
    if (auto status = service.bind_ephemeral(endpoint, link.local_ip); !status) return status;
    if (auto status = service.connect(endpoint, link.peer_ip, 0); !status) return status;
    const auto peer = service.peer(endpoint);
    const auto binding = service.binding(endpoint);
    if (!peer || !binding || !peer.value().associated || peer.value().port ||
        peer.value().address != link.peer_ip || !binding.value().bound || binding.value().address != link.local_ip)
        return Status::fault("native UDP zero-port association model failed");
    if (auto status = service.connect(endpoint, link.peer_ip, 41000); !status) return status;
    if (service.binding(endpoint).value() != binding.value())
        return Status::fault("native UDP reconnect changed the selected binding");
    if (auto status = service.disconnect(endpoint); !status) return status;
    const auto disconnected = service.binding(endpoint);
    if (!disconnected || disconnected.value().bound || disconnected.value().port ||
        disconnected.value().address != link.local_ip || service.peer(endpoint).value().associated)
        return Status::fault("native UDP disconnect binding provenance model failed");
    if (auto status = service.autobind(endpoint); !status) return status;
    if (service.binding(endpoint).value().address != link.local_ip)
        return Status::fault("native UDP autobind discarded the retained explicit address");
    if (auto status = service.close(endpoint); !status) return status;
    return service.peer(endpoint).status().code() == StatusCode::bad_descriptor
        ? Status{} : Status::fault("native UDP association stale handle model failed");
}

Status test_storage(Kernel &kernel)
{
    if (auto status = test::test_cache_failure(); !status) return status;
    if (kernel.disk_name() != "virtio-blk0")
    {
        return Status::fault("QEMU virtio block disk was not selected for driver storage tests");
    }

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
    const auto driver_stats_before = kernel.disk().io_stats();
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
    const auto cache_stats = cache.stats();
    if (cache_stats.misses != 1 || cache_stats.hits != 1 || cache_stats.read_requests != 2 ||
        cache_stats.write_requests != 1 || cache_stats.device_reads != 1 || cache_stats.device_writes != 1 ||
        cache_stats.bytes_read != driver::block_sector_size * 2 ||
        cache_stats.bytes_written != driver::block_sector_size || read_block[0] != std::byte{0x5a})
    {
        return Status::fault("block cache hit/miss or IO accounting validation failed");
    }
    if (cache.read_block(kernel.disk().geometry().block_count, read_block).code() != StatusCode::invalid_argument ||
        cache.write_block(kernel.disk().geometry().block_count, block).code() != StatusCode::invalid_argument)
    {
        return Status::fault("out-of-range block access was not rejected");
    }
    const auto driver_stats_after = kernel.disk().io_stats();
    if (driver_stats_after.read_operations <= driver_stats_before.read_operations ||
        driver_stats_after.write_operations <= driver_stats_before.write_operations ||
        driver_stats_after.bytes_read <= driver_stats_before.bytes_read ||
        driver_stats_after.bytes_written <= driver_stats_before.bytes_written)
    {
        return Status::fault("block driver IO usage counters did not advance");
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
        info.value().volume_name.view() != "OKEXT4" || !info.value().has_extents)
    {
        return Status::fault("EXT4 superblock validation failed");
    }
    std::array<std::byte, 1024> ext4_block{};
    if (auto status = volume.read_block(1, ext4_block); !status.ok())
    {
        return status;
    }
    if (ext4_block[0] != std::byte{0x7e} || volume.read_block(4, ext4_block).code() != StatusCode::invalid_argument)
    {
        return Status::fault("EXT4 read-only block validation failed");
    }
    if (volume.read_block(u64{1} << 63, ext4_block).code() != StatusCode::invalid_argument ||
        volume.read_block(~u64{0}, ext4_block).code() != StatusCode::invalid_argument ||
        ext4_block[0] != std::byte{0x7e})
    {
        return Status::fault("EXT4 overflowing block read was not rejected without output mutation");
    }
    auto corrupt_ext4_image = make_ext4_test_image(false);
    fs::Ext4Volume corrupt;
    if (corrupt.mount(corrupt_ext4_image).code() != StatusCode::invalid_argument)
    {
        return Status::fault("corrupted EXT4 superblock was not rejected");
    }
    write_le16(corrupt_ext4_image, fs::ext4_superblock_offset + 0x38, fs::ext4_superblock_magic);
    write_le32(corrupt_ext4_image, fs::ext4_superblock_offset + 0x04, 5);
    if (volume.mount(corrupt_ext4_image).code() != StatusCode::invalid_argument ||
        !volume.info() || volume.info().value().block_count_low != 4 ||
        !volume.read_block(1, ext4_block) || ext4_block[0] != std::byte{0x7e})
    {
        return Status::fault("EXT4 failed rebind changed the admitted filesystem");
    }

    auto fat32_image = make_fat32_test_image(true);
    FatBootFixtureDevice fat32_device{fat32_image, 65536};
    fs::FatVolume fat32;
    if (auto status = fat32.mount(fat32_device); !status.ok())
    {
        return status;
    }
    auto fat32_info = fat32.info();
    if (!fat32_info || fat32_info.value().variant != fs::FatVariant::fat32 ||
        fat32_info.value().bytes_per_sector != 512 || fat32_info.value().sectors_per_cluster != 8 ||
        fat32_info.value().fat_count != 2 || fat32_info.value().root_cluster != 2 ||
        fat32_info.value().volume_label.view() != "OKFAT32")
    {
        return Status::fault("FAT32 boot sector validation failed");
    }
    std::array<std::byte, fs::fat_boot_sector_size> fat_sector{};
    if (auto status = fat32.read_sector(0, fat_sector); !status.ok())
    {
        return status;
    }
    if (fat_sector[0] != std::byte{0xeb})
    {
        return Status::fault("FAT32 sector read validation failed");
    }

    auto exfat_image = make_exfat_test_image();
    FatBootFixtureDevice exfat_device{exfat_image, 131072};
    fs::FatVolume exfat;
    if (auto status = exfat.mount(exfat_device); !status.ok())
    {
        return status;
    }
    auto exfat_info = exfat.info();
    if (!exfat_info || exfat_info.value().variant != fs::FatVariant::exfat ||
        exfat_info.value().bytes_per_sector != 512 || exfat_info.value().sectors_per_cluster != 8 ||
        exfat_info.value().fat_count != 1 || exfat_info.value().cluster_count != 1024 ||
        exfat_info.value().root_cluster != 2)
    {
        return Status::fault("exFAT boot sector validation failed");
    }
    auto corrupt_fat32_image = make_fat32_test_image(false);
    fs::FatVolume corrupt_fat;
    if (corrupt_fat.mount(corrupt_fat32_image).code() != StatusCode::invalid_argument)
    {
        return Status::fault("corrupted FAT boot sector was not rejected");
    }

    return Status::success();
}

} // namespace

Status run_network_storage_roadmap_tests(Kernel &kernel, KernelTestReport &report)
{
    if (auto status = test_network_wire_bytes(); !status.ok())
    {
        return status;
    }
    if (auto status = test_network_device_and_protocols(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = test_socket_table(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = test_native_udp_description_owner(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = test_native_udp_association_model(); !status.ok())
    {
        return status;
    }
    if (auto status = test_storage(kernel); !status.ok())
    {
        return status;
    }

    if (auto status = test_fat_mounted_block_model(); !status.ok())
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
