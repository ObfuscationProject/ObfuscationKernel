#pragma once
#include <ok/net/native_tcp_stream.hpp>
#include <ok/sched/scheduler.hpp>

namespace ok::user::native
{
enum class TcpDescriptionKind : u8 { bad_fd, not_socket, retained_tcp };

// An operation owns this pin until its captured result has been published and
// its image authority reset. Resolve process/descriptor records on every check;
// no resource pointer survives a provider callback.
class TcpOriginalDescription final
{
  public:
    TcpOriginalDescription(sched::SchedulerState &scheduler, net::NativeTcpStreamService &service,
                           Status &failure)
        : scheduler_{scheduler}, service_{service}, failure_{failure} {}
    ~TcpOriginalDescription() { static_cast<void>(release()); }
    TcpOriginalDescription(const TcpOriginalDescription &) = delete;
    TcpOriginalDescription &operator=(const TcpOriginalDescription &) = delete;
    TcpOriginalDescription(TcpOriginalDescription &&) = delete;
    TcpOriginalDescription &operator=(TcpOriginalDescription &&) = delete;
    Result<TcpDescriptionKind> acquire(const sched::ProcessControlBlock &process, i32 fd)
    {
        if (attempted_) return Status::busy("TCP original description has already been selected");
        attempted_ = true;
        if (scheduler_.native_tcp_service() != &service_ || scheduler_.find(process.pid()) != &process ||
            process.state() == sched::ProcessState::zombie || process.state() == sched::ProcessState::exited)
            return Status::denied("TCP original process/service is not canonical");
        const auto *slot = process.fd_table().entry(fd);
        const auto *open = slot && slot->used ? scheduler_.open_description(slot->description) : nullptr;
        if (!open || !open->references) return TcpDescriptionKind::bad_fd;
        if (!open->native_tcp_handle) return TcpDescriptionKind::not_socket;
        if (!pure(*open) || !service_.valid(open->native_tcp_handle))
            return Status::io_error("TCP original description has an invalid provider shape");
        const auto selected = slot->description;
        if (auto status = scheduler_.retain_description(selected); !status) return status;
        pid_ = process.pid(); fd_ = fd; description_ = selected; installation_ = slot->installation;
        handle_ = open->native_tcp_handle; flags_ = open->flags;
        const auto bound = service_.bind_original_operation(this,
            [](void *context, const void *resources, i32 closed_fd, u16 description) {
                auto &self = *static_cast<TcpOriginalDescription *>(context);
                const auto *current = self.scheduler_.find(self.pid_);
                if (current && &current->resources() == resources && closed_fd == self.fd_ &&
                    description == self.description_) self.cancelled_ = true;
            });
        if (!bound)
        {
            const auto released = release();
            return !released ? Result<TcpDescriptionKind>{released} : Result<TcpDescriptionKind>{bound};
        }
        observed_ = true;
        return TcpDescriptionKind::retained_tcp;
    }
    [[nodiscard]] bool live() const
    {
        if (cancelled_ || description_ == sched::invalid_description || scheduler_.native_tcp_service() != &service_) return false;
        const auto *process = scheduler_.find(pid_);
        if (!process || process->state() == sched::ProcessState::zombie || process->state() == sched::ProcessState::exited) return false;
        const auto *slot = process->fd_table().entry(fd_);
        const auto *open = scheduler_.open_description(description_);
        return slot && slot->used && slot->description == description_ && slot->installation == installation_ &&
            open && open->references &&
            pure(*open) && open->native_tcp_handle == handle_ && service_.valid(handle_);
    }
    [[nodiscard]] bool matches(u64 pid, i32 fd) const { return pid == pid_ && fd == fd_ && live(); }
    [[nodiscard]] net::TcpStreamHandle handle() const { return handle_; }
    [[nodiscard]] u32 flags() const { return flags_; }
    Status release()
    {
        const auto selected = std::exchange(description_, sched::invalid_description);
        if (selected == sched::invalid_description) return {};
        const auto unbound = std::exchange(observed_, false) ? service_.clear_original_operation(this) : Status{};
        const auto status = scheduler_.release_description(selected);
        const auto result = !unbound ? unbound : status;
        if (!result && failure_) failure_ = result;
        return result;
    }
  private:
    static bool pure(const sched::OpenFileDescription &open)
    {
        return open.native_tcp_handle && !open.reservation && !open.native_udp_handle && !open.native_shared_file && !open.unix_endpoint && !open.node &&
            open.pipe == sched::invalid_description && open.unix_write_pipe == sched::invalid_description &&
            !open.console && !open.input_console;
    }
    sched::SchedulerState &scheduler_;
    net::NativeTcpStreamService &service_;
    Status &failure_;
    u64 pid_{}, installation_{};
    i32 fd_{};
    u16 description_{sched::invalid_description};
    net::TcpStreamHandle handle_{};
    u32 flags_{};
    bool attempted_{}, observed_{}, cancelled_{};
};
}
