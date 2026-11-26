#pragma once

#include "ok/core/types.hpp"

#include <array>
#include <optional>

namespace ok::sched
{
// A boot-CPU wait owns one absolute deadline. Generation tokens make a canceled
// or previously completed registration unusable after the record is rearmed.
class DeadlineWait
{
  public:
    Result<u64> arm(u64 now, i64 seconds, i64 nanoseconds, bool absolute)
    {
        constexpr u64 second = 1000000000;
        if (seconds < 0 || nanoseconds < 0 || nanoseconds >= static_cast<i64>(second))
            return Status::invalid_argument("invalid sleep interval");
        const auto sec = static_cast<u64>(seconds);
        const auto ns = static_cast<u64>(nanoseconds);
        if (sec > (~u64{0} - ns) / second)
            return Status::overflow("sleep interval overflows clock");
        const auto interval = sec * second + ns;
        if (!absolute && interval > ~u64{0} - now)
            return Status::overflow("sleep deadline overflows clock");
        if (armed_)
            return Status::busy("thread already owns a deadline");
        if (generation_ == ~u64{0})
            return Status::overflow("wait generation exhausted");
        deadline_ = absolute ? interval : now + interval;
        armed_ = true;
        return ++generation_;
    }
    bool complete(u64 now, u64 generation)
    {
        if (!armed_ || generation != generation_ || now < deadline_)
            return false;
        armed_ = false;
        return true;
    }
    void cancel()
    {
        armed_ = false;
    }
    [[nodiscard]] bool armed() const { return armed_; }
    [[nodiscard]] u64 generation() const { return generation_; }
    [[nodiscard]] u64 deadline() const { return deadline_; }

  private:
    u64 generation_{};
    u64 deadline_{};
    bool armed_{};
};

// An indexed binary min-heap of absolute deadlines over a fixed set of owner
// keys (smp-scaling WP4b, spec 4.6). Each key holds at most one deadline, so
// the heap cannot overflow; set, remove and pop are O(log Keys). `pos_` holds
// a key's heap position plus one, zero when the key is absent, so the empty
// heap is all zero bytes. The caller serializes every operation (the
// scheduler's deadline lock).
template <usize Keys> class DeadlineHeap
{
    static_assert(Keys != 0 && Keys < 0xffff, "keys and positions are u16");

  public:
    static constexpr u64 never = ~u64{0};

    [[nodiscard]] static constexpr usize capacity() { return Keys; }
    [[nodiscard]] usize size() const { return count_; }
    [[nodiscard]] bool empty() const { return count_ == 0; }
    [[nodiscard]] bool contains(u16 key) const { return key < Keys && pos_[key] != 0; }
    [[nodiscard]] u64 when(u16 key) const { return contains(key) ? due_[key] : never; }
    [[nodiscard]] u64 earliest() const { return count_ ? due_[heap_[0]] : never; }

    // Inserts `key` at `deadline`, or moves it there.
    bool set(u16 key, u64 deadline)
    {
        if (key >= Keys) return false;
        if (pos_[key] == 0)
        {
            heap_[count_] = key;
            pos_[key] = static_cast<u16>(++count_);
            due_[key] = deadline;
            up(count_ - 1);
            return true;
        }
        const u64 before = due_[key];
        due_[key] = deadline;
        const usize at = pos_[key] - 1u;
        if (deadline < before) up(at);
        else down(at);
        return true;
    }
    bool remove(u16 key)
    {
        if (!contains(key)) return false;
        const usize at = pos_[key] - 1u;
        const usize last = --count_;
        pos_[key] = 0;
        if (at != last)
        {
            place(at, heap_[last]);
            if (up(at) == at) down(at);
        }
        return true;
    }
    // The earliest key whose deadline is at or before `now`, removed.
    std::optional<u16> pop_due(u64 now)
    {
        if (!count_ || due_[heap_[0]] > now) return std::nullopt;
        const u16 key = heap_[0];
        remove(key);
        return key;
    }
    // Heap order and the position index agree (tests and audits).
    [[nodiscard]] bool consistent() const
    {
        usize present = 0;
        for (usize key = 0; key < Keys; ++key)
            if (pos_[key])
            {
                ++present;
                if (pos_[key] > count_ || heap_[pos_[key] - 1u] != key) return false;
            }
        if (present != count_) return false;
        for (usize at = 1; at < count_; ++at)
            if (due_[heap_[(at - 1) / 2]] > due_[heap_[at]]) return false;
        return true;
    }

  private:
    void place(usize at, u16 key)
    {
        heap_[at] = key;
        pos_[key] = static_cast<u16>(at + 1);
    }
    usize up(usize at)
    {
        const u16 key = heap_[at];
        while (at != 0)
        {
            const usize parent = (at - 1) / 2;
            if (due_[heap_[parent]] <= due_[key]) break;
            place(at, heap_[parent]);
            at = parent;
        }
        place(at, key);
        return at;
    }
    void down(usize at)
    {
        const u16 key = heap_[at];
        for (;;)
        {
            const usize left = 2 * at + 1;
            if (left >= count_) break;
            usize child = left;
            if (left + 1 < count_ && due_[heap_[left + 1]] < due_[heap_[left]]) child = left + 1;
            if (due_[heap_[child]] >= due_[key]) break;
            place(at, heap_[child]);
            at = child;
        }
        place(at, key);
    }

    std::array<u64, Keys> due_{};
    std::array<u16, Keys> heap_{};
    std::array<u16, Keys> pos_{};
    usize count_{};
};
} // namespace ok::sched
