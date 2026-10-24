#pragma once

#include "ok/arch/execution.hpp"
#include "ok/posix/posix.hpp"
#include "ok/sched/signal_mask.hpp"
#include "ok/uapi/syscall.h"
#include <bit>

namespace ok { class Kernel; }
namespace ok::user::native
{
// Copied boundary data: no pointer to a temporary kernel request survives
// admission. Completion destinations remain checked user addresses.
enum class PollTimeoutFormat : u8 { timespec, timeval };
struct PollTimeoutOutput
{
    uptr address{};
    posix::ClockTime end{};
    u8 field_bytes{};
    bool big_endian{};
    PollTimeoutFormat format{PollTimeoutFormat::timespec};
};
struct PollRequest
{
    uptr output{};
    std::span<const ok_pollfd> descriptors{};
    posix::ClockTime timeout{};
    PollTimeoutOutput timeout_output{};
    bool timed{}, immediate{}, absolute{}, linux_events{};
    bool big_endian{std::endian::native == std::endian::big};
    u16 read_normal{}, write_normal{};
};

// Private mask admission; ordinary builds explicitly return ENOSYS. These
// functions add no fields to exported request or scheduler records. The caller
// supplies a mask already sanitized by its selected Linux ABI codec.
i64 begin_poll_signal_mask(Kernel &kernel, sched::ProcessId pid, sched::SignalMask requested);
void finish_poll_signal_mask(Kernel &kernel, sched::ProcessId pid, bool complete);
class PollSignalMaskScope
{
public:
    PollSignalMaskScope(Kernel &kernel, sched::ProcessId pid, bool &complete)
        : kernel_(kernel), pid_(pid), complete_(complete) {}
    PollSignalMaskScope(const PollSignalMaskScope &) = delete;
    PollSignalMaskScope &operator=(const PollSignalMaskScope &) = delete;
    i64 begin(sched::SignalMask mask)
    {
        const auto result = begin_poll_signal_mask(kernel_, pid_, mask);
        active_ = result == 0;
        return result;
    }
    ~PollSignalMaskScope() { if (active_) finish_poll_signal_mask(kernel_, pid_, complete_); }
private:
    Kernel &kernel_;
    sched::ProcessId pid_;
    bool &complete_;
    bool active_{};
};

// Common serialized boot-CPU registration retains descriptions and freezes
// request bytes. Runtime completion encodes the actual process personality.
i64 begin_poll(Kernel &kernel, sched::ProcessId pid, const PollRequest &request, bool &complete);
void finish_poll_timeout(const arch::execution::AddressSpace &space, const PollTimeoutOutput &output, u64 now);
} // namespace ok::user::native
