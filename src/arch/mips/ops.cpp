#include "../ops_private.hpp"

namespace ok::arch::detail
{
namespace
{

class MipsOperations final : public ProfiledArchOperationsBase<Architecture::mips>
{
  public:
    [[nodiscard]] std::string_view interrupt_model() const override
    {
        return "cp0-status-intctl";
    }
    [[nodiscard]] std::string_view syscall_model() const override
    {
        return "syscall";
    }
    [[nodiscard]] std::string_view user_transition_model() const override
    {
        return "eret";
    }

    [[nodiscard]] u64 read_cycle_counter() const noexcept override
    {
#if defined(__mips__)
        u32 value = 0;
        asm volatile("mfc0 %0, $9" : "=r"(value));
        return value == 0 ? 1 : value;
#else
        return fallback_cycle_counter();
#endif
    }

    void memory_fence() noexcept override
    {
#if defined(__mips__)
        asm volatile("sync" ::: "memory");
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
#if defined(__mips__)
        asm volatile("nop" ::: "memory");
#endif
        ArchOperationsBase::halt();
    }
};

} // namespace

ArchOperations &mips_operations()
{
    static MipsOperations operations;
    return operations;
}

} // namespace ok::arch::detail
