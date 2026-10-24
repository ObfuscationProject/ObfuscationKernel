#pragma once
#include <ok/sched/resources.hpp>

namespace ok::net { class PreparedTcpAccept; }
namespace ok::sched
{
class SchedulerState;
class PreparedPtyPair;

// Owner storage is stable; process resources are resolved by PID on every use.
// The shared token also identifies the fd installation created by publication.
class DescriptorReservation final
{
  public:
    DescriptorReservation(const DescriptorReservation &) = delete;
    DescriptorReservation &operator=(const DescriptorReservation &) = delete;
    DescriptorReservation &operator=(DescriptorReservation &&) = delete;
    DescriptorReservation(DescriptorReservation &&other) noexcept;
    ~DescriptorReservation();
    [[nodiscard]] i32 descriptor() const { return fd_; }
    [[nodiscard]] u16 description() const { return description_; }
    [[nodiscard]] u64 token() const { return token_; }
    [[nodiscard]] Status validate() const;
    [[nodiscard]] Status cancel();
  private:
    friend class SchedulerState;
    friend class PreparedPtyPair;
class PreparedPtyPair;
    friend class ::ok::net::PreparedTcpAccept;
    DescriptorReservation(SchedulerState &owner, u64 pid, i32 fd, u16 description, u64 token)
        : owner_{&owner}, pid_{pid}, token_{token}, fd_{fd}, description_{description} {}
    // Caller has checked every endpoint/stream/descriptor reservation in the
    // serialized section. This publication has no callback or fallible retain.
    i32 publish_checked(OpenFileDescription description, bool close_on_exec);
    // Failed holder retirement is diagnostic ownership. Keep exact slots and
    // lifetime counters unavailable; destruction must not silently release them.
    void preserve_failed_holder() { owner_ = nullptr; }
    SchedulerState *owner_{};
    u64 pid_{}, token_{};
    i32 fd_{-1};
    u16 description_{invalid_description};
};
}
