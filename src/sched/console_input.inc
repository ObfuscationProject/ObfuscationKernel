ProcessControlBlock *SchedulerState::console_caller()
{
    auto *process = find(current_pid());
    auto *thread = current_thread();
    if (!process || !thread || thread->owner != process->pid() || !thread->native_active ||
        thread->state != ProcessState::running || process->state() != ProcessState::running ||
        process->execution() != ProcessExecution::user_process)
        return nullptr;
    return process;
}

Result<u32> SchedulerState::claim_console(ProcessId target)
{
    auto *caller = console_caller();
    if (!caller || !console_input_.manager(caller->pid()))
        return Status::denied("console claim requires the running boot init");
    auto *child = find(target);
    if (!child || child->parent_pid() != caller->pid() || child->console_epoch_ ||
        child->execution() != ProcessExecution::user_process ||
        child->state() == ProcessState::zombie || child->state() == ProcessState::exited ||
        !child->children().empty())
        return Status::denied("console claim requires a new direct user child");
    usize live = 0;
    for (const auto &thread : child->threads()) live += thread.state != ProcessState::exited;
    if (live != 1) return Status::denied("console claim requires one live child thread");
    auto issued = console_input_.claim(caller->pid(), target);
    if (issued)
    {
        child->console_epoch_ = issued.value();
        console_terminal_ = {};
    }
    return issued;
}

Status SchedulerState::revoke_console(u32 expected_epoch)
{
    auto *caller = console_caller();
    if (!caller) return Status::denied("console revoke requires a running user owner");
    auto status = console_input_.revoke(caller->pid(), expected_epoch);
    if (status)
    {
        console_terminal_ = {};
        cancel_console_epoch(expected_epoch);
    }
    return status;
}

Status SchedulerState::flush_console(u32 expected_epoch)
{
    auto *caller = console_caller();
    if (!caller) return Status::denied("console flush requires a running user owner");
    return console_input_.flush(caller->pid(), caller->console_epoch_, expected_epoch);
}

bool SchedulerState::console_authorized(ProcessId pid, u32 expected_epoch) const
{
    const auto live = [](const ProcessControlBlock *process) {
        if (!process || process->execution() != ProcessExecution::user_process ||
            process->state() == ProcessState::zombie || process->state() == ProcessState::exited)
            return false;
        for (const auto &thread : process->threads())
            if (thread.state != ProcessState::exited) return true;
        return false;
    };
    const auto *process = find(pid);
    return live(process) && live(find(console_input_.leader())) &&
        console_input_.authorized(pid, process->console_epoch_, expected_epoch);
}

bool SchedulerState::console_readable(ProcessId pid, u32 expected_epoch) const
{
    const auto *process = find(pid);
    return console_read_authority(pid, expected_epoch).ok() &&
        console_input_.readable(pid, process->console_epoch_, expected_epoch);
}

Status SchedulerState::console_read_authority(ProcessId pid, u32 expected_epoch) const
{
    if (!console_authorized(pid, expected_epoch))
        return Status::denied("console read has no current lease authority");
    const auto *process = find(pid);
    if (console_terminal_.epoch &&
        (console_terminal_.epoch != console_input_.epoch() ||
         process->session_id() != console_terminal_.session ||
         process->process_group_id() != console_terminal_.foreground))
        return Status::io_error("console read requires the controlling foreground group");
    return {};
}

Result<i64> SchedulerState::console_control(ConsoleControlOperation operation, i32 value)
{
    auto *caller = console_caller();
    if (!caller || !console_authorized(caller->pid(), caller->console_epoch_))
        return Status::not_terminal("caller has no current console association");
    const auto epoch = console_input_.epoch();
    if (operation == ConsoleControlOperation::acquire)
    {
        if (value) return Status::unsupported("forced console acquisition is unavailable");
        if (caller->session_id() != caller->pid() || console_input_.leader() != caller->pid())
            return Status::permission("only the console lease session leader may acquire it");
        if (console_terminal_.epoch)
        {
            if (console_terminal_.epoch != epoch || console_terminal_.session != caller->session_id())
                return Status::permission("console already belongs to another session");
            return i64{0};
        }
        console_terminal_ = {epoch, caller->session_id(), caller->process_group_id()};
        cancel_console_epoch(epoch);
        return i64{0};
    }
    if (console_terminal_.epoch != epoch || console_terminal_.session != caller->session_id())
        return Status::not_terminal("console is not the caller's controlling terminal");
    switch (operation)
    {
    case ConsoleControlOperation::get_session:
        return static_cast<i64>(console_terminal_.session);
    case ConsoleControlOperation::get_foreground:
        return static_cast<i64>(console_terminal_.foreground);
    case ConsoleControlOperation::set_foreground:
    {
        if (value < 0) return Status::invalid_argument("foreground group is negative");
        bool group = false, same_session = false;
        // Retained zombie membership reserves a PGID until normal reap.
        for (const auto &member : processes_)
            if (member.process_group_id() == static_cast<ProcessId>(value))
            {
                group = true;
                same_session |= member.session_id() == console_terminal_.session;
            }
        if (!value || !group) return Status::no_process("foreground group does not exist");
        if (!same_session) return Status::permission("foreground group belongs to another session");
        // This bounded profile permits same-session background control. It does
        // not yet implement SIGTTOU or stop/continue job control.
        console_terminal_.foreground = static_cast<ProcessId>(value);
        cancel_console_epoch(epoch);
        return i64{0};
    }
    default:
        return Status::not_terminal("console control operation is unavailable");
    }
}

Result<usize> SchedulerState::read_console(ProcessId pid, u32 expected_epoch, std::span<std::byte> output)
{
    const auto *caller = console_caller();
    if (!caller || caller->pid() != pid)
        return Status::denied("console read has no current lease authority");
    if (auto status = console_read_authority(pid, expected_epoch); !status) return status;
    return console_input_.read(pid, caller->console_epoch_, expected_epoch, output);
}

void SchedulerState::console_input(char value, WakeSource source)
{
    if (console_input_.active() && !console_authorized(console_input_.leader(), console_input_.epoch()))
        abort_console_input();
    if (console_input_.input(value))
        wake_waiters(ThreadWaitReason::console_read, console_input_.epoch(), source);
}

void SchedulerState::cancel_console_epoch(u32 epoch)
{
    for (auto &process : processes_)
        for (auto &thread : process.threads())
            if (thread.state == ProcessState::blocked && thread.wait_reason == ThreadWaitReason::console_read &&
                thread.wait_key == epoch)
                static_cast<void>(complete_wait(thread.wait_registration.ticket(), WaitCompletion::cancelled));
}

void SchedulerState::retire_console_process(ProcessId pid)
{
    const auto epoch = console_input_.epoch();
    if (console_input_.retire(pid))
    {
        console_terminal_ = {};
        cancel_console_epoch(epoch);
    }
}

void SchedulerState::abort_console_input()
{
    retire_console_process(console_input_.leader());
}

Status SchedulerState::shutdown_console_status() const
{
    return console_input_.active() || console_input_.buffered()
        ? Status::busy("shutdown retains a console input lease or buffered input") : Status{};
}
