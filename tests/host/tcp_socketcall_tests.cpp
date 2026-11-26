// Legacy socketcall routing for TCP streams on all ten profiles: which
// profiles have it, selector mapping, argument arrays in each byte order,
// and which descriptors TCP owns. Value model only; no guest executes here.
#include <ok/abi/linux_native.hpp>
#include "../../src/user/tcp_socketcall.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace ok;
namespace lu = ok::abi::linux_user;
using ok::user::native::TcpSocketcall;

namespace
{
usize checks{};
#define CHECK(value) do { ++checks; if (!static_cast<bool>(value)) { \
    std::fprintf(stderr, "TCP_SOCKETCALL_FAIL line=%d check=%zu: %s\n", __LINE__, checks, #value); std::abort(); } } while (false)
using Call = lu::TcpCall;
usize routed_profiles{};

template <lu::Profile P> lu::RegisterFile frame(u64 number, u64 selector, u64 array)
{
    using Target = lu::Codec<P>;
    lu::RegisterFile saved{};
    saved.integer[Target::profile.number_register] = number;
    saved.integer[Target::profile.argument_registers[0]] = selector;
    saved.integer[Target::profile.argument_registers[1]] = array;
    return saved;
}
template <lu::Profile P> std::vector<std::byte> words(std::initializer_list<u32> values)
{
    std::vector<std::byte> out;
    for (const u32 value : values)
        for (usize index = 0; index < 4; ++index)
        {
            const usize shift = lu::Codec<P>::profile.big_endian ? (3 - index) * 8 : index * 8;
            out.push_back(std::byte{static_cast<u8>(value >> shift)});
        }
    return out;
}

template <lu::Profile P> void exercise(bool expected)
{
    using S = TcpSocketcall<P>;
    using D = typename S::Descriptor;
    using Constants = lu::UdpConstants<P>;
    static_assert(S::nargs.size() == 21);
    CHECK(S::available == expected);
    const u32 number = lu::tcp_socketcall_numbers[static_cast<usize>(P)];
    if (!expected)
    {
        // Direct-call profiles: no register file is a TCP socketcall.
        for (u64 selector = 0; selector < 22; ++selector)
            CHECK(S::route(frame<P>(102, selector, 0x1000)).call == Call::none &&
                  S::route(frame<P>(4102, selector, 0x1000)).call == Call::none);
        return;
    }
    ++routed_profiles;
    // Selector mapping (net/socket.c): accept/accept4 keep their own route,
    // UNIX message calls and the mmsg calls are not TCP.
    const std::array<Call, 22> expected_calls{Call::none, Call::socket, Call::bind, Call::connect, Call::listen,
        Call::none, Call::getsockname, Call::getpeername, Call::none, Call::sendto, Call::recvfrom, Call::sendto,
        Call::recvfrom, Call::shutdown, Call::setsockopt, Call::getsockopt, Call::none, Call::none, Call::none,
        Call::none, Call::none, Call::none};
    for (u64 selector = 0; selector < expected_calls.size(); ++selector)
    {
        const auto route = S::route(frame<P>(number, selector, 0x1000));
        CHECK(route.call == expected_calls[selector]);
        if (route.call != Call::none)
            CHECK(route.selector == selector && route.array == 0x1000 && route.words == S::nargs[selector] &&
                  route.bytes() == route.words * 4);
    }
    CHECK(S::route(frame<P>(number + 1, 3, 0x1000)).call == Call::none);
    CHECK(S::route(frame<P>(number, u64{1} << 32, 0x1000)).call == Call::none);
    CHECK(S::route(frame<P>(number, 3, u64{1} << 32)).call == Call::none);
    // The array must be present and must not wrap.
    CHECK(!S::interval(S::route(frame<P>(number, 3, 0))));
    CHECK(!S::interval(S::route(frame<P>(number, 11, 0xfffffff0u))));
    CHECK(S::interval(S::route(frame<P>(number, 11, 0xffffffe8u))));
    // Arguments in the profile's byte order; send/recv leave the address 0.
    const auto sendto = S::route(frame<P>(number, 11, 0x2000));
    const auto six = words<P>({7, 0x40001000u, 512, 0x4040, 0x40002000u, 16});
    auto decoded = S::arguments(sendto, six); CHECK(decoded);
    CHECK((decoded.value() == std::array<u64, 6>{7, 0x40001000u, 512, 0x4040, 0x40002000u, 16}));
    const auto send = S::route(frame<P>(number, 9, 0x2000));
    const auto four = words<P>({7, 0x40001000u, 512, 0x40});
    decoded = S::arguments(send, four); CHECK(decoded);
    CHECK((decoded.value() == std::array<u64, 6>{7, 0x40001000u, 512, 0x40, 0, 0}));
    CHECK(send.call == Call::sendto && S::route(frame<P>(number, 10, 0x2000)).call == Call::recvfrom);
    CHECK(!S::arguments(send, six));
    CHECK(S::word(std::span<const std::byte>{four}.first(4)) == 7);
    // Ownership from the first two words.
    const auto kinds = [](u64 fd) { return fd == 3 ? D::tcp : fd == 4 ? D::udp : D::other; };
    const auto socket = S::route(frame<P>(number, 1, 0x2000));
    CHECK(S::owner(socket, 2, Constants::sock_stream, kinds) == Call::socket);
    CHECK(S::owner(socket, 10, Constants::sock_stream | Constants::sock_nonblock, kinds) == Call::socket);
    CHECK(S::owner(socket, 2, Constants::sock_dgram, kinds) == Call::none);
    for (const u64 selector : {2u, 3u, 4u, 6u, 7u, 9u, 10u, 11u, 12u, 13u, 14u})
    {
        const auto route = S::route(frame<P>(number, selector, 0x2000));
        CHECK(S::owner(route, 3, 0, kinds) == route.call);
        CHECK(S::owner(route, 4, 0, kinds) == Call::none && S::owner(route, 5, 0, kinds) == Call::none);
    }
    const auto getsockopt = S::route(frame<P>(number, 15, 0x2000));
    CHECK(S::owner(getsockopt, 3, 0, kinds) == Call::getsockopt && S::owner(getsockopt, 4, 0, kinds) == Call::getsockopt);
    CHECK(S::owner(getsockopt, 5, 0, kinds) == Call::none);
    CHECK(S::owner(typename S::Route{}, 3, 0, kinds) == Call::none);
}
}

int main()
{
    exercise<lu::Profile::i386>(true); exercise<lu::Profile::x86_64>(false);
    exercise<lu::Profile::aarch64>(false); exercise<lu::Profile::arm32>(false);
    exercise<lu::Profile::rv64>(false); exercise<lu::Profile::rv32>(false);
    exercise<lu::Profile::loongarch64>(false); exercise<lu::Profile::mips>(true);
    exercise<lu::Profile::mips64>(false); exercise<lu::Profile::ppc>(true);
    CHECK(routed_profiles == 3);
    std::printf("PASS tcp socketcall scope=model profiles=10 routed=%zu checks=%zu\n", routed_profiles, checks);
}
