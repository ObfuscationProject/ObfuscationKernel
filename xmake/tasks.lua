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
