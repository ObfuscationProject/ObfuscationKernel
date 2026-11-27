#pragma once

// Bounded repetition for the hosted race models whose checks require every
// interleaving to have occurred. A loaded host can replay one order for a
// long stretch: two host threads time-sliced on one host CPU hand over at the
// same point every round, so one outcome never shows. A model therefore runs
// its usual budget and then keeps racing until each outcome was observed or a
// generous bound is reached. The coverage check itself is unchanged, and the
// model reports the rounds (or time) it took. Extension rounds alternate a
// free race with a nudged one, in which the side that has to arrive late for
// the missing outcome first gives up its host CPU.

#include <chrono>
#include <cstddef>
#include <thread>

namespace ok::test
{

// Round budget of a race: `minimum` rounds, then more while an outcome is
// missing, at most `maximum` in all.
class CoverageRounds
{
  public:
    constexpr CoverageRounds(std::size_t minimum, std::size_t maximum) : minimum_(minimum), maximum_(maximum) {}

    // Starts the next round unless the budget is spent: past the minimum,
    // only while `covered` is false.
    bool next(bool covered)
    {
        if (rounds_ >= maximum_ || (rounds_ >= minimum_ && covered)) return false;
        ++rounds_;
        return true;
    }
    // The current round is an extension that nudges the late side.
    bool nudged() const { return rounds_ > minimum_ && (rounds_ - minimum_) % 2 == 1; }
    std::size_t rounds() const { return rounds_; }
    std::size_t minimum() const { return minimum_; }

  private:
    std::size_t minimum_, maximum_, rounds_{0};
};

// Gives up the host CPU long enough for the other side of a race to run.
inline void nudge() { std::this_thread::sleep_for(std::chrono::microseconds(100)); }

// Free-running racers: lets them run for `minimum`, then until `covered()`
// holds or `maximum` has passed. Returns how long they ran, in milliseconds.
template <typename Covered>
std::size_t run_until_covered(std::chrono::milliseconds minimum, std::chrono::milliseconds maximum, Covered covered)
{
    const auto start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(minimum);
    while (!covered() && std::chrono::steady_clock::now() - start < maximum)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}

} // namespace ok::test
