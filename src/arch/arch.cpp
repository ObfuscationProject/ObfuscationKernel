#include "ok/arch/arch.hpp"

namespace ok::arch
{

ArchOperations &configured_arch_operations()
{
    static ArchOperations operations;
    return operations;
}

ArchOperations &arch_operations(Architecture architecture)
{
    if (!validate_architecture(architecture).ok())
    {
        // An internal target mismatch is a programming error. Public paths use
        // validate_architecture() to return an ordinary unsupported status.
        __builtin_trap();
    }
    return configured_arch_operations();
}

} // namespace ok::arch
