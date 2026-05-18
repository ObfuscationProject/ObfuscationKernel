local task_arches = {"i386", "x86_64", "aarch64", "arm32", "rv64", "rv32", "loongarch64"}

local task_arch_specs = {
    i386 = {triple = "i386-elf"},
    x86_64 = {triple = "x86_64-elf"},
    aarch64 = {triple = "aarch64-elf"},
    arm32 = {triple = "arm-none-eabi"},
    rv64 = {triple = "riscv64-elf"},
    rv32 = {triple = "riscv32-elf"},
    loongarch64 = {triple = "loongarch64-elf"},
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
            {"a", "target-arch", "kv", "all", "Architecture: i386/x86_64/aarch64/arm32/rv64/rv32/loongarch64/all"},
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
        description = "Build and test qemu_smoke for the current xmake architecture",
        options = {
            {"a", "profile", "kv", nil, "Temporarily test another architecture"},
            {"m", "check-mode", "kv", nil, "Build mode used for the smoke test"}
        }
    }
    on_run(function ()
        import("core.base.option")
        import("core.project.config")
        config.load()

        local current_arch = config.get("arch") or "x86_64"
        local current_mode = config.get("mode") or "release"
        local mode = option.get("check-mode") or current_mode
        local requested_arch = option.get("profile")

        if requested_arch then
            local arch = task_normalize_arch(requested_arch)
            task_require_arch(arch)
            os.execv("xmake", {"f", "-c", "-m", mode, "-a", arch})
        end

        os.execv("xmake", {"-y", "-b", "qemu_smoke"})
        os.execv("xmake", {"test"})

        if requested_arch then
            os.execv("xmake", {"f", "-c", "-m", current_mode, "-a", current_arch})
        end
    end)
task_end()

task("qemu-window-test")
    set_menu {
        usage = "xmake qemu-window-test",
        description = "Show the current architecture smoke result in a QEMU graphical window",
        options = {
            {"m", "check-mode", "kv", nil, "Build mode used for the smoke test"},
            {nil, "display", "kv", "gtk", "QEMU display backend"},
            {nil, "no-launch", "k", nil, "Generate the demo image without opening QEMU"}
        }
    }
    on_run(function ()
        import("core.base.option")
        import("core.project.config")
        config.load()
        local arch = task_normalize_arch(config.get("arch") or "x86_64")
        task_require_arch(arch)
        local argv = {
            path.join(os.projectdir(), "scripts", "qemu_window_demo.py"),
            "--arch", arch,
            "--mode", option.get("check-mode") or config.get("mode") or "release",
            "--display", option.get("display") or "gtk"
        }
        if option.get("no-launch") then
            table.insert(argv, "--no-launch")
        end
        os.execv("python3", argv)
    end)
task_end()
