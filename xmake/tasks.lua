local task_arches = {
    "i386", "x86_64", "aarch64", "arm32", "rv64", "rv32", "loongarch64",
    "mips", "mips64", "ppc", "ppc64"
}

local task_arch_specs = {
    i386 = {triple = "i386-elf", bootable = true},
    x86_64 = {triple = "x86_64-elf", bootable = true},
    aarch64 = {triple = "aarch64-elf"},
    arm32 = {triple = "arm-none-eabi"},
    rv64 = {triple = "riscv64-elf"},
    rv32 = {triple = "riscv32-elf"},
    loongarch64 = {triple = "loongarch64-elf"},
    mips = {triple = "mips-elf"},
    mips64 = {triple = "mips64-elf"},
    ppc = {triple = "powerpc-eabi"},
    ppc64 = {triple = "powerpc64-elf"},
}

local function task_normalize_arch(arch)
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

local function task_require_arch(arch)
    local normalized = task_normalize_arch(arch)
    local spec = task_arch_specs[normalized]
    if spec == nil then
        raise("unsupported kernel arch '%s'; use one of: %s", normalized, table.concat(task_arches, ", "))
    end
    return normalized, spec
end

local function task_toolchain_binary(spec, tool)
    return path.join(os.projectdir(), "toolchains", spec.triple, "bin", spec.triple .. "-" .. tool)
end

task("toolchains")
    set_menu {
        usage = "xmake toolchains -a ARCH",
        description = "Build GCC/binutils cross toolchains into ./toolchains",
        options = {
            {"a", "target-arch", "kv", "all", "Architecture: i386/x86_64/aarch64/arm32/rv64/rv32/loongarch64/mips/mips64/ppc/ppc64/all"},
            {"j", "jobs", "kv", nil, "Parallel build jobs"}
        }
    }
    on_run(function ()
        import("core.base.option")
        local argv = {path.join(os.projectdir(), "scripts", "build-toolchain.sh"), "--arch", option.get("target-arch") or "all"}
        if option.get("jobs") then
            table.insert(argv, "--jobs")
            table.insert(argv, option.get("jobs"))
        end
        os.execv("bash", argv)
    end)
task_end()

task("toolchain-check")
    set_menu {
        usage = "xmake toolchain-check [-a ARCH] [--all]",
        description = "Check whether required freestanding toolchains are installed",
        options = {
            {"a", "profile", "kv", nil, "Architecture to check"},
            {nil, "all", "k", nil, "Check every supported architecture"}
        }
    }
    on_run(function ()
        import("core.base.option")
        import("core.project.config")
        config.load()

        local arches = {}
        if option.get("all") then
            arches = task_arches
        else
            table.insert(arches, option.get("profile") or config.get("arch") or "x86_64")
        end

        local missing = {}
        for _, arch in ipairs(arches) do
            local normalized, spec = task_require_arch(arch)
            local compiler = task_toolchain_binary(spec, "g++")
            if os.isfile(compiler) then
                print(string.format("[ok] %s: %s", normalized, compiler))
            else
                print(string.format("[missing] %s: %s", normalized, compiler))
                table.insert(missing, normalized)
            end
        end

        if #missing > 0 then
            raise("missing toolchain(s): %s. Run: xmake toolchains -a %s",
                  table.concat(missing, ", "), #missing == 1 and missing[1] or "all")
        end
    end)
task_end()

task("qemu-test")
    set_menu {
        usage = "xmake qemu-test [-a ARCH]",
        description = "Build and run the debug kernel test for the current xmake architecture",
        options = {
            {"a", "profile", "kv", nil, "Temporarily test another architecture"},
            {"m", "check-mode", "kv", nil, "Build mode used for the debug kernel test"}
        }
    }
    on_run(function ()
        import("core.base.option")
        import("core.project.config")
        config.load()

        local current_arch = config.get("arch") or "x86_64"
        local current_mode = config.get("mode") or "release"
        local mode = option.get("check-mode") or "debug"
        local requested_arch = option.get("profile")
        local test_arch = current_arch

        if requested_arch then
            test_arch = task_normalize_arch(requested_arch)
            task_require_arch(test_arch)
        end
        local _, test_spec = task_require_arch(test_arch)
        if not test_spec.bootable then
            raise("qemu boot test is not implemented for %s yet; build okernel to check the freestanding profile", test_arch)
        end

        local reconfigured = requested_arch or current_mode ~= mode
        if reconfigured then
            os.execv("xmake", {"f", "-c", "-m", mode, "-a", test_arch})
        end

        local build_code = os.execv("xmake", {"-y", "-b", "kernel"}, {try = true})
        local test_code = 0
        if build_code == 0 then
            test_code = os.execv("xmake", {"run", "kernel"}, {try = true})
        end

        if reconfigured then
            os.execv("xmake", {"f", "-c", "-m", current_mode, "-a", current_arch})
        end

        if build_code ~= 0 then
            raise("kernel build failed for %s", test_arch)
        end
        if test_code ~= 0 then
            raise("qemu kernel test failed for %s", test_arch)
        end
    end)
task_end()

task("qemu-window-test")
    set_menu {
        usage = "xmake qemu-window-test",
        description = "Show the debug kernel display output in a QEMU graphical window",
        options = {
            {"m", "check-mode", "kv", nil, "Build mode used for the debug kernel test"},
            {nil, "display", "kv", "gtk", "QEMU display backend"},
            {nil, "no-launch", "k", nil, "Generate the demo image without opening QEMU"}
        }
    }
    on_run(function ()
        import("core.base.option")
        import("core.project.config")
        config.load()
        local arch = task_normalize_arch(config.get("arch") or "x86_64")
        local _, spec = task_require_arch(arch)
        if not spec.bootable then
            raise("qemu window test is not implemented for %s yet; build okernel to check the freestanding profile", arch)
        end
        local mode = option.get("check-mode") or "debug"
        local current_mode = config.get("mode") or "release"
        local reconfigured = current_mode ~= mode
        if reconfigured then
            os.execv("xmake", {"f", "-c", "-m", mode, "-a", arch})
        end
        local build_code = os.execv("xmake", {"-y", "-b", "kernel"}, {try = true})
        if build_code ~= 0 then
            if reconfigured then
                os.execv("xmake", {"f", "-c", "-m", current_mode, "-a", arch})
            end
            raise("kernel build failed for %s", arch)
        end
        local argv = {
            path.join(os.projectdir(), "scripts", "qemu_window_test.py"),
            "--arch", arch,
            "--mode", mode,
            "--kernel", path.join(os.projectdir(), "build", "linux", arch, mode, "kernel.bin"),
            "--display", option.get("display") or "gtk"
        }
        if option.get("no-launch") then
            table.insert(argv, "--no-launch")
        end
        local code = os.execv("python3", argv, {try = true})
        if reconfigured then
            os.execv("xmake", {"f", "-c", "-m", current_mode, "-a", arch})
        end
        if code ~= 0 then
            raise("qemu window test failed for %s", arch)
        end
    end)
task_end()
