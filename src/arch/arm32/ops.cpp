#include "../ops_private.hpp"

namespace ok::arch::detail
{
namespace
{

class Arm32Operations final : public ProfiledArchOperationsBase<Architecture::arm32>
{
  public:
    [[nodiscard]] std::string_view interrupt_model() const override
    {
        return "vector-table-gic";
    }
    [[nodiscard]] std::string_view syscall_model() const override
    {
        return "svc";
    }
    [[nodiscard]] std::string_view user_transition_model() const override
    {
        return "exception-return-user";
    }

    [[nodiscard]] u64 read_cycle_counter() const noexcept override
    {
#if defined(__arm__)
        u32 value = 0;
        asm volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(value));
        return value;
#else
        return fallback_cycle_counter();
#endif
    }

    void memory_fence() noexcept override
    {
#if defined(__arm__)
        asm volatile("dsb sy\nisb" ::: "memory");
#else
        compiler_fence();
#endif
    }

    void enable_interrupts() noexcept override
    {
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__arm__)
        asm volatile("cpsie i" ::: "memory");
#endif
        ArchOperationsBase::enable_interrupts();
    }

    void disable_interrupts() noexcept override
    {
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__arm__)
        asm volatile("cpsid i" ::: "memory");
#endif
        ArchOperationsBase::disable_interrupts();
    }

    void halt() noexcept override
    {
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__arm__)
        asm volatile("wfi" ::: "memory");
#elif defined(__arm__)
        asm volatile("yield" ::: "memory");
#endif
        ArchOperationsBase::halt();
    }
};

} // namespace

ArchOperations &arm32_operations()
{
    static Arm32Operations operations;
    return operations;
}

} // namespace ok::arch::detail
