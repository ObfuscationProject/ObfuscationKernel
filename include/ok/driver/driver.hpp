#pragma once

#include "ok/core/concepts.hpp"
#include "ok/core/fixed.hpp"
#include "ok/core/types.hpp"

#include <array>
#include <concepts>
#include <span>
#include <string_view>

namespace ok::driver
{

enum class Class : u8
{
    console,
    timer,
    block,
    display,
    network,
    input,
    entropy,
};

class Driver
{
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
inline constexpr usize display_text_columns = 80;
inline constexpr usize display_text_rows = 25;
inline constexpr usize display_text_buffer_size = (display_text_columns + 1) * display_text_rows;
inline constexpr usize input_queue_capacity = 128;

enum class IoMode : u8
{
    polling,
    interrupt,
    dma,
};

class DriverManager final
{
  public:
    Status add(Driver &driver);

    Status start_all();
    [[nodiscard]] Driver *find(Class driver_class);
    [[nodiscard]] usize driver_count() const
    {
        return drivers_.size();
    }

  private:
    StaticVector<Driver *, max_drivers> drivers_;
};

class ConsoleDriver final : public Driver
{
  public:
    [[nodiscard]] std::string_view name() const override
    {
        return "kernel-console";
    }
    [[nodiscard]] Class driver_class() const override
    {
        return Class::console;
    }
    Status probe() override;
    Status start() override;
    Status stop() override;
    Status write(std::string_view text);
    [[nodiscard]] std::string_view buffer() const
    {
        return {buffer_.data(), buffer_size_};
    }

  private:
    bool started_{false};
    std::array<char, console_buffer_size> buffer_{};
    usize buffer_size_{0};
};

class TimerDriver final : public Driver
{
  public:
    [[nodiscard]] std::string_view name() const override
    {
        return "kernel-timer";
    }
    [[nodiscard]] Class driver_class() const override
    {
        return Class::timer;
    }
    Status probe() override;
    Status start() override;
    Status stop() override;
    void tick()
    {
        ++ticks_;
    }
    [[nodiscard]] u64 ticks() const
    {
        return ticks_;
    }

  private:
    bool started_{false};
    u64 ticks_{0};
};

class NullBlockDriver final : public Driver
{
  public:
    [[nodiscard]] std::string_view name() const override
    {
        return "null-block";
    }
    [[nodiscard]] Class driver_class() const override
    {
        return Class::block;
    }
    Status probe() override;
    Status start() override;
    Status stop() override;
    Status read(uptr, std::span<std::byte> out);
    Status write(uptr, std::span<const std::byte> in);

  private:
    bool started_{false};
};

struct KeyEvent
{
    u8 scancode{0};
    char ascii{0};
    bool pressed{false};
};

class KeyboardDriver final : public Driver
{
  public:
    [[nodiscard]] std::string_view name() const override
    {
        return "ps2-keyboard";
    }
    [[nodiscard]] Class driver_class() const override
    {
        return Class::input;
    }
    Status probe() override;
    Status start() override;
    Status stop() override;
    Status feed_scancode(u8 scancode);
    Result<KeyEvent> read_event();
    [[nodiscard]] IoMode mode() const
    {
        return mode_;
    }
    void set_mode(IoMode mode)
    {
        mode_ = mode;
    }
    [[nodiscard]] usize queued() const
    {
        return events_.size();
    }

  private:
    [[nodiscard]] char translate(u8 scancode) const;

    bool started_{false};
    bool left_shift_{false};
    bool right_shift_{false};
    IoMode mode_{IoMode::polling};
    StaticQueue<KeyEvent, input_queue_capacity> events_{};
};

struct MousePacket
{
    i8 delta_x{0};
    i8 delta_y{0};
    bool left_button{false};
    bool right_button{false};
    bool middle_button{false};
};

class Ps2MouseDriver final : public Driver
{
  public:
    [[nodiscard]] std::string_view name() const override
    {
        return "ps2-mouse";
    }
    [[nodiscard]] Class driver_class() const override
    {
        return Class::input;
    }
    Status probe() override;
    Status start() override;
    Status stop() override;
    Status feed_packet(MousePacket packet);
    Result<MousePacket> read_packet();
    [[nodiscard]] IoMode mode() const
    {
        return mode_;
    }
    void set_mode(IoMode mode)
    {
        mode_ = mode;
    }

  private:
    bool started_{false};
    IoMode mode_{IoMode::polling};
    StaticQueue<MousePacket, input_queue_capacity> packets_{};
};

struct DisplayMode
{
    u32 width{framebuffer_width};
    u32 height{framebuffer_height};
    u32 pitch{framebuffer_width * sizeof(u32)};
    u8 bits_per_pixel{32};
};

class FramebufferDisplayDriver final : public Driver
{
  public:
    [[nodiscard]] std::string_view name() const override
    {
        return "simple-framebuffer";
    }
    [[nodiscard]] Class driver_class() const override
    {
        return Class::display;
    }
    Status probe() override;
    Status start() override;
    Status stop() override;
    Status clear(u32 rgba);
    Status put_pixel(u32 x, u32 y, u32 rgba);
    Status fill_rect(u32 x, u32 y, u32 width, u32 height, u32 rgba);
    Status write_line(std::string_view text);
    [[nodiscard]] DisplayMode mode() const
    {
        return mode_;
    }
    [[nodiscard]] std::string_view text() const
    {
        return {text_.data(), text_size_};
    }
    [[nodiscard]] u64 checksum() const;

  private:
    void draw_cell(u32 column, u32 row, char value);

    bool started_{false};
    DisplayMode mode_{};
    std::array<u32, framebuffer_pixels> pixels_{};
    std::array<char, display_text_buffer_size> text_{};
    usize text_size_{0};
    u32 cursor_row_{0};
};

} // namespace ok::driver
