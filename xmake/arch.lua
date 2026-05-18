OK_SUPPORTED_ARCHES = {"i386", "x86_64", "aarch64", "arm32", "rv64", "rv32", "loongarch64"}

OK_ARCH_SPECS = {
    i386 = {
        define = "OK_ARCH_TARGET_I386",
        source = "i386",
        toolchain = "ok-i386-elf",
        triple = "i386-elf"
    },
    x86_64 = {
        define = "OK_ARCH_TARGET_X86_64",
        source = "x86_64",
        toolchain = "ok-x86_64-elf",
        triple = "x86_64-elf"
    },
    aarch64 = {
        define = "OK_ARCH_TARGET_AARCH64",
        source = "aarch64",
        toolchain = "ok-aarch64-elf",
        triple = "aarch64-elf"
    },
    arm32 = {
        define = "OK_ARCH_TARGET_ARM32",
        source = "arm32",
        toolchain = "ok-arm32-elf",
        triple = "arm-none-eabi"
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

function add_ok_freestanding_toolchain()
    local arch, spec = ok_require_arch(ok_current_arch())
    if spec == nil then
        spec = OK_ARCH_SPECS.x86_64
    end
    local compiler = ok_toolchain_binary(spec, "g++")
    if os.isfile(compiler) then
        set_toolchains(spec.toolchain)
    end
    before_build(function ()
        if not os.isfile(compiler) then
            raise("missing freestanding toolchain for %s (%s). Run: xmake toolchains -a %s",
                  arch, spec.triple, arch)
        end
    end)
end

function add_ok_debug_test_points()
    if is_mode("debug") then
        add_defines("OK_ENABLE_TEST_POINTS")
    end
end
