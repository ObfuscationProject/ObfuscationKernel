#include "../ops_private.hpp"

namespace ok::arch::detail {
namespace {

class Rv32Operations final : public ProfiledArchOperationsBase<Architecture::rv32> {
public:
    [[nodiscard]] std::string_view interrupt_model() const override { return "stvec-plic-sbi"; }
    [[nodiscard]] std::string_view syscall_model() const override { return "ecall"; }
    [[nodiscard]] std::string_view user_transition_model() const override { return "sret"; }

    [[nodiscard]] u64 read_cycle_counter() const noexcept override
    {
#if defined(__riscv) && __riscv_xlen == 32
        u32 high_before = 0;
        u32 low = 0;
        u32 high_after = 0;
        do {
            asm volatile("rdcycleh %0" : "=r"(high_before));
            asm volatile("rdcycle %0" : "=r"(low));
            asm volatile("rdcycleh %0" : "=r"(high_after));
        } while (high_before != high_after);
        return (static_cast<u64>(high_after) << 32) | low;
#else
        return fallback_cycle_counter();
#endif
    }

    void memory_fence() noexcept override
    {
#if defined(__riscv)
        asm volatile("fence rw, rw" ::: "memory");
#else
        compiler_fence();
#endif
    }

    void enable_interrupts() noexcept override
    {
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__riscv)
        asm volatile("csrsi sstatus, 2" ::: "memory");
#endif
        ArchOperationsBase::enable_interrupts();
    }

    void disable_interrupts() noexcept override
    {
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__riscv)
        asm volatile("csrci sstatus, 2" ::: "memory");
#endif
        ArchOperationsBase::disable_interrupts();
    }

    void halt() noexcept override
    {
#if defined(OK_USE_PRIVILEGED_ASM) && defined(__riscv)
        asm volatile("wfi" ::: "memory");
#elif defined(__riscv)
        asm volatile("nop" ::: "memory");
#endif
        ArchOperationsBase::halt();
    }
};

} // namespace

std::unique_ptr<ArchOperations> make_rv32_operations()
{
    return std::make_unique<Rv32Operations>();
}

} // namespace ok::arch::detail

