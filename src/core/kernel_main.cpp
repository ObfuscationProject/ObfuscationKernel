#include "ok/arch/arch.hpp"
#include "ok/boot/boot_memory.hpp"
#include "ok/core/entry.hpp"
#include "ok/core/native_shutdown.hpp"
#include "ok/interrupt/platform.hpp"
#include "ok/arch/terminal_power.hpp"
#include "ok/user/runtime.hpp"

#include <string_view>

namespace
{

extern "C" void ok_platform_console_init();
extern "C" void ok_platform_console_write_char(char value);
extern "C" void ok_platform_display_clear();
extern "C" void ok_platform_display_write_char(char value);
extern "C" void ok_platform_debug_exit(ok::u32 code);
extern "C" void ok_platform_halt();
extern "C" void ok_platform_poll_idle() __attribute__((weak));
extern "C" int ok_platform_input_poll();
extern "C" int ok_platform_console_poll() __attribute__((weak));
bool native_userland_started{};

void platform_write(std::string_view text)
{
    for (const auto value : text)
    {
        ok_platform_console_write_char(value);
        ok_platform_display_write_char(value);
    }
}

[[maybe_unused]] void debug_write(void *, std::string_view text)
{
    for (char value : text)
        ok_platform_console_write_char(value);
}

// The interrupt report before the shutdown line, once the service runs.
void report_interrupts()
{
    if (native_userland_started)
        ok::user::native::report_interrupts();
}

[[noreturn]] void halt_forever()
{
    for (;;)
    {
        ok_platform_halt();
    }
}

void desktop_idle()
{
    if (ok_platform_poll_idle != nullptr)
    {
        ok_platform_poll_idle();
    }
}

} // namespace

namespace ok::detail
{
[[maybe_unused, noreturn]] void desktop_loop(bool show_shell)
{
    if (show_shell)
    {
        static_cast<void>(ok::ok_debug_shell_show_gui());
    }
    const auto shutdown_boundary = [] {
        NativeShutdownBoundary boundary;
        auto action = ok_advance_native_shutdown(boundary);
        if (!action)
        {
            platform_write("OK_SHUTDOWN failed=");
            platform_write(action.status().message());
            platform_write("\n");
            arch::terminal_power_action(SystemPowerAction::none);
        }
        if (action.value() != SystemPowerAction::none)
        {
            report_interrupts();
            if (action.value() == SystemPowerAction::halt)
                platform_write("OK_SHUTDOWN clean=1 action=halt\n");
            else if (action.value() == SystemPowerAction::reboot)
                platform_write("OK_SHUTDOWN clean=1 action=reboot\n");
            else if (action.value() == SystemPowerAction::poweroff)
                platform_write("OK_SHUTDOWN clean=1 action=poweroff\n");
            else
                arch::terminal_power_action(SystemPowerAction::none);
            arch::terminal_power_action(action.value());
        }
    };
    // Device interrupts and the idle wait need the native backend's trap
    // vectors, so they start only with native userland.
    if (native_userland_started)
        if (auto status = ok::interrupt::start_platform_irqs(); !status.ok())
        {
            platform_write("OK_IRQ_FAIL ");
            platform_write(status.message());
            platform_write("\n");
        }
    ok::u64 ui_tick_counter = 0;
    for (;;)
    {
        shutdown_boundary();
        static_cast<void>(ok::interrupt::drain_platform_irqs());
        // Retain link failure in the executor; unrelated tasks keep running.
        static_cast<void>(ok::ok_poll_network());
        static_cast<void>(ok::ok_step_userland());
        shutdown_boundary();
        if (!ok::ok_userland_status().ok())
        {
            platform_write("OK_TEST_FAIL native-runtime=");
            platform_write(ok::ok_userland_status().message());
            platform_write("\n");
            ok_platform_debug_exit(0x11u);
            halt_forever();
        }
        bool did_work = false;
        // A driven source is read only after its interrupt posted it; the
        // platform posts it again while bytes remain.
        using ok::interrupt::EventSource;
        if (ok::interrupt::take_event(EventSource::console))
        {
            const int serial = ok_platform_console_poll != nullptr ? ok_platform_console_poll() : -1;
            if (serial >= 0)
            {
                if (native_userland_started)
                    ok::ok_userland_input(static_cast<char>(serial == '\r' ? '\n' : serial),
                                          ok::interrupt::event_wake_source(EventSource::console));
                else
                    static_cast<void>(ok::ok_gui_key_event(serial));
                did_work = true;
            }
        }
        if (ok::interrupt::take_event(EventSource::input))
        {
            const int value = ok_platform_input_poll();
            if (value >= 0)
            {
                static_cast<void>(ok::ok_gui_key_event(value));
                did_work = true;
            }
        }
        ++ui_tick_counter;
        if ((ui_tick_counter & 0x7ffu) == 0)
        {
            static_cast<void>(ok::ok_debug_shell_tick());
            did_work = true;
        }
        // With nothing runnable and no event left to serve, wait for a tick or
        // a device line instead of spinning; profiles without a controller
        // driver keep polling.
        if (!did_work && !ok::interrupt::events_posted() &&
            !(native_userland_started && ok::ok_userland_idle() && ok::interrupt::wait_for_interrupt()))
        {
            desktop_idle();
        }
    }
}

} // namespace

extern "C" void ok_platform_display_write_line(const char *text, ok::usize size)
{
    for (ok::usize i = 0; i < size; ++i)
    {
        ok_platform_display_write_char(text[i]);
    }
    ok_platform_display_write_char('\n');
}

extern "C" ok::KernelExtension *ok_boot_extension() __attribute__((weak));
extern "C" ok::fs::BootArchive ok_boot_archive() __attribute__((weak));

extern "C" [[noreturn]] void kernel_main()
{
    ok_platform_console_init();
    ok_platform_display_clear();

    ok::KernelEntryConfig config{};
    config.kernel.architecture = ok::arch::configured_architecture();
    config.kernel.extension = ok_boot_extension != nullptr ? ok_boot_extension() : nullptr;

#if defined(OK_ENABLE_TEST_POINTS)
    config.mode = ok::KernelBootMode::debug;
    config.debug = ok::KernelDebugSink{.context = nullptr, .write = debug_write};
#else
    config.mode = ok::KernelBootMode::normal;
#endif

    // Boot memory is discovered once, before any translation changes.
    [[maybe_unused]] auto status = ok::user::native::prepare_memory();
    if (const auto &boot_memory = ok::boot::kernel_boot_memory(); status.ok() && boot_memory.map != nullptr)
    {
        config.kernel.physical_memory = boot_memory.map;
        config.kernel.direct_map_offset = boot_memory.direct_offset;
    }
    if (status.ok()) status = ok::ok_kernel_entry(config);
    if (status.ok() && ok_boot_archive != nullptr)
    {
        auto native = ok::ok_start_userland(ok_boot_archive());
        if (!native.ok())
        {
            platform_write("OK_NATIVE startup=");
            platform_write(native.message());
            platform_write("\n");
            if (native.code() != ok::StatusCode::unsupported)
                status = native;
        }
        else
        {
            native_userland_started = true;
        }
    }

#if defined(OK_ENABLE_TEST_POINTS)
    // Native userland is driven with console/input and GUI services. It may
    // contain persistent runnable tasks or deadlines, so never drain it first.
    if (status.ok() && native_userland_started)
        ok::detail::desktop_loop(false);
    ok_platform_debug_exit(status.ok() ? 0x10u : 0x11u);
    if (status.ok())
    {
        ok::detail::desktop_loop(false);
    }
#else
    // Core initialization observation precedes extension/session startup.
    // This uses the existing startup status and writes only to the console.
    debug_write(nullptr, "OK_KERNEL_BOOT scope=kernel mode=release arch=");
    debug_write(nullptr, ok::arch::ArchOperations{}.name());
    debug_write(nullptr, status.ok() ? " status=ready\n" : " status=failed\n");
    if (status.ok())
    {
        if (config.kernel.extension != nullptr)
        {
            status = ok::ok_start_boot_extension();
            for (char value : std::string_view{status.ok() ? "OK_BOOT_EXTENSION boot=complete\n"
                                                           : "OK_BOOT_EXTENSION boot=failed\n"})
                ok_platform_console_write_char(value);
            if (!status.ok())
            {
                ok_platform_debug_exit(0x11u);
                halt_forever();
            }
            ok::detail::desktop_loop(false);
        }
#if defined(OK_ENABLE_KERNEL_GUI)
        ok::detail::desktop_loop(true);
#endif
    }
#endif

    halt_forever();
}

extern "C" void *memcpy(void *destination, const void *source, ok::usize count)
{
    auto *out = static_cast<ok::u8 *>(destination);
    const auto *in = static_cast<const ok::u8 *>(source);
    for (ok::usize i = 0; i < count; ++i)
    {
        out[i] = in[i];
    }
    return destination;
}

extern "C" void *memmove(void *destination, const void *source, ok::usize count)
{
    auto *out = static_cast<ok::u8 *>(destination);
    const auto *in = static_cast<const ok::u8 *>(source);
    if (out < in)
    {
        for (ok::usize i = 0; i < count; ++i)
        {
            out[i] = in[i];
        }
    }
    else
    {
        for (ok::usize i = count; i != 0; --i)
        {
            out[i - 1] = in[i - 1];
        }
    }
    return destination;
}

extern "C" void *memset(void *destination, int value, ok::usize count)
{
    auto *out = static_cast<ok::u8 *>(destination);
    for (ok::usize i = 0; i < count; ++i)
    {
        out[i] = static_cast<ok::u8>(value);
    }
    return destination;
}

extern "C" int memcmp(const void *lhs, const void *rhs, ok::usize count)
{
    const auto *left = static_cast<const ok::u8 *>(lhs);
    const auto *right = static_cast<const ok::u8 *>(rhs);
    for (ok::usize i = 0; i < count; ++i)
    {
        if (left[i] != right[i])
        {
            return left[i] < right[i] ? -1 : 1;
        }
    }
    return 0;
}

extern "C" void *memchr(const void *memory, int value, ok::usize count)
{
    const auto *bytes = static_cast<const ok::u8 *>(memory);
    const auto needle = static_cast<ok::u8>(value);
    for (ok::usize i = 0; i < count; ++i)
    {
        if (bytes[i] == needle)
        {
            return const_cast<ok::u8 *>(&bytes[i]);
        }
    }
    return nullptr;
}

extern "C" ok::usize strlen(const char *text)
{
    ok::usize size = 0;
    while (text[size] != '\0')
    {
        ++size;
    }
    return size;
}

extern "C" int atexit(void (*)())
{
    return 0;
}

extern "C" int __cxa_atexit(void (*)(void *), void *, void *)
{
    return 0;
}

extern "C"
{
    void *__dso_handle = nullptr;
}

extern "C" int __cxa_guard_acquire(ok::u64 *guard)
{
    return (*guard & 1u) == 0 ? 1 : 0;
}

extern "C" void __cxa_guard_release(ok::u64 *guard)
{
    *guard |= 1u;
}

extern "C" void __cxa_guard_abort(ok::u64 *)
{
}

extern "C" void __cxa_pure_virtual()
{
    halt_forever();
}

namespace std
{

[[noreturn]] void terminate() noexcept
{
    halt_forever();
}

} // namespace std

void operator delete(void *) noexcept
{
}
void operator delete(void *, ok::usize) noexcept
{
}
void operator delete[](void *) noexcept
{
}
void operator delete[](void *, ok::usize) noexcept
{
}

namespace __cxxabiv1
{

class __class_type_info
{
  public:
    virtual ~__class_type_info();
};

class __si_class_type_info : public __class_type_info
{
  public:
    ~__si_class_type_info() override;
};

class __vmi_class_type_info : public __class_type_info
{
  public:
    ~__vmi_class_type_info() override;
};

__class_type_info::~__class_type_info() = default;
__si_class_type_info::~__si_class_type_info() = default;
__vmi_class_type_info::~__vmi_class_type_info() = default;

} // namespace __cxxabiv1
