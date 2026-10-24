#pragma once
#include <ok/net/native_tcp_stream.hpp>
#include <ok/sched/scheduler.hpp>

namespace ok::net
{
class TcpStreamOperations;
class PreparedTcpAccept final
{
  public:
    PreparedTcpAccept(const PreparedTcpAccept &) = delete;
    PreparedTcpAccept &operator=(const PreparedTcpAccept &) = delete;
    PreparedTcpAccept &operator=(PreparedTcpAccept &&) = delete;
    PreparedTcpAccept(PreparedTcpAccept &&other) noexcept
        : scheduler_{other.scheduler_}, service_{other.service_}, descriptor_{std::move(other.descriptor_)},
          stream_{other.stream_}, pid_{other.pid_}, installation_{other.installation_}, listener_fd_{other.listener_fd_},
          listener_description_{std::exchange(other.listener_description_, sched::invalid_description)},
          nonblocking_{other.nonblocking_}, close_on_exec_{other.close_on_exec_},
          stream_held_{std::exchange(other.stream_held_, false)}, committed_{other.committed_},
          mutating_{std::exchange(other.mutating_, nullptr)}, failure_{other.failure_}, cleanup_failure_{other.cleanup_failure_} {}
    ~PreparedTcpAccept()
    {
        if (auto status = retire(); !status) descriptor_.preserve_failed_holder();
    }
    [[nodiscard]] i32 descriptor() const { return descriptor_.descriptor(); }
    [[nodiscard]] tcp::Endpoint peer() const { return stream_.endpoint.peer; }
    [[nodiscard]] Status prepare_child()
    {
        if (!live() || stream_held_) return Status::denied("TCP accept cannot select another child for this admission");
        if (auto status = descriptor_.validate(); !status) return status;
        auto prepared = service_->prepare_accept(stream_.listener); if (!prepared) return prepared.status();
        stream_ = prepared.value(); stream_held_ = true;
        return {};
    }
    [[nodiscard]] bool live() const
    {
        if (!cleanup_failure_ || committed_ || listener_description_ == sched::invalid_description || !mutating_ || !*mutating_ ||
            scheduler_->native_tcp_service() != service_) return false;
        const auto *process = scheduler_->find(pid_);
        if (!process || process->state() == sched::ProcessState::zombie || process->state() == sched::ProcessState::exited) return false;
        const auto *slot = process->fd_table().entry(listener_fd_);
        const auto *open = scheduler_->open_description(listener_description_);
        return slot && slot->used && slot->description == listener_description_ && slot->installation == installation_ &&
            open && open->references && !open->reservation && open->native_tcp_handle == stream_.listener &&
            !open->native_udp_handle && !open->native_shared_file && !open->node && open->pipe == sched::invalid_description &&
            open->unix_write_pipe == sched::invalid_description && !open->console && !open->input_console;
    }
    Result<i32> commit()
    {
        if (!live() || !stream_held_) return Status::denied("prepared TCP accept lost its original listener or selected child");
        if (auto status = descriptor_.validate(); !status) return status;
        if (auto status = service_->validate_accept(stream_); !status) return status;
        auto accepted = service_->commit_accept(stream_); if (!accepted) return accepted.status();
        stream_held_ = false;
        const auto fd = descriptor_.publish_checked(sched::OpenFileDescription{.used = true, .readable = true,
            .writable = true, .flags = 2u | (nonblocking_ ? 0x0800u : 0u), .native_tcp_handle = accepted.value()}, close_on_exec_);
        committed_ = true;
        detach_mutation();
        // Retain the original OFD until the caller publishes its captured result.
        return fd;
    }
    Status cancel()
    {
        if (!cleanup_failure_) return cleanup_failure_;
        if (committed_) return {};
        if (stream_held_)
        {
            const auto status = service_->cancel_accept(stream_);
            if (!status) return remember(status);
            stream_held_ = false;
        }
        if (auto status = descriptor_.cancel(); !status) return remember(status);
        detach_mutation();
        return {};
    }
    Status retire()
    {
        if (auto status = cancel(); !status) return status;
        const auto id = std::exchange(listener_description_, sched::invalid_description);
        return id == sched::invalid_description ? Status{} : remember(scheduler_->release_description(id));
    }
  private:
    friend class TcpStreamOperations;
    PreparedTcpAccept(sched::SchedulerState &scheduler, NativeTcpStreamService &service,
        sched::DescriptorReservation &&descriptor, TcpAcceptReservation stream, u64 pid, i32 fd,
        u16 description, u64 installation, bool nonblocking, bool close_on_exec, bool &mutating, Status &failure)
        : scheduler_{&scheduler}, service_{&service}, descriptor_{std::move(descriptor)}, stream_{stream},
          pid_{pid}, installation_{installation}, listener_fd_{fd}, listener_description_{description},
          nonblocking_{nonblocking}, close_on_exec_{close_on_exec}, stream_held_{false}, mutating_{&mutating}, failure_{&failure} {}
    void detach_mutation()
    { if (auto *active = std::exchange(mutating_, nullptr)) *active = false; }
    Status remember(Status status)
    {
        if (!status)
        {
            if (cleanup_failure_) cleanup_failure_ = status;
            if (*failure_) *failure_ = status;
        }
        return status;
    }
    sched::SchedulerState *scheduler_{};
    NativeTcpStreamService *service_{};
    sched::DescriptorReservation descriptor_;
    TcpAcceptReservation stream_{};
    u64 pid_{}, installation_{};
    i32 listener_fd_{};
    u16 listener_description_{sched::invalid_description};
    bool nonblocking_{}, close_on_exec_{}, stream_held_{}, committed_{};
    bool *mutating_{};
    Status *failure_{};
    Status cleanup_failure_{}; // Failed cleanup retains diagnostic holds without destructor retry.
};
}
