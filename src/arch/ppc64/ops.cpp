#include "../ops_private.hpp"

namespace ok::arch::detail
{
namespace
{

class Ppc64Operations final : public ProfiledArchOperationsBase<Architecture::ppc64>
{
  public:
    [[nodiscard]] std::string_view interrupt_model() const override
    {
        return "msr-ee-decrementer";
    }
    [[nodiscard]] std::string_view syscall_model() const override
    {
        return "sc";
    }
    [[nodiscard]] std::string_view user_transition_model() const override
    {
        return "rfid";
    }

    [[nodiscard]] u64 read_cycle_counter() const noexcept override
    {
        return fallback_cycle_counter();
    }

    void memory_fence() noexcept override
    {
#if defined(__powerpc__) || defined(__PPC__)
        asm volatile("sync\nisync" ::: "memory");
#else
        compiler_fence();
#endif
    }

    void enable_interrupts() noexcept override
    {
        ArchOperationsBase::enable_interrupts();
    }

    void disable_interrupts() noexcept override
    {
        ArchOperationsBase::disable_interrupts();
    }

    void halt() noexcept override
    {
#if defined(__powerpc__) || defined(__PPC__)
        asm volatile("nop" ::: "memory");
#endif
        ArchOperationsBase::halt();
    }
};

} // namespace

ArchOperations &ppc64_operations()
{
    static Ppc64Operations operations;
    return operations;
}

} // namespace ok::arch::detail
