#include "ok/fs/ext4/journal.hpp"

ok::Result<ok::fs::jbd2::RecoveryIo> bind_jbd2_recovery(ok::driver::BlockDevice &device,
                                                     ok::fs::jbd2::RecoveryLayout layout)
{
    return ok::fs::jbd2::RecoveryIo::open(device, layout);
}

ok::Status run_jbd2_recovery(ok::fs::jbd2::ReplayExecutor<> &executor, ok::fs::jbd2::RecoveryIo &io,
                             std::span<const ok::u8, 16> uuid, const ok::fs::jbd2::TargetDomain &domain)
{
    return executor.run(io, uuid, domain);
}
ok::Status run_internal_jbd2_recovery(ok::fs::jbd2::ReplayExecutor<> &executor, ok::fs::jbd2::RecoveryIo &io,
                                      std::span<const ok::u8, 16> uuid, const ok::fs::jbd2::TargetDomain &domain)
{
    return executor.run(io, uuid, domain, ok::fs::jbd2::DescriptorUuidPolicy::linux_internal_zero);
}
