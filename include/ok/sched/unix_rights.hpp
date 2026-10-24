#pragma once
#include "ok/sched/resources.hpp"
#include <span>

namespace ok::sched
{
class SchedulerState;

// The queue retains its child until successful publication. Cancellation only
// retires this preview and descriptor reservation, never the queued connection.
class UnixAccept final
{
  public:
    UnixAccept(const UnixAccept &) = delete;
    UnixAccept &operator=(const UnixAccept &) = delete;
    UnixAccept &operator=(UnixAccept &&) = delete;
    UnixAccept(UnixAccept &&other) noexcept;
    ~UnixAccept();
    [[nodiscard]] i32 descriptor() const { return fd_; }
    [[nodiscard]] u16 description() const { return child_; }
    [[nodiscard]] UnixPeerCredentials peer_credentials() const { return peer_; }
    [[nodiscard]] Result<i32> commit();
    void cancel();
  private:
    friend class SchedulerState;
    UnixAccept(SchedulerState &owner, ProcessResources &resources, u16 listener,
        u16 child, UnixEndpointHandle endpoint, u64 token, i32 fd,
        bool close_on_exec, u32 status_flags, UnixPeerCredentials peer)
        : owner_(&owner), resources_(&resources), listener_(listener), child_(child),
          endpoint_(endpoint), token_(token), fd_(fd), close_on_exec_(close_on_exec),
          status_flags_(status_flags), peer_(peer) {}
    SchedulerState *owner_{};
    ProcessResources *resources_{};
    u16 listener_{invalid_description}, child_{invalid_description};
    UnixEndpointHandle endpoint_{};
    u64 token_{};
    i32 fd_{-1};
    bool close_on_exec_{};
    u32 status_flags_{};
    UnixPeerCredentials peer_{};
};

// Synchronous boot-CPU operation. The caller keeps ProcessResources alive until
// commit/cancel; no user address, TCB or callback survives this move-only value.
class UnixReceive final
{
  public:
    UnixReceive(const UnixReceive &) = delete;
    UnixReceive &operator=(const UnixReceive &) = delete;
    UnixReceive &operator=(UnixReceive &&) = delete;
    UnixReceive(UnixReceive &&other) noexcept;
    ~UnixReceive();
    [[nodiscard]] usize bytes() const { return bytes_; }
    [[nodiscard]] std::span<const i32> descriptors() const { return {fds_.data(), held_count_}; }
    [[nodiscard]] bool control_truncated() const { return truncated_; }
    [[nodiscard]] Result<usize> commit();
    void cancel();
  private:
    friend class SchedulerState;
    explicit UnixReceive(SchedulerState &owner, ProcessResources &resources, u16 description,
        u16 pipe, u64 revision, u64 token, bool peek, bool close_on_exec);
    SchedulerState *owner_{};
    ProcessResources *resources_{};
    u16 description_{invalid_description}, pipe_{invalid_description}, record_{invalid_description};
    u64 revision_{}, token_{};
    usize bytes_{}, held_count_{};
    bool peek_{}, close_on_exec_{}, truncated_{};
    std::array<u16, max_unix_rights> held_{};
    std::array<i32, max_unix_rights> fds_{};
};
} // namespace ok::sched
