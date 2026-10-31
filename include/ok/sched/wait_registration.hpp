#pragma once

#include "ok/core/types.hpp"

namespace ok::sched
{

struct WaitTicket
{
    u64 owner{};
    u64 thread{};
    u64 generation{};

    [[nodiscard]] constexpr bool operator==(const WaitTicket &) const = default;
};

enum class WaitCompletion : u8
{
    none,
    ready,
    timeout,
    cancelled,
    interrupted,
};

// What made a wait ready: another thread's state change, a deadline, a device
// event the kernel served after its interrupt, or one it found by polling a
// source that has no bound interrupt line.
enum class WakeSource : u8
{
    thread,
    timer,
    interrupt,
    poll,
};
inline constexpr usize wake_source_count = 4;

// A single authoritative record belongs to one scheduler thread. The caller
// serializes arm/complete/cancel on the boot CPU; this class supplies no IRQ or
// SMP synchronization. Exclusive relocation keeps tickets valid; never duplicate
// the record into another authoritative owner or reset it while tickets survive.
// Do not reuse owner/thread identity before every external reference is drained.
class WaitRegistration final
{
  public:
    Result<WaitTicket> arm(u64 owner, u64 thread)
    {
        if (owner == 0 || thread == 0)
            return Status::invalid_argument("wait requires process and thread identities");
        if (active_)
            return Status::busy("thread already owns a wait registration");
        if (ticket_.generation == ~u64{0})
            return Status::overflow("wait registration generation exhausted");
        ticket_ = {owner, thread, ticket_.generation + 1};
        completion_ = WaitCompletion::none;
        source_ = WakeSource::thread;
        active_ = true;
        return ticket_;
    }

    [[nodiscard]] bool pending(WaitTicket ticket) const
    {
        return active_ && ticket == ticket_;
    }

    // A timeout is always a timer wake, whatever `source` the caller names.
    bool complete(WaitTicket ticket, WaitCompletion completion, WakeSource source = WakeSource::thread)
    {
        if (!pending(ticket) ||
            (completion != WaitCompletion::ready && completion != WaitCompletion::timeout &&
             completion != WaitCompletion::cancelled && completion != WaitCompletion::interrupted))
            return false;
        completion_ = completion;
        source_ = completion == WaitCompletion::timeout ? WakeSource::timer : source;
        active_ = false;
        return true;
    }

    bool cancel()
    {
        return complete(ticket_, WaitCompletion::cancelled);
    }

    [[nodiscard]] WaitTicket ticket() const { return ticket_; }
    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] WaitCompletion completion() const { return completion_; }
    // Meaningful once the wait completed; reset by the next arm().
    [[nodiscard]] WakeSource source() const { return source_; }

  private:
    // Test-only fault injection reaches the otherwise impractical u64 boundary;
    // production has no generation reset or restoration operation.
    friend struct WaitRegistrationTestAccess;
    WaitTicket ticket_{};
    WaitCompletion completion_{WaitCompletion::none};
    WakeSource source_{WakeSource::thread};
    bool active_{};
};

} // namespace ok::sched
