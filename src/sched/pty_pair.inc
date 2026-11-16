// Included in scheduler.cpp inside ok::sched. Uses real descriptor reservations.
PreparedPtyPair::PreparedPtyPair(SchedulerState &owner, DescriptorReservation first,
    DescriptorReservation second, private_pty::PairHandles handles, u32 flags)
    : owner_(&owner), first_(std::move(first)), second_(std::move(second)), handles_(handles), flags_(flags) {}
PreparedPtyPair::PreparedPtyPair(PreparedPtyPair &&other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), first_(std::move(other.first_)),
      second_(std::move(other.second_)), handles_(other.handles_), flags_(other.flags_) {}
PreparedPtyPair::~PreparedPtyPair()
{
    if (owner_)
    {
        auto *owner = owner_;
        const auto status = cancel();
        owner->record_resource_failure(status, -1, invalid_description, 0, false);
    }
}
std::array<i32, 2> PreparedPtyPair::descriptors() const
{
    return {first_ ? first_->descriptor() : -1, second_ ? second_->descriptor() : -1};
}
Status PreparedPtyPair::validate() const
{
    if (!owner_ || !first_ || !second_) return Status::busy("PTY pair preparation retired");
    if (auto status = first_->validate(); !status) return status;
    if (auto status = second_->validate(); !status) return status;
    if (first_->descriptor() == second_->descriptor() || first_->description() == second_->description() ||
        handles_.master.slot != handles_.slave.slot || handles_.master.generation != handles_.slave.generation ||
        handles_.master.side != private_pty::Side::master || handles_.slave.side != private_pty::Side::slave)
        return Status::busy("PTY pair ownership identity changed");
    if (!owner_->pty_transport_.readiness(handles_.master) || !owner_->pty_transport_.readiness(handles_.slave))
        return Status::busy("PTY pair provider ownership expired");
    return {};
}
void PreparedPtyPair::preserve_failed_holder()
{
    if (first_) first_->preserve_failed_holder();
    if (second_) second_->preserve_failed_holder();
    owner_ = nullptr;
}
Status PreparedPtyPair::cancel()
{
    if (!owner_) return {};
    if (auto status = validate(); !status) { preserve_failed_holder(); return status; }
    // No callbacks occur between this validation and provider/slot retirement.
    auto retired = owner_->pty_transport_.retire(handles_.master);
    if (!retired) { const auto status = retired.status(); preserve_failed_holder(); return status; }
    retired = owner_->pty_transport_.retire(handles_.slave);
    if (!retired) { const auto status = retired.status(); preserve_failed_holder(); return status; }
    if (auto status = first_->cancel(); !status) { preserve_failed_holder(); return status; }
    if (auto status = second_->cancel(); !status) { preserve_failed_holder(); return status; }
    owner_ = nullptr;
    return {};
}
void PreparedPtyPair::publish_checked() noexcept
{
    constexpr u32 nonblock = 0x0800u, cloexec = 0x080000u;
    const auto flags = 2u | (flags_ & nonblock);
    static_cast<void>(first_->publish_checked(OpenFileDescription{.readable = true, .writable = true,
        .flags = flags, .pty_endpoint = handles_.master}, (flags_ & cloexec) != 0));
    static_cast<void>(second_->publish_checked(OpenFileDescription{.readable = true, .writable = true,
        .flags = flags, .pty_endpoint = handles_.slave}, (flags_ & cloexec) != 0));
    owner_ = nullptr;
}
Result<PreparedPtyPair> SchedulerState::prepare_pty_pair(u64 pid, u32 flags)
{
    constexpr u32 allowed = 0x0800u | 0x080000u;
    if (flags & ~allowed) return Status::invalid_argument("unsupported raw PTY pair flags");
    auto first = prepare_descriptor(pid);
    if (!first) return first.status();
    auto second = prepare_descriptor(pid);
    if (!second) return second.status();
    auto pair = pty_transport_.create();
    if (!pair) return pair.status();
    return PreparedPtyPair{*this, std::move(first.value()), std::move(second.value()), pair.value(), flags};
}
Result<private_pty::Readiness> SchedulerState::pty_description_readiness(u16 id) const
{
    const auto *open = open_description(id);
    if (!open || !open->references || open->reservation || !open->pty_endpoint)
        return Status::bad_descriptor("description is not a retained raw PTY");
    return pty_transport_.readiness(open->pty_endpoint);
}
void SchedulerState::wake_pty(private_pty::Handle endpoint, u8 wakes)
{
    auto master = endpoint, slave = endpoint;
    master.side = private_pty::Side::master;
    slave.side = private_pty::Side::slave;
    if (wakes & private_pty::wake_master_read) wake_waiters(ThreadWaitReason::pty_read, pty_wait_key(master));
    if (wakes & private_pty::wake_master_write) wake_waiters(ThreadWaitReason::pty_write, pty_wait_key(master));
    if (wakes & private_pty::wake_slave_read) wake_waiters(ThreadWaitReason::pty_read, pty_wait_key(slave));
    if (wakes & private_pty::wake_slave_write) wake_waiters(ThreadWaitReason::pty_write, pty_wait_key(slave));
}
namespace
{
// O_NONBLOCK of the description: a slave read then takes what is below VMIN.
bool pty_nonblocking(const OpenFileDescription &open) { return (open.flags & 0x0800u) != 0; }
} // namespace
Result<usize> SchedulerState::read_pty_description(u16 id, std::span<std::byte> output)
{
    auto *open = open_description(id);
    if (!open || !open->references || open->reservation || !open->readable || !open->pty_endpoint)
        return Status::bad_descriptor("description is not a readable retained raw PTY");
    auto result = pty_transport_.read(open->pty_endpoint, output, pty_nonblocking(*open));
    if (!result) return result.status();
    wake_pty(open->pty_endpoint, result.value().change.wakes);
    return result.value().bytes;
}
Result<usize> SchedulerState::write_pty_description(u16 id, std::span<const std::byte> input)
{
    auto *open = open_description(id);
    if (!open || !open->references || open->reservation || !open->writable || !open->pty_endpoint)
        return Status::bad_descriptor("description is not a writable retained raw PTY");
    auto result = pty_transport_.write(open->pty_endpoint, input);
    if (!result) return result.status();
    // Master input stores bytes in the slave's N_TTY: its readers are woken
    // on every stored byte (Linux __receive_buf), its echo wakes master readers.
    wake_pty(open->pty_endpoint, result.value().change.wakes);
    return result.value().bytes;
}

Result<SchedulerState::PreparedPtyRead> SchedulerState::prepare_pty_read(u16 id, usize count, bool timer_expired,
                                                                        private_pty::ReadTimer timer)
{
    auto *open = open_description(id);
    if (!open || !open->references || open->reservation || !open->readable || !open->pty_endpoint)
        return Status::bad_descriptor("description is not a readable retained raw PTY");
    return pty_transport_.prepare_read(open->pty_endpoint, count, pty_nonblocking(*open), timer_expired, timer);
}
Result<SchedulerState::PtyReadState> SchedulerState::pty_read_state(u16 id, usize count,
                                                                    private_pty::ReadTimer timer) const
{
    const auto *open = open_description(id);
    if (!open || !open->references || open->reservation || !open->readable || !open->pty_endpoint)
        return Status::bad_descriptor("description is not a readable retained raw PTY");
    return pty_transport_.read_state(open->pty_endpoint, count, timer);
}
Result<bool> SchedulerState::pty_write_progress(u16 id, std::byte next) const
{
    const auto *open = open_description(id);
    if (!open || !open->references || open->reservation || !open->writable || !open->pty_endpoint)
        return Status::bad_descriptor("description is not a writable retained raw PTY");
    return pty_transport_.write_progress(open->pty_endpoint, next);
}
Result<usize> SchedulerState::commit_pty_read(PreparedPtyRead &preview)
{
    if (!preview.owned_by(pty_transport_))
        return Status::busy("PTY read preview belongs to a different or retired owner");
    const auto endpoint = preview.endpoint();
    const auto result = preview.commit();
    if (!result) return result.status();
    wake_pty(endpoint, result.value().change.wakes);
    return result.value().bytes;
}

// The description must be freshly allocated: used, published, exactly one
// reference (the opener's pin) and no provider of any kind.
namespace
{
bool pty_bindable(const OpenFileDescription *open)
{
    return open && open->used && !open->reservation && open->references == 1 && !open->unix_queue_references &&
        !open->pty_endpoint && !open->node && !open->console && !open->input_console && !open->native_udp_handle &&
        !open->native_tcp_handle && !open->native_shared_file && !open->unix_endpoint &&
        open->pipe == invalid_description && open->unix_write_pipe == invalid_description;
}
} // namespace
Result<private_pty::Handle> SchedulerState::bind_pty_multiplexer(u16 id)
{
    return bind_pty_multiplexer(id, 0, 0);
}
// The new pair is listed in /dev/pts at once, locked, owned by uid:gid.
Result<private_pty::Handle> SchedulerState::bind_pty_multiplexer(u16 id, u32 uid, u32 gid)
{
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission is closed");
    auto *open = open_description(id);
    if (!pty_bindable(open)) return Status::invalid_argument("PTY open needs a fresh provider-free description");
    auto master = pty_transport_.create_multiplexed();
    if (!master) return master.status();
    open->pty_endpoint = master.value();
    terminal_namespace_.publish(master.value().slot, {.generation = master.value().generation, .uid = uid, .gid = gid,
                                                      .mode = fs::pts_default_mode, .present = true, .locked = true});
    return master.value();
}
Status SchedulerState::bind_pty_slave(u16 id, private_pty::Handle slave)
{
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission is closed");
    auto *open = open_description(id);
    if (!pty_bindable(open)) return Status::invalid_argument("PTY open needs a fresh provider-free description");
    if (slave.side != private_pty::Side::slave) return Status::invalid_argument("PTY open names a slave endpoint");
    if (auto status = pty_transport_.admit_open(slave); !status) return status;
    const auto change = pty_transport_.attach(slave);
    open->pty_endpoint = slave;
    wake_pty(slave, change.wakes);
    return {};
}
Result<u32> SchedulerState::pty_index(u16 id) const
{
    const auto *open = open_description(id);
    if (!open || !open->used || !open->pty_endpoint) return Status::not_terminal("description is not a PTY master");
    const auto index = pty_transport_.index(open->pty_endpoint);
    if (!index) return index.status();
    return u32{index.value()};
}
Status SchedulerState::set_pty_lock(u16 id, bool locked)
{
    const auto *open = open_description(id);
    if (!open || !open->used || !open->pty_endpoint) return Status::not_terminal("description is not a PTY master");
    if (auto status = pty_transport_.set_locked(open->pty_endpoint, locked); !status) return status;
    terminal_namespace_.set_locked(open->pty_endpoint.slot, open->pty_endpoint.generation, locked);
    return {};
}
Status SchedulerState::terminal_audit() const
{
    if (!pty_transport_.audit().cleared_outside_live_bytes) return Status::fault("PTY table invariant failed");
    for (const auto &open : descriptions_)
        if (open.used && open.pty_endpoint && !pty_transport_.open_count(open.pty_endpoint))
            return Status::fault("PTY description outlived its endpoint open");
    for (u16 slot = 0; slot < private_pty::RawPtyTable<>::pair_capacity; ++slot)
    {
        const auto generation = pty_transport_.live_generation(slot);
        if (!generation) continue;
        for (const auto side : {private_pty::Side::master, private_pty::Side::slave})
        {
            const private_pty::Handle endpoint{slot, generation, side};
            usize bound = 0;
            for (const auto &open : descriptions_) bound += open.used && open.pty_endpoint == endpoint ? 1 : 0;
            if (bound != pty_transport_.open_count(endpoint))
                return Status::fault("PTY open count differs from its bound descriptions");
        }
    }
    return {};
}
