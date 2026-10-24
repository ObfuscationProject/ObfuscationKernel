#pragma once

#include "ok/abi/linux_native.hpp"
#include "ok/abi/linux_udp_name_data.hpp"
#include "ok/abi/linux_udp_connected_data.hpp"

namespace ok::user::native
{
enum class LinuxUdpImmediateOperation : u8 { none, socket, bind, sendto, recvfrom, socketcall, getsockname, connect, getpeername };

// Numeric classification from the pinned Linux/musl inventory; no IO authority.
template <abi::linux_user::Profile Selected>
[[nodiscard]] constexpr LinuxUdpImmediateOperation linux_udp_immediate_operation(u32 number)
{
    static_assert(abi::linux_user::SupportedProfile<Selected>);
    if (number == abi::linux_user::udp_getsockname_numbers[static_cast<usize>(Selected)])
        return LinuxUdpImmediateOperation::getsockname;
    if (number == abi::linux_user::udp_connect_numbers[static_cast<usize>(Selected)])
        return LinuxUdpImmediateOperation::connect;
    if (number == abi::linux_user::udp_getpeername_numbers[static_cast<usize>(Selected)])
        return LinuxUdpImmediateOperation::getpeername;
    constexpr std::array<std::array<u32, 5>, 10> numbers{{
        {359, 361, 369, 371, 102}, // i386
        {41, 49, 44, 45, abi::linux_user::unavailable_number}, // x86_64
        {198, 200, 206, 207, abi::linux_user::unavailable_number}, // aarch64
        {281, 282, 290, 292, abi::linux_user::unavailable_number}, // arm32
        {198, 200, 206, 207, abi::linux_user::unavailable_number}, // rv64
        {198, 200, 206, 207, abi::linux_user::unavailable_number}, // rv32
        {198, 200, 206, 207, abi::linux_user::unavailable_number}, // loongarch64
        {4183, 4169, 4180, 4176, 4102}, // mips
        {5040, 5048, 5043, 5044, abi::linux_user::unavailable_number}, // mips64
        {326, 327, 335, 337, 102}, // ppc
    }};
    const auto &row = numbers[static_cast<usize>(Selected)];
    for (usize i = 0; i < row.size(); ++i)
        if (row[i] != abi::linux_user::unavailable_number && row[i] == number)
            return static_cast<LinuxUdpImmediateOperation>(i + 1);
    return LinuxUdpImmediateOperation::none;
}
// A legacy socketcall selector is a saved register value, not user-memory input.
// Reserve TCP accept/accept4 and UNIX socketpair/sendmsg/recvmsg before UDP capture.
template <abi::linux_user::Profile Selected>
[[nodiscard]] LinuxUdpImmediateOperation linux_udp_immediate_operation_for_frame(
    const arch::execution::UserFrame &frame)
{
    const auto raw = abi::linux_user::NativeBridge<Selected>::decode_call(frame, 1);
    if (!raw) return LinuxUdpImmediateOperation::none;
    const auto operation = linux_udp_immediate_operation<Selected>(raw.value().number);
    if (operation == LinuxUdpImmediateOperation::socketcall &&
        (raw.value().arguments[0] == 5 || raw.value().arguments[0] == 18 || raw.value().arguments[0] == 8 ||
         raw.value().arguments[0] == 16 || raw.value().arguments[0] == 17))
        return LinuxUdpImmediateOperation::none;
    return operation;
}
} // namespace ok::user::native
