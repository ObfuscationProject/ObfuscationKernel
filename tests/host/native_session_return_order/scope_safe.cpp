// Retain public lifecycle/refusal scenarios; the inherited corruption campaign
// is compiled for preservation but none of its mutation functions is executed.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main retained_scope_main
#include "../ext4_direct_read_scope_tests.cpp"
#undef main
#pragma GCC diagnostic pop
int main()
{
    successful_paused_prefix();
    refusals();
    nested_and_abandoned();
    std::printf("DIRECT_READ_SCOPE_SAFE cases=%zu checks=%zu\n", cases, checks);
    return 0;
}
