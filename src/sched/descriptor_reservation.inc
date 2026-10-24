DescriptorReservation::DescriptorReservation(DescriptorReservation &&other) noexcept
    : owner_{std::exchange(other.owner_, nullptr)}, pid_{other.pid_}, token_{other.token_},
      fd_{other.fd_}, description_{other.description_} {}
DescriptorReservation::~DescriptorReservation()
{
    if (owner_)
    {
        auto *owner = owner_;
        const auto status = cancel();
        owner->record_resource_failure(status, fd_, description_, 0, false);
    }
}
Status DescriptorReservation::validate() const
{ return owner_ ? owner_->validate_descriptor_reservation(*this) : Status::busy("descriptor reservation already retired"); }
Status DescriptorReservation::cancel()
{ return owner_ ? owner_->cancel_descriptor_reservation(*this) : Status{}; }
i32 DescriptorReservation::publish_checked(OpenFileDescription description, bool close_on_exec)
{ return owner_->publish_descriptor_reservation(*this, description, close_on_exec); }

Result<u64> SchedulerState::next_descriptor_token()
{
    if (next_unix_receive_token_ == ~u64{0})
        return Status::overflow("shared descriptor operation token exhausted");
    return next_unix_receive_token_++;
}
Result<DescriptorReservation> SchedulerState::prepare_descriptor(u64 pid)
{
    if (shutdown_admission_closed_) return Status::denied("scheduler shutdown admission is closed");
    auto *process = find(pid);
    if (!process || process->state() == ProcessState::zombie || process->state() == ProcessState::exited)
        return Status::denied("prepared descriptor requires a live process owner");
    auto &resources = process->resources();
    if (active_descriptor_reservations_ == ~usize{0} || resources.prepared_descriptor_operations == ~usize{0})
        return Status::overflow("prepared descriptor operation count exhausted");
    i32 fd = -1;
    for (i32 candidate = 3; candidate < static_cast<i32>(max_process_fds); ++candidate)
        if (!resources.files.contains(candidate)) { fd = candidate; break; }
    if (fd < 0) return Status::process_file_limit("prepared descriptor table is full");
    auto token = next_descriptor_token(); if (!token) return token.status();
    auto id = allocate_description({}); if (!id) return id.status();
    open_description(id.value())->reservation = token.value();
    resources.files.entry(fd)->reservation = token.value();
    ++resources.prepared_descriptor_operations; ++active_descriptor_reservations_;
    return DescriptorReservation{*this, pid, fd, id.value(), token.value()};
}
Status SchedulerState::validate_descriptor_reservation(const DescriptorReservation &operation) const
{
    if (operation.owner_ != this || !operation.token_ || !active_descriptor_reservations_)
        return Status::busy("foreign prepared descriptor owner");
    const auto *process = find(operation.pid_);
    if (!process || process->state() == ProcessState::zombie || process->state() == ProcessState::exited)
        return Status::busy("prepared descriptor process authority expired");
    const auto &resources = process->resources();
    const auto *slot = resources.files.entry(operation.fd_);
    const auto *open = open_description(operation.description_);
    if (!resources.prepared_descriptor_operations || !slot || slot->used || slot->reservation != operation.token_ ||
        !open || open->reservation != operation.token_ || open->references || open->node || open->native_tcp_handle ||
        open->native_udp_handle || open->native_shared_file || open->unix_endpoint || open->pty_endpoint || open->pipe != invalid_description || open->unix_write_pipe != invalid_description ||
        open->console || open->input_console || open->unix_queue_references)
        return Status::busy("prepared fd or canonical OFD reservation changed");
    return {};
}
Status SchedulerState::cancel_descriptor_reservation(DescriptorReservation &operation)
{
    if (auto status = validate_descriptor_reservation(operation); !status) return status;
    auto &resources = find(operation.pid_)->resources();
    *resources.files.entry(operation.fd_) = {};
    *open_description(operation.description_) = {};
    --resources.prepared_descriptor_operations; --active_descriptor_reservations_;
    operation.owner_ = nullptr;
    return {};
}
i32 SchedulerState::publish_descriptor_reservation(DescriptorReservation &operation,
    OpenFileDescription description, bool close_on_exec)
{
    auto &resources = find(operation.pid_)->resources();
    description.used = true; description.reservation = 0; description.references = 1;
    *open_description(operation.description_) = description;
    *resources.files.entry(operation.fd_) = FileDescriptor{.used = true, .close_on_exec = close_on_exec,
        .description = operation.description_, .installation = operation.token_};
    --resources.prepared_descriptor_operations; --active_descriptor_reservations_;
    operation.owner_ = nullptr;
    return operation.fd_;
}
