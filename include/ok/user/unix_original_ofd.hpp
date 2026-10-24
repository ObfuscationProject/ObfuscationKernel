#pragma once
#include "ok/sched/scheduler.hpp"

namespace ok::user::native
{
// A synchronous syscall pins its original canonical OFD before any user copy.
// Revalidation resolves PID and descriptor installation anew after every copy;
// a callback may close/reuse the descriptor, but cannot redirect this operation.
class UnixOriginalDescription final
{
  public:
    explicit UnixOriginalDescription(sched::SchedulerState &owner) : owner_{owner} {}
    ~UnixOriginalDescription() { if (id_ != sched::invalid_description) static_cast<void>(owner_.release_description(id_)); }
    UnixOriginalDescription(const UnixOriginalDescription &) = delete;
    UnixOriginalDescription &operator=(const UnixOriginalDescription &) = delete;
    Status acquire(const sched::ProcessControlBlock &process, u64 fd)
    {
        if (id_ != sched::invalid_description) return Status::busy("UNIX original OFD is already retained");
        if (fd > static_cast<u64>(__INT32_MAX__) || owner_.find(process.pid()) != &process)
            return Status::bad_descriptor("UNIX original descriptor is not canonical");
        const auto *slot = process.fd_table().entry(static_cast<i32>(fd));
        if (!slot || !slot->used) return Status::bad_descriptor("UNIX original descriptor is absent");
        if (!owner_.is_unix_socket(slot->description)) return Status::invalid_argument("original descriptor is not a UNIX socket");
        if (auto held = owner_.retain_description(slot->description); !held) return held;
        id_ = slot->description; pid_ = process.pid(); fd_ = static_cast<i32>(fd); installation_ = slot->installation;
        return {};
    }
    bool live() const
    {
        const auto *process = owner_.find(pid_);
        if (!process || process->state() == sched::ProcessState::zombie || process->state() == sched::ProcessState::exited) return false;
        const auto *slot = process->fd_table().entry(fd_);
        return slot && slot->used && slot->description == id_ && slot->installation == installation_ && owner_.is_unix_socket(id_);
    }
    u16 description() const { return id_; }
  private:
    sched::SchedulerState &owner_;
    sched::ProcessId pid_{};
    i32 fd_{};
    u16 id_{sched::invalid_description};
    u64 installation_{};
};
}
