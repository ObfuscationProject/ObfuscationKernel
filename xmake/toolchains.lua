local freestanding_toolchains = {
    {"ok-i386-elf", "i386-elf"},
    {"ok-x86_64-elf", "x86_64-elf"},
    {"ok-aarch64-elf", "aarch64-elf"},
    {"ok-arm32-elf", "arm-none-eabi"},
    {"ok-rv64-elf", "riscv64-elf"},
    {"ok-rv32-elf", "riscv32-elf"},
    {"ok-loongarch64-elf", "loongarch64-elf"},
    {"ok-mips-elf", "mips-elf"},
    {"ok-mips64-elf", "mips64-elf"},
    {"ok-ppc-elf", "powerpc-eabi"},
}

local linux_user_toolchains = {
    {"ok-i386-linux", "i686-linux-gnu"},
    {"ok-aarch64-linux", "aarch64-linux-gnu"},
    {"ok-arm32-linux", "arm-linux-gnueabihf"},
    {"ok-rv64-linux", "riscv64-linux-gnu"},
}

for _, spec in ipairs(freestanding_toolchains) do
    toolchain(spec[1])
        set_kind("standalone")
        set_sdkdir(path.join(os.projectdir(), "toolchains", spec[2]))
        set_toolset("cc", spec[2] .. "-gcc")
        set_toolset("cxx", spec[2] .. "-g++")
        set_toolset("as", spec[2] .. "-gcc")
        set_toolset("ld", spec[2] .. "-g++")
        set_toolset("ar", spec[2] .. "-ar")
        set_toolset("strip", spec[2] .. "-strip")
    toolchain_end()
end

for _, spec in ipairs(linux_user_toolchains) do
    toolchain(spec[1])
        set_kind("standalone")
        set_toolset("cc", spec[2] .. "-gcc")
        set_toolset("cxx", spec[2] .. "-g++")
        set_toolset("as", spec[2] .. "-as")
        set_toolset("ld", spec[2] .. "-ld")
        set_toolset("ar", spec[2] .. "-ar")
        set_toolset("strip", spec[2] .. "-strip")
    toolchain_end()
end
