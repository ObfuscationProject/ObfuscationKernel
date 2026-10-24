// Actual native retirement/identity code with hosted backend ownership controls.
#define OK_SHUTDOWN_RUNTIME_MODEL 1
#define OK_SYSCALL_ORIGIN_MODEL 1
#define OK_NATIVE_SIGNAL_RUNTIME 1
#define OK_ENABLE_TEST_POINTS 1
#include "native_clock_model.hpp"
#include <string_view>

namespace ok::user::native
{
struct SyscallOriginTestAccess
{
    static void inject_borrow(SyscallOrigins &table, const sched::ThreadControlBlock &thread)
    { table.records_[thread.entry_stack.slot].linux_udp_borrows = 1; }
};
}
namespace
{
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
void authority(Kernel &kernel)
{
    const auto owner = native::shutdown_owner(kernel); CHECK(owner);
    CHECK(native::shutdown_caller(kernel));
    CHECK(native::shutdown_preflight(kernel, owner.value()));
    auto stale = owner.value(); ++stale.incarnation;
    CHECK(!native::shutdown_preflight(kernel, stale));
    auto *process = kernel.scheduler().find(1);
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
        process->set_credentials(ids);
        CHECK(!native::shutdown_caller(kernel));
        CHECK(!native::shutdown_preflight(kernel, owner.value()));
    }
    process->set_credentials(user::root_credentials());
    process->set_parent(9); CHECK(!native::shutdown_caller(kernel)); process->set_parent(0);
    process->resources().personality = sched::UserPersonality::linux;
    CHECK(!native::shutdown_caller(kernel)); process->resources().personality = sched::UserPersonality::native;
    auto child = kernel.scheduler().create_user_process("live-service", process->threads()[0].context); CHECK(child);
    kernel.scheduler().find(child.value())->set_parent(1);
    CHECK(process->add_child(child.value()));
    CHECK(kernel.scheduler().shutdown_console_status());
    const auto console = kernel.scheduler().claim_console(child.value()); CHECK(console);
    kernel.scheduler().console_input('s');
    CHECK(kernel.scheduler().shutdown_console_status().code() == StatusCode::busy);
    CHECK(native::shutdown_preflight(kernel, owner.value()));
    CHECK(!native::shutdown_check_commit(kernel, owner.value()));
    CHECK(kernel.scheduler().exit_process(child.value(), 0));
    CHECK(kernel.scheduler().shutdown_console_status());
    CHECK(!native::shutdown_check_commit(kernel, owner.value()));
    CHECK(kernel.scheduler().wait_process(1, child.value()));
    CHECK(kernel.scheduler().find(1)->children().empty());
    CHECK(native::shutdown_check_commit(kernel, owner.value()));
    process = kernel.scheduler().find(1);
    auto peer = kernel.scheduler().create_thread(1, process->threads()[0].context); CHECK(peer);
    CHECK(!native::shutdown_caller(kernel)); CHECK(!native::shutdown_check_commit(kernel, owner.value()));
    CHECK(kernel.scheduler().exit_thread(1, peer.value(), 0));
    CHECK(native::shutdown_caller(kernel));
    native::runtime_step_active = true;
    CHECK(!native::shutdown_outer_boundary(kernel));
    CHECK(!native::shutdown_close_admission(kernel));
    native::runtime_step_active = false;
    CHECK(native::shutdown_outer_boundary(kernel));
    mock::shutdown_backend_executing = true; CHECK(!native::shutdown_outer_boundary(kernel));
    mock::shutdown_backend_executing = false;
    const auto before = native::shutdown_report(kernel);
    const auto after = native::shutdown_report(kernel);
    CHECK(before.origin_records == after.origin_records && before.pool_pages == after.pool_pages);
    CHECK(before.leased_entries == 1 && before.backend_spaces == 1 && before.address_spaces == 1);
    CHECK(before.origin_records && before.vfs_records && before.signal_owners);
}
void retire(Kernel &kernel, std::string_view mode)
{
    if (mode == "shared")
    {
        CHECK(kernel.posix().activate(0));
        auto fd = kernel.posix().memfd_create("boot-shared-owner", posix::mfd_ALLOW_SEALING); CHECK(fd);
        CHECK(kernel.posix().ftruncate(fd.value(), 4096));
        const std::byte byte{42};
        CHECK(kernel.posix().pwrite(fd.value(), {&byte, 1}, 0));
        CHECK(kernel.posix().activate(1));
    }
    CHECK(native::shutdown_close_admission(kernel));
    const auto entries = mock::entries;
    CHECK(!native::step(kernel) && mock::entries == entries);
    CHECK(!native::start(kernel, {}));
    CHECK(!native::shutdown_caller(kernel));
    if (mode == "borrow")
    {
        native::SyscallOriginTestAccess::inject_borrow(native::syscall_origins, kernel.scheduler().find(1)->threads()[0]);
        CHECK(native::shutdown_report(kernel).origin_borrows == 1);
    }
    if (mode == "table-failure") mock::fail_destroy_root = kernel.scheduler().find(1)->resources().native_space.root;
    if (mode == "orphan") ++mock::shutdown_backend_owners;
    const auto status = native::shutdown_retire(kernel);
    if (mode == "borrow" || mode == "table-failure" || mode == "orphan")
    {
        CHECK(!status && !native::runtime_status());
        const auto report = native::shutdown_report(kernel);
        CHECK(!report.failure && report.retirement_started && !report.retirement_completed);
        CHECK(!native::shutdown_retire(kernel));
        CHECK(!native::shutdown_verify_drained(kernel));
        if (mode == "borrow") CHECK(report.leased_entries == 1 && report.address_spaces == 1);
        if (mode == "orphan") CHECK(report.backend_spaces == 1 && !report.address_spaces && !report.leased_entries);
        if (mode == "table-failure") CHECK(report.quarantined_spaces && report.pool_pages);
        return;
    }
    CHECK(status);
    const auto report = native::shutdown_report(kernel);
    CHECK(report.retirement_completed && !report.native_threads && !report.address_spaces && !report.backend_spaces);
    CHECK(!report.origin_records && !report.vfs_records && !report.signal_owners && !report.leased_entries);
    CHECK(kernel.scheduler().find(1)->state() == sched::ProcessState::zombie);
    if (mode == "shared")
    {
        CHECK(report.shared_files == 1 && report.user_pages == 1 && report.pool_pages == 1);
        CHECK(report.failure);
    }
    CHECK(kernel.scheduler().drain_all_resources());
    CHECK(native::shutdown_verify_drained(kernel));
    CHECK(!native::shutdown_retire(kernel));
    CHECK(native::shutdown_verify_drained(kernel));
}
}
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    static Kernel kernel;
    setup(kernel); authority(kernel); retire(kernel, argv[1]);
    std::printf("NATIVE_SHUTDOWN_RUNTIME_PASS mode=%s hosted=1 backend_mock=1 guest=0 isolation=0\n", argv[1]);
}
