#pragma once

// Bounded client and prepared accept capability contract; acceptance receipts qualify execution.
#include <ok/core/module.hpp>
#include <ok/net/native_tcp_ethernet.hpp>
#include <ok/abi/linux_tcp_accept_call.hpp>
#include <ok/net/tcp_prepared_accept.hpp>
#include <ok/net/tcp_accept_copied.hpp>
#include <concepts>
#include <string_view>
#include <type_traits>

namespace ok::net
{
inline constexpr std::string_view native_tcp_profile = "bootcpu-static-peer-ipv4-captured-nonblocking-immediate-v1";
inline constexpr std::string_view linux_tcp_profile = "bootcpu-direct-static-peer-ipv4-captured-nonblocking-immediate-v1";
inline constexpr std::string_view native_tcp_accept_profile = "bootcpu-explicit-ipv4-captured-nonblocking-reserved-accept-v1";
inline constexpr std::string_view linux_tcp_accept_profile = "bootcpu-direct-explicit-ipv4-captured-nonblocking-reserved-accept-v1";
inline constexpr u32 native_tcp_module_abi = 78;
inline constexpr usize native_tcp_stream_capacity = 16;
inline constexpr usize native_tcp_connection_capacity = 8;
inline constexpr usize native_tcp_listener_capacity = 4;
inline constexpr usize native_tcp_backlog_capacity = 4;
inline constexpr usize native_tcp_send_capacity = 1024;
inline constexpr usize native_tcp_receive_capacity = 1024;
inline constexpr usize native_tcp_segment_capacity = 536;
inline constexpr usize native_tcp_poll_budget = 8;

static_assert(kernel_module_abi_version == native_tcp_module_abi);
static_assert(NativeTcpStreamService::stream_capacity == native_tcp_stream_capacity);
static_assert(std::is_same_v<NativeTcpStreamService::Table,
    tcp::EndpointTable<native_tcp_connection_capacity, native_tcp_listener_capacity,
                       native_tcp_backlog_capacity, native_tcp_send_capacity,
                       native_tcp_receive_capacity, native_tcp_segment_capacity>>);
static_assert(NativeTcpEthernetExecutor::budget == native_tcp_poll_budget);
static_assert(!std::is_copy_constructible_v<NativeTcpStreamService>);
static_assert(!std::is_move_constructible_v<NativeTcpStreamService>);
static_assert(!std::is_copy_constructible_v<NativeTcpEthernetExecutor>);
static_assert(!std::is_move_constructible_v<NativeTcpEthernetExecutor>);
static_assert(std::is_same_v<decltype(&NativeTcpStreamService::Table::local_reserved),
    Result<bool> (NativeTcpStreamService::Table::*)(tcp::Endpoint) const>);

static_assert(std::is_same_v<decltype(&NativeTcpStreamService::prepare_accept),
    Result<TcpAcceptReservation> (NativeTcpStreamService::*)(TcpStreamHandle)>);
static_assert(std::is_same_v<decltype(&NativeTcpStreamService::validate_accept),
    Status (NativeTcpStreamService::*)(const TcpAcceptReservation &) const>);
static_assert(std::is_same_v<decltype(&NativeTcpStreamService::commit_accept),
    Result<TcpStreamHandle> (NativeTcpStreamService::*)(const TcpAcceptReservation &)>);
static_assert(std::is_same_v<decltype(&NativeTcpStreamService::cancel_accept),
    Status (NativeTcpStreamService::*)(const TcpAcceptReservation &)>);
static_assert(std::is_same_v<decltype(&TcpStreamOperations::prepare_accept),
    Result<PreparedTcpAccept> (TcpStreamOperations::*)(u64, i32, bool, bool)>);
static_assert(std::is_same_v<decltype(&TcpStreamOperations::begin_accept),
    Result<PreparedTcpAccept> (TcpStreamOperations::*)(u64, i32, bool, bool)>);
static_assert(std::is_same_v<decltype(&PreparedTcpAccept::commit), Result<i32> (PreparedTcpAccept::*)()>);
static_assert(std::is_same_v<decltype(&PreparedTcpAccept::cancel), Status (PreparedTcpAccept::*)()>);
static_assert(std::is_same_v<decltype(&PreparedTcpAccept::live), bool (PreparedTcpAccept::*)() const>);
static_assert(std::is_same_v<decltype(&PreparedTcpAccept::prepare_child), Status (PreparedTcpAccept::*)()>);
} // namespace ok::net
