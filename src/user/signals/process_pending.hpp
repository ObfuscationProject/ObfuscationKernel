#pragma once

#include "ok/sched/pending_signals.hpp"

namespace ok::user::native
{
// Stable, serialized process owner. The second identity key is a group owner
// tag, not a recipient TID. No queue storage is embedded in compacted tasks.
template <u32 MaximumSignal, usize RealtimeCapacity = 32, u64 MaximumSequence = ~u64{0}>
class ProcessPendingSignals
{
public:
    using Queue = sched::PendingSignals<MaximumSignal, RealtimeCapacity, MaximumSequence>;
    using Identity = typename Queue::Identity;
    enum class Enqueue : u8 { queued, coalesced, invalid, exhausted };
    struct Delivery
    {
        sched::PendingSignalInfo info{};
        Identity owner{};
        typename Queue::Delivery allocated{};
        const ProcessPendingSignals *queue{};
        u64 fallback{};
    };
    [[nodiscard]] bool reset(Identity identity)
    {
        if (identity.process != identity.thread || !queue_.reset(identity)) return false;
        owner_ = identity;
        fallback_ = {};
        return true;
    }
    void retire()
    {
        queue_.retire();
        fallback_ = {};
        owner_ = {};
    }
    [[nodiscard]] Identity identity() const { return owner_; }
    [[nodiscard]] Enqueue enqueue(sched::PendingSignalInfo info)
    {
        if (!owner_.process || info.code != 0 || !info.valid(MaximumSignal))
            return Enqueue::invalid;
        const auto result = queue_.enqueue(info);
        if (result == Queue::Enqueue::queued || result == Queue::Enqueue::coalesced)
        {
            // Linux's pending bit is subsumed by an allocated same-number
            // record; it is not an additional occurrence behind that record.
            if (info.signal >= 32) fallback_[info.signal - 32] = 0;
            return result == Queue::Enqueue::queued ? Enqueue::queued : Enqueue::coalesced;
        }
        if (result != Queue::Enqueue::full) return Enqueue::invalid;
        if (info.signal < 32) return Enqueue::exhausted;
        if (pending_mask().contains(info.signal)) return Enqueue::coalesced;
        if (next_fallback_ == MaximumSequence) return Enqueue::exhausted;
        fallback_[info.signal - 32] = ++next_fallback_;
        return Enqueue::queued;
    }
    // Automatic lifecycle events never use the lossy realtime fallback.
    [[nodiscard]] Enqueue enqueue_child_exit(sched::ChildExitStatus status, u32 child_pid, u32 child_uid)
    {
        if (!owner_.process) return Enqueue::invalid;
        const auto result = queue_.enqueue_child_exit(status, child_pid, child_uid);
        if (result == Queue::Enqueue::queued) return Enqueue::queued;
        if (result == Queue::Enqueue::coalesced) return Enqueue::coalesced;
        return result == Queue::Enqueue::full ? Enqueue::exhausted : Enqueue::invalid;
    }
    // Timer expiry never uses the lossy realtime fallback either: a full
    // queue leaves the expiry owed to the timer owner.
    [[nodiscard]] typename Queue::Enqueue enqueue_kernel(u32 signal)
    {
        return owner_.process ? queue_.enqueue_kernel(signal) : Queue::Enqueue::invalid;
    }
    [[nodiscard]] typename Queue::Enqueue enqueue_timer(u32 signal, i32 timer, i32 overrun, u64 value)
    {
        return owner_.process ? queue_.enqueue_timer(signal, timer, overrun, value) : Queue::Enqueue::invalid;
    }
    [[nodiscard]] bool timer_queued(u32 signal, i32 timer) const { return queue_.timer_queued(signal, timer); }
    bool refresh_timer(u32 signal, i32 timer, i32 overrun) { return queue_.refresh_timer(signal, timer, overrun); }
    void discard_timer(u32 signal, i32 timer) { queue_.discard_timer(signal, timer); }
    void discard_timers() { queue_.discard_timers(); }
    [[nodiscard]] bool peek(const sched::SignalMask &blocked, Delivery &out) const
    {
        if (!owner_.process) return false;
        typename Queue::Delivery allocated;
        const bool found = queue_.peek(blocked, allocated);
        u32 fallback = 0;
        for (u32 signal = 32; signal <= MaximumSignal; ++signal)
            if (fallback_[signal - 32] && !blocked.contains(signal)) { fallback = signal; break; }
        if (found && (!fallback || allocated.info.signal <= fallback))
        {
            out = {allocated.info, owner_, allocated, this, 0};
            return true;
        }
        if (!fallback) return false;
        out = {{fallback, 0, 0, 0}, owner_, {}, this, fallback_[fallback - 32]};
        return true;
    }
    [[nodiscard]] bool consume(const Delivery &ticket)
    {
        if (!owner_.process || ticket.queue != this || ticket.owner != owner_) return false;
        if (!ticket.fallback)
            return ticket.info == ticket.allocated.info && queue_.consume(ticket.allocated);
        const auto signal = ticket.info.signal;
        if (signal < 32 || signal > MaximumSignal || ticket.info != sched::PendingSignalInfo{signal, 0, 0, 0} ||
            fallback_[signal - 32] != ticket.fallback) return false;
        fallback_[signal - 32] = 0;
        return true;
    }
    void discard(u32 signal) noexcept
    {
        queue_.discard(signal);
        if (signal >= 32 && signal <= MaximumSignal) fallback_[signal - 32] = 0;
    }
    [[nodiscard]] sched::SignalMask pending_mask() const
    {
        if (!owner_.process) return {};
        auto mask = queue_.pending_mask();
        for (u32 signal = 32; signal <= MaximumSignal; ++signal)
            if (fallback_[signal - 32]) mask.set(signal);
        return mask;
    }
    [[nodiscard]] usize realtime_size() const { return queue_.realtime_size(); }
private:
    Queue queue_{};
    Identity owner_{};
    std::array<u64, MaximumSignal - 31> fallback_{};
    u64 next_fallback_{};
};

// Borrow only within one serialized runtime operation. Tickets retain their
// original queue/owner; changing either owner invalidates the entire view.
template <typename Directed, typename Shared>
class SignalPendingView
{
public:
    struct Delivery
    {
        sched::PendingSignalInfo info{};
        typename Directed::Delivery directed{};
        typename Shared::Delivery shared{};
        bool from_shared{};
    };
    SignalPendingView(Directed *directed, Shared *shared)
        : directed_(directed), shared_(shared),
          directed_owner_(directed ? directed->identity() : typename Directed::Identity{}),
          shared_owner_(shared ? shared->identity() : typename Shared::Identity{}) {}
    [[nodiscard]] explicit operator bool() const
    {
        return directed_ && shared_ && directed_owner_.process &&
               directed_owner_.process == shared_owner_.process &&
               directed_->identity() == directed_owner_ && shared_->identity() == shared_owner_;
    }
    [[nodiscard]] bool peek(const sched::SignalMask &blocked, Delivery &out) const
    {
        if (!*this) return false;
        typename Directed::Delivery directed;
        if (directed_->peek(blocked, directed))
        { out = {directed.info, directed, {}, false}; return true; }
        typename Shared::Delivery shared;
        if (shared_->peek(blocked, shared))
        { out = {shared.info, {}, shared, true}; return true; }
        return false;
    }
    [[nodiscard]] bool consume(const Delivery &ticket)
    {
        if (!*this) return false;
        return ticket.from_shared
            ? ticket.info == ticket.shared.info && shared_->consume(ticket.shared)
            : ticket.info == ticket.directed.info && directed_->consume(ticket.directed);
    }
    [[nodiscard]] sched::SignalMask pending_mask() const
    {
        if (!*this) return {};
        auto result = directed_->pending_mask();
        const auto shared = shared_->pending_mask();
        for (usize i = 0; i < result.words.size(); ++i) result.words[i] |= shared.words[i];
        return result;
    }
private:
    Directed *directed_{};
    Shared *shared_{};
    typename Directed::Identity directed_owner_{};
    typename Shared::Identity shared_owner_{};
};
} // namespace ok::user::native
