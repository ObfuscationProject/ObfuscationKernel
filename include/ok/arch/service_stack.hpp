#pragma once

#include "ok/arch/instantaneous_sp.hpp"
#include "ok/arch/stack_bounds.hpp"

extern "C"
{
    // Assembly aliases on the actual boot stack, without extra allocation.
    extern const unsigned char ok_service_stack_lower[];
    extern const unsigned char ok_service_stack_top[];
}

namespace ok::arch
{

[[nodiscard, gnu::always_inline]] inline StackBounds linked_service_stack_bounds() noexcept
{
    return {reinterpret_cast<uptr>(ok_service_stack_lower), reinterpret_cast<uptr>(ok_service_stack_top)};
}

// Check the current instruction's SP, including its compiler frame. This does
// not reserve the suffix or survive a stack/domain switch. Callers that need a
// durable capability still require a mapping and execution-lifetime producer.
[[nodiscard, gnu::always_inline]] inline Status require_service_stack(usize suffix_bytes = 0) noexcept
{
    const auto sp = read_instantaneous_stack_pointer();
    return linked_service_stack_bounds().require(sp, suffix_bytes);
}

} // namespace ok::arch
