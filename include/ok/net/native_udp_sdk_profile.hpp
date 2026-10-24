#pragma once

// Compiled source contract for the assigned private ABI67 successor.
// Capability declarations do not qualify NIC execution or whole-call-chain stacks.
#include "ok/core/extension.hpp"
#include "ok/sched/resources.hpp"
#include <concepts>
#include <string_view>
#include <type_traits>

namespace ok::net
{
inline constexpr std::string_view native_udp_profile = "bootcpu-static-peer-ipv4-polling-captured-connected-zero-send-v3";
inline constexpr std::string_view linux_udp_profile = "bootcpu-static-peer-ipv4-captured-connected-zero-send-v3";
inline constexpr std::string_view native_udp_connected_profile = "bounded-static-peer-association-zero-destination-output-v2";
inline constexpr u32 native_udp_module_abi = 74;
inline constexpr usize native_udp_socket_capacity = 16;
inline constexpr usize native_udp_queue_capacity = 2;
inline constexpr usize native_udp_datagram_capacity = 1472;
inline constexpr usize native_udp_frame_capacity = 1514;

template <class Extension>
concept NativeUdpSdkExtension = requires(Extension &extension, Kernel &kernel,
                                        usize budget, std::span<std::byte> input,
                                        std::span<const std::byte> output) {
    { extension.network_link(kernel) } -> std::same_as<Result<NativeUdpLink>>;
    { extension.network_receive(kernel, input) } -> std::same_as<Result<usize>>;
    { extension.network_try_transmit(kernel, output) } -> std::same_as<Status>;
    { extension.network_poll_transmit(kernel, budget) } -> std::same_as<Result<NativeUdpTransmitState>>;
};

static_assert(kernel_module_abi_version == native_udp_module_abi);
static_assert(NativeUdpSdkExtension<KernelExtension>);
static_assert(NativeUdpService::socket_capacity == native_udp_socket_capacity);
static_assert(NativeUdpService::queue_capacity == native_udp_queue_capacity);
static_assert(NativeUdpService::datagram_capacity == native_udp_datagram_capacity);
static_assert(NativeUdpService::frame_capacity == native_udp_frame_capacity);
static_assert(!std::is_copy_constructible_v<NativeUdpService>);
static_assert(!std::is_move_constructible_v<NativeUdpService>);
static_assert(!std::is_copy_constructible_v<NativeUdpExecutor>);
static_assert(!std::is_move_constructible_v<NativeUdpExecutor>);
static_assert(std::is_same_v<decltype(NativeUdpFrame::queued_handle), u32>);
static_assert(std::is_same_v<decltype(&NativeUdpService::bind_ephemeral),
                           Status (NativeUdpService::*)(u32, wire::Ipv4)>);
static_assert(std::is_same_v<decltype(&NativeUdpService::autobind),
                           Status (NativeUdpService::*)(u32)>);
static_assert(std::is_same_v<decltype(&NativeUdpService::connect),
                           Status (NativeUdpService::*)(u32, wire::Ipv4, u16)>);
static_assert(std::is_same_v<decltype(&NativeUdpService::disconnect),
                           Status (NativeUdpService::*)(u32)>);
static_assert(std::is_same_v<decltype(&NativeUdpService::peer),
                           Result<NativeUdpPeerSnapshot> (NativeUdpService::*)(u32) const>);
static_assert(std::is_same_v<decltype(NativeUdpPeerSnapshot::associated), bool>);
static_assert(std::is_same_v<decltype(NativeUdpPeerSnapshot::address), wire::Ipv4>);
static_assert(std::is_same_v<decltype(NativeUdpPeerSnapshot::port), u16>);
static_assert(std::is_trivially_copyable_v<NativeUdpPeerSnapshot>);
static_assert(std::is_standard_layout_v<NativeUdpPeerSnapshot>);
static_assert(static_cast<u8>(sched::ThreadWaitReason::udp_receive) == 11);
} // namespace ok::net
