#include "ok/arch/arch.hpp"
#include "ops_private.hpp"

namespace ok::arch {

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
    switch (architecture) {
    case Architecture::i386:
        return detail::make_i386_operations();
    case Architecture::x86_64:
        return detail::make_x86_64_operations();
    case Architecture::aarch64:
        return detail::make_aarch64_operations();
    case Architecture::arm32:
        return detail::make_arm32_operations();
    case Architecture::rv64:
        return detail::make_rv64_operations();
    case Architecture::rv32:
        return detail::make_rv32_operations();
    case Architecture::loongarch64:
        return detail::make_loongarch64_operations();
    case Architecture::host:
        return detail::make_host_operations();
    }
    return detail::make_host_operations();
}

} // namespace ok::arch
