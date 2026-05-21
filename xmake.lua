set_project("ObfuscationKernel")
set_version("0.1.0")
set_xmakever("3.0.0")

add_rules("mode.debug", "mode.release")
set_languages("c++23")

includes("xmake/arch.lua")
includes("xmake/toolchains.lua")

function add_ok_kernel_sources(include_kernel_main)
    add_files("src/core/*.cpp")
    if not include_kernel_main then
        remove_files("src/core/kernel_main.cpp")
    end
    add_files("src/driver/*.cpp")
    add_files("src/fs/*.cpp")
    add_files("src/interrupt/*.cpp")
    add_files("src/ipc/*.cpp")
    add_files("src/memory/*.cpp")
    add_files("src/net/*.cpp")
    add_files("src/posix/*.cpp")
    add_files("src/sched/*.cpp")
    add_files("src/smp/*.cpp")
    add_files("src/syscall/*.cpp")
    add_files("src/user/*.cpp")
    add_ok_arch_files()
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
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        {force = true}
    )
    add_defines("OK_KERNEL_FREESTANDING")
    add_ok_arch_profile()
    add_ok_debug_test_points()
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
    add_ok_arch_profile()
    add_ok_debug_test_points()
    if ok_current_arch() == "i386" or ok_current_arch() == "x86_64" or ok_current_arch() == "aarch64" or
        ok_current_arch() == "arm32" or ok_current_arch() == "rv64" or ok_current_arch() == "rv32" then
        add_tests("qemu")
    end
    after_build(function (target)
        local arch = target:values("ok.arch")
        local kernel_bin = path.join(target:targetdir(), "kernel.bin")
        if target:values("ok.image_format") == "elf" then
            os.cp(target:targetfile(), kernel_bin)
            return
        end
        local triples = {
            i386 = "i386-elf",
            x86_64 = "x86_64-elf",
            aarch64 = "aarch64-elf",
            rv64 = "riscv64-elf"
        }
        local triple = triples[arch]
        if triple == nil then
            raise("kernel.bin objcopy is not configured for %s", arch)
        end
        local toolchain_bin = path.join(os.projectdir(), "toolchains", triple, "bin")
        local gcc = path.join(toolchain_bin, triple .. "-gcc")
        local ld = path.join(toolchain_bin, triple .. "-ld")
        local objcopy = path.join(toolchain_bin, triple .. "-objcopy")

        local payload_bin = path.join(target:targetdir(), "kernel_payload.bin")
        os.execv(objcopy, {"-O", "binary", target:targetfile(), payload_bin})

        if target:values("ok.image_format") == "linux-image" then
            os.cp(payload_bin, kernel_bin)
            return
        end

        local autogen = path.join(target:autogendir(), "boot")
        os.mkdir(autogen)

        local boot_object = path.join(autogen, "boot16.o")
        local boot_sector = path.join(autogen, "boot16.bin")
        os.execv(gcc, {"-c", path.join(os.projectdir(), "src/arch/x86_64/boot16.S"), "-o", boot_object})
        os.execv(ld, {"-Ttext=0x7c00", "--oformat=binary", boot_object, "-o", boot_sector})

        local payload_size = os.filesize(payload_bin)
        local payload_capacity = 512 * 768
        if payload_size > payload_capacity then
            raise("kernel payload is too large: %d bytes > %d bytes", payload_size, payload_capacity)
        end
        os.cp(boot_sector, kernel_bin)
        os.execv("truncate", {"-s", tostring(512 + payload_capacity), kernel_bin})
        os.execv("dd", {"if=" .. payload_bin, "of=" .. kernel_bin, "bs=512", "seek=1", "conv=notrunc", "status=none"})
    end)
    on_run(function (target)
        os.execv("python3", {
            path.join(os.projectdir(), "scripts", "qemu_test.py"),
            "--arch", target:values("ok.arch"),
            "--kernel", path.join(target:targetdir(), "kernel.bin")
        })
    end)
    on_test(function (target)
        local code = os.execv("python3", {
            path.join(os.projectdir(), "scripts", "qemu_test.py"),
            "--arch", target:values("ok.arch"),
            "--kernel", path.join(target:targetdir(), "kernel.bin")
        }, {try = true})
        return code == 0
    end)
target_end()

includes("xmake/tasks.lua")
