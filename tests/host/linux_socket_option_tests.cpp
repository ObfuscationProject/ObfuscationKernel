// Socket option names and errno values of the Linux personality on all ten
// profiles (ok/abi/linux_tcp_data.hpp) against the musl 1.2.5 values:
// include/sys/socket.h and arch/{mips,mips64}/bits/socket.h (PowerPC keeps
// the generic numbers of these options), netinet/tcp.h, and each profile's
// bits/errno.h. Value model only; no guest executes here.
#include <ok/abi/linux_tcp_data.hpp>
#include <ok/uapi/syscall.h>
#include <array>
#include <cstdio>
#include <cstdlib>

using namespace ok;
namespace lu = ok::abi::linux_user;

namespace
{
usize checks{};
#define CHECK(value) do { ++checks; if (!static_cast<bool>(value)) { \
    std::fprintf(stderr, "LINUX_SOCKET_OPTION_FAIL line=%d check=%zu: %s\n", __LINE__, checks, #value); std::abort(); } } while (false)

struct Names { u32 sol_socket, type, error, reuse_address, keep_alive, send_buffer, receive_buffer, accept_connection, protocol, domain; };
constexpr Names generic{1, 3, 4, 2, 9, 7, 8, 30, 38, 39};
constexpr Names mips{0xffff, 0x1008, 0x1007, 0x0004, 0x0008, 0x1001, 0x1002, 0x1009, 0x1028, 0x1029};

template <lu::Profile P> void profile(usize &mips_profiles)
{
    constexpr auto at = static_cast<usize>(P);
    const bool renumbered = P == lu::Profile::mips || P == lu::Profile::mips64;
    mips_profiles += renumbered;
    const Names &want = renumbered ? mips : generic;
    const Names got{lu::tcp_sol_socket[at], lu::tcp_so_type[at], lu::tcp_so_error[at], lu::tcp_so_reuseaddr[at],
        lu::tcp_so_keepalive[at], lu::tcp_so_sndbuf[at], lu::tcp_so_rcvbuf[at], lu::tcp_so_acceptconn[at],
        lu::tcp_so_protocol[at], lu::tcp_so_domain[at]};
    CHECK(got.sol_socket == want.sol_socket && got.type == want.type && got.error == want.error);
    CHECK(got.reuse_address == want.reuse_address && got.keep_alive == want.keep_alive);
    CHECK(got.send_buffer == want.send_buffer && got.receive_buffer == want.receive_buffer);
    CHECK(got.accept_connection == want.accept_connection && got.protocol == want.protocol && got.domain == want.domain);
    // The runtime names an option by the first match: names must be distinct.
    const std::array<u32, 9> options{got.type, got.error, got.reuse_address, got.keep_alive, got.send_buffer,
        got.receive_buffer, got.accept_connection, got.protocol, got.domain};
    for (usize i = 0; i < options.size(); ++i)
        for (usize j = i + 1; j < options.size(); ++j) CHECK(options[i] != options[j]);
    // SOL_SOCKET never collides with SOL_IP (0), SOL_TCP (6), SOL_UDP (17) or SOL_IPV6 (41).
    for (u32 level : {0u, lu::tcp_sol_tcp, 17u, 41u}) CHECK(got.sol_socket != level);
    CHECK(lu::tcp_errno_enoprotoopt[at] == (renumbered ? 99u : 92u));
    CHECK(lu::tcp_errno_econnaborted[at] == (renumbered ? 130u : 103u));
    CHECK(lu::tcp_errno_eisconn[at] == (renumbered ? 133u : 106u));
}
}

int main()
{
    CHECK(lu::tcp_sol_tcp == 6 && lu::tcp_nodelay == 1);
    // The native personality uses the generic numbers.
    CHECK(OK_SOL_SOCKET == generic.sol_socket && OK_SO_TYPE == generic.type && OK_SO_ERROR == generic.error);
    CHECK(OK_SO_REUSEADDR == generic.reuse_address && OK_SO_KEEPALIVE == generic.keep_alive);
    CHECK(OK_SO_SNDBUF == generic.send_buffer && OK_SO_RCVBUF == generic.receive_buffer);
    CHECK(OK_SO_ACCEPTCONN == generic.accept_connection && OK_SO_PROTOCOL == generic.protocol && OK_SO_DOMAIN == generic.domain);
    CHECK(OK_TCP_NODELAY == lu::tcp_nodelay && OK_IPPROTO_TCP == lu::tcp_sol_tcp && OK_ENOPROTOOPT == 92);
    usize renumbered{};
    profile<lu::Profile::i386>(renumbered); profile<lu::Profile::x86_64>(renumbered);
    profile<lu::Profile::aarch64>(renumbered); profile<lu::Profile::arm32>(renumbered);
    profile<lu::Profile::rv64>(renumbered); profile<lu::Profile::rv32>(renumbered);
    profile<lu::Profile::loongarch64>(renumbered); profile<lu::Profile::mips>(renumbered);
    profile<lu::Profile::mips64>(renumbered); profile<lu::Profile::ppc>(renumbered);
    CHECK(renumbered == 2);
    std::printf("PASS linux socket options scope=model profiles=10 renumbered=%zu checks=%zu\n", renumbered, checks);
}
