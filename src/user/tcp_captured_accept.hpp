#pragma once
#include "tcp_immediate_access.hpp"
#include <ok/abi/linux_tcp_accept_call.hpp>

namespace ok::user::native
{
struct TcpCapturedAcceptArguments
{
    TcpDescriptionKind original{TcpDescriptionKind::bad_fd};
    abi::linux_user::TcpCall operation{abi::linux_user::TcpCall::none};
    bool linux{};
    std::array<u64, 6> arguments{};
};

// This is the accept-only runtime path. Actual origin authority and memory
// callbacks are mandatory; pure register/value helpers cannot call it.
template <abi::linux_user::Profile Selected>
class TcpCapturedAccept final
{
    using Access = TcpImmediateAccess<Selected>;
    using Call = abi::linux_user::TcpCall;
    using Target = abi::linux_user::Codec<Selected>;
    using Route = abi::linux_user::TcpAcceptCall<Selected>;
  public:
    [[nodiscard]] static Result<TcpCapturedAcceptArguments> capture(sched::SchedulerState &scheduler,
        const arch::execution::UserFrame &saved_frame, Access &access, TcpOriginalDescription &original)
    {
        if (!access.live() || (access.operation() != Call::accept && access.operation() != Call::accept4))
            return Status::denied("TCP accept requires its current captured operation");
        TcpCapturedAcceptArguments result{TcpDescriptionKind::bad_fd, access.operation(), access.linux_personality(), {}};
        abi::linux_user::TcpCallRoute route{};
        i32 fd{};
        if (result.linux)
        {
            const auto registers = abi::linux_user::NativeBridge<Selected>::snapshot(saved_frame);
            route = Route::route(registers);
            if (registers.integer[Target::profile.number_register] != access.raw_syscall() || route.operation != result.operation)
                return Status::denied("TCP accept saved frame differs from its captured route");
            if (route.socketcall)
            {
                if (auto status = Route::interval(route); !status) return status;
                std::array<std::byte, 4> head{};
                if (auto status = access.read(route.array, head); !status) return status;
                auto selected = Route::head(route, head); if (!selected) return selected.status();
                fd = selected.value();
            }
            else fd = static_cast<i32>(static_cast<u32>(registers.integer[Target::profile.argument_registers[0]] & Target::word_mask));
        }
        else
        {
            const auto call = arch::execution::syscall(saved_frame);
            if (call.number != access.raw_syscall()) return Status::denied("TCP accept native saved syscall differs from capture");
            fd = static_cast<i32>(static_cast<u32>(call.arguments[0]));
            for (usize at = 0; at < result.arguments.size(); ++at) result.arguments[at] = call.arguments[at];
            if (result.operation == Call::accept) result.arguments[3] = 0;
        }
        // The head exception ends here. No process/resource reference survives
        // the next memory callback, and the fd is never copied again.
        if (!access.live()) return Status::denied("TCP accept fd capture lost its image");
        const auto *process = scheduler.find(access.pid());
        if (!process) return Status::denied("TCP accept captured process disappeared before original selection");
        auto selected = original.acquire(*process, fd); if (!selected) return selected.status();
        result.original = selected.value();
        if (result.original != TcpDescriptionKind::retained_tcp) return result;
        if (auto status = access.bind_original(original, fd); !status) return status;
        if (result.linux)
        {
            if (route.socketcall)
            {
                std::array<std::byte, 12> tail{};
                const usize size = route.array_bytes - 4;
                if (auto status = access.read(route.array + 4, std::span{tail}.first(size)); !status) return status;
                if (!access.live()) return Status::denied("TCP accept tail lost its captured image/original");
                auto arguments = Route::tail(route, fd, std::span{tail}.first(size));
                if (!arguments) return arguments.status();
                result.arguments = arguments.value();
            }
            else
            {
                // Three/four register arguments on every direct profile,
                // including O32. No stack/control copy belongs here.
                auto call = Target::decode_call(abi::linux_user::NativeBridge<Selected>::snapshot(saved_frame),
                    result.operation == Call::accept4 ? 4 : 3);
                if (!call) return call.status();
                result.arguments = call.value().arguments;
            }
        }
        return access.live() ? Result<TcpCapturedAcceptArguments>{result}
                             : Result<TcpCapturedAcceptArguments>{Status::denied("TCP accept decoded arguments lost authority")};
    }
};
}
