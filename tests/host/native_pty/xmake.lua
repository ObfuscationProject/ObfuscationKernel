set_project("Native PTY actual owner and checked-boundary hosted tests")
set_languages("c++23")
add_rules("mode.debug", "mode.release")
option("sanitizers") set_default(false) set_showmenu(true) option_end()
option("runtime_tests") set_default(false) set_showmenu(true) option_end()
local kernel = path.absolute("../../..", os.projectdir())
add_includedirs(path.join(kernel, "include"), path.join(kernel, "tests/host"))
add_defines("OK_ARCH_TARGET_X86_64")
add_cxxflags("-Wall", "-Wextra", "-Werror", "-fno-exceptions", "-frtti", "-fno-pie",
             "-ffunction-sections", "-fdata-sections", {force = true})
if has_config("sanitizers") then
    add_cxxflags("-fsanitize=address,undefined", "-fno-omit-frame-pointer", {force = true})
    add_ldflags("-fsanitize=address,undefined", {force = true})
end
target("native-pty-owner-hosted-runtime")
    set_kind("static")
    for _, source in ipairs({
        "src/fs/ext4_recovery.cpp", "src/memory/memory.cpp", "src/net/native_udp.cpp",
        "src/sched/scheduler.cpp", "src/sched/process.cpp", "src/posix/posix.cpp", "src/posix/fd.cpp",
        "src/posix/path.cpp", "src/posix/process.cpp", "src/fs/vfs.cpp", "src/driver/driver.cpp",
        "src/smp/smp.cpp", "src/arch/arch.cpp", "src/arch/x86_64/ops.cpp", "src/syscall/linux.cpp",
        "tests/host/ext4_realtime_model.cpp"}) do
        add_files(path.join(kernel, source))
    end
target_end()
for _, suite in ipairs({
        {"native-pty-ofd-tests", "native_pty_ofd_tests.cpp", true},
        {"native-pty-raw-owner-tests", "raw_pty_owner_tests.cpp", false},
        {"native-pty-pair-dispatch-tests", "native_pty_pair_dispatch_tests.cpp", false},
        {"native-pty-boot-checks-tests", "native_pty_boot_checks_tests.cpp", true},
        {"native-pty-unix-rights-regressions", "unix_rights_tests.cpp", true},
        {"native-pty-unix-path-regressions", "unix_path_owner_tests.cpp", true},
        {"native-pty-shared-rights-regressions", "native_shared_file_rights_tests.cpp", true}}) do
    target(suite[1])
        set_kind("binary")
        add_files(path.join(kernel, "tests/host", suite[2]))
        add_ldflags("-no-pie", {force = true})
        if suite[3] then
            add_deps("native-pty-owner-hosted-runtime")
            add_ldflags("-Wl,--gc-sections", "-Wl,--no-as-needed", "-Wl,-l:libext2fs.so.2", {force = true})
            add_syslinks("pthread")
        end
    target_end()
end
target("native-pty-boot-runner-compile")
    set_kind("static")
    add_files(path.join(kernel, "tests/kernel/points/test_point.cpp"))
    add_defines("OK_ENABLE_TEST_POINTS=1")
    add_cxxflags("-ffreestanding", "-fno-stack-protector", "-fno-use-cxa-atexit",
                 "-fno-threadsafe-statics", {force = true})
    -- The complete existing runner instantiates an unrelated ext4 lambda whose
    -- constexpr capture is diagnosed only by Clang. Keep that warning visible.
    add_cxxflags("-Wno-error=unused-lambda-capture", {tools = "clang", force = true})
target_end()
-- The runtime suite is moved alongside these files when native runtime
-- integration is composed. Enabling this option requires its explicit source.
if has_config("runtime_tests") then
    target("native-pty-actual-runtime-hosted-library")
        set_kind("static")
        add_defines("OK_ENABLE_TEST_POINTS=1")
        -- The fixture includes runtime.cpp through native_clock_model.hpp.
        -- This separate library matches the full existing hosted native
        -- dispatch harness; it deliberately excludes the realtime model shim.
        for _, source in ipairs({
            "src/core/kernel.cpp", "src/core/kernel_syscalls.cpp", "src/core/module.cpp", "src/core/native_shutdown.cpp",
            "src/apps/file_manager.cpp", "src/apps/shell/filesystem.cpp", "src/apps/shell/network.cpp",
            "src/apps/shell/status.cpp", "src/apps/shell/storage.cpp", "src/apps/shell/task_manager.cpp",
            "src/apps/shell.cpp", "src/apps/task_manager.cpp", "src/driver/abi.cpp", "src/driver/block.cpp",
            "src/driver/bus_input.cpp", "src/driver/display.cpp", "src/driver/driver.cpp", "src/fs/archive.cpp",
            "src/fs/ext4.cpp", "src/fs/ext4_fs.cpp", "src/fs/ext4_recovery.cpp", "src/fs/fat.cpp", "src/fs/simplefs.cpp",
            "src/fs/storage.cpp", "src/fs/unix.cpp", "src/fs/vfs.cpp", "src/gui/desktop.cpp", "src/gui/gui.cpp",
            "src/interrupt/interrupt.cpp", "src/ipc/ipc.cpp", "src/memory/memory.cpp", "src/net/native_udp.cpp",
            "src/net/net.cpp", "src/posix/entropy.cpp", "src/posix/fd.cpp", "src/posix/path.cpp", "src/posix/posix.cpp",
            "src/posix/process.cpp", "src/sched/process.cpp", "src/sched/scheduler.cpp", "src/smp/preempt.cpp",
            "src/smp/smp.cpp", "src/syscall/linux.cpp", "src/syscall/syscall.cpp", "src/user/user.cpp",
            "src/arch/arch.cpp", "src/arch/terminal_power.cpp", "src/arch/x86_64/ops.cpp"}) do
            add_files(path.join(kernel, source))
        end
    target_end()
    target("native-pty-runtime-tests")
        set_kind("binary")
        add_defines("OK_ENABLE_TEST_POINTS=1")
        add_files(path.join(kernel, "tests/host/native_pty_runtime_tests.cpp"))
        add_deps("native-pty-actual-runtime-hosted-library")
        add_ldflags("-no-pie", "-Wl,--gc-sections", "-Wl,--no-as-needed", "-Wl,-l:libext2fs.so.2", {force = true})
        add_syslinks("pthread")
    target_end()
end
