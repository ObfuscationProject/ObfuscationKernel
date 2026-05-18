set_project("ObfuscationKernel")
set_version("0.1.0")
set_xmakever("3.0.0")

add_rules("mode.debug", "mode.release")
set_languages("c++23")

includes("xmake/arch.lua")
includes("xmake/toolchains.lua")

function add_ok_kernel_sources(runtime)
    add_files("src/core/*.cpp")
    if not runtime then
        remove_files("src/core/runtime.cpp")
    end
    add_files("src/driver/*.cpp")
    add_files("src/fs/*.cpp")
    add_files("src/interrupt/*.cpp")
    add_files("src/ipc/*.cpp")
    add_files("src/memory/*.cpp")
    add_files("src/sched/*.cpp")
    add_files("src/smp/*.cpp")
    add_files("src/syscall/*.cpp")
    add_files("src/user/*.cpp")
    add_ok_arch_files()
end

target("okernel")
    set_kind("static")
    set_languages("c++23")
    add_ok_freestanding_toolchain()
    add_ok_freestanding_arch_flags()
    add_ok_kernel_sources(true)
    add_includedirs("include", {public = true})
    add_cxxflags(
        "-ffreestanding",
        "-fno-exceptions",
        "-fno-stack-protector",
        "-fno-use-cxa-atexit",
        "-fno-threadsafe-statics",
        "-frtti",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        {force = true}
    )
    add_defines("OK_KERNEL_FREESTANDING")
    add_ok_arch_profile()
    add_ok_debug_test_points()
target_end()

target("qemu_smoke")
    set_kind("binary")
    set_languages("c++23")
    set_arch(os.arch())
    set_values("ok.arch", ok_current_arch())
    add_ok_kernel_sources(false)
    add_files("tests/qemu/smoke_main.cpp")
    add_includedirs("include")
    add_ok_arch_profile()
    add_ok_debug_test_points()
    add_tests("smoke")
    on_run(function (target)
        os.execv("python3", {
            path.join(os.projectdir(), "scripts", "qemu_smoke.py"),
            "--arch", target:values("ok.arch"),
            "--binary", target:targetfile(),
            "--direct"
        })
    end)
target_end()

includes("xmake/tasks.lua")
