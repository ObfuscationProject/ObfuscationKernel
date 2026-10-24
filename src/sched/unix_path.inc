// Included in scheduler.cpp inside ok::sched. All operations are serialized.
UnixAccept::UnixAccept(UnixAccept &&other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), resources_(std::exchange(other.resources_, nullptr)),
      listener_(other.listener_), child_(other.child_), endpoint_(other.endpoint_), token_(other.token_),
      fd_(other.fd_), close_on_exec_(other.close_on_exec_), status_flags_(other.status_flags_), peer_(other.peer_) {}
UnixAccept::~UnixAccept() { cancel(); }
Result<i32> UnixAccept::commit()
{
    return owner_ ? owner_->commit_unix_accept(*this)
                  : Result<i32>{Status::bad_descriptor("UNIX accept has already retired")};
}
void UnixAccept::cancel() { if (owner_) owner_->cancel_unix_accept(*this); }

const SchedulerState::UnixEndpoint *SchedulerState::unix_endpoint(u16 description) const
{
    const auto *open = open_description(description);
    if (!open || !open->unix_endpoint) return nullptr;
    const auto handle = open->unix_endpoint;
    const auto &endpoint = unix_endpoints_[handle.slot];
    return endpoint.used && endpoint.generation == handle.generation && endpoint.description == description
        ? &endpoint : nullptr;
}
SchedulerState::UnixEndpoint *SchedulerState::unix_endpoint(u16 description)
{ return const_cast<UnixEndpoint *>(static_cast<const SchedulerState *>(this)->unix_endpoint(description)); }
bool SchedulerState::is_unix_socket(u16 description) const
{
    const auto *open = open_description(description);
    if (!open || !open->references || !open->readable || !open->writable || open->node ||
        open->native_udp_handle || open->native_tcp_handle || open->native_shared_file ||
        open->console || open->input_console) return false;
    const auto *endpoint = unix_endpoint(description);
    if (!endpoint) return !open->unix_endpoint && canonical_unix_description(description);
    return endpoint->state == UnixEndpointState::connected ? canonical_unix_description(description)
        : open->pipe == invalid_description && open->unix_write_pipe == invalid_description;
}
Result<UnixEndpointHandle> SchedulerState::reserve_unix_endpoint()
{
    if (next_unix_endpoint_generation_ == ~u64{0})
        return Status::overflow("UNIX endpoint generation cannot wrap");
    for (usize index = 0; index < unix_endpoints_.size(); ++index)
        if (!unix_endpoints_[index].used)
        {
            auto &endpoint = unix_endpoints_[index];
            endpoint.used = true;
            endpoint.generation = next_unix_endpoint_generation_++;
            endpoint.description = invalid_description;
            endpoint.state = UnixEndpointState::unbound;
            endpoint.listen_credentials = {}; endpoint.peer_credentials = {};
            endpoint.backlog = 0; endpoint.pending_count = 0; endpoint.accept_token = 0;
            endpoint.pending.fill(invalid_description);
            return UnixEndpointHandle{static_cast<u16>(index), endpoint.generation};
        }
    return Status::system_file_limit("pathname UNIX endpoint table is full");
}
usize SchedulerState::unix_endpoint_count() const
{
    usize count{}; for (const auto &endpoint : unix_endpoints_) count += endpoint.used;
    return count;
}
Status SchedulerState::shutdown_endpoint_status() const
{
    if (!resource_drain_report_.first_failure) return resource_drain_report_.first_failure;
    if (active_unix_receives_ || active_descriptor_reservations_ || resource_drain_report_.active)
        return Status::busy("shutdown retains a prepared descriptor or endpoint operation");
    return unix_endpoint_count() ? Status::busy("shutdown retains a pathname UNIX endpoint") : Status{};
}
Result<i32> SchedulerState::create_unix_socket(ProcessResources &resources, bool close_on_exec, u32 status_flags)
{
    if (status_flags & ~0x0800u) return Status::invalid_argument("unsupported UNIX status flags");
    i32 fd = -1;
    for (i32 candidate = 0; candidate < static_cast<i32>(max_process_fds); ++candidate)
        if (!resources.files.contains(candidate)) { fd = candidate; break; }
    if (fd < 0) return Status::process_file_limit("UNIX socket descriptor table is full");
    auto endpoint = reserve_unix_endpoint(); if (!endpoint) return endpoint.status();
    auto description = allocate_description({.readable = true, .writable = true,
        .flags = 2u | status_flags, .unix_endpoint = endpoint.value()});
    if (!description)
    {
        static_cast<void>(release_unix_endpoint(endpoint.value()));
        return description.status();
    }
    const auto installed = install_descriptor(resources, fd, description.value(), close_on_exec);
    if (!installed)
    {
        *open_description(description.value()) = {};
        static_cast<void>(release_unix_endpoint(endpoint.value()));
        return installed;
    }
    return fd;
}
Status SchedulerState::validate_unix_bind(u16 description) const
{
    const auto *endpoint = unix_endpoint(description);
    if (!is_unix_socket(description) || !endpoint)
        return Status::bad_descriptor("bind requires a pathname UNIX socket");
    return endpoint->state == UnixEndpointState::unbound && !endpoint->binding
        ? Status{} : Status::invalid_argument("UNIX socket is already bound or connected");
}
Status SchedulerState::bind_unix_socket(u16 description, fs::SocketPathReference &&binding)
{
    if (auto status = validate_unix_bind(description); !status) return status;
    if (!binding.valid()) return Status::invalid_argument("UNIX bind has no retained socket identity");
    for (const auto &existing : unix_endpoints_)
        if (existing.used && existing.binding && existing.binding->same_identity(binding))
            return Status::already_exists("socket node already has a live UNIX endpoint");
    auto *endpoint = unix_endpoint(description);
    endpoint->binding.emplace(std::move(binding));
    endpoint->state = UnixEndpointState::bound;
    return {};
}
Status SchedulerState::listen_unix_socket(u16 description, usize backlog, UnixPeerCredentials credentials)
{
    auto *endpoint = unix_endpoint(description);
    if (!is_unix_socket(description) || !endpoint)
        return Status::bad_descriptor("listen requires a pathname UNIX socket");
    if (endpoint->state != UnixEndpointState::bound && endpoint->state != UnixEndpointState::listening)
        return Status::invalid_argument("UNIX listen requires a bound socket");
    if (!credentials.pid || credentials.pid > static_cast<u64>(__INT32_MAX__))
        return Status::overflow("UNIX peer PID exceeds the signed credential record");
    if (endpoint->state == UnixEndpointState::bound) endpoint->listen_credentials = credentials;
    endpoint->backlog = !backlog ? 1 : backlog < max_unix_backlog ? backlog : max_unix_backlog;
    endpoint->state = UnixEndpointState::listening;
    return {};
}
Status SchedulerState::connect_unix_socket(u16 description, const fs::SocketPathReference &target,
    UnixPeerCredentials credentials)
{
    auto *client = unix_endpoint(description);
    if (!is_unix_socket(description) || !client)
        return Status::bad_descriptor("connect requires a pathname UNIX socket");
    if (client->state == UnixEndpointState::connected) return Status::busy("UNIX socket is already connected");
    if (client->state == UnixEndpointState::listening) return Status::invalid_argument("UNIX listener cannot connect");
    if (!credentials.pid || credentials.pid > static_cast<u64>(__INT32_MAX__))
        return Status::overflow("UNIX peer PID exceeds the signed credential record");
    if (!target.valid()) return Status::not_found("UNIX target identity is no longer retained");
    UnixEndpoint *listener = nullptr;
    for (auto &endpoint : unix_endpoints_)
        if (endpoint.used && endpoint.state == UnixEndpointState::listening && endpoint.binding &&
            endpoint.binding->same_identity(target) && is_unix_socket(endpoint.description))
        { listener = &endpoint; break; }
    if (!listener) return Status::not_found("socket node has no live UNIX listener");
    if (listener->pending_count >= listener->backlog || listener->pending_count == max_unix_backlog)
        return Status::would_block("UNIX listener backlog is full");
    std::array<u16, 2> buffers{invalid_description, invalid_description};
    usize count{};
    for (usize index = 0; index < pipes_.size() && count != buffers.size(); ++index)
        if (!pipes_[index].used) buffers[count++] = static_cast<u16>(index);
    if (count != buffers.size()) return Status::system_file_limit("UNIX connection needs two stream buffers");
    auto server = reserve_unix_endpoint(); if (!server) return server.status();
    auto child = allocate_description({.readable = true, .writable = true, .flags = 2u,
        .pipe = buffers[1], .unix_write_pipe = buffers[0], .unix_endpoint = server.value()});
    if (!child)
    {
        static_cast<void>(release_unix_endpoint(server.value()));
        return child.status();
    }
    // All fallible admission has completed. Queue ownership is published with
    // both transport endpoints, so data may precede accept without false EOF.
    pipes_[buffers[0]] = PipeBuffer{.used = true};
    pipes_[buffers[1]] = PipeBuffer{.used = true};
    auto *client_open = open_description(description);
    client_open->pipe = buffers[0]; client_open->unix_write_pipe = buffers[1];
    client->state = UnixEndpointState::connected;
    client->peer_credentials = listener->listen_credentials;
    auto *server_endpoint = unix_endpoint(child.value());
    server_endpoint->state = UnixEndpointState::connected;
    server_endpoint->peer_credentials = credentials;
    auto *server_open = open_description(child.value());
    server_open->references = 1; server_open->unix_queue_references = 1;
    listener->pending[listener->pending_count++] = child.value();
    return {};
}
Result<UnixPeerCredentials> SchedulerState::unix_peer_credentials(u16 description) const
{
    if (!is_unix_socket(description)) return Status::bad_descriptor("peer credentials require a UNIX socket");
    const auto *endpoint = unix_endpoint(description);
    if (!endpoint) return Status::unsupported("legacy socketpair has no credential snapshot");
    if (endpoint->state != UnixEndpointState::connected)
        return Status::not_found("UNIX socket has no connected peer");
    return endpoint->peer_credentials;
}
Result<UnixSocketReadiness> SchedulerState::unix_socket_readiness(u16 description) const
{
    if (!is_unix_socket(description)) return Status::bad_descriptor("readiness requires a UNIX socket");
    const auto *endpoint = unix_endpoint(description);
    if (endpoint && endpoint->state == UnixEndpointState::listening)
        return UnixSocketReadiness{.listening = true, .readable = endpoint->pending_count != 0};
    if (endpoint && endpoint->state != UnixEndpointState::connected) return UnixSocketReadiness{};
    const auto *open = open_description(description);
    auto incoming = pipe_readiness(open->pipe); if (!incoming) return incoming.status();
    auto outgoing = pipe_readiness(open->unix_write_pipe); if (!outgoing) return outgoing.status();
    return UnixSocketReadiness{.readable = incoming.value().buffered || !incoming.value().writers,
        .writable = outgoing.value().available && outgoing.value().readers, .hangup = !incoming.value().writers};
}
Result<UnixAccept> SchedulerState::prepare_unix_accept(ProcessResources &resources, u16 description,
    bool close_on_exec, u32 status_flags)
{
    if (status_flags & ~0x0800u) return Status::invalid_argument("unsupported UNIX accept status flags");
    auto *endpoint = unix_endpoint(description);
    if (!is_unix_socket(description) || !endpoint || endpoint->state != UnixEndpointState::listening)
        return Status::invalid_argument("UNIX accept requires a live listener");
    if (!endpoint->pending_count) return Status::would_block("UNIX listener queue is empty");
    if (endpoint->accept_token) return Status::busy("UNIX listener child already has a prepared accept");
    if (active_unix_receives_ == ~usize{0} || resources.unix_receive_operations == ~usize{0})
        return Status::overflow("UNIX accept operation count exhausted");
    i32 fd = -1;
    for (i32 candidate = 0; candidate < static_cast<i32>(max_process_fds); ++candidate)
        if (!resources.files.contains(candidate)) { fd = candidate; break; }
    if (fd < 0) return Status::process_file_limit("UNIX accept descriptor table is full");
    const auto child = endpoint->pending[0];
    if (!canonical_unix_description(child)) return Status::io_error("UNIX listener child lost its canonical owner");
    auto token = next_descriptor_token(); if (!token) return token.status();
    if (auto status = retain_description(description); !status) return status;
    if (auto status = retain_description(child); !status)
    {
        record_resource_failure(release_description(description), -1, description, 0, false);
        return status;
    }
    endpoint->accept_token = token.value();
    resources.files.entry(fd)->reservation = token.value();
    ++resources.unix_receive_operations; ++active_unix_receives_;
    return UnixAccept{*this, resources, description, child, open_description(description)->unix_endpoint,
        token.value(), fd, close_on_exec, status_flags, unix_endpoint(child)->peer_credentials};
}
void SchedulerState::cancel_unix_accept(UnixAccept &operation)
{
    auto &resources = *operation.resources_;
    const auto listener = operation.listener_, child = operation.child_;
    auto *slot = resources.files.entry(operation.fd_);
    if (slot && !slot->used && slot->reservation == operation.token_) *slot = {};
    if (auto *endpoint = unix_endpoint(listener); endpoint &&
        open_description(listener)->unix_endpoint == operation.endpoint_ && endpoint->accept_token == operation.token_)
        endpoint->accept_token = 0;
    --resources.unix_receive_operations; --active_unix_receives_;
    operation.owner_ = nullptr; operation.resources_ = nullptr;
    record_resource_failure(release_description(child), -1, child, 0, false);
    record_resource_failure(release_description(listener), -1, listener, 0, false);
}
Result<i32> SchedulerState::commit_unix_accept(UnixAccept &operation)
{
    auto *endpoint = unix_endpoint(operation.listener_);
    auto *slot = operation.resources_->files.entry(operation.fd_);
    if (!endpoint || open_description(operation.listener_)->unix_endpoint != operation.endpoint_ ||
        endpoint->state != UnixEndpointState::listening || endpoint->accept_token != operation.token_ ||
        !endpoint->pending_count || endpoint->pending[0] != operation.child_ || !slot || slot->used ||
        slot->reservation != operation.token_ || !canonical_unix_description(operation.child_))
    {
        cancel_unix_accept(operation);
        return Status::busy("UNIX accepted child or descriptor reservation became stale");
    }
    const auto listener = operation.listener_, child = operation.child_;
    const auto fd = operation.fd_;
    auto *open = open_description(child);
    if (!open->unix_queue_references)
    {
        cancel_unix_accept(operation);
        return Status::io_error("UNIX accept queue lost its ownership classification");
    }
    open->flags = 2u | operation.status_flags_;
    *slot = FileDescriptor{.used = true, .close_on_exec = operation.close_on_exec_,
        .description = child, .installation = operation.token_};
    for (usize index = 1; index < endpoint->pending_count; ++index)
        endpoint->pending[index - 1] = endpoint->pending[index];
    endpoint->pending[--endpoint->pending_count] = invalid_description;
    endpoint->accept_token = 0;
    --open->unix_queue_references;
    --operation.resources_->unix_receive_operations; --active_unix_receives_;
    operation.owner_ = nullptr; operation.resources_ = nullptr;
    // The preview's child hold is now the fd hold. Consume the detached queue
    // hold and listener preview only after publication and memory-only changes.
    record_resource_failure(release_description(child), -1, child, 0, false);
    record_resource_failure(release_description(listener), -1, listener, 0, false);
    return fd;
}
Status SchedulerState::release_unix_pending(u16 child)
{
    auto *open = open_description(child);
    if (!open || !open->unix_queue_references)
        return Status::bad_descriptor("UNIX pending queue lost its child owner");
    --open->unix_queue_references;
    return release_description(child);
}
Status SchedulerState::release_unix_endpoint(UnixEndpointHandle handle)
{
    if (!handle) return Status::bad_descriptor("invalid UNIX endpoint retirement");
    auto &endpoint = unix_endpoints_[handle.slot];
    if (!endpoint.used || endpoint.generation != handle.generation)
        return Status::bad_descriptor("stale UNIX endpoint retirement");
    const auto children = endpoint.pending;
    const auto count = endpoint.pending_count;
    std::optional<fs::SocketPathReference> binding;
    if (endpoint.binding) binding.emplace(std::move(*endpoint.binding));
    endpoint.binding.reset();
    endpoint.used = false; endpoint.generation = 0; endpoint.description = invalid_description;
    endpoint.pending_count = 0; endpoint.pending.fill(invalid_description); endpoint.accept_token = 0;
    endpoint.state = UnixEndpointState::unbound; endpoint.backlog = 0;
    endpoint.listen_credentials = {}; endpoint.peer_credentials = {};
    Status result{};
    // Remove all queue classifications before any release callback. These
    // detached holds are temporary external collection roots until consumed.
    for (usize index = 0; index < count; ++index)
    {
        auto *open = open_description(children[index]);
        if (!open || !open->unix_queue_references)
        { if (result) result = Status::bad_descriptor("UNIX endpoint queue lost child ownership"); }
        else --open->unix_queue_references;
    }
    for (usize index = 0; index < count; ++index)
    {
        const auto released = release_description(children[index]);
        if (result && !released) result = released;
    }
    if (binding)
    {
        const auto released = binding->release();
        if (result && !released) result = released;
    }
    return result;
}
