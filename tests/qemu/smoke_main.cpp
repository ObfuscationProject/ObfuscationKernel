#include "ok/arch/arch.hpp"
#include "ok/core/kernel.hpp"

#include <iostream>

namespace {

int fail(ok::Status status)
{
    std::cerr << "OK_TEST_FAIL code=" << static_cast<unsigned>(status.code())
              << " message=" << status.message() << '\n';
    return 1;
}

} // namespace

int main()
{
    ok::Kernel kernel;
    ok::KernelConfig config {};
    config.architecture = ok::arch::configured_architecture();
    const auto architecture = config.architecture;

    if (auto status = kernel.boot(config); !status.ok()) {
        return fail(status);
    }
    if (auto status = kernel.run_smoke_suite(); !status.ok()) {
        return fail(status);
    }

    std::cout << "OK_TEST_PASS arch=" << ok::arch::to_string(architecture)
              << " processes=" << kernel.scheduler().process_count()
              << " cpus=" << kernel.topology().online_count()
              << " drivers=" << kernel.drivers().driver_count()
              << " free_frames=" << kernel.memory().frames().free_frames()
              << " syscalls=" << kernel.syscalls().handler_count()
              << " debug_test_points=" << kernel.debug_test_points_run()
              << '\n';
    return 0;
}
