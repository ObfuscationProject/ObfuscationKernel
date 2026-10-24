// Actual power dispatch/coordinator, with hosted trap/VM/storage backend seams.
#define OK_SHUTDOWN_RUNTIME_MODEL 1
#define OK_SYSCALL_ORIGIN_MODEL 1
#define OK_NATIVE_POWER_MODEL 1
#define OK_NATIVE_SIGNAL_RUNTIME 1
#define OK_ENABLE_TEST_POINTS 1
#include "native_clock_model.hpp"
#include "ok/core/shutdown_storage.hpp"
#include "native_power_backend_model.hpp"
#include <csetjmp>
#include <string_view>

namespace
{
Status storage_status{};
usize storage_preflights{}, storage_seals{};
SystemPowerAction sealed_action{};
Kernel *boundary_kernel{};
Status boundary_status{};
SystemPowerAction boundary_action{};
std::jmp_buf boundary_escape;

void advance(Kernel &kernel)
{
    boundary_kernel = &kernel;
    if (setjmp(boundary_escape) == 0) ok::detail::desktop_loop(false);
}
void setup(Kernel &kernel)
{
    CHECK(kernel.boot({}));
    CHECK(native::activate_pool());
    CHECK(native::shared_files.bind(native::page_pool));
    CHECK(kernel.scheduler().set_shared_memory_files(native::shared_files));
    CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    auto made = kernel.scheduler().create_init_process(kernel.arch().make_user_context({hw::user_begin, hw::user_begin + 4096}));
    CHECK(made && made.value() == 1);
    auto &process = *kernel.scheduler().find(1);
    process.set_credentials(user::root_credentials());
    process.resources().native_space = {0x100000, 0, 1};
    CHECK(native::allocate(process.resources().native_pages, hw::user_begin, hw::page_read | hw::page_execute));
    auto &thread = process.threads()[0];
    CHECK(native::prepare_native_thread(thread));
    thread.native_active = true;
    CHECK(native::private_signal_process(kernel, 1));
    CHECK(native::private_signal_queue(thread));
    CHECK(kernel.scheduler().set_runnable(1));
    CHECK(kernel.scheduler().activate_thread(1, thread.tid));
    CHECK(kernel.posix().activate(1));
    native::runtime_kernel = &kernel;
    native::started = true;
    native::original_shutdown_owner = {&native::original_shutdown_owner, 1, 1, thread.tid};
    mock::shutdown_backend_owners = 1;
}
i64 invoke(Kernel &kernel, syscall::Number number, uptr value, usize reserved = 0)
{
    hw::Syscall call{static_cast<uptr>(number), {value, 0, 0, 0, 0, 0}};
    if (reserved) call.arguments[reserved] = 1;
    bool complete = true;
    const auto result = native::dispatch(kernel, kernel.scheduler().current_pid(), call, complete);
    CHECK(complete);
    return result;
}
i64 prepare(Kernel &kernel, uptr action = OK_POWER_HALT)
{ return invoke(kernel, syscall::Number::power_prepare, action); }
i64 commit(Kernel &kernel, uptr token = 1)
{ return invoke(kernel, syscall::Number::power_commit, token); }
void unchanged(Kernel &kernel, usize preflights)
{
    const auto report = native::shutdown_report(kernel);
    CHECK(!native_shutdown_pending() && !native::shutdown_admission_closed);
    CHECK(!report.retirement_started && report.native_threads == 1 && report.address_spaces == 1);
    CHECK(report.leased_entries == 1 && mock::shutdown_backend_owners == 1);
    CHECK(storage_preflights == preflights && !storage_seals && !mock::entries && !mock::copies && !mock::outputs);
}
void wire(Kernel &kernel)
{
    CHECK(commit(kernel) == -22); // No public reservation, even for original PID1.
    for (const auto number : {syscall::Number::power_prepare, syscall::Number::power_commit})
    {
        CHECK(invoke(kernel, number, 0) == -22);
        CHECK(invoke(kernel, number, OK_POWER_TOKEN_MAX + uptr{1}) == -22);
        for (usize index = 1; index < 6; ++index) CHECK(invoke(kernel, number, 1, index) == -22);
        if constexpr (sizeof(uptr) == 8)
            CHECK(invoke(kernel, number, static_cast<uptr>(u64{1} << 32) | 1) == -22);
    }
    CHECK(prepare(kernel, 4) == -22);
    CHECK(prepare(kernel, OK_POWER_REBOOT) == -95);
    CHECK(prepare(kernel, OK_POWER_POWEROFF) == -95);
    auto &process = *kernel.scheduler().find(1);
    process.resources().personality = sched::UserPersonality::linux;
    CHECK(prepare(kernel) == -38 && commit(kernel) == -38);
    process.resources().personality = sched::UserPersonality::native;
    unchanged(kernel, 0);
}
void credentials(Kernel &kernel, usize expected_preflights, uptr action = OK_POWER_HALT)
{
    auto &process = *kernel.scheduler().find(1);
    for (unsigned index = 0; index < 7; ++index)
    {
        auto ids = user::root_credentials();
        if (index == 0) ids.uid = 1;
        if (index == 1) ids.euid = 1;
        if (index == 2) ids.suid = 1;
        if (index == 3) ids.gid = 1;
        if (index == 4) ids.egid = 1;
        if (index == 5) ids.sgid = 1;
        if (index == 6) ids.kernel_space = true;
        process.set_credentials(ids);
        CHECK(prepare(kernel, action) == -1 && commit(kernel) == -1);
        // Identity is checked even on a malformed native request.
        CHECK(invoke(kernel, syscall::Number::power_commit, 0, 5) == -1);
    }
    process.set_credentials(user::root_credentials());
    process.set_parent(7); CHECK(prepare(kernel, action) == -1 && commit(kernel) == -1); process.set_parent(0);
    auto peer = kernel.scheduler().create_thread(1, process.threads()[0].context); CHECK(peer);
    CHECK(prepare(kernel, action) == -1 && commit(kernel) == -1);
    CHECK(kernel.scheduler().set_runnable(1));
    CHECK(kernel.scheduler().activate_thread(1, peer.value()));
    CHECK(prepare(kernel, action) == -1 && commit(kernel) == -1);
    CHECK(kernel.scheduler().exit_thread(1, peer.value(), 0));
    CHECK(kernel.scheduler().activate_thread(1, process.threads()[0].tid));
    unchanged(kernel, expected_preflights);
}
void policy(Kernel &kernel)
{
    credentials(kernel, 0);
    auto *process = kernel.scheduler().find(1);
    const auto leader = process->threads()[0].tid;
    auto made = kernel.scheduler().create_user_process("service", process->threads()[0].context); CHECK(made);
    const auto child = made.value();
    auto *service = kernel.scheduler().find(child);
    service->set_parent(1); CHECK(kernel.scheduler().find(1)->add_child(child));
    service->set_credentials(user::root_credentials());
    CHECK(kernel.scheduler().set_runnable(child));
    CHECK(kernel.scheduler().activate_thread(child, service->threads()[0].tid));
    CHECK(prepare(kernel) == -1 && commit(kernel) == -1); // Root child is not original init.
    CHECK(kernel.scheduler().activate_thread(1, leader));
    CHECK(prepare(kernel) == 1); // Reservation precedes retirement of a live service.
    CHECK(storage_preflights == 1 && prepare(kernel) == 1 && storage_preflights == 1);
    CHECK(prepare(kernel, OK_POWER_REBOOT) == -16);
    CHECK(commit(kernel, 2) == -22 && commit(kernel) == -16);
    ++native::original_shutdown_owner.incarnation;
    CHECK(prepare(kernel) == -1 && commit(kernel) == -1);
    --native::original_shutdown_owner.incarnation;
    credentials(kernel, 1);
    CHECK(kernel.scheduler().exit_process(child, 0));
    CHECK(commit(kernel) == -16); // A zombie is still an unreaped original-init child.
    CHECK(kernel.scheduler().wait_process(1, child));
    CHECK(kernel.scheduler().find(1)->children().empty());
    CHECK(commit(kernel) == 0 && native_shutdown_pending());
    const auto report = native::shutdown_report(kernel);
    CHECK(!report.retirement_started && report.address_spaces == 1 && report.leased_entries == 1);
    CHECK(!storage_seals && !native::shutdown_admission_closed);
}
void preflight(Kernel &kernel)
{
    for (const auto status : {Status::unsupported("hosted inventory refusal"),
                            Status::busy("hosted preflight refusal"), Status::io_error("hosted invariant refusal")})
    {
        storage_status = status;
        const auto expected = status.code() == StatusCode::unsupported ? -95 : status.code() == StatusCode::busy ? -16 : -5;
        CHECK(prepare(kernel) == expected);
        CHECK(commit(kernel) == -22);
        unchanged(kernel, storage_preflights);
    }
    storage_status = {};
    CHECK(prepare(kernel) == 1 && storage_preflights == 4);
    CHECK(!native_shutdown_pending());
}
void pending(Kernel &kernel)
{
    CHECK(prepare(kernel) == 1);
    auto &thread = kernel.scheduler().find(1)->threads()[0];
    thread.syscall_pending = true;
    thread.native_frame.registers[14] = OK_SYS_POWER_COMMIT;
    thread.native_frame.registers[9] = 1;
    CHECK(!native::step(kernel));
    CHECK(!thread.syscall_pending && thread.native_frame.registers[0] == 0);
    CHECK(native_shutdown_pending() && !native::runtime_step_active);
    CHECK(native::shutdown_outer_boundary(kernel));
    const auto before = native::shutdown_report(kernel);
    CHECK(!before.retirement_started && before.address_spaces == 1 && before.leased_entries == 1);
    CHECK(!native::shutdown_admission_closed && !storage_seals && !mock::entries);
    CHECK(!native::step(kernel) && !mock::entries);
}
void console(Kernel &kernel)
{
    CHECK(kernel.scheduler().shutdown_console_status());
    auto *init = kernel.scheduler().find(1);
    auto made = kernel.scheduler().create_user_process("leased-service", init->threads()[0].context); CHECK(made);
    const auto child = made.value();
    kernel.scheduler().find(child)->set_parent(1); CHECK(kernel.scheduler().find(1)->add_child(child));
    auto epoch = kernel.scheduler().claim_console(child); CHECK(epoch);
    CHECK(!kernel.scheduler().shutdown_console_status());
    kernel.scheduler().console_input('s');
    CHECK(kernel.scheduler().console_readable(child, epoch.value()));
    CHECK(!kernel.scheduler().shutdown_console_status());
    CHECK(prepare(kernel) == 1); // PREPARE must precede service and console retirement.
    CHECK(commit(kernel) == -16);
    CHECK(kernel.scheduler().revoke_console(epoch.value()));
    CHECK(kernel.scheduler().shutdown_console_status());
    CHECK(!kernel.scheduler().console_readable(child, epoch.value()));
    kernel.scheduler().console_input('x'); // Closed admission to this lease drops input.
    CHECK(kernel.scheduler().shutdown_console_status());
    CHECK(commit(kernel) == -16); // Console retirement does not replace child reap.
    CHECK(kernel.scheduler().exit_process(child, 0));
    CHECK(kernel.scheduler().wait_process(1, child));
    CHECK(kernel.scheduler().shutdown_console_status());
    CHECK(commit(kernel) == 0 && native_shutdown_pending() && !storage_seals);
}
void control_references(Kernel &kernel)
{
    auto opened = kernel.posix().unix_socket(posix::o_NONBLOCK); CHECK(opened);
    const auto listener = opened.value();
    auto &resources = kernel.scheduler().find(1)->resources();
    const auto listener_id = resources.files.entry(listener)->description;
    CHECK(kernel.posix().bind_unix_description(listener_id, "/tmp/native-power-control.sock"));
    CHECK(kernel.posix().listen_unix_description(listener_id, 4));
    CHECK(kernel.vfs().socket_path_reference_count() == 1 && kernel.scheduler().unix_endpoint_count() == 1);
    auto duplicate = kernel.posix().duplicate(listener); CHECK(duplicate);
    auto client = kernel.posix().unix_socket(posix::o_NONBLOCK); CHECK(client);
    auto connected = kernel.posix().connect_unix_description(resources.files.entry(client.value())->description,
                                                           "/tmp/native-power-control.sock");
    CHECK(connected && connected.value());
    CHECK(kernel.scheduler().unix_endpoint_count() == 3);
    CHECK(prepare(kernel) == 1); // Valid endpoint owners are still live during reservation.
    CHECK(commit(kernel) == -16);
    auto accepted = kernel.posix().prepare_unix_accept(listener_id, posix::o_NONBLOCK); CHECK(accepted);
    CHECK(!kernel.scheduler().shutdown_endpoint_status() && commit(kernel) == -16);
    CHECK(kernel.posix().close(listener));
    CHECK(kernel.vfs().socket_path_reference_count() == 1 && kernel.scheduler().unix_endpoint_count() == 3);
    CHECK(kernel.posix().close(duplicate.value()));
    CHECK(kernel.vfs().socket_path_reference_count() == 1 && kernel.scheduler().unix_endpoint_count() == 3);
    CHECK(kernel.posix().close(client.value()));
    CHECK(!kernel.scheduler().shutdown_endpoint_status() && commit(kernel) == -16);
    accepted.value().cancel();
    CHECK(kernel.scheduler().unix_endpoint_count() == 0 && !kernel.vfs().socket_path_reference_count());
    CHECK(kernel.scheduler().shutdown_endpoint_status());
    auto retained = kernel.vfs().resolve_socket({}, "/tmp/native-power-control.sock"); CHECK(retained);
    CHECK(kernel.vfs().socket_path_reference_count() == 1 && commit(kernel) == -16);
    CHECK(retained.value().release());
    // Closing the final OFD releases the binding and pending queue. A linked
    // socket filename remains harmless and does not require unlink policy.
    auto linked = kernel.vfs().resolve({}, "/tmp/native-power-control.sock"); CHECK(linked);
    CHECK(linked.value().node()->metadata().type == fs::NodeType::socket);
    CHECK(linked.value().release());
    CHECK(commit(kernel) == 0 && native_shutdown_pending() && !storage_seals);
}
void reservations(Kernel &kernel)
{
    auto operation = kernel.scheduler().prepare_descriptor(1); CHECK(operation);
    CHECK(!kernel.scheduler().shutdown_endpoint_status());
    CHECK(!kernel.scheduler().unix_endpoint_count() && !kernel.vfs().socket_path_reference_count());
    CHECK(prepare(kernel) == 1 && commit(kernel) == -16);
    unchanged(kernel, 1);
    CHECK(operation.value().cancel());
    CHECK(kernel.scheduler().shutdown_endpoint_status());
    CHECK(commit(kernel) == 0 && native_shutdown_pending() && !storage_seals);
}

void clean_action(Kernel &kernel, u32 action)
{
    arch::hosted_power::reboot_enabled = true;
    credentials(kernel, 0, action);
    const auto selected = action == OK_POWER_REBOOT ? SystemPowerAction::reboot : SystemPowerAction::halt;
    const auto other = action == OK_POWER_REBOOT ? OK_POWER_HALT : OK_POWER_REBOOT;
    CHECK(prepare(kernel, action) == 1);
    CHECK(storage_preflights == 1 && arch::hosted_power::preflights == 1);
    CHECK(prepare(kernel, action) == 1 && prepare(kernel, other) == -16);
    CHECK(storage_preflights == 1 && arch::hosted_power::preflights == 1);
    CHECK(commit(kernel) == 0 && native_shutdown_pending() && !storage_seals);
    advance(kernel);
    CHECK(boundary_status && boundary_action == selected);
    CHECK(storage_preflights == 2 && storage_seals == 1 && sealed_action == selected);
    CHECK(arch::hosted_power::preflights == 2 && arch::hosted_power::interrupt_closures == 1);
    CHECK(native::shutdown_report(kernel).retirement_completed && !mock::entries);
}

void reboot_preflight(Kernel &kernel)
{
    arch::hosted_power::reboot_enabled = true;
    arch::hosted_power::reboot_status = Status::busy("controller busy");
    CHECK(prepare(kernel, OK_POWER_REBOOT) == -16 && commit(kernel) == -22);
    unchanged(kernel, 0);
    arch::hosted_power::reboot_status = Status::unsupported("controller absent");
    CHECK(prepare(kernel, OK_POWER_REBOOT) == -95 && commit(kernel) == -22);
    unchanged(kernel, 0);
    arch::hosted_power::reboot_status = {};
    CHECK(prepare(kernel, OK_POWER_REBOOT) == 1 && commit(kernel) == 0);
    arch::hosted_power::reboot_status = Status::busy("controller changed after reservation");
    advance(kernel);
    CHECK(boundary_status.code() == StatusCode::busy && boundary_action == SystemPowerAction::none);
    CHECK(native_shutdown_pending() && !storage_seals && storage_preflights == 1);
    CHECK(!arch::hosted_power::interrupt_closures && !native::shutdown_report(kernel).retirement_started);
    const auto before = arch::hosted_power::preflights;
    advance(kernel);
    CHECK(boundary_status.code() == StatusCode::busy && arch::hosted_power::preflights == before);
}
} // namespace

namespace ok::detail
{
[[noreturn]] void desktop_loop(bool)
{
    NativeShutdownBoundary boundary;
    auto result = advance_native_shutdown(*boundary_kernel, boundary);
    boundary_status = result ? Status{} : result.status();
    boundary_action = result ? result.value() : SystemPowerAction::none;
    std::longjmp(boundary_escape, 1);
}
} // namespace ok::detail

extern "C" ok::Status ok_shutdown_storage_preflight(ok::fs::VirtualFileSystem &, ok::SystemPowerAction action)
{
    CHECK(action == SystemPowerAction::halt || action == SystemPowerAction::reboot);
    ++storage_preflights;
    return storage_status;
}
extern "C" ok::Status ok_shutdown_storage_seal(ok::fs::VirtualFileSystem &, const ok::ShutdownStorageBarrier &barrier)
{
    ++storage_seals;
    sealed_action = barrier.ticket().action;
    return {};
}
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    static Kernel kernel;
    setup(kernel);
    const std::string_view mode{argv[1]};
    if (mode == "wire") wire(kernel);
    else if (mode == "policy") policy(kernel);
    else if (mode == "preflight") preflight(kernel);
    else if (mode == "pending") pending(kernel);
    else if (mode == "console") console(kernel);
    else if (mode == "control") control_references(kernel);
    else if (mode == "reservations") reservations(kernel);
    else if (mode == "reboot") clean_action(kernel, OK_POWER_REBOOT);
    else if (mode == "halt") clean_action(kernel, OK_POWER_HALT);
    else if (mode == "reboot-preflight") reboot_preflight(kernel);
    else CHECK(false);
    std::printf("NATIVE_POWER_DISPATCH_PASS mode=%s hosted=1 backend_mock=1 guest=0 isolation=0\n", argv[1]);
}
