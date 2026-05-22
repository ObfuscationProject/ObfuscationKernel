#pragma once

#include "ok/core/kernel.hpp"

namespace ok
{

Status run_unix_vfs_roadmap_tests(Kernel &kernel, KernelTestReport &report);
Status run_linux_abi_roadmap_tests(Kernel &kernel, KernelTestReport &report);

} // namespace ok
