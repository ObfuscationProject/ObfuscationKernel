-- Real implementation hosted models; no native or hardware execution.
set_project("journalled-directory-create-models")
set_languages("c++23")
add_rules("mode.release")
local kernel = os.getenv("OK_MKDIR_KERNEL_SOURCE")
if not kernel then raise("exact private Kernel source required") end
toolchain("mkdir-host-gcc")
    set_kind("standalone")
    set_toolset("cxx", "/usr/bin/g++")
    set_toolset("cc", "/usr/bin/gcc")
    set_toolset("ld", "/usr/bin/g++")
    set_toolset("ar", "/usr/bin/ar")
toolchain_end()
set_toolchains("mkdir-host-gcc")
add_defines("OK_ARCH_TARGET_X86_64")
add_includedirs(path.join(kernel, "include"))
add_cxxflags("-O2", "-Wall", "-Wextra", "-Werror", "-fno-exceptions", "-frtti", "-fno-pie", "-ffunction-sections", "-fdata-sections", {force=true})
local runtime = {
    "src/fs/ext4_recovery.cpp", "src/memory/memory.cpp", "src/net/native_udp.cpp",
    "src/sched/scheduler.cpp", "src/sched/process.cpp", "src/posix/posix.cpp", "src/posix/fd.cpp",
    "src/posix/path.cpp", "src/posix/process.cpp", "src/fs/vfs.cpp", "src/driver/driver.cpp",
    "src/smp/smp.cpp", "src/arch/arch.cpp", "src/arch/x86_64/ops.cpp", "src/syscall/linux.cpp",
    "tests/host/ext4_realtime_model.cpp"
}
local retained_runtime = os.getenv("OK_MKDIR_RETAINED_RUNTIME_ARCHIVE")
if not retained_runtime then
target("mkdir-host-runtime")
    set_kind("static")
    for _, source in ipairs(runtime) do add_files(path.join(kernel, source)) end
target_end()
end
for _, suite in ipairs({
        {"regular-create-plan", "tests/host/ext4_create_plan_tests.cpp", false},
        {"directory-create-plan", "tests/host/ext4_directory_create_plan_tests.cpp", false},
        {"directory-replay-shape", "tests/host/ext4_replay_shape_tests.cpp", false},
        {"regular-owned-create", "tests/host/ext4_owned_create_tests.cpp", true},
        {"regular-mounted-create", "tests/host/ext4_mounted_create_tests.cpp", true},
        {"common-opening-budget", "tests/host/common_opening_budget/model.cpp", false},
        {"directory-replay-semantics", "tests/host/ext4_directory_create_replay_tests.cpp", true},
        {"mounted-directory-create", "tests/host/ext4_mounted_directory_create_tests.cpp", true}}) do
    target(suite[1] .. "-model")
        set_kind("binary")
        add_files(path.join(kernel, suite[2]))
        -- The legacy regular mounted fixture scopes its access shim in source.
        -- New directory models use public APIs without the historical shim.
        if suite[3] then
            if retained_runtime then add_ldflags(retained_runtime, {force=true})
            else add_deps("mkdir-host-runtime") end
        end
        add_ldflags("-no-pie", "-Wl,--gc-sections", "-Wl,--no-as-needed", "-Wl,-l:libext2fs.so.2", {force=true})
        add_syslinks("pthread")
    target_end()
end
