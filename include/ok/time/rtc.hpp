#pragma once

#include "ok/time/epoch.hpp"
#include <concepts>

namespace ok::time
{

template <typename Bus>
concept RtcReadBus = requires(Bus &bus, u32 offset) {
    { bus.read(offset) } -> std::same_as<u32>;
};

// Only the plain ARM PL031 retained by QEMU virt, never ST calendar variants.
template <RtcReadBus Bus> [[nodiscard]] Result<EpochReading> read_pl031(Bus &bus)
{
    constexpr u32 identity[]{0x31, 0x10, 0x04, 0x00, 0x0d, 0xf0, 0x05, 0xb1};
    for (u32 index = 0; index < 8; ++index)
        // PID2 high nibble is the revision; the ARM designer low nibble is 4.
        if ((bus.read(0xfe0 + 4 * index) & (index == 2 ? 0x0fu : ~u32{0})) != identity[index])
            return Status::unsupported("plain ARM PL031 identity is unavailable");
    if (!(bus.read(0x0c) & 1))
        return Status::io_error("PL031 counter is disabled");
    const auto seconds = bus.read(0);
    if (!(bus.read(0x0c) & 1))
        return Status::io_error("PL031 counter stopped during read");
    return EpochReading{{static_cast<i64>(seconds), 0}, EpochSource::pl031, nanoseconds_per_second};
}

// LOW latches HIGH. Both accesses stay byte-order-correct 32-bit device reads.
template <RtcReadBus Bus> [[nodiscard]] Result<EpochReading> read_goldfish(Bus &bus)
{
    const u32 low = bus.read(0);
    const u32 high = bus.read(4);
    const u64 value = (u64{high} << 32) | low;
    return EpochReading{{static_cast<i64>(value / nanoseconds_per_second),
                         static_cast<u32>(value % nanoseconds_per_second)}, EpochSource::goldfish, 1};
}

template <RtcReadBus Bus> [[nodiscard]] Result<EpochReading> read_ls7a(Bus &bus)
{
    constexpr u32 enabled = (1u << 11) | (1u << 8);
    if ((bus.read(0x40) & enabled) != enabled)
        return Status::unsupported("LS7A TOY oscillator is not enabled");
    for (unsigned attempt = 0; attempt < 64; ++attempt)
    {
        const auto year = bus.read(0x30);
        const auto calendar = bus.read(0x2c) & ~u32{15};
        const auto checked_year = bus.read(0x30);
        const auto checked_calendar = bus.read(0x2c) & ~u32{15};
        if (year != checked_year || calendar != checked_calendar)
            continue;
        if ((bus.read(0x40) & enabled) != enabled)
            return Status::io_error("LS7A TOY oscillator stopped during read");
        if (year > 8099)
            return Status::overflow("LS7A calendar year exceeds supported domain");
        auto epoch = calendar_epoch({year + 1900, calendar >> 26, (calendar >> 21) & 31,
                                     (calendar >> 16) & 31, (calendar >> 10) & 63, (calendar >> 4) & 63});
        if (!epoch)
            return epoch.status();
        return EpochReading{epoch.value(), EpochSource::ls7a, nanoseconds_per_second};
    }
    return Status::io_error("LS7A TOY snapshot did not stabilize");
}

} // namespace ok::time
