#include "ok/arch/arch.hpp"
#include "ops_private.hpp"

namespace ok::arch
{

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
    return Architecture::x86_64;
#endif
}

ArchOperations &arch_operations(Architecture architecture)
{
    static_cast<void>(architecture);
#if defined(OK_ARCH_TARGET_I386)
    return detail::i386_operations();
#elif defined(OK_ARCH_TARGET_X86_64)
    return detail::x86_64_operations();
#elif defined(OK_ARCH_TARGET_AARCH64)
    return detail::aarch64_operations();
#elif defined(OK_ARCH_TARGET_ARM32)
    return detail::arm32_operations();
#elif defined(OK_ARCH_TARGET_RV64)
    return detail::rv64_operations();
#elif defined(OK_ARCH_TARGET_RV32)
    return detail::rv32_operations();
#elif defined(OK_ARCH_TARGET_LOONGARCH64)
    return detail::loongarch64_operations();
#else
    return detail::x86_64_operations();
#endif
}

} // namespace ok::arch
