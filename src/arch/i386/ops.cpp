#include "../ops_private.hpp"

namespace ok::arch::detail
{
namespace
{

class I386Operations final : public ProfiledArchOperationsBase<Architecture::i386>
{
  public:
    [[nodiscard]] std::string_view interrupt_model() const override
    {
        return "idt-pic-apic";
    }
    [[nodiscard]] std::string_view syscall_model() const override
    {
        return "int80-sysenter";
    }
    [[nodiscard]] std::string_view user_transition_model() const override
    {
        return "iret-tss";
    }

    [[nodiscard]] u64 read_cycle_counter() const noexcept override
    {
#if defined(__i386__) || defined(__x86_64__)
        u32 low = 0;
        u32 high = 0;
        asm volatile("rdtsc" : "=a"(low), "=d"(high));
        return (static_cast<u64>(high) << 32) | low;
#else
        return fallback_cycle_counter();
#endif
    }

    void memory_fence() noexcept override
    {
#if defined(__x86_64__)
        asm volatile("mfence" ::: "memory");
#elif defined(__i386__)
        asm volatile("lock; addl $0, 0(%%esp)" ::: "memory");
#else
        compiler_fence();
#endif
    }

    void enable_interrupts() noexcept override
    {
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__i386__)
        asm volatile("sti" ::: "memory");
#endif
        ArchOperationsBase::enable_interrupts();
    }

    void disable_interrupts() noexcept override
    {
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__i386__)
        asm volatile("cli" ::: "memory");
#endif
        ArchOperationsBase::disable_interrupts();
    }

    void halt() noexcept override
    {
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__i386__)
        asm volatile("hlt" ::: "memory");
#elif defined(__i386__) || defined(__x86_64__)
        asm volatile("pause" ::: "memory");
#endif
        ArchOperationsBase::halt();
    }
};

} // namespace

ArchOperations &i386_operations()
{
    static I386Operations operations;
    return operations;
}

} // namespace ok::arch::detail
