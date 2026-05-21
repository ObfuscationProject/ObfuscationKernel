#include "../ops_private.hpp"

namespace ok::arch::detail
{
namespace
{

class LoongArch64Operations final : public ProfiledArchOperationsBase<Architecture::loongarch64>
{
  public:
    [[nodiscard]] std::string_view interrupt_model() const override
    {
        return "eentry-ecfg";
    }
    [[nodiscard]] std::string_view syscall_model() const override
    {
        return "syscall";
    }
    [[nodiscard]] std::string_view user_transition_model() const override
    {
        return "ertn";
    }

    [[nodiscard]] u64 read_cycle_counter() const noexcept override
    {
#if defined(__loongarch__) && defined(__loongarch64)
        u64 value = 0;
        asm volatile("rdtime.d %0, $r0" : "=r"(value));
        return value == 0 ? fallback_cycle_counter() : value;
#else
        return fallback_cycle_counter();
#endif
    }

    void memory_fence() noexcept override
    {
#if defined(__loongarch__)
        asm volatile("dbar 0" ::: "memory");
#else
        compiler_fence();
#endif
    }

    void enable_interrupts() noexcept override
    {
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__loongarch__)
        asm volatile("csrxchg $r0, $r0, 0x0" ::: "memory");
#endif
        ArchOperationsBase::enable_interrupts();
    }

    void disable_interrupts() noexcept override
    {
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__loongarch__)
        asm volatile("csrxchg $r0, $r0, 0x0" ::: "memory");
#endif
        ArchOperationsBase::disable_interrupts();
    }

    void halt() noexcept override
    {
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__loongarch__)
        asm volatile("idle 0" ::: "memory");
#elif defined(__loongarch__)
        asm volatile("nop" ::: "memory");
#endif
        ArchOperationsBase::halt();
    }
};

} // namespace

ArchOperations &loongarch64_operations()
{
    static LoongArch64Operations operations;
    return operations;
}

} // namespace ok::arch::detail
