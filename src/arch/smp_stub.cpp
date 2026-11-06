#include "ok/arch/smp.hpp"

// Profiles without a secondary-CPU start path run on the boot CPU alone.
namespace ok::arch::smp
{

Status discover(boot::CpuTopologyMap &map)
{
    map = {};
    map.count = 1;
    map.cpus[0] = {.hardware_id = 0, .enabled = true, .boot = true};
    map.up_reason = "profile-up";
    return Status::success();
}

std::string_view start_method_name(const boot::CpuTopologyMap &) noexcept
{
    return "unsupported";
}

std::string_view clock_sync_name() noexcept
{
    return {};
}

Status prepare_boot_cpu(const boot::CpuTopologyMap &)
{
    return Status::unsupported("this profile starts no secondary CPUs");
}

u64 current_hardware_id() noexcept
{
    return 0;
}

Status start(ok::smp::CpuId, u64, uptr, uptr)
{
    return Status::unsupported("this profile starts no secondary CPUs");
}

void send_ipi(ok::smp::CpuId, u64) noexcept
{
}

void wait_local() noexcept
{
}

} // namespace ok::arch::smp
