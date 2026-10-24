// Included in the private serialized runtime after signal-owner definitions.
Status private_child_exit_commit(Kernel &kernel, PrivateChildExit::Completion &completion)
{
    PrivateChildExit::Notification notification{};
    const auto status = PrivateChildExit::commit(kernel.scheduler(), completion, private_signal_find, notification);
    if (!status) return status;
    if constexpr (native_signal_diagnostics)
    {
        emit("OK_NATIVE_CHILD_EXIT child="); emit_number(completion.child);
        emit(" parent="); emit_number(completion.parent);
        emit(" parent_tid="); emit_number(completion.parent_tid);
        emit(" parent_epoch="); emit_number(completion.parent_epoch);
        emit(" uid="); emit_number(completion.uid);
        emit(" code="); emit_number(static_cast<u32>(completion.notification.code()));
        emit(" status="); emit_number(static_cast<u32>(completion.notification.status()));
        emit(" wait_code="); emit_number(static_cast<u32>(completion.wait_cause.code()));
        emit(" wait_status="); emit_number(static_cast<u32>(completion.wait_cause.status()));
        emit(" published="); emit_number(completion.published);
        emit(" notify="); emit_number(completion.notify);
        emit(" queued="); emit_number(notification == PrivateChildExit::Notification::queued);
        emit(" coalesced="); emit_number(notification == PrivateChildExit::Notification::coalesced);
        emit(" autoreap="); emit_number(completion.autoreap); emit("\n");
    }
    return Status::success();
}

bool private_child_reap(Kernel &kernel)
{
    sched::OrphanReapProgress progress{};
    const auto status = kernel.scheduler().reap_orphans_checked(progress);
    if (native_signal_diagnostics && (progress.reaped || !status))
    {
        emit("OK_NATIVE_CHILD_REAP count="); emit_number(progress.reaped);
        emit(" failed_pid="); emit_number(progress.failed_pid);
        emit(" failed_after_erase="); emit_number(progress.failed_after_erase);
        emit(" success="); emit_number(status.ok()); emit("\n");
    }
    if (!status) { runtime_failure = status; started = false; }
    return status.ok();
}
