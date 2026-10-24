#pragma once

#include "ok/sched/scheduler.hpp"
#include "ok/abi/linux_profile_data.hpp"
#include <concepts>

namespace ok::user::native { template <abi::linux_user::Profile Selected> class LinuxUdpReceiveCoordinator; }

namespace ok::user::native::linux_udp
{
enum class DescriptionKind : u8 { bad_fd, not_socket, retained_udp };

struct DescriptionSelection
{
    DescriptionKind kind{DescriptionKind::bad_fd};
    u32 endpoint{}, flags{};
};

// The scheduler and failure sink must outlive this local operation guard.
class OriginalUdpDescription final
{
  public:
    OriginalUdpDescription(sched::SchedulerState &scheduler, Status &failure)
        : scheduler_{scheduler}, failure_{failure} {}
    ~OriginalUdpDescription() { static_cast<void>(release()); }
    OriginalUdpDescription(const OriginalUdpDescription &) = delete;
    OriginalUdpDescription &operator=(const OriginalUdpDescription &) = delete;
    OriginalUdpDescription(OriginalUdpDescription &&) = delete;
    OriginalUdpDescription &operator=(OriginalUdpDescription &&) = delete;

    [[nodiscard]] Result<DescriptionSelection> acquire(
        const sched::ProcessControlBlock &process, i32 descriptor)
    {
        return acquire_original(process, descriptor, false);
    }

    // Connect copies the address before reporting a valid non-socket fd.
    // This retained candidate grants no UDP receive admission.
    [[nodiscard]] Result<DescriptionSelection> acquire_connect_candidate(
        const sched::ProcessControlBlock &process, i32 descriptor)
    {
        return acquire_original(process, descriptor, true);
    }

  private:
    [[nodiscard]] Result<DescriptionSelection> acquire_original(
        const sched::ProcessControlBlock &process, i32 descriptor, bool retain_non_socket)
    {
        if (attempted_) return Status::busy("Linux UDP description guard has already been used");
        attempted_ = true;
        if (scheduler_.find(process.pid()) != &process)
            return Status::denied("Linux UDP description process is not canonical");
        const auto *slot = process.fd_table().entry(descriptor);
        const auto *open = slot && slot->used
            ? scheduler_.open_description(slot->description) : nullptr;
        if (!open) return DescriptionSelection{DescriptionKind::bad_fd};
        const bool udp = open->native_udp_handle != 0;
        if (!udp && !retain_non_socket) return DescriptionSelection{DescriptionKind::not_socket};
        // UDP admission requires a pure endpoint. A connect candidate can hold
        // a non-socket OFD, whose final provider release follows access reset.
        if (!open->references || (udp && (open->unix_endpoint || open->native_tcp_handle || open->native_shared_file || open->unix_write_pipe != sched::invalid_description || open->node || open->pipe != sched::invalid_description ||
            open->console || open->input_console)))
            return Status::io_error("Linux UDP description has an unsupported provider shape");
        const u16 selected = slot->description;
        if (auto status = scheduler_.retain_description(selected); !status) return status;
        // Retain does not call a provider for an already descriptor-owned OFD.
        description_ = selected;
        return DescriptionSelection{udp ? DescriptionKind::retained_udp : DescriptionKind::not_socket,
                                    open->native_udp_handle, open->flags};
    }

  public:
    [[nodiscard]] bool held() const { return description_ != sched::invalid_description; }
    [[nodiscard]] Status release()
    {
        const auto selected = std::exchange(description_, sched::invalid_description);
        if (selected == sched::invalid_description) return {};
        const auto status = scheduler_.release_description(selected);
        if (!status && failure_) failure_ = status;
        return status;
    }

  private:
    template <abi::linux_user::Profile Selected> friend class ::ok::user::native::LinuxUdpReceiveCoordinator;
    sched::SchedulerState &scheduler_;
    Status &failure_;
    u16 description_{sched::invalid_description};
    bool attempted_{};
};

template <class Access>
concept ImmediateReset = requires(Access &access) { { access.reset() } -> std::same_as<void>; };

template <ImmediateReset Access> class ImmediateLifetimeScope final
{
  public:
    ImmediateLifetimeScope(Access &access, OriginalUdpDescription &description)
        : access_{access}, description_{description} {}
    ~ImmediateLifetimeScope() { static_cast<void>(finish()); }
    ImmediateLifetimeScope(const ImmediateLifetimeScope &) = delete;
    ImmediateLifetimeScope &operator=(const ImmediateLifetimeScope &) = delete;
    ImmediateLifetimeScope(ImmediateLifetimeScope &&) = delete;
    ImmediateLifetimeScope &operator=(ImmediateLifetimeScope &&) = delete;

    [[nodiscard]] Status finish()
    {
        if (std::exchange(finished_, true)) return {};
        access_.reset();
        return description_.release();
    }

  private:
    Access &access_;
    OriginalUdpDescription &description_;
    bool finished_{};
};
} // namespace ok::user::native::linux_udp
