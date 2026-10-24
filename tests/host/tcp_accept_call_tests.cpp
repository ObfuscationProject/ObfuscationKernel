#include <ok/abi/linux_tcp_accept_call.hpp>
#include <ok/abi/linux_native.hpp>
#include <ok/abi/linux_udp_call.hpp>
#include <cstdio>
#include <cstdlib>
using namespace ok;
namespace lu = ok::abi::linux_user;
namespace
{
usize checks{};
#define CHECK(value) do { ++checks; if (!static_cast<bool>(value)) { std::fprintf(stderr, "TCP_ACCEPT_CALL_FAIL line=%d check=%zu: %s\n", __LINE__, checks, #value); std::abort(); } } while(false)
template <lu::Profile Selected> std::array<std::byte, 4> word(u32 value)
{
    std::array<std::byte, 4> bytes{};
    for (usize index = 0; index < 4; ++index)
    {
        const usize shift = lu::Codec<Selected>::profile.big_endian ? (3 - index) * 8 : index * 8;
        bytes[index] = std::byte{static_cast<u8>(value >> shift)};
    }
    return bytes;
}
template <lu::Profile Selected> void controls()
{
    using Helper = lu::TcpAcceptCall<Selected>; using Codec = lu::Codec<Selected>;
    constexpr auto index = static_cast<usize>(Selected); const auto &profile = Codec::profile;
    lu::RegisterFile frame{};
    frame.integer[profile.number_register] = lu::tcp_accept4_numbers[index];
    frame.integer[profile.argument_registers[0]] = 9;
    auto direct = Helper::route(frame); CHECK(direct.operation == lu::TcpCall::accept4 && !direct.socketcall);
    auto decoded = Codec::decode_call(frame, 4); CHECK(decoded && decoded.value().arguments[0] == 9);
    if constexpr (Selected == lu::Profile::mips) CHECK(decoded.value().argument_count == 4);
    frame.integer[profile.number_register] = lu::tcp_accept_numbers[index];
    if (lu::tcp_accept_numbers[index] != lu::unavailable_number)
    {
        CHECK(Helper::route(frame).operation == lu::TcpCall::accept);
        CHECK(Codec::decode_call(frame, 3));
    }
    else CHECK(Helper::route(frame).operation == lu::TcpCall::none);
    frame.integer[profile.number_register] = lu::tcp_socketcall_numbers[index] == lu::unavailable_number ? 102 : lu::tcp_socketcall_numbers[index];
    frame.integer[profile.argument_registers[1]] = 0x1000;
    constexpr bool legacy = Selected == lu::Profile::i386 || Selected == lu::Profile::mips || Selected == lu::Profile::ppc;
    for (u64 selector : {u64{5}, u64{18}, u64{8}, u64{16}, u64{17}, u64{1}, u64{6}, u64{20}, u64{0x100000005}})
    {
        frame.integer[profile.argument_registers[0]] = selector;
        const auto route = Helper::route(frame);
        if (!legacy || (selector != 5 && selector != 18))
        { CHECK(route.operation == lu::TcpCall::none && !route.socketcall); continue; }
        CHECK(route.socketcall && route.operation == (selector == 5 ? lu::TcpCall::accept : lu::TcpCall::accept4));
        CHECK(route.array == 0x1000 && route.array_bytes == (selector == 5 ? 12 : 16)); CHECK(Helper::interval(route));
        CHECK(Codec::decode_call(frame, 2)); // Outer O32 never requires stacked words.
        auto head = word<Selected>(0x01020304); auto fd = Helper::head(route, head); CHECK(fd && fd.value() == 0x01020304);
        CHECK(!Helper::head(route, std::span{head}.first(3)));
        std::array<std::byte, 12> tail{};
        const std::array<u32, 3> values{0x10203040, 0x50607080, lu::UdpConstants<Selected>::sock_nonblock | lu::UdpConstants<Selected>::sock_cloexec};
        for (usize at = 0; at < values.size(); ++at)
        { auto encoded = word<Selected>(values[at]); for (usize byte = 0; byte < 4; ++byte) tail[at * 4 + byte] = encoded[byte]; }
        auto arguments = Helper::tail(route, fd.value(), std::span{tail}.first(route.array_bytes - 4)); CHECK(arguments);
        CHECK(arguments.value()[0] == 0x01020304 && arguments.value()[1] == 0x10203040 && arguments.value()[2] == 0x50607080);
        CHECK(arguments.value()[3] == (selector == 18 ? values[2] : 0) && arguments.value()[4] == 0 && arguments.value()[5] == 0);
        CHECK(!Helper::tail(route, fd.value(), std::span{tail}.first(route.array_bytes - 5)));
        auto negative = Helper::head(route, word<Selected>(0xffffffff)); CHECK(negative && negative.value() == -1);
        CHECK(Helper::tail(route, -1, std::span{tail}.first(route.array_bytes - 4)).value()[0] == 0xffffffff);
        auto foreign = route; foreign.array_bytes = 20; CHECK(!Helper::interval(foreign));
        foreign = route; foreign.array = 0; CHECK(!Helper::interval(foreign));
        foreign = route; foreign.array = Codec::word_mask - route.array_bytes + 2; CHECK(!Helper::interval(foreign));
        foreign.array -= 1; CHECK(Helper::interval(foreign));
    }
    frame.integer[profile.number_register] = 0x100000000 + lu::tcp_accept4_numbers[index];
    CHECK(Helper::route(frame).operation == lu::TcpCall::none);
}
}
int main()
{
    controls<lu::Profile::i386>(); controls<lu::Profile::x86_64>(); controls<lu::Profile::aarch64>(); controls<lu::Profile::arm32>();
    controls<lu::Profile::rv64>(); controls<lu::Profile::rv32>(); controls<lu::Profile::loongarch64>(); controls<lu::Profile::mips>();
    controls<lu::Profile::mips64>(); controls<lu::Profile::ppc>();
    std::printf("TCP_ACCEPT_CALL_PASS checks=%zu profiles=10 copied_callbacks=0 syscall_execution=0 device_execution=0\n", checks);
}
