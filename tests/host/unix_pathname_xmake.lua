-- Hosted real-implementation models. This fixture supplies no hardware evidence.
set_project("pathname-unix-hosted-models")
set_languages("c++23")
add_rules("mode.release")
option("kernel_source") set_showmenu(true) option_end()
option("captured_cxx") set_showmenu(true) option_end()
option("sanitize") set_default(false) option_end()
local kernel = assert(get_config("kernel_source"), "explicit copied kernel source required")
local captured = assert(get_config("captured_cxx"), "explicit same-command compiler capture required")
toolchain("captured-gcc")
    set_kind("standalone")
    set_toolset("cxx", captured)
    set_toolset("cc", "/usr/bin/gcc")
    set_toolset("ld", "/usr/bin/g++")
    set_toolset("ar", "/usr/bin/ar")
toolchain_end()
set_toolchains("captured-gcc")
add_defines("OK_ARCH_TARGET_X86_64")
add_includedirs(path.join(kernel, "include"))
add_cxxflags("-O2", "-Wall", "-Wextra", "-Werror", "-fno-exceptions", "-frtti", "-fno-pie", "-ffunction-sections", "-fdata-sections", {force=true})
if has_config("sanitize") then
    add_cxxflags("-fsanitize=address,undefined", "-fno-omit-frame-pointer", {force=true})
    add_ldflags("-fsanitize=address,undefined", {force=true})
end
local runtime = {
    "src/fs/ext4_recovery.cpp", "src/memory/memory.cpp", "src/net/native_udp.cpp",
    "src/sched/scheduler.cpp", "src/sched/process.cpp", "src/posix/posix.cpp", "src/posix/fd.cpp",
    "src/posix/path.cpp", "src/posix/process.cpp", "src/fs/vfs.cpp", "src/driver/driver.cpp",
    "src/smp/smp.cpp", "src/arch/arch.cpp", "src/arch/x86_64/ops.cpp", "src/syscall/linux.cpp",
    "tests/host/ext4_realtime_model.cpp"
}
target("pathname-runtime")
    set_kind("static")
    for _, source in ipairs(runtime) do add_files(path.join(kernel, source)) end
target_end()
for _, suite in ipairs({
        {"vfs-socket-path", "tests/host/vfs_socket_path_tests.cpp"},
        {"unix-path-owner", "tests/host/unix_path_owner_tests.cpp"},
        {"unix-path-syscall", "tests/host/unix_pathname_syscall_tests.cpp"}}) do
    target(suite[1] .. "-model")
        set_kind("binary")
        add_files(path.join(kernel, suite[2]))
        add_deps("pathname-runtime")
        add_ldflags("-no-pie", "-Wl,--gc-sections", "-Wl,--no-as-needed", "-Wl,-l:libext2fs.so.2", {force=true})
        add_syslinks("pthread")
    target_end()
end
