#include "ok/arch/arch.hpp"
#include "ok/arch/profiles.hpp"

namespace ok::arch {
namespace {

class GenericArchOperations final : public ArchOperations {
public:
    explicit GenericArchOperations(Architecture architecture) : architecture_(architecture) {}

    [[nodiscard]] std::string_view name() const override { return to_string(architecture_); }
    [[nodiscard]] Architecture architecture() const override { return architecture_; }

    [[nodiscard]] usize page_size() const override
    {
        switch (architecture_) {
        case Architecture::i386:
            return ArchTraits<Architecture::i386>::page_size;
        case Architecture::x86_64:
            return ArchTraits<Architecture::x86_64>::page_size;
        case Architecture::aarch64:
            return ArchTraits<Architecture::aarch64>::page_size;
        case Architecture::arm32:
            return ArchTraits<Architecture::arm32>::page_size;
        case Architecture::rv64:
            return ArchTraits<Architecture::rv64>::page_size;
        case Architecture::rv32:
            return ArchTraits<Architecture::rv32>::page_size;
        case Architecture::loongarch64:
            return ArchTraits<Architecture::loongarch64>::page_size;
        case Architecture::host:
            return 4096;
        }
        return 4096;
    }

    [[nodiscard]] usize hardware_thread_count() const override { return 1; }
    [[nodiscard]] bool supports_user_mode() const override { return architecture_ != Architecture::host; }

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

private:
    Architecture architecture_;
    bool interrupts_enabled_ {false};
    bool halted_ {false};
};

} // namespace

Architecture configured_architecture()
{
#if defined(OK_ARCH_TARGET_I386)
    return Architecture::i386;
#elif defined(OK_ARCH_TARGET_X86_64)
    return Architecture::x86_64;
#elif defined(OK_ARCH_TARGET_AARCH64)
    return Architecture::aarch64;
#elif defined(OK_ARCH_TARGET_ARM32)
    return Architecture::arm32;
#elif defined(OK_ARCH_TARGET_RV64)
    return Architecture::rv64;
#elif defined(OK_ARCH_TARGET_RV32)
    return Architecture::rv32;
#elif defined(OK_ARCH_TARGET_LOONGARCH64)
    return Architecture::loongarch64;
#else
    return Architecture::host;
#endif
}

std::unique_ptr<ArchOperations> make_arch_operations(Architecture architecture)
{
    if (architecture == Architecture::host) {
        architecture = configured_architecture();
    }
    return std::make_unique<GenericArchOperations>(architecture);
}

} // namespace ok::arch

