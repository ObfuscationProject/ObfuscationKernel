// Included in scheduler.cpp inside ok::sched, after pty_pair.inc.
// Terminal device nodes and open() (docs/modules/tty.md sections 2 and 3).

namespace
{
bool terminal_open_live(const ProcessControlBlock *process)
{
    return process && process->state() != ProcessState::zombie && process->state() != ProcessState::exited;
}
} // namespace

Result<TerminalOpenResult> SchedulerState::open_terminal(ProcessId pid, u16 id, const TerminalOpenRequest &request)
{
    using Kind = fs::TerminalNodeKind;
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission is closed");
    auto *open = open_description(id);
    if (!pty_bindable(open)) return Status::invalid_argument("terminal open needs a fresh provider-free description");
    if (!request.readable && !request.writable) return Status::invalid_argument("terminal open needs an access mode");
    auto *caller = find(pid);
    if (!terminal_open_live(caller)) return Status::no_process("terminal open has no live caller");
    // Resolve and admit; nothing changes before the commit below.
    const auto kind = request.node.kind;
    private_pty::Handle slave{};
    bool console = false;
    switch (kind)
    {
    case Kind::controlling:
    {
        const auto ctty = controlling_terminal(*caller);
        if (!ctty) return Status::no_device("caller has no controlling terminal");
        if (*ctty == console_terminal_ref) console = true;
        else
        {
            slave = {static_cast<u16>(ctty->index - 1), ctty->generation, private_pty::Side::slave};
            if (auto status = pty_transport_.admit_open(slave); !status) return status;
        }
        break;
    }
    case Kind::console:
        console = true;
        break;
    case Kind::multiplexer:
        break;
    case Kind::pts_slave:
    {
        const auto index = request.node.index;
        if (index >= fs::pts_capacity) return Status::not_found("no such pts entry");
        const auto entry = terminal_namespace_.entry(index);
        slave = pty_transport_.slave_handle(index);
        if (!entry.present || entry.generation != request.node.generation || slave.generation != entry.generation)
            return Status::not_found("pts entry is gone");
        if (auto status = pty_transport_.admit_open(slave); !status) return status;
        break;
    }
    case Kind::none:
    default:
        return Status::invalid_argument("node is not a terminal");
    }
    // Commit: only the pair allocation below can still fail, before any change.
    TerminalOpenResult result{.bound = kind};
    if (kind == Kind::multiplexer)
    {
        auto master = bind_pty_multiplexer(id, request.euid, request.egid);
        if (!master)
            return master.status().code() == StatusCode::system_file_limit
                ? Status::no_space("every PTY pair slot is in use") : master.status();
        result.index = master.value().slot;
    }
    else if (!console)
    {
        if (auto status = bind_pty_slave(id, slave); !status) return status;
        result.index = slave.slot;
    }
    else
    {
        open->console = request.writable;
        open->input_console = request.readable;
    }
    open->readable = request.readable;
    open->writable = request.writable;
    open->flags = request.flags;
    open->terminal_origin = kind;
    // Linux tty_open_proc_set_tty: /dev/console, /dev/tty and masters never
    // acquire; a write-only open does not either. A failed precondition is
    // silently skipped.
    if (kind == Kind::pts_slave && !request.no_controlling_terminal && request.readable &&
        !controlling_terminal(*caller))
    {
        const TerminalRef ref{static_cast<u16>(slave.slot + 1), slave.generation};
        result.acquired = acquire_terminal(ref, *caller, true, AcquireMode::open).ok();
    }
    return result;
}

Result<TerminalStatus> SchedulerState::terminal_status(u16 id) const
{
    using Kind = fs::TerminalNodeKind;
    const auto *open = open_description(id);
    if (!open) return Status::bad_descriptor("bad terminal descriptor");
    auto kind = open->terminal_origin;
    if (kind == Kind::none && (open->console || open->input_console)) kind = Kind::console;
    if (kind == Kind::none) return Status::not_terminal("description was not opened from a terminal node");
    TerminalStatus status{.kind = kind};
    u16 index = 0;
    switch (kind)
    {
    case Kind::controlling:
    case Kind::multiplexer:
        status.mode = 0666;
        break;
    case Kind::console:
        status.mode = 0600;
        break;
    case Kind::pts_slave:
    {
        index = open->pty_endpoint.slot;
        const auto entry = terminal_namespace_.entry(index);
        // A removed entry keeps its last owner and mode for open slaves.
        if (entry.generation != open->pty_endpoint.generation)
            return Status::not_terminal("pts description has no namespace entry");
        status.uid = entry.uid;
        status.gid = entry.gid;
        status.mode = entry.mode;
        break;
    }
    case Kind::none:
        break;
    }
    status.rdev = fs::terminal_device(kind, index);
    status.inode = fs::terminal_inode(kind, index);
    return status;
}

Result<fs::TerminalNode> SchedulerState::pts_node(u16 id) const
{
    const auto *open = open_description(id);
    if (!open || open->terminal_origin != fs::TerminalNodeKind::pts_slave || !open->pty_endpoint)
        return Status::not_terminal("description is not a /dev/pts slave");
    return fs::TerminalNode{.kind = fs::TerminalNodeKind::pts_slave, .index = open->pty_endpoint.slot,
                            .generation = open->pty_endpoint.generation};
}

namespace
{
// The entry a pts node names while its pair lives (open slaves keep it after
// the master's last close removed it from the listing).
bool terminal_open_pts_entry(const fs::TerminalNamespace &terminals, const private_pty::RawPtyTable<> &table,
                   fs::TerminalNode node, fs::PtsEntryView &entry)
{
    if (node.kind != fs::TerminalNodeKind::pts_slave || node.index >= fs::pts_capacity) return false;
    entry = terminals.entry(node.index);
    return entry.generation && entry.generation == node.generation && table.live_generation(node.index) == node.generation;
}
} // namespace

Status SchedulerState::change_pts_mode(fs::TerminalNode node, u32 mode, user::Credentials caller)
{
    fs::PtsEntryView entry{};
    if (!terminal_open_pts_entry(terminal_namespace_, pty_transport_, node, entry)) return Status::not_found("pts entry is gone");
    const auto decision = fs::mode_change_policy(entry.uid, entry.gid, mode, caller);
    if (!decision.authorized) return Status::permission("only the owner can chmod a pts entry");
    entry.mode = static_cast<u16>(decision.permissions);
    terminal_namespace_.publish(node.index, entry);
    return {};
}

Status SchedulerState::change_pts_owner(fs::TerminalNode node, u32 uid, u32 gid, user::Credentials caller)
{
    fs::PtsEntryView entry{};
    if (!terminal_open_pts_entry(terminal_namespace_, pty_transport_, node, entry)) return Status::not_found("pts entry is gone");
    const auto decision = fs::owner_change_policy(entry.uid, entry.gid, entry.mode, uid, gid, caller);
    if (!decision.authorized) return Status::permission("ownership change requires root");
    entry.uid = decision.uid;
    entry.gid = decision.gid;
    entry.mode = static_cast<u16>(decision.permissions);
    terminal_namespace_.publish(node.index, entry);
    return {};
}
