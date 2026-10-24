#include "ok/arch/stack_bounds.hpp"

#include <cstdio>
#include <cstdlib>

namespace
{
using namespace ok;
using arch::StackBounds;
usize checks{};

void check(bool pass, const char *message)
{
    ++checks;
    if (!pass)
    {
        std::fprintf(stderr, "FAIL stack-bounds: %s\n", message);
        std::exit(1);
    }
}
void status(Status actual, StatusCode expected, const char *message)
{
    check(actual.code() == expected, message);
}
} // namespace

int main()
{
    constexpr StackBounds bounds{0x1000, 0x11000};
    static_assert(bounds.valid() && bounds.contains(bounds.lower) && bounds.contains(bounds.top));
    static_assert(!StackBounds{}.valid() && !StackBounds{0x1000, 0x1000}.valid());
    static_assert(!StackBounds{0x2000, 0x1000}.valid());
    for (const auto invalid : {StackBounds{}, StackBounds{0, 4096}, StackBounds{4096, 4096},
                               StackBounds{~uptr{0}, 1}})
    {
        check(!invalid.valid(), "invalid range");
        status(invalid.require(4096, 0), StatusCode::invalid_argument, "invalid range rejects zero budget");
        check(!invalid.remaining(4096), "invalid range provides no remaining bytes");
    }
    status(bounds.require(bounds.lower - 1, 0), StatusCode::fault, "below lower is another stack");
    status(bounds.require(bounds.top + 1, 0), StatusCode::fault, "above top is another stack");
    status(bounds.require(bounds.lower, 0), StatusCode::ok, "empty suffix at lower");
    status(bounds.require(bounds.lower, 1), StatusCode::no_memory, "positive suffix at lower");
    status(bounds.require(bounds.top, 65536), StatusCode::ok, "entire unused downward stack");
    status(bounds.require(bounds.top, 65537), StatusCode::no_memory, "one byte beyond range");
    for (usize used = 0; used <= 65536; used += 256)
    {
        const uptr sp = bounds.top - used;
        auto remaining = bounds.remaining(sp);
        check(remaining && remaining.value() == 65536 - used, "remaining follows actual SP");
        status(bounds.require(sp, 65536 - used), StatusCode::ok, "exact suffix accepted");
        status(bounds.require(sp, 65537 - used), StatusCode::no_memory, "oversized suffix rejected");
    }
    constexpr StackBounds high{~uptr{0} - 4095, ~uptr{0}};
    status(high.require(high.top, 4095), StatusCode::ok, "high address exact subtraction");
    status(high.require(high.top, ~usize{0}), StatusCode::no_memory, "max budget cannot wrap");
    status(high.require(0, 0), StatusCode::fault, "wrapped SP is outside");
    std::printf("PASS stack-bounds checks=%zu scope=hosted-coordinates guards=0\n", checks);
}
