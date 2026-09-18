#include "ok/arch/arch.hpp"
#include "ok/core/entry.hpp"

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
extern "C" void ok_platform_reboot() __attribute__((weak));
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

[[maybe_unused, noreturn]] void desktop_loop(bool show_shell)
{
    if (show_shell)
    {
        static_cast<void>(ok::ok_debug_shell_show_gui());
    }
    ok::u64 ui_tick_counter = 0;
    for (;;)
    {
        switch (ok::ok_system_power_action())
        {
        case ok::SystemPowerAction::none:
            break;
        case ok::SystemPowerAction::reboot:
            if (ok_platform_reboot != nullptr)
            {
                ok_platform_reboot();
            }
            ok_platform_debug_exit(0x12u);
            halt_forever();
        case ok::SystemPowerAction::halt:
        case ok::SystemPowerAction::poweroff:
            ok_platform_debug_exit(0x10u);
            halt_forever();
        }

        static_cast<void>(ok::ok_step_userland());
        bool did_work = false;
        const int serial = ok_platform_console_poll != nullptr ? ok_platform_console_poll() : -1;
        if (serial >= 0)
        {
            if (native_userland_started)
                ok::ok_userland_input(static_cast<char>(serial == '\r' ? '\n' : serial));
            else
                static_cast<void>(ok::ok_gui_key_event(serial));
            did_work = true;
        }
        const int value = ok_platform_input_poll();
        if (value >= 0)
        {
            static_cast<void>(ok::ok_gui_key_event(value));
            did_work = true;
        }
        ++ui_tick_counter;
        if ((ui_tick_counter & 0x7ffu) == 0)
        {
            static_cast<void>(ok::ok_debug_shell_tick());
            did_work = true;
        }
        if (!did_work)
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

    [[maybe_unused]] auto status = ok::ok_kernel_entry(config);
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
            for (ok::usize i = 0; i < 8192 && ok::ok_step_userland(); ++i)
            {
            }
        }
    }

#if defined(OK_ENABLE_TEST_POINTS)
    ok_platform_debug_exit(status.ok() ? 0x10u : 0x11u);
    if (status.ok())
    {
        desktop_loop(false);
    }
#else
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
            desktop_loop(false);
        }
#if defined(OK_ENABLE_KERNEL_GUI)
        desktop_loop(true);
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
