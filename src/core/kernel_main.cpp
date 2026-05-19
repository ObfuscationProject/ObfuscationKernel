#include "ok/arch/arch.hpp"
#include "ok/core/entry.hpp"

namespace
{

constexpr ok::u16 com1 = 0x3f8;
constexpr ok::u16 debug_exit_port = 0x00f4;
constexpr ok::usize vga_columns = 80;
constexpr ok::usize vga_rows = 25;

ok::usize vga_row = 0;
ok::usize vga_column = 0;

void outb(ok::u16 port, ok::u8 value)
{
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

[[maybe_unused]] void outl(ok::u16 port, ok::u32 value)
{
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

ok::u8 inb(ok::u16 port)
{
    ok::u8 value = 0;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

void serial_init()
{
    outb(com1 + 1, 0x00);
    outb(com1 + 3, 0x80);
    outb(com1 + 0, 0x03);
    outb(com1 + 1, 0x00);
    outb(com1 + 3, 0x03);
    outb(com1 + 2, 0xc7);
    outb(com1 + 4, 0x0b);
}

void serial_write_char(char value)
{
    for (ok::usize attempt = 0; attempt < 100000; ++attempt)
    {
        if ((inb(com1 + 5) & 0x20u) != 0)
        {
            break;
        }
    }
    outb(com1, static_cast<ok::u8>(value));
}

volatile ok::u16 *vga_buffer()
{
    return reinterpret_cast<volatile ok::u16 *>(0xb8000);
}

void vga_clear()
{
    auto *buffer = vga_buffer();
    for (ok::usize i = 0; i < vga_columns * vga_rows; ++i)
    {
        buffer[i] = static_cast<ok::u16>(0x0f00u | ' ');
    }
    vga_row = 0;
    vga_column = 0;
}

void vga_newline()
{
    vga_column = 0;
    if (vga_row + 1 < vga_rows)
    {
        ++vga_row;
        return;
    }

    auto *buffer = vga_buffer();
    for (ok::usize row = 1; row < vga_rows; ++row)
    {
        for (ok::usize column = 0; column < vga_columns; ++column)
        {
            buffer[(row - 1) * vga_columns + column] = buffer[row * vga_columns + column];
        }
    }
    for (ok::usize column = 0; column < vga_columns; ++column)
    {
        buffer[(vga_rows - 1) * vga_columns + column] = static_cast<ok::u16>(0x0f00u | ' ');
    }
}

void vga_write_char(char value)
{
    if (value == '\n')
    {
        vga_newline();
        return;
    }
    if (value == '\r')
    {
        vga_column = 0;
        return;
    }

    auto *buffer = vga_buffer();
    buffer[vga_row * vga_columns + vga_column] = static_cast<ok::u16>(0x0f00u | static_cast<ok::u8>(value));
    ++vga_column;
    if (vga_column == vga_columns)
    {
        vga_newline();
    }
}

void platform_write(std::string_view text)
{
    for (const auto value : text)
    {
        serial_write_char(value);
        vga_write_char(value);
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
        asm volatile("hlt" ::: "memory");
    }
}

} // namespace

extern "C" void ok_platform_display_write_line(const char *text, ok::usize size)
{
    for (ok::usize i = 0; i < size; ++i)
    {
        vga_write_char(text[i]);
    }
    vga_write_char('\n');
}

extern "C" [[noreturn]] void kernel_main()
{
    serial_init();
    vga_clear();

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
    outl(debug_exit_port, status.ok() ? 0x10u : 0x11u);
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
