#include "../kernel/points/deadline_checks.hpp"
#include <cstdio>
int main()
{
    auto status = ok::tests::check_deadline_wait();
    if (!status)
    {
        std::fprintf(stderr, "%.*s\n", static_cast<int>(status.message().size()), status.message().data());
        return 1;
    }
    std::puts("PASS: deadline ownership, cancellation, generation and overflow (host model)");
}
