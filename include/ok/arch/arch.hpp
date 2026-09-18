#pragma once

#include "ok/arch/native.hpp"
#include "ok/arch/profiles.hpp"

#include <concepts>
#include <type_traits>

namespace ok::arch
{

// Architecture selection is a build-time boundary. Runtime polymorphism belongs
// at loadable driver/module interfaces, not at every CPU primitive call.
template <typename T>
concept ArchitectureInstructions = requires {
    { T::read_cycle_counter() } noexcept -> std::same_as<u64>;
    { T::memory_fence() } noexcept -> std::same_as<void>;
    { T::enable_interrupts() } noexcept -> std::same_as<void>;
    { T::disable_interrupts() } noexcept -> std::same_as<void>;
    { T::halt() } noexcept -> std::same_as<void>;
};

template <Architecture A, ArchitectureInstructions Instructions = detail::NativeInstructions<A>>
    requires ArchitectureProfile<ArchTraits<A>>
class ArchitectureOperations final
{
  public:
    [[nodiscard]] constexpr std::string_view name() const
    {
        return ArchTraits<A>::name;
    }
    [[nodiscard]] constexpr Architecture architecture() const
    {
        return A;
    }
    [[nodiscard]] constexpr usize page_size() const
    {
        return ArchTraits<A>::page_size;
    }
    [[nodiscard]] constexpr usize register_count() const
    {
        return ArchTraits<A>::register_count;
    }
    [[nodiscard]] constexpr Endianness endianness() const
    {
        return ArchTraits<A>::endianness;
    }
    // This remains profile metadata until topology discovery is implemented.
    [[nodiscard]] constexpr usize hardware_thread_count() const
    {
        return ArchTraits<A>::hardware_threads;
    }
    [[nodiscard]] constexpr bool supports_user_mode() const
    {
        return ArchTraits<A>::has_user_mode;
    }
    [[nodiscard]] constexpr std::string_view interrupt_model() const
    {
        return ArchTraits<A>::interrupt_model;
    }
    [[nodiscard]] constexpr std::string_view syscall_model() const
    {
        return ArchTraits<A>::syscall_model;
    }
    [[nodiscard]] constexpr std::string_view user_transition_model() const
    {
        return ArchTraits<A>::user_transition_model;
    }

    [[nodiscard]] constexpr CpuContext make_kernel_context(uptr entry, uptr stack_top) const
    {
        CpuContext context{};
        context.architecture = A;
        context.program_counter = entry;
        context.stack_pointer = stack_top;
        context.mode = PrivilegeMode::kernel;
        return context;
    }

    [[nodiscard]] constexpr CpuContext make_user_context(UserEntry entry) const
    {
        CpuContext context{};
        context.architecture = A;
        context.program_counter = entry.instruction_pointer;
        context.stack_pointer = entry.stack_pointer;
        context.registers[0] = entry.argument;
        context.mode = PrivilegeMode::user;
        return context;
    }

    [[nodiscard]] u64 read_cycle_counter() const noexcept
    {
        return Instructions::read_cycle_counter();
    }
    void memory_fence() noexcept
    {
        Instructions::memory_fence();
    }
    void enable_interrupts() noexcept
    {
        Instructions::enable_interrupts();
    }
    void disable_interrupts() noexcept
    {
        Instructions::disable_interrupts();
    }
    void halt() noexcept
    {
        Instructions::halt();
    }
};

[[nodiscard]] constexpr std::string_view to_string(Architecture architecture)
{
    switch (architecture)
    {
    case Architecture::i386:
        return "i386";
    case Architecture::x86_64:
        return "x86_64";
    case Architecture::aarch64:
        return "aarch64";
    case Architecture::arm32:
        return "arm32";
    case Architecture::rv64:
        return "rv64";
    case Architecture::rv32:
        return "rv32";
    case Architecture::loongarch64:
        return "loongarch64";
    case Architecture::mips:
        return "mips";
    case Architecture::mips64:
        return "mips64";
    case Architecture::ppc:
        return "ppc";
    }
    return "unknown";
}

[[nodiscard]] constexpr Architecture architecture_from_string(std::string_view value)
{
    if (value == "i386")
    {
        return Architecture::i386;
    }
    if (value == "x86_64" || value == "x64")
    {
        return Architecture::x86_64;
    }
    if (value == "aarch64")
    {
        return Architecture::aarch64;
    }
    if (value == "arm32" || value == "arm")
    {
        return Architecture::arm32;
    }
    if (value == "rv64" || value == "riscv64")
    {
        return Architecture::rv64;
    }
    if (value == "rv32" || value == "riscv32")
    {
        return Architecture::rv32;
    }
    if (value == "loongarch64")
    {
        return Architecture::loongarch64;
    }
    if (value == "mips")
    {
        return Architecture::mips;
    }
    if (value == "mips64")
    {
        return Architecture::mips64;
    }
    if (value == "ppc" || value == "powerpc")
    {
        return Architecture::ppc;
    }
    return Architecture::x86_64;
}

[[nodiscard]] constexpr Architecture configured_architecture()
{
#if defined(OK_ARCH_TARGET_I386)
    return Architecture::i386;
#elif defined(OK_ARCH_TARGET_X86_64)
    return Architecture::x86_64;
#elif defined(OK_ARCH_TARGET_AARCH64)
    return Architecture::aarch64;
#elif defined(OK_ARCH_TARGET_ARM32)
    return Architecture::arm32;
#elif defined(OK_ARCH_TARGET_RV64)
    return Architecture::rv64;
#elif defined(OK_ARCH_TARGET_RV32)
    return Architecture::rv32;
#elif defined(OK_ARCH_TARGET_LOONGARCH64)
    return Architecture::loongarch64;
#elif defined(OK_ARCH_TARGET_MIPS)
    return Architecture::mips;
#elif defined(OK_ARCH_TARGET_MIPS64)
    return Architecture::mips64;
#elif defined(OK_ARCH_TARGET_PPC)
    return Architecture::ppc;
#else
    return Architecture::x86_64;
#endif
}

using ArchOperations = ArchitectureOperations<configured_architecture()>;
static_assert(!std::is_polymorphic_v<ArchOperations>);

[[nodiscard]] constexpr Status validate_architecture(Architecture requested)
{
    return requested == configured_architecture()
               ? Status::success()
               : Status::unsupported("architecture differs from the compiled kernel target");
}

[[nodiscard]] ArchOperations &configured_arch_operations();
// Compatibility accessor: the argument is a required match, never a request to
// switch instruction sets. Validate untrusted configuration before calling it.
[[nodiscard]] ArchOperations &arch_operations(Architecture architecture);

} // namespace ok::arch
