#include "ok/time/epoch.hpp"
#include "ok/time/rtc.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <limits>

using namespace ok;
using namespace ok::time;

namespace
{
struct Bus
{
    std::array<u32, 1024> words{};
    std::array<u32, 1024> trace{};
    usize count{};
    bool tear{}, stop{}, goldfish{};
    u64 goldfish_value{};
    u32 latched{};
    u32 read(u32 offset)
    {
        assert(offset % 4 == 0 && offset < 4096 && count < trace.size());
        trace[count++] = offset;
        if (goldfish && offset == 0)
        {
            latched = static_cast<u32>(goldfish_value >> 32);
            return static_cast<u32>(goldfish_value++);
        }
        if (goldfish && offset == 4) return latched;
        if (tear && offset == 0x2c) return (words[offset / 4] += 16);
        if (stop && count > 9 && offset == 0x0c) return 0;
        return words[offset / 4];
    }
};

Bus pl031()
{
    Bus bus{};
    constexpr u32 id[]{0x31, 0x10, 0x14, 0, 0x0d, 0xf0, 5, 0xb1};
    for (u32 i = 0; i < 8; ++i) bus.words[(0xfe0 + 4 * i) / 4] = id[i];
    bus.words[3] = 1;
    return bus;
}

void registers()
{
    auto arm = pl031();
    arm.words[0] = 0xffffffff;
    auto value = read_pl031(arm);
    assert(value && value.value().time == (EpochTime{4294967295, 0}));
    assert(value.value().resolution_ns == 1000000000 && value.value().source == EpochSource::pl031);
    arm = pl031(); arm.words[0] = 0;
    assert(read_pl031(arm).value().time == EpochTime{});
    arm = pl031(); arm.words[3] = 0;
    assert(read_pl031(arm).status().code() == StatusCode::io_error);
    arm = pl031(); arm.words[0xfe8 / 4] = 0x80;
    assert(read_pl031(arm).status().code() == StatusCode::unsupported);
    arm = pl031(); arm.stop = true;
    assert(read_pl031(arm).status().code() == StatusCode::io_error);

    for (u64 raw : {u64{0}, u64{999999999}, u64{1000000000}, u64{0xffffffff}, ~u64{0}})
    {
        Bus fish{}; fish.goldfish = true; fish.goldfish_value = raw; fish.latched = 0xbad;
        value = read_goldfish(fish);
        assert(value && value.value().time.seconds == static_cast<i64>(raw / 1000000000));
        assert(value.value().time.nanoseconds == raw % 1000000000);
        assert(fish.count == 2 && fish.trace[0] == 0 && fish.trace[1] == 4);
    }

    Bus loong{};
    assert(read_ls7a(loong).status().code() == StatusCode::unsupported);
    loong = {}; loong.words[0x40 / 4] = (1 << 11) | (1 << 8);
    loong.words[0x30 / 4] = 124;
    loong.words[0x2c / 4] = (2u << 26) | (29u << 21) | (12u << 16) | (34u << 10) | (56u << 4) | 15;
    value = read_ls7a(loong);
    assert(value && value.value().time == (EpochTime{1709210096, 0}));
    loong.count = 0; loong.words[0x30 / 4] = 200;
    assert(read_ls7a(loong).status().code() == StatusCode::invalid_argument); // 2100 is not leap.
    loong.count = 0; loong.words[0x30 / 4] = 0xffffffff;
    assert(read_ls7a(loong).status().code() == StatusCode::overflow);
    loong.count = 0; loong.words[0x30 / 4] = 124; loong.tear = true;
    assert(read_ls7a(loong).status().code() == StatusCode::io_error && loong.count == 257);
}

void anchors()
{
    const EpochReading origin{{-1, 500000000}, EpochSource::goldfish, 1};
    EpochAnchor absent;
    assert(absent.read({0, 1}).status().code() == StatusCode::not_initialized);
    EpochAnchor ready;
    assert(ready.initialize(origin, {100, 7}, {150, 7}));
    assert(ready.info().acquisition_span_ns == 50 && ready.info().monotonic_generation == 7);
    assert(ready.info().accuracy == Accuracy::unknown);
    assert(ready.read({500000150, 7}).value() == EpochTime{});
    assert(ready.initialize({{123, 0}, EpochSource::pl031, 1}, {0, 1}, {0, 1}));
    assert(ready.read({1500000150, 7}).value() == (EpochTime{1, 0}));
    assert(!ready.read({1500000149, 7}) && ready.info().state == AnchorState::invalidated);
    assert(!ready.initialize(origin, {0, 7}, {0, 7}) && !ready.read({~u64{0}, 7}));
    EpochAnchor generation;
    assert(generation.initialize(origin, {0, 1}, {0, 1}));
    assert(!generation.read({0, 2}) && generation.info().state == AnchorState::invalidated);
    EpochAnchor overflow;
    assert(overflow.initialize({{std::numeric_limits<i64>::max(), 999999999}, EpochSource::goldfish, 1}, {0, 1}, {0, 1}));
    assert(overflow.read({1, 1}).status().code() == StatusCode::overflow);
    assert(overflow.info().state == AnchorState::invalidated && !overflow.read({0, 1}));
    EpochAnchor unavailable;
    assert(!unavailable.reject(Status::unsupported("fixture missing source")));
    assert(!unavailable.initialize(origin, {0, 1}, {0, 1}));
    assert(unavailable.read({0, 1}).status().code() == StatusCode::unsupported);
    for (auto before : {MonotonicPoint{0, 0}, MonotonicPoint{2, 1}, MonotonicPoint{0, 2}})
    {
        EpochAnchor bad;
        assert(!bad.initialize(origin, before, {1, 1}));
        assert(bad.info().state == AnchorState::unavailable && !bad.read({2, 1}));
    }
    EpochAnchor slow;
    assert(!slow.initialize(origin, {0, 1}, {EpochAnchor::maximum_acquisition_ns + 1, 1}));
    for (EpochReading invalid : {EpochReading{}, EpochReading{{0, 1000000000}, EpochSource::pl031, 1},
                                EpochReading{{0, 0}, EpochSource::pl031, 0},
                                EpochReading{{0, 0}, static_cast<EpochSource>(255), 1},
                                EpochReading{{0, 0}, EpochSource::pl031, 1000000001}})
    {
        EpochAnchor bad;
        assert(!bad.initialize(invalid, {0, 1}, {0, 1}));
    }
    assert(add_elapsed({-1, 999999999}, 1).value() == EpochTime{});
    assert(add_elapsed({std::numeric_limits<i64>::min(), 0}, 0).value().seconds == std::numeric_limits<i64>::min());
    assert(!add_elapsed({0, 1000000000}, 0));
    assert(!add_elapsed({std::numeric_limits<i64>::max(), 0}, 1000000000));
    __extension__ using Wide = __int128;
    u64 random = 42;
    for (unsigned index = 0; index < 10000; ++index)
    {
        random = random * 6364136223846793005ULL + 1;
        const EpochTime base{static_cast<i64>(random & 0x7fffffffffffffffULL) - 4000000000000000000LL,
                             static_cast<u32>(random % 1000000000)};
        const u64 elapsed = random ^ 0x7654321012345678ULL;
        const Wide total = Wide{base.seconds} * 1000000000 + base.nanoseconds + elapsed;
        const Wide seconds = total >= 0 ? total / 1000000000 : (total - 999999999) / 1000000000;
        auto actual = add_elapsed(base, elapsed);
        if (seconds > std::numeric_limits<i64>::max()) assert(!actual);
        else assert(actual && actual.value().seconds == seconds && actual.value().nanoseconds == total - seconds * 1000000000);
    }
}
} // namespace

int main()
{
    registers(); anchors();
    for (Calendar bad : {Calendar{0, 1, 1, 0, 0, 0}, Calendar{10000, 1, 1, 0, 0, 0},
                         Calendar{2024, 0, 1, 0, 0, 0}, Calendar{2024, 13, 1, 0, 0, 0},
                         Calendar{2024, 1, 0, 0, 0, 0}, Calendar{2024, 4, 31, 0, 0, 0},
                         Calendar{2100, 2, 29, 0, 0, 0}, Calendar{2024, 1, 1, 24, 0, 0},
                         Calendar{2024, 1, 1, 0, 60, 0}, Calendar{2024, 1, 1, 0, 0, 60}})
        assert(!calendar_epoch(bad));
    unsigned year, month, day, hour, minute, second, count = 0;
    long long expected;
    while (std::scanf("%u %u %u %u %u %u %lld", &year, &month, &day, &hour, &minute, &second, &expected) == 7)
    {
        auto actual = calendar_epoch({year, month, day, hour, minute, second});
        assert(actual && actual.value().seconds == expected && actual.value().nanoseconds == 0);
        ++count;
    }
    assert(count > 10000);
    std::printf("REALTIME_MODEL_PASS calendar_oracles=%u scope=hosted-not-hardware\n", count);
}
