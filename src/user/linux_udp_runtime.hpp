// Included inside the runtime's private namespace after set_result is defined.
linux_udp::OperationScratch linux_udp_operations;

// UDP errors are already target-numbered. Generic bootstrap translation would
// change MIPS ENOTSOCK (95) into EOPNOTSUPP (122).
template <lu::Profile Target>
Status encode_linux_udp_target_result(hw::UserFrame &frame, i64 result)
{
    if (result < -4095)
        return Status::invalid_argument("Linux UDP result is outside the target errno range");
    const u32 target_error = result < 0 ? static_cast<u32>(-result) : 0;
    return lu::NativeBridge<Target>::encode_return(frame,
        {target_error ? 0 : static_cast<u64>(result), target_error});
}

Status set_linux_udp_result(const sched::ProcessControlBlock &process,
                            hw::UserFrame &frame, i64 result)
{
    syscall_origins.revoke_frame(process, frame);
    if (auto status = encode_linux_udp_target_result<lu::configured_native_profile()>(frame, result); !status)
        return status;
#ifdef OK_PRIVATE_MIPS64_N64_SIGNAL
    return private_n64_completion(process, frame, result);
#elif defined(OK_PRIVATE_PPC32_SIGNAL)
    return private_ppc_completion(process, frame, result);
#else
    return Status::success();
#endif
}

template <lu::Profile Target>
bool linux_udp_delay_slot_excluded(const hw::Trap &trap)
{
    if constexpr (Target == lu::Profile::mips || Target == lu::Profile::mips64)
        return trap.kind == hw::TrapKind::syscall && trap.vector == 8 &&
            ((trap.error_code >> 2) & 0x1f) == 8 && (trap.error_code & 0x80000000U);
    else
        return false;
}

bool linux_udp_frame(const hw::UserFrame &frame)
{
    return linux_udp_immediate_operation_for_frame<lu::configured_native_profile()>(frame) !=
        LinuxUdpImmediateOperation::none;
}

Result<LinuxUdpReceiveBegin> begin_linux_udp_receive(Kernel &kernel,
    sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread,
    LinuxUdpImmediateAccess<lu::configured_native_profile()> &access,
    linux_udp::OriginalUdpDescription &original, LinuxUdpReceiveAdmissionToken &&admission);

// Only this concrete private authority supplies the runtime begin/publisher.
// OperationScratch exposes no arbitrary callback injection seam for handoff.
class LinuxUdpOperationRuntimeAuthority final
{
  public:
    static Status dispatch(Kernel &kernel, sched::ProcessControlBlock &process,
                           sched::ThreadControlBlock &thread)
    {
        using Access = LinuxUdpImmediateAccess<lu::configured_native_profile()>;
        auto admitted = Access::acquire(syscall_origins, process, thread);
        if (!admitted) return admitted.status();
        auto &access = admitted.value();
        auto begin = [&](Access &current, linux_udp::OriginalUdpDescription &original,
                         LinuxUdpReceiveAdmissionToken &&admission) -> Result<LinuxUdpReceiveBegin> {
            return begin_linux_udp_receive(kernel, process, thread, current, original, std::move(admission));
        };
        auto publish = [&](i64 value) -> Status {
            if (!access.live() || kernel.scheduler().find(process.pid()) != &process ||
                kernel.scheduler().find_thread(process.pid(), thread.tid) != &thread ||
                kernel.scheduler().current_thread() != &thread)
                return Status::denied("Linux UDP result lost its original captured frame");
            thread.syscall_pending = false;
            return set_linux_udp_result(process, thread.native_frame, value);
        };
        return linux_udp_operations.dispatch<lu::configured_native_profile()>(
            kernel, process, thread, access, begin, publish, runtime_failure);
    }
};

Status dispatch_linux_udp(Kernel &kernel, sched::ProcessControlBlock &process,
                          sched::ThreadControlBlock &thread)
{
    return LinuxUdpOperationRuntimeAuthority::dispatch(kernel, process, thread);
}
