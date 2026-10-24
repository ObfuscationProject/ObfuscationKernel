#include "ok/arch/service_stack.hpp"

using namespace ok;
namespace
{
volatile u32 observed_checks{};

#if defined(OK_ARCH_TARGET_I386) || defined(OK_ARCH_TARGET_X86_64)
void out(u16 port, u8 value) { asm volatile("outb %0, %1" : : "a"(value), "Nd"(port)); }
u8 in(u16 port)
{
    u8 value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
void emit(const char *text)
{
    while (*text)
    {
        while (!(in(0x3fd) & 0x20)) {}
        out(0x3f8, static_cast<u8>(*text++));
    }
}
[[noreturn]] void finish(bool pass)
{
    emit(pass ? "PASS service-stack native=1 guards=0\n" : "FAIL service-stack\n");
    asm volatile("outl %0, %1" : : "a"(pass ? 0x10u : 0x11u), "Nd"(u16{0xf4}));
    for (;;) asm volatile("cli; hlt");
}
#else
// Non-x86 rows compile and link; this fixture does not claim their execution.
[[noreturn]] void finish(bool) { for (;;) asm volatile("" : : : "memory"); }
#endif
void check(bool pass)
{
    observed_checks = observed_checks + 1;
    if (!pass) finish(false);
}
[[gnu::noinline]] void nested_observation(uptr caller_sp)
{
    // Keep a real nested frame live while sampling, rather than treating a
    // local address or frame pointer as an instantaneous SP observation.
    volatile u8 live[512]{};
    live[0] = 17;
    live[511] = 23;
    const auto sp = arch::read_instantaneous_stack_pointer();
    check(sp < caller_sp);
    check(arch::linked_service_stack_bounds().contains(sp));
    check(arch::require_service_stack().ok());
    check(live[0] == 17 && live[511] == 23);
}
} // namespace

extern "C" void ok_boot_capture(uptr, uptr, uptr, uptr) {}
extern "C" void ok_arm32_boot_capture(uptr, uptr, uptr) {}
extern "C" usize strlen(const char *text)
{
    usize count = 0;
    while (text[count]) ++count;
    return count;
}
extern "C" void *memset(void *destination, int byte, usize count)
{
    auto *out = static_cast<volatile u8 *>(destination);
    for (usize i = 0; i < count; ++i) out[i] = static_cast<u8>(byte);
    return destination;
}
extern "C" void *memcpy(void *destination, const void *source, usize count)
{
    auto *out = static_cast<volatile u8 *>(destination);
    auto *in = static_cast<const volatile u8 *>(source);
    for (usize i = 0; i < count; ++i) out[i] = in[i];
    return destination;
}
extern "C" [[noreturn]] void kernel_main()
{
    const auto bounds = arch::linked_service_stack_bounds();
    const auto sp = arch::read_instantaneous_stack_pointer();
    check(bounds.valid());
    check(bounds.top - bounds.lower == 1048576);
    check(bounds.contains(sp));
    check(arch::require_service_stack().ok());
    check(!arch::require_service_stack(1048577));
    check(bounds.require(bounds.lower - 1, 0).code() == StatusCode::fault);
    check(bounds.require(bounds.top + 1, 0).code() == StatusCode::fault);
    check(bounds.require(bounds.lower, 1).code() == StatusCode::no_memory);
    nested_observation(sp);
    check(observed_checks == 12);
    finish(true);
}
