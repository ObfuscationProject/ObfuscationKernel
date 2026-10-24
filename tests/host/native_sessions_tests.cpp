// Actual syscall/adapter dispatch with hosted mappings, no user-mode execution.
#include "native_clock_model.hpp"
#include "ok/uapi/syscall.h"

namespace
{
using N = syscall::Number;
using O = lu::Operation;
unsigned profiles{}, calls{}, forms{};

i64 invoke(Kernel &kernel, sched::ProcessId pid, N number, std::array<uptr, 6> args = {})
{
    bool complete = true;
    ++calls;
    const auto result = native::dispatch(kernel, pid, {static_cast<uptr>(number), args}, complete);
    CHECK(complete);
    return result;
}

void native_boundary(Kernel &kernel, sched::ProcessId parent, sched::ProcessId other)
{
    auto &scheduler = kernel.scheduler();
    CHECK(scheduler.activate(parent));
    const auto child = scheduler.fork_process(parent); CHECK(child);
    auto *copy = scheduler.find(child.value());
    CHECK(invoke(kernel, parent, N::getpgid) == static_cast<i64>(parent));
    CHECK(invoke(kernel, parent, N::getpgrp, {~uptr{0}, ~uptr{0}}) == static_cast<i64>(parent));
    CHECK(invoke(kernel, parent, N::getsid, {child.value()}) == static_cast<i64>(parent));
    CHECK(invoke(kernel, parent, N::setsid) == -1);
    CHECK(invoke(kernel, parent, N::getpgid, {static_cast<uptr>(-1)}) == -3);
    CHECK(invoke(kernel, parent, N::getsid, {sched::max_task_id}) == -3);
    CHECK(invoke(kernel, parent, N::setpgid, {child.value(), static_cast<uptr>(-1)}) == -22);
    CHECK(invoke(kernel, parent, N::setpgid, {other, 0}) == -3);
    CHECK(invoke(kernel, parent, N::setpgid, {child.value(), sched::max_task_id}) == -1);
    constexpr uptr upper = sizeof(uptr) == 8 ? static_cast<uptr>(u64{0x12345678} << 32) : 0;
    CHECK(invoke(kernel, parent, N::setpgid, {upper | child.value(), upper | child.value()}) == 0);
    CHECK(copy->process_group_id() == child.value());
    CHECK(scheduler.activate(child.value()));
    CHECK(invoke(kernel, child.value(), N::setsid) == -1);
    CHECK(scheduler.activate(parent));
    CHECK(invoke(kernel, parent, N::setpgid, {child.value(), parent}) == 0);
    CHECK(scheduler.activate(other));
    CHECK(invoke(kernel, child.value(), N::setsid) == -22);
    CHECK(copy->session_id() == parent && copy->process_group_id() == parent);
    CHECK(scheduler.activate(child.value()));
    auto *thread = scheduler.current_thread();
    thread->state = sched::ProcessState::blocked;
    CHECK(invoke(kernel, child.value(), N::setsid) == -22);
    thread->state = sched::ProcessState::running;
    CHECK(invoke(kernel, child.value(), N::setsid, {~uptr{0}, ~uptr{0}}) == static_cast<i64>(child.value()));
    CHECK(invoke(kernel, child.value(), N::getpgrp) == static_cast<i64>(child.value()));
    CHECK(invoke(kernel, child.value(), N::getsid) == static_cast<i64>(child.value()));
    CHECK(scheduler.activate(parent));
    CHECK(invoke(kernel, parent, N::setpgid, {child.value(), parent}) == -1);
    const auto peer = scheduler.create_thread(child.value(), copy->threads()[0].context); CHECK(peer);
    CHECK(invoke(kernel, parent, N::getpgid, {peer.value()}) == static_cast<i64>(child.value()));
    CHECK(invoke(kernel, parent, N::getsid, {peer.value()}) == static_cast<i64>(child.value()));
    CHECK(invoke(kernel, parent, N::setpgid, {peer.value(), parent}) == -22);
    CHECK(scheduler.exit_thread(child.value(), peer.value()));
    CHECK(invoke(kernel, parent, N::getpgid, {peer.value()}) == -3);
    CHECK(scheduler.exit_process(child.value(), 0));
    CHECK(invoke(kernel, parent, N::getsid, {child.value()}) == static_cast<i64>(child.value()));
    CHECK(scheduler.wait_process(parent, child.value()));
    CHECK(invoke(kernel, parent, N::getpgid, {child.value()}) == -3);
}

template <lu::Profile P>
void profile(Kernel &kernel, sched::ProcessId parent, sched::ProcessId other,
             usize number_slot, std::array<usize, 2> slots, std::array<u32, 5> numbers)
{
    using C = lu::Codec<P>;
    auto &scheduler = kernel.scheduler();
    const auto child = scheduler.fork_process(parent); CHECK(child);
    const auto pid = child.value();
    scheduler.find(pid)->resources().personality = sched::UserPersonality::linux;
    CHECK(scheduler.activate(pid));
    auto call = [&](O operation, std::array<u64, 2> arguments, N native_number, usize count) {
        hw::UserFrame frame{};
        frame.registers.fill(~u64{0});
        frame.registers[number_slot] = *C::syscall_number(operation);
        for (usize index = 0; index < 2; ++index) frame.registers[slots[index]] = arguments[index];
        bool done = true, forwarded = false;
        const auto copies = mock::copies;
        const auto result = user::linux_bootstrap::dispatch<P>(kernel, pid, frame, done,
            [&](hw::Syscall request, bool &complete) {
                forwarded = true;
                CHECK(request.number == static_cast<uptr>(native_number));
                for (usize index = 0; index < count; ++index)
                    CHECK(request.arguments[index] == static_cast<uptr>(static_cast<i64>(static_cast<i32>(arguments[index]))));
                for (usize index = count; index < request.arguments.size(); ++index)
                    CHECK(request.arguments[index] == 0);
                return native::dispatch(kernel, pid, request, complete);
            });
        CHECK(done && forwarded && mock::copies == copies);
        CHECK(user::linux_bootstrap::complete<P>(frame, result));
        const auto returned = lu::NativeBridge<P>::decode_return(frame); CHECK(returned);
        CHECK(returned.value().error == (result < 0 ? static_cast<u32>(-result) : 0));
        if (result >= 0) CHECK(returned.value().value == static_cast<u64>(result));
        ++calls;
        return result;
    };
    const O operations[]{O::setpgid, O::getpgid, O::getpgrp, O::setsid, O::getsid};
    for (usize index = 0; index < std::size(operations); ++index)
    {
        const auto number = C::syscall_number(operations[index]);
        CHECK(number.has_value() == (numbers[index] != 0));
        if (number) { CHECK(*number == numbers[index]); ++forms; }
    }
    constexpr u64 high = u64{0x12345678} << 32;
    CHECK(call(O::getpgid, {high, ~u64{0}}, N::getpgid, 1) == static_cast<i64>(parent));
    CHECK(call(O::getsid, {high | parent, 0}, N::getsid, 1) == static_cast<i64>(parent));
    if (C::syscall_number(O::getpgrp))
        CHECK(call(O::getpgrp, {~u64{0}, ~u64{0}}, N::getpgrp, 0) == static_cast<i64>(parent));
    CHECK(call(O::getpgid, {high | 0xffffffffU, 0}, N::getpgid, 1) == -3);
    CHECK(call(O::getsid, {high | 0x80000000U, 0}, N::getsid, 1) == -3);
    CHECK(call(O::setpgid, {high, high | 0xffffffffU}, N::setpgid, 2) == -22);
    CHECK(call(O::setpgid, {high | 0xffffffffU, high | parent}, N::setpgid, 2) == -3);
    CHECK(call(O::setpgid, {high, high}, N::setpgid, 2) == 0);
    CHECK(call(O::setsid, {~u64{0}, ~u64{0}}, N::setsid, 0) == -1);
    CHECK(call(O::setpgid, {high, high | parent}, N::setpgid, 2) == 0);
    CHECK(scheduler.activate(other));
    CHECK(call(O::setsid, {~u64{0}, ~u64{0}}, N::setsid, 0) == -22);
    CHECK(scheduler.find(pid)->session_id() == parent);
    CHECK(scheduler.activate(pid));
    CHECK(call(O::setsid, {~u64{0}, ~u64{0}}, N::setsid, 0) == static_cast<i64>(pid));
    CHECK(call(O::getsid, {0, 0}, N::getsid, 1) == static_cast<i64>(pid));
    CHECK(call(O::setpgid, {0, parent}, N::setpgid, 2) == -1);
    CHECK(scheduler.exit_process(pid, 0));
    CHECK(scheduler.wait_process(parent, pid));
    ++profiles;
}
}

int main()
{
    static_assert(OK_NATIVE_SESSIONS_VERSION == 3 && OK_ESRCH == 3);
    static_assert(OK_SYS_SETSID == static_cast<u64>(N::setsid));
    static_assert(OK_SYS_SETPGID == static_cast<u64>(N::setpgid));
    static Kernel kernel; CHECK(kernel.boot({}));
    const auto context = kernel.arch().make_user_context({hw::user_begin, hw::user_end - 4096});
    const auto process = kernel.scheduler().create_user_process("session-boundary", context); CHECK(process);
    const auto other = kernel.scheduler().create_user_process("session-other", context); CHECK(other);
    const auto pid = process.value();
    native_boundary(kernel, pid, other.value());
    profile<lu::Profile::i386>(kernel,pid,other.value(),7,{4,6},{57,132,65,66,147});
    profile<lu::Profile::x86_64>(kernel,pid,other.value(),14,{9,10},{109,121,111,112,124});
    profile<lu::Profile::aarch64>(kernel,pid,other.value(),8,{0,1},{154,155,0,157,156});
    profile<lu::Profile::arm32>(kernel,pid,other.value(),7,{0,1},{57,132,65,66,147});
    profile<lu::Profile::rv64>(kernel,pid,other.value(),17,{10,11},{154,155,0,157,156});
    profile<lu::Profile::rv32>(kernel,pid,other.value(),17,{10,11},{154,155,0,157,156});
    profile<lu::Profile::loongarch64>(kernel,pid,other.value(),11,{4,5},{154,155,0,157,156});
    profile<lu::Profile::mips>(kernel,pid,other.value(),2,{4,5},{4057,4132,4065,4066,4151});
    profile<lu::Profile::mips64>(kernel,pid,other.value(),2,{4,5},{5107,5119,5109,5110,5122});
    profile<lu::Profile::ppc>(kernel,pid,other.value(),0,{3,4},{57,132,65,66,147});
    CHECK(profiles == 10 && forms == 46);
    std::printf("NATIVE_SESSIONS_BOUNDARY_PASS profiles=%u forms=%u calls=%u caller_authority=1 pid32=1 guest=0\n", profiles, forms, calls);
}
