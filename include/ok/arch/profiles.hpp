#pragma once

#include "ok/arch/types.hpp"

#include <concepts>

namespace ok::arch
{

template <Architecture A> struct ArchTraits;

template <typename T>
concept ArchitectureProfile = requires {
    { T::architecture } -> std::convertible_to<Architecture>;
    { T::name } -> std::convertible_to<std::string_view>;
    { T::page_size } -> std::convertible_to<usize>;
    { T::register_count } -> std::convertible_to<usize>;
    { T::endianness } -> std::convertible_to<Endianness>;
    { T::hardware_threads } -> std::convertible_to<usize>;
    { T::has_user_mode } -> std::convertible_to<bool>;
    { T::interrupt_model } -> std::convertible_to<std::string_view>;
    { T::syscall_model } -> std::convertible_to<std::string_view>;
    { T::user_transition_model } -> std::convertible_to<std::string_view>;
};

template <> struct ArchTraits<Architecture::i386>
{
    static constexpr Architecture architecture = Architecture::i386;
    static constexpr std::string_view user_transition_model = "iret-tss";
    static constexpr std::string_view syscall_model = "int80-sysenter";
    static constexpr std::string_view interrupt_model = "idt-pic-apic";
    static constexpr std::string_view name = "i386";
    static constexpr usize page_size = 4096;
    static constexpr usize register_count = 8;
    static constexpr Endianness endianness = Endianness::little;
    static constexpr bool has_user_mode = true;
    static constexpr usize hardware_threads = 2;
};

template <> struct ArchTraits<Architecture::x86_64>
{
    static constexpr Architecture architecture = Architecture::x86_64;
    static constexpr std::string_view user_transition_model = "iretq-sysret";
    static constexpr std::string_view syscall_model = "syscall-sysret";
    static constexpr std::string_view interrupt_model = "idt-ist-x2apic";
    static constexpr std::string_view name = "x86_64";
    static constexpr usize page_size = 4096;
    static constexpr usize register_count = 16;
    static constexpr Endianness endianness = Endianness::little;
    static constexpr bool has_user_mode = true;
    static constexpr usize hardware_threads = 4;
};

template <> struct ArchTraits<Architecture::aarch64>
{
    static constexpr Architecture architecture = Architecture::aarch64;
    static constexpr std::string_view user_transition_model = "eret-el0";
    static constexpr std::string_view syscall_model = "svc-el0";
    static constexpr std::string_view interrupt_model = "el1-vector-gic";
    static constexpr std::string_view name = "aarch64";
    static constexpr usize page_size = 4096;
    static constexpr usize register_count = 31;
    static constexpr Endianness endianness = Endianness::little;
    static constexpr bool has_user_mode = true;
    static constexpr usize hardware_threads = 4;
};

template <> struct ArchTraits<Architecture::arm32>
{
    static constexpr Architecture architecture = Architecture::arm32;
    static constexpr std::string_view user_transition_model = "exception-return-user";
    static constexpr std::string_view syscall_model = "svc";
    static constexpr std::string_view interrupt_model = "vector-table-gic";
    static constexpr std::string_view name = "arm32";
    static constexpr usize page_size = 4096;
    static constexpr usize register_count = 16;
    static constexpr Endianness endianness = Endianness::little;
    static constexpr bool has_user_mode = true;
    static constexpr usize hardware_threads = 2;
};

template <> struct ArchTraits<Architecture::rv64>
{
    static constexpr Architecture architecture = Architecture::rv64;
    static constexpr std::string_view user_transition_model = "sret";
    static constexpr std::string_view syscall_model = "ecall";
    static constexpr std::string_view interrupt_model = "stvec-plic-sbi";
    static constexpr std::string_view name = "rv64";
    static constexpr usize page_size = 4096;
    static constexpr usize register_count = 32;
    static constexpr Endianness endianness = Endianness::little;
    static constexpr bool has_user_mode = true;
    static constexpr usize hardware_threads = 4;
};

template <> struct ArchTraits<Architecture::rv32>
{
    static constexpr Architecture architecture = Architecture::rv32;
    static constexpr std::string_view user_transition_model = "sret";
    static constexpr std::string_view syscall_model = "ecall";
    static constexpr std::string_view interrupt_model = "stvec-plic-sbi";
    static constexpr std::string_view name = "rv32";
    static constexpr usize page_size = 4096;
    static constexpr usize register_count = 32;
    static constexpr Endianness endianness = Endianness::little;
    static constexpr bool has_user_mode = true;
    static constexpr usize hardware_threads = 2;
};

template <> struct ArchTraits<Architecture::loongarch64>
{
    static constexpr Architecture architecture = Architecture::loongarch64;
    static constexpr std::string_view user_transition_model = "ertn";
    static constexpr std::string_view syscall_model = "syscall";
    static constexpr std::string_view interrupt_model = "eentry-ecfg";
    static constexpr std::string_view name = "loongarch64";
    static constexpr usize page_size = 4096;
    static constexpr usize register_count = 32;
    static constexpr Endianness endianness = Endianness::little;
    static constexpr bool has_user_mode = true;
    static constexpr usize hardware_threads = 4;
};

template <> struct ArchTraits<Architecture::mips>
{
    static constexpr Architecture architecture = Architecture::mips;
    static constexpr std::string_view user_transition_model = "eret";
    static constexpr std::string_view syscall_model = "syscall";
    static constexpr std::string_view interrupt_model = "cp0-status-intctl";
    static constexpr std::string_view name = "mips";
    static constexpr usize page_size = 4096;
    static constexpr usize register_count = 32;
    static constexpr Endianness endianness = Endianness::big;
    static constexpr bool has_user_mode = true;
    static constexpr usize hardware_threads = 2;
};

template <> struct ArchTraits<Architecture::mips64>
{
    static constexpr Architecture architecture = Architecture::mips64;
    static constexpr std::string_view user_transition_model = "eret";
    static constexpr std::string_view syscall_model = "syscall";
    static constexpr std::string_view interrupt_model = "cp0-status-intctl";
    static constexpr std::string_view name = "mips64";
    static constexpr usize page_size = 4096;
    static constexpr usize register_count = 32;
    static constexpr Endianness endianness = Endianness::big;
    static constexpr bool has_user_mode = true;
    static constexpr usize hardware_threads = 4;
};

template <> struct ArchTraits<Architecture::ppc>
{
    static constexpr Architecture architecture = Architecture::ppc;
    static constexpr std::string_view user_transition_model = "rfi";
    static constexpr std::string_view syscall_model = "sc";
    static constexpr std::string_view interrupt_model = "msr-ee-decrementer";
    static constexpr std::string_view name = "ppc";
    static constexpr usize page_size = 4096;
    static constexpr usize register_count = 32;
    static constexpr Endianness endianness = Endianness::big;
    static constexpr bool has_user_mode = true;
    static constexpr usize hardware_threads = 2;
};

static_assert(ArchitectureProfile<ArchTraits<Architecture::i386>>);
static_assert(ArchitectureProfile<ArchTraits<Architecture::x86_64>>);
static_assert(ArchitectureProfile<ArchTraits<Architecture::aarch64>>);
static_assert(ArchitectureProfile<ArchTraits<Architecture::arm32>>);
static_assert(ArchitectureProfile<ArchTraits<Architecture::rv64>>);
static_assert(ArchitectureProfile<ArchTraits<Architecture::rv32>>);
static_assert(ArchitectureProfile<ArchTraits<Architecture::loongarch64>>);
static_assert(ArchitectureProfile<ArchTraits<Architecture::mips>>);
static_assert(ArchitectureProfile<ArchTraits<Architecture::mips64>>);
static_assert(ArchitectureProfile<ArchTraits<Architecture::ppc>>);

} // namespace ok::arch
