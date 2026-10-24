#pragma once

#include "ok/arch/arch.hpp"

#if !defined(__GNUC__) && !defined(__clang__)
#error "Instantaneous SP observation requires GNU-compatible extended assembly"
#endif
#if !defined(OK_KERNEL_FREESTANDING) || !defined(__STDC_HOSTED__) || __STDC_HOSTED__ != 0
#error "Instantaneous SP observation is a freestanding Kernel mechanism"
#endif
#if !defined(__CHAR_BIT__) || __CHAR_BIT__ != 8
#error "Instantaneous SP profiles require eight-bit bytes"
#endif
#if (defined(OK_ARCH_TARGET_I386) + defined(OK_ARCH_TARGET_X86_64) + \
     defined(OK_ARCH_TARGET_AARCH64) + defined(OK_ARCH_TARGET_ARM32) + \
     defined(OK_ARCH_TARGET_RV64) + defined(OK_ARCH_TARGET_RV32) + \
     defined(OK_ARCH_TARGET_LOONGARCH64) + defined(OK_ARCH_TARGET_MIPS) + \
     defined(OK_ARCH_TARGET_MIPS64) + defined(OK_ARCH_TARGET_PPC)) != 1
#error "Instantaneous SP observation requires exactly one explicit Kernel profile"
#endif
#if !defined(__BYTE_ORDER__) || !defined(__ORDER_LITTLE_ENDIAN__) || !defined(__ORDER_BIG_ENDIAN__)
#error "Instantaneous SP observation requires explicit compiler byte-order evidence"
#endif

#if defined(OK_ARCH_TARGET_I386)
#if !defined(__i386__) || __SIZEOF_POINTER__ != 4
#error "The i386 SP profile requires native i386 with 32-bit pointers"
#endif
#elif defined(OK_ARCH_TARGET_X86_64)
#if !defined(__x86_64__) || __SIZEOF_POINTER__ != 8
#error "The x86_64 SP profile requires native x86_64 with 64-bit pointers"
#endif
#elif defined(OK_ARCH_TARGET_AARCH64)
#if !defined(__aarch64__) || __SIZEOF_POINTER__ != 8
#error "The aarch64 SP profile requires native AArch64 with 64-bit pointers"
#endif
#elif defined(OK_ARCH_TARGET_ARM32)
#if !defined(__arm__) || defined(__aarch64__) || defined(__thumb__) || __SIZEOF_POINTER__ != 4
#error "The arm32 SP profile requires native 32-bit ARM state, matching -marm"
#endif
#elif defined(OK_ARCH_TARGET_RV64)
#if !defined(__riscv) || !defined(__riscv_xlen) || __riscv_xlen != 64 || __SIZEOF_POINTER__ != 8
#error "The rv64 SP profile requires native RV64 with 64-bit pointers"
#endif
#elif defined(OK_ARCH_TARGET_RV32)
#if !defined(__riscv) || !defined(__riscv_xlen) || __riscv_xlen != 32 || __SIZEOF_POINTER__ != 4
#error "The rv32 SP profile requires native RV32 with 32-bit pointers"
#endif
#elif defined(OK_ARCH_TARGET_LOONGARCH64)
#if !defined(__loongarch__) || !defined(__loongarch64) || __SIZEOF_POINTER__ != 8
#error "The loongarch64 SP profile requires native LoongArch64 with 64-bit pointers"
#endif
#elif defined(OK_ARCH_TARGET_MIPS)
#if !defined(__mips__) || defined(__mips64) || !defined(_MIPS_SIM) || !defined(_ABIO32) || \
    _MIPS_SIM != _ABIO32 || __SIZEOF_POINTER__ != 4
#error "The mips SP profile requires native MIPS o32 with 32-bit pointers"
#endif
#elif defined(OK_ARCH_TARGET_MIPS64)
#if !defined(__mips__) || !defined(__mips64) || !defined(_MIPS_SIM) || !defined(_ABI64) || \
    _MIPS_SIM != _ABI64 || __SIZEOF_POINTER__ != 8
#error "The mips64 SP profile requires native MIPS n64 with 64-bit pointers"
#endif
#elif defined(OK_ARCH_TARGET_PPC)
#if (!defined(__powerpc__) && !defined(__PPC__)) || defined(__powerpc64__) || __SIZEOF_POINTER__ != 4
#error "The ppc SP profile requires native PPC32 with 32-bit pointers"
#endif
#endif

namespace ok::arch
{

template <Architecture A> struct StackPointerProfile;

#define OK_INSTANTANEOUS_SP_PROFILE(target, bytes, register_text) \
    template <> struct StackPointerProfile<Architecture::target> \
    { \
        static constexpr usize pointer_bytes = bytes; \
        static constexpr std::string_view native_register = register_text; \
    }

OK_INSTANTANEOUS_SP_PROFILE(i386, 4, "esp");
OK_INSTANTANEOUS_SP_PROFILE(x86_64, 8, "rsp");
OK_INSTANTANEOUS_SP_PROFILE(aarch64, 8, "sp (currently selected SP bank)");
OK_INSTANTANEOUS_SP_PROFILE(arm32, 4, "r13/sp (current processor mode)");
OK_INSTANTANEOUS_SP_PROFILE(rv64, 8, "x2/sp");
OK_INSTANTANEOUS_SP_PROFILE(rv32, 4, "x2/sp");
OK_INSTANTANEOUS_SP_PROFILE(loongarch64, 8, "r3/sp");
OK_INSTANTANEOUS_SP_PROFILE(mips, 4, "r29/sp");
OK_INSTANTANEOUS_SP_PROFILE(mips64, 8, "r29/sp");
OK_INSTANTANEOUS_SP_PROFILE(ppc, 4, "r1");
#undef OK_INSTANTANEOUS_SP_PROFILE

static_assert(sizeof(uptr) == sizeof(void *));
static_assert(sizeof(uptr) == StackPointerProfile<configured_architecture()>::pointer_bytes);
static_assert((__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ &&
               ArchTraits<configured_architecture()>::endianness == Endianness::little) ||
              (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__ &&
               ArchTraits<configured_architecture()>::endianness == Endianness::big),
              "Native byte order must match the explicit Kernel profile");

namespace detail
{

template <Architecture A> struct NativeStackPointerInstructions;

template <> struct NativeStackPointerInstructions<configured_architecture()> final
{
    static constexpr Architecture architecture = configured_architecture();

    // The value belongs to this instruction, including any compiler frame.
    [[nodiscard, gnu::always_inline]] static inline uptr read() noexcept
    {
        uptr value;
#if defined(OK_ARCH_TARGET_I386)
        asm volatile("mov{l %%esp, %0| %0, esp}" : "=r"(value) : : "memory");
#elif defined(OK_ARCH_TARGET_X86_64)
        asm volatile("mov{q %%rsp, %0| %0, rsp}" : "=r"(value) : : "memory");
#elif defined(OK_ARCH_TARGET_AARCH64)
        asm volatile("mov %0, sp" : "=r"(value) : : "memory");
#elif defined(OK_ARCH_TARGET_ARM32)
        asm volatile("mov %0, sp" : "=r"(value) : : "memory");
#elif defined(OK_ARCH_TARGET_RV64) || defined(OK_ARCH_TARGET_RV32)
        asm volatile("mv %0, sp" : "=r"(value) : : "memory");
#elif defined(OK_ARCH_TARGET_LOONGARCH64)
        asm volatile("move %0, $sp" : "=r"(value) : : "memory");
#elif defined(OK_ARCH_TARGET_MIPS) || defined(OK_ARCH_TARGET_MIPS64)
        asm volatile("move %0, $29" : "=r"(value) : : "memory");
#elif defined(OK_ARCH_TARGET_PPC)
        asm volatile("mr %0, 1" : "=r"(value) : : "memory");
#endif
        return value;
    }
};

} // namespace detail

template <typename T>
concept InstantaneousStackPointerInstructions = requires {
    { T::architecture } -> std::convertible_to<Architecture>;
    { T::read() } noexcept -> std::same_as<uptr>;
};

template <Architecture A>
    requires (A == configured_architecture()) &&
             InstantaneousStackPointerInstructions<detail::NativeStackPointerInstructions<A>>
class InstantaneousStackPointerReader final
{
  public:
    [[nodiscard, gnu::always_inline]] static inline uptr read() noexcept
    {
        return detail::NativeStackPointerInstructions<A>::read();
    }
};

using ConfiguredStackPointerReader = InstantaneousStackPointerReader<configured_architecture()>;
static_assert(!std::is_polymorphic_v<ConfiguredStackPointerReader>);

// No range, domain, CPU identity or high-water conclusion is attached to this number.
[[nodiscard, gnu::always_inline]] inline uptr read_instantaneous_stack_pointer() noexcept
{
    return ConfiguredStackPointerReader::read();
}

} // namespace ok::arch
