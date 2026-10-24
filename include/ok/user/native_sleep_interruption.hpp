#pragma once

#include "ok/abi/linux_native.hpp"
#include "ok/sched/scheduler.hpp"
#include "ok/user/native_signal_action.hpp"

namespace ok::user::native
{

// A blocked syscall retains its captured frame unchanged until completion.
// Sleep has at most four register arguments on every supported ABI, including
// o32; decoding continuation metadata never rereads the user's time request.
template <abi::linux_user::Profile Profile>
struct LinuxSleepInterruption
{
    using Codec = abi::linux_user::Codec<Profile>;
    using Bridge = abi::linux_user::NativeBridge<Profile>;
    using Format = abi::linux_user::TimeFormat;
    struct Request
    {
        uptr remainder{};
        Format format{Format::kernel_timespec};
        bool absolute{};
    };
    enum class Outcome : u8 { lost, interrupted, timeout, failed };
    struct Completion
    {
        Outcome outcome{Outcome::lost};
        Request request{};
        u64 deadline{};
        i64 result{};
    };

    [[nodiscard]] static Result<Request> decode(const arch::execution::UserFrame &frame)
    {
        using Operation = abi::linux_user::Operation;
        const auto call = Codec::decode_call(Bridge::snapshot(frame), 4);
        if (!call) return call.status();
        auto is = [&](Operation operation) {
            const auto number = Codec::syscall_number(operation);
            return number && call.value().number == *number;
        };
        const bool plain = is(Operation::nanosleep) || is(Operation::nanosleep_time32);
        if (!plain && !is(Operation::clock_nanosleep) && !is(Operation::clock_nanosleep_time32) &&
            !is(Operation::clock_nanosleep_time64)) return Status::unsupported("not a captured Linux sleep");
        const auto &arguments = call.value().arguments;
        return Request{static_cast<uptr>(arguments[plain ? 1 : 3]),
            Codec::profile.word_bytes == 8 || is(Operation::clock_nanosleep_time64)
                ? Format::kernel_timespec : Format::legacy_timespec,
            !plain && (static_cast<u32>(arguments[1]) & 1U) != 0};
    }

    [[nodiscard]] static bool owns(const sched::ProcessControlBlock &process,
                                   const sched::ThreadControlBlock &thread, sched::WaitTicket ticket)
    {
        return process.resources().personality == sched::UserPersonality::linux &&
            process.state() != sched::ProcessState::zombie && process.state() != sched::ProcessState::exited &&
            thread.owner == process.pid() && thread.native_active && thread.syscall_pending &&
            thread.state == sched::ProcessState::blocked && thread.wait_reason == sched::ThreadWaitReason::deadline &&
            ticket.owner == process.pid() && ticket.thread == thread.tid && thread.wait_registration.pending(ticket) &&
            thread.native_deadline.armed() && thread.wait_key == thread.native_deadline.generation() &&
            decode(thread.native_frame).ok();
    }

    // The caller has revalidated an eligible signal and serializes selection,
    // this claim and subsequent handler construction without a scheduling edge.
    // Complete owns cleanup before copyout; losing tickets do absolutely nothing.
    template <typename Write, typename Finish>
    [[nodiscard]] static Completion complete(sched::SchedulerState &scheduler,
        sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread,
        sched::WaitTicket ticket, u64 now, Write &&write, Finish &&finish)
    {
        if (!owns(process, thread, ticket)) return {};
        Completion out;
        out.request = decode(thread.native_frame).value();
        out.deadline = thread.native_deadline.deadline();
        const bool expired = now >= out.deadline;
        if (!scheduler.complete_wait(ticket, expired ? sched::WaitCompletion::timeout : sched::WaitCompletion::interrupted))
            return {};
        out.result = expired ? 0 : -4;
        if (!expired && !out.request.absolute && out.request.remainder)
        {
            const u64 remaining = out.deadline - now;
            std::array<u8, 16> bytes{};
            const usize count = Codec::timespec_size(out.request.format);
            if (!Codec::encode_timespec({bytes.data(), count}, out.request.format,
                {static_cast<i64>(remaining / 1000000000), static_cast<i64>(remaining % 1000000000)}))
            { out.outcome = Outcome::failed; return out; }
            if (!std::forward<Write>(write)(out.request.remainder, std::span<const u8>{bytes.data(), count}))
                out.result = -14;
        }
        thread.syscall_pending = false;
        if (!std::forward<Finish>(finish)(out.result))
        { out.outcome = Outcome::failed; return out; }
        out.outcome = expired ? Outcome::timeout : Outcome::interrupted;
        return out;
    }
};

// Ignored pending work must not hide a lower-priority caught signal, but neither
// selecting nor skipping it completes a wait or consumes a signal ticket.
template <abi::linux_user::Profile Profile, typename Queue, typename Supported>
[[nodiscard]] bool sleep_interrupting_signal(const Queue &queue, const sched::SignalActionTable &actions,
                                             sched::SignalMask blocked, Supported &&supported)
{
    using Codec = SignalActionCodec<Profile>;
    typename Queue::Delivery ticket;
    while (queue.peek(blocked, ticket))
    {
        const auto *action = actions.find(ticket.info.signal);
        if (!action) return false;
        if (Codec::ignored(ticket.info.signal, *action))
        { blocked.set(ticket.info.signal); continue; }
        return std::forward<Supported>(supported)(*action);
    }
    return false;
}

} // namespace ok::user::native
