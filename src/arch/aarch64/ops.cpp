#include "../ops_private.hpp"

namespace ok::arch::detail {
namespace {

class AArch64Operations final : public ProfiledArchOperationsBase<Architecture::aarch64> {
public:
    [[nodiscard]] std::string_view interrupt_model() const override { return "el1-vector-gic"; }
    [[nodiscard]] std::string_view syscall_model() const override { return "svc-el0"; }
    [[nodiscard]] std::string_view user_transition_model() const override { return "eret-el0"; }

    [[nodiscard]] u64 read_cycle_counter() const noexcept override
    {
#if defined(__aarch64__)
        u64 value = 0;
        asm volatile("mrs %0, cntvct_el0" : "=r"(value));
        return value;
#else
        return fallback_cycle_counter();
#endif
    }

    void memory_fence() noexcept override
    {
#if defined(__aarch64__)
        asm volatile("dsb sy\nisb" ::: "memory");
#else
        compiler_fence();
#endif
    }

    void enable_interrupts() noexcept override
    {
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__aarch64__)
        asm volatile("msr daifclr, #2" ::: "memory");
#endif
        ArchOperationsBase::enable_interrupts();
    }

    void disable_interrupts() noexcept override
    {
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__aarch64__)
        asm volatile("msr daifset, #2" ::: "memory");
#endif
        ArchOperationsBase::disable_interrupts();
    }

    void halt() noexcept override
    {
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__aarch64__)
        asm volatile("wfi" ::: "memory");
#elif defined(__aarch64__)
        asm volatile("yield" ::: "memory");
#endif
        ArchOperationsBase::halt();
    }
};

} // namespace

std::unique_ptr<ArchOperations> make_aarch64_operations()
{
    return std::make_unique<AArch64Operations>();
}

} // namespace ok::arch::detail

