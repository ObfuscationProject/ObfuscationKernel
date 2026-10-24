#pragma once

#include "ok/sched/descriptor_reservation.hpp"
#include <array>
#include <optional>

namespace ok::sched
{
class SchedulerState;

// Private candidate only. Both existing scheduler reservations own their slots;
// no fd or OFD is published until the serialized caller validates and commits.
class PreparedPtyPair final
{
  public:
    PreparedPtyPair(const PreparedPtyPair &) = delete;
    PreparedPtyPair &operator=(const PreparedPtyPair &) = delete;
    PreparedPtyPair &operator=(PreparedPtyPair &&) = delete;
    PreparedPtyPair(PreparedPtyPair &&other) noexcept;
    ~PreparedPtyPair();
    [[nodiscard]] std::array<i32, 2> descriptors() const;
    [[nodiscard]] Status validate() const;
    [[nodiscard]] Status cancel();
    // Requires a successful validate with no intervening serialized mutation.
    // No callback, allocation, reference increment or fallible provider call.
    void publish_checked() noexcept;
  private:
    friend class SchedulerState;
    PreparedPtyPair(SchedulerState &owner, DescriptorReservation first,
        DescriptorReservation second, private_pty::PairHandles handles, u32 flags);
    void preserve_failed_holder();
    SchedulerState *owner_{};
    std::optional<DescriptorReservation> first_{}, second_{};
    private_pty::PairHandles handles_{};
    u32 flags_{};
};
}
