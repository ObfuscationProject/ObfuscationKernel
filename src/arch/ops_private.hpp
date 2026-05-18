#pragma once

#include "ok/arch/arch.hpp"
#include "ok/arch/profiles.hpp"

#include <atomic>

namespace ok::arch::detail {

class ArchOperationsBase : public ArchOperations {
public:
    [[nodiscard]] Architecture architecture() const override { return architecture_; }
    [[nodiscard]] usize hardware_thread_count() const override { return 1; }

    [[nodiscard]] CpuContext make_kernel_context(uptr entry, uptr stack_top) const override
    {
        CpuContext context {};
        context.architecture = architecture_;
        context.program_counter = entry;
        context.stack_pointer = stack_top;
        context.mode = PrivilegeMode::kernel;
        return context;
    }

    [[nodiscard]] CpuContext make_user_context(UserEntry entry) const override
    {
        CpuContext context {};
        context.architecture = architecture_;
        context.program_counter = entry.instruction_pointer;
        context.stack_pointer = entry.stack_pointer;
        context.registers[0] = entry.argument;
        context.mode = PrivilegeMode::user;
        return context;
    }

    void enable_interrupts() noexcept override { interrupts_enabled_ = true; }
    void disable_interrupts() noexcept override { interrupts_enabled_ = false; }
    void halt() noexcept override { halted_ = true; }

protected:
    explicit ArchOperationsBase(Architecture architecture) : architecture_(architecture) {}

    static u64 fallback_cycle_counter() noexcept
    {
        static std::atomic<u64> counter {1};
        return counter.fetch_add(1, std::memory_order_relaxed);
    }

    static void compiler_fence() noexcept { std::atomic_signal_fence(std::memory_order_seq_cst); }

    bool interrupts_enabled_ {false};
    bool halted_ {false};

private:
    Architecture architecture_;
};

template <Architecture A>
class ProfiledArchOperationsBase : public ArchOperationsBase {
public:
    ProfiledArchOperationsBase() : ArchOperationsBase(A) {}

    [[nodiscard]] std::string_view name() const override { return ArchTraits<A>::name; }
    [[nodiscard]] usize page_size() const override { return ArchTraits<A>::page_size; }
    [[nodiscard]] usize register_count() const override { return ArchTraits<A>::register_count; }
    [[nodiscard]] Endianness endianness() const override { return ArchTraits<A>::endianness; }
    [[nodiscard]] usize hardware_thread_count() const override { return ArchTraits<A>::hardware_threads; }
    [[nodiscard]] bool supports_user_mode() const override { return ArchTraits<A>::has_user_mode; }
};

ArchOperations& i386_operations();
ArchOperations& x86_64_operations();
ArchOperations& aarch64_operations();
ArchOperations& arm32_operations();
ArchOperations& rv64_operations();
ArchOperations& rv32_operations();
ArchOperations& loongarch64_operations();

} // namespace ok::arch::detail
