// Included in scheduler.cpp inside ok::sched. Queue edges own canonical OFDs.
UnixReceive::UnixReceive(SchedulerState &owner, ProcessResources &resources, u16 description,
    u16 pipe, u64 revision, u64 token, bool peek, bool close_on_exec)
    : owner_(&owner), resources_(&resources), description_(description), pipe_(pipe),
      revision_(revision), token_(token), peek_(peek), close_on_exec_(close_on_exec)
{
}
UnixReceive::UnixReceive(UnixReceive &&other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), resources_(std::exchange(other.resources_, nullptr)),
      description_(other.description_), pipe_(other.pipe_), record_(other.record_),
      revision_(other.revision_), token_(other.token_), bytes_(other.bytes_), held_count_(other.held_count_),
      peek_(other.peek_), close_on_exec_(other.close_on_exec_), truncated_(other.truncated_),
      held_(other.held_), fds_(other.fds_)
{
    other.held_count_ = 0;
}
UnixReceive::~UnixReceive() { cancel(); }
Result<usize> UnixReceive::commit()
{
    return owner_ ? owner_->commit_unix_receive(*this)
                  : Result<usize>{Status::bad_descriptor("UNIX receive has already completed")};
}
void UnixReceive::cancel() { if (owner_) owner_->cancel_unix_receive(*this); }

bool SchedulerState::canonical_unix_description(u16 id) const
{
    const auto *open = open_description(id);
    return open && open->references && open->readable && open->writable &&
        open->pipe < pipes_.size() && open->unix_write_pipe < pipes_.size() &&
        open->pipe != open->unix_write_pipe && pipes_[open->pipe].used &&
        pipes_[open->unix_write_pipe].used && !open->node && !open->native_udp_handle &&
        !open->native_tcp_handle && !open->native_shared_file && !open->pty_endpoint && !open->console && !open->input_console &&
        (!open->unix_endpoint || (unix_endpoint(id) && unix_endpoint(id)->state == UnixEndpointState::connected));
}

usize SchedulerState::unix_read_count(u16 pipe, usize requested, u16 &record) const
{
    record = invalid_description;
    const auto &buffer = pipes_[pipe];
    usize count = requested < buffer.size ? requested : buffer.size;
    usize first = pipe_capacity;
    for (usize index = 0; index < buffer.rights.size(); ++index)
    {
        const auto &candidate = buffer.rights[index];
        if (candidate.used && candidate.start < count && candidate.start < first)
        {
            record = static_cast<u16>(index);
            first = candidate.start;
        }
    }
    if (record != invalid_description && buffer.rights[record].end < count)
        count = buffer.rights[record].end;
    return count;
}

Status SchedulerState::release_unix_record(UnixRightsRecord record)
{
    Status result{};
    // Remove every queue classification before invoking any provider callback.
    // The retained references themselves remain temporary external roots.
    for (usize index = 0; index < record.count; ++index)
    {
        auto *open = open_description(record.descriptions[index]);
        if (!open || !open->unix_queue_references)
        {
            if (result) result = Status::bad_descriptor("UNIX queue edge lost its owner");
            continue;
        }
        --open->unix_queue_references;
    }
    for (usize index = 0; index < record.count; ++index)
    {
        const auto released = release_description(record.descriptions[index]);
        if (result && !released) result = released;
    }
    return result;
}

Status SchedulerState::retire_pipe(u16 pipe)
{
    if (pipe >= pipes_.size() || !pipes_[pipe].used) return {};
    const auto records = pipes_[pipe].rights;
    pipes_[pipe] = {}; // A callback may immediately reuse this slot.
    Status result{};
    for (const auto &record : records) if (record.used)
    {
        const auto released = release_unix_record(record);
        if (result && !released) result = released;
    }
    return result;
}

void SchedulerState::consume_unix_bytes(u16 pipe, usize count, u16 record)
{
    if (!count) return;
    auto &buffer = pipes_[pipe];
    UnixRightsRecord detached{};
    if (record < buffer.rights.size())
    {
        detached = buffer.rights[record];
        buffer.rights[record] = {};
    }
    buffer.head = (buffer.head + count) % pipe_capacity;
    buffer.size -= count;
    ++buffer.revision;
    for (auto &remaining : buffer.rights) if (remaining.used)
    {
        remaining.start -= count;
        remaining.end -= count;
    }
    wake_waiters(ThreadWaitReason::pipe_write, pipe);
    if (detached.used)
    {
        const auto released = release_unix_record(detached);
        record_resource_failure(released, -1, invalid_description, 0, false);
    }
    // No buffer or description pointer is used after provider callbacks.
}

Result<usize> SchedulerState::send_unix_rights(ProcessResources &resources, i32 fd,
    std::span<const std::byte> payload, std::span<const i32> descriptors, bool nonblocking)
{
    static_cast<void>(nonblocking); // This immediate slice never arms an unresumable wait.
    const auto *slot = resources.files.entry(fd);
    if (!slot || !slot->used) return Status::bad_descriptor("UNIX send needs an open descriptor");
    if (!canonical_unix_description(slot->description))
        return Status::unsupported("rights transfer needs a canonical UNIX stream");
    if (descriptors.size() > max_unix_rights)
        return Status::message_size("UNIX rights bound exceeded");
    std::array<u16, max_unix_rights> ids{};
    for (usize index = 0; index < descriptors.size(); ++index)
    {
        const auto *passed = resources.files.entry(descriptors[index]);
        const auto *open = passed && passed->used ? open_description(passed->description) : nullptr;
        if (!open || !open->references)
            return Status::bad_descriptor("UNIX right has no live description");
        ids[index] = passed->description;
    }
    if (payload.empty()) return usize{0};
    const auto pipe = open_description(slot->description)->unix_write_pipe;
    if (!pipe_has_endpoint(pipe, false)) return Status::broken_pipe("UNIX peer has closed");
    auto &buffer = pipes_[pipe];
    if (buffer.revision == ~u64{0}) return Status::overflow("stream revision cannot wrap");
    const auto available = pipe_capacity - buffer.size;
    if (!available) return Status::would_block("UNIX stream is full");
    u16 record = invalid_description;
    if (!descriptors.empty())
    {
        for (usize index = 0; index < buffer.rights.size(); ++index)
            if (!buffer.rights[index].used) { record = static_cast<u16>(index); break; }
        if (record == invalid_description) return Status::would_block("UNIX rights records are full");
    }
    usize retained{};
    for (; retained < descriptors.size(); ++retained)
        if (auto status = retain_description(ids[retained]); !status)
        {
            while (retained) static_cast<void>(release_description(ids[--retained]));
            return status;
        }
    const auto count = payload.size() < available ? payload.size() : available;
    for (usize index = 0; index < count; ++index)
        buffer.data[(buffer.head + buffer.size + index) % pipe_capacity] = payload[index];
    if (record != invalid_description)
    {
        buffer.rights[record] = UnixRightsRecord{.used = true, .start = buffer.size,
            .end = buffer.size + count, .count = descriptors.size(), .descriptions = ids};
        for (usize index = 0; index < descriptors.size(); ++index)
            ++open_description(ids[index])->unix_queue_references;
    }
    buffer.size += count;
    ++buffer.revision;
    wake_waiters(ThreadWaitReason::pipe_read, pipe);
    return count;
}

Result<UnixReceive> SchedulerState::prepare_unix_receive(ProcessResources &resources, i32 fd,
    std::span<std::byte> preview, usize descriptor_capacity, bool peek, bool close_on_exec, bool nonblocking)
{
    static_cast<void>(nonblocking);
    const auto *slot = resources.files.entry(fd);
    if (!slot || !slot->used) return Status::bad_descriptor("UNIX receive needs an open descriptor");
    const auto description = slot->description;
    if (!canonical_unix_description(description))
        return Status::unsupported("rights receive needs a canonical UNIX stream");
    const auto pipe = open_description(description)->pipe;
    const auto &buffer = pipes_[pipe];
    if (!preview.empty() && !buffer.size && pipe_has_endpoint(pipe, true))
        return Status::would_block("UNIX stream has no buffered data");
    if (buffer.revision == ~u64{0} || next_unix_receive_token_ == ~u64{0} ||
        active_unix_receives_ == ~usize{0} || resources.unix_receive_operations == ~usize{0})
        return Status::overflow("UNIX prepared operation limit exceeded");
    if (auto status = retain_description(description); !status) return status;
    auto token = next_descriptor_token();
    if (!token)
    { record_resource_failure(release_description(description), -1, description, 0, false); return token.status(); }
    UnixReceive operation{*this, resources, description, pipe, buffer.revision,
        token.value(), peek, close_on_exec};
    ++active_unix_receives_;
    ++resources.unix_receive_operations;
    operation.bytes_ = unix_read_count(pipe, preview.size(), operation.record_);
    for (usize index = 0; index < operation.bytes_; ++index)
        preview[index] = buffer.data[(buffer.head + index) % pipe_capacity];
    if (operation.record_ != invalid_description)
    {
        const auto &record = buffer.rights[operation.record_];
        const auto limit = descriptor_capacity < max_unix_rights ? descriptor_capacity : max_unix_rights;
        for (i32 number = 0; number < static_cast<i32>(max_process_fds) &&
            operation.held_count_ < record.count && operation.held_count_ < limit; ++number)
        {
            auto *target = resources.files.entry(number);
            if (target->used || target->reservation) continue;
            const auto id = record.descriptions[operation.held_count_];
            if (auto status = retain_description(id); !status) return status;
            target->reservation = operation.token_;
            operation.fds_[operation.held_count_] = number;
            operation.held_[operation.held_count_++] = id;
        }
        operation.truncated_ = operation.held_count_ != record.count;
    }
    return operation;
}

void SchedulerState::cancel_unix_receive(UnixReceive &operation)
{
    const auto description = operation.description_;
    const auto held = operation.held_;
    const auto count = operation.held_count_;
    auto &resources = *operation.resources_;
    for (usize index = 0; index < count; ++index)
    {
        auto *slot = resources.files.entry(operation.fds_[index]);
        if (slot && slot->reservation == operation.token_) *slot = {};
    }
    --resources.unix_receive_operations;
    --active_unix_receives_;
    operation.owner_ = nullptr;
    operation.resources_ = nullptr;
    operation.held_count_ = 0;
    for (usize index = 0; index < count; ++index)
        record_resource_failure(release_description(held[index]), -1, held[index], 0, false);
    record_resource_failure(release_description(description), -1, description, 0, false);
}

Result<usize> SchedulerState::commit_unix_receive(UnixReceive &operation)
{
    if (!canonical_unix_description(operation.description_) || operation.pipe_ >= pipes_.size() ||
        !pipes_[operation.pipe_].used || pipes_[operation.pipe_].revision != operation.revision_)
    {
        cancel_unix_receive(operation);
        return Status::busy("UNIX receive preview became stale");
    }
    auto &resources = *operation.resources_;
    for (usize index = 0; index < operation.held_count_; ++index)
    {
        const auto *slot = resources.files.entry(operation.fds_[index]);
        if (!slot || slot->used || slot->reservation != operation.token_)
        {
            cancel_unix_receive(operation);
            return Status::busy("UNIX receive descriptor reservation became stale");
        }
    }
    const auto description = operation.description_;
    const auto pipe = operation.pipe_;
    const auto record = operation.record_;
    const auto count = operation.bytes_;
    const auto peek = operation.peek_;
    for (usize index = 0; index < operation.held_count_; ++index)
        *resources.files.entry(operation.fds_[index]) = FileDescriptor{.used = true,
            .close_on_exec = operation.close_on_exec_, .description = operation.held_[index], .installation = operation.token_};
    --resources.unix_receive_operations;
    --active_unix_receives_;
    operation.owner_ = nullptr;
    operation.resources_ = nullptr;
    // Held references become descriptor ownership without another fallible retain.
    if (!peek) consume_unix_bytes(pipe, count, record);
    record_resource_failure(release_description(description), -1, description, 0, false);
    return count;
}

Status SchedulerState::collect_unix_rights()
{
    if (collecting_unix_rights_) return {};
    collecting_unix_rights_ = true;
    Status result{};
    for (;;)
    {
        std::array<bool, max_open_descriptions> marked{};
        std::array<bool, max_process_pipes> reachable{};
        for (usize id = 0; id < descriptions_.size(); ++id)
            marked[id] = descriptions_[id].used &&
                descriptions_[id].references > descriptions_[id].unix_queue_references;
        bool changed{};
        do
        {
            changed = false;
            for (usize id = 0; id < descriptions_.size(); ++id)
            {
                if (marked[id])
                    if (const auto *endpoint = unix_endpoint(static_cast<u16>(id)); endpoint &&
                        endpoint->state == UnixEndpointState::listening)
                        for (usize index = 0; index < endpoint->pending_count; ++index)
                            if (!marked[endpoint->pending[index]])
                            {
                                marked[endpoint->pending[index]] = true;
                                changed = true;
                            }
                if (marked[id] && canonical_unix_description(static_cast<u16>(id)))
                {
                    const auto pipe = descriptions_[id].pipe;
                    reachable[pipe] = true;
                    for (const auto &record : pipes_[pipe].rights) if (record.used)
                        for (usize index = 0; index < record.count; ++index)
                            if (!marked[record.descriptions[index]])
                            {
                                marked[record.descriptions[index]] = true;
                                changed = true;
                            }
                }
            }
        } while (changed);
        // An unreachable listener owns its pending endpoints through queue edges,
        // just as an unreachable stream owns rights. Detach one before callbacks.
        u16 pending_child = invalid_description;
        for (usize id = 0; id < descriptions_.size(); ++id)
            if (!marked[id])
                if (auto *endpoint = unix_endpoint(static_cast<u16>(id)); endpoint &&
                    endpoint->state == UnixEndpointState::listening && endpoint->pending_count)
                {
                    pending_child = endpoint->pending[--endpoint->pending_count];
                    endpoint->pending[endpoint->pending_count] = invalid_description;
                    break;
                }
        if (pending_child != invalid_description)
        {
            const auto released = release_unix_pending(pending_child);
            if (result && !released) result = released;
            continue;
        }
        u16 selected_pipe = invalid_description, selected_record = invalid_description;
        for (usize pipe = 0; pipe < pipes_.size() && selected_pipe == invalid_description; ++pipe)
            if (pipes_[pipe].used && !reachable[pipe])
                for (usize record = 0; record < pipes_[pipe].rights.size(); ++record)
                    if (pipes_[pipe].rights[record].used)
                    {
                        selected_pipe = static_cast<u16>(pipe);
                        selected_record = static_cast<u16>(record);
                        break;
                    }
        if (selected_pipe == invalid_description) break;
        const auto detached = pipes_[selected_pipe].rights[selected_record];
        pipes_[selected_pipe].rights[selected_record] = {};
        if (pipes_[selected_pipe].revision != ~u64{0}) ++pipes_[selected_pipe].revision;
        const auto released = release_unix_record(detached);
        if (result && !released) result = released;
        // Callback-driven reuse requires a fresh graph, never a borrowed pointer.
    }
    collecting_unix_rights_ = false;
    return result;
}
