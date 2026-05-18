#pragma once

#include "ok/core/types.hpp"

namespace ok {
class Kernel;
}

namespace ok::test {

#if defined(OK_ENABLE_TEST_POINTS)
inline constexpr bool test_points_enabled = true;
#else
inline constexpr bool test_points_enabled = false;
#endif

[[nodiscard]] Result<usize> run_kernel_test_points(Kernel& kernel);

} // namespace ok::test

