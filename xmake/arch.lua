OK_SUPPORTED_ARCHES = {
    "i386", "x86_64", "aarch64", "arm32", "rv64", "rv32", "loongarch64",
    "mips", "mips64", "ppc", "ppc64"
}

OK_ARCH_SPECS = {
    i386 = {
        define = "OK_ARCH_TARGET_I386",
        source = "i386",
        toolchain = "ok-i386-elf",
        triple = "i386-elf",
        boot_source = "src/arch/i386/boot.S",
        linker_script = "src/arch/i386/linker.ld",
        qemu_system = "qemu-system-i386",
        platform_source = "src/arch/x86_common/platform.cpp"
    },
    x86_64 = {
        define = "OK_ARCH_TARGET_X86_64",
        source = "x86_64",
        toolchain = "ok-x86_64-elf",
        triple = "x86_64-elf",
        boot_source = "src/arch/x86_64/boot.S",
        linker_script = "src/arch/x86_64/linker.ld",
        qemu_system = "qemu-system-x86_64",
        platform_source = "src/arch/x86_common/platform.cpp",
        freestanding_cxxflags = {"-mno-red-zone"}
    },
    aarch64 = {
        define = "OK_ARCH_TARGET_AARCH64",
        source = "aarch64",
        toolchain = "ok-aarch64-elf",
        triple = "aarch64-elf",
        boot_source = "src/arch/aarch64/boot.S",
        linker_script = "src/arch/aarch64/linker.ld",
        qemu_system = "qemu-system-aarch64",
        platform_source = "src/arch/aarch64/platform.cpp",
        image_format = "linux-image",
        freestanding_cxxflags = {"-mno-outline-atomics"}
    },
    arm32 = {
        define = "OK_ARCH_TARGET_ARM32",
        source = "arm32",
        toolchain = "ok-arm32-elf",
        triple = "arm-none-eabi",
        freestanding_cxxflags = {"-march=armv7-a", "-marm"}
    },
    rv64 = {
        define = "OK_ARCH_TARGET_RV64",
        source = "rv64",
        toolchain = "ok-rv64-elf",
        triple = "riscv64-elf"
    },
    rv32 = {
        define = "OK_ARCH_TARGET_RV32",
        source = "rv32",
        toolchain = "ok-rv32-elf",
        triple = "riscv32-elf"
    },
    loongarch64 = {
        define = "OK_ARCH_TARGET_LOONGARCH64",
        source = "loongarch64",
        toolchain = "ok-loongarch64-elf",
        triple = "loongarch64-elf"
    },
    mips = {
        define = "OK_ARCH_TARGET_MIPS",
        source = "mips",
        toolchain = "ok-mips-elf",
        triple = "mips-elf",
        freestanding_cxxflags = {"-march=mips32r2"}
    },
    mips64 = {
        define = "OK_ARCH_TARGET_MIPS64",
        source = "mips64",
        toolchain = "ok-mips64-elf",
        triple = "mips64-elf",
        freestanding_cxxflags = {"-march=mips64r2"}
    },
    ppc = {
        define = "OK_ARCH_TARGET_PPC",
        source = "ppc",
        toolchain = "ok-ppc-elf",
        triple = "powerpc-eabi"
    },
    ppc64 = {
        define = "OK_ARCH_TARGET_PPC64",
        source = "ppc64",
        toolchain = "ok-ppc64-elf",
        triple = "powerpc64-elf"
    },
}

function ok_normalize_arch(arch)
    if arch == nil or arch == "" then
        return "x86_64"
    end
    if arch == "x64" then
        return "x86_64"
    end
    if arch == "arm64" then
        return "aarch64"
    end
    if arch == "arm" then
        return "arm32"
    end
    if arch == "riscv64" then
        return "rv64"
    end
    if arch == "riscv32" then
        return "rv32"
    end
    if arch == "loong64" then
        return "loongarch64"
    end
    if arch == "powerpc" then
        return "ppc"
    end
    if arch == "powerpc64" then
        return "ppc64"
    end
    return arch
end

function ok_current_arch()
    return ok_normalize_arch(get_config("arch"))
end

function ok_arch_spec(arch)
    return OK_ARCH_SPECS[ok_normalize_arch(arch)]
end

function ok_require_arch(arch)
    local normalized = ok_normalize_arch(arch)
    local spec = OK_ARCH_SPECS[normalized]
    return normalized, spec
end

function ok_toolchain_binary(spec, tool)
    return path.join(os.projectdir(), "toolchains", spec.triple, "bin", spec.triple .. "-" .. tool)
end

function ok_has_freestanding_toolchain(arch)
    local _, spec = ok_require_arch(arch)
    if spec == nil then
        return false
    end
    return os.isfile(ok_toolchain_binary(spec, "g++"))
end

function ok_require_freestanding_toolchain(arch)
    local normalized, spec = ok_require_arch(arch)
    if spec == nil or not ok_has_freestanding_toolchain(normalized) then
        return false
    end
    return true
end

function add_ok_arch_profile()
    local _, spec = ok_require_arch(ok_current_arch())
    add_defines(spec.define)
end

function add_ok_arch_files()
    local _, spec = ok_require_arch(ok_current_arch())
    add_files("src/arch/arch.cpp")
    add_files(path.join("src/arch", spec.source, "ops.cpp"))
end

function add_ok_boot_files()
    local arch, spec = ok_require_arch(ok_current_arch())
    if spec.boot_source == nil or spec.linker_script == nil then
        before_build(function ()
            raise("bootable kernel output is not implemented for %s yet", arch)
        end)
        return
    end
    add_files(spec.boot_source)
    if spec.platform_source then
        add_files(spec.platform_source)
    end
    set_values("ok.linker_script", path.join(os.projectdir(), spec.linker_script))
    set_values("ok.image_format", spec.image_format or "bios-disk")
    set_values("ok.kernel_bin", path.join(os.projectdir(), "build", "linux", arch, get_config("mode") or "release", "kernel.bin"))
end

function add_ok_freestanding_toolchain()
    local arch, spec = ok_require_arch(ok_current_arch())
    if spec == nil then
        spec = OK_ARCH_SPECS.x86_64
    end
    local compiler = ok_toolchain_binary(spec, "g++")
    local toolchain_dir = path.join(os.projectdir(), "toolchains", spec.triple)
    if os.isfile(compiler) then
        set_toolchains(spec.toolchain)
        add_includedirs(path.join(toolchain_dir, spec.triple, "include"), {public = true})
        add_includedirs(path.join(toolchain_dir, "include"), {public = true})
        if os.isdir(toolchain_dir) then
            add_cxxflags("--sysroot=" .. toolchain_dir, {force = true})
        end
    end
    before_build(function ()
        if not os.isfile(compiler) then
            raise("missing freestanding toolchain for %s (%s). Run: xmake toolchains -a %s",
                  arch, spec.triple, arch)
        end
    end)
end

function add_ok_freestanding_arch_flags()
    local _, spec = ok_require_arch(ok_current_arch())
    if spec.freestanding_cxxflags then
        for _, flag in ipairs(spec.freestanding_cxxflags) do
            add_cxxflags(flag, {force = true})
        end
    end
end

function add_ok_debug_test_points()
    if is_mode("debug") then
        add_defines("OK_ENABLE_TEST_POINTS")
    end
end
