set_project("ObfuscationKernel")
set_version("0.1.0")
set_xmakever("3.0.0")

add_rules("mode.debug", "mode.release")
set_languages("c++23")

option("arch_target")
    set_default("host")
    set_showmenu(true)
    set_values("host", "i386", "x86_64", "aarch64", "arm32", "rv64", "rv32", "loongarch64")
    set_description("Kernel architecture profile used for compile-time traits")
option_end()

local arch_defines = {
    i386 = "OK_ARCH_TARGET_I386",
    x86_64 = "OK_ARCH_TARGET_X86_64",
    aarch64 = "OK_ARCH_TARGET_AARCH64",
    arm32 = "OK_ARCH_TARGET_ARM32",
    rv64 = "OK_ARCH_TARGET_RV64",
    rv32 = "OK_ARCH_TARGET_RV32",
    loongarch64 = "OK_ARCH_TARGET_LOONGARCH64",
}

local toolchain_specs = {
    {"ok-i386-elf", "i386-elf"},
    {"ok-x86_64-elf", "x86_64-elf"},
    {"ok-aarch64-elf", "aarch64-elf"},
    {"ok-arm32-elf", "arm-none-eabi"},
    {"ok-rv64-elf", "riscv64-elf"},
    {"ok-rv32-elf", "riscv32-elf"},
    {"ok-loongarch64-elf", "loongarch64-elf"},
}

local linux_user_toolchain_specs = {
    {"ok-i386-linux", "i686-linux-gnu"},
    {"ok-aarch64-linux", "aarch64-linux-gnu"},
    {"ok-arm32-linux", "arm-linux-gnueabihf"},
    {"ok-rv64-linux", "riscv64-linux-gnu"},
}

for _, spec in ipairs(toolchain_specs) do
    toolchain(spec[1])
        set_kind("standalone")
        set_sdkdir(path.join(os.projectdir(), "toolchains", spec[2]))
        set_toolset("cc", spec[2] .. "-gcc")
        set_toolset("cxx", spec[2] .. "-g++")
        set_toolset("as", spec[2] .. "-as")
        set_toolset("ld", spec[2] .. "-ld")
        set_toolset("ar", spec[2] .. "-ar")
        set_toolset("strip", spec[2] .. "-strip")
    toolchain_end()
end

for _, spec in ipairs(linux_user_toolchain_specs) do
    toolchain(spec[1])
        set_kind("standalone")
        set_toolset("cc", spec[2] .. "-gcc")
        set_toolset("cxx", spec[2] .. "-g++")
        set_toolset("as", spec[2] .. "-as")
        set_toolset("ld", spec[2] .. "-ld")
        set_toolset("ar", spec[2] .. "-ar")
        set_toolset("strip", spec[2] .. "-strip")
    toolchain_end()
end

local function add_arch_profile()
    local arch = get_config("arch_target") or "host"
    if arch_defines[arch] then
        add_defines(arch_defines[arch])
    end
end

target("okernel")
    set_kind("static")
    set_languages("c++23")
    add_files("src/**/*.cpp")
    add_includedirs("include", {public = true})
    add_cxxflags("-frtti", "-Wall", "-Wextra", "-Wpedantic", {tools = {"gcc", "clang"}})
    add_arch_profile()
target_end()

target("qemu_smoke")
    set_kind("binary")
    set_languages("c++23")
    add_deps("okernel")
    add_files("tests/qemu/smoke_main.cpp")
    add_includedirs("include")
    add_arch_profile()
    on_run(function (target)
        os.execv("python3", {
            path.join(os.projectdir(), "scripts", "qemu_smoke.py"),
            "--arch", get_config("arch_target") or "host",
            "--binary", target:targetfile()
        })
    end)
target_end()

task("toolchains")
    set_menu {
        usage = "xmake toolchains [--arch ARCH]",
        description = "Build GCC/binutils cross toolchains into ./toolchains",
        options = {
            {"a", "arch", "kv", "all", "Architecture: i386/x86_64/aarch64/arm32/rv64/rv32/loongarch64/all"},
            {"j", "jobs", "kv", nil, "Parallel build jobs"}
        }
    }
    on_run(function ()
        import("core.base.option")
        local argv = {path.join(os.projectdir(), "scripts", "build-toolchain.sh"), "--arch", option.get("arch") or "all"}
        if option.get("jobs") then
            table.insert(argv, "--jobs")
            table.insert(argv, option.get("jobs"))
        end
        os.execv("bash", argv)
    end)
task_end()

task("qemu-test")
    set_menu {
        usage = "xmake qemu-test",
        description = "Build and run the qemu_smoke target for the configured architecture"
    }
    on_run(function ()
        os.execv("xmake", {"build", "qemu_smoke"})
        os.execv("xmake", {"run", "qemu_smoke"})
    end)
task_end()
