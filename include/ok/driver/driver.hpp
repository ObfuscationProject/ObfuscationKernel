#pragma once

#include "ok/core/concepts.hpp"
#include "ok/core/fixed.hpp"
#include "ok/core/types.hpp"

#include <concepts>
#include <span>
#include <string_view>

namespace ok::driver {

enum class Class : u8 {
    console,
    timer,
    block,
    display,
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

inline constexpr usize max_drivers = 32;
inline constexpr usize console_buffer_size = 4096;
inline constexpr usize framebuffer_width = 160;
inline constexpr usize framebuffer_height = 100;
inline constexpr usize framebuffer_pixels = framebuffer_width * framebuffer_height;

class DriverManager final {
public:
    Status add(Driver& driver);

    Status start_all();
    [[nodiscard]] Driver* find(Class driver_class);
    [[nodiscard]] usize driver_count() const { return drivers_.size(); }

private:
    StaticVector<Driver*, max_drivers> drivers_;
};

class ConsoleDriver final : public Driver {
public:
    [[nodiscard]] std::string_view name() const override { return "kernel-console"; }
    [[nodiscard]] Class driver_class() const override { return Class::console; }
    Status probe() override;
    Status start() override;
    Status stop() override;
    Status write(std::string_view text);
    [[nodiscard]] std::string_view buffer() const { return {buffer_.data(), buffer_size_}; }

private:
    bool started_ {false};
    std::array<char, console_buffer_size> buffer_ {};
    usize buffer_size_ {0};
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

struct DisplayMode {
    u32 width {framebuffer_width};
    u32 height {framebuffer_height};
    u32 pitch {framebuffer_width * sizeof(u32)};
    u8 bits_per_pixel {32};
};

class FramebufferDisplayDriver final : public Driver {
public:
    [[nodiscard]] std::string_view name() const override { return "simple-framebuffer"; }
    [[nodiscard]] Class driver_class() const override { return Class::display; }
    Status probe() override;
    Status start() override;
    Status stop() override;
    Status clear(u32 rgba);
    Status put_pixel(u32 x, u32 y, u32 rgba);
    Status fill_rect(u32 x, u32 y, u32 width, u32 height, u32 rgba);
    [[nodiscard]] DisplayMode mode() const { return mode_; }
    [[nodiscard]] u64 checksum() const;

private:
    bool started_ {false};
    DisplayMode mode_ {};
    std::array<u32, framebuffer_pixels> pixels_ {};
};

} // namespace ok::driver
