set_project("ObfuscationKernel")
set_version("0.1.0")

add_rules("mode.debug", "mode.release")
set_languages("c++23")

option("private_futex_peer_fixture")
    set_default(false)
    set_showmenu(true)
    set_description("Opt-in debug harness for the raw private futex peer ELF")
option_end()

option("native_pc8042_reboot")
    set_default(false)
    set_showmenu(true)
    set_description("Opt in to the bounded x86_64 PC/8042 terminal reset backend")
option_end()

option("native_qemu_piix4_poweroff")
    set_default(false)
    set_showmenu(true)
    set_description("Opt in to the bounded x86_64 QEMU PIIX4 PM terminal poweroff backend")
option_end()

option("native_cow_fork")
    set_default(true)
    set_showmenu(true)
    set_description("Share private pages copy-on-write at native fork; disable to copy them eagerly")
option_end()

option("private_signal_delivery_fixture")
    set_default(false)
    set_showmenu(true)
    set_description("Opt-in Debug diagnostics for the ordinary ten-profile native signal runtime")
option_end()

option("kernel_gui")
    set_default(false)
    set_showmenu(true)
    set_description("Enable the normal-mode kernel GUI desktop")
option_end()

option("systoolchain_dir")
    set_showmenu(true)
    set_description("Use an existing SysToolchain installation instead of resolving the package")
option_end()

option("qemu_binary")
    set_showmenu(true)
    set_description("Explicit QEMU system executable for the configured architecture")
option_end()

if not get_config("systoolchain_dir") then
    add_repositories("obfuscation-repo https://github.com/ObfuscationProject/ObfuscationRepo.git")
    add_requires("systoolchain 0.1.0", {
        system = false,
        configs = {build_from_source = false}
    })
end


includes("xmake/arch.lua")

rule("ok-native-pc8042-reboot")
    on_load(function (target)
        assert(target:values("ok.arch") == "x86_64", "native_pc8042_reboot requires x86_64")
    end)
rule_end()

local function add_ok_terminal_reboot_policy()
    if get_config("native_pc8042_reboot") then
        add_rules("ok-native-pc8042-reboot")
        add_defines("OK_NATIVE_PC8042_REBOOT=1")
    end
end

rule("ok-native-qemu-piix4-poweroff")
    on_load(function (target)
        assert(target:values("ok.arch") == "x86_64", "native_qemu_piix4_poweroff requires x86_64")
    end)
rule_end()

local function add_ok_terminal_poweroff_policy()
    if get_config("native_qemu_piix4_poweroff") then
        add_rules("ok-native-qemu-piix4-poweroff")
        add_defines("OK_NATIVE_QEMU_PIIX4_POWEROFF=1")
    end
end

local function add_ok_native_fork_policy()
    if get_config("native_cow_fork") then
        add_defines("OK_NATIVE_COW_FORK")
    end
end

local configured_arch_spec = ok_arch_spec(ok_current_arch())
local configured_toolchain_dir = get_config("systoolchain_dir")
local configured_qemu_binary = get_config("qemu_binary")
local configured_mode = get_config("mode") or "release"

local function systoolchain_qemu(target, arch)
    local spec = configured_arch_spec
    if spec == nil then
        raise("QEMU is not configured for %s", arch)
    end
    if configured_qemu_binary then
        return path.absolute(configured_qemu_binary)
    end
    if configured_toolchain_dir then
        local qemu = path.join(path.absolute(configured_toolchain_dir), "bin", spec.qemu_system)
        if not os.isfile(qemu) then
            raise("local SysToolchain is missing QEMU: %s", qemu)
        end
        return qemu
    end
    local package = target:pkg("systoolchain")
    if package == nil then
        raise("the systoolchain package is not resolved")
    end
    local qemu = path.absolute(path.join(package:installdir(), "bin", spec.qemu_system))
    if not os.isfile(qemu) then
        raise("the systoolchain package is missing QEMU: %s", qemu)
    end
    return qemu
end

function add_ok_kernel_sources(include_kernel_main)
    add_files("src/boot/raw_handoff.cpp", "src/boot/fdt.cpp", "src/boot/boot_memory.cpp")
    add_files("src/core/*.cpp")
    -- Rescue applications are diagnostics: built with kernel_gui or debug test
    -- points; other builds link stand-ins that behave as never opened.
    if has_config("kernel_gui") or is_mode("debug") then
        add_files("src/rescue/*.cpp")
        add_files("src/rescue/shell/*.cpp")
    else
        add_files("src/rescue/disabled.cpp")
        add_defines("OK_DISABLE_RESCUE_APPS")
    end
    if not include_kernel_main then
        remove_files("src/core/kernel_main.cpp")
    end
    add_files("tests/kernel/debug/*.cpp")
    add_files("tests/kernel/points/*.cpp")
    add_files("tests/kernel/roadmap/*.cpp")
    add_files("src/driver/*.cpp")
    add_files("src/fs/*.cpp")
    add_files("src/gui/*.cpp")
    add_files("src/interrupt/*.cpp")
    add_files("src/ipc/*.cpp")
    add_files("src/memory/*.cpp")
    add_files("src/net/*.cpp")
    add_files("src/posix/*.cpp")
    add_files("src/sched/*.cpp")
    add_files("src/smp/*.cpp")
    add_files("src/syscall/*.cpp")
    add_files("src/user/*.cpp")
    add_files("src/user/runtime/*.cpp")
    add_ok_arch_files()
end

local function build_kernel_image(target)
        local arch = target:values("ok.arch")
        local image_format = target:values("ok.image_format")
        assert((arch == "arm32") == (image_format == "arm32-linux-image"),
            "arm32 requires the ARM-only arm32-linux-image format")
        local kernel_bin = path.join(target:targetdir(), "kernel.bin")
        if image_format == "elf" then
            os.cp(target:targetfile(), kernel_bin)
            return
        end
        local triple = assert(target:values("ok.triple"),
            "kernel.bin tools are not configured for %s", arch)
        local cxx = assert(target:tool("cxx"), "systoolchain C++ compiler is unavailable for %s", arch)
        local gcc = path.join(path.directory(cxx), triple .. "-gcc")
        local objcopy = path.join(path.directory(cxx), triple .. "-objcopy")
        local ld = path.join(path.directory(objcopy), triple .. "-ld")
        assert(os.isfile(ld), "systoolchain linker is unavailable: %s", ld)

        local payload_bin = path.join(target:targetdir(), "kernel_payload.bin")
        os.execv(objcopy, {"-O", "binary", target:targetfile(), payload_bin})

        if image_format == "arm32-linux-image" then
            os.execv("python3", {
                path.join(os.projectdir(), "tools", "export_sdk.py"), "validate-image",
                "--arch", arch, "--image-format", image_format, "--kernel-bin", payload_bin
            })
            os.cp(payload_bin, kernel_bin)
            return
        end
        if image_format == "linux-image" then
            os.cp(payload_bin, kernel_bin)
            return
        end
        assert(image_format == "bios-disk", "unsupported kernel image format: %s", image_format)

        local autogen = path.join(target:autogendir(), "boot")
        os.mkdir(autogen)

        local boot_object = path.join(autogen, "boot16.o")
        local boot_sector = path.join(autogen, "boot16.bin")
        os.execv(gcc, {"-c", path.join(os.projectdir(), "src/arch/x86_64/boot16.S"), "-o", boot_object})
        os.execv(ld, {"-Ttext=0x7c00", "--oformat=binary", boot_object, "-o", boot_sector})
        os.cp(boot_sector, path.join(target:targetdir(), "boot16.bin"))

        local payload_size = os.filesize(payload_bin)
        local payload_capacity = 512 * 32 * 512
        if payload_size > payload_capacity then
            raise("kernel payload is too large: %d bytes > %d bytes", payload_size, payload_capacity)
        end
        os.cp(boot_sector, kernel_bin)
        os.execv("truncate", {"-s", tostring(512 + payload_capacity), kernel_bin})
        os.execv("dd", {"if=" .. payload_bin, "of=" .. kernel_bin, "bs=512", "seek=1", "conv=notrunc", "status=none"})
end

target("okernel")
    set_kind("static")
    set_languages("c++23")
    set_values("ok.arch", ok_current_arch())
    add_ok_freestanding_toolchain()
    add_ok_freestanding_arch_flags()
    add_ok_kernel_sources(false)
    add_includedirs("include", {public = true})
    add_cxxflags(
        "-ffreestanding",
        "-fno-exceptions",
        "-fno-stack-protector",
        "-fno-use-cxa-atexit",
        "-fno-threadsafe-statics",
        "-frtti",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        {force = true}
    )
    add_defines("OK_KERNEL_FREESTANDING")
    add_defines("OK_NATIVE_SIGNAL_RUNTIME")
    if ok_current_arch() == "mips64" then
        -- Signal admission precedes compiled hard-float C++ in both modes.
        add_asflags("-DOK_NATIVE_SIGNAL_RUNTIME", {force = true})
    end
    add_ok_arch_profile()
    add_ok_terminal_reboot_policy()
    add_ok_terminal_poweroff_policy()
    add_ok_native_fork_policy()
    add_ok_debug_test_points()
    if get_config("private_futex_peer_fixture") and is_mode("debug") then
        add_defines("OK_PRIVATE_FUTEX_PEER_FIXTURE")
    end
    if get_config("private_signal_delivery_fixture") then
        on_load(function (target)
            assert(is_mode("debug") and (target:values("ok.arch") == "i386" or target:values("ok.arch") == "x86_64" or target:values("ok.arch") == "aarch64" or target:values("ok.arch") == "arm32" or target:values("ok.arch") == "rv32" or target:values("ok.arch") == "rv64" or target:values("ok.arch") == "loongarch64" or target:values("ok.arch") == "mips" or target:values("ok.arch") == "mips64" or target:values("ok.arch") == "ppc"), "signal delivery fixture requires i386/x86_64/AArch64/ARM32/RV32/RV64/LoongArch64/MIPS o32/MIPS64 n64/PPC32 Debug")
        end)
        add_defines("OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE")
        if ok_current_arch() == "mips64" and is_mode("debug") then
            -- n64 admission must reach assembly before hard-float C++ boot.
            add_asflags("-DOK_PRIVATE_SIGNAL_DELIVERY_FIXTURE", "-DOK_ENABLE_TEST_POINTS", {force = true})
        end
    end
    add_ok_kernel_gui_option()
    add_tests("profile")
    on_test(function (target)
        print("freestanding profile compiled for " .. target:values("ok.arch"))
        return true
    end)
target_end()

target("okernel_image")
    set_kind("binary")
    set_languages("c++23")
    set_filename("kernel.elf")
    set_values("ok.arch", ok_current_arch())
    add_deps("okernel")
    add_ok_freestanding_toolchain()
    add_ok_freestanding_arch_flags()
    add_files("src/core/kernel_main.cpp")
    add_ok_boot_files()
    add_includedirs("include")
    add_cxxflags(
        "-ffreestanding",
        "-fno-exceptions",
        "-fno-stack-protector",
        "-fno-use-cxa-atexit",
        "-fno-threadsafe-statics",
        "-fno-pic",
        "-fno-pie",
        "-fno-asynchronous-unwind-tables",
        "-frtti",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        {force = true}
    )
    add_asflags("-ffreestanding", "-fno-pic", "-fno-pie", {force = true})
    add_ldflags(
        "-nostdlib",
        "-static",
        "-no-pie",
        "-Wl,--build-id=none",
        "-Wl,-z,max-page-size=0x1000",
        {force = true}
    )
    after_load(function (target)
        local linker_script = target:values("ok.linker_script")
        if linker_script then
            target:add("ldflags", "-Wl,-T," .. linker_script, {force = true})
        end
    end)
    add_syslinks("gcc")
    add_defines("OK_KERNEL_FREESTANDING")
    add_defines("OK_NATIVE_SIGNAL_RUNTIME")
    if ok_current_arch() == "mips64" then
        -- Signal admission precedes compiled hard-float C++ in both modes.
        add_asflags("-DOK_NATIVE_SIGNAL_RUNTIME", {force = true})
    end
    add_ok_arch_profile()
    add_ok_terminal_reboot_policy()
    add_ok_terminal_poweroff_policy()
    add_ok_native_fork_policy()
    add_ok_debug_test_points()
    if get_config("private_futex_peer_fixture") and is_mode("debug") then
        add_defines("OK_PRIVATE_FUTEX_PEER_FIXTURE")
    end
    if get_config("private_signal_delivery_fixture") then
        on_load(function (target)
            assert(is_mode("debug") and (target:values("ok.arch") == "i386" or target:values("ok.arch") == "x86_64" or target:values("ok.arch") == "aarch64" or target:values("ok.arch") == "arm32" or target:values("ok.arch") == "rv32" or target:values("ok.arch") == "rv64" or target:values("ok.arch") == "loongarch64" or target:values("ok.arch") == "mips" or target:values("ok.arch") == "mips64" or target:values("ok.arch") == "ppc"), "signal delivery fixture requires i386/x86_64/AArch64/ARM32/RV32/RV64/LoongArch64/MIPS o32/MIPS64 n64/PPC32 Debug")
        end)
        add_defines("OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE")
        if ok_current_arch() == "mips64" and is_mode("debug") then
            -- n64 admission must reach assembly before hard-float C++ boot.
            add_asflags("-DOK_PRIVATE_SIGNAL_DELIVERY_FIXTURE", "-DOK_ENABLE_TEST_POINTS", {force = true})
        end
    end
    add_ok_kernel_gui_option()
    if ok_arch_spec(ok_current_arch()).boot_source ~= nil then
        add_tests("qemu")
    end
    after_build(build_kernel_image)
    on_run(function (target)
        os.execv("python3", {
            path.join(os.projectdir(), "scripts", "qemu_test.py"),
            "--arch", target:values("ok.arch"),
            "--kernel", path.join(target:targetdir(), "kernel.bin"),
            "--qemu", systoolchain_qemu(target, target:values("ok.arch"))
        })
    end)
    on_test(function (target)
        local code = os.execv("python3", {
            path.join(os.projectdir(), "scripts", "qemu_test.py"),
            "--arch", target:values("ok.arch"),
            "--kernel", path.join(target:targetdir(), "kernel.bin"),
            "--qemu", systoolchain_qemu(target, target:values("ok.arch"))
        }, {try = true})
        return code == 0
    end)
target_end()

target("kernel-sdk")
    set_kind("phony")
    set_default(false)
    add_deps("okernel_image")
    on_build(function (target)
        import("core.project.project")
        local image = project.target("okernel_image")
        local core = project.target("okernel")
        -- Re-derive the distributable image from the exact linked ELF, even
        -- when an earlier interrupted build left an older kernel.bin behind.
        build_kernel_image(image)
        local arch = image:values("ok.arch")
        local spec = configured_arch_spec
        local mode = configured_mode
        local compiler = image:tool("cxx")
        local argv = {
            "-B",
            path.join(os.projectdir(), "tools", "export_sdk.py"),
            "--output", path.join(os.projectdir(), "build", "sdk", arch, mode),
            "--headers", path.join(os.projectdir(), "include"),
            "--library", core:targetfile(),
            "--linker-script", image:values("ok.linker_script"),
            "--kernel-bin", path.join(image:targetdir(), "kernel.bin"),
            "--kernel-elf", image:targetfile(),
            "--compiler", compiler,
            "--ar", path.join(path.directory(compiler), spec.triple .. "-ar"),
            "--arch", arch, "--mode", mode, "--triple", spec.triple,
            "--image-format", image:values("ok.image_format"),
        }
        if image:values("ok.image_format") == "bios-disk" then
            table.insert(argv, "--boot-sector")
            table.insert(argv, path.join(image:targetdir(), "boot16.bin"))
        end
        for _, object in ipairs(image:objectfiles()) do
            table.insert(argv, "--boot-object")
            table.insert(argv, object)
        end
        for _, flag in ipairs(image:get("cxxflags") or {}) do
            table.insert(argv, "--cxxflag=" .. flag)
        end
        for _, flag in ipairs(image:get("asflags") or {}) do
            table.insert(argv, "--asflag=" .. flag)
        end
        for _, flag in ipairs(spec.freestanding_ldflags or {}) do
            table.insert(argv, "--ldflag=" .. flag)
        end
        for _, define in ipairs(image:get("defines") or {}) do
            table.insert(argv, "--define=" .. define)
        end
        os.execv("python3", argv)
    end)
target_end()

includes("xmake/tasks.lua")
