#include "../kernel/points/shutdown_checks.hpp"
ok::Status compile_shutdown_debug_point(ok::Kernel &kernel)
{
    if (auto status = ok::tests::check_native_power_wire_refusal_model(kernel); !status) return status;
    return ok::tests::check_shutdown_refusal_model(kernel);
}
