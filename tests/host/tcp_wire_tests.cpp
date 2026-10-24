#include <ok/net/tcp_wire.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include "tcp_wire_vectors.hpp"

using namespace ok;
using namespace ok::net::wire;
static unsigned checks{};

static void check(bool condition, const char *label)
{
    ++checks;
    if (!condition) { std::fprintf(stderr, "TCP_WIRE_FAIL %s\n", label); std::exit(1); }
}
static bool equal(Bytes left, Bytes right)
{
    if (left.size() != right.size()) return false;
    for (usize i = 0; i < left.size(); ++i) if (left[i] != right[i]) return false;
    return true;
}
static bool fields_equal(TcpFields a, TcpFields b)
{
    return a.source_port == b.source_port && a.destination_port == b.destination_port &&
        a.sequence == b.sequence && a.acknowledgment == b.acknowledgment && a.flags == b.flags &&
        a.window == b.window && a.urgent_pointer == b.urgent_pointer;
}

// Independent byte accumulator, used to keep malformed structural controls
// checksum-valid. It calls no wire sum/checksum/pseudo-header implementation.
static void repair_tcp(std::span<std::byte> segment, Ipv4 source, Ipv4 destination)
{
    segment[16] = segment[17] = std::byte{};
    u64 value = 6 + segment.size();
    for (usize i = 0; i < 4; i += 2)
        value += source[i] * 256u + source[i + 1] + destination[i] * 256u + destination[i + 1];
    for (usize i = 0; i < segment.size(); i += 2)
        value += std::to_integer<unsigned>(segment[i]) * 256u +
            (i + 1 < segment.size() ? std::to_integer<unsigned>(segment[i + 1]) : 0u);
    while (value >> 16) value = (value & 65535u) + (value >> 16);
    const auto checksum_value = static_cast<u16>(~value);
    segment[16] = std::byte{static_cast<u8>(checksum_value >> 8)};
    segment[17] = std::byte{static_cast<u8>(checksum_value)};
}
static Ipv4Packet view(Bytes segment)
{
    Ipv4Packet result{};
    result.source = tcp_vectors::v0.source;
    result.destination = tcp_vectors::v0.destination;
    result.protocol = 6;
    result.payload = segment;
    return result;
}

static void goldens()
{
    for (const auto &golden : tcp_vectors::all)
    {
        auto ip = ipv4(golden.packet);
        check(ip.ok(), "independent golden IPv4");
        auto parsed = tcp(ip.value());
        check(parsed.ok(), "independent golden TCP");
        check(fields_equal(parsed.value().fields, golden.fields), "all scalar fields round-trip golden");
        check(equal(parsed.value().payload, golden.payload), "exact golden payload");
        check(parsed.value().payload.data() == golden.packet.data() + golden.packet.size() - golden.payload.size(),
            "borrowed payload pointer");
        check(parsed.value().options.data() == golden.packet.data() + 40, "borrowed option pointer");
        std::array<std::byte, 256> output{}; output.fill(std::byte{0xa5});
        auto built = build_tcp(output, golden.source, golden.destination, golden.identification,
            golden.ttl, golden.fields, golden.options, golden.payload);
        check(built.ok() && built.value() == golden.packet.size(), "golden builder length");
        check(equal(Bytes{output.data(), built.value()}, golden.packet), "independent golden exact packet bytes");
        for (usize i = built.value(); i < output.size(); ++i)
            check(output[i] == std::byte{0xa5}, "unused output tail unchanged");
        for (usize length = 0; length < 20; ++length)
        {
            auto truncated = ip.value(); truncated.payload = ip.value().payload.first(length);
            check(!tcp(truncated), "every fixed TCP header truncation");
        }
        auto altered_source = ip.value(); altered_source.source[3] ^= 1;
        check(!tcp(altered_source), "pseudo-header source bound");
        auto altered_destination = ip.value(); altered_destination.destination[3] ^= 1;
        check(!tcp(altered_destination), "pseudo-header destination bound");
        auto wrong_protocol = ip.value(); wrong_protocol.protocol = 17;
        check(!tcp(wrong_protocol), "TCP protocol required");
        std::array<std::byte, 256> corrupted{};
        for (usize i = 0; i < ip.value().payload.size(); ++i) corrupted[i] = ip.value().payload[i];
        corrupted[ip.value().payload.size() - 1] ^= std::byte{1};
        check(!tcp(view(Bytes{corrupted.data(), ip.value().payload.size()})), "checksum covers final odd or even byte");
    }
    check(be16(tcp_vectors::v5.packet, 36) == 0, "independent valid TCP zero checksum fixture");
}

static void header_and_options()
{
    std::array<std::byte, 24> packet{};
    for (usize i = 0; i < 20; ++i) packet[i] = tcp_vectors::v0.packet[20 + i];
    for (u8 offset : {u8{0}, u8{4}, u8{7}, u8{15}})
    {
        packet[12] = std::byte{static_cast<u8>(offset << 4)};
        repair_tcp(packet, tcp_vectors::v0.source, tcp_vectors::v0.destination);
        check(!tcp(view(packet)), "checksum-valid invalid data offset");
    }
    packet[12] = std::byte{0x5f};
    repair_tcp(packet, tcp_vectors::v0.source, tcp_vectors::v0.destination);
    check(tcp(view(packet)).ok(), "received reserved bits ignored");

    constexpr std::array<std::array<u8, 4>, 7> bad{{
        {{2,3,5,0}}, {{253,0,0,0}}, {{253,1,0,0}}, {{253,5,0xaa,0xbb}},
        {{1,1,1,253}}, {{0,1,0,0}}, {{2,4,5,0xb4}}
    }};
    for (const auto &option : bad)
    {
        packet[12] = std::byte{0x60}; packet[13] = std::byte{tcp_ack};
        for (usize i = 0; i < 4; ++i) packet[20 + i] = std::byte{option[i]};
        repair_tcp(packet, tcp_vectors::v0.source, tcp_vectors::v0.destination);
        check(!tcp(view(packet)), "checksum-valid malformed option or non-SYN MSS refused");
        std::array<std::byte, 96> output{}; output.fill(std::byte{0xa5}); const auto before = output;
        TcpFields fields{}; fields.flags = tcp_ack;
        check(!build_tcp(output, {}, {}, 0, 64, fields, Bytes{packet.data()+20,4}, {}),
            "builder malformed option refused");
        check(equal(output, before), "invalid option preserves entire builder output");
    }
    std::array<std::byte, 4> opaque{std::byte{3},std::byte{2},std::byte{},std::byte{}};
    for (unsigned kind = 3; kind <= 255; ++kind)
    {
        opaque[0] = std::byte{static_cast<u8>(kind)};
        TcpOptions cursor{opaque, tcp_ack};
        auto item = cursor.next();
        check(item.ok() && item.value() && item.value()->kind == kind && item.value()->data.empty(),
            "every unknown TLV kind retained");
        check(item.value()->bytes.data() == opaque.data() && item.value()->bytes.size() == 2,
            "opaque option borrowed without reinterpretation");
        auto eol = cursor.next(); check(eol.ok() && eol.value() && eol.value()->kind == 0, "EOL returned once");
        auto end = cursor.next(); check(end.ok() && !end.value(), "EOL terminates walk");
        end = cursor.next(); check(end.ok() && !end.value(), "ended cursor remains ended");
        packet[12] = std::byte{0x60}; packet[13] = std::byte{tcp_ack};
        for (usize i = 0; i < 4; ++i) packet[20+i] = opaque[i];
        repair_tcp(packet, tcp_vectors::v0.source, tcp_vectors::v0.destination);
        check(tcp(view(packet)).ok(), "unknown option in received non-SYN segment");
    }
    TcpOptions unaligned{tcp_vectors::v2.options, tcp_syn};
    const std::array<u8,3> kinds{1,2,253};
    for (u8 kind : kinds)
    {
        auto item = unaligned.next(); check(item.ok() && item.value() && item.value()->kind == kind, "unaligned option walk");
        if (kind == 2) check(be16(item.value()->data,0) == 1460, "MSS network-order value");
        if (kind == 253) check(item.value()->data.size() == 1 && byte(item.value()->data,0) == 0xaa, "opaque body retained");
    }
    auto end = unaligned.next(); check(end.ok() && !end.value(), "option area boundary terminates walk");
    const std::array<std::byte,8> duplicates{std::byte{2},std::byte{4},std::byte{},std::byte{},
        std::byte{2},std::byte{4},std::byte{5},std::byte{0xb4}};
    TcpOptions repeated{duplicates,tcp_syn};
    for (unsigned i=0;i<2;++i) { auto item=repeated.next(); check(item.ok() && item.value(), "duplicate MSS wire records preserved"); }
    std::array<std::byte,41> oversized{}; oversized.fill(std::byte{1});
    TcpOptions too_long{oversized,tcp_syn}; check(!too_long.next(), "option area exceeds forty bytes");
}

static void boundaries()
{
    const auto &golden=tcp_vectors::v0;
    std::array<std::byte,128> output{}; output.fill(std::byte{0xa5}); const auto saved=output;
    check(!build_tcp(output,golden.source,golden.destination,0,0,golden.fields,{},{}), "zero TTL invalid");
    check(equal(output,saved), "zero TTL preserves output");
    for (usize capacity=0;capacity<golden.packet.size();++capacity)
    {
        check(!build_tcp(std::span<std::byte>{output}.first(capacity),golden.source,golden.destination,
            0,64,golden.fields,{},{}), "every short output capacity refused");
        check(equal(output,saved), "capacity failure preserves entire backing buffer");
    }
    std::array<std::byte,65536> large_output{}, large_input{};
    large_output.fill(std::byte{0xa5}); large_input.fill(std::byte{0x71});
    const auto payload=Bytes{large_input.data(),65495};
    auto full=build_tcp(large_output,golden.source,golden.destination,0,64,golden.fields,{},payload);
    check(full.ok() && full.value()==65535, "maximum IPv4 packet builds");
    auto ip=ipv4(Bytes{large_output.data(),65535}); check(ip.ok() && tcp(ip.value()).ok(), "maximum TCP segment parses");
    const auto before=large_output;
    check(!build_tcp(large_output,golden.source,golden.destination,0,64,golden.fields,{},
        Bytes{large_input.data(),65496}), "one-byte IPv4 excess refused");
    check(equal(large_output,before), "oversize payload preserves entire output");
    auto too_big=view(Bytes{large_input.data(),65516}); check(!tcp(too_big), "one-byte TCP segment excess refused");
    std::array<std::byte,40> options{}; options.fill(std::byte{1});
    full=build_tcp(large_output,golden.source,golden.destination,0,64,golden.fields,options,
        Bytes{large_input.data(),65455});
    check(full.ok() && full.value()==65535, "maximum option header at IPv4 limit");
    ip=ipv4(Bytes{large_output.data(),65535}); check(ip.ok() && tcp(ip.value()).ok(), "sixty-byte TCP header parses");
    for (usize length=20;length<60;++length)
    {
        auto cut=ip.value(); cut.payload=cut.payload.first(length);
        check(!tcp(cut), "every variable TCP header truncation");
    }
}

static void aliasing()
{
    const auto &golden=tcp_vectors::v0;
    struct Case { usize offset, length; bool valid; };
    const std::array<Case,6> cases{{{127,1,true},{127,2,false},{128,1,false},
        {168,1,false},{169,1,true},{300,4,true}}};
    for (const auto &item:cases)
    {
        std::array<std::byte,512> storage{}; storage.fill(std::byte{0x71}); const auto before=storage;
        const auto output=std::span<std::byte>{storage}.subspan(128,256);
        const auto payload=Bytes{storage.data()+item.offset,item.length};
        auto built=build_tcp(output,golden.source,golden.destination,0,64,golden.fields,{},payload);
        check(built.ok()==item.valid, "payload alias boundary");
        if (!item.valid) check(equal(storage,before), "payload alias failure preserves all output and input");
        else
        {
            check(equal(Bytes{output.data()+40,item.length},Bytes{before.data()+item.offset,item.length}), "disjoint borrowed payload copied exactly");
            check(equal(Bytes{storage.data()+128+built.value(),storage.size()-128-built.value()},
                Bytes{before.data()+128+built.value(),storage.size()-128-built.value()}), "unused output tail input retained");
        }
    }
    for (usize offset : {usize{127},usize{128},usize{171},usize{172}})
    {
        std::array<std::byte,512> storage{}; storage.fill(std::byte{1}); const auto before=storage;
        auto built=build_tcp(std::span<std::byte>{storage}.subspan(128,256),golden.source,golden.destination,
            0,64,golden.fields,Bytes{storage.data()+offset,1},{});
        const bool valid=offset==127 || offset==172;
        check(built.ok()==valid, "option alias boundary includes padding prefix");
        if (!valid) check(equal(storage,before), "option alias refusal preserves backing buffer");
    }
}

int main()
{
    goldens(); header_and_options(); boundaries(); aliasing();
    std::printf("TCP_WIRE_HOSTED_PASS checks=%u independent_goldens=6 guest_execution=0\n",checks);
}
