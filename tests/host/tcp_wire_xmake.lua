set_project("tcp-wire-leaf")
set_version("0.1.0")
set_languages("cxx23")
add_rules("mode.debug", "mode.release")

local source = os.getenv("TCP_WIRE_SOURCE")
local cxx = os.getenv("TCP_WIRE_CXX")
local cc = os.getenv("TCP_WIRE_CC")
local kind = os.getenv("TCP_WIRE_KIND")

toolchain("tcp-wire-selected")
    set_kind("standalone")
    on_check(function (toolchain)
        assert(source and os.isdir(source), "explicit private source required")
        assert(kind == "hosted" or kind == "sanitized" or kind == "object", "invalid codec qualification mode")
        assert(os.isfile(cxx) and os.isfile(cc), "exact selected compiler is missing")
        return true
    end)
    on_load(function (toolchain)
        toolchain:set("toolset", "cxx", cxx)
        toolchain:set("toolset", "cc", cc)
        toolchain:set("toolset", "ld", cxx)
    end)
toolchain_end()

target("tcp-wire")
    set_kind(kind == "object" and "object" or "binary")
    set_toolchains("tcp-wire-selected")
    add_includedirs(path.join(source, "include"))
    add_cxxflags("-std=c++23", "-Wall", "-Wextra", "-Werror", "-fno-exceptions", {force = true})
    if kind == "object" then
        add_files(path.join(source, "tests/native/tcp_wire_object.cpp"))
        add_cxxflags("-ffreestanding", "-fno-stack-protector", "-fno-threadsafe-statics", {force = true})
        local flags = os.getenv("TCP_WIRE_ARCH_FLAGS")
        if flags and #flags > 0 then
            for flag in flags:gmatch("%S+") do add_cxxflags(flag, {force = true}) end
        end
    else
        add_files(path.join(source, "tests/host/tcp_wire_tests.cpp"))
        if kind == "sanitized" then
            add_cxxflags("-fsanitize=address,undefined", "-fno-omit-frame-pointer", {force = true})
            add_ldflags("-fsanitize=address,undefined", {force = true})
        end
    end
target_end()
