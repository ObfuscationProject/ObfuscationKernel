#pragma once

#include "ok/core/types.hpp"

#include <concepts>
#include <string_view>

// Estimates a starting CPU's TSC offset from the boot CPU's and decides how to
// correct it. Pure: smp.cpp drives it over the two CPUs' handshake, and the
// hosted model test (tests/host/tsc_sync_tests.cpp) over injected skew and noise.
namespace ok::arch::x86::tsc
{

// One ping-pong round: the boot CPU reads `before`, the target answers with
// its own reading `remote`, and the boot CPU reads `after` once it saw the
// answer. The target's reading was taken at some boot-CPU time in
// [before, after].
struct Round
{
    u64 before{};
    u64 remote{};
    u64 after{};
};

// The target's counter minus the boot CPU's: the true offset lies within
// `error` of `offset`. `rounds` counts the rounds it was chosen from.
struct Estimate
{
    i64 offset{};
    u64 error{~u64{0}};
    u32 rounds{};

    [[nodiscard]] constexpr bool valid() const noexcept
    {
        return rounds != 0;
    }
};

[[nodiscard]] constexpr u64 magnitude(i64 value) noexcept
{
    return value < 0 ? u64{0} - static_cast<u64>(value) : static_cast<u64>(value);
}

// Keeps the tightest round: its midpoint is the estimate, half its width the
// bound. A round whose boot-CPU readings run backwards is discarded.
class Estimator
{
  public:
    constexpr void add(const Round &round) noexcept
    {
        if (round.after < round.before)
            return;
        const u64 width = round.after - round.before;
        const u64 error = width - width / 2;
        ++best_.rounds;
        if (error < best_.error)
        {
            best_.error = error;
            best_.offset = static_cast<i64>(round.remote - (round.before + width / 2));
        }
    }

    [[nodiscard]] constexpr Estimate estimate() const noexcept
    {
        return best_;
    }

  private:
    Estimate best_{};
};

// In increasing severity, so the worst CPU names a whole bring-up.
enum class Verdict : u8
{
    sync,     // within the measurement bound; nothing changed
    adjusted, // IA32_TSC_ADJUST moved the target's TSC
    offset,   // the target's clock reads subtract a software offset
    unsynced, // no correction could be verified
};

[[nodiscard]] constexpr std::string_view name(Verdict verdict) noexcept
{
    switch (verdict)
    {
    case Verdict::sync:
        return "sync";
    case Verdict::adjusted:
        return "adjusted";
    case Verdict::offset:
        return "offset";
    case Verdict::unsynced:
        break;
    }
    return "unsynced";
}

[[nodiscard]] constexpr Verdict worse(Verdict a, Verdict b) noexcept
{
    return a < b ? b : a;
}

[[nodiscard]] constexpr u64 saturating_add(u64 a, u64 b) noexcept
{
    return a > ~u64{0} - b ? ~u64{0} : a + b;
}

// No measurable offset: zero lies within the estimate's bound plus `slack`.
[[nodiscard]] constexpr bool within(const Estimate &estimate, u64 slack) noexcept
{
    return estimate.valid() && magnitude(estimate.offset) <= saturating_add(estimate.error, slack);
}

struct Policy
{
    u32 rounds{64};
    u64 slack{}; // cycles of offset tolerated beyond the bound
};

// The boot CPU's side of the channel to the target.
// - round: one ping-pong; false once the target stopped answering.
// - can_adjust: both CPUs have IA32_TSC_ADJUST.
// - adjust(delta): the target adds `delta` to its TSC; false if refused.
// - add_offset(value): the target's clock reads subtract `value` more from
//   then on.
template <typename Link>
concept SyncLink = requires(Link &link, Round &round, i64 value) {
    { link.round(round) } -> std::same_as<bool>;
    { link.can_adjust() } -> std::same_as<bool>;
    { link.adjust(value) } -> std::same_as<bool>;
    { link.add_offset(value) } -> std::same_as<bool>;
};

struct Outcome
{
    Verdict verdict{Verdict::unsynced};
    Estimate initial{}; // before any correction
    Estimate final{};   // the last measurement
    i64 adjusted{};     // added to the target's TSC
    i64 offset{};       // added to what the target's clock reads subtract
};

// Up to `rounds` rounds; invalid if the target stopped answering.
template <SyncLink Link> [[nodiscard]] Estimate measure(Link &link, u32 rounds)
{
    Estimator estimator{};
    for (u32 i = 0; i < rounds; ++i)
    {
        Round round{};
        if (!link.round(round))
            return {};
        estimator.add(round);
    }
    return estimator.estimate();
}

// Measure; if the target is off by more than the bound, move its TSC with
// IA32_TSC_ADJUST, else (or if that did not take) offset its clock reads, and
// verify each correction by measuring again. A correction can leave a residue
// as large as the bound of the measurement it was based on, so verification
// tolerates that bound on top of the slack.
template <SyncLink Link> [[nodiscard]] Outcome synchronize(Link &link, const Policy &policy)
{
    Outcome outcome{};
    outcome.initial = outcome.final = measure(link, policy.rounds);
    if (!outcome.initial.valid())
        return outcome;
    if (within(outcome.initial, policy.slack))
    {
        outcome.verdict = Verdict::sync;
        return outcome;
    }
    constexpr i64 lowest = static_cast<i64>(u64{1} << 63);
    auto basis = outcome.initial;
    if (basis.offset != lowest && link.can_adjust() && link.adjust(-basis.offset))
    {
        outcome.adjusted = -basis.offset;
        outcome.final = measure(link, policy.rounds);
        if (!outcome.final.valid())
            return outcome;
        if (within(outcome.final, saturating_add(policy.slack, basis.error)))
        {
            outcome.verdict = Verdict::adjusted;
            return outcome;
        }
        basis = outcome.final;
    }
    if (!link.add_offset(basis.offset))
        return outcome;
    outcome.offset = basis.offset;
    outcome.final = measure(link, policy.rounds);
    if (within(outcome.final, saturating_add(policy.slack, basis.error)))
        outcome.verdict = Verdict::offset;
    return outcome;
}

} // namespace ok::arch::x86::tsc
