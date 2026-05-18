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

task("qemu-test")
    set_menu {
        usage = "xmake qemu-test [-a ARCH] [--user]",
        description = "Build and run hosted smoke tests without inheriting freestanding toolchains",
        options = {
            {"a", "profile", "kv", nil, "Architecture profile to test, or all"},
            {"m", "check-mode", "kv", "release", "Build mode used for the smoke test"},
            {nil, "user", "k", nil, "Use qemu-user Linux cross toolchains when available"}
        }
    }
    on_run(function ()
        import("core.base.option")
        import("core.project.config")
        config.load()
        local current_arch = config.get("arch_target") or "host"
        local current_toolchain = config.get("toolchain")
        local arch = option.get("profile") or current_arch
        local mode = option.get("check-mode") or "release"
        local argv = {
            path.join(os.projectdir(), "scripts", "qemu_profile_test.py"),
            "--arch", arch,
            "--mode", mode,
            "--restore-arch", current_arch
        }
        if current_toolchain then
            table.insert(argv, "--restore-toolchain")
            table.insert(argv, current_toolchain)
        end
        if option.get("user") then
            table.insert(argv, "--user")
        end
        os.execv("python3", argv)
    end)
task_end()

task("arch-check")
    set_menu {
        usage = "xmake arch-check -m debug",
        description = "Build every architecture profile and run debug-only profile test points directly",
        options = {
            {"m", "check-mode", "kv", "debug", "Build mode used for the architecture sweep"}
        }
    }
    on_run(function ()
        import("core.base.option")
        os.execv("python3", {
            path.join(os.projectdir(), "scripts", "arch_check.py"),
            "--mode", option.get("check-mode") or "debug"
        })
    end)
task_end()

task("qemu-window-test")
    set_menu {
        usage = "xmake qemu-window-test",
        description = "Run architecture smoke tests and show the summary in a QEMU graphical window",
        options = {
            {"m", "check-mode", "kv", "debug", "Build mode used for the architecture sweep"},
            {nil, "display", "kv", "gtk", "QEMU display backend"},
            {nil, "no-launch", "k", nil, "Generate the demo image without opening QEMU"}
        }
    }
    on_run(function ()
        import("core.base.option")
        import("core.project.config")
        config.load()
        local argv = {
            path.join(os.projectdir(), "scripts", "qemu_window_demo.py"),
            "--mode", option.get("check-mode") or "debug",
            "--display", option.get("display") or "gtk",
            "--restore-arch", config.get("arch_target") or "host"
        }
        local toolchain = config.get("toolchain")
        if toolchain then
            table.insert(argv, "--restore-toolchain")
            table.insert(argv, toolchain)
        end
        if option.get("no-launch") then
            table.insert(argv, "--no-launch")
        end
        os.execv("python3", argv)
    end)
task_end()

task("freestanding-check")
    set_menu {
        usage = "xmake freestanding-check",
        description = "Build freestanding okernel for every architecture toolchain",
        options = {
            {nil, "allow-missing", "k", nil, "Skip architectures whose toolchain is not installed"}
        }
    }
    on_run(function ()
        import("core.base.option")
        local argv = {path.join(os.projectdir(), "scripts", "freestanding_check.py")}
        if option.get("allow-missing") then
            table.insert(argv, "--allow-missing")
        end
        os.execv("python3", argv)
    end)
task_end()
