#include "../ops_private.hpp"

namespace ok::arch::detail {
namespace {

class Rv64Operations final : public ProfiledArchOperationsBase<Architecture::rv64> {
public:
    [[nodiscard]] std::string_view interrupt_model() const override { return "stvec-plic-sbi"; }
    [[nodiscard]] std::string_view syscall_model() const override { return "ecall"; }
    [[nodiscard]] std::string_view user_transition_model() const override { return "sret"; }

    [[nodiscard]] u64 read_cycle_counter() const noexcept override
    {
#if defined(__riscv) && __riscv_xlen == 64
        u64 value = 0;
        asm volatile("rdcycle %0" : "=r"(value));
        return value;
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

ArchOperations& rv64_operations()
{
    static Rv64Operations operations;
    return operations;
}

} // namespace ok::arch::detail
