// Actual native runtime dispatch with hosted checked-copy mappings, no guest.
#include "native_clock_model.hpp"
#include "ok/uapi/syscall.h"

namespace
{
using N = syscall::Number;
constexpr u32 keep = OK_ID_UNCHANGED;
usize calls{};

bool same(user::Credentials a, user::Credentials b)
{
    return a.uid == b.uid && a.euid == b.euid && a.suid == b.suid &&
           a.gid == b.gid && a.egid == b.egid && a.sgid == b.sgid && a.kernel_space == b.kernel_space;
}

i64 invoke(Kernel &kernel, sched::ProcessId pid, N number, std::array<uptr, 6> arguments = {})
{
    bool complete = true;
    ++calls;
    const auto result = native::dispatch(kernel, pid, {static_cast<uptr>(number), arguments}, complete);
    CHECK(complete);
    return result;
}

void setters(Kernel &kernel, sched::ProcessId pid, sched::ProcessId other)
{
    const N numbers[]{N::setuid, N::setgid, N::setreuid, N::setregid, N::setresuid, N::setresgid};
    const user::Credentials base{.uid=17, .gid=27, .euid=18, .egid=28, .suid=19, .sgid=29};
    for (usize index = 0; index < std::size(numbers); ++index)
    {
        auto *process = kernel.scheduler().find(pid);
        process->set_credentials(base);
        const auto stores = mock::outputs;
        CHECK(kernel.scheduler().activate(other));
        CHECK(invoke(kernel, pid, numbers[index], {42, 43, 44}) == -22);
        CHECK(same(process->credentials(), base) && mock::outputs == stores);
        CHECK(kernel.scheduler().activate(pid));
        auto *thread = kernel.scheduler().current_thread();
        thread->state = sched::ProcessState::blocked;
        CHECK(invoke(kernel, pid, numbers[index], {42, 43, 44}) == -22);
        thread->state = sched::ProcessState::running;
        CHECK(invoke(kernel, pid, numbers[index], {42, 43, 44}) == -1);
        CHECK(same(process->credentials(), base));
        CHECK(invoke(kernel, pid, numbers[index], {keep, keep, keep}) == (index < 2 ? -22 : 0));
        CHECK(same(process->credentials(), base));
        process->set_credentials(user::root_credentials());
        constexpr uptr high = sizeof(uptr) == 8 ? static_cast<uptr>(u64{0x12345678} << 32) : 0;
        CHECK(invoke(kernel, pid, numbers[index], {high | 4000000000U, high | 4000000001U,
                                                 high | 4000000002U}) == 0);
        const bool group = index % 2;
        auto expected = user::root_credentials();
        const u32 real = 4000000000U;
        const u32 effective = index < 2 ? real : 4000000001U;
        const u32 saved = index < 4 ? effective : 4000000002U;
        if (group) { expected.gid = real; expected.egid = effective; expected.sgid = saved; }
        else { expected.uid = real; expected.euid = effective; expected.suid = saved; }
        CHECK(same(process->credentials(), expected) && mock::outputs == stores);
        const auto peer = kernel.scheduler().create_thread(pid, thread->context); CHECK(peer);
        CHECK(invoke(kernel, pid, numbers[index], {keep, keep, keep}) == -95);
        CHECK(same(process->credentials(), expected));
        CHECK(kernel.scheduler().exit_thread(pid, peer.value()));
        process->set_credentials(user::root_credentials());
        CHECK(invoke(kernel, pid, numbers[index], {71, 72, 73}) == 0);
        for (usize i = process->threads().size(); i > 1; --i)
            if (process->threads()[i-1].state == sched::ProcessState::exited)
                CHECK(process->threads().erase_at(i-1));
        process->set_credentials(user::kernel_credentials());
        CHECK(invoke(kernel, pid, numbers[index], {42, 43, 44}) == -1);
        CHECK(same(process->credentials(), user::kernel_credentials()));
        process->set_credentials(base);
        process->resources().personality = sched::UserPersonality::linux;
        CHECK(invoke(kernel, pid, numbers[index], {42, 43, 44}) == -38);
        CHECK(same(process->credentials(), base));
        process->resources().personality = sched::UserPersonality::native;
    }
    auto *process = kernel.scheduler().find(pid);
    process->set_credentials(user::root_credentials());
    CHECK(invoke(kernel, pid, N::setresuid, {1000, 1000, 0}) == 0);
    CHECK(invoke(kernel, pid, N::setuid, {0}) == 0 && process->credentials().euid == 0);
    CHECK(invoke(kernel, pid, N::setgid, {1000}) == 0);
    CHECK(invoke(kernel, pid, N::setuid, {1000}) == 0);
    CHECK(process->credentials().uid == 1000 && process->credentials().euid == 1000 &&
          process->credentials().suid == 1000 && process->credentials().gid == 1000 &&
          process->credentials().egid == 1000 && process->credentials().sgid == 1000);
    CHECK(invoke(kernel, pid, N::setuid, {0}) == -1);
    CHECK(invoke(kernel, pid, N::setgid, {0}) == -1);
    CHECK(invoke(kernel, pid, N::getuid) == 1000 && invoke(kernel, pid, N::geteuid) == 1000 &&
          invoke(kernel, pid, N::getgid) == 1000 && invoke(kernel, pid, N::getegid) == 1000);
}

void getters(Kernel &kernel, sched::ProcessId pid, sched::ProcessId other)
{
    auto *process = kernel.scheduler().find(pid);
    const user::Credentials ids{.uid=65535, .gid=0x12345678, .euid=65536, .egid=29,
                                .suid=0xfffffffe, .sgid=65535};
    process->set_credentials(ids);
    auto read = [&](usize offset) { u32 value{}; std::memcpy(&value, mock::memory.data()+offset, 4); return value; };
    const std::array<uptr, 6> addresses{hw::user_begin+129, hw::user_begin+141, hw::user_begin+153};
    for (const auto number : {N::getresuid, N::getresgid})
    {
        const bool group = number == N::getresgid;
        const u32 values[]{group ? ids.gid : ids.uid, group ? ids.egid : ids.euid, group ? ids.sgid : ids.suid};
        const auto stores = mock::outputs;
        CHECK(kernel.scheduler().activate(other));
        CHECK(invoke(kernel, pid, number, addresses) == -22 && mock::outputs == stores);
        CHECK(kernel.scheduler().activate(pid));
        mock::memory.fill(std::byte{0x55}); auto before = mock::memory;
        CHECK(invoke(kernel, pid, number, addresses) == 0);
        for (usize field = 0; field < 3; ++field)
        {
            const usize offset = addresses[field] - hw::user_begin;
            CHECK(read(offset) == values[field]);
            for (usize byte = 0; byte < 4; ++byte) before[offset+byte] = mock::memory[offset+byte];
        }
        CHECK(mock::memory == before);
        for (usize fault = 0; fault < 3; ++fault)
        {
            mock::memory.fill(std::byte{0x55}); mock::write_forbidden = addresses[fault];
            const auto writes = mock::outputs;
            CHECK(invoke(kernel, pid, number, addresses) == -14 && mock::outputs == writes+fault+1);
            for (usize field = 0; field < 3; ++field)
                CHECK(read(addresses[field]-hw::user_begin) == (field < fault ? values[field] : 0x55555555U));
            CHECK(same(process->credentials(), ids)); mock::write_forbidden = 0;
        }
        CHECK(invoke(kernel, pid, number, {addresses[0], addresses[0], addresses[0]}) == 0 && read(129) == values[2]);
        CHECK(invoke(kernel, pid, number, {0, addresses[1], addresses[2]}) == -14);
        const auto end = hw::user_begin + mock::memory.size() - 4;
        CHECK(invoke(kernel, pid, number, {end, end, end}) == 0 && read(mock::memory.size()-4) == values[2]);
        CHECK(invoke(kernel, pid, number, {end+1, end, end}) == -14);
        const auto peer = kernel.scheduler().create_thread(pid, process->threads()[0].context); CHECK(peer);
        CHECK(invoke(kernel, pid, number, addresses) == 0);
        CHECK(kernel.scheduler().exit_thread(pid, peer.value()));
    }
}
}

int main()
{
    static_assert(OK_NATIVE_CREDENTIALS_VERSION == 1 && sizeof(u32) == 4);
    static_assert(OK_SYS_SETUID == static_cast<u64>(N::setuid) && OK_SYS_GETRESGID == static_cast<u64>(N::getresgid));
    static Kernel kernel; CHECK(kernel.boot({}));
    const auto context = kernel.arch().make_user_context({hw::user_begin, hw::user_end-4096});
    const auto process = kernel.scheduler().create_user_process("native-ids-model", context); CHECK(process);
    const auto other = kernel.scheduler().create_user_process("native-ids-other", context); CHECK(other);
    CHECK(kernel.posix().activate(process.value()));
    CHECK(!user::native_credentials::select(N::getuid));
    setters(kernel, process.value(), other.value());
    getters(kernel, process.value(), other.value());
    CHECK(calls == 84);
    std::puts("NATIVE_CREDENTIALS_MODEL_PASS forms=8 caller_identity=1 checked_copy=1 permanent_drop=1 thread_gate=1 guest=0");
}
