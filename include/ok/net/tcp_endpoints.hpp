#pragma once
#include <ok/net/tcp_transport.hpp>
#include <concepts>

namespace ok::net::tcp
{
struct TableIdentity { u64 value{}; };
enum class HandleKind : u8 { connection, listener };
struct Handle
{
    u64 owner{}, generation{}; u32 slot{}; HandleKind kind{};
    bool operator==(const Handle &) const = default;
};
struct TransmitTicket
{
    u64 owner{}, generation{}, serial{}, core_token{}; u32 slot{};
    bool operator==(const TransmitTicket &) const = default;
};
struct EndpointPacket { usize length{}; TransmitTicket ticket{}; };
struct AcceptTicket
{
    Handle listener{}, child{}; u64 operation{}, queue_order{};
    bool operator==(const AcceptTicket &) const = default;
};
struct AcceptPreview { AcceptTicket ticket{}; Endpoint local{}, peer{}; };
struct EndpointLimits
{
    u64 generation_max{~u64{0}}, readiness_max{~u64{0}}, transmit_max{~u64{0}};
    u32 reference_max{65535};
    u64 accept_operation_max{~u64{0}};
};
struct Readiness
{
    static constexpr u8 read_interest = 1, write_interest = 2, connect_interest = 4, accept_interest = 8;
    Handle handle{}; u64 sequence{}; usize readable_bytes{}, writable_bytes{}, accept_count{};
    bool eof{}, error{}, connect_complete{}, terminal{};
    [[nodiscard]] bool readable() const { return readable_bytes || eof || error || accept_count; }
    [[nodiscard]] bool writable() const { return writable_bytes != 0; }
    [[nodiscard]] bool may_wait(Handle expected, u64 observed, u8 interests = read_interest | accept_interest) const
    {
        return handle == expected && sequence == observed && !(interests & ~u8{15}) && !terminal && !error &&
            (!(interests & read_interest) || !(readable_bytes || eof)) &&
            (!(interests & write_interest) || !writable()) &&
            (!(interests & connect_interest) || !connect_complete) &&
            (!(interests & accept_interest) || !accept_count);
    }
};
struct EndpointState { Handle handle{}; Endpoint local{}, peer{}; Snapshot transport{}; u32 references{}; bool terminal{}; };
struct PoolState
{
    usize connections{}, listeners{}, unaccepted{}, retiring{};
    bool retired{}, prepared{}; u64 monotonic_ms{};
    usize accept_holds{}, accept_listener_holds{};
};
struct IngressResult { bool matched{}, allocated{}, dropped{}; };

template <typename Source>
concept IssSource = requires(Source &source, Endpoint local, Endpoint peer)
{ { source(local, peer) } -> std::same_as<Result<u32>>; };

// Internal canonical-resource owner, never a second fd namespace. Registry
// identity must not be reused across table lifetimes. One executor serializes
// commands; no borrowed packet/user data, ISS source or device pointer is retained.
template <usize Connections = 8, usize Listeners = 4, usize Backlog = 4,
          usize SendBytes = 1024, usize ReceiveBytes = 1024, usize SegmentBytes = 536>
class EndpointTable final
{
    static_assert(Connections > 0 && Connections <= 65535 && Listeners > 0 && Listeners <= 65535);
    static_assert(Backlog > 0 && Backlog <= Connections);
    using Core = Connection<SendBytes, ReceiveBytes, SegmentBytes>;
  public:
    static constexpr u64 retirement_ms = 240000;
    static constexpr usize packet_capacity = 20 + 24 + SegmentBytes;
    explicit EndpointTable(TableIdentity identity, EndpointLimits limits = {}) : identity_{identity.value}, limits_{limits}
    { if (!identity_ || !limits.generation_max || !limits.readiness_max || !limits.transmit_max ||
        !limits.reference_max || !limits.accept_operation_max) retired_ = true; }
    EndpointTable(const EndpointTable &) = delete;
    EndpointTable &operator=(const EndpointTable &) = delete;
    EndpointTable(EndpointTable &&) = delete;
    EndpointTable &operator=(EndpointTable &&) = delete;

    [[nodiscard]] PoolState pools() const
    {
        PoolState out{}; out.retired = retired_; out.prepared = pending_; out.monotonic_ms = now_;
        for (const auto &slot : connections_) if (slot.used)
        { ++out.connections; if (slot.parent) ++out.unaccepted; if (slot.closing) ++out.retiring;
          out.accept_holds += slot.accept_operation != 0; }
        for (const auto &slot : listeners_)
        { if (slot.used) ++out.listeners; out.accept_listener_holds += slot.accept_holds; }
        out.prepared = out.prepared || out.accept_holds;
        return out;
    }
    [[nodiscard]] Status status() const { return retired_ ? Status::io_error("TCP endpoint table is retired") : Status::success(); }
    [[nodiscard]] Result<bool> local_reserved(Endpoint local) const
    {
        if (auto s = command(); !s) return s;
        // Protocol ownership outlives application references, including
        // selected packets, unaccepted children and full TIME_WAIT.
        for (const auto &slot : connections_) if (slot.used && slot.core.local() == local) return true;
        for (const auto &slot : listeners_) if (slot.used && slot.local == local) return true;
        return false;
    }
    Result<Handle> listen(Endpoint local, usize backlog)
    {
        if (auto s = command(); !s) return s;
        if (!valid(local) || !backlog || backlog > Backlog) return Status::invalid_argument("invalid TCP listen endpoint/backlog");
        for (const auto &slot : listeners_) if (slot.used && slot.local == local) return Status::already_exists("TCP listener endpoint is reserved");
        for (usize i = 0; i < Listeners; ++i)
        {
            auto &slot = listeners_[i]; if (slot.used || slot.accept_holds || slot.exhausted) continue;
            if (slot.generation == limits_.generation_max) { slot.exhausted = true; continue; }
            ++slot.generation; slot.local = local; slot.backlog = backlog; slot.references = 1; slot.sequence = 1;
            slot.terminal = false; slot.used = true; return listener_handle(i);
        }
        return Status::no_memory("TCP listener pool is exhausted");
    }
    Result<Handle> connect(Endpoint local, Endpoint peer, u32 iss, u64 now)
    {
        if (auto s = command_time(now); !s) return s;
        if (!valid(local) || !valid(peer)) return Status::invalid_argument("invalid TCP connection endpoints");
        if (tuple(local, peer)) return Status::already_exists("TCP four-tuple is reserved");
        auto selected = vacant(); if (!selected) return selected.status(); const usize i = selected.value();
        auto &slot = connections_[i]; auto opened = slot.core.active_open(local, peer, iss, now);
        if (!opened) return opened;
        allocate(slot); now_ = now; return connection_handle(i);
    }
    Status retain(Handle handle)
    {
        if (auto s = command(); !s) return s;
        if (handle.kind == HandleKind::listener)
        {
            auto *slot = listener(handle); if (!slot) return stale();
            if (slot->references == limits_.reference_max) return Status::overflow("TCP listener reference limit");
            ++slot->references;
        }
        else
        {
            auto *slot = connection(handle); if (!slot) return stale();
            if (slot->references == limits_.reference_max) return Status::overflow("TCP endpoint reference limit");
            ++slot->references;
        }
        return Status::success();
    }
    Status release(Handle handle, u64 now)
    {
        if (callback_) return Status::busy("TCP ISS callback is active");
        if (now < now_) return Status::invalid_argument("TCP endpoint clock moved backward");
        if (handle.kind == HandleKind::listener)
        {
            auto *slot = listener(handle); if (!slot) return stale();
            if (--slot->references) { now_ = now; return Status::success(); }
            close_listener(handle.slot, now);
        }
        else
        {
            auto *slot = connection(handle); if (!slot) return stale();
            if (--slot->references) { now_ = now; return Status::success(); }
            slot->closing = true; slot->retire_deadline = deadline(now, retirement_ms);
            settle(handle.slot, now);
        }
        now_ = now; return Status::success();
    }
    Status abort(Handle handle, u64 now)
    {
        if (auto s = command_time(now); !s) return s;
        auto *slot = connection(handle); if (!slot) return stale();
        slot->terminal = true; slot->abort_pending = true; bump(*slot); settle(handle.slot, now); now_ = now;
        return Status::success();
    }
    Result<Handle> accept(Handle handle)
    {
        if (auto s = command(); !s) return s;
        auto *owner = listener(handle); if (!owner) return stale();
        if (!ensure_listener(handle.slot)) return Status::io_error("TCP listener readiness is terminal");
        usize chosen = Connections; u64 order = ~u64{0};
        for (usize i = 0; i < Connections; ++i)
        {
            auto &slot = connections_[i];
            if (slot.used && slot.parent && slot.parent_slot == handle.slot && slot.parent_generation == handle.generation &&
                slot.queue_order && slot.queue_order <= order && !slot.terminal && !slot.accept_operation &&
                (slot.core.snapshot().state == State::established || slot.core.snapshot().state == State::close_wait))
            { chosen = i; order = slot.queue_order; }
        }
        if (chosen == Connections) return Status::would_block("TCP accept queue is empty");
        auto &slot = connections_[chosen]; slot.parent = false; slot.queue_order = 0; slot.references = 1;
        bump(*owner); bump(slot); return connection_handle(chosen);
    }
    Result<AcceptPreview> prepare_accept(Handle handle)
    {
        if (auto s = command(); !s) return s;
        auto *owner = listener(handle); if (!owner) return stale();
        if (owner->terminal || owner->sequence >= limits_.readiness_max)
            return Status::io_error("TCP listener cannot reserve accept readiness");
        if (accept_operation_serial_ == limits_.accept_operation_max || owner->accept_holds == ~usize{0})
            return Status::overflow("TCP prepared accept operation limit");
        usize chosen = Connections; u64 order = ~u64{0};
        for (usize i = 0; i < Connections; ++i)
        {
            const auto &slot = connections_[i];
            if (acceptable(slot, handle) && !slot.accept_operation && slot.queue_order <= order)
            { chosen = i; order = slot.queue_order; }
        }
        if (chosen == Connections) return Status::would_block("TCP accept queue is empty");
        auto &child = connections_[chosen];
        // Both completion increments remain fallible checks, before any edge
        // changes. Preparation itself changes listener accept readiness.
        if (owner->sequence >= limits_.readiness_max - 1 || child.sequence >= limits_.readiness_max)
            return Status::overflow("TCP accept readiness increment is exhausted");
        const auto operation = ++accept_operation_serial_;
        child.accept_operation = operation; child.accept_queue_order = order;
        ++owner->accept_holds; ++owner->sequence;
        return AcceptPreview{{handle, connection_handle(chosen), operation, order}, child.core.local(), child.core.peer()};
    }
    [[nodiscard]] Status validate_accept(AcceptTicket ticket) const
    {
        if (auto s = command(); !s) return s;
        const auto *owner = listener(ticket.listener);
        const auto *child = accept_child(ticket);
        if (!owner || !child || !owner->accept_holds || !acceptable(*child, ticket.listener) ||
            owner->terminal || child->queue_order != ticket.queue_order)
            return Status::busy("TCP prepared accept authority is stale");
        if (owner->sequence >= limits_.readiness_max || child->sequence >= limits_.readiness_max)
            return Status::overflow("TCP accept commit readiness increment is exhausted");
        return {};
    }
    Result<Handle> commit_accept(AcceptTicket ticket)
    {
        if (auto s = validate_accept(ticket); !s) return s;
        auto &owner = listeners_[ticket.listener.slot];
        auto &child = connections_[ticket.child.slot];
        // No allocation, retain, callback or other fallible transition follows.
        child.parent = false; child.queue_order = 0; child.references = 1;
        child.accept_operation = child.accept_queue_order = 0; --owner.accept_holds;
        ++owner.sequence; ++child.sequence;
        return ticket.child;
    }
    Status cancel_accept(AcceptTicket ticket)
    {
        if (callback_) return Status::busy("TCP ISS callback is active");
        auto *child = accept_child(ticket);
        if (!child || ticket.listener.owner != identity_ || ticket.listener.kind != HandleKind::listener ||
            ticket.listener.slot >= Listeners) return Status::invalid_argument("foreign or stale TCP accept cancellation");
        auto &owner = listeners_[ticket.listener.slot];
        if (owner.generation != ticket.listener.generation || !owner.accept_holds ||
            child->parent_slot != ticket.listener.slot || child->parent_generation != ticket.listener.generation)
            return Status::invalid_argument("TCP accept cancellation owner differs");
        const bool queued = acceptable(*child, ticket.listener) && child->queue_order == ticket.queue_order;
        child->accept_operation = child->accept_queue_order = 0; --owner.accept_holds;
        if (queued && owner.used && !owner.terminal)
        {
            if (owner.sequence >= limits_.readiness_max) close_listener(ticket.listener.slot, now_);
            else ++owner.sequence;
        }
        settle(ticket.child.slot, now_);
        return {};
    }
    [[nodiscard]] Result<Readiness> readiness(Handle handle) const
    {
        Readiness out{}; out.handle = handle;
        if (handle.kind == HandleKind::listener)
        {
            const auto *slot = listener(handle); if (!slot) return stale();
            out.sequence = slot->sequence; out.terminal = out.error = slot->terminal || retired_;
            for (const auto &child : connections_) if (child.used && child.parent && child.parent_slot == handle.slot &&
                child.parent_generation == handle.generation && child.queue_order && !child.terminal &&
                !child.accept_operation) ++out.accept_count;
        }
        else
        {
            const auto *slot = connection(handle); if (!slot) return stale(); const auto s = slot->core.snapshot();
            out.sequence = slot->sequence; out.readable_bytes = s.receive_buffered; out.eof = s.eof;
            out.terminal = slot->terminal || retired_; out.error = out.terminal || s.failure != Failure::none;
            out.connect_complete = s.state != State::syn_sent && s.state != State::syn_received;
            if (!out.error && !s.write_shutdown && !s.prepared && (s.state == State::established || s.state == State::close_wait))
                out.writable_bytes = SendBytes - s.send_buffered;
        }
        return out;
    }
    [[nodiscard]] Result<EndpointState> state(Handle handle) const
    {
        const auto *slot = connection(handle); if (!slot) return stale();
        return EndpointState{handle, slot->core.local(), slot->core.peer(), slot->core.snapshot(), slot->references, slot->terminal || retired_};
    }
    Result<usize> write(Handle handle, Bytes bytes)
    {
        if (auto s = command(); !s) return s;
        if (aliases(bytes)) return Status::invalid_argument("TCP endpoint write aliases owner table");
        auto *slot = connection(handle); if (!slot) return stale();
        if (!ensure_connection(handle.slot)) return Status::io_error("TCP endpoint readiness is terminal");
        const auto before = slot->core.snapshot(); auto result = slot->core.write(bytes);
        if (result && changed(before, slot->core.snapshot())) bump(*slot);
        return result;
    }
    [[nodiscard]] Result<usize> peek(Handle handle, std::span<std::byte> bytes) const
    {
        if (callback_) return Status::busy("TCP ISS callback is active");
        if (retired_) return Status::io_error("TCP endpoint table is retired");
        if (aliases(Bytes{bytes.data(), bytes.size()})) return Status::invalid_argument("TCP endpoint peek aliases owner table");
        const auto *slot = connection(handle); if (!slot) return stale();
        if (slot->terminal) return Status::io_error("TCP endpoint readiness is terminal");
        return slot->core.peek(bytes);
    }
    Result<usize> read(Handle handle, std::span<std::byte> bytes)
    {
        if (auto s = command(); !s) return s;
        if (aliases(Bytes{bytes.data(), bytes.size()})) return Status::invalid_argument("TCP endpoint read aliases owner table");
        auto *slot = connection(handle); if (!slot) return stale();
        if (!ensure_connection(handle.slot)) return Status::io_error("TCP endpoint readiness is terminal");
        const auto before = slot->core.snapshot(); auto result = slot->core.read(bytes);
        if (result && changed(before, slot->core.snapshot())) bump(*slot);
        return result;
    }
    Status shutdown_write(Handle handle)
    {
        if (auto s = command(); !s) return s;
        auto *slot = connection(handle); if (!slot) return stale();
        if (!ensure_connection(handle.slot)) return Status::io_error("TCP endpoint readiness is terminal");
        const auto before = slot->core.snapshot(); auto result = slot->core.shutdown_write();
        if (result && changed(before, slot->core.snapshot())) bump(*slot);
        return result;
    }
    template <IssSource Source> Result<IngressResult> ingest(Bytes packet, u64 now, Source &source)
    {
        if (auto s = command_time(now); !s) return s;
        if (aliases(packet)) return Status::invalid_argument("TCP endpoint ingress aliases owner table");
        auto ip = wire::ipv4(packet); if (!ip) return ip.status();
        auto parsed = wire::tcp(ip.value()); if (!parsed) return parsed.status();
        const auto &fields = parsed.value().fields;
        const Endpoint local{ip.value().destination, fields.destination_port}, peer{ip.value().source, fields.source_port};
        if (!valid(local) || !valid(peer)) return IngressResult{false, false, true};
        if (auto *slot = tuple(local, peer))
        {
            const usize i = static_cast<usize>(slot - connections_.data());
            if (!ensure_connection(i)) return IngressResult{true, false, true};
            const auto before = slot->core.snapshot(); now_ = now; auto received = slot->core.receive(packet, now);
            if (!received) return received.status();
            if (changed(before, slot->core.snapshot())) bump(*slot);
            settle(i, now); return IngressResult{true, false, received.value() == Ingress::ignored};
        }
        usize owner = Listeners;
        for (usize i = 0; i < Listeners; ++i) if (listeners_[i].used && listeners_[i].references &&
            !listeners_[i].terminal && listeners_[i].local == local) { owner = i; break; }
        if (owner == Listeners || !(fields.flags & tcp_syn) || (fields.flags & (tcp_ack | tcp_rst | tcp_fin)))
            return IngressResult{false, false, true};
        auto &listener_slot = listeners_[owner];
        if (!ensure_listener(owner)) return IngressResult{true, false, true};
        if (children(owner) >= listener_slot.backlog) return IngressResult{true, false, true};
        auto available = vacant(); if (!available) return IngressResult{true, false, true};
        callback_ = true; auto iss = source(local, peer); callback_ = false;
        if (!iss) return iss.status();
        now_ = now; auto &slot = connections_[available.value()]; auto opened = slot.core.passive_open(local, iss.value(), now);
        if (!opened) return opened;
        auto received = slot.core.receive(packet, now);
        if (!received || slot.core.snapshot().state != State::syn_received)
        { static_cast<void>(slot.core.abort()); return received ? Result<IngressResult>{IngressResult{true, false, true}} : Result<IngressResult>{received.status()}; }
        allocate(slot); slot.references = 0; slot.parent = true; slot.parent_slot = owner; slot.parent_generation = listener_slot.generation;
        bump(listener_slot); now_ = now; return IngressResult{true, true, false};
    }
    Status tick(u64 now)
    {
        if (auto s = command_time(now); !s) return s;
        now_ = now;
        for (usize i = 0; i < Connections; ++i)
        {
            auto &slot = connections_[i]; if (!slot.used || selected(i)) continue;
            const auto before = slot.core.snapshot();
            if (slot.closing && now >= slot.retire_deadline && before.state != State::time_wait) slot.abort_pending = true;
            if (!slot.terminal) static_cast<void>(slot.core.tick(now));
            if (changed(before, slot.core.snapshot())) bump(slot);
            settle(i, now);
        }
        return Status::success();
    }
    Result<std::optional<EndpointPacket>> prepare(std::span<std::byte> output, u64 now)
    {
        if (auto s = command_time(now); !s) return s;
        if (pending_) return Status::busy("TCP endpoint ticket is outstanding");
        if (aliases(Bytes{output.data(), output.size()})) return Status::invalid_argument("TCP endpoint packet output aliases owner table");
        if (transmit_serial_ == limits_.transmit_max) { static_cast<void>(retire()); return Status::overflow("TCP endpoint transmit serial exhausted"); }
        auto timed = tick(now); if (!timed) return timed;
        for (usize checked = 0; checked < Connections; ++checked)
        {
            const usize i = (round_robin_ + checked) % Connections; auto &slot = connections_[i];
            if (!slot.used || slot.terminal || !ensure_connection(i)) continue;
            const auto before = slot.core.snapshot(); auto prepared = slot.core.prepare(output, now);
            if (!prepared) return prepared.status();
            if (!prepared.value()) continue;
            ticket_ = {identity_, slot.generation, ++transmit_serial_, prepared.value()->token, static_cast<u32>(i)};
            pending_ = true; round_robin_ = (i + 1) % Connections;
            if (changed(before, slot.core.snapshot())) bump(slot);
            return std::optional<EndpointPacket>{EndpointPacket{prepared.value()->length, ticket_}};
        }
        return std::optional<EndpointPacket>{};
    }
    Status complete(TransmitTicket ticket, bool accepted, u64 now)
    {
        if (callback_) return Status::busy("TCP ISS callback is active");
        if (!pending_ || ticket != ticket_) return Status::invalid_argument("stale TCP endpoint completion ticket");
        if (now < now_) return Status::invalid_argument("TCP endpoint clock moved backward");
        auto &slot = connections_[ticket.slot]; const auto before = slot.core.snapshot();
        auto result = slot.core.complete_transmit(ticket.core_token, accepted, now); if (!result) return result;
        pending_ = false; ticket_ = {}; now_ = now;
        if (changed(before, slot.core.snapshot())) bump(slot);
        settle(ticket.slot, now); return Status::success();
    }
    Status retire()
    {
        if (callback_) return Status::busy("TCP ISS callback is active");
        if (pending_)
        {
            auto &slot = connections_[ticket_.slot];
            auto result = slot.core.complete_transmit(ticket_.core_token, false, now_); if (!result) return result;
            pending_ = false; ticket_ = {};
        }
        retired_ = true;
        for (auto &owner : listeners_) { owner.used = false; owner.references = 0; }
        for (auto &slot : connections_) if (slot.used)
        {
            slot.terminal = true; slot.parent = false; slot.queue_order = 0;
            static_cast<void>(slot.core.abort());
            if (!slot.references) reclaim(slot);
        }
        return Status::success();
    }

  private:
    struct ConnectionSlot
    {
        Core core{}; u64 generation{}, sequence{}, parent_generation{}, queue_order{}, retire_deadline{};
        u64 accept_operation{}, accept_queue_order{};
        u32 references{}; usize parent_slot{};
        bool used{}, exhausted{}, terminal{}, parent{}, closing{}, abort_pending{};
    };
    struct ListenerSlot
    {
        Endpoint local{}; u64 generation{}, sequence{}; usize backlog{}, accept_holds{}; u32 references{};
        bool used{}, exhausted{}, terminal{};
    };
    static Status stale() { return Status::not_found("stale or unowned TCP endpoint handle"); }
    static bool valid(Endpoint ep) { return ep.port && ep.address[0] && ep.address[0] < 224; }
    static u64 deadline(u64 now, u64 delay) { return now > ~u64{0} - delay ? ~u64{0} : now + delay; }
    bool aliases(Bytes bytes) const
    {
        if (bytes.empty()) return false;
        const uptr owner = reinterpret_cast<uptr>(this), source = reinterpret_cast<uptr>(bytes.data());
        return source >= owner ? source - owner < sizeof(*this) : owner - source < bytes.size();
    }
    Status command() const
    { return callback_ ? Status::busy("TCP ISS callback is active") : status(); }
    Status command_time(u64 now) const
    { if (auto s = command(); !s) return s; return now < now_ ? Status::invalid_argument("TCP endpoint clock moved backward") : Status::success(); }
    Handle connection_handle(usize i) const { return {identity_, connections_[i].generation, static_cast<u32>(i), HandleKind::connection}; }
    Handle listener_handle(usize i) const { return {identity_, listeners_[i].generation, static_cast<u32>(i), HandleKind::listener}; }
    ConnectionSlot *connection(Handle h)
    {
        if (h.owner != identity_ || h.kind != HandleKind::connection || h.slot >= Connections) return nullptr;
        auto &slot = connections_[h.slot]; return slot.used && slot.references && slot.generation == h.generation ? &slot : nullptr;
    }
    const ConnectionSlot *connection(Handle h) const { return const_cast<EndpointTable *>(this)->connection(h); }
    ListenerSlot *listener(Handle h)
    {
        if (h.owner != identity_ || h.kind != HandleKind::listener || h.slot >= Listeners) return nullptr;
        auto &slot = listeners_[h.slot]; return slot.used && slot.references && slot.generation == h.generation ? &slot : nullptr;
    }
    const ListenerSlot *listener(Handle h) const { return const_cast<EndpointTable *>(this)->listener(h); }
    static bool acceptable(const ConnectionSlot &slot, Handle parent)
    {
        const auto state = slot.core.snapshot().state;
        return slot.used && slot.parent && slot.parent_slot == parent.slot &&
            slot.parent_generation == parent.generation && slot.queue_order && !slot.terminal &&
            (state == State::established || state == State::close_wait);
    }
    ConnectionSlot *accept_child(AcceptTicket ticket)
    {
        if (!ticket.operation || ticket.child.owner != identity_ || ticket.child.kind != HandleKind::connection ||
            ticket.child.slot >= Connections) return nullptr;
        auto &slot = connections_[ticket.child.slot];
        return slot.used && slot.generation == ticket.child.generation && slot.accept_operation == ticket.operation &&
            slot.accept_queue_order == ticket.queue_order ? &slot : nullptr;
    }
    const ConnectionSlot *accept_child(AcceptTicket ticket) const
    { return const_cast<EndpointTable *>(this)->accept_child(ticket); }
    ConnectionSlot *tuple(Endpoint local, Endpoint peer)
    { for (auto &slot : connections_) if (slot.used && slot.core.local() == local && slot.core.peer() == peer) return &slot; return nullptr; }
    Result<usize> vacant()
    {
        for (usize i = 0; i < Connections; ++i)
        {
            auto &slot = connections_[i]; if (slot.used || slot.exhausted) continue;
            if (slot.generation == limits_.generation_max) { slot.exhausted = true; continue; }
            return i;
        }
        return Status::no_memory("TCP connection pool is exhausted");
    }
    void allocate(ConnectionSlot &slot)
    {
        ++slot.generation; slot.sequence = 1; slot.references = 1; slot.used = true;
        slot.terminal = slot.parent = slot.closing = slot.abort_pending = false;
        slot.parent_generation = slot.queue_order = slot.retire_deadline = slot.accept_operation = slot.accept_queue_order = 0;
        slot.parent_slot = 0;
    }
    bool selected(usize i) const { return pending_ && ticket_.slot == i; }
    usize children(usize owner) const
    {
        usize count{};
        for (const auto &slot : connections_)
            if (slot.used && slot.parent && slot.parent_slot == owner &&
                slot.parent_generation == listeners_[owner].generation) ++count;
        return count;
    }
    void bump(ConnectionSlot &slot)
    {
        if (slot.terminal) return;
        if (slot.sequence == limits_.readiness_max) { slot.terminal = true; slot.abort_pending = true; }
        else ++slot.sequence;
    }
    void bump(ListenerSlot &slot)
    { if (!slot.terminal) { if (slot.sequence == limits_.readiness_max) slot.terminal = true; else ++slot.sequence; } }
    bool ensure_connection(usize i)
    {
        auto &slot = connections_[i];
        if (slot.terminal || slot.sequence == limits_.readiness_max)
        { slot.terminal = true; slot.abort_pending = true; settle(i, now_); return false; }
        return true;
    }
    bool ensure_listener(usize i)
    {
        auto &slot = listeners_[i]; if (slot.terminal || slot.sequence == limits_.readiness_max)
        { close_listener(i, now_); return false; } return true;
    }
    static bool changed(const Snapshot &a, const Snapshot &b)
    {
        return a.state != b.state || a.failure != b.failure || a.send_buffered != b.send_buffered ||
            a.receive_buffered != b.receive_buffered || a.eof != b.eof || a.write_shutdown != b.write_shutdown ||
            a.prepared != b.prepared || a.peer_window != b.peer_window || a.receive_window != b.receive_window ||
            a.send_unacknowledged != b.send_unacknowledged || a.receive_next != b.receive_next;
    }
    void reclaim(ConnectionSlot &slot)
    {
        // Protocol retirement can consume the queue edge while a copied accept
        // still owns its exact generation. Only cancellation retires that hold.
        if (slot.accept_operation)
        {
            static_cast<void>(slot.core.abort()); slot.references = 0; slot.parent = false;
            slot.queue_order = 0; slot.closing = true; slot.terminal = true; slot.abort_pending = false;
            return;
        }
        static_cast<void>(slot.core.abort()); slot.used = false; slot.references = 0; slot.parent = false;
        slot.queue_order = 0; slot.closing = false; slot.abort_pending = false;
    }
    void close_listener(usize owner, u64 now)
    {
        auto &listener_slot = listeners_[owner]; listener_slot.used = false; listener_slot.references = 0;
        for (usize i = 0; i < Connections; ++i)
        {
            auto &child = connections_[i]; if (!child.used || !child.parent || child.parent_slot != owner ||
                child.parent_generation != listener_slot.generation) continue;
            child.parent = false; child.queue_order = 0; child.closing = true; child.abort_pending = true;
            child.retire_deadline = deadline(now, retirement_ms); settle(i, now);
        }
    }
    void settle(usize i, u64 now)
    {
        auto &slot = connections_[i]; if (!slot.used || selected(i)) return;
        auto snapshot = slot.core.snapshot();
        if (slot.parent)
        {
            auto &owner = listeners_[slot.parent_slot];
            if (!owner.used || owner.generation != slot.parent_generation || snapshot.state == State::closed ||
                snapshot.state == State::listen || slot.terminal)
            { bump(owner); reclaim(slot); return; }
            if (!slot.queue_order && (snapshot.state == State::established || snapshot.state == State::close_wait))
            {
                if (accept_serial_ == ~u64{0}) { bump(owner); reclaim(slot); return; }
                slot.queue_order = ++accept_serial_; bump(owner);
            }
        }
        if (slot.abort_pending)
        { static_cast<void>(slot.core.abort()); slot.abort_pending = false; snapshot = slot.core.snapshot(); }
        if (!slot.closing) return;
        if ((now >= slot.retire_deadline && snapshot.state != State::time_wait) || snapshot.state == State::closed || snapshot.state == State::listen ||
            snapshot.state == State::syn_sent || snapshot.state == State::syn_received)
        { reclaim(slot); return; }
        if (!snapshot.write_shutdown && (snapshot.state == State::established || snapshot.state == State::close_wait))
            static_cast<void>(slot.core.shutdown_write());
        // Discard application-unowned receive bytes to avoid flow-control
        // deadlock while the peer completes its orderly close.
        if (slot.core.snapshot().receive_buffered) static_cast<void>(slot.core.read(discarded_));
    }
    const u64 identity_{}; const EndpointLimits limits_{};
    std::array<ConnectionSlot, Connections> connections_{};
    std::array<ListenerSlot, Listeners> listeners_{};
    // Executor-owned scratch scales with configured storage, never call stack.
    std::array<std::byte, ReceiveBytes> discarded_{};
    u64 now_{}, transmit_serial_{}, accept_serial_{}, accept_operation_serial_{}; usize round_robin_{};
    TransmitTicket ticket_{}; bool pending_{}, retired_{}, callback_{};
};
}
