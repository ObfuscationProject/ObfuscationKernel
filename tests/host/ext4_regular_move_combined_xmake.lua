-- Two fresh models; retain only the exact authenticated ordinary runtime.
set_project("ext4-regular-move-combined-models")
set_languages("c++23")
add_rules("mode.release")
local kernel = os.getenv("OK_REGULAR_MOVE_KERNEL_SOURCE")
local archive = os.getenv("OK_REGULAR_MOVE_RETAINED_RUNTIME")
if not kernel or not archive then raise("exact private source and retained runtime required") end
toolchain("regular-move-combined-host-gcc")
    set_kind("standalone")
    set_toolset("cxx", "/usr/bin/g++")
    set_toolset("ld", "/usr/bin/g++")
    set_toolset("cc", "/usr/bin/gcc")
    set_toolset("ar", "/usr/bin/ar")
toolchain_end()
set_toolchains("regular-move-combined-host-gcc")
for _, model in ipairs({
    {"owned-regular-move-model", "ext4_owned_regular_move_tests.cpp"},
    {"cross-parent-regular-move-mounted-model", "ext4_regular_move_mounted_tests.cpp"}
}) do
    target(model[1])
        set_kind("binary")
        add_defines("OK_ARCH_TARGET_X86_64")
        add_includedirs(path.join(kernel, "include"), path.join(kernel, "tests/host"))
        add_files(path.join(kernel, "tests/host", model[2]))
        add_cxxflags("-O2", "-Wall", "-Wextra", "-Werror", "-fno-exceptions", "-frtti", "-fno-pie", "-ffunction-sections", "-fdata-sections", {force=true})
        add_ldflags(archive, "-no-pie", "-Wl,--gc-sections", "-Wl,--no-as-needed", "-Wl,-l:libext2fs.so.2", {force=true})
        add_syslinks("pthread")
    target_end()
end
