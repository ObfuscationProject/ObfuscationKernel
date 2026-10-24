#include "ok/time/realtime_clock.hpp"
#include "ok/user/clock_set.hpp"
#include <cstdio>
#include <cstdlib>

using namespace ok;
static unsigned checks{};
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) { std::fprintf(stderr,"FAIL line=%d: %s\n",__LINE__,#__VA_ARGS__); std::abort(); } } while(false)

static void transitions()
{
    time::RealtimeClock clock;
    CHECK(!clock.read({1,1}));
    CHECK(clock.set({100,0},{1,1},1).code()==StatusCode::not_initialized);
    CHECK(clock.info().wall_generation==0);
    CHECK(!clock.reject_boot(Status::unsupported("no RTC")));
    const auto boot=clock.boot_info();
    CHECK(boot.state==time::AnchorState::unavailable);
    CHECK(clock.set({100,123},{1000,1},1));
    CHECK(clock.info().wall_generation==1 && clock.info().origin==time::RealtimeOrigin::administrator_software);
    CHECK(clock.boot_info().state==boot.state && clock.boot_info().source==boot.source);
    auto sample=clock.read({1007,1});CHECK(sample && sample.value().value==time::EpochTime{100,130});
    CHECK(sample.value().wall_generation==1 && sample.value().monotonic.nanoseconds==1007);
    CHECK(!clock.initialize_boot({{300,0},time::EpochSource::pl031,1000000000},{1010,1},{1020,1},1));
    CHECK(clock.info().wall_generation==1 && clock.info().origin==time::RealtimeOrigin::administrator_software);
    CHECK(clock.set({90,999999999},{2000,1},10)); // Deliberate backwards wall step.
    sample=clock.read({2001,1});CHECK(sample && sample.value().value==time::EpochTime{91,0});
    CHECK(sample.value().wall_generation==2 && clock.info().conversion_resolution_ns==10);
    CHECK(!clock.set({1,1000000000},{2002,1},10));
    CHECK(!clock.set({1,0},{2002,0},10));
    CHECK(!clock.set({1,0},{2002,1},0));
    CHECK(!clock.set({1,0},{2000,1},10));
    CHECK(!clock.set({1,0},{2002,2},10));
    CHECK(clock.info().state==time::AnchorState::ready && clock.info().wall_generation==2);
    sample=clock.read({2011,1});CHECK(sample && sample.value().value==time::EpochTime{91,10});
    CHECK(!clock.read({2010,1}) && clock.info().state==time::AnchorState::invalidated);
    CHECK(!clock.set({200,0},{3000,1},1));
    CHECK(!clock.read({4000,1}) && clock.info().origin==time::RealtimeOrigin::administrator_software);
    CHECK(clock.boot_info().state==time::AnchorState::unavailable);
}

static void physical_and_overflow()
{
    time::RealtimeClock clock;
    CHECK(clock.initialize_boot({{0,999999999},time::EpochSource::pl031,1000000000},{3,4},{7,4},2));
    CHECK(clock.info().wall_generation==1 && clock.info().origin==time::RealtimeOrigin::boot_rtc);
    auto sample=clock.read({8,4});CHECK(sample && sample.value().value==time::EpochTime{1,0});
    CHECK(clock.set({5,8},{9,4},2));
    CHECK(clock.boot_info().state==time::AnchorState::ready && clock.info().wall_generation==2);
    CHECK(clock.initialize_boot({{999,0},time::EpochSource::goldfish,1},{10,4},{11,4},1));
    sample=clock.read({19,4});CHECK(sample && sample.value().value==time::EpochTime{5,18});
    CHECK(!clock.read({20,5}) && !clock.set({100,0},{21,4},1));
    CHECK(clock.boot_info().state==time::AnchorState::ready); // No fallback to it.
    CHECK(!clock.read({22,4}));
    CHECK(time::next_wall_generation(0).value()==1);
    CHECK(time::next_wall_generation(~u64{0}-1).value()==~u64{0});
    CHECK(time::next_wall_generation(~u64{0}).status().code()==StatusCode::overflow);
    time::RealtimeClock wide;
    CHECK(!wide.reject_boot(Status::not_initialized("early clock")));
    CHECK(wide.set({static_cast<i64>((~u64{0})>>1),999999999},{0,1},1));
    CHECK(!wide.read({1,1}) && wide.info().state==time::AnchorState::invalidated);
    time::RealtimeClock bad;
    CHECK(!bad.initialize_boot({{0,0},time::EpochSource::unavailable,1},{0,1},{1,1},1));
    CHECK(bad.boot_info().state==time::AnchorState::unavailable);
    CHECK(bad.set({100,0},{2,1},1)); // Explicit recovery of absent boot epoch only.
}

static void authority_and_floor()
{
    unsigned samples{},commits{};
    time::MonotonicPoint point{1000000005,7};
    time::EpochTime committed{};time::MonotonicPoint captured{};
    auto sample=[&]() -> Result<time::MonotonicPoint> { ++samples; return point; };
    auto commit=[&](time::EpochTime epoch,time::MonotonicPoint anchor) { ++commits;committed=epoch;captured=anchor;return Status::success(); };
    auto call=[&](user::ClockSetValue value,user::Credentials credentials) { return user::set_realtime_checked(value,credentials,sample,commit); };
    const auto root=user::root_credentials(),ordinary=user::default_user_credentials();
    for(auto value:{user::ClockSetValue{-1,0},{0,-1},{0,1000000000},{user::maximum_set_realtime_seconds,0}})
    {
        CHECK(call(value,ordinary).code()==StatusCode::invalid_argument);
        CHECK(call(value,root).code()==StatusCode::invalid_argument);
    }
    CHECK(samples==0 && commits==0);
    CHECK(call({0,0},ordinary).code()==StatusCode::permission);
    CHECK(samples==0 && commits==0);
    CHECK(call({1,4},root).code()==StatusCode::invalid_argument && samples==1 && commits==0);
    CHECK(call({1,5},root) && samples==2 && commits==1);
    CHECK(committed==time::EpochTime{1,5} && captured.nanoseconds==point.nanoseconds && captured.generation==point.generation);
    CHECK(call({user::maximum_set_realtime_seconds-1,999999999},root));
    CHECK(committed.seconds==user::maximum_set_realtime_seconds-1);
    auto credential=ordinary;credential.euid=0;CHECK(call({2,0},credential));
    credential=root;credential.euid=1000;CHECK(call({2,0},credential).code()==StatusCode::permission);
    const unsigned before=commits;
    CHECK(user::set_realtime_checked({2,0},root,[]() -> Result<time::MonotonicPoint> { return Status::not_initialized("no clock"); },commit).code()==StatusCode::io_error);
    CHECK(commits==before);
}

int main()
{
    transitions();physical_and_overflow();authority_and_floor();
    std::printf("PASS %u hosted realtime coordinator/helper checks; no hardware execution\n",checks);
}
