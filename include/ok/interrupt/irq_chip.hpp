#pragma once

#include "ok/core/concepts.hpp"
#include "ok/core/types.hpp"
#include "ok/interrupt/interrupt.hpp"

#include <array>
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

// Static-dispatch contract for an interrupt controller backend. Each
// architecture supplies one implementation (8259/APIC, GICv2, PLIC, EIOINTC,
// MIPS CP0/i8259, MPIC) under src/arch/<arch>/irq_chip.cpp; generic code is
// instantiated over it. No virtual call sits on the interrupt path.
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

// Hosted model chip: records state so the router can be tested without
// hardware. It is explicitly not an interrupt controller.
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
        ++acknowledged_[line];
        return Status::success();
    }
    Status end_of_interrupt(IrqLine line)
    {
        if (!initialized_ || line >= line_count())
            return Status{StatusCode::invalid_argument, "irq line out of range"};
        ++completed_[line];
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
        return line < line_count() && masked_[line];
    }
    [[nodiscard]] usize acknowledged(IrqLine line) const
    {
        return line < line_count() ? acknowledged_[line] : 0;
    }
    [[nodiscard]] usize completed(IrqLine line) const
    {
        return line < line_count() ? completed_[line] : 0;
    }

  private:
    Status set_masked(IrqLine line, bool value)
    {
        if (!initialized_)
            return Status{StatusCode::not_initialized, "irq chip not initialised"};
        if (line >= line_count())
            return Status{StatusCode::invalid_argument, "irq line out of range"};
        masked_[line] = value;
        return Status::success();
    }
    bool initialized_{false};
    std::array<bool, 32> masked_{};
    std::array<IrqTrigger, 32> triggers_{};
    std::array<usize, 32> acknowledged_{};
    std::array<usize, 32> completed_{};
};
static_assert(IrqChip<ModelIrqChip>);

// Binds controller lines to dispatcher vectors and performs the
// acknowledge -> dispatch -> end-of-interrupt sequence. Top halves must stay
// bounded; deferred work goes through the work queue (feat/irq-wait-time).
template <IrqChip Chip> class IrqRouter final : private NonCopyable
{
  public:
    IrqRouter(Chip &chip, InterruptDispatcher &dispatcher) : chip_(chip), dispatcher_(dispatcher)
    {
    }

    Status bind(IrqLine line, Vector vector, IrqTrigger trigger)
    {
        if (line >= chip_.line_count() || line >= max_irq_lines)
            return Status{StatusCode::invalid_argument, "irq line out of range"};
        if (bound_[line])
            return Status{StatusCode::already_exists, "irq line already bound"};
        if (auto configured = chip_.configure(line, trigger); !configured.ok())
            return configured;
        if (auto unmasked = chip_.unmask(line); !unmasked.ok())
            return unmasked;
        vectors_[line] = vector;
        bound_[line] = true;
        return Status::success();
    }
    Status unbind(IrqLine line)
    {
        if (line >= max_irq_lines || !bound_[line])
            return Status{StatusCode::not_found, "irq line not bound"};
        bound_[line] = false;
        return chip_.mask(line);
    }
    // Called from the architecture trap entry with the pending line.
    Status on_interrupt(IrqLine line, arch::TrapFrame &frame)
    {
        if (line >= max_irq_lines || !bound_[line])
        {
            ++spurious_;
            return Status{StatusCode::not_found, "spurious interrupt line"};
        }
        if (auto acked = chip_.acknowledge(line); !acked.ok())
            return acked;
        ++delivered_[line];
        const Status handled = dispatcher_.dispatch_vector(vectors_[line], frame);
        const Status eoi = chip_.end_of_interrupt(line);
        return handled.ok() ? eoi : handled;
    }
    [[nodiscard]] usize delivered(IrqLine line) const
    {
        return line < max_irq_lines ? delivered_[line] : 0;
    }
    [[nodiscard]] usize spurious() const
    {
        return spurious_;
    }

  private:
    Chip &chip_;
    InterruptDispatcher &dispatcher_;
    std::array<Vector, max_irq_lines> vectors_{};
    std::array<bool, max_irq_lines> bound_{};
    std::array<usize, max_irq_lines> delivered_{};
    usize spurious_{0};
};

} // namespace ok::interrupt
