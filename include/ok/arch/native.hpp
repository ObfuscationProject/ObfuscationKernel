#pragma once

#include "ok/arch/types.hpp"

namespace ok::arch::detail
{

// Native instruction entry points are selected by template argument, never by a vtable.
template <Architecture A> struct NativeInstructions;

template <> struct NativeInstructions<Architecture::i386>
{
    static u64 read_cycle_counter() noexcept;
    static void memory_fence() noexcept;
    static void enable_interrupts() noexcept;
    static void disable_interrupts() noexcept;
    static void halt() noexcept;
};

template <> struct NativeInstructions<Architecture::x86_64>
{
    static u64 read_cycle_counter() noexcept;
    static void memory_fence() noexcept;
    static void enable_interrupts() noexcept;
    static void disable_interrupts() noexcept;
    static void halt() noexcept;
};

template <> struct NativeInstructions<Architecture::aarch64>
{
    static u64 read_cycle_counter() noexcept;
    static void memory_fence() noexcept;
    static void enable_interrupts() noexcept;
    static void disable_interrupts() noexcept;
    static void halt() noexcept;
};

template <> struct NativeInstructions<Architecture::arm32>
{
    static u64 read_cycle_counter() noexcept;
    static void memory_fence() noexcept;
    static void enable_interrupts() noexcept;
    static void disable_interrupts() noexcept;
    static void halt() noexcept;
};

template <> struct NativeInstructions<Architecture::rv64>
{
    static u64 read_cycle_counter() noexcept;
    static void memory_fence() noexcept;
    static void enable_interrupts() noexcept;
    static void disable_interrupts() noexcept;
    static void halt() noexcept;
};

template <> struct NativeInstructions<Architecture::rv32>
{
    static u64 read_cycle_counter() noexcept;
    static void memory_fence() noexcept;
    static void enable_interrupts() noexcept;
    static void disable_interrupts() noexcept;
    static void halt() noexcept;
};

template <> struct NativeInstructions<Architecture::loongarch64>
{
    static u64 read_cycle_counter() noexcept;
    static void memory_fence() noexcept;
    static void enable_interrupts() noexcept;
    static void disable_interrupts() noexcept;
    static void halt() noexcept;
};

template <> struct NativeInstructions<Architecture::mips>
{
    static u64 read_cycle_counter() noexcept;
    static void memory_fence() noexcept;
    static void enable_interrupts() noexcept;
    static void disable_interrupts() noexcept;
    static void halt() noexcept;
};

template <> struct NativeInstructions<Architecture::mips64>
{
    static u64 read_cycle_counter() noexcept;
    static void memory_fence() noexcept;
    static void enable_interrupts() noexcept;
    static void disable_interrupts() noexcept;
    static void halt() noexcept;
};

template <> struct NativeInstructions<Architecture::ppc>
{
    static u64 read_cycle_counter() noexcept;
    static void memory_fence() noexcept;
    static void enable_interrupts() noexcept;
    static void disable_interrupts() noexcept;
    static void halt() noexcept;
};

} // namespace ok::arch::detail
