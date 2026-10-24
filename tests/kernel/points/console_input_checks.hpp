#pragma once

#include "ok/sched/console_input.hpp"

namespace ok::tests
{
// Lease algorithm only; no native trap, terminal frontend or password UI claim.
inline Status check_console_input_lease_model()
{
    sched::ConsoleInputLease input;
    std::array<std::byte, 4> output{};
    if (input.input('x') || input.read(2, 0, 0, output) || !input.bind_manager(1) ||
        input.bind_manager(1) || input.claim(2, 3) || input.claim(1, 1))
        return Status::fault("console lease default-closed authority model failed");
    auto first = input.claim(1, 2);
    if (!first || !input.input('s') || input.read(3, 0, first.value(), output) ||
        input.inherit(first.value()) != first.value())
        return Status::fault("console lease exclusive issuance model failed");
    auto read = input.read(2, first.value(), first.value(), output);
    if (!read || read.value() != 1 || output[0] != std::byte{'s'} || input.buffered())
        return Status::fault("console lease bounded read model failed");
    if (!input.input('p') || input.revoke(3, first.value()) || !input.revoke(1, first.value()) ||
        input.input('x') || input.buffered() || input.inherit(first.value()))
        return Status::fault("console lease revocation model failed");
    auto next = input.claim(1, 4);
    if (!next || next.value() == first.value() || input.revoke(1, first.value()) ||
        input.read(2, first.value(), first.value(), output) || !input.input('n') ||
        !input.retire(4) || input.buffered() || input.active())
        return Status::fault("console lease stale generation or leader retirement model failed");
    auto last = input.claim(1, 5);
    if (!last || !input.retire(1) || input.claim(1, 6) || input.input('x'))
        return Status::fault("console lease manager retirement model failed");
    return {};
}
} // namespace ok::tests
