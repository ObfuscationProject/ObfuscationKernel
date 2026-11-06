#pragma once

#include "ok/core/concepts.hpp"
#include "ok/core/types.hpp"
#include "ok/interrupt/interrupt.hpp"
#include "ok/smp/lock.hpp"

#include <array>
#include <atomic>
#include <concepts>

namespace ok::interrupt
{

using IrqLine = u16;
inline constexpr usize max_irq_lines = 256;

enum class IrqTrigger : u8
{
    edge,
    level,
};

// Static-dispatch contract for an interrupt controller backend. Each profile
// supplies one implementation (the 8259A pair, GICv2, the PLIC, EIOINTC with
// PCH-PIC, Malta's 8259 pair, the MPIC), selected as PlatformIrqChip in
// src/interrupt/platform_irq.cpp; generic code is instantiated over it. No
// virtual call sits on the interrupt path.
template <typename Chip>
concept IrqChip = requires(Chip chip, const Chip cchip, IrqLine line, IrqTrigger trigger) {
    { chip.initialize() } -> std::same_as<Status>;
    { chip.configure(line, trigger) } -> std::same_as<Status>;
    { chip.mask(line) } -> std::same_as<Status>;
    { chip.unmask(line) } -> std::same_as<Status>;
    { chip.acknowledge(line) } -> std::same_as<Status>;
    { chip.end_of_interrupt(line) } -> std::same_as<Status>;
    { cchip.line_count() } -> std::convertible_to<usize>;
    { cchip.name() } -> std::convertible_to<std::string_view>;
};

// A controller that can deliver a line to one chosen CPU (GICv2 ITARGETSR,
// the PLIC's per-hart contexts). The router calls route() with its lock held.
template <typename Chip>
concept RoutableIrqChip = IrqChip<Chip> && requires(Chip chip, IrqLine line, smp::CpuId cpu) {
    { chip.route(line, cpu) } -> std::same_as<Status>;
};

// Hosted model chip: records state so the router can be tested without
// hardware. It is explicitly not an interrupt controller. Model threads may
// stand in for CPUs, so its records are atomic.
class ModelIrqChip final
{
  public:
    Status initialize()
    {
        initialized_ = true;
        return Status::success();
    }
    Status configure(IrqLine line, IrqTrigger trigger)
    {
        if (!initialized_)
            return Status{StatusCode::not_initialized, "irq chip not initialised"};
        if (line >= line_count())
            return Status{StatusCode::invalid_argument, "irq line out of range"};
        triggers_[line] = trigger;
        return Status::success();
    }
    Status mask(IrqLine line)
    {
        return set_masked(line, true);
    }
    Status unmask(IrqLine line)
    {
        return set_masked(line, false);
    }
    Status acknowledge(IrqLine line)
    {
        if (!initialized_ || line >= line_count())
            return Status{StatusCode::invalid_argument, "irq line out of range"};
        acknowledged_[line].fetch_add(1, std::memory_order_relaxed);
        return Status::success();
    }
    Status end_of_interrupt(IrqLine line)
    {
        if (!initialized_ || line >= line_count())
            return Status{StatusCode::invalid_argument, "irq line out of range"};
        completed_[line].fetch_add(1, std::memory_order_relaxed);
        return Status::success();
    }
    Status route(IrqLine line, smp::CpuId cpu)
    {
        if (!initialized_ || line >= line_count() || cpu >= smp::max_cpus)
            return Status{StatusCode::invalid_argument, "irq line or CPU out of range"};
        routes_[line].store(cpu, std::memory_order_relaxed);
        return Status::success();
    }
    [[nodiscard]] usize line_count() const
    {
        return 32;
    }
    [[nodiscard]] std::string_view name() const
    {
        return "model-irq-chip";
    }
    [[nodiscard]] bool masked(IrqLine line) const
    {
        return line < line_count() && masked_[line].load(std::memory_order_relaxed);
    }
    [[nodiscard]] usize acknowledged(IrqLine line) const
    {
        return line < line_count() ? acknowledged_[line].load(std::memory_order_relaxed) : 0;
    }
    [[nodiscard]] usize completed(IrqLine line) const
    {
        return line < line_count() ? completed_[line].load(std::memory_order_relaxed) : 0;
    }
    [[nodiscard]] smp::CpuId routed(IrqLine line) const
    {
        return line < line_count() ? routes_[line].load(std::memory_order_relaxed) : smp::CpuId{0};
    }

  private:
    Status set_masked(IrqLine line, bool value)
    {
        if (!initialized_)
            return Status{StatusCode::not_initialized, "irq chip not initialised"};
        if (line >= line_count())
            return Status{StatusCode::invalid_argument, "irq line out of range"};
        masked_[line].store(value, std::memory_order_relaxed);
        return Status::success();
    }
    bool initialized_{false};
    std::array<std::atomic<bool>, 32> masked_{};
    std::array<IrqTrigger, 32> triggers_{};
    std::array<std::atomic<usize>, 32> acknowledged_{};
    std::array<std::atomic<usize>, 32> completed_{};
    std::array<std::atomic<smp::CpuId>, 32> routes_{};
};
static_assert(RoutableIrqChip<ModelIrqChip>);

// Bottom-half handler for one line: services the device (reads its interrupt
// status, consumes completions) with interrupts disabled, outside trap context.
using IrqHandler = void (*)(void *context, IrqLine line);

// Splits an interrupt into a bounded top half and a deferred bottom half. The
// top half runs in the trap path of the CPU that took the line: it
// acknowledges the line, masks it so a level-triggered device cannot re-enter
// before it is serviced, records it pending and completes it at that CPU's
// controller interface. drain() runs on any CPU and calls each pending line's
// handlers once, then unmasks the line. No device code and no allocation runs
// in trap context.
//
// SMP: line records, handler lists, statistics and every chip
// read-modify-write sequence sit under one irq_router lock, taken with
// interrupts masked and never held while a handler runs. A drain claims a
// line's pending flag by atomic exchange, so one CPU at a time runs that
// line's handlers, and the line stays masked from its top half until they
// returned. Handlers of different lines, and unbind() against a running
// handler, are serialized by the caller (the kernel lock).
template <IrqChip Chip> class IrqRouter final : private NonCopyable
{
  public:
    // A line may be shared (PCI INTx); its handlers run in bind order.
    static constexpr usize max_line_handlers = 4;

    constexpr explicit IrqRouter(Chip &chip) : chip_(chip)
    {
    }

    // The first handler configures the line, routes it to `target` on a
    // routable chip (to CPU 0 when `target` cannot take it) and unmasks it.
    Status bind(IrqLine line, IrqTrigger trigger, IrqHandler handler, void *context, smp::CpuId target = 0)
    {
        if (line >= chip_.line_count() || line >= max_irq_lines || handler == nullptr)
            return Status{StatusCode::invalid_argument, "irq line out of range or without handler"};
        Guard guard(lock_);
        auto &entry = lines_[line];
        for (usize i = 0; i < entry.count; ++i)
            if (entry.handlers[i].handler == handler && entry.handlers[i].context == context)
                return Status{StatusCode::already_exists, "irq handler already bound to this line"};
        if (entry.count == max_line_handlers)
            return Status{StatusCode::overflow, "irq line has no free handler slot"};
        if (entry.count != 0 && entry.trigger != trigger)
            return Status{StatusCode::invalid_argument, "irq line is already bound with another trigger"};
        if (entry.count == 0)
        {
            if (auto configured = chip_.configure(line, trigger); !configured.ok())
                return configured;
            entry.trigger = trigger;
            entry.target = 0;
            if constexpr (RoutableIrqChip<Chip>)
            {
                if (target != 0 && chip_.route(line, target).ok())
                    entry.target = target;
                else if (auto routed = chip_.route(line, 0); !routed.ok())
                    return routed;
            }
        }
        entry.handlers[entry.count++] = {handler, context};
        if (entry.count == 1)
            if (auto unmasked = chip_.unmask(line); !unmasked.ok())
            {
                reset(entry);
                return unmasked;
            }
        return Status::success();
    }
    // Removes one handler; the line is masked when its last handler goes.
    Status unbind(IrqLine line, IrqHandler handler, void *context)
    {
        if (line >= max_irq_lines)
            return Status{StatusCode::not_found, "irq line not bound"};
        Guard guard(lock_);
        auto &entry = lines_[line];
        for (usize i = 0; i < entry.count; ++i)
        {
            if (entry.handlers[i].handler != handler || entry.handlers[i].context != context)
                continue;
            for (usize j = i + 1; j < entry.count; ++j)
                entry.handlers[j - 1] = entry.handlers[j];
            entry.handlers[--entry.count] = {};
            if (entry.count != 0)
                return Status::success();
            reset(entry);
            return chip_.mask(line);
        }
        return Status{StatusCode::not_found, "irq handler not bound to this line"};
    }
    // Delivers a bound line to `target` from now on. An interrupt the old CPU
    // already claimed still runs its top half there; pending state is kept.
    Status retarget(IrqLine line, smp::CpuId target)
    {
        if (line >= max_irq_lines)
            return Status{StatusCode::not_found, "irq line not bound"};
        Guard guard(lock_);
        auto &entry = lines_[line];
        if (entry.count == 0)
            return Status{StatusCode::not_found, "irq line not bound"};
        if (entry.target == target)
            return Status::success();
        if constexpr (RoutableIrqChip<Chip>)
        {
            if (auto routed = chip_.route(line, target); !routed.ok())
                return routed;
            entry.target = target;
            return Status::success();
        }
        else
            return Status{StatusCode::unsupported, "irq controller delivers to CPU 0 only"};
    }
    [[nodiscard]] bool bound(IrqLine line) const
    {
        return line < max_irq_lines && lines_[line].count != 0;
    }
    // The CPU a bound line is delivered to.
    [[nodiscard]] smp::CpuId target(IrqLine line) const
    {
        return line < max_irq_lines ? lines_[line].target : smp::CpuId{0};
    }
    // Top half: called with interrupts disabled, on the CPU that took `line`.
    // True when the line was bound and is now pending.
    bool top_half(IrqLine line)
    {
        bool claimed = false;
        {
            Guard guard(lock_);
            if (line >= max_irq_lines || lines_[line].count == 0)
            {
                // Unclaimed: keep it from re-asserting and complete it.
                bump(spurious_);
                if (line >= chip_.line_count())
                    return false;
                static_cast<void>(chip_.mask(line));
            }
            else
            {
                if (!chip_.acknowledge(line).ok() || !chip_.mask(line).ok())
                    bump(controller_errors_);
                bump(delivered_[line]);
                lines_[line].pending.store(true, std::memory_order_release);
                any_pending_.store(true, std::memory_order_seq_cst);
                claimed = true;
            }
        }
        // The completion goes to this CPU's own controller interface.
        if (!chip_.end_of_interrupt(line).ok() && claimed)
        {
            Guard guard(lock_);
            bump(controller_errors_);
        }
        return claimed;
    }
    // Bottom half: runs every pending line's handlers once and re-enables it.
    usize drain()
    {
        if (!any_pending_.exchange(false, std::memory_order_acq_rel))
            return 0;
        usize serviced = 0;
        for (IrqLine line = 0; line < max_irq_lines; ++line)
        {
            auto &entry = lines_[line];
            if (!entry.pending.load(std::memory_order_relaxed) ||
                !entry.pending.exchange(false, std::memory_order_acq_rel))
                continue;
            // This CPU owns the masked line until it unmasks it. The handlers
            // run from a snapshot, without the lock.
            std::array<Handler, max_line_handlers> handlers{};
            usize count = 0;
            {
                Guard guard(lock_);
                handlers = entry.handlers;
                count = entry.count;
            }
            for (usize i = 0; i < count; ++i)
                handlers[i].handler(handlers[i].context, line);
            Guard guard(lock_);
            bump(serviced_[line]);
            ++serviced;
            if (entry.count != 0 && !chip_.unmask(line).ok())
                bump(controller_errors_);
        }
        return serviced;
    }
    // Some line may be pending (sequentially consistent with the top half's
    // store, so a waiter that published itself first cannot miss it).
    [[nodiscard]] bool pending() const
    {
        return any_pending_.load(std::memory_order_seq_cst);
    }
    [[nodiscard]] usize delivered(IrqLine line) const
    {
        return line < max_irq_lines ? delivered_[line].load(std::memory_order_relaxed) : 0;
    }
    [[nodiscard]] usize serviced(IrqLine line) const
    {
        return line < max_irq_lines ? serviced_[line].load(std::memory_order_relaxed) : 0;
    }
    [[nodiscard]] usize spurious() const
    {
        return spurious_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] usize controller_errors() const
    {
        return controller_errors_.load(std::memory_order_relaxed);
    }

  private:
    using Guard = smp::RankedSpinGuard<smp::LockRank::irq_router>;
    struct Handler
    {
        IrqHandler handler{nullptr};
        void *context{nullptr};
    };
    struct Line
    {
        std::array<Handler, max_line_handlers> handlers{};
        usize count{0};
        IrqTrigger trigger{IrqTrigger::edge};
        smp::CpuId target{0};
        std::atomic<bool> pending{false};
    };
    // Counters change only under the lock and are read without it. A load
    // and a store avoid read-modify-write atomics, which not every profile
    // has.
    static void bump(std::atomic<usize> &counter)
    {
        counter.store(counter.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
    static void reset(Line &entry)
    {
        entry.handlers = {};
        entry.count = 0;
        entry.trigger = IrqTrigger::edge;
        entry.target = 0;
        entry.pending.store(false, std::memory_order_relaxed);
    }
    Chip &chip_;
    smp::RankedSpinLock<smp::LockRank::irq_router> lock_{};
    std::array<Line, max_irq_lines> lines_{};
    std::array<std::atomic<usize>, max_irq_lines> delivered_{};
    std::array<std::atomic<usize>, max_irq_lines> serviced_{};
    std::atomic<bool> any_pending_{false};
    std::atomic<usize> spurious_{0};
    std::atomic<usize> controller_errors_{0};
};

} // namespace ok::interrupt
