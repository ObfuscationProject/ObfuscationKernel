set_project("tcp-streams-private")
set_languages("cxx23")
add_rules("mode.debug", "mode.release")
local source = os.getenv("TCP_STREAMS_SOURCE")
local cxx = os.getenv("TCP_STREAMS_CXX")
local cc = os.getenv("TCP_STREAMS_CC")
local kind = os.getenv("TCP_STREAMS_KIND")
local arch = os.getenv("TCP_STREAMS_ARCH")
toolchain("tcp-streams-selected")
    set_kind("standalone")
    on_check(function ()
        assert(source and os.isdir(source) and os.isfile(cxx) and os.isfile(cc), "explicit source/compiler required")
        assert(kind == "object" or kind == "hosted" or kind == "sanitized", "explicit qualification kind required")
        return true
    end)
    on_load(function (toolchain)
        toolchain:set("toolset", "cxx", cxx)
        toolchain:set("toolset", "cc", cc)
        toolchain:set("toolset", "ld", cxx)
    end)
toolchain_end()
target("tcp-streams")
    set_kind(kind == "object" and "object" or "binary")
    set_toolchains("tcp-streams-selected")
    add_includedirs(path.join(source, "include"))
    add_defines("OK_ARCH_TARGET_" .. arch:upper())
    add_cxxflags("-std=c++23", "-Wall", "-Wextra", "-Werror", "-fno-exceptions", "-ffunction-sections", "-fdata-sections", {force = true})
    add_files(path.join(source, "src/sched/scheduler.cpp"))
    if kind == "object" then
        add_files(path.join(source, "tests/native/tcp_streams_object.cpp"))
        add_cxxflags("-ffreestanding", "-fno-stack-protector", "-fno-threadsafe-statics", {force = true})
        for flag in (os.getenv("TCP_STREAMS_ARCH_FLAGS") or ""):gmatch("%S+") do
            add_cxxflags(flag, {force = true})
        end
    else
        add_files(path.join(source, "tests/host/tcp_streams_tests.cpp"),
            path.join(source, "src/sched/process.cpp"), path.join(source, "src/arch/arch.cpp"),
            path.join(source, "src/arch", arch, "ops.cpp"), path.join(source, "src/net/native_udp.cpp"),
            path.join(source, "src/memory/memory.cpp"))
        add_ldflags("-Wl,--gc-sections", {force = true})
        if kind == "sanitized" then
            add_cxxflags("-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", {force = true})
            add_ldflags("-fsanitize=address,undefined", "-no-pie", {force = true})
        end
    end
target_end()
if kind ~= "object" then
    for _, name in ipairs({"tcp-transport", "tcp-endpoints"}) do
        target(name)
            set_kind("binary")
            set_toolchains("tcp-streams-selected")
            add_includedirs(path.join(source, "include"))
            add_cxxflags("-std=c++23", "-Wall", "-Wextra", "-Werror", "-fno-exceptions", {force = true})
            add_files(path.join(source, "tests/host", name:gsub("-", "_") .. "_tests.cpp"))
            if kind == "sanitized" then
                add_cxxflags("-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", {force = true})
                add_ldflags("-fsanitize=address,undefined", "-no-pie", {force = true})
            end
        target_end()
    end
end
