#include "ok/abi/linux_udp_address.hpp"
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
}

int main()
{
    vectors<Profile::i386, false>();
    vectors<Profile::x86_64, false>();
    vectors<Profile::aarch64, false>();
    vectors<Profile::arm32, false>();
    vectors<Profile::rv64, false>();
    vectors<Profile::rv32, false>();
    vectors<Profile::loongarch64, false>();
    vectors<Profile::mips, true>();
    vectors<Profile::mips64, true>();
    vectors<Profile::ppc, true>();
    std::printf("LINUX_UDP_ADDRESS_MODEL_PASS profiles=10 checks=%zu\n", checks);
}
