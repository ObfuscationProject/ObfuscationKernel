// Hosted model test: clock_settime's commit against realtime readers on other
// CPUs, through the kernel's own src/arch/realtime.cpp. Threads stand in for
// CPUs (ScopedHostedCpu); the monotonic clock is a shared counter. No RTC,
// interrupt mask or kernel section executes.
#include "ok/arch/clock.hpp"
#include "ok/arch/realtime.hpp"
#include "ok/smp/cpu.hpp"
#include "ok/user/clock_set.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace ok;

namespace
{
std::atomic<u64> counter{1000};
unsigned checks{};

void expect(bool condition, const char *what)
{
    ++checks;
    if (!condition)
    {
        std::fprintf(stderr, "FAIL %s\n", what);
        std::exit(1);
    }
}
} // namespace

namespace ok::arch
{
// Strictly increasing on every CPU, one generation.
Result<time::MonotonicPoint> monotonic_sample()
{
    return time::MonotonicPoint{counter.fetch_add(1) + 1, 1};
}
Status clock_status() { return Status::success(); }
u64 clock_resolution_ns() { return 1; }
} // namespace ok::arch

int main()
{
    const smp::ScopedHostedCpu boot(0);
    // The x86_64 profile has no qualified RTC: the boot anchor is rejected and
    // only an administrator setting makes realtime ready.
    expect(!arch::initialize_realtime(), "hosted x86_64 accepted a boot RTC");
    expect(!arch::realtime(), "realtime ready before a setting");
    expect(arch::set_realtime_now({100, 0}).ok(), "first setting");
    expect(arch::realtime().ok(), "realtime after a setting");

    // The hazard set_realtime_now closes: a reading between the setter's
    // sample and its commit advances the anchor past the sample.
    auto early = arch::monotonic_sample();
    expect(early.ok() && arch::realtime().ok(), "sample and reading");
    expect(arch::set_realtime({200, 0}, early.value()).code() == StatusCode::io_error,
           "a commit at a sample older than a reading kept continuity");
    expect(arch::realtime().ok(), "a refused commit invalidated realtime");

    // The kernel's form refuses a value before elapsed monotonic time and
    // checks range and authority before it samples.
    const auto root = user::root_credentials();
    const auto before = counter.load();
    expect(user::set_realtime_checked({0, 0}, user::default_user_credentials(), arch::set_realtime_now).code() ==
                   StatusCode::permission &&
               counter.load() == before,
           "an unprivileged setting sampled the clock");
    counter.store(5'000'000'000);
    expect(user::set_realtime_checked({4, 999'999'999}, root, arch::set_realtime_now).code() ==
               StatusCode::invalid_argument,
           "a setting before elapsed monotonic time was accepted");
    expect(arch::realtime().ok(), "a refused setting invalidated realtime");

    // Readers on two CPUs while CPU 0 sets: every setting and reading holds.
    constexpr int settings = 20000;
    std::atomic<bool> stop{false};
    std::atomic<unsigned> reader_failures{0};
    std::atomic<u64> readings{0};
    auto reader = [&](smp::CpuId cpu) {
        const smp::ScopedHostedCpu scoped(cpu);
        while (!stop.load(std::memory_order_relaxed))
        {
            if (!arch::realtime())
                reader_failures.fetch_add(1);
            readings.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread one(reader, 1), two(reader, 2);
    while (readings.load() < 1000)
        std::this_thread::yield();
    // Until enough readings overlapped the settings, too.
    unsigned setter_failures = 0;
    const auto first = readings.load();
    for (i64 index = 0; index < settings || readings.load() - first < 200000; ++index)
        if (!user::set_realtime_checked({1000 + index, 0}, root, arch::set_realtime_now))
            ++setter_failures;
    stop.store(true);
    one.join();
    two.join();
    expect(setter_failures == 0, "a setting failed against concurrent readers");
    expect(reader_failures.load() == 0, "a reading failed against concurrent settings");
    expect(arch::realtime_clock_info().state == time::AnchorState::ready &&
               arch::realtime_clock_info().origin == time::RealtimeOrigin::administrator_software,
           "realtime left ready with the administrator's origin");
    std::printf("PASS %u hosted realtime setting/reader checks (%llu readings); model only\n", checks,
                static_cast<unsigned long long>(readings.load()));
    return 0;
}
