#pragma once

#include "ok/core/types.hpp"

#include <array>
#include <memory>
#include <string_view>

namespace ok::arch {

enum class Architecture : u8 {
    host,
    i386,
    x86_64,
    aarch64,
    arm32,
    rv64,
    rv32,
    loongarch64,
};

enum class Endianness : u8 {
    little,
    big,
};

enum class PrivilegeMode : u8 {
    kernel,
    user,
};

struct CpuContext {
    Architecture architecture {Architecture::host};
    std::array<uptr, 32> registers {};
    uptr program_counter {0};
    uptr stack_pointer {0};
    uptr status_register {0};
    PrivilegeMode mode {PrivilegeMode::kernel};
};

struct TrapFrame {
    u64 vector {0};
    u64 error_code {0};
    CpuContext context {};
};

struct UserEntry {
    uptr instruction_pointer {0};
    uptr stack_pointer {0};
    uptr argument {0};
};

class ArchOperations {
public:
    virtual ~ArchOperations() = default;

    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual Architecture architecture() const = 0;
    [[nodiscard]] virtual usize page_size() const = 0;
    [[nodiscard]] virtual usize register_count() const = 0;
    [[nodiscard]] virtual Endianness endianness() const = 0;
    [[nodiscard]] virtual usize hardware_thread_count() const = 0;
    [[nodiscard]] virtual bool supports_user_mode() const = 0;
    [[nodiscard]] virtual std::string_view interrupt_model() const = 0;
    [[nodiscard]] virtual std::string_view syscall_model() const = 0;
    [[nodiscard]] virtual std::string_view user_transition_model() const = 0;
    [[nodiscard]] virtual CpuContext make_kernel_context(uptr entry, uptr stack_top) const = 0;
    [[nodiscard]] virtual CpuContext make_user_context(UserEntry entry) const = 0;
    [[nodiscard]] virtual u64 read_cycle_counter() const noexcept = 0;
    virtual void memory_fence() noexcept = 0;
    virtual void enable_interrupts() noexcept = 0;
    virtual void disable_interrupts() noexcept = 0;
    virtual void halt() noexcept = 0;
};

template <Architecture A>
struct ArchTraits;

[[nodiscard]] constexpr std::string_view to_string(Architecture architecture)
{
    switch (architecture) {
    case Architecture::host:
        return "host";
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
    }
    return "unknown";
}

[[nodiscard]] constexpr Architecture architecture_from_string(std::string_view value)
{
    if (value == "i386") {
        return Architecture::i386;
    }
    if (value == "x86_64" || value == "x64") {
        return Architecture::x86_64;
    }
    if (value == "aarch64") {
        return Architecture::aarch64;
    }
    if (value == "arm32" || value == "arm") {
        return Architecture::arm32;
    }
    if (value == "rv64" || value == "riscv64") {
        return Architecture::rv64;
    }
    if (value == "rv32" || value == "riscv32") {
        return Architecture::rv32;
    }
    if (value == "loongarch64") {
        return Architecture::loongarch64;
    }
    return Architecture::host;
}

[[nodiscard]] Architecture configured_architecture();
[[nodiscard]] std::unique_ptr<ArchOperations> make_arch_operations(Architecture architecture);

} // namespace ok::arch
