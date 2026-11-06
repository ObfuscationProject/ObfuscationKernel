#include "ok/abi/linux_udp_address.hpp"
#include "ok/abi/linux_udp_call.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace
{
using namespace ok;
using namespace ok::abi::linux_user;
usize checks{};
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "LINUX_UDP_ADDRESS_MODEL_FAIL line=%d checks=%zu: %s\n", __LINE__, checks, #expression); \
    std::abort(); } } while (false)

template <Profile Selected>
concept AvailableCodec = requires { typename Ipv4SocketAddressCodec<Selected>; };
static_assert(!AvailableCodec<Profile::count>);

template <Profile Selected, bool Big>
void vectors()
{
    using Address = Ipv4SocketAddressCodec<Selected>;
    // Literal vectors independently name the target family and network port.
    constexpr std::array<u8, 16> little{2, 0, 0xd4, 0x31, 192, 0, 2, 9,
        0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x12, 0x34};
    constexpr std::array<u8, 16> big{0, 2, 0xd4, 0x31, 192, 0, 2, 9,
        0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x12, 0x34};
    constexpr std::array<u8, 16> little_output{2, 0, 0xd4, 0x31, 192, 0, 2, 9, 0, 0, 0, 0, 0, 0, 0, 0};
    constexpr std::array<u8, 16> big_output{0, 2, 0xd4, 0x31, 192, 0, 2, 9, 0, 0, 0, 0, 0, 0, 0, 0};
    const auto input = Big ? big : little;
    const auto unchanged = input;
    auto decoded = Address::decode(input);
    CHECK(decoded.ok());
    CHECK(decoded.value().family == 2 && decoded.value().port == 54321);
    CHECK((decoded.value().address == net::wire::Ipv4{192, 0, 2, 9}));
    CHECK(input == unchanged);
    std::array<u8, 16> output{};
    output.fill(0x5a);
    CHECK(Address::encode(output, {2, 54321, {192, 0, 2, 9}}).ok());
    CHECK(output == (Big ? big_output : little_output));

    constexpr std::array<u8, 16> unknown_little{0xcd, 0xab, 0x12, 0x34, 203, 0, 113, 4,
        0, 0, 0, 0, 0, 0, 0, 0};
    constexpr std::array<u8, 16> unknown_big{0xab, 0xcd, 0x12, 0x34, 203, 0, 113, 4,
        0, 0, 0, 0, 0, 0, 0, 0};
    decoded = Address::decode(Big ? unknown_big : unknown_little);
    CHECK(decoded.ok() && decoded.value().family == 0xabcd && decoded.value().port == 0x1234);
    CHECK((decoded.value().address == net::wire::Ipv4{203, 0, 113, 4}));
    CHECK(Address::encode(output, {0xabcd, 0x1234, {203, 0, 113, 4}}).ok());
    CHECK(output == (Big ? unknown_big : unknown_little));

    // AF_UNSPEC stays raw; only an operation can decide its special meanings.
    constexpr std::array<u8, 16> unspecified{};
    decoded = Address::decode(unspecified);
    CHECK(decoded.ok() && decoded.value().family == 0 && decoded.value().port == 0);
    CHECK((decoded.value().address == net::wire::Ipv4{}));
    CHECK(Address::encode(output, {0, 0, {}}).ok() && output == unspecified);
    constexpr std::array<u8, 16> maximum{0xff, 0xff, 0xff, 0xff, 255, 255, 255, 255,
        0, 0, 0, 0, 0, 0, 0, 0};
    decoded = Address::decode(maximum);
    CHECK(decoded.ok() && decoded.value().family == 65535 && decoded.value().port == 65535);
    CHECK((decoded.value().address == net::wire::Ipv4{255, 255, 255, 255}));
    CHECK(Address::encode(output, {65535, 65535, {255, 255, 255, 255}}).ok() && output == maximum);

    std::array<u8, 32> invalid{};
    invalid.fill(0x6e);
    const auto before = invalid;
    for (usize length = 0; length <= invalid.size(); ++length)
    {
        if (length == Address::record_size) continue;
        CHECK(Address::decode(std::span<const u8>{invalid}.first(length)).status().code() == StatusCode::invalid_argument);
        CHECK(Address::encode(std::span<u8>{invalid}.first(length), {2, 54321, {192, 0, 2, 9}}).code() == StatusCode::invalid_argument);
        CHECK(invalid == before);
    }
    // Publication touches only the selected copied record, including padding.
    CHECK(Address::encode(std::span<u8>{invalid}.subspan(5, 16), {2, 54321, {192, 0, 2, 9}}).ok());
    const auto &expected = Big ? big_output : little_output;
    CHECK(std::equal(expected.begin(), expected.end(), invalid.begin() + 5));
    CHECK(std::all_of(invalid.begin(), invalid.begin() + 5, [](u8 b) { return b == 0x6e; }));
    CHECK(std::all_of(invalid.begin() + 21, invalid.end(), [](u8 b) { return b == 0x6e; }));
}
template <Profile Selected, bool Big>
void ipv6_vectors()
{
    using Address = Ipv6SocketAddressCodec<Selected>;
    static_assert(Address::record_size == 28 && Address::minimum_size == 24 && udp_af_inet6 == 10);
    static_assert(UdpConstants<Selected>::af_inet6 == 10);
    // Family and scope_id follow the profile; port and flowinfo are network order.
    constexpr std::array<u8, 28> little{10, 0, 0xd4, 0x31, 0x01, 0x02, 0x03, 0x04,
        0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x09, 0x78, 0x56, 0x34, 0x12};
    constexpr std::array<u8, 28> big{0, 10, 0xd4, 0x31, 0x01, 0x02, 0x03, 0x04,
        0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x09, 0x12, 0x34, 0x56, 0x78};
    constexpr net::wire::Ipv6 documentation{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x09};
    const auto &input = Big ? big : little;
    auto decoded = Address::decode(input);
    CHECK(decoded.ok() && decoded.value().family == 10 && decoded.value().port == 54321);
    CHECK(decoded.value().flowinfo == 0x01020304u && decoded.value().scope_id == 0x12345678u);
    CHECK(decoded.value().address == documentation);
    std::array<u8, 28> output{};
    output.fill(0x5a);
    CHECK(Address::encode(output, {10, 54321, 0x01020304u, documentation, 0x12345678u}).ok() && output == input);
    // RFC 2133 input omits scope_id; it decodes as zero.
    decoded = Address::decode(std::span<const u8>{input}.first(24));
    CHECK(decoded.ok() && decoded.value().scope_id == 0 && decoded.value().address == documentation);
    for (usize length = 0; length <= 32; ++length)
    {
        std::array<u8, 32> bytes{};
        if (length != 24 && length != 28)
            CHECK(Address::decode(std::span<const u8>{bytes}.first(length)).status().code() == StatusCode::invalid_argument);
        if (length != 28)
            CHECK(Address::encode(std::span<u8>{bytes}.first(length), {}).code() == StatusCode::invalid_argument);
    }
    // Copied input admits 24..128 bytes; scope_id is read only from a full record.
    std::array<u8, 128> copied{};
    copied.fill(0x77);
    for (usize at = 0; at < input.size(); ++at) copied[at] = input[at];
    for (usize length : {usize{0}, usize{2}, usize{16}, usize{23}, usize{129}})
        CHECK(UdpWire<Selected>::decode_ipv6_address(std::span<const u8>{copied.data(), length}).status().code() ==
              StatusCode::invalid_argument);
    for (usize length : {usize{24}, usize{27}, usize{28}, usize{128}})
    {
        decoded = UdpWire<Selected>::decode_ipv6_address(std::span<const u8>{copied}.first(length));
        CHECK(decoded.ok() && decoded.value().address == documentation && decoded.value().port == 54321);
        CHECK(decoded.value().scope_id == (length < 28 ? 0u : 0x12345678u));
    }

    // Names: an IPv4 address keeps the exact sockaddr_in output.
    auto name = encode_socket_name<Selected>(net::IpAddress{192, 0, 2, 9}, 54321);
    std::array<u8, 16> ipv4{};
    CHECK(name.ok() && name.value().size == 16 &&
          Ipv4SocketAddressCodec<Selected>::encode(ipv4, {2, 54321, {192, 0, 2, 9}}).ok());
    CHECK(std::equal(ipv4.begin(), ipv4.end(), name.value().bytes.begin()));
    // IPv6 names carry flowinfo 0 and scope 1 only for link-local addresses.
    name = encode_socket_name<Selected>(net::IpAddress{documentation}, 54321);
    CHECK(name.ok() && name.value().size == 28 && name.value().record().size() == 28);
    decoded = Address::decode(name.value().record());
    CHECK(decoded.ok() && decoded.value().family == 10 && decoded.value().flowinfo == 0 && decoded.value().scope_id == 0);
    constexpr net::wire::Ipv6 link{0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x50, 0x54, 0, 0xff, 0xfe, 0x12, 0x34, 0x56};
    name = encode_socket_name<Selected>(net::IpAddress{link}, 0);
    decoded = Address::decode(name.value().record());
    CHECK(decoded.ok() && decoded.value().scope_id == 1 && decoded.value().port == 0 && decoded.value().address == link);
    CHECK(name.value().bytes[Big ? 27 : 24] == 1);
    name = encode_socket_name<Selected>(net::IpAddress{net::wire::Ipv6{}}, 0);
    decoded = Address::decode(name.value().record());
    CHECK(decoded.ok() && decoded.value().family == 10 && decoded.value().scope_id == 0 &&
          decoded.value().address == net::wire::Ipv6{});
}
}

int main()
{
    vectors<Profile::i386, false>();
    ipv6_vectors<Profile::i386, false>();
    vectors<Profile::x86_64, false>();
    ipv6_vectors<Profile::x86_64, false>();
    vectors<Profile::aarch64, false>();
    ipv6_vectors<Profile::aarch64, false>();
    vectors<Profile::arm32, false>();
    ipv6_vectors<Profile::arm32, false>();
    vectors<Profile::rv64, false>();
    ipv6_vectors<Profile::rv64, false>();
    vectors<Profile::rv32, false>();
    ipv6_vectors<Profile::rv32, false>();
    vectors<Profile::loongarch64, false>();
    ipv6_vectors<Profile::loongarch64, false>();
    vectors<Profile::mips, true>();
    ipv6_vectors<Profile::mips, true>();
    vectors<Profile::mips64, true>();
    ipv6_vectors<Profile::mips64, true>();
    vectors<Profile::ppc, true>();
    ipv6_vectors<Profile::ppc, true>();
    std::printf("LINUX_UDP_ADDRESS_MODEL_PASS profiles=10 checks=%zu\n", checks);
}
