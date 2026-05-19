#include "ok/core/types.hpp"

// Freestanding C++ ABI/compiler helpers for okernel.
// This file is not a hosted runtime and must not grow into libc/libstdc++.

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

extern "C" ok::u64 __udivdi3(ok::u64 numerator, ok::u64 denominator)
{
    if (denominator == 0)
    {
        return 0;
    }

    ok::u64 quotient = 0;
    ok::u64 bit = 1;
    while (denominator < numerator && (denominator & (ok::u64{1} << 63)) == 0)
    {
        denominator <<= 1;
        bit <<= 1;
    }
    while (bit != 0)
    {
        if (numerator >= denominator)
        {
            numerator -= denominator;
            quotient |= bit;
        }
        denominator >>= 1;
        bit >>= 1;
    }
    return quotient;
}

extern "C" ok::u64 __umoddi3(ok::u64 numerator, ok::u64 denominator)
{
    if (denominator == 0)
    {
        return 0;
    }
    return numerator - __udivdi3(numerator, denominator) * denominator;
}

#if defined(__arm__)
extern "C" ok::u32 __aeabi_uidiv(ok::u32 numerator, ok::u32 denominator)
{
    if (denominator == 0)
    {
        return 0;
    }

    ok::u32 quotient = 0;
    ok::u32 bit = 1;
    while (denominator < numerator && (denominator & 0x8000'0000u) == 0)
    {
        denominator <<= 1;
        bit <<= 1;
    }
    while (bit != 0)
    {
        if (numerator >= denominator)
        {
            numerator -= denominator;
            quotient |= bit;
        }
        denominator >>= 1;
        bit >>= 1;
    }
    return quotient;
}

extern "C" [[gnu::naked]] void __aeabi_uidivmod()
{
    asm volatile(
        "push {r0, r1, lr}\n"
        "bl __aeabi_uidiv\n"
        "pop {r1, r2, lr}\n"
        "mul r3, r0, r2\n"
        "sub r1, r1, r3\n"
        "bx lr\n");
}

extern "C" [[gnu::naked]] void __aeabi_uldivmod()
{
    asm volatile(
        "push {r4, r5, r6, r7, r8, r9, lr}\n"
        "mov r4, r0\n"
        "mov r5, r1\n"
        "mov r6, r2\n"
        "mov r7, r3\n"
        "bl __udivdi3\n"
        "mov r8, r0\n"
        "mov r9, r1\n"
        "mov r0, r4\n"
        "mov r1, r5\n"
        "mov r2, r6\n"
        "mov r3, r7\n"
        "bl __umoddi3\n"
        "mov r2, r0\n"
        "mov r3, r1\n"
        "mov r0, r8\n"
        "mov r1, r9\n"
        "pop {r4, r5, r6, r7, r8, r9, pc}\n");
}
#endif

extern "C" int atexit(void (*)())
{
    return 0;
}

extern "C" void __cxa_pure_virtual()
{
    for (;;)
    {
    }
}

namespace std
{

[[noreturn]] void terminate() noexcept
{
    for (;;)
    {
    }
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
