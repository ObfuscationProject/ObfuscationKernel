#pragma once

#include "ok/sched/signal_mask.hpp"
#include <array>

namespace ok::sched
{

// Numeric guest addresses, never retained kernel pointers. The native runtime
// owns one disposition table per serialized Linux thread group.
struct SignalAction
{
    static constexpr u64 default_handler = 0;
    static constexpr u64 ignored_handler = 1;
    u64 handler{};
    u64 flags{};
    u64 restorer{};
    SignalMask mask{};
    constexpr bool operator==(const SignalAction &) const = default;
};

// One value-owned disposition table per process/thread group. Same-group
// threads refer to their owner's table; fork takes a value snapshot. Separate
// groups sharing CLONE_SIGHAND require an explicit retained owner in a later
// integration and are not implemented by copying this table.
class SignalActionTable
{
  public:
    static constexpr usize capacity = 128;
    static constexpr usize storage_bytes = capacity * sizeof(SignalAction);

    [[nodiscard]] constexpr SignalAction *find(u32 signal)
    {
        return signal >= 1 && signal <= capacity ? &actions_[signal - 1] : nullptr;
    }
    [[nodiscard]] constexpr const SignalAction *find(u32 signal) const
    {
        return signal >= 1 && signal <= capacity ? &actions_[signal - 1] : nullptr;
    }
    [[nodiscard]] constexpr SignalActionTable fork_copy() const { return *this; }

    // Linux flush_signal_handlers: even preserved SIG_IGN loses all other
    // action fields. Pending queues and the caller's blocked mask are separate
    // owners and must not be reset by this disposition-only operation.
    constexpr void reset_for_exec(bool force_default = false)
    {
        for (auto &action : actions_)
        {
            const bool ignored = !force_default && action.handler == SignalAction::ignored_handler;
            action = {};
            if (ignored) action.handler = SignalAction::ignored_handler;
        }
    }
    constexpr bool operator==(const SignalActionTable &) const = default;

  private:
    std::array<SignalAction, capacity> actions_{};
};

static_assert(sizeof(SignalAction) == 40);
static_assert(sizeof(SignalActionTable) == SignalActionTable::storage_bytes);

} // namespace ok::sched
