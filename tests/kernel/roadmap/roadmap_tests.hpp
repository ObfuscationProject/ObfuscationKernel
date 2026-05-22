#pragma once

#include "ok/core/kernel.hpp"

namespace ok
{

Status run_module_roadmap_tests(KernelTestReport &report);
Status run_vm_roadmap_tests(Kernel &kernel, KernelTestReport &report);
Status run_process_roadmap_tests(Kernel &kernel, KernelTestReport &report);
Status run_unix_vfs_roadmap_tests(Kernel &kernel, KernelTestReport &report);
Status run_linux_abi_roadmap_tests(Kernel &kernel, KernelTestReport &report);
Status run_driver_abi_roadmap_tests(KernelTestReport &report);
Status run_network_storage_roadmap_tests(Kernel &kernel, KernelTestReport &report);
Status run_smp_irq_preempt_roadmap_tests(Kernel &kernel, KernelTestReport &report);

} // namespace ok
