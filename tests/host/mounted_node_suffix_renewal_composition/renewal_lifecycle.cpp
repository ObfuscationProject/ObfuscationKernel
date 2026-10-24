// Execute authentic renewal lifecycle and detached serial predicates only.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#include "renewal_fixture.cpp"
#pragma GCC diagnostic pop
int main() {
    lifecycle(); detached_serial_controls();
    std::printf("PASS composed renewal lifecycle: checks=%zu renewals=%zu quiet_refusals=%zu detached_controls=%zu; no synthetic live serial history\n",
        checks, renewals, quiet_refusals, detached_controls);
}
