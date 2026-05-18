set_project("ObfuscationKernel")
set_version("0.1.0")
set_xmakever("3.0.0")

add_rules("mode.debug", "mode.release")
set_languages("c++23")

includes("xmake/arch.lua")
includes("xmake/toolchains.lua")

target("okernel")
    set_kind("static")
    set_languages("c++23")
    add_files("src/**/*.cpp")
    add_includedirs("include", {public = true})
    add_cxxflags("-frtti", "-Wall", "-Wextra", "-Wpedantic", {tools = {"gcc", "clang"}})
    add_ok_arch_profile()
    add_ok_debug_test_points()
target_end()

target("qemu_smoke")
    set_kind("binary")
    set_languages("c++23")
    add_deps("okernel")
    add_files("tests/qemu/smoke_main.cpp")
    add_includedirs("include")
    add_ok_arch_profile()
    add_ok_debug_test_points()
    on_run(function (target)
        os.execv("python3", {
            path.join(os.projectdir(), "scripts", "qemu_smoke.py"),
            "--arch", get_config("arch_target") or "host",
            "--binary", target:targetfile()
        })
    end)
target_end()

includes("xmake/tasks.lua")
