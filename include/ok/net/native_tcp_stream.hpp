#pragma once
#include <ok/net/tcp_stream_handle.hpp>
#include <ok/net/tcp_endpoints.hpp>

namespace ok::user::native { class TcpOriginalDescription; }

namespace ok::net
{
struct TcpStreamCapacity { u64 generation_max{~u64{0}}; usize owners{16}; u64 operation_max{~u64{0}}; };
struct TcpAcceptReservation
{
    TcpStreamHandle listener{}, stream{};
    u64 operation{};
    tcp::AcceptPreview endpoint{};
};
struct TcpStreamSnapshot
{
    TcpStreamHandle handle{}; tcp::Endpoint local{}; std::optional<tcp::Handle> endpoint{};
};
// The scheduler emits actual close/exec/exit events. A callback retains no
// caller buffer and must consume its wait edge before releasing its OFD pin.
struct TcpWaitObserver
{
    void *context{};
    void (*closed)(void *, const void *, i32, u16){};
    void (*process_cancelled)(void *, u64){};
};

class NativeTcpStreamService final
{
  public:
    using Table = tcp::EndpointTable<>;
    static constexpr usize stream_capacity = 16;
    explicit NativeTcpStreamService(tcp::TableIdentity identity, TcpStreamCapacity capacity = {},
                                    tcp::EndpointLimits limits = {})
        : table_{identity, limits}, identity_{identity.value}, capacity_{capacity} {}
    NativeTcpStreamService(const NativeTcpStreamService &) = delete;
    NativeTcpStreamService &operator=(const NativeTcpStreamService &) = delete;
    NativeTcpStreamService(NativeTcpStreamService &&) = delete;
    NativeTcpStreamService &operator=(NativeTcpStreamService &&) = delete;
    [[nodiscard]] Table &endpoints() { return table_; }
    [[nodiscard]] const Table &endpoints() const { return table_; }
    [[nodiscard]] u64 now() const { return table_.pools().monotonic_ms; }
    [[nodiscard]] usize active_streams() const
    { usize count{}; for (const auto &entry : streams_) count += entry.used || entry.closing || entry.reservation; return count; }
    [[nodiscard]] usize prepared_accepts() const
    { usize count{}; for (const auto &entry : streams_) count += entry.reservation != 0; return count; }
    [[nodiscard]] usize pending_closes() const
    { usize count{}; for (const auto &entry : streams_) count += entry.closing; return count; }
    [[nodiscard]] bool valid(TcpStreamHandle handle) const { return find(handle) != nullptr; }
    [[nodiscard]] Result<TcpStreamSnapshot> snapshot(TcpStreamHandle handle) const
    {
        const auto *entry = find(handle); if (!entry) return stale();
        return TcpStreamSnapshot{handle, entry->local, entry->endpoint};
    }
    Status set_wait_observer(TcpWaitObserver observer)
    {
        if (observer_.context && observer_.context != observer.context)
            return Status::busy("TCP wait observer is already bound");
        if (!observer.context || !observer.closed || !observer.process_cancelled)
            return Status::invalid_argument("TCP wait observer is incomplete");
        observer_ = observer; return {};
    }
    Status clear_wait_observer(void *context)
    {
        if (observer_.context != context) return Status::invalid_argument("foreign TCP wait observer");
        observer_ = {}; return {};
    }
    void descriptor_closed(const void *resources, i32 fd, u16 description)
    {
        if (original_context_) original_closed_(original_context_, resources, fd, description);
        if (observer_.context) observer_.closed(observer_.context, resources, fd, description);
    }
    void cancel_process(u64 pid)
    { if (observer_.context) observer_.process_cancelled(observer_.context, pid); }
    Result<TcpStreamHandle> open()
    {
        if (auto status = table_.status(); !status) return status;
        auto selected = vacant(); if (!selected) return selected.status();
        return allocate(selected.value(), {}, {});
    }
    Status close(TcpStreamHandle handle)
    {
        auto *entry = find(handle); if (!entry) return stale();
        // Revoke the external edge once. ISS callback reentry can defer the
        // endpoint release into this private record, which cannot be reused.
        entry->used = false; entry->closing = true;
        return drain(*entry);
    }
    Status drain_closes()
    {
        Status first{};
        for (auto &entry : streams_) if (entry.closing)
        { auto status = drain(entry); if (!status && first) first = status; }
        return first;
    }
    Status bind(TcpStreamHandle handle, tcp::Endpoint local)
    {
        auto *entry = find(handle); if (!entry) return stale();
        if (entry->endpoint || entry->local.port) return Status::invalid_argument("TCP stream is already bound or started");
        if (!local.port || (!local.address[0] || local.address[0] >= 224)) return Status::invalid_argument("TCP bind requires a specified unicast endpoint");
        auto reserved = table_.local_reserved(local); if (!reserved) return reserved.status();
        if (reserved.value()) return Status::already_exists("TCP local endpoint is reserved by protocol ownership");
        for (const auto &other : streams_)
            if ((other.used || other.closing) && other.local == local)
                return Status::already_exists("TCP local endpoint is reserved by a stream owner");
        entry->local = local; return {};
    }
    Status listen(TcpStreamHandle handle, usize backlog)
    {
        auto *entry = find(handle); if (!entry) return stale();
        if (entry->endpoint) return Status::invalid_argument("TCP stream has already started");
        auto endpoint = table_.listen(entry->local, backlog); if (!endpoint) return endpoint.status();
        entry->endpoint = endpoint.value(); return {};
    }
    Status connect(TcpStreamHandle handle, tcp::Endpoint peer, u32 iss, u64 now)
    {
        auto *entry = find(handle); if (!entry) return stale();
        if (entry->endpoint) return Status::already_exists("TCP stream has already started");
        auto endpoint = table_.connect(entry->local, peer, iss, now); if (!endpoint) return endpoint.status();
        entry->endpoint = endpoint.value(); return {};
    }
    Result<TcpStreamHandle> accept(TcpStreamHandle listener)
    {
        auto endpoint = endpoint_handle(listener); if (!endpoint) return endpoint.status();
        auto selected = vacant(); if (!selected) return selected.status();
        auto accepted = table_.accept(endpoint.value()); if (!accepted) return accepted.status();
        auto state = table_.state(accepted.value());
        if (!state)
        { static_cast<void>(table_.release(accepted.value(), now())); return state.status(); }
        return allocate(selected.value(), state.value().local, accepted.value());
    }
    Result<TcpAcceptReservation> prepare_accept(TcpStreamHandle listener)
    {
        auto endpoint = endpoint_handle(listener); if (!endpoint) return endpoint.status();
        if (endpoint.value().kind != tcp::HandleKind::listener)
            return Status::invalid_argument("TCP accept requires a listener stream");
        if (!capacity_.operation_max || accept_operation_serial_ == capacity_.operation_max)
            return Status::overflow("TCP stream accept operation serial is exhausted");
        auto selected = vacant(); if (!selected) return selected.status();
        auto &entry = streams_[selected.value()];
        const auto operation = ++accept_operation_serial_;
        ++entry.generation; entry.reservation = operation;
        auto prepared = table_.prepare_accept(endpoint.value());
        if (!prepared)
        { entry.reservation = 0; return prepared.status(); }
        entry.prepared = prepared.value(); entry.prepared_listener = listener;
        return TcpAcceptReservation{listener, {identity_, entry.generation, static_cast<u32>(selected.value())},
            operation, prepared.value()};
    }
    [[nodiscard]] Status validate_accept(const TcpAcceptReservation &reservation) const
    {
        const auto *entry = reserved(reservation); if (!entry) return Status::busy("TCP stream accept reservation is stale");
        const auto *listener = find(reservation.listener);
        if (!listener || !listener->endpoint || *listener->endpoint != reservation.endpoint.ticket.listener)
            return Status::busy("TCP original listener stream was revoked");
        return table_.validate_accept(reservation.endpoint.ticket);
    }
    Result<TcpStreamHandle> commit_accept(const TcpAcceptReservation &reservation)
    {
        if (auto status = validate_accept(reservation); !status) return status;
        auto accepted = table_.commit_accept(reservation.endpoint.ticket); if (!accepted) return accepted.status();
        auto &entry = streams_[reservation.stream.slot];
        // All reservations were checked before endpoint ownership transfer.
        entry.local = reservation.endpoint.local; entry.endpoint = accepted.value();
        entry.prepared = {}; entry.prepared_listener = {}; entry.reservation = 0; entry.used = true;
        return reservation.stream;
    }
    Status cancel_accept(const TcpAcceptReservation &reservation)
    {
        auto *entry = reserved(reservation); if (!entry) return Status::invalid_argument("foreign or stale TCP stream accept cancellation");
        const auto status = table_.cancel_accept(reservation.endpoint.ticket);
        if (!status) return status; // Explicit reserved owner prevents reuse until retry.
        entry->prepared = {}; entry->prepared_listener = {}; entry->reservation = 0;
        return {};
    }
    [[nodiscard]] Result<tcp::Readiness> readiness(TcpStreamHandle handle) const
    {
        auto endpoint = endpoint_handle(handle); return endpoint ? table_.readiness(endpoint.value())
            : Result<tcp::Readiness>{endpoint.status()};
    }
    [[nodiscard]] Result<usize> peek(TcpStreamHandle handle, std::span<std::byte> bytes) const
    {
        if (aliases({bytes.data(), bytes.size()})) return Status::invalid_argument("TCP stream peek aliases its owner");
        auto endpoint = endpoint_handle(handle); return endpoint ? table_.peek(endpoint.value(), bytes)
            : Result<usize>{endpoint.status()};
    }
    Result<usize> read(TcpStreamHandle handle, std::span<std::byte> bytes)
    {
        if (aliases({bytes.data(), bytes.size()})) return Status::invalid_argument("TCP stream read aliases its owner");
        auto endpoint = endpoint_handle(handle); return endpoint ? table_.read(endpoint.value(), bytes)
            : Result<usize>{endpoint.status()};
    }
    Result<usize> write(TcpStreamHandle handle, wire::Bytes bytes)
    {
        if (aliases(bytes)) return Status::invalid_argument("TCP stream write aliases its owner");
        auto endpoint = endpoint_handle(handle); return endpoint ? table_.write(endpoint.value(), bytes)
            : Result<usize>{endpoint.status()};
    }
    Status shutdown_write(TcpStreamHandle handle)
    {
        auto endpoint = endpoint_handle(handle); return endpoint ? table_.shutdown_write(endpoint.value()) : endpoint.status();
    }
  private:
    friend class ::ok::user::native::TcpOriginalDescription;
    Status bind_original_operation(void *context, void (*closed)(void *, const void *, i32, u16))
    {
        if (original_context_) return Status::busy("TCP original immediate operation is already bound");
        if (!context || !closed) return Status::invalid_argument("TCP original immediate observer is incomplete");
        original_context_ = context; original_closed_ = closed; return {};
    }
    Status clear_original_operation(void *context)
    {
        if (original_context_ != context) return Status::denied("TCP original immediate observer has a foreign owner");
        original_context_ = nullptr; original_closed_ = nullptr; return {};
    }
    bool aliases(wire::Bytes bytes) const
    {
        if (bytes.empty()) return false;
        const auto start = reinterpret_cast<uptr>(bytes.data()), owner = reinterpret_cast<uptr>(this);
        return start >= owner ? start - owner < sizeof(*this) : owner - start < bytes.size();
    }
    struct Stream
    {
        bool used{}, closing{}; u64 generation{}, reservation{};
        tcp::Endpoint local{}; std::optional<tcp::Handle> endpoint{};
        tcp::AcceptPreview prepared{};
        TcpStreamHandle prepared_listener{};
    };
    Stream *reserved(const TcpAcceptReservation &reservation)
    {
        const auto handle = reservation.stream;
        if (!reservation.operation || handle.owner != identity_ || !handle.generation || handle.slot >= streams_.size()) return nullptr;
        auto &entry = streams_[handle.slot];
        return !entry.used && !entry.closing && entry.generation == handle.generation && entry.reservation == reservation.operation &&
            entry.prepared_listener == reservation.listener && entry.prepared.ticket == reservation.endpoint.ticket &&
            entry.prepared.local == reservation.endpoint.local &&
            entry.prepared.peer == reservation.endpoint.peer ? &entry : nullptr;
    }
    const Stream *reserved(const TcpAcceptReservation &reservation) const
    { return const_cast<NativeTcpStreamService *>(this)->reserved(reservation); }
    Status drain(Stream &entry)
    {
        if (entry.endpoint && !(table_.pools().retired && entry.endpoint->kind == tcp::HandleKind::listener))
        {
            auto status = table_.release(*entry.endpoint, now());
            if (!status)
                return status.code() == StatusCode::busy ? Status::success() : status;
        }
        const auto generation = entry.generation; entry = {}; entry.generation = generation;
        return {};
    }
    static Status stale() { return Status::bad_descriptor("TCP stream owner is stale"); }
    Stream *find(TcpStreamHandle handle)
    {
        return handle.owner == identity_ && handle.generation && handle.slot < streams_.size() &&
            streams_[handle.slot].used && streams_[handle.slot].generation == handle.generation ? &streams_[handle.slot] : nullptr;
    }
    const Stream *find(TcpStreamHandle handle) const
    { return const_cast<NativeTcpStreamService *>(this)->find(handle); }
    Result<tcp::Handle> endpoint_handle(TcpStreamHandle handle) const
    {
        const auto *entry = find(handle); if (!entry) return stale();
        return entry->endpoint ? Result<tcp::Handle>{*entry->endpoint}
            : Result<tcp::Handle>{Status::invalid_argument("TCP stream is not connected or listening")};
    }
    Result<usize> vacant() const
    {
        if (!identity_ || !capacity_.generation_max || !capacity_.owners || capacity_.owners > streams_.size())
            return Status::invalid_argument("invalid TCP stream owner capacity");
        for (usize i = 0; i < capacity_.owners; ++i)
            if (!streams_[i].used && !streams_[i].closing && !streams_[i].reservation &&
                streams_[i].generation < capacity_.generation_max) return i;
        return Status::no_memory("TCP stream owner pool is exhausted");
    }
    TcpStreamHandle allocate(usize slot, tcp::Endpoint local, std::optional<tcp::Handle> endpoint)
    {
        auto &entry = streams_[slot]; ++entry.generation; entry.used = true; entry.local = local; entry.endpoint = endpoint;
        return {identity_, entry.generation, static_cast<u32>(slot)};
    }
    Table table_;
    const u64 identity_;
    const TcpStreamCapacity capacity_;
    std::array<Stream, stream_capacity> streams_{};
    u64 accept_operation_serial_{};
    TcpWaitObserver observer_{};
    void *original_context_{};
    void (*original_closed_)(void *, const void *, i32, u16){};
};
}
