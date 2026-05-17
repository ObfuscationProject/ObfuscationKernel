#pragma once

#include "ok/core/concepts.hpp"
#include "ok/core/types.hpp"

#include <concepts>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ok::driver {

enum class Class : u8 {
    console,
    timer,
    block,
    network,
    input,
    entropy,
};

class Driver {
public:
    virtual ~Driver() = default;
    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual Class driver_class() const = 0;
    virtual Status probe() = 0;
    virtual Status start() = 0;
    virtual Status stop() = 0;
};

template <typename T>
concept KernelDriver = std::derived_from<T, Driver>;

class DriverManager final {
public:
    template <KernelDriver D, typename... Args>
    D& add(Args&&... args)
    {
        auto driver = std::make_unique<D>(std::forward<Args>(args)...);
        auto& ref = *driver;
        drivers_.push_back(std::move(driver));
        return ref;
    }

    Status start_all();
    [[nodiscard]] Driver* find(Class driver_class);
    [[nodiscard]] const std::vector<std::unique_ptr<Driver>>& drivers() const { return drivers_; }

private:
    std::vector<std::unique_ptr<Driver>> drivers_;
};

class ConsoleDriver final : public Driver {
public:
    [[nodiscard]] std::string_view name() const override { return "kernel-console"; }
    [[nodiscard]] Class driver_class() const override { return Class::console; }
    Status probe() override;
    Status start() override;
    Status stop() override;
    Status write(std::string_view text);
    [[nodiscard]] std::string_view buffer() const { return buffer_; }

private:
    bool started_ {false};
    std::string buffer_;
};

class TimerDriver final : public Driver {
public:
    [[nodiscard]] std::string_view name() const override { return "kernel-timer"; }
    [[nodiscard]] Class driver_class() const override { return Class::timer; }
    Status probe() override;
    Status start() override;
    Status stop() override;
    void tick() { ++ticks_; }
    [[nodiscard]] u64 ticks() const { return ticks_; }

private:
    bool started_ {false};
    u64 ticks_ {0};
};

class NullBlockDriver final : public Driver {
public:
    [[nodiscard]] std::string_view name() const override { return "null-block"; }
    [[nodiscard]] Class driver_class() const override { return Class::block; }
    Status probe() override;
    Status start() override;
    Status stop() override;
    Status read(uptr, std::span<std::byte> out);
    Status write(uptr, std::span<const std::byte> in);

private:
    bool started_ {false};
};

} // namespace ok::driver

