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

// Claim, revoke and retirement start a fresh console terminal (Linux
// tty_init_termios on the first open after the last close).
void SchedulerState::reset_console_terminal()
{
    console_terminal_.reset(tty::console_compat_termios(), tty::console_window());
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
        reset_console_terminal();
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
        reset_console_terminal();
        cancel_console_epoch(expected_epoch);
    }
    return status;
}

Status SchedulerState::flush_console(u32 expected_epoch)
{
    auto *caller = console_caller();
    if (!caller) return Status::denied("console flush requires a running user owner");
    auto status = console_input_.flush(caller->pid(), caller->console_epoch_, expected_epoch);
    // A cooked line typed before the prompt must not survive the flush.
    if (status) console_terminal_.ldisc.flush_input();
    return status;
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

// The console treats VTIME as 0 (docs/modules/tty.md section 14, item 7).
tty::Termios SchedulerState::console_read_termios() const
{
    auto termios = console_terminal_.termios;
    termios[tty::Cc::time] = 0;
    return termios;
}

bool SchedulerState::console_readable(ProcessId pid, u32 expected_epoch) const
{
    return console_read_authority(pid, expected_epoch).ok() &&
        console_terminal_.ldisc.poll_readable(console_read_termios());
}

Status SchedulerState::console_read_authority(ProcessId pid, u32 expected_epoch) const
{
    if (!console_authorized(pid, expected_epoch))
        return Status::denied("console read has no current lease authority");
    const auto *process = find(pid);
    if (console_terminal_.console_epoch &&
        (console_terminal_.console_epoch != console_input_.epoch() ||
         process->session_id() != console_terminal_.session ||
         process->process_group_id() != console_terminal_.foreground))
        return Status::io_error("console read requires the controlling foreground group");
    return {};
}

Terminal *SchedulerState::terminal(TerminalRef ref)
{
    if (ref == console_terminal_ref) return &console_terminal_;
    if (!ref.valid() || !ref.index) return nullptr;
    return pty_transport_.terminal(static_cast<u16>(ref.index - 1), ref.generation);
}

const Terminal *SchedulerState::terminal(TerminalRef ref) const
{
    return const_cast<SchedulerState *>(this)->terminal(ref);
}

Terminal &SchedulerState::terminal_storage(usize index)
{
    return index ? pty_transport_.terminal_storage(static_cast<u16>(index - 1)) : console_terminal_;
}

Result<TerminalSubject> SchedulerState::terminal_subject(u16 description) const
{
    const auto *open = open_description(description);
    if (!open || !open->used) return Status::bad_descriptor("bad terminal descriptor");
    if (const auto endpoint = open->pty_endpoint)
    {
        // A master names its slave's terminal (Linux real_tty).
        const TerminalRef ref{static_cast<u16>(endpoint.slot + 1), endpoint.generation};
        return TerminalSubject{description, ref,
                               endpoint.side == private_pty::Side::master ? TerminalSide::pty_master
                                                                          : TerminalSide::pty_slave};
    }
    if (!open->console && !open->input_console)
        return Status::not_terminal("descriptor is not a terminal description");
    return TerminalSubject{description, console_terminal_ref, TerminalSide::console};
}

std::optional<TerminalRef> SchedulerState::controlling_terminal(const ProcessControlBlock &process) const
{
    if (process.ctty_detached_ || !process.session_id()) return std::nullopt;
    if (console_terminal_.session == process.session_id() && console_terminal_.console_epoch &&
        console_terminal_.console_epoch == console_input_.epoch() && console_input_.active())
        return console_terminal_ref;
    for (u16 slot = 0; slot < private_pty::RawPtyTable<>::pair_capacity; ++slot)
    {
        const TerminalRef ref{static_cast<u16>(slot + 1), pty_transport_.live_generation(slot)};
        if (const auto *candidate = terminal(ref); candidate && candidate->session == process.session_id()) return ref;
    }
    return std::nullopt;
}

TerminalJobFacts SchedulerState::terminal_job_facts(ProcessId pid, TerminalRef ref) const
{
    const auto *process = find(pid);
    const auto *target = terminal(ref);
    if (!process || !target) return {};
    const auto ctty = controlling_terminal(*process);
    TerminalJobFacts facts{};
    facts.is_ctty = ctty && *ctty == ref;
    facts.has_foreground = target->foreground != 0;
    facts.pgrp = process->process_group_id();
    facts.in_foreground = facts.has_foreground && facts.pgrp == target->foreground;
    return facts;
}

// TIOCSCTTY (Linux tiocsctty without capabilities): the argument matters only
// for a terminal owned by another session, which is refused either way.
Status SchedulerState::acquire_terminal(TerminalRef ref, ProcessControlBlock &caller, bool readable, AcquireMode mode)
{
    auto *target = terminal(ref);
    if (!target) return Status::not_terminal("terminal is not available");
    const auto ctty = controlling_terminal(caller);
    const bool leader = caller.session_id() == caller.pid();
    if (leader && ctty && *ctty == ref) return {};
    if (!leader || ctty) return Status::permission("only a session leader without a terminal may acquire one");
    const bool owned = ref == console_terminal_ref
        ? console_terminal_.console_epoch && console_terminal_.console_epoch == console_input_.epoch()
        : target->session != 0;
    if (owned) return Status::permission("terminal already belongs to another session");
    if (!readable && mode == AcquireMode::ioctl) return Status::permission("terminal acquisition needs a readable descriptor");
    if (ref == console_terminal_ref)
    {
        if (console_input_.leader() != caller.pid())
            return Status::permission("only the console lease leader may acquire it");
        target->console_epoch = console_input_.epoch();
        cancel_console_epoch(console_input_.epoch());
    }
    target->session = caller.session_id();
    target->foreground = caller.process_group_id();
    return {};
}

// Association checks touch no user memory and precede every copy-in. A
// caller outside the console lease may only read the attributes and window.
Status SchedulerState::terminal_check(const TerminalSubject &subject, ConsoleControlOperation operation)
{
    using Op = ConsoleControlOperation;
    if (subject.terminal != console_terminal_ref) return pty_terminal_check(subject, operation);
    switch (operation)
    {
    case Op::detach: case Op::get_pty_number: case Op::set_pty_lock: case Op::count:
        return Status::not_terminal("terminal request is not available on the console");
    default:
        break;
    }
    if (operation == Op::get_attributes || operation == Op::get_window) return {};
    auto *caller = console_caller();
    if (!caller || !console_authorized(caller->pid(), caller->console_epoch_))
        return Status::not_terminal("caller has no current console association");
    if (operation == Op::get_session || operation == Op::get_foreground)
        if (const auto ctty = controlling_terminal(*caller); !ctty || *ctty != console_terminal_ref)
            return Status::not_terminal("console is not the caller's controlling terminal");
    return {};
}

// PTY descriptors (docs/modules/tty.md sections 4, 5 and 8). A hung-up ptmx
// slave refuses everything with EIO but TIOCSPGRP (ENOTTY), as Linux
// hung_up_tty_ioctl. A master is never a controlling terminal (deviation 10);
// TIOCGPTN and TIOCSPTLCK need a ptmx master (raw masters are not in /dev/pts).
Status SchedulerState::pty_terminal_check(const TerminalSubject &subject, ConsoleControlOperation operation)
{
    using Op = ConsoleControlOperation;
    const auto *target = terminal(subject.terminal);
    const auto *open = open_description(subject.description);
    if (!target || !open || !open->pty_endpoint) return Status::not_terminal("terminal is not available");
    const bool master = subject.side == TerminalSide::pty_master;
    if (!master && target->hung_up)
        return operation == Op::set_foreground ? Status::not_terminal("terminal was hung up")
                                               : Status::io_error("terminal was hung up");
    switch (operation)
    {
    case Op::count: case Op::detach: // TIOCNOTTY arrives with S8.
        return Status::not_terminal("terminal request is not available");
    case Op::acquire:
        if (master) return Status::not_terminal("a PTY master is not a controlling terminal");
        break;
    case Op::get_pty_number: case Op::set_pty_lock:
        if (const auto index = pty_transport_.index(open->pty_endpoint); !index) return index.status();
        return {};
    default:
        break;
    }
    const bool needs_caller = operation == Op::acquire || operation == Op::set_foreground ||
        (!master && (operation == Op::get_session || operation == Op::get_foreground));
    auto *caller = console_caller();
    if (needs_caller && !caller) return Status::not_terminal("terminal request has no running caller");
    if (!master && (operation == Op::get_session || operation == Op::get_foreground))
        if (const auto ctty = controlling_terminal(*caller); !ctty || *ctty != subject.terminal)
            return Status::not_terminal("terminal is not the caller's controlling terminal");
    if (operation == Op::get_session && !target->session) return Status::not_terminal("terminal has no session");
    return {};
}

void SchedulerState::terminal_changed(TerminalRef ref)
{
    // Linux n_tty_set_termios and flushes wake readers and writers unconditionally.
    if (ref == console_terminal_ref)
        wake_waiters(ThreadWaitReason::console_read, console_input_.epoch());
    else
        wake_pty({static_cast<u16>(ref.index - 1), ref.generation, private_pty::Side::slave},
                 private_pty::wake_slave_read | private_pty::wake_slave_write | private_pty::wake_master_write);
}

Result<TerminalReply> SchedulerState::terminal_control(const TerminalSubject &subject, const TerminalRequest &request)
{
    using Op = ConsoleControlOperation;
    if (auto status = terminal_check(subject, request.operation); !status) return status;
    auto *target = terminal(subject.terminal);
    if (!target) return Status::not_terminal("terminal is not available");
    auto &tty = *target;
    const bool console = subject.terminal == console_terminal_ref;
    const auto *open = open_description(subject.description);
    const auto endpoint = open ? open->pty_endpoint : private_pty::Handle{};
    TerminalReply reply{};
    const auto set_attributes = [&] {
        const auto old = tty.termios;
        tty.termios = request.termios;
        tty.residue = request.residue;
        const bool readable = tty.ldisc.set_termios(old, request.termios);
        if (!console || readable) terminal_changed(subject.terminal);
    };
    const auto flush_input = [&] {
        tty.ldisc.flush_input();
        if (!console) terminal_changed(subject.terminal);
    };
    switch (request.operation)
    {
    case Op::get_attributes:
        reply.termios = tty.termios;
        reply.residue = tty.residue;
        return reply;
    case Op::get_window:
        reply.window = tty.window;
        return reply;
    case Op::acquire:
    {
        auto *caller = console_caller();
        if (auto status = acquire_terminal(subject.terminal, *caller, open && open->readable, AcquireMode::ioctl); !status)
            return status;
        return reply;
    }
    case Op::get_session:
        reply.value = static_cast<i64>(tty.session);
        return reply;
    case Op::get_foreground:
        reply.value = static_cast<i64>(tty.foreground);
        return reply;
    case Op::set_foreground:
    {
        // Linux tiocspgrp order: EINVAL, ENOTTY, ESRCH, EPERM.
        const auto value = request.scalar;
        if (value < 0) return Status::invalid_argument("foreground group is negative");
        auto *caller = console_caller();
        if (const auto ctty = controlling_terminal(*caller); !ctty || *ctty != subject.terminal)
            return Status::not_terminal("terminal is not the caller's controlling terminal");
        bool group = false, same_session = false;
        // Retained zombie membership reserves a PGID until normal reap.
        for (const auto &member : processes_)
            if (member.process_group_id() == static_cast<ProcessId>(value))
            {
                group = true;
                same_session |= member.session_id() == tty.session;
            }
        if (!value || !group) return Status::no_process("foreground group does not exist");
        if (!same_session) return Status::permission("foreground group belongs to another session");
        // Background callers are not gated before S8 (no SIGTTOU yet).
        tty.foreground = static_cast<ProcessId>(value);
        if (console) cancel_console_epoch(console_input_.epoch());
        return reply;
    }
    case Op::set_flush:
        flush_input();
        set_attributes();
        return reply;
    case Op::set_now:
    case Op::set_drain: // Terminal output is never held back for the drain to wait on.
        set_attributes();
        return reply;
    case Op::drain_break:
        return reply;
    case Op::flush:
    {
        if (request.scalar < 0 || request.scalar > 2) return Status::invalid_argument("TCFLSH queue selector");
        const bool input = request.scalar != static_cast<i32>(tty::FlushQueue::output);
        const bool output = request.scalar != static_cast<i32>(tty::FlushQueue::input);
        if (console)
        {
            if (input) tty.ldisc.flush_input();
            return reply;
        }
        const auto flushed = pty_transport_.flush(endpoint, input, output);
        if (!flushed) return flushed.status();
        wake_pty(endpoint, flushed.value().wakes);
        return reply;
    }
    case Op::set_window:
        if (!(tty.window == request.window))
        {
            tty.window = request.window;
            static_cast<void>(tty.owe(tty.foreground, tty::TerminalSignal::winch, true));
        }
        return reply;
    case Op::input_pending:
    {
        if (console)
        {
            reply.value = static_cast<i64>(tty.ldisc.inq(tty.termios));
            return reply;
        }
        const auto pending = pty_transport_.input_pending(endpoint);
        if (!pending) return pending.status();
        reply.value = static_cast<i64>(pending.value());
        return reply;
    }
    case Op::get_pty_number:
    {
        const auto index = pty_transport_.index(endpoint);
        if (!index) return index.status();
        reply.value = index.value();
        return reply;
    }
    case Op::set_pty_lock:
        if (auto status = pty_transport_.set_locked(endpoint, request.scalar != 0); !status) return status;
        return reply;
    default:
        return Status::not_terminal("terminal request is not available");
    }
}

Result<TerminalLdisc::ReadPlan> SchedulerState::prepare_console_read(ProcessId pid, u32 expected_epoch, usize count,
                                                                     bool nonblocking)
{
    const auto *caller = console_caller();
    if (!caller || caller->pid() != pid)
        return Status::denied("console read has no current lease authority");
    if (auto status = console_read_authority(pid, expected_epoch); !status) return status;
    const auto plan = console_terminal_.ldisc.plan_read(console_read_termios(), count, nonblocking, false);
    using Wait = TerminalLdisc::ReadWait;
    if (plan.wait == Wait::would_block) return Status::would_block("console input is not ready");
    if (plan.wait != Wait::none) return Status::would_block("console read waits for input");
    return plan;
}

usize SchedulerState::preview_console_read(const TerminalLdisc::ReadPlan &plan, std::span<std::byte> output,
                                           usize offset) const
{
    return console_terminal_.ldisc.preview(plan, output, offset);
}

Status SchedulerState::commit_console_read(const TerminalLdisc::ReadPlan &plan)
{
    return console_terminal_.ldisc.consume(plan) ? Status{} : Status::busy("console input changed during the read");
}

TerminalLdisc::Input SchedulerState::console_input(char value, WakeSource source)
{
    if (console_input_.active() && !console_authorized(console_input_.leader(), console_input_.epoch()))
        abort_console_input();
    if (!console_input_.admit()) return {};
    auto &console = console_terminal_;
    const std::array<std::byte, 1> bytes{static_cast<std::byte>(value)};
    // A full read buffer drops the byte (a UART overrun); there is no writer to hold back.
    auto input = console.ldisc.receive(console.termios, bytes, console.outbox_room(), true);
    constexpr std::array<tty::TerminalSignal, 3> isig{tty::TerminalSignal::interrupt, tty::TerminalSignal::quit,
                                                      tty::TerminalSignal::suspend};
    for (usize index = 0; index < input.signal_count; ++index)
        for (const auto signal : isig)
            if (input.signals[index] == signal) static_cast<void>(console.owe(console.foreground, signal, true));
    if (input.became_readable) wake_waiters(ThreadWaitReason::console_read, console_input_.epoch(), source);
    return input;
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
        reset_console_terminal();
        cancel_console_epoch(epoch);
    }
}

void SchedulerState::abort_console_input()
{
    retire_console_process(console_input_.leader());
}

Status SchedulerState::shutdown_console_status() const
{
    return console_input_.active() || console_terminal_.ldisc.buffered()
        ? Status::busy("shutdown retains a console input lease or buffered input") : Status{};
}
