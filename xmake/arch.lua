OK_SUPPORTED_ARCHES = {"host", "i386", "x86_64", "aarch64", "arm32", "rv64", "rv32", "loongarch64"}

option("arch_target")
    set_default("host")
    set_showmenu(true)
    set_values(table.unpack(OK_SUPPORTED_ARCHES))
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

function add_ok_arch_profile()
    local arch = get_config("arch_target") or "host"
    if arch_defines[arch] then
        add_defines(arch_defines[arch])
    end
end

function add_ok_arch_files()
    local arch = get_config("arch_target") or "host"
    add_files("src/arch/arch.cpp")
    add_files(path.join("src/arch", arch, "ops.cpp"))
end

function add_ok_debug_test_points()
    if is_mode("debug") then
        add_defines("OK_ENABLE_TEST_POINTS")
    end
end
