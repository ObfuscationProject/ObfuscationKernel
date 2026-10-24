#pragma once

#include "ok/core/types.hpp"

namespace ok::sched
{

// Trusted completion cause, independent of the lossy packed wait status.
// This is value storage only; constructing it neither exits nor reaps a task.
class ChildExitStatus
{
  public:
    constexpr ChildExitStatus() = default;
    [[nodiscard]] static constexpr ChildExitStatus normal(i32 status) noexcept
    {
        return ChildExitStatus{1, static_cast<u32>(status) & 0xffU};
    }
    [[nodiscard]] static constexpr ChildExitStatus signaled(u32 signal) noexcept
    {
        return signal && signal <= 128 ? ChildExitStatus{2, signal} : ChildExitStatus{};
    }
    [[nodiscard]] constexpr bool valid(u32 maximum_signal) const noexcept
    {
        return (maximum_signal == 64 || maximum_signal == 128) &&
            ((code_ == 1 && status_ <= 255) || (code_ == 2 && status_ && status_ <= maximum_signal));
    }
    [[nodiscard]] constexpr i32 code() const noexcept { return code_; }
    [[nodiscard]] constexpr i32 status() const noexcept { return static_cast<i32>(status_); }
    constexpr bool operator==(const ChildExitStatus &) const = default;

  private:
    constexpr ChildExitStatus(i32 code, u32 status) : code_(code), status_(status) {}
    i32 code_{};
    u32 status_{};
};

} // namespace ok::sched
