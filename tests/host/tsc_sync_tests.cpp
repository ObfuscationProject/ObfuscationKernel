#include "../../src/arch/x86_64/tsc_sync.hpp"

#include <cassert>
#include <cstdio>

using ok::i64;
using ok::u32;
using ok::u64;
namespace tsc = ok::arch::x86::tsc;

namespace
{

// A deterministic generator, so a failing seed reproduces.
struct Random
{
    u64 state;
    u64 next()
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
    u64 below(u64 bound)
    {
        return bound == 0 ? 0 : next() % bound;
    }
};

// Two CPUs in boot-CPU cycles. The target's clock reads its TSC (boot TSC +
// skew + drift + whatever TSC_ADJUST added) minus its software offset. Each
// message takes a random delay; `outbound` and `inbound` add a fixed one-way
// cost, and `wide_rounds` rounds first take `stall` more each.
struct Pair
{
    i64 skew{};
    u64 noise{400};
    u64 outbound{60};
    u64 inbound{60};
    bool has_adjust{};
    bool adjust_takes{true}; // false: the MSR write is accepted but ignored
    bool offset_accepted{true};
    bool adjust_answers{true}; // false: the change takes, the answer is late
    bool offset_answers{true};
    u32 answers{~u32{0}}; // rounds answered before the target goes silent
    u32 wide_rounds{};
    u64 stall{};
    i64 drift_ppm{}; // the target's counter runs this much faster
    Random random{0x9e3779b97f4a7c15};

    u64 start{1'000'000'000'000};
    u64 now{start};
    i64 adjusted{};
    i64 software{};
    u32 rounds{};
    u32 adjust_calls{};
    u32 offset_calls{};

    [[nodiscard]] i64 residual() const
    {
        return skew + static_cast<i64>(now - start) * drift_ppm / 1'000'000 + adjusted - software;
    }
    bool round(tsc::Round &round)
    {
        if (rounds == answers)
            return false;
        const u64 extra = rounds < wide_rounds ? stall : 0;
        ++rounds;
        round.before = now;
        now += outbound + random.below(noise) + extra / 2;
        round.remote = now + static_cast<u64>(residual());
        now += inbound + random.below(noise) + extra - extra / 2;
        round.after = now;
        now += random.below(noise);
        return true;
    }
    [[nodiscard]] bool can_adjust() const
    {
        return has_adjust;
    }
    tsc::Reply adjust(i64 delta)
    {
        ++adjust_calls;
        if (!has_adjust)
            return tsc::Reply::refused;
        if (adjust_takes)
            adjusted += delta;
        return adjust_answers ? tsc::Reply::done : tsc::Reply::silent;
    }
    tsc::Reply add_offset(i64 value)
    {
        ++offset_calls;
        if (!offset_accepted)
            return tsc::Reply::refused;
        software += value;
        return offset_answers ? tsc::Reply::done : tsc::Reply::silent;
    }
};
static_assert(tsc::SyncLink<Pair>);

constexpr tsc::Policy policy{.rounds = 64, .attempts = 4, .slack = 200, .max_error = 20'000};

// After a verified verdict the true residue is within the last estimate's
// error of that estimate, which passed with its own error plus the tolerance:
// the slack and the error of the measurement the correction was based on
// (at most half the widest possible round, and never above max_error).
void require_bounded(const Pair &pair, const tsc::Outcome &outcome)
{
    assert(outcome.verdict != tsc::Verdict::unsynced);
    const u64 widest = (pair.outbound + pair.inbound + 2 * pair.noise) / 2 + 1;
    assert(outcome.initial.error <= widest && outcome.final.error <= widest);
    assert(outcome.final.error <= policy.max_error && outcome.tolerance <= policy.slack + policy.max_error);
    assert(outcome.tolerance <= policy.slack + widest);
    assert(tsc::magnitude(pair.residual()) <= 2 * outcome.final.error + outcome.tolerance);
}

void estimator()
{
    tsc::Estimator estimator;
    assert(!estimator.estimate().valid());
    // A round with its boot-CPU readings reversed is no evidence.
    estimator.add({.before = 100, .remote = 0, .after = 50});
    assert(!estimator.estimate().valid());
    estimator.add({.before = 1000, .remote = 1600, .after = 1200}); // midpoint 1100
    auto estimate = estimator.estimate();
    assert(estimate.valid() && estimate.offset == 500 && estimate.error == 100 && estimate.rounds == 1);
    // A wider round does not replace a tighter one.
    estimator.add({.before = 2000, .remote = 0, .after = 3000});
    estimate = estimator.estimate();
    assert(estimate.offset == 500 && estimate.error == 100 && estimate.rounds == 2);
    // An odd width rounds the bound up, so the truth stays inside it.
    estimator.add({.before = 5000, .remote = 4000, .after = 5003}); // midpoint 5001
    estimate = estimator.estimate();
    assert(estimate.offset == -1001 && estimate.error == 2);
    // The target behind by more than the boot CPU's reading.
    tsc::Estimator behind;
    behind.add({.before = 10, .remote = 3, .after = 10});
    assert(behind.estimate().offset == -7 && behind.estimate().error == 0);

    assert(tsc::within({.offset = 300, .error = 100, .rounds = 1}, 200));
    assert(!tsc::within({.offset = -301, .error = 100, .rounds = 1}, 200));
    assert(!tsc::within({}, ~u64{0}));
    assert(tsc::within({.offset = static_cast<i64>(u64{1} << 63), .error = ~u64{0} - 1, .rounds = 1}, 5));
    assert(tsc::magnitude(static_cast<i64>(u64{1} << 63)) == u64{1} << 63);
}

void names()
{
    assert(tsc::name(tsc::Verdict::sync) == "sync");
    assert(tsc::name(tsc::Verdict::adjusted) == "adjusted");
    assert(tsc::name(tsc::Verdict::offset) == "offset");
    assert(tsc::name(tsc::Verdict::unsynced) == "unsynced");
    assert(tsc::worse(tsc::Verdict::sync, tsc::Verdict::offset) == tsc::Verdict::offset);
    assert(tsc::worse(tsc::Verdict::unsynced, tsc::Verdict::adjusted) == tsc::Verdict::unsynced);
    assert(tsc::worse(tsc::Verdict::adjusted, tsc::Verdict::sync) == tsc::Verdict::adjusted);
}

void decisions()
{
    {
        // In sync: nothing changes, whatever the target supports.
        Pair pair{.has_adjust = true};
        const auto outcome = tsc::synchronize(pair, policy);
        assert(outcome.verdict == tsc::Verdict::sync && pair.adjust_calls == 0 && pair.offset_calls == 0);
        assert(pair.rounds == policy.rounds && outcome.initial.rounds == policy.rounds);
        assert(tsc::magnitude(outcome.initial.offset) <= outcome.initial.error + policy.slack);
    }
    {
        // A skew below the slack is tolerated.
        Pair pair{.skew = -150, .noise = 0};
        assert(tsc::synchronize(pair, policy).verdict == tsc::Verdict::sync);
    }
    {
        // All latency on the way back: the estimate is off by the asymmetry
        // but stays inside its bound, so a synchronized pair is not "fixed".
        Pair pair{.noise = 0, .outbound = 0, .inbound = 900};
        const auto outcome = tsc::synchronize(pair, policy);
        assert(outcome.verdict == tsc::Verdict::sync && outcome.initial.offset == -450);
    }
    {
        // TSC_ADJUST removes the skew.
        Pair pair{.skew = 1'000'000'000, .has_adjust = true};
        const auto outcome = tsc::synchronize(pair, policy);
        assert(outcome.verdict == tsc::Verdict::adjusted && pair.adjust_calls == 1 && pair.offset_calls == 0);
        assert(tsc::magnitude(outcome.adjusted + 1'000'000'000) <= outcome.initial.error);
        require_bounded(pair, outcome);
    }
    {
        // No TSC_ADJUST: the target's clock reads carry the offset.
        Pair pair{.skew = -38'874'658'213};
        const auto outcome = tsc::synchronize(pair, policy);
        assert(outcome.verdict == tsc::Verdict::offset && pair.adjust_calls == 0 && pair.offset_calls == 1);
        assert(outcome.offset == pair.software && outcome.adjusted == 0);
        require_bounded(pair, outcome);
    }
    {
        // TSC_ADJUST advertised but the write does nothing: re-measuring
        // catches it and the software offset takes over.
        Pair pair{.skew = 77'000'000, .has_adjust = true, .adjust_takes = false};
        const auto outcome = tsc::synchronize(pair, policy);
        assert(outcome.verdict == tsc::Verdict::offset && pair.adjust_calls == 1 && pair.offset_calls == 1);
        assert(outcome.adjusted == -outcome.initial.offset && pair.adjusted == 0);
        require_bounded(pair, outcome);
    }
    {
        // The target refuses the adjustment (its own CPUID lacks it).
        Pair pair{.skew = 5'000'000, .has_adjust = false};
        struct Claims : Pair
        {
            bool can_adjust() const { return true; }
        } claims{pair};
        const auto outcome = tsc::synchronize(claims, policy);
        assert(outcome.verdict == tsc::Verdict::offset && claims.adjust_calls == 1 && outcome.adjusted == 0);
        require_bounded(claims, outcome);
    }
    {
        // An offset the target does not take leaves it unsynced.
        Pair pair{.skew = 5'000'000, .offset_accepted = false};
        const auto outcome = tsc::synchronize(pair, policy);
        assert(outcome.verdict == tsc::Verdict::unsynced && outcome.offset == 0 && pair.software == 0);
    }
    {
        // Silent from the start: no estimate, no correction.
        Pair pair{.skew = 5'000'000, .has_adjust = true, .answers = 0};
        const auto outcome = tsc::synchronize(pair, policy);
        assert(outcome.verdict == tsc::Verdict::unsynced && !outcome.initial.valid());
        assert(pair.adjust_calls == 0 && pair.offset_calls == 0);
    }
    {
        // Silent partway through the first measurement: still no estimate.
        Pair pair{.skew = 5'000'000, .answers = 10};
        const auto outcome = tsc::synchronize(pair, policy);
        assert(outcome.verdict == tsc::Verdict::unsynced && !outcome.initial.valid() && pair.offset_calls == 0);
    }
    {
        // Silent while verifying the adjustment: unverified, and no offset.
        Pair pair{.skew = 5'000'000, .has_adjust = true, .answers = policy.rounds + 3};
        const auto outcome = tsc::synchronize(pair, policy);
        assert(outcome.verdict == tsc::Verdict::unsynced && outcome.initial.valid() && !outcome.final.valid());
        assert(outcome.adjusted != 0 && pair.offset_calls == 0);
    }
    {
        // Silent while verifying the offset.
        Pair pair{.skew = 5'000'000, .answers = policy.rounds + 3};
        const auto outcome = tsc::synchronize(pair, policy);
        assert(outcome.verdict == tsc::Verdict::unsynced && outcome.offset != 0);
    }
    {
        // One clean round among outliers is enough.
        Pair pair{.skew = 123'456, .noise = 0, .outbound = 5000, .inbound = 5000};
        struct OneClean : Pair
        {
            bool round(tsc::Round &round)
            {
                const bool answered = Pair::round(round);
                if (answered && rounds == 40)
                {
                    round.before = now;
                    round.remote = now + 3 + static_cast<u64>(residual());
                    round.after = now + 6;
                    now += 10;
                }
                return answered;
            }
        } clean{pair};
        const auto outcome = tsc::synchronize(clean, policy);
        assert(outcome.initial.error == 3 && outcome.initial.offset == 123'456);
        assert(outcome.verdict == tsc::Verdict::offset);
    }
    {
        // An offset of exactly -2^63 cannot be negated for TSC_ADJUST; the
        // software path still takes it.
        struct Extreme
        {
            u64 now{0};
            u64 lag{u64{1} << 63};
            u32 adjust_calls{};
            bool round(tsc::Round &round)
            {
                round = {.before = now, .remote = now + 5 + lag, .after = now + 10};
                now += 20;
                return true;
            }
            bool can_adjust() const { return true; }
            tsc::Reply adjust(i64)
            {
                ++adjust_calls;
                return tsc::Reply::done;
            }
            tsc::Reply add_offset(i64 value)
            {
                lag -= static_cast<u64>(value);
                return tsc::Reply::done;
            }
        } extreme;
        const auto outcome = tsc::synchronize(extreme, policy);
        assert(extreme.adjust_calls == 0 && outcome.verdict == tsc::Verdict::offset && extreme.lag == 0);
    }
    {
        // The adjustment takes but its answer comes too late: the boot CPU
        // cannot tell whether it took, so it sends nothing more (in
        // particular no software offset on top) and measures no more.
        Pair pair{.skew = 5'000'000, .has_adjust = true, .adjust_answers = false};
        const auto outcome = tsc::synchronize(pair, policy);
        assert(outcome.verdict == tsc::Verdict::unsynced && outcome.adjusted == -outcome.initial.offset);
        assert(pair.adjust_calls == 1 && pair.offset_calls == 0 && pair.rounds == policy.rounds);
    }
    {
        // The same for a late software offset.
        Pair pair{.skew = 5'000'000, .offset_answers = false};
        const auto outcome = tsc::synchronize(pair, policy);
        assert(outcome.verdict == tsc::Verdict::unsynced && pair.offset_calls == 1 && pair.rounds == policy.rounds);
    }
    {
        // A late answer to an adjustment that did not take: still no second
        // correction.
        Pair pair{.skew = -9'000'000, .has_adjust = true, .adjust_takes = false, .adjust_answers = false};
        const auto outcome = tsc::synchronize(pair, policy);
        assert(outcome.verdict == tsc::Verdict::unsynced && pair.offset_calls == 0 && pair.software == 0);
    }
    {
        // Every round wider than max_error: an offset that hides inside the
        // bound is not "sync", and nothing is corrected.
        Pair pair{.skew = 15'000, .noise = 0, .wide_rounds = ~u32{0}, .stall = 2 * policy.max_error + 2};
        const auto outcome = tsc::synchronize(pair, policy);
        assert(tsc::within(outcome.initial, policy.slack) && outcome.initial.error > policy.max_error);
        assert(outcome.verdict == tsc::Verdict::unsynced && pair.adjust_calls == 0 && pair.offset_calls == 0);
        assert(pair.rounds == policy.rounds * policy.attempts);
    }
    {
        // Wide rounds for the first two measurements only: the third is tight
        // enough and decides.
        Pair pair{.noise = 0, .wide_rounds = 2 * policy.rounds, .stall = 2 * policy.max_error + 2};
        const auto outcome = tsc::synchronize(pair, policy);
        assert(outcome.verdict == tsc::Verdict::sync && pair.rounds == 3 * policy.rounds);
        assert(outcome.initial.error <= policy.max_error && outcome.initial.rounds == 3 * policy.rounds);
    }
    {
        // A tight first measurement, then the target stalls on every round:
        // the correction cannot be verified.
        Pair pair{.skew = 5'000'000, .noise = 0, .stall = 2 * policy.max_error + 2};
        struct LateStall : Pair
        {
            bool round(tsc::Round &round)
            {
                wide_rounds = rounds >= policy.rounds ? ~u32{0} : 0;
                return Pair::round(round);
            }
        } late{pair};
        const auto outcome = tsc::synchronize(late, policy);
        assert(outcome.verdict == tsc::Verdict::unsynced && late.offset_calls == 1);
        assert(outcome.final.valid() && outcome.final.error > policy.max_error);
    }
    {
        // A slow rate difference is corrected like a skew...
        Pair pair{.skew = 1'000'000, .drift_ppm = 1};
        const auto outcome = tsc::synchronize(pair, policy);
        assert(outcome.verdict == tsc::Verdict::offset);
        require_bounded(pair, outcome);
    }
    {
        // ...but a counter that runs 5% fast moves away faster than either
        // correction can be verified.
        Pair pair{.skew = 1'000'000, .has_adjust = true, .drift_ppm = 50'000};
        const auto outcome = tsc::synchronize(pair, policy);
        assert(outcome.verdict == tsc::Verdict::unsynced && pair.adjust_calls == 1 && pair.offset_calls == 1);
    }
}

// Skews across the range, noise and asymmetry, each target with or without a
// working TSC_ADJUST: every outcome is verified and bounded.
void sweep()
{
    Random random{0x2545f4914f6cdd1d};
    u32 counts[4]{};
    u32 marginal = 0;
    for (u32 trial = 0; trial < 4000; ++trial)
    {
        Pair pair{};
        const unsigned magnitude_bits = static_cast<unsigned>(random.below(51));
        const u64 size = random.below((u64{1} << magnitude_bits) + 1);
        pair.skew = (random.next() & 1) != 0 ? -static_cast<i64>(size) : static_cast<i64>(size);
        pair.noise = random.below(5000) + 1;
        pair.outbound = random.below(400);
        pair.inbound = random.below(400);
        pair.has_adjust = (random.next() & 1) != 0;
        pair.adjust_takes = random.below(4) != 0;
        pair.random.state = random.next() | 1;
        const auto outcome = tsc::synchronize(pair, policy);
        ++counts[static_cast<unsigned>(outcome.verdict)];
        require_bounded(pair, outcome);
        // An ignored adjustment can still verify when the skew was marginal:
        // require_bounded above holds the skew itself to the bound then.
        if (outcome.verdict == tsc::Verdict::adjusted)
            assert(pair.has_adjust && pair.software == 0);
        if (outcome.verdict == tsc::Verdict::adjusted && !pair.adjust_takes)
            ++marginal;
        if (outcome.verdict == tsc::Verdict::sync)
            assert(pair.adjusted == 0 && pair.software == 0);
    }
    assert(counts[0] && counts[1] && counts[2] && counts[3] == 0);
    std::printf("tsc sweep: sync=%u adjusted=%u offset=%u unsynced=%u marginal=%u\n", counts[0], counts[1], counts[2],
                counts[3], marginal);
}

} // namespace

int main()
{
    estimator();
    names();
    decisions();
    sweep();
    std::puts("OK_TSC_SYNC scope=model estimator=pass decisions=pass sweep=pass");
}
