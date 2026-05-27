local task_arches = {
    "i386", "x86_64", "aarch64", "arm32", "rv64", "rv32", "loongarch64",
    "mips", "mips64", "ppc"
}

local task_arch_specs = {
    i386 = {triple = "i386-elf", bootable = true},
    x86_64 = {triple = "x86_64-elf", bootable = true},
    aarch64 = {triple = "aarch64-elf", bootable = true},
    arm32 = {triple = "arm-none-eabi", bootable = true},
    rv64 = {triple = "riscv64-elf", bootable = true},
    rv32 = {triple = "riscv32-elf", bootable = true},
    loongarch64 = {triple = "loongarch64-elf", bootable = true},
    mips = {triple = "mips-elf", bootable = true},
    mips64 = {triple = "mips64-elf", bootable = true},
    ppc = {triple = "powerpc-eabi", bootable = true},
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

local function task_config_bool(value)
    return value == true or value == "true" or value == "y" or value == "yes" or value == "1"
end

local function task_xmake(execv, projectdir, args, options)
    local argv = {}
    for _, arg in ipairs(args) do
        table.insert(argv, arg)
    end
    if args[1] and args[1]:sub(1, 1) == "-" then
        table.insert(argv, 1, projectdir)
        table.insert(argv, 1, "-P")
    else
        table.insert(argv, "-P")
        table.insert(argv, projectdir)
    end
    return execv("xmake", argv, options)
end

task("toolchains")
    set_menu {
        usage = "xmake toolchains -a ARCH",
        description = "Build GCC/binutils cross toolchains into ./toolchains",
        options = {
            {"a", "target-arch", "kv", "all", "Architecture: i386/x86_64/aarch64/arm32/rv64/rv32/loongarch64/mips/mips64/ppc/all"},
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

task("qemu-harness-test")
    set_menu {
        usage = "xmake qemu-harness-test",
        description = "Run host-side unit tests for the P0 QEMU validation harness"
    }
    on_run(function ()
        local code = os.execv("python3", {
            "-B",
            "-m",
            "unittest",
            "discover",
            "-s",
            path.join(os.projectdir(), "tests", "qemu"),
            "-p",
            "test_*.py",
        }, {try = true})
        if code ~= 0 then
            raise("QEMU harness unit tests failed")
        end
    end)
task_end()

task("profile-matrix")
    set_menu {
        usage = "xmake profile-matrix [-a ARCH|all] [-m MODE]",
        description = "Compile the freestanding okernel profile for one or every supported architecture",
        options = {
            {"a", "profile", "kv", "all", "Architecture to compile, or all"},
            {"m", "check-mode", "kv", nil, "Build mode used for profile compilation"}
        }
    }
    on_run(function ()
        import("core.base.option")
        import("core.project.config")
        config.load()

        local current_arch = task_normalize_arch(config.get("arch") or "x86_64")
        local current_mode = config.get("mode") or "release"
        local mode = option.get("check-mode") or current_mode
        local requested = option.get("profile") or "all"
        local arches = {}
        if requested == "all" then
            arches = task_arches
        else
            table.insert(arches, task_normalize_arch(requested))
        end

        local failed = {}
        for _, arch in ipairs(arches) do
            task_require_arch(arch)
            print(string.format("[profile] building %s (%s)", arch, mode))
            local config_code = task_xmake(os.execv, os.projectdir(), {"f", "-c", "-m", mode, "-a", arch}, {try = true})
            local build_code = config_code == 0 and task_xmake(os.execv, os.projectdir(), {"-y", "-b", "okernel"}, {try = true}) or config_code
            if build_code ~= 0 then
                table.insert(failed, arch)
            end
        end

        task_xmake(os.execv, os.projectdir(), {"f", "-c", "-m", current_mode, "-a", current_arch})
        if #failed > 0 then
            raise("profile build failed for: %s", table.concat(failed, ", "))
        end
    end)
task_end()

task("qemu-matrix")
    set_menu {
        usage = "xmake qemu-matrix [-m MODE]",
        description = "Build and run QEMU tests for every supported architecture",
        options = {
            {"m", "check-mode", "kv", nil, "Build mode used for QEMU tests"}
        }
    }
    on_run(function ()
        import("core.base.option")
        import("core.project.config")
        config.load()

        local current_arch = task_normalize_arch(config.get("arch") or "x86_64")
        local current_mode = config.get("mode") or "release"
        local mode = option.get("check-mode") or "debug"
        local failed = {}

        for _, arch in ipairs(task_arches) do
            local _, spec = task_require_arch(arch)
            if spec.bootable then
                print(string.format("[qemu] testing %s (%s)", arch, mode))
                local config_code = task_xmake(os.execv, os.projectdir(), {"f", "-c", "-m", mode, "-a", arch}, {try = true})
                local build_code = config_code == 0 and task_xmake(os.execv, os.projectdir(), {"-y", "-b", "okernel_image"}, {try = true}) or config_code
                local test_code = 0
                if build_code == 0 then
                    test_code = task_xmake(os.execv, os.projectdir(), {"run", "okernel_image"}, {try = true})
                end
                if build_code ~= 0 or test_code ~= 0 then
                    table.insert(failed, arch)
                end
            end
        end

        task_xmake(os.execv, os.projectdir(), {"f", "-c", "-m", current_mode, "-a", current_arch})
        if #failed > 0 then
            raise("QEMU test failed for: %s", table.concat(failed, ", "))
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
            task_xmake(os.execv, os.projectdir(), {"f", "-c", "-m", mode, "-a", test_arch})
        end

        local build_code = task_xmake(os.execv, os.projectdir(), {"-y", "-b", "okernel_image"}, {try = true})
        local test_code = 0
        if build_code == 0 then
            test_code = task_xmake(os.execv, os.projectdir(), {"run", "okernel_image"}, {try = true})
        end

        if reconfigured then
            task_xmake(os.execv, os.projectdir(), {"f", "-c", "-m", current_mode, "-a", current_arch})
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
        usage = "xmake qemu-window-test [-a ARCH]",
        description = "Show the debug kernel display output in a QEMU graphical window",
        options = {
            {"a", "profile", "kv", nil, "Temporarily test another architecture"},
            {"m", "check-mode", "kv", nil, "Build mode used for the debug kernel test"},
            {nil, "display", "kv", "gtk", "QEMU display backend"},
            {nil, "no-launch", "k", nil, "Generate the demo image without opening QEMU"}
        }
    }
    on_run(function ()
        import("core.base.option")
        import("core.project.config")
        config.load()
        local current_arch = task_normalize_arch(config.get("arch") or "x86_64")
        local arch = task_normalize_arch(option.get("profile") or current_arch)
        local _, spec = task_require_arch(arch)
        local mode = option.get("check-mode") or "debug"
        local current_mode = config.get("mode") or "release"
        local reconfigured = current_mode ~= mode or arch ~= current_arch
        if reconfigured then
            task_xmake(os.execv, os.projectdir(), {"f", "-c", "-m", mode, "-a", arch})
        end
        if not spec.bootable then
            local profile_code = task_xmake(os.execv, os.projectdir(), {"-y", "-b", "okernel"}, {try = true})
            if reconfigured then
                task_xmake(os.execv, os.projectdir(), {"f", "-c", "-m", current_mode, "-a", current_arch})
            end
            if profile_code ~= 0 then
                raise("freestanding profile build failed for %s", arch)
            end
            print(string.format("QEMU_WINDOW_TEST_SKIP arch=%s reason=boot_image_not_implemented profile=okernel", arch))
            return
        end
        local build_code = task_xmake(os.execv, os.projectdir(), {"-y", "-b", "okernel_image"}, {try = true})
        if build_code ~= 0 then
            if reconfigured then
                task_xmake(os.execv, os.projectdir(), {"f", "-c", "-m", current_mode, "-a", current_arch})
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
            task_xmake(os.execv, os.projectdir(), {"f", "-c", "-m", current_mode, "-a", current_arch})
        end
        if code ~= 0 then
            raise("qemu window test failed for %s", arch)
        end
    end)
task_end()

task("qemu-gui")
    set_menu {
        usage = "xmake qemu-gui [-a ARCH]",
        description = "Build a normal-mode kernel GUI image and launch it in a QEMU window",
        options = {
            {"a", "profile", "kv", nil, "Temporarily launch another architecture"},
            {"m", "check-mode", "kv", nil, "Build mode used for the GUI kernel"},
            {nil, "display", "kv", "gtk", "QEMU display backend"}
        }
    }
    on_run(function ()
        import("core.base.option")
        import("core.project.config")
        config.load()

        local current_arch = task_normalize_arch(config.get("arch") or "x86_64")
        local current_mode = config.get("mode") or "release"
        local current_kernel_gui = task_config_bool(config.get("kernel_gui"))
        local arch = task_normalize_arch(option.get("profile") or current_arch)
        local _, spec = task_require_arch(arch)
        local mode = option.get("check-mode") or "release"
        local reconfigured = current_mode ~= mode or arch ~= current_arch or not current_kernel_gui

        if reconfigured then
            task_xmake(os.execv, os.projectdir(), {"f", "-c", "-m", mode, "-a", arch, "--kernel_gui=y"})
        end
        if not spec.bootable then
            if reconfigured then
                task_xmake(os.execv, os.projectdir(), {"f", "-c", "-m", current_mode, "-a", current_arch,
                            "--kernel_gui=" .. (current_kernel_gui and "y" or "n")})
            end
            raise("qemu GUI boot is not implemented for %s yet", arch)
        end

        local build_code = task_xmake(os.execv, os.projectdir(), {"-y", "-b", "okernel_image"}, {try = true})
        local run_code = 0
        if build_code == 0 then
            run_code = os.execv("python3", {
                path.join(os.projectdir(), "scripts", "qemu_window_test.py"),
                "--arch", arch,
                "--mode", mode,
                "--kernel", path.join(os.projectdir(), "build", "linux", arch, mode, "kernel.bin"),
                "--display", option.get("display") or "gtk",
                "--normal-gui"
            }, {try = true})
        end

        if reconfigured then
            task_xmake(os.execv, os.projectdir(), {"f", "-c", "-m", current_mode, "-a", current_arch,
                        "--kernel_gui=" .. (current_kernel_gui and "y" or "n")})
        end
        if build_code ~= 0 then
            raise("kernel GUI build failed for %s", arch)
        end
        if run_code ~= 0 then
            raise("qemu GUI launch failed for %s", arch)
        end
    end)
task_end()

task("qemu-window-matrix")
    set_menu {
        usage = "xmake qemu-window-matrix [-m MODE]",
        description = "Run the headless QEMU window validation path for every supported architecture",
        options = {
            {"m", "check-mode", "kv", nil, "Build mode used for window validation"},
            {nil, "display", "kv", "none", "QEMU display backend for bootable profiles"}
        }
    }
    on_run(function ()
        import("core.base.option")
        import("core.project.config")
        config.load()

        local current_arch = task_normalize_arch(config.get("arch") or "x86_64")
        local current_mode = config.get("mode") or "release"
        local mode = option.get("check-mode") or "debug"
        local failed = {}

        for _, arch in ipairs(task_arches) do
            print(string.format("[qemu-window] checking %s (%s)", arch, mode))
            task_xmake(os.execv, os.projectdir(), {"f", "-c", "-m", mode, "-a", arch})
            local _, spec = task_require_arch(arch)
            local code = 0
            if spec.bootable then
                code = task_xmake(os.execv, os.projectdir(), {"qemu-window-test", "-a", arch, "-m", mode,
                                   "--display=" .. (option.get("display") or "none"), "--no-launch"},
                                  {try = true})
            else
                code = task_xmake(os.execv, os.projectdir(), {"qemu-window-test", "-a", arch, "-m", mode, "--no-launch"}, {try = true})
            end
            if code ~= 0 then
                table.insert(failed, arch)
            end
        end

        task_xmake(os.execv, os.projectdir(), {"f", "-c", "-m", current_mode, "-a", current_arch})
        if #failed > 0 then
            raise("QEMU window validation failed for: %s", table.concat(failed, ", "))
        end
    end)
task_end()
