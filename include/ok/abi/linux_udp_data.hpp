#pragma once

// Generated from the exact reviewed inventory; see docs/abi/linux-udp-profiles.json.
#include "ok/abi/linux_profile_data.hpp"

namespace ok::abi::linux_user
{
enum class UdpMessage : u8 { batch, cmsg_cloexec, confirm, ctrunc, dontroute, dontwait, eor, errqueue, fastopen, fin, more, nosignal, oob, peek, proxy, rst, syn, trunc, waitall, waitforone, zerocopy, count };
enum class UdpErrno : u8 { e_acces, e_addrinuse, e_addrnotavail, e_afnosupport, e_again, e_badf, e_destaddrreq, e_fault, e_inval, e_io, e_mfile, e_msgsize, e_netdown, e_netunreach, e_nfile, e_nobufs, e_nomem, e_nosys, e_notsock, e_opnotsupp, e_overflow, e_protonosupport, e_socktnosupport, e_wouldblock, count };
inline constexpr u32 udp_af_unspec = 0U;
inline constexpr u32 udp_af_inet = 2U;
inline constexpr u32 udp_protocol = 17U;
inline constexpr usize udp_sockaddr_input_limit = 128;
inline constexpr std::array<u32, 4> udp_subcalls{1U, 2U, 11U, 12U};
struct UdpProfileData
{
    std::array<u32, 5> numbers; // socket, bind, sendto, recvfrom, socketcall
    std::array<u32, 5> socket; // stream, dgram, nonblock, cloexec, type mask
    std::array<u32, static_cast<usize>(UdpMessage::count)> messages;
    std::array<u32, static_cast<usize>(UdpErrno::count)> errors;
};
inline constexpr std::array<UdpProfileData, static_cast<usize>(Profile::count)> udp_profiles{{
    // i386
    {{359U, 361U, 369U, 371U, 102U},
     {1U, 2U, 2048U, 524288U, 15U},
     {262144U, 1073741824U, 2048U, 8U, 4U, 64U, 128U, 8192U, 536870912U, 512U, 32768U, 16384U, 1U, 2U, 16U, 4096U, 1024U, 32U, 256U, 65536U, 67108864U},
     {13U, 98U, 99U, 97U, 11U, 9U, 89U, 14U, 22U, 5U, 24U, 90U, 100U, 101U, 23U, 105U, 12U, 38U, 88U, 95U, 75U, 93U, 94U, 11U}},
    // x86_64
    {{41U, 49U, 44U, 45U, unavailable_number},
     {1U, 2U, 2048U, 524288U, 15U},
     {262144U, 1073741824U, 2048U, 8U, 4U, 64U, 128U, 8192U, 536870912U, 512U, 32768U, 16384U, 1U, 2U, 16U, 4096U, 1024U, 32U, 256U, 65536U, 67108864U},
     {13U, 98U, 99U, 97U, 11U, 9U, 89U, 14U, 22U, 5U, 24U, 90U, 100U, 101U, 23U, 105U, 12U, 38U, 88U, 95U, 75U, 93U, 94U, 11U}},
    // aarch64
    {{198U, 200U, 206U, 207U, unavailable_number},
     {1U, 2U, 2048U, 524288U, 15U},
     {262144U, 1073741824U, 2048U, 8U, 4U, 64U, 128U, 8192U, 536870912U, 512U, 32768U, 16384U, 1U, 2U, 16U, 4096U, 1024U, 32U, 256U, 65536U, 67108864U},
     {13U, 98U, 99U, 97U, 11U, 9U, 89U, 14U, 22U, 5U, 24U, 90U, 100U, 101U, 23U, 105U, 12U, 38U, 88U, 95U, 75U, 93U, 94U, 11U}},
    // arm32
    {{281U, 282U, 290U, 292U, unavailable_number},
     {1U, 2U, 2048U, 524288U, 15U},
     {262144U, 1073741824U, 2048U, 8U, 4U, 64U, 128U, 8192U, 536870912U, 512U, 32768U, 16384U, 1U, 2U, 16U, 4096U, 1024U, 32U, 256U, 65536U, 67108864U},
     {13U, 98U, 99U, 97U, 11U, 9U, 89U, 14U, 22U, 5U, 24U, 90U, 100U, 101U, 23U, 105U, 12U, 38U, 88U, 95U, 75U, 93U, 94U, 11U}},
    // rv64
    {{198U, 200U, 206U, 207U, unavailable_number},
     {1U, 2U, 2048U, 524288U, 15U},
     {262144U, 1073741824U, 2048U, 8U, 4U, 64U, 128U, 8192U, 536870912U, 512U, 32768U, 16384U, 1U, 2U, 16U, 4096U, 1024U, 32U, 256U, 65536U, 67108864U},
     {13U, 98U, 99U, 97U, 11U, 9U, 89U, 14U, 22U, 5U, 24U, 90U, 100U, 101U, 23U, 105U, 12U, 38U, 88U, 95U, 75U, 93U, 94U, 11U}},
    // rv32
    {{198U, 200U, 206U, 207U, unavailable_number},
     {1U, 2U, 2048U, 524288U, 15U},
     {262144U, 1073741824U, 2048U, 8U, 4U, 64U, 128U, 8192U, 536870912U, 512U, 32768U, 16384U, 1U, 2U, 16U, 4096U, 1024U, 32U, 256U, 65536U, 67108864U},
     {13U, 98U, 99U, 97U, 11U, 9U, 89U, 14U, 22U, 5U, 24U, 90U, 100U, 101U, 23U, 105U, 12U, 38U, 88U, 95U, 75U, 93U, 94U, 11U}},
    // loongarch64
    {{198U, 200U, 206U, 207U, unavailable_number},
     {1U, 2U, 2048U, 524288U, 15U},
     {262144U, 1073741824U, 2048U, 8U, 4U, 64U, 128U, 8192U, 536870912U, 512U, 32768U, 16384U, 1U, 2U, 16U, 4096U, 1024U, 32U, 256U, 65536U, 67108864U},
     {13U, 98U, 99U, 97U, 11U, 9U, 89U, 14U, 22U, 5U, 24U, 90U, 100U, 101U, 23U, 105U, 12U, 38U, 88U, 95U, 75U, 93U, 94U, 11U}},
    // mips
    {{4183U, 4169U, 4180U, 4176U, 4102U},
     {2U, 1U, 128U, 524288U, 15U},
     {262144U, 1073741824U, 2048U, 8U, 4U, 64U, 128U, 8192U, 536870912U, 512U, 32768U, 16384U, 1U, 2U, 16U, 4096U, 1024U, 32U, 256U, 65536U, 67108864U},
     {13U, 125U, 126U, 124U, 11U, 9U, 96U, 14U, 22U, 5U, 24U, 97U, 127U, 128U, 23U, 132U, 12U, 89U, 95U, 122U, 79U, 120U, 121U, 11U}},
    // mips64
    {{5040U, 5048U, 5043U, 5044U, unavailable_number},
     {2U, 1U, 128U, 524288U, 15U},
     {262144U, 1073741824U, 2048U, 8U, 4U, 64U, 128U, 8192U, 536870912U, 512U, 32768U, 16384U, 1U, 2U, 16U, 4096U, 1024U, 32U, 256U, 65536U, 67108864U},
     {13U, 125U, 126U, 124U, 11U, 9U, 96U, 14U, 22U, 5U, 24U, 97U, 127U, 128U, 23U, 132U, 12U, 89U, 95U, 122U, 79U, 120U, 121U, 11U}},
    // ppc
    {{326U, 327U, 335U, 337U, 102U},
     {1U, 2U, 2048U, 524288U, 15U},
     {262144U, 1073741824U, 2048U, 8U, 4U, 64U, 128U, 8192U, 536870912U, 512U, 32768U, 16384U, 1U, 2U, 16U, 4096U, 1024U, 32U, 256U, 65536U, 67108864U},
     {13U, 98U, 99U, 97U, 11U, 9U, 89U, 14U, 22U, 5U, 24U, 90U, 100U, 101U, 23U, 105U, 12U, 38U, 88U, 95U, 75U, 93U, 94U, 11U}},
}};
} // namespace ok::abi::linux_user
