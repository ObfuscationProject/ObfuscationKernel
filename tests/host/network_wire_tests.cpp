#include <ok/net/wire.hpp>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace ok;
namespace wire = ok::net::wire;

namespace
{
usize checks{};
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    std::fprintf(stderr, "NETWORK_WIRE_FAIL line=%d check=%zu: %s\n", __LINE__, checks, #condition); \
    std::abort(); } } while (false)

std::vector<std::byte> decode(const char *hex)
{
    auto digit = [](char value) -> u8 {
        if (value >= '0' && value <= '9') return static_cast<u8>(value - '0');
        if (value >= 'a' && value <= 'f') return static_cast<u8>(value - 'a' + 10);
        CHECK(false); return 0;
    };
    std::vector<std::byte> out;
    for (usize i = 0; hex[i]; i += 2)
    {
        CHECK(hex[i + 1]);
        out.push_back(std::byte{static_cast<u8>((digit(hex[i]) << 4) | digit(hex[i + 1]))});
    }
    return out;
}

u16 independent_checksum(wire::Bytes bytes, u32 initial = 0)
{
    std::uint64_t sum = initial;
    for (usize i = 0; i < bytes.size(); i += 2)
    {
        sum += static_cast<std::uint64_t>(std::to_integer<u8>(bytes[i])) << 8;
        if (i + 1 < bytes.size()) sum += std::to_integer<u8>(bytes[i + 1]);
    }
    while (sum > 0xffff) sum = (sum & 0xffff) + (sum >> 16);
    return static_cast<u16>(~sum);
}

bool independent_udp_valid(wire::Bytes packet)
{
    const usize udp_length = packet.size() - 20;
    std::vector<std::byte> pseudo(12 + udp_length);
    for (usize i = 0; i < 4; ++i) { pseudo[i] = packet[12 + i]; pseudo[4 + i] = packet[16 + i]; }
    pseudo[9] = std::byte{17};
    pseudo[10] = std::byte{static_cast<u8>(udp_length >> 8)};
    pseudo[11] = std::byte{static_cast<u8>(udp_length)};
    for (usize i = 0; i < udp_length; ++i) pseudo[12 + i] = packet[20 + i];
    return independent_checksum(pseudo) == 0;
}

void repair_ipv4_checksum(std::span<std::byte> packet, usize header = 20)
{
    packet[10] = packet[11] = std::byte{};
    wire::be16(packet, 10, independent_checksum({packet.data(), header}));
}

void exact_fixture()
{
    // Generated separately from the parser/builder: IPv4 + UDP + odd payload.
    const auto golden = decode("4500002e1234400040113c54c0000201c63364023039d431001a7345"
                               "4f62667573636174696f6e4f5300756470ff");
    const auto payload = decode("4f62667573636174696f6e4f5300756470ff");
    CHECK(golden.size() == 46 && payload.size() == 18);
    CHECK(wire::checksum({golden.data(), 20}) == 0);
    CHECK(independent_checksum({golden.data(), 20}) == 0);
    CHECK(independent_udp_valid(golden));
    const auto rfc1071 = decode("0001f203f4f5f6f7");
    CHECK(wire::checksum(rfc1071) == 0x220d);
    CHECK(independent_checksum(rfc1071) == 0x220d);
    const auto full_words = decode("ffff");
    // A caller may supply an uncollapsed 32-bit partial sum. Folding must not
    // lose its carry before the next word is added.
    CHECK(wire::sum(full_words, 0xffffffffu) == 0xffffu);
    for (u32 initial : {0u, 0xffffu, 0x10000u, 0xffffffffu})
        for (usize length = 0; length <= rfc1071.size(); ++length)
        {
            const auto prefix = wire::Bytes{rfc1071.data(), length};
            CHECK(wire::sum(prefix, initial) == static_cast<u16>(~independent_checksum(prefix, initial)));
        }
    const wire::Ipv4 source{192, 0, 2, 1}, destination{198, 51, 100, 2};
    std::array<std::byte, 64> output{}; output.fill(std::byte{0xa5});
    const auto built = wire::build_udp(output, source, destination, 12345, 54321, 0x1234, 64, payload);
    CHECK(built && built.value() == golden.size());
    CHECK(std::memcmp(output.data(), golden.data(), golden.size()) == 0);
    for (usize i = golden.size(); i < output.size(); ++i) CHECK(output[i] == std::byte{0xa5});
    const auto ip = wire::ipv4({output.data(), built.value()});
    CHECK(ip);
    CHECK(ip.value().source == source && ip.value().destination == destination);
    CHECK(ip.value().traffic_class == 0 && ip.value().ttl == 64 && ip.value().protocol == 17);
    CHECK(ip.value().identification == 0x1234 && ip.value().dont_fragment);
    CHECK(ip.value().payload.data() == output.data() + 20 && ip.value().payload.size() == 26);
    const auto udp = wire::udp(ip.value());
    CHECK(udp && udp.value().source_port == 12345 && udp.value().destination_port == 54321);
    CHECK(udp.value().checksum_present && udp.value().payload.size() == payload.size());
    CHECK(std::memcmp(udp.value().payload.data(), payload.data(), payload.size()) == 0);

    auto zero = golden;
    zero[26] = zero[27] = std::byte{};
    const auto zero_ip = wire::ipv4(zero);
    CHECK(zero_ip);
    const auto zero_udp = wire::udp(zero_ip.value());
    CHECK(zero_udp && !zero_udp.value().checksum_present);
    CHECK(zero_udp.value().payload.size() == payload.size());
}

void length_and_mutation_rejections()
{
    const auto golden = decode("4500002e1234400040113c54c0000201c63364023039d431001a7345"
                               "4f62667573636174696f6e4f5300756470ff");
    for (usize length = 0; length < golden.size(); ++length)
        CHECK(!wire::ipv4({golden.data(), length}));
    const auto parsed = wire::ipv4(golden); CHECK(parsed);
    for (usize length = 0; length < parsed.value().payload.size(); ++length)
    {
        auto short_ip = parsed.value(); short_ip.payload = short_ip.payload.first(length);
        CHECK(!wire::udp(short_ip));
    }
    for (usize at = 0; at < 20; ++at)
        for (u8 bit = 0; bit < 8; ++bit)
        {
            auto changed = golden;
            changed[at] ^= std::byte{static_cast<u8>(1u << bit)};
            CHECK(!wire::ipv4(changed));
        }
    for (usize at = 20; at < golden.size(); ++at)
        for (u8 bit = 0; bit < 8; ++bit)
        {
            auto changed = golden;
            changed[at] ^= std::byte{static_cast<u8>(1u << bit)};
            auto ip = wire::ipv4(changed); CHECK(ip);
            CHECK(!wire::udp(ip.value()));
        }
    for (usize at = 0; at < 4; ++at)
        for (u8 bit = 0; bit < 8; ++bit)
        {
            auto source_changed = parsed.value();
            source_changed.source[at] ^= static_cast<u8>(1u << bit);
            CHECK(!wire::udp(source_changed));
            auto destination_changed = parsed.value();
            destination_changed.destination[at] ^= static_cast<u8>(1u << bit);
            CHECK(!wire::udp(destination_changed));
        }

    auto bad = golden;
    bad[0] = std::byte{0x65}; CHECK(!wire::ipv4(bad)); // IPv6 version.
    bad = golden; bad[0] = std::byte{0x44}; CHECK(!wire::ipv4(bad)); // IHL < 5.
    bad = golden; wire::be16(bad, 2, 19); CHECK(!wire::ipv4(bad));
    bad = golden; wire::be16(bad, 2, 47); CHECK(!wire::ipv4(bad));
    bad = golden; wire::be16(bad, 6, 0x8000); repair_ipv4_checksum(bad);
    CHECK(!wire::ipv4(bad) && wire::ipv4(bad).status().code() == StatusCode::invalid_argument);
    for (u16 fragment : {u16{0x2000}, u16{1}})
    {
        bad = golden; wire::be16(bad, 6, fragment); repair_ipv4_checksum(bad);
        CHECK(!wire::ipv4(bad) && wire::ipv4(bad).status().code() == StatusCode::unsupported);
    }
    bad.assign(50, std::byte{});
    std::memcpy(bad.data(), golden.data(), 20);
    std::memcpy(bad.data() + 24, golden.data() + 20, 26);
    bad[0] = std::byte{0x46}; wire::be16(bad, 2, 50); repair_ipv4_checksum(bad, 24);
    CHECK(!wire::ipv4(bad) && wire::ipv4(bad).status().code() == StatusCode::unsupported);
    bad = golden; bad[9] = std::byte{6}; repair_ipv4_checksum(bad);
    const auto tcp = wire::ipv4(bad); CHECK(tcp);
    CHECK(!wire::udp(tcp.value()) && wire::udp(tcp.value()).status().code() == StatusCode::unsupported);
    bad = golden; wire::be16(bad, 24, 7);
    auto ip = wire::ipv4(bad); CHECK(ip); CHECK(!wire::udp(ip.value()));
    bad = golden; wire::be16(bad, 24, 27);
    ip = wire::ipv4(bad); CHECK(ip); CHECK(!wire::udp(ip.value()));
    bad = golden; wire::be16(bad, 22, 0);
    ip = wire::ipv4(bad); CHECK(ip); CHECK(!wire::udp(ip.value()));
    // A shorter UDP length is intentionally legal; surplus IP data is ignored.
    bad = golden; wire::be16(bad, 24, 8); wire::be16(bad, 26, 0);
    ip = wire::ipv4(bad); CHECK(ip);
    const auto short_udp = wire::udp(ip.value());
    CHECK(short_udp && short_udp.value().payload.empty() && !short_udp.value().checksum_present);
}

void builder_edges()
{
    const wire::Ipv4 source{1, 2, 3, 4}, destination{5, 6, 7, 8};
    for (usize size = 0; size < 64; ++size)
    {
        std::vector<std::byte> payload(size);
        for (usize i = 0; i < size; ++i) payload[i] = std::byte{static_cast<u8>(i * 17 + 3)};
        std::vector<std::byte> output(28 + size + 2, std::byte{0xac});
        auto built = wire::build_udp(output, source, destination, 0, 7, 42, 1, payload);
        CHECK(built && built.value() == 28 + size);
        CHECK(output[built.value()] == std::byte{0xac});
        auto ip = wire::ipv4({output.data(), built.value()}); CHECK(ip);
        auto udp = wire::udp(ip.value()); CHECK(udp);
        CHECK(udp.value().source_port == 0 && udp.value().destination_port == 7);
        CHECK(udp.value().payload.size() == size);
        CHECK(size == 0 || std::memcmp(udp.value().payload.data(), payload.data(), size) == 0);
        CHECK(wire::checksum({output.data(), 20}) == 0);
        CHECK(independent_checksum({output.data(), 20}) == 0);
        CHECK(wire::checksum(payload) == independent_checksum(payload));
        CHECK(independent_udp_valid({output.data(), built.value()}));
    }
    std::array<std::byte, 128> buffer{}; buffer.fill(std::byte{0xa5});
    const auto original = buffer;
    for (usize size : {usize{0}, usize{1}, usize{3}, usize{99}})
    {
        std::vector<std::byte> data(size, std::byte{0x5a});
        buffer = original;
        auto built = wire::build_udp(buffer, source, destination, 1, 0, 1, 64, data);
        CHECK(built && built.value() == 28 + size);
        CHECK(buffer[22] == std::byte{} && buffer[23] == std::byte{});
        CHECK(independent_checksum({buffer.data(), 20}) == 0);
        CHECK(independent_udp_valid({buffer.data(), built.value()}));
        CHECK(size == 0 || std::memcmp(buffer.data() + 28, data.data(), size) == 0);
        for (usize i = built.value(); i < buffer.size(); ++i) CHECK(buffer[i] == original[i]);
        auto ip = wire::ipv4({buffer.data(), built.value()}); CHECK(ip);
        CHECK(wire::udp(ip.value()).status().code() == StatusCode::invalid_argument);
    }
    buffer = original;
    auto failed = wire::build_udp(buffer, source, destination, 1, 2, 1, 0, {});
    CHECK(!failed && failed.status().code() == StatusCode::invalid_argument && buffer == original);
    failed = wire::build_udp({buffer.data(), 27}, source, destination, 1, 2, 1, 64, {});
    CHECK(!failed && failed.status().code() == StatusCode::overflow && buffer == original);
    std::vector<std::byte> too_large(65508, std::byte{0x55});
    failed = wire::build_udp(buffer, source, destination, 1, 2, 1, 64, too_large);
    CHECK(!failed && failed.status().code() == StatusCode::overflow && buffer == original);
    failed = wire::build_udp({buffer.data() + 10, 50}, source, destination, 1, 2, 1, 64,
                             {buffer.data() + 20, 10});
    CHECK(!failed && failed.status().code() == StatusCode::invalid_argument && buffer == original);
    failed = wire::build_udp({buffer.data() + 10, 50}, source, destination, 1, 2, 1, 64,
                             {buffer.data() + 5, 10});
    CHECK(!failed && failed.status().code() == StatusCode::invalid_argument && buffer == original);
    failed = wire::build_udp({buffer.data() + 10, 50}, source, destination, 1, 2, 1, 64,
                             {buffer.data() + 47, 10});
    CHECK(!failed && failed.status().code() == StatusCode::invalid_argument && buffer == original);
    // A payload that ends exactly where output begins is disjoint.
    auto adjacent = wire::build_udp({buffer.data() + 10, 50}, source, destination, 1, 2, 1, 64,
                                    {buffer.data(), 10});
    CHECK(adjacent && adjacent.value() == 38);

    // Find the 16-bit payload whose uncomplemented UDP sum is 0xffff.
    std::array<std::byte, 2> pair{};
    std::array<std::byte, 30> packet{};
    bool found = false;
    for (u32 candidate = 0; candidate <= 0xffff; ++candidate)
    {
        pair[0] = std::byte{static_cast<u8>(candidate >> 8)};
        pair[1] = std::byte{static_cast<u8>(candidate)};
        auto built = wire::build_udp(packet, source, destination, 1, 2, 1, 64, pair);
        CHECK(built && built.value() == packet.size());
        if (wire::be16(packet, 26) != 0xffff) continue;
        auto ip = wire::ipv4(packet); CHECK(ip);
        auto udp = wire::udp(ip.value()); CHECK(udp && udp.value().checksum_present);
        CHECK(udp.value().payload.size() == 2 && udp.value().payload[0] == pair[0] && udp.value().payload[1] == pair[1]);
        found = true; break;
    }
    CHECK(found);
}

void ethernet_arp_edges()
{
    std::array<std::byte, 42> frame{};
    for (usize i = 0; i < 6; ++i) { frame[i] = std::byte{0xff}; frame[i + 6] = std::byte{static_cast<u8>(i + 1)}; }
    wire::be16(frame, 12, 0x0806);
    wire::be16(frame, 14, 1); wire::be16(frame, 16, 0x0800);
    frame[18] = std::byte{6}; frame[19] = std::byte{4}; wire::be16(frame, 20, 1);
    for (usize i = 0; i < 6; ++i) { frame[22 + i] = std::byte{static_cast<u8>(i + 1)}; frame[32 + i] = std::byte{static_cast<u8>(i + 11)}; }
    for (usize i = 0; i < 4; ++i) { frame[28 + i] = std::byte{static_cast<u8>(i + 21)}; frame[38 + i] = std::byte{static_cast<u8>(i + 31)}; }
    for (usize length = 0; length < 14; ++length) CHECK(!wire::ethernet({frame.data(), length}));
    auto ether = wire::ethernet(frame); CHECK(ether && ether.value().type == 0x0806);
    CHECK(ether.value().payload.data() == frame.data() + 14 && ether.value().payload.size() == 28);
    auto arp = wire::arp(ether.value().payload); CHECK(arp && arp.value().operation == 1);
    CHECK(arp.value().sender_mac == wire::Mac({1, 2, 3, 4, 5, 6}));
    CHECK(arp.value().target_ip == wire::Ipv4({31, 32, 33, 34}));
    for (usize length = 0; length < 28; ++length) CHECK(!wire::arp({frame.data() + 14, length}));
    for (u16 type : {u16{0x05ff}, u16{0x8100}, u16{0x88a8}})
    {
        auto bad = frame; wire::be16(bad, 12, type);
        CHECK(!wire::ethernet(bad) && wire::ethernet(bad).status().code() == StatusCode::unsupported);
    }
    for (usize at : {usize{14}, usize{16}, usize{18}, usize{19}, usize{20}})
    {
        auto bad = frame; bad[at] ^= std::byte{1};
        CHECK(!wire::arp({bad.data() + 14, 28}));
    }
    auto reply = frame; wire::be16(reply, 20, 2);
    CHECK(wire::arp({reply.data() + 14, 28}).value().operation == 2);
}

u32 random_state = 0x91e10da5;
u32 next_random()
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}
void deterministic_sanitizer_fuzz()
{
    std::array<std::byte, 300> data{}, packet{};
    for (usize iteration = 0; iteration < 10000; ++iteration)
    {
        const usize random_length = next_random() % data.size();
        for (usize i = 0; i < random_length; ++i) data[i] = std::byte{static_cast<u8>(next_random())};
        auto bytes = wire::Bytes{data.data(), random_length};
        static_cast<void>(wire::ethernet(bytes));
        static_cast<void>(wire::arp(bytes));
        auto random_ip = wire::ipv4(bytes);
        if (random_ip) static_cast<void>(wire::udp(random_ip.value()));
        const usize payload_length = next_random() % 271;
        wire::Ipv4 source{}, destination{};
        for (auto &value : source) value = static_cast<u8>(next_random());
        for (auto &value : destination) value = static_cast<u8>(next_random());
        const u16 destination_port = static_cast<u16>((next_random() % 65535) + 1);
        const auto built = wire::build_udp(packet, source, destination, static_cast<u16>(next_random()),
                                           destination_port, static_cast<u16>(next_random()),
                                           static_cast<u8>((next_random() % 255) + 1),
                                           {data.data(), payload_length});
        CHECK(built && built.value() == 28 + payload_length);
        const auto parsed = wire::ipv4({packet.data(), built.value()}); CHECK(parsed);
        const auto udp = wire::udp(parsed.value()); CHECK(udp && udp.value().payload.size() == payload_length);
        CHECK(std::memcmp(udp.value().payload.data(), data.data(), payload_length) == 0);
        CHECK(independent_udp_valid({packet.data(), built.value()}));
        const usize changed = next_random() % built.value();
        packet[changed] ^= std::byte{static_cast<u8>(1u << (next_random() & 7))};
        const auto mutated = wire::ipv4({packet.data(), built.value()});
        if (mutated) static_cast<void>(wire::udp(mutated.value()));
    }
}
}

int main()
{
    exact_fixture();
    length_and_mutation_rejections();
    builder_edges();
    ethernet_arp_edges();
    deterministic_sanitizer_fuzz();
    std::printf("NETWORK_WIRE_PASS scope=hosted-model checks=%zu fuzz-seed=91e10da5 iterations=10000\n", checks);
}
