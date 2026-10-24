#include "../kernel/points/wait_registration_checks.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>

namespace
{
ok::usize checks{};
void require(bool condition, const char *message)
{
    ++checks;
    if (!condition)
    {
        std::fprintf(stderr, "FAIL wait-registration check=%zu: %s\n", checks, message);
        std::exit(1);
    }
}
} // namespace

int main()
{
    using namespace ok;
    using namespace ok::sched;
    require(tests::check_wait_registration().ok(), "shared kernel checks");
    // Every order of ready/timeout/exit/signal competes for one registration. The first
    // arrival wins; no later producer can overwrite its terminal outcome.
    std::array outcomes{WaitCompletion::ready, WaitCompletion::timeout, WaitCompletion::cancelled,
                        WaitCompletion::interrupted};
    do
    {
        WaitRegistration wait;
        const auto ticket = wait.arm(1, 2);
        require(ticket.ok(), "completion-order arm");
        for (usize i = 0; i < outcomes.size(); ++i)
        {
            require(wait.complete(ticket.value(), outcomes[i]) == (i == 0), "exactly one outcome wins");
            require(wait.completion() == outcomes[0], "winner remains immutable");
        }
        require(!wait.cancel(), "cancel after winner is inert");
    } while (std::next_permutation(outcomes.begin(), outcomes.end()));

    WaitRegistration wait;
    std::array<WaitTicket, 256> history{};
    for (usize i = 0; i < history.size(); ++i)
    {
        const auto owner = i % 7 + 1;
        const auto thread = i % 13 + 1;
        const auto armed = wait.arm(owner, thread);
        require(armed.ok(), "repeated registration admits after completion");
        history[i] = armed.value();
        require(armed.value() == WaitTicket{owner, thread, i + 1}, "monotonic generation and exact identity");
        for (usize j = 0; j < i; ++j)
        {
            require(!wait.pending(history[j]), "prior registration is never pending");
            require(!wait.complete(history[j], WaitCompletion::ready), "prior ready callback cannot wake replacement");
            require(!wait.complete(history[j], WaitCompletion::timeout), "prior timer cannot wake replacement");
            require(!wait.complete(history[j], WaitCompletion::interrupted), "prior signal cannot wake replacement");
        }
        require(wait.pending(history[i]), "stale callbacks leave current registration pending");
        require(wait.arm(owner + 1, thread + 1).status().code() == StatusCode::busy,
                "duplicate arm cannot replace another identity");
        require(wait.arm(0, thread).status().code() == StatusCode::invalid_argument,
                "invalid identity is rejected even while active");
        require(wait.ticket() == history[i] && wait.active() && wait.completion() == WaitCompletion::none,
                "refusals preserve full active state");
        const auto reason = outcomes[i % outcomes.size()];
        require(reason == WaitCompletion::cancelled ? wait.cancel() : wait.complete(history[i], reason),
                "current owner completes");
        require(!wait.active() && wait.ticket() == history[i] && wait.completion() == reason,
                "terminal state retains identity and outcome");
        require(!wait.cancel() && !wait.complete(history[i], WaitCompletion::ready),
                "duplicate terminal producers are inert");
    }
    // Invalid enum encodings and all partial identity mismatches must preserve
    // a live registration; neither ticket equality nor enum range is trusted.
    const auto final = wait.arm(~u64{0}, ~u64{0});
    require(final.ok(), "maximum identities are legal");
    for (unsigned value = 5; value < 256; ++value)
        require(!wait.complete(final.value(), static_cast<WaitCompletion>(value)), "invalid terminal encoding");
    for (unsigned bits = 1; bits < 8; ++bits)
    {
        auto wrong = final.value();
        if (bits & 1) --wrong.owner;
        if (bits & 2) --wrong.thread;
        if (bits & 4) --wrong.generation;
        require(!wait.pending(wrong) && !wait.complete(wrong, WaitCompletion::ready), "ticket fields all match");
    }
    require(wait.pending(final.value()) && wait.completion() == WaitCompletion::none, "invalid producers are inert");
    require(wait.cancel(), "final cleanup");
    std::printf("PASS wait-registration checks=%zu scope=hosted-model guest=0 smp=0\n", checks);
}
