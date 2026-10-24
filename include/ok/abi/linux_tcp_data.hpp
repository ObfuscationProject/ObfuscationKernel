#pragma once

// Derived from the sealed all-ten Linux/musl TCP inventory.
#include <ok/abi/linux_user.hpp>

namespace ok::abi::linux_user
{
enum class TcpCall : u8 { none, socket, bind, connect, listen, accept, accept4, shutdown, getsockname, getpeername, getsockopt, setsockopt, read, write, readv, writev, sendto, recvfrom, socketcall };
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_socket_numbers{359U, 41U, 198U, 281U, 198U, 198U, 198U, 4183U, 5040U, 326U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_bind_numbers{361U, 49U, 200U, 282U, 200U, 200U, 200U, 4169U, 5048U, 327U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_connect_numbers{362U, 42U, 203U, 283U, 203U, 203U, 203U, 4170U, 5041U, 328U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_listen_numbers{363U, 50U, 201U, 284U, 201U, 201U, 201U, 4174U, 5049U, 329U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_accept_numbers{unavailable_number, 43U, 202U, 285U, 202U, 202U, 202U, 4168U, 5042U, 330U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_accept4_numbers{364U, 288U, 242U, 366U, 242U, 242U, 242U, 4334U, 5293U, 344U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_shutdown_numbers{373U, 48U, 210U, 293U, 210U, 210U, 210U, 4182U, 5047U, 338U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_getsockname_numbers{367U, 51U, 204U, 286U, 204U, 204U, 204U, 4172U, 5050U, 331U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_getpeername_numbers{368U, 52U, 205U, 287U, 205U, 205U, 205U, 4171U, 5051U, 332U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_getsockopt_numbers{365U, 55U, 209U, 295U, 209U, 209U, 209U, 4173U, 5054U, 340U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_setsockopt_numbers{366U, 54U, 208U, 294U, 208U, 208U, 208U, 4181U, 5053U, 339U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_read_numbers{3U, 0U, 63U, 3U, 63U, 63U, 63U, 4003U, 5000U, 3U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_write_numbers{4U, 1U, 64U, 4U, 64U, 64U, 64U, 4004U, 5001U, 4U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_readv_numbers{145U, 19U, 65U, 145U, 65U, 65U, 65U, 4145U, 5018U, 145U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_writev_numbers{146U, 20U, 66U, 146U, 66U, 66U, 66U, 4146U, 5019U, 146U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_sendto_numbers{369U, 44U, 206U, 290U, 206U, 206U, 206U, 4180U, 5043U, 335U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_recvfrom_numbers{371U, 45U, 207U, 292U, 207U, 207U, 207U, 4176U, 5044U, 337U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_socketcall_numbers{102U, unavailable_number, unavailable_number, unavailable_number, unavailable_number, unavailable_number, unavailable_number, 4102U, unavailable_number, 102U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_errno_ealready{114U, 114U, 114U, 114U, 114U, 114U, 114U, 149U, 149U, 114U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_errno_econnaborted{103U, 103U, 103U, 103U, 103U, 103U, 103U, 130U, 130U, 103U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_errno_econnrefused{111U, 111U, 111U, 111U, 111U, 111U, 111U, 146U, 146U, 111U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_errno_econnreset{104U, 104U, 104U, 104U, 104U, 104U, 104U, 131U, 131U, 104U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_errno_einprogress{115U, 115U, 115U, 115U, 115U, 115U, 115U, 150U, 150U, 115U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_errno_eintr{4U, 4U, 4U, 4U, 4U, 4U, 4U, 4U, 4U, 4U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_errno_eisconn{106U, 106U, 106U, 106U, 106U, 106U, 106U, 133U, 133U, 106U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_errno_enotconn{107U, 107U, 107U, 107U, 107U, 107U, 107U, 134U, 134U, 107U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_errno_epipe{32U, 32U, 32U, 32U, 32U, 32U, 32U, 32U, 32U, 32U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_errno_etimedout{110U, 110U, 110U, 110U, 110U, 110U, 110U, 145U, 145U, 110U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_sol_socket{1U, 1U, 1U, 1U, 1U, 1U, 1U, 65535U, 65535U, 1U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_so_error{4U, 4U, 4U, 4U, 4U, 4U, 4U, 4103U, 4103U, 4U};
inline constexpr std::array<u32, static_cast<usize>(Profile::count)> tcp_so_type{3U, 3U, 3U, 3U, 3U, 3U, 3U, 4104U, 4104U, 3U};
template <Profile Selected> requires SupportedProfile<Selected>
[[nodiscard]] constexpr TcpCall tcp_call(u32 number)
{
    if (number == unavailable_number) return TcpCall::none;
    if (number == tcp_socket_numbers[static_cast<usize>(Selected)]) return TcpCall::socket;
    if (number == tcp_bind_numbers[static_cast<usize>(Selected)]) return TcpCall::bind;
    if (number == tcp_connect_numbers[static_cast<usize>(Selected)]) return TcpCall::connect;
    if (number == tcp_listen_numbers[static_cast<usize>(Selected)]) return TcpCall::listen;
    if (number == tcp_accept_numbers[static_cast<usize>(Selected)]) return TcpCall::accept;
    if (number == tcp_accept4_numbers[static_cast<usize>(Selected)]) return TcpCall::accept4;
    if (number == tcp_shutdown_numbers[static_cast<usize>(Selected)]) return TcpCall::shutdown;
    if (number == tcp_getsockname_numbers[static_cast<usize>(Selected)]) return TcpCall::getsockname;
    if (number == tcp_getpeername_numbers[static_cast<usize>(Selected)]) return TcpCall::getpeername;
    if (number == tcp_getsockopt_numbers[static_cast<usize>(Selected)]) return TcpCall::getsockopt;
    if (number == tcp_setsockopt_numbers[static_cast<usize>(Selected)]) return TcpCall::setsockopt;
    if (number == tcp_read_numbers[static_cast<usize>(Selected)]) return TcpCall::read;
    if (number == tcp_write_numbers[static_cast<usize>(Selected)]) return TcpCall::write;
    if (number == tcp_readv_numbers[static_cast<usize>(Selected)]) return TcpCall::readv;
    if (number == tcp_writev_numbers[static_cast<usize>(Selected)]) return TcpCall::writev;
    if (number == tcp_sendto_numbers[static_cast<usize>(Selected)]) return TcpCall::sendto;
    if (number == tcp_recvfrom_numbers[static_cast<usize>(Selected)]) return TcpCall::recvfrom;
    if (number == tcp_socketcall_numbers[static_cast<usize>(Selected)]) return TcpCall::socketcall;
    return TcpCall::none;
}
}
