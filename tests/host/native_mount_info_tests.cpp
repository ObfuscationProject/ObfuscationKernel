// Actual native dispatch and serializer with hosted mappings; no guest execution.
#include "native_clock_model.hpp"
#include "ok/user/native_mount_info.hpp"

namespace
{
using N = syscall::Number;
constexpr usize output_offset = 513;
constexpr uptr output_address = hw::user_begin + output_offset;
i64 invoke(Kernel &kernel, sched::ProcessId pid, std::array<uptr, 6> arguments)
{
    bool complete = true;
    const auto result = native::dispatch(kernel, pid, {OK_SYS_MOUNT_INFO, arguments}, complete);
    CHECK(complete);
    return result;
}
ok_mount_info result()
{
    ok_mount_info value{};
    std::memcpy(&value, mock::memory.data() + output_offset, sizeof(value));
    return value;
}
void same_snapshot(const ok_mount_info &wire, const fs::MountSnapshot &snapshot)
{
    CHECK(wire.version == 1 && wire.size == 288 && wire.capacity == 9);
    CHECK(wire.generation == snapshot.generation && wire.slot == snapshot.slot &&
          wire.state == static_cast<u32>(snapshot.state) && wire.parent_id == snapshot.parent_id &&
          wire.path_length == snapshot.path_length);
    for (usize index = 0; index < sizeof(wire.path); ++index)
        CHECK(wire.path[index] == (index < snapshot.path_length ? snapshot.path[index] : '\0'));
}
void actual_boundary(Kernel &kernel, sched::ProcessId pid, sched::ProcessId other)
{
    const std::array<uptr, 6> valid{0, output_address, sizeof(ok_mount_info), 0, 0, 0};
    auto reject = [&](std::array<uptr, 6> arguments, i64 expected) {
        mock::memory.fill(std::byte{0x55}); const auto before = mock::memory;
        const auto copies = mock::copies, outputs = mock::outputs;
        CHECK(invoke(kernel, pid, arguments) == expected);
        CHECK(mock::memory == before && mock::copies == copies && mock::outputs == outputs);
    };
    CHECK(kernel.scheduler().activate(other)); reject(valid, -22);
    CHECK(kernel.scheduler().activate(pid));
    auto *thread = kernel.scheduler().current_thread();
    thread->state = sched::ProcessState::blocked; reject(valid, -22);
    thread->state = sched::ProcessState::running;
    auto &process = *kernel.scheduler().find(pid);
    process.resources().personality = sched::UserPersonality::linux;
    reject(valid, -38);
    hw::UserFrame linux_frame{}; linux_frame.registers[14] = OK_SYS_MOUNT_INFO;
    bool linux_complete = true; usize translated_native_calls{};
    const auto linux_copies = mock::copies, linux_outputs = mock::outputs;
    CHECK(user::linux_bootstrap::dispatch<lu::Profile::x86_64>(kernel, pid, linux_frame, linux_complete,
        [&](hw::Syscall, bool &) -> i64 { ++translated_native_calls; return 42; }) == -38);
    CHECK(linux_complete && !translated_native_calls && mock::copies == linux_copies &&
          mock::outputs == linux_outputs);
    auto malformed = valid; malformed[0] = 9; reject(malformed, -38);
    process.resources().personality = sched::UserPersonality::native;
    for (const uptr slot : {uptr{9}, ~uptr{0}})
    { malformed = valid; malformed[0] = slot; reject(malformed, -22); }
    for (const uptr size : {uptr{0}, uptr{287}, uptr{289}, uptr{1024}, ~uptr{0}})
    { malformed = valid; malformed[2] = size; reject(malformed, -22); }
    for (usize field = 3; field < 6; ++field)
    { malformed = valid; malformed[field] = 1; reject(malformed, -22); }
    if constexpr (sizeof(uptr) == 8)
    {
        malformed = valid; malformed[0] = static_cast<uptr>(u64{1} << 32); reject(malformed, -22);
        malformed = valid; malformed[2] |= static_cast<uptr>(u64{1} << 32); reject(malformed, -22);
        for (usize field = 3; field < 6; ++field)
        { malformed = valid; malformed[field] = static_cast<uptr>(u64{1} << 32); reject(malformed, -22); }
    }
    for (const uptr address : {uptr{0}, hw::user_begin - 1,
             hw::user_begin + mock::memory.size() - sizeof(ok_mount_info) + 1, ~uptr{0}})
    { malformed = valid; malformed[1] = address; reject(malformed, -14); }
    mock::write_forbidden = output_address + 47; reject(valid, -14); mock::write_forbidden = 0;
    process.set_credentials({.uid=1000, .gid=1000, .euid=1000, .egid=1000, .suid=1000, .sgid=1000});
    for (uptr slot = 0; slot < 9; ++slot)
    {
        mock::memory.fill(std::byte{0x55}); malformed = valid; malformed[0] = slot;
        fs::MountSnapshot snapshot{}; CHECK(kernel.vfs().mount_snapshot(static_cast<u32>(slot), snapshot));
        const auto copies = mock::copies, outputs = mock::outputs;
        CHECK(invoke(kernel, pid, malformed) == 0 && mock::copies == copies && mock::outputs == outputs + 1);
        same_snapshot(result(), snapshot);
        for (usize index = 0; index < mock::memory.size(); ++index)
            if (index < output_offset || index >= output_offset + sizeof(ok_mount_info))
                CHECK(mock::memory[index] == std::byte{0x55});
    }
    malformed = valid; malformed[1] = hw::user_begin + mock::memory.size() - sizeof(ok_mount_info);
    CHECK(invoke(kernel, pid, malformed) == 0); // The last complete mapped record fits.
    mock::memory.fill(std::byte{0x55}); mock::partial_output = 13;
    CHECK(invoke(kernel, pid, valid) == -14);
    CHECK(mock::memory[output_offset] != std::byte{0x55});
    CHECK(mock::memory[output_offset + 13] == std::byte{0x55});
    mock::partial_output = 0;
}
void topology(Kernel &kernel, sched::ProcessId pid)
{
    fs::MountSnapshot before{}; CHECK(kernel.vfs().mount_snapshot(0, before));
    CHECK(kernel.vfs().create("/mount-info-model", fs::NodeType::directory));
    auto mounted = kernel.vfs().mount_ram_owned("/mount-info-model"); CHECK(mounted);
    const auto slot = mounted.value().id();
    CHECK(invoke(kernel, pid, {slot, output_address, sizeof(ok_mount_info)}) == 0);
    auto wire = result();
    CHECK(wire.slot == slot && wire.state == OK_MOUNT_STATE_OWNED_RAM && wire.parent_id == 0 &&
          wire.generation == before.generation + 1 && std::string_view(wire.path) == "/mount-info-model");
    CHECK(kernel.vfs().detach(mounted.value()));
    CHECK(invoke(kernel, pid, {slot, output_address, sizeof(ok_mount_info)}) == 0);
    const auto detached = result();
    CHECK(detached.state == OK_MOUNT_STATE_DETACHED && detached.generation == wire.generation + 1 &&
          detached.parent_id == wire.parent_id && std::string_view(detached.path) == wire.path);
}
void injected_controls()
{
    using namespace user::native_mount_info;
    const std::array<u64, 6> arguments{1, 0x1234, sizeof(ok_mount_info)};
    fs::MountSnapshot snapshot{}; snapshot.generation = 2; snapshot.slot = 1;
    snapshot.state = fs::MountSnapshotState::owned_provider;
    snapshot.path.fill('x'); snapshot.path[0] = '/'; snapshot.path_length = 1;
    usize queried{}, ranged{}, copied{};
    Status query_status{}; bool range_ok = true, copy_ok = true;
    std::array<u8, sizeof(ok_mount_info)> output{};
    auto call = [&](const fs::MountSnapshot &row) {
        return dispatch(true, arguments,
            [&](u32 slot, fs::MountSnapshot &captured) { CHECK(slot == 1); ++queried; captured = row; return query_status; },
            [&](u64 address, usize size) { CHECK(address == 0x1234 && size == 288); ++ranged; return range_ok; },
            [&](u64 address, std::span<const u8> bytes) { CHECK(address == 0x1234 && bytes.size() == 288); ++copied;
                for (usize index = 0; index < bytes.size(); ++index) output[index] = bytes[index];
                return copy_ok;
            });
    };
    CHECK(call(snapshot) == 0 && queried == 1 && ranged == 1 && copied == 1);
    for (usize index = 33; index < output.size(); ++index) CHECK(output[index] == 0);
    query_status = Status::busy("injected active namespace operation");
    CHECK(call(snapshot) == -16 && queried == 2 && copied == 1);
    query_status = Status::invalid_argument("injected selector refusal"); CHECK(call(snapshot) == -22 && copied == 1);
    query_status = Status::success(); range_ok = false;
    const auto queries = queried; CHECK(call(snapshot) == -14 && queried == queries && copied == 1);
    range_ok = true; copy_ok = false; CHECK(call(snapshot) == -14 && copied == 2); copy_ok = true;
    const auto copies = copied;
    for (usize fault = 0; fault < 6; ++fault)
    {
        auto malformed = snapshot;
        if (fault == 0) malformed.generation = 0;
        if (fault == 1) malformed.generation = 18;
        if (fault == 2) malformed.slot = 2;
        if (fault == 3) malformed.parent_id = 9;
        if (fault == 4) malformed.state = static_cast<fs::MountSnapshotState>(6);
        if (fault == 5) malformed.path_length = 256;
        CHECK(call(malformed) == -5 && copied == copies);
    }
    snapshot.path_length = 255;
    CHECK(call(snapshot) == 0 && output[287] == 0);
    const std::array<u32, 6> narrow{1, 0x1234, sizeof(ok_mount_info)};
    CHECK(dispatch(true, narrow, [&](u32, fs::MountSnapshot &row) { row = snapshot; return Status{}; },
                   [](u32, usize) { return true; }, [](u32, std::span<const u8>) { return true; }) == 0);
}
}
int main()
{
    static_assert(OK_UAPI_VERSION_MINOR == 16 && sizeof(ok_mount_info) == 288);
    static_assert(OK_SYS_MOUNT_INFO == static_cast<u64>(N::mount_info));
    static Kernel kernel; CHECK(kernel.boot({}));
    const auto context = kernel.arch().make_user_context({hw::user_begin, hw::user_end - 4096});
    auto pid = kernel.scheduler().create_user_process("mount-info-model", context); CHECK(pid);
    auto other = kernel.scheduler().create_user_process("mount-info-other", context); CHECK(other);
    CHECK(kernel.posix().activate(pid.value()));
    CHECK(!kernel.syscalls().has_handler(N::mount_info)); // Native early boundary only.
    actual_boundary(kernel, pid.value(), other.value());
    topology(kernel, pid.value());
    injected_controls();
    std::puts("NATIVE_MOUNT_INFO_MODEL_PASS caller_identity=1 native_only=1 fixed_wire=288 checked_copy=1 slots=9 topology=1 guest=0");
}
