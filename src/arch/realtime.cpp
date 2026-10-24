#include "ok/arch/realtime.hpp"
#include "ok/arch/clock.hpp"
#include "ok/time/rtc.hpp"

namespace ok::arch
{
namespace
{

time::RealtimeClock clock{};

#if defined(OK_ARCH_TARGET_AARCH64) || defined(OK_ARCH_TARGET_ARM32) || \
    defined(OK_ARCH_TARGET_RV32) || defined(OK_ARCH_TARGET_RV64) || defined(OK_ARCH_TARGET_LOONGARCH64)
struct DeviceBus
{
    uptr base;
    u32 read(u32 offset)
    {
#if defined(OK_ARCH_TARGET_AARCH64) || defined(OK_ARCH_TARGET_ARM32)
        asm volatile("dmb sy" : : : "memory");
#elif defined(OK_ARCH_TARGET_RV32) || defined(OK_ARCH_TARGET_RV64)
        asm volatile("fence iorw, iorw" : : : "memory");
#else
        asm volatile("dbar 0" : : : "memory");
#endif
        const auto value = *reinterpret_cast<volatile u32 *>(base + offset);
#if defined(OK_ARCH_TARGET_AARCH64) || defined(OK_ARCH_TARGET_ARM32)
        asm volatile("dmb sy" : : : "memory");
#elif defined(OK_ARCH_TARGET_RV32) || defined(OK_ARCH_TARGET_RV64)
        asm volatile("fence iorw, iorw" : : : "memory");
#else
        asm volatile("dbar 0" : : : "memory");
#endif
        return value; // Every selected MMIO profile is little-endian.
    }
};
#endif

} // namespace

Result<time::EpochReading> read_epoch_source()
{
#if defined(OK_ARCH_TARGET_AARCH64) || defined(OK_ARCH_TARGET_ARM32)
    DeviceBus bus{0x09010000};
    return time::read_pl031(bus);
#elif defined(OK_ARCH_TARGET_RV32) || defined(OK_ARCH_TARGET_RV64)
    DeviceBus bus{0x00101000};
    return time::read_goldfish(bus);
#elif defined(OK_ARCH_TARGET_LOONGARCH64)
    DeviceBus bus{0x100d0100};
    return time::read_ls7a(bus);
#elif defined(OK_ARCH_TARGET_I386) || defined(OK_ARCH_TARGET_X86_64) || \
      defined(OK_ARCH_TARGET_MIPS) || defined(OK_ARCH_TARGET_MIPS64)
    return Status::unsupported("MC146818 full century is not qualified");
#elif defined(OK_ARCH_TARGET_PPC)
    return Status::unsupported("DS1338 full era and MPC I2C transport are not qualified");
#else
#error "An epoch capability decision is required for every retained profile"
#endif
}

Status initialize_realtime()
{
    if (clock.boot_info().state != time::AnchorState::uninitialized)
        return clock.boot_status();
    auto before = monotonic_sample();
    if (!before)
        return clock.reject_boot(before.status());
    auto source = read_epoch_source();
    if (!source)
        return clock.reject_boot(source.status());
    auto after = monotonic_sample();
    if (!after)
        return clock.reject_boot(after.status());
    return clock.initialize_boot(source.value(), before.value(), after.value(), clock_resolution_ns());
}

Result<time::RealtimeSample> realtime_sample()
{
    if (clock.info().state != time::AnchorState::ready)
        return clock.status();
    auto current = monotonic_sample();
    if (!current)
        return clock.invalidate(current.status());
    return clock.read(current.value());
}

Result<time::EpochTime> realtime()
{
    auto current = realtime_sample();
    if (!current)
        return current.status();
    return current.value().value;
}

time::AnchorInfo realtime_info()
{
    if (clock.boot_info().state == time::AnchorState::ready)
        if (auto status = clock_status(); !status)
        {
            static_cast<void>(clock.invalidate_boot(status));
            static_cast<void>(clock.invalidate(status));
        }
    return clock.boot_info();
}

time::RealtimeInfo realtime_clock_info()
{
    if (clock.info().state == time::AnchorState::ready)
        if (auto status = clock_status(); !status)
            static_cast<void>(clock.invalidate(status));
    return clock.info();
}

Status set_realtime(time::EpochTime value, time::MonotonicPoint point)
{
    if (!clock_status())
        return Status::io_error("realtime setting requires a continuous platform clock");
    return clock.set(value, point, clock_resolution_ns());
}

} // namespace ok::arch
