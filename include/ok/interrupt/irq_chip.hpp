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

// Bottom-half handler for one line: services the device (reads its interrupt
// status, consumes completions) with interrupts disabled, outside trap context.
using IrqHandler = void (*)(void *context, IrqLine line);

// Splits an interrupt into a bounded top half and a deferred bottom half. The
// top half runs in the trap path: it acknowledges the line, masks it so a
// level-triggered device cannot re-enter before it is serviced, records it
// pending and completes it at the controller. drain() runs from the kernel
// loop and calls each pending line's handler once, then unmasks the line. No
// device code and no allocation runs in trap context.
template <IrqChip Chip> class IrqRouter final : private NonCopyable
{
  public:
    // A line may be shared (PCI INTx); its handlers run in bind order.
    static constexpr usize max_line_handlers = 4;

    constexpr explicit IrqRouter(Chip &chip) : chip_(chip)
    {
    }

    Status bind(IrqLine line, IrqTrigger trigger, IrqHandler handler, void *context)
    {
        if (line >= chip_.line_count() || line >= max_irq_lines || handler == nullptr)
            return Status{StatusCode::invalid_argument, "irq line out of range or without handler"};
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
        }
        entry.handlers[entry.count++] = {handler, context};
        if (entry.count == 1)
            if (auto unmasked = chip_.unmask(line); !unmasked.ok())
            {
                entry = {};
                return unmasked;
            }
        return Status::success();
    }
    // Removes one handler; the line is masked when its last handler goes.
    Status unbind(IrqLine line, IrqHandler handler, void *context)
    {
        if (line >= max_irq_lines)
            return Status{StatusCode::not_found, "irq line not bound"};
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
            if (entry.pending)
                --pending_count_;
            entry = {};
            return chip_.mask(line);
        }
        return Status{StatusCode::not_found, "irq handler not bound to this line"};
    }
    [[nodiscard]] bool bound(IrqLine line) const
    {
        return line < max_irq_lines && lines_[line].count != 0;
    }
    // Top half: called with interrupts disabled for the line the trap names.
    void top_half(IrqLine line)
    {
        if (line >= max_irq_lines || lines_[line].count == 0)
        {
            // Unclaimed: keep it from re-asserting and complete it.
            ++spurious_;
            if (line < chip_.line_count())
            {
                static_cast<void>(chip_.mask(line));
                static_cast<void>(chip_.end_of_interrupt(line));
            }
            return;
        }
        if (!chip_.acknowledge(line).ok() || !chip_.mask(line).ok())
            ++controller_errors_;
        if (!lines_[line].pending)
        {
            lines_[line].pending = true;
            ++pending_count_;
        }
        ++delivered_[line];
        if (!chip_.end_of_interrupt(line).ok())
            ++controller_errors_;
    }
    // Bottom half: runs every pending line's handlers once and re-enables it.
    usize drain()
    {
        usize serviced = 0;
        for (IrqLine line = 0; pending_count_ != 0 && line < max_irq_lines; ++line)
        {
            auto &entry = lines_[line];
            if (!entry.pending)
                continue;
            entry.pending = false;
            --pending_count_;
            for (usize i = 0; i < entry.count; ++i)
                entry.handlers[i].handler(entry.handlers[i].context, line);
            ++serviced_[line];
            ++serviced;
            if (entry.count != 0 && !chip_.unmask(line).ok())
                ++controller_errors_;
        }
        return serviced;
    }
    [[nodiscard]] bool pending() const
    {
        return pending_count_ != 0;
    }
    [[nodiscard]] usize delivered(IrqLine line) const
    {
        return line < max_irq_lines ? delivered_[line] : 0;
    }
    [[nodiscard]] usize serviced(IrqLine line) const
    {
        return line < max_irq_lines ? serviced_[line] : 0;
    }
    [[nodiscard]] usize spurious() const
    {
        return spurious_;
    }
    [[nodiscard]] usize controller_errors() const
    {
        return controller_errors_;
    }

  private:
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
        bool pending{false};
    };
    Chip &chip_;
    std::array<Line, max_irq_lines> lines_{};
    std::array<usize, max_irq_lines> delivered_{};
    std::array<usize, max_irq_lines> serviced_{};
    usize pending_count_{0};
    usize spurious_{0};
    usize controller_errors_{0};
};

} // namespace ok::interrupt
