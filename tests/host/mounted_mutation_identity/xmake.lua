set_project("ObfuscationKernel authentic mounted mutation hosted models")
set_languages("c++23")
add_rules("mode.debug", "mode.release")
add_defines("OK_ARCH_TARGET_X86_64")
option("sanitizers")
    set_default(false)
    set_showmenu(true)
option_end()
if has_config("sanitizers") then
    add_cxxflags("-fsanitize=address,undefined", "-fno-omit-frame-pointer", {force = true})
    add_ldflags("-fsanitize=address,undefined", {force = true})
end
for _, suite in ipairs({"scope", "leaf"}) do
    target("direct-read-" .. suite .. "-model")
        set_kind("binary")
        add_files("../ext4_direct_read_" .. suite .. "_tests.cpp")
        add_deps("mounted-model-runtime")
        add_includedirs("../../../include")
        add_links("pthread")
        add_ldflags("-no-pie", "-Wl,--gc-sections", "/usr/lib/x86_64-linux-gnu/libext2fs.so.2", {force = true})
        add_cxxflags("-fno-exceptions", "-frtti", "-fno-access-control", "-O2", "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra", "-Werror", {force = true})
    target_end()
end
target("mounted-model-runtime")
    set_kind("static")
    add_files("../../../src/fs/ext4_recovery.cpp", "../../../src/memory/memory.cpp", "../../../src/net/native_udp.cpp",
        "../../../src/sched/scheduler.cpp", "../../../src/sched/process.cpp", "../../../src/posix/posix.cpp",
        "../../../src/posix/fd.cpp", "../../../src/posix/path.cpp", "../../../src/posix/process.cpp",
        "../../../src/fs/vfs.cpp", "../../../src/driver/driver.cpp", "../../../src/smp/smp.cpp",
        "../../../src/arch/arch.cpp", "../../../src/arch/x86_64/ops.cpp", "../../../src/syscall/linux.cpp",
        "../ext4_realtime_model.cpp")
    add_includedirs("../../../include")
    add_cxxflags("-fno-exceptions", "-frtti", "-O2", "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra", "-Werror", {force = true})
target_end()
for _, suite in ipairs({"mutation_identity", "dirty", "lifetime", "root_retention", "create", "first_data", "next_data", "legacy_startup"}) do
    target("mounted-" .. suite .. "-model")
        set_kind("binary")
        add_files("../ext4_mounted_" .. suite .. "_tests.cpp")
        add_deps("mounted-model-runtime")
        add_includedirs("../../../include")
        add_links("pthread")
        add_ldflags("-no-pie", "-Wl,--gc-sections", "/usr/lib/x86_64-linux-gnu/libext2fs.so.2", {force = true})
        add_cxxflags("-fno-exceptions", "-frtti", "-fno-access-control", "-O2", "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra", "-Werror", {force = true})
    target_end()
end
target("mounted-mutation-identity-freestanding-compile")
    set_kind("static")
    add_files("../ext4_mounted_mutation_identity_compile.cpp")
    add_includedirs("../../../include")
    add_cxxflags("-ffreestanding", "-fno-exceptions", "-fno-stack-protector", "-fno-use-cxa-atexit", "-fno-threadsafe-statics", "-frtti", "-O2", "-Wall", "-Wextra", "-Werror", "-fstack-usage", {force = true})
target_end()
