#pragma once

#include "ok/arch/arch.hpp"
#include "ok/core/types.hpp"

#include <array>
#include <concepts>
#include <memory>
#include <string_view>

namespace ok::interrupt {

using Vector = u16;
inline constexpr usize max_vectors = 256;

class InterruptHandler {
public:
    virtual ~InterruptHandler() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    virtual Status handle(arch::TrapFrame& frame) = 0;
};

template <typename F>
concept InterruptCallable = requires(F function, arch::TrapFrame& frame) {
    { function(frame) } -> std::same_as<Status>;
};

template <InterruptCallable F>
class FunctionInterruptHandler final : public InterruptHandler {
public:
    FunctionInterruptHandler(std::string_view handler_name, F function)
        : name_(handler_name), function_(std::move(function))
    {
    }

    [[nodiscard]] std::string_view name() const override { return name_; }
    Status handle(arch::TrapFrame& frame) override { return function_(frame); }

private:
    std::string_view name_;
    F function_;
};

class InterruptDispatcher final {
public:
    template <InterruptCallable F>
    Status register_function(Vector vector, std::string_view name, F function)
    {
        return register_handler(vector, std::make_unique<FunctionInterruptHandler<F>>(name, std::move(function)));
    }

    Status register_handler(Vector vector, std::unique_ptr<InterruptHandler> handler);
    Status dispatch(arch::TrapFrame& frame);

    [[nodiscard]] usize handled_count(Vector vector) const;
    [[nodiscard]] bool has_handler(Vector vector) const;

private:
    std::array<std::unique_ptr<InterruptHandler>, max_vectors> handlers_ {};
    std::array<usize, max_vectors> handled_counts_ {};
};

class InterruptController {
public:
    virtual ~InterruptController() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    virtual Status initialize() = 0;
    virtual Status enable(Vector vector) = 0;
    virtual Status disable(Vector vector) = 0;
};

class SimulatedInterruptController final : public InterruptController {
public:
    [[nodiscard]] std::string_view name() const override { return "simulated-interrupt-controller"; }
    Status initialize() override;
    Status enable(Vector vector) override;
    Status disable(Vector vector) override;
    [[nodiscard]] bool enabled(Vector vector) const;

private:
    std::array<bool, max_vectors> enabled_ {};
    bool initialized_ {false};
};

} // namespace ok::interrupt

