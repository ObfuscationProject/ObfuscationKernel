#include "../kernel/points/persistent_sync_checks.hpp"

ok::Status compile_persistent_sync_debug_point(ok::Kernel &kernel)
{
    return ok::tests::check_persistent_sync_refusal_model(kernel);
}
