#include "ok/arch/arch.hpp"
#include "ok/core/entry.hpp"

namespace
{

extern "C" void ok_platform_console_init();
extern "C" void ok_platform_console_write_char(char value);
extern "C" void ok_platform_display_clear();
extern "C" void ok_platform_display_write_char(char value);
extern "C" void ok_platform_debug_exit(ok::u32 code);
extern "C" void ok_platform_halt();
extern "C" int ok_platform_input_poll();

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
    platform_write(text);
}

[[noreturn]] void halt_forever()
{
    for (;;)
    {
        ok_platform_halt();
    }
}

[[maybe_unused, noreturn]] void interactive_loop()
{
    platform_write("\nOK_INTERACTIVE ready keyboard=ps2 mouse=ps2\n[input] ");
    for (;;)
    {
        const int value = ok_platform_input_poll();
        if (value >= 0)
        {
            const char ch = static_cast<char>(value);
            if (ch == '\r' || ch == '\n')
            {
                platform_write("\n[input] ");
            }
            else
            {
                platform_write(std::string_view{&ch, 1});
            }
        }
        asm volatile("" ::: "memory");
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

extern "C" [[noreturn]] void kernel_main()
{
    ok_platform_console_init();
    ok_platform_display_clear();

    ok::KernelEntryConfig config{};
    config.kernel.architecture = ok::arch::configured_architecture();

#if defined(OK_ENABLE_TEST_POINTS)
    config.mode = ok::KernelBootMode::debug;
    config.debug = ok::KernelDebugSink{.context = nullptr, .write = debug_write};
#else
    config.mode = ok::KernelBootMode::normal;
#endif

    [[maybe_unused]] const auto status = ok::ok_kernel_entry(config);

#if defined(OK_ENABLE_TEST_POINTS)
    ok_platform_debug_exit(status.ok() ? 0x10u : 0x11u);
    if (status.ok())
    {
        interactive_loop();
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

extern "C" void __cxa_guard_abort(ok::u64 *) {}

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

void operator delete(void *) noexcept {}
void operator delete(void *, ok::usize) noexcept {}
void operator delete[](void *) noexcept {}
void operator delete[](void *, ok::usize) noexcept {}

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
