#include "ok/arch/clock.hpp"
#include "clock_counter.hpp"

namespace ok::arch
{
namespace
{

detail::CounterClock clock_state{};
bool clock_attempted{};
bool clock_invalidated{};
Status clock_failure{Status::not_initialized("platform clock is not initialized")};

#if defined(OK_ARCH_TARGET_X86_64) || defined(OK_ARCH_TARGET_I386)
struct PlatformClock
{
    static constexpr unsigned width = 64;
    static void out8(u16 port, u8 value)
    {
        asm volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
    }
    static u8 in8(u16 port)
    {
        u8 value;
        asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
        return value;
    }
    static u64 read()
    {
        // CPUID works on both retained PC CPU profiles; i386 does not require
        // SSE2/LFENCE. Serialize older instructions before sampling the TSC.
        u32 a = 0, b, c, d;
        asm volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d) : : "memory");
        u32 low, high;
        asm volatile("rdtsc" : "=a"(low), "=d"(high) : : "memory");
        return (u64{high} << 32) | low;
    }
    static Result<u64> frequency()
    {
        u32 a = 1, b, c, d;
        asm volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d) : : "memory");
        if ((d & (1u << 4)) == 0)
            return Status::unsupported("PC monotonic clock requires TSC");
        constexpr u32 pit_frequency = 1193182;
        constexpr u16 interval = 59659; // Approximately 50 ms at the PIT input rate.
        const auto saved = in8(0x61);
        const auto disabled = static_cast<u8>(saved & ~u8{3});
        out8(0x61, disabled);
        out8(0x43, 0xb0); // Channel 2, low/high byte, mode 0, binary.
        out8(0x42, static_cast<u8>(interval));
        out8(0x42, static_cast<u8>(interval >> 8));
        if ((in8(0x61) & 0x20) != 0)
        {
            out8(0x61, saved);
            return Status::io_error("PIT reference did not enter countdown mode");
        }
        const auto start = read();
        out8(0x61, static_cast<u8>(disabled | 1));
        bool finished = false;
        for (u32 attempt = 0; attempt < 100000000; ++attempt)
            if ((in8(0x61) & 0x20) != 0)
            {
                finished = true;
                break;
            }
        const auto end = read();
        out8(0x61, saved);
        if (!finished || end <= start || end - start > (~u64{0}) / pit_frequency)
            return Status::io_error("PIT reference did not calibrate the TSC");
        return (end - start) * pit_frequency / interval;
    }
};
#elif defined(OK_ARCH_TARGET_AARCH64)
struct PlatformClock
{
    static constexpr unsigned width = 64;
    static u64 read()
    {
        u64 counter;
        asm volatile("isb; mrs %0, cntvct_el0" : "=r"(counter) : : "memory");
        return counter;
    }
    static Result<u64> frequency()
    {
        u64 value;
        asm volatile("mrs %0, cntfrq_el0" : "=r"(value));
        return value;
    }
};
#elif defined(OK_ARCH_TARGET_ARM32)
struct PlatformClock
{
    static constexpr unsigned width = 64;
    static u64 read()
    {
        u32 low, high;
        asm volatile("isb; mrrc p15, 0, %0, %1, c14" : "=r"(low), "=r"(high) : : "memory");
        return (u64{high} << 32) | low; // CNTPCT, not the optional PMU counter.
    }
    static Result<u64> frequency()
    {
        u32 value;
        asm volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(value));
        return u64{value};
    }
};
#elif defined(OK_ARCH_TARGET_RV64) || defined(OK_ARCH_TARGET_RV32)
struct PlatformClock
{
    static constexpr unsigned width = 64;
    static u64 read()
    {
        // QEMU virt CLINT/ACLINT mtime. RV32 needs a stable split-word read.
        auto *counter = reinterpret_cast<volatile u32 *>(uptr{0x0200bff8});
        u32 high, low, checked;
        do
        {
            high = counter[1];
            low = counter[0];
            checked = counter[1];
        } while (high != checked);
        return (u64{high} << 32) | low;
    }
    static Result<u64> frequency()
    {
        // QEMU virt's /cpus/timebase-frequency, also used by native preemption.
        return u64{10000000};
    }
};
#elif defined(OK_ARCH_TARGET_LOONGARCH64)
struct PlatformClock
{
    static constexpr unsigned width = 64;
    static u64 read()
    {
        u64 counter, cpu;
        asm volatile("rdtime.d %0, %1" : "=r"(counter), "=r"(cpu) : : "memory");
        return counter;
    }
    static Result<u64> frequency()
    {
        u64 base, ratio, index = 4;
        asm volatile("cpucfg %0, %1" : "=r"(base) : "r"(index));
        index = 5;
        asm volatile("cpucfg %0, %1" : "=r"(ratio) : "r"(index));
        const auto denominator = (ratio >> 16) & 0xffff;
        const auto numerator = ratio & 0xffff;
        base &= 0xffffffff;
        if (!base || !numerator || !denominator)
            return Status::unsupported("LoongArch constant-clock frequency is unavailable");
        return base * numerator / denominator;
    }
};
#elif defined(OK_ARCH_TARGET_MIPS) || defined(OK_ARCH_TARGET_MIPS64)
struct PlatformClock
{
    static constexpr unsigned width = 32;
    static u64 read()
    {
        u32 counter;
        asm volatile("mfc0 %0, $9" : "=r"(counter));
        return counter;
    }
    static u8 rtc(u8 index)
    {
        // Malta GT64120 PCI I/O aperture, uncached KSEG1, byte-wide ISA RTC.
        constexpr uptr io = static_cast<uptr>(static_cast<__INTPTR_TYPE__>(-1207959552LL)); // 0xb8000000
        *reinterpret_cast<volatile u8 *>(io + 0x70) = index;
        asm volatile("sync" : : : "memory");
        return *reinterpret_cast<volatile u8 *>(io + 0x71);
    }
    static bool wait_update(bool active)
    {
        for (u32 attempt = 0; attempt < 100000000; ++attempt)
            if (((rtc(0x0a) & 0x80) != 0) == active)
                return true;
        return false;
    }
    static Result<u64> frequency()
    {
        // Two rising UIP edges delimit one RTC second independently of Count
        // frequency. Do not alter the RTC or assume Count is CPU clock / 2.
        if ((rtc(0x0b) & 0x80) != 0 || !wait_update(false) || !wait_update(true))
            return Status::io_error("Malta RTC reference is unavailable");
        const auto first = static_cast<u32>(read());
        if (!wait_update(false) || !wait_update(true))
            return Status::io_error("Malta RTC reference did not advance");
        const auto second = static_cast<u32>(read());
        return u64{static_cast<u32>(second - first)};
    }
};
#elif defined(OK_ARCH_TARGET_PPC)
struct PlatformClock
{
    static constexpr unsigned width = 64;
    static u64 read()
    {
        u32 high, low, checked;
        do
        {
            asm volatile("mfspr %0, 269; mfspr %1, 268; mfspr %2, 269"
                         : "=r"(high), "=r"(low), "=r"(checked) : : "memory");
        } while (high != checked);
        return (u64{high} << 32) | low;
    }
    static Result<u64> frequency()
    {
        // QEMU ppce500 /cpus/PowerPC,8544@0/timebase-frequency. This is a
        // retained board contract, not a frequency claim for arbitrary e500s.
        return u64{400000000};
    }
};
#else
#error "A platform clock backend is required for every retained architecture"
#endif

} // namespace

Status initialize_clock()
{
    if (clock_invalidated)
        return clock_failure;
    if (clock_state.initialized())
        return Status::success();
    if (clock_attempted)
        return clock_failure;
    clock_attempted = true;
    auto frequency = PlatformClock::frequency();
    if (!frequency)
        return clock_failure = frequency.status();
    return clock_failure = clock_state.initialize(frequency.value(), PlatformClock::read(), PlatformClock::width);
}

Result<u64> monotonic_nanoseconds()
{
    if (clock_invalidated)
        return clock_failure;
    if (!clock_state.initialized())
        return clock_failure;
    auto result = clock_state.sample(PlatformClock::read());
    if (!result)
    {
        clock_failure = result.status();
        clock_invalidated = true;
    }
    return result;
}

Result<time::MonotonicPoint> monotonic_sample()
{
    auto value = monotonic_nanoseconds();
    if (!value)
        return value.status();
    // There is exactly one successful counter epoch per boot; no restart API.
    return time::MonotonicPoint{value.value(), 1};
}

void invalidate_clock()
{
    if (!clock_invalidated)
    {
        clock_failure = Status::io_error("platform clock continuity invalidated");
        clock_invalidated = true;
    }
}

Status clock_status()
{
    return clock_invalidated || !clock_state.initialized() ? clock_failure : Status::success();
}

u64 clock_resolution_ns()
{
    return clock_invalidated ? 0 : clock_state.resolution();
}

} // namespace ok::arch
