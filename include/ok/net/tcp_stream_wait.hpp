#pragma once
#include <ok/net/native_tcp_stream.hpp>
#include <ok/sched/scheduler.hpp>

namespace ok::net
{
struct TcpWaitSnapshot
{
    sched::WaitTicket ticket{}; TcpStreamHandle stream{}; sched::WaitCompletion completion{};
    u16 description{sched::invalid_description}; bool owns_description{};
};

// Real scheduler registrations, serialized with endpoint and descriptor work.
// Copy failure leaves a ready record intact until the caller explicitly finishes.
class TcpStreamWaits final
{
  public:
    static constexpr usize capacity = 32;
    TcpStreamWaits(sched::SchedulerState &scheduler, NativeTcpStreamService &service)
        : scheduler_{scheduler}, service_{service}
    {
        status_ = service_.set_wait_observer({this,
            [](void *self, const void *resources, i32 fd, u16 description)
            { static_cast<TcpStreamWaits *>(self)->closed(resources, fd, description); },
            [](void *self, u64 pid) { static_cast<TcpStreamWaits *>(self)->cancel_process(pid); }});
        bound_ = bool(status_);
    }
    TcpStreamWaits(const TcpStreamWaits &) = delete;
    TcpStreamWaits &operator=(const TcpStreamWaits &) = delete;
    TcpStreamWaits(TcpStreamWaits &&) = delete;
    TcpStreamWaits &operator=(TcpStreamWaits &&) = delete;
    ~TcpStreamWaits() { static_cast<void>(shutdown()); }
    [[nodiscard]] Status status() const { return status_; }
    [[nodiscard]] usize records() const
    { usize count{}; for (const auto &record : waits_) count += record.used; return count; }
    Result<sched::WaitTicket> arm(u64 pid, i32 fd, u8 interests, std::optional<u64> deadline = {})
    {
        if (!status_) return status_;
        if (scheduler_.native_tcp_service() != &service_) return Status::denied("TCP wait service is not canonical");
        if (!interests || (interests & ~u8{15})) return Status::invalid_argument("TCP wait interests are invalid");
        auto *process = scheduler_.find(pid); auto *thread = scheduler_.current_thread();
        if (!process || !thread || thread->owner != pid || scheduler_.current_pid() != pid)
            return Status::denied("TCP wait requires the current canonical thread");
        for (const auto &record : waits_) if (record.used && record.ticket.owner == pid && record.ticket.thread == thread->tid)
            return Status::busy("TCP thread already owns an unconsumed wait result");
        Record *record = nullptr;
        for (auto &entry : waits_) if (!entry.used) { record = &entry; break; }
        if (!record) return Status::no_memory("TCP wait record pool is full");
        const auto *slot = process->fd_table().entry(fd);
        const auto *open = slot && slot->used ? scheduler_.open_description(slot->description) : nullptr;
        if (!open || !open->references || !open->native_tcp_handle) return Status::bad_descriptor("TCP wait descriptor is unavailable");
        if (open->flags & nonblock) return Status::would_block("TCP nonblocking description cannot sleep");
        auto ready = service_.readiness(open->native_tcp_handle); if (!ready) return ready.status();
        if (deadline && *deadline <= service_.now()) return Status::would_block("TCP wait deadline has elapsed");
        if (!ready.value().may_wait(ready.value().handle, ready.value().sequence, interests))
            return Status::would_block("TCP wait condition is already ready");
        const auto id = slot->description; const auto stream = open->native_tcp_handle;
        if (auto status = scheduler_.retain_description(id); !status) return status;
        auto ticket = scheduler_.register_current_wait(sched::ThreadWaitReason::tcp_stream, stream.generation);
        if (!ticket)
        { remember(scheduler_.release_description(id)); return ticket.status(); }
        *record = {true, true, ticket.value(), stream, id, fd, interests, ready.value().sequence,
                   ready.value().handle, deadline, sched::WaitCompletion::none};
        // No serialized endpoint/descriptor mutation may escape this recheck.
        ready = service_.readiness(stream);
        if (!ready || !ready.value().may_wait(record->endpoint, record->observed, interests))
            complete(*record, sched::WaitCompletion::ready);
        return ticket.value();
    }
    [[nodiscard]] Result<TcpWaitSnapshot> observe(sched::WaitTicket ticket) const
    {
        const auto *record = find(ticket); if (!record) return Status::not_found("TCP wait ticket is stale");
        if (record->completion == sched::WaitCompletion::none) return Status::would_block("TCP wait remains pending");
        return TcpWaitSnapshot{ticket, record->stream, record->completion, record->description, record->held};
    }
    Status finish(sched::WaitTicket ticket)
    {
        auto *record = find(ticket); if (!record) return Status::not_found("TCP wait ticket is stale");
        if (record->completion == sched::WaitCompletion::none) return Status::busy("TCP wait is still pending");
        const auto detached = *record; *record = {};
        return detached.held ? remember(scheduler_.release_description(detached.description)) : Status::success();
    }
    Status cancel(sched::WaitTicket ticket, sched::WaitCompletion cause = sched::WaitCompletion::cancelled)
    {
        if (cause != sched::WaitCompletion::cancelled && cause != sched::WaitCompletion::interrupted)
            return Status::invalid_argument("TCP cancellation cause is invalid");
        auto *record = find(ticket); if (!record) return Status::not_found("TCP wait ticket is stale");
        if (record->completion != sched::WaitCompletion::none) return Status::already_exists("TCP wait has already completed");
        complete(*record, cause); return status_;
    }
    Status poll()
    {
        if (!status_) return status_;
        for (auto &record : waits_) if (record.used && record.completion == sched::WaitCompletion::none)
        {
            auto *thread = scheduler_.find_thread(record.ticket.owner, record.ticket.thread);
            if (!thread || thread->wait_registration.ticket() != record.ticket)
            { complete(record, sched::WaitCompletion::cancelled); continue; }
            if (!thread->wait_registration.active())
            {
                auto completion = thread->wait_registration.completion();
                complete(record, completion == sched::WaitCompletion::none ? sched::WaitCompletion::cancelled : completion);
                continue;
            }
            const auto *open = scheduler_.open_description(record.description);
            const auto ready = service_.readiness(record.stream);
            if (!open || open->native_tcp_handle != record.stream || (open->flags & nonblock) || !ready ||
                !ready.value().may_wait(record.endpoint, record.observed, record.interests))
                complete(record, sched::WaitCompletion::ready);
            else if (record.deadline && service_.now() >= *record.deadline)
                complete(record, sched::WaitCompletion::timeout);
        }
        return status_;
    }
    // Exec/exit invalidate the old operation entirely, including completed results.
    void cancel_process(u64 pid)
    {
        for (auto &record : waits_) if (record.used && record.ticket.owner == pid)
        {
            complete(record, sched::WaitCompletion::cancelled);
            const auto ticket = record.ticket; static_cast<void>(finish(ticket));
        }
    }
    Status shutdown()
    {
        for (auto &record : waits_) if (record.used)
        {
            complete(record, sched::WaitCompletion::cancelled);
            const auto ticket = record.ticket; static_cast<void>(finish(ticket));
        }
        if (bound_) { remember(service_.clear_wait_observer(this)); bound_ = false; }
        return status_;
    }
  private:
    static constexpr u32 nonblock = 0x0800;
    struct Record
    {
        bool used{}, held{}; sched::WaitTicket ticket{}; TcpStreamHandle stream{};
        u16 description{sched::invalid_description}; i32 fd{}; u8 interests{};
        u64 observed{}; tcp::Handle endpoint{}; std::optional<u64> deadline{};
        sched::WaitCompletion completion{};
    };
    Record *find(sched::WaitTicket ticket)
    { for (auto &record : waits_) if (record.used && record.ticket == ticket) return &record; return nullptr; }
    const Record *find(sched::WaitTicket ticket) const
    { return const_cast<TcpStreamWaits *>(this)->find(ticket); }
    Status remember(Status status) { if (!status && status_) status_ = status; return status; }
    void release_pin(Record &record)
    {
        if (!record.held) return;
        record.held = false; const auto id = record.description; record.description = sched::invalid_description;
        remember(scheduler_.release_description(id));
    }
    void complete(Record &record, sched::WaitCompletion completion)
    {
        if (record.completion == sched::WaitCompletion::none)
        {
            auto *thread = scheduler_.find_thread(record.ticket.owner, record.ticket.thread);
            if (thread && thread->wait_registration.pending(record.ticket))
                if (!scheduler_.complete_wait(record.ticket, completion))
                    remember(Status::fault("TCP waiter lost its exact scheduler ticket"));
        }
        record.completion = completion;
        if (completion != sched::WaitCompletion::ready) release_pin(record);
    }
    void closed(const void *resources, i32 fd, u16 description)
    {
        for (auto &record : waits_) if (record.used && record.fd == fd && record.description == description)
        {
            const auto *process = scheduler_.find(record.ticket.owner);
            if (process && &process->resources() == resources) complete(record, sched::WaitCompletion::cancelled);
        }
    }
    sched::SchedulerState &scheduler_; NativeTcpStreamService &service_;
    std::array<Record, capacity> waits_{}; Status status_{}; bool bound_{true};
};
}
