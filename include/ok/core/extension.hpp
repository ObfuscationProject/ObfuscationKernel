#pragma once

#include "ok/core/module.hpp"
#include "ok/fs/vfs.hpp"
#include "ok/gui/gui.hpp"
#include "ok/net/native_udp.hpp"
#include "ok/net/native_udp_executor.hpp"

namespace ok
{
class Kernel;
enum class ExtensionSurfaceAction : u8
{
    focus,
    refresh,
    close
};

// In-process SDK interface for a statically linked distribution component.
// This is not a stable binary module ABI. Build against the matching exported SDK.
// Callbacks run synchronously on the boot CPU; the owner outlives Kernel.
class KernelExtension
{
  public:
    virtual ~KernelExtension() = default;
    virtual Status self_test(Kernel &)
    {
        return Status::success();
    }
    virtual Status start(Kernel &)
    {
        return Status::success();
    }
    virtual Result<fs::FileBuffer> read_package(Kernel &, std::string_view)
    {
        return Status::not_found("extension package not found");
    }
    virtual Status load_package(Kernel &, std::string_view, const ModuleImageInfo &)
    {
        return Status::unsupported("no prelinked extension accepts this package");
    }
    virtual Result<bool> pointer_click(Kernel &, i32, i32)
    {
        return false;
    }
    virtual Result<bool> key(Kernel &, int)
    {
        return false;
    }
    virtual Status surface_action(Kernel &, gui::SurfaceId, ExtensionSurfaceAction)
    {
        return Status::not_found("surface is not extension-owned");
    }
    virtual Status process_exited(Kernel &, sched::ProcessId)
    {
        return Status::success();
    }
    virtual Status refresh(Kernel &, bool)
    {
        return Status::success();
    }
    // Read-only admission for the exact distribution producer inventory.
    virtual Status shutdown_preflight(Kernel &)
    {
        return Status::unsupported("distribution producer retirement is unqualified");
    }
    // Called once after global admission closes; storage/serial stay alive.
    virtual Status shutdown_stop_producers(Kernel &)
    {
        return Status::unsupported("distribution producer retirement is unqualified");
    }
    // One call transfers one complete Ethernet frame. These coarse runtime
    // device-service boundaries do not parse protocol bytes in the OS driver.
    virtual Result<net::NativeUdpLink> network_link(Kernel &)
    {
        return Status::unsupported("distribution has no native UDP link");
    }
    virtual Result<usize> network_receive(Kernel &, std::span<std::byte>)
    {
        return Status::not_found("distribution has no network frame");
    }
    virtual Status network_transmit(Kernel &, std::span<const std::byte>)
    {
        return Status::unsupported("distribution has no native UDP transmitter");
    }
    // Immediate full-frame copy or would_block. No completion wait is allowed.
    virtual Status network_try_transmit(Kernel &, std::span<const std::byte>)
    {
        return Status::unsupported("distribution has no immediate native UDP transmitter");
    }
    // Reap at most budget completions, then copy the CPU-owned credit ledger.
    virtual Result<net::NativeUdpTransmitState> network_poll_transmit(Kernel &, usize)
    {
        return Status::unsupported("distribution has no native UDP TX reaper");
    }
};
} // namespace ok
