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
Result<usize> SchedulerState::read_pty_description(u16 id, std::span<std::byte> output)
{
    auto *open = open_description(id);
    if (!open || !open->references || open->reservation || !open->readable || !open->pty_endpoint)
        return Status::bad_descriptor("description is not a readable retained raw PTY");
    auto result = pty_transport_.read(open->pty_endpoint, output);
    if (!result) return result.status();
    if (result.value().bytes)
    {
        auto peer = open->pty_endpoint;
        peer.side = peer.side == private_pty::Side::master ? private_pty::Side::slave : private_pty::Side::master;
        wake_waiters(ThreadWaitReason::pty_write, pty_wait_key(peer));
    }
    return result.value().bytes;
}
Result<usize> SchedulerState::write_pty_description(u16 id, std::span<const std::byte> input)
{
    auto *open = open_description(id);
    if (!open || !open->references || open->reservation || !open->writable || !open->pty_endpoint)
        return Status::bad_descriptor("description is not a writable retained raw PTY");
    auto result = pty_transport_.write(open->pty_endpoint, input);
    if (!result) return result.status();
    if (result.value().bytes)
    {
        auto peer = open->pty_endpoint;
        peer.side = peer.side == private_pty::Side::master ? private_pty::Side::slave : private_pty::Side::master;
        wake_waiters(ThreadWaitReason::pty_read, pty_wait_key(peer));
    }
    return result.value().bytes;
}

Result<SchedulerState::PreparedPtyRead> SchedulerState::prepare_pty_read(u16 id, usize count)
{
    auto *open = open_description(id);
    if (!open || !open->references || open->reservation || !open->readable || !open->pty_endpoint)
        return Status::bad_descriptor("description is not a readable retained raw PTY");
    return pty_transport_.prepare_read(open->pty_endpoint, count);
}
Result<usize> SchedulerState::commit_pty_read(PreparedPtyRead &preview)
{
    if (!preview.owned_by(pty_transport_))
        return Status::busy("PTY read preview belongs to a different or retired owner");
    auto peer = preview.endpoint();
    const auto result = preview.commit();
    if (!result) return result.status();
    if (result.value().bytes)
    {
        peer.side = peer.side == private_pty::Side::master ? private_pty::Side::slave : private_pty::Side::master;
        wake_waiters(ThreadWaitReason::pty_write, pty_wait_key(peer));
    }
    return result.value().bytes;
}
