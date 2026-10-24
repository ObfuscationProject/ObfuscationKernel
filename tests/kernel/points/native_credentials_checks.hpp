#pragma once

#include "ok/user/native_credentials.hpp"
#include <new>

namespace ok::tests
{
// Execute native selection, authorization, PCB publication and copy callbacks.
// This boot diagnostic uses kernel-owned storage, not user-mode trap delivery.
inline Status check_native_credentials_model()
{
    using N = syscall::Number;
    using namespace user::native_credentials;
    alignas(sched::ProcessControlBlock) static std::array<std::byte, sizeof(sched::ProcessControlBlock)> storage{};
    auto &process = *::new(storage.data()) sched::ProcessControlBlock;
    const auto result = [&]() -> Status {
        if (auto status = process.add_thread(1, {}, sched::ProcessState::running); !status) return status;
        process.set_credentials(user::root_credentials());
        usize writes = 0;
        std::array<u32, 3> output{};
        auto call = [&](N number, std::array<uptr, 6> arguments) {
            const auto selected = select(number);
            if (!selected) return i64{-38};
            return dispatch(process, *selected, arguments, [&](uptr field, std::span<const u8> bytes) {
                ++writes;
                if (field >= output.size() || bytes.size() != sizeof(u32)) return false;
                auto *destination = reinterpret_cast<u8 *>(&output[field]);
                for (usize index = 0; index < bytes.size(); ++index) destination[index] = bytes[index];
                return true;
            });
        };
        if (call(N::setresuid, {1000, 1000, 0}) || call(N::setuid, {0}) ||
            process.credentials().euid != 0 || call(N::setregid, {2000, 2001}) ||
            call(N::setresgid, {user::unchanged_id, user::unchanged_id, 2002}) ||
            call(N::getresgid, {0, 1, 2}) || output != std::array<u32, 3>{2000, 2001, 2002})
            return Status::fault("native saved credential boot model failed");
        if (call(N::getresgid, {0, 3, 2}) != -14 || writes != 5)
            return Status::fault("native credential sequential output model failed");
        if (auto status = process.add_thread(2, {}, sched::ProcessState::blocked); !status) return status;
        if (call(N::setgid, {3000}) != -95 || process.credentials().gid != 2000)
            return Status::fault("native credential live thread gate failed");
        process.threads()[1].state = sched::ProcessState::exited;
        if (call(N::setgid, {1000}) || call(N::setreuid, {1000, 1000}) || call(N::setuid, {1000}) ||
            call(N::setuid, {0}) != -1 || call(N::setgid, {0}) != -1 ||
            process.credentials().suid != 1000 || process.credentials().sgid != 1000)
            return Status::fault("native credential permanent drop boot model failed");
        return {};
    }();
    process.~ProcessControlBlock();
    return result;
}
} // namespace ok::tests
