#pragma once

#include "ok/sched/signal_mask.hpp"
#include "ok/sched/child_exit.hpp"
#include <array>

namespace ok::sched
{

// Owned sender, zero-payload SI_KERNEL or trusted child-completion payload.
// Child status is separate from the packed wait word. No pointers are retained.
struct PendingSignalInfo
{
    u32 signal{};
    i32 code{};
    u32 sender_pid{};
    u32 sender_uid{};
    i32 child_status{};
    constexpr bool operator==(const PendingSignalInfo &) const = default;

    [[nodiscard]] constexpr bool valid(u32 maximum_signal) const noexcept
    {
        if ((maximum_signal != 64 && maximum_signal != 128) || !signal || signal > maximum_signal)
            return false;
        if (code == 0 || code == -6) return child_status == 0;
        if (code == 128) return !sender_pid && !sender_uid && !child_status;
        if (signal != (maximum_signal == 128 ? 18U : 17U) || !sender_pid || sender_pid > 0x7fffffffU)
            return false;
        return (code == 1 && child_status >= 0 && child_status <= 255) ||
            (code == 2 && child_status > 0 && static_cast<u32>(child_status) <= maximum_signal);
    }
};

template <u32 MaximumSignal, usize RealtimeCapacity = 32, u64 MaximumSequence = ~u64{0}>
class PendingSignals
{
    static_assert(MaximumSignal == 64 || MaximumSignal == 128);
    static_assert(RealtimeCapacity > 0);
    static_assert(MaximumSequence > 0);
  public:
    constexpr PendingSignals() = default;
    PendingSignals(const PendingSignals &) = delete;
    PendingSignals &operator=(const PendingSignals &) = delete;
    PendingSignals(PendingSignals &&) = delete;
    PendingSignals &operator=(PendingSignals &&) = delete;

    struct Identity
    {
        u64 process{};
        u64 thread{};
        u64 generation{};
        constexpr bool operator==(const Identity &) const = default;
    };
    struct Delivery
    {
        Identity owner{};
        u64 sequence{};
        PendingSignalInfo info{};
        const PendingSignals *queue{};
        constexpr bool operator==(const Delivery &) const = default;
    };
    enum class Enqueue : u8 { queued, coalesced, full, invalid };

    // Caller serializes access and supplies a non-reused task lifetime. Keep
    // this object at a stable address; fork receives an empty, separate queue.
    // Any reset drains work but never reuses ticket numbers, even after an
    // intervening owner. Exhaustion is permanent for this object's lifetime.
    [[nodiscard]] constexpr bool reset(Identity identity) noexcept
    {
        if (!identity.process || !identity.thread || !identity.generation) return false;
        owner_ = identity;
        normal_ = {};
        realtime_ = {};
        normal_mask_ = {};
        realtime_count_ = 0;
        return true;
    }
    constexpr void retire() noexcept
    {
        owner_ = {};
        normal_ = {};
        realtime_ = {};
        normal_mask_ = {};
        realtime_count_ = 0;
    }
    [[nodiscard]] constexpr Enqueue enqueue(PendingSignalInfo info)
    {
        if ((info.code != 0 && info.code != -6) || !info.valid(MaximumSignal)) return Enqueue::invalid;
        return enqueue_checked(info);
    }
    // Kernel callers only; no user-supplied siginfo or sender fields enter here.
    [[nodiscard]] constexpr Enqueue enqueue_kernel(u32 signal)
    {
        return enqueue_checked({signal, 128, 0, 0});
    }
    // Trusted lifecycle callers only. Generic sender enqueue still rejects
    // positive CLD codes; signal number and cause cannot be supplied as siginfo.
    [[nodiscard]] constexpr Enqueue enqueue_child_exit(ChildExitStatus status, u32 child_pid, u32 child_uid)
    {
        if (!status.valid(MaximumSignal)) return Enqueue::invalid;
        const PendingSignalInfo info{MaximumSignal == 128 ? 18U : 17U, status.code(),
                                     child_pid, child_uid, status.status()};
        return info.valid(MaximumSignal) ? enqueue_checked(info) : Enqueue::invalid;
    }

  private:
    [[nodiscard]] constexpr Enqueue enqueue_checked(PendingSignalInfo info)
    {
        if (!owner_.process || !info.signal || info.signal > MaximumSignal) return Enqueue::invalid;
        if (info.signal < 32 && normal_mask_.contains(info.signal)) return Enqueue::coalesced;
        if ((info.signal >= 32 && realtime_count_ == RealtimeCapacity) || next_sequence_ == MaximumSequence)
            return Enqueue::full;
        Entry entry{++next_sequence_, info};
        if (info.signal < 32)
        {
            normal_[info.signal - 1] = entry;
            normal_mask_.set(info.signal);
        }
        else realtime_[realtime_count_++] = entry;
        return Enqueue::queued;
    }

  public:
    [[nodiscard]] constexpr bool peek(const SignalMask &blocked, Delivery &out) const
    {
        // Linux next_signal prioritizes these signal numbers even when their
        // pending payload came from SI_USER/SI_TKILL. MIPS has BUS=10/SYS=12.
        constexpr u64 synchronous = (u64{1} << 3) | (u64{1} << 4) | (u64{1} << 7) |
            (u64{1} << 10) | (u64{1} << (MaximumSignal == 128 ? 9 : 6)) |
            (u64{1} << (MaximumSignal == 128 ? 11 : 30));
        auto eligible = normal_mask_.words[0] & ~blocked.words[0];
        if (eligible & synchronous) eligible &= synchronous;
        for (u32 signal = 1; signal < 32; ++signal)
            if (eligible & (u64{1} << (signal - 1)))
            {
                const auto &entry = normal_[signal - 1];
                out = {owner_, entry.sequence, entry.info, this};
                return true;
            }
        usize chosen = realtime_count_;
        for (usize i = 0; i < realtime_count_; ++i)
            if (!blocked.contains(realtime_[i].info.signal) &&
                (chosen == realtime_count_ || realtime_[i].info.signal < realtime_[chosen].info.signal))
                chosen = i;
        if (chosen == realtime_count_) return false;
        const auto &entry = realtime_[chosen];
        out = {owner_, entry.sequence, entry.info, this};
        return true;
    }
    // Commit only after frame construction succeeds. Removal elsewhere cannot
    // make a stale ticket consume its replacement or a different queue owner.
    [[nodiscard]] constexpr bool consume(const Delivery &ticket)
    {
        if (ticket.queue != this || ticket.owner != owner_ || !ticket.sequence ||
            !ticket.info.signal || ticket.info.signal > MaximumSignal)
            return false;
        if (ticket.info.signal < 32)
        {
            auto &entry = normal_[ticket.info.signal - 1];
            if (!normal_mask_.contains(ticket.info.signal) || !matches(entry, ticket)) return false;
            normal_mask_.set(ticket.info.signal, false);
            entry = {};
            return true;
        }
        for (usize i = 0; i < realtime_count_; ++i)
            if (matches(realtime_[i], ticket))
            {
                erase_realtime(i);
                return true;
            }
        return false;
    }
    constexpr void discard(u32 signal) noexcept
    {
        if (!signal || signal > MaximumSignal) return;
        if (signal < 32)
        {
            normal_mask_.set(signal, false);
            normal_[signal - 1] = {};
        }
        else
            for (usize i = 0; i < realtime_count_;)
                if (realtime_[i].info.signal == signal) erase_realtime(i); else ++i;
    }
    [[nodiscard]] constexpr SignalMask pending_mask() const
    {
        auto mask = normal_mask_;
        for (usize i = 0; i < realtime_count_; ++i) mask.set(realtime_[i].info.signal);
        return mask;
    }
    [[nodiscard]] constexpr usize realtime_size() const { return realtime_count_; }
    [[nodiscard]] constexpr Identity identity() const { return owner_; }

  private:
    // Queue identity belongs to an external ticket, not every stored signal.
    // No ticket points into these arrays; compaction never invalidates a ticket.
    struct Entry
    {
        u64 sequence{};
        PendingSignalInfo info{};
    };
    static_assert(sizeof(PendingSignalInfo) == 20);
    static_assert(sizeof(Entry) == (alignof(u64) == 8 ? 32 : 28));
    [[nodiscard]] static constexpr bool matches(const Entry &entry, const Delivery &ticket)
    {
        return entry.sequence == ticket.sequence && entry.info == ticket.info;
    }
    constexpr void erase_realtime(usize at)
    {
        for (usize i = at + 1; i < realtime_count_; ++i) realtime_[i - 1] = realtime_[i];
        realtime_[--realtime_count_] = {};
    }
    Identity owner_{};
    u64 next_sequence_{};
    SignalMask normal_mask_{};
    std::array<Entry, 31> normal_{};
    std::array<Entry, RealtimeCapacity> realtime_{};
    usize realtime_count_{};
};

} // namespace ok::sched
