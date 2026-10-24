set_project("Original Node suffix renewal private composition")
set_languages("c++23")
add_rules("mode.release")
option("sanitizers")
    set_default(false)
    set_showmenu(true)
option_end()
add_includedirs("../../../include", "..")
add_cxxflags("-fno-exceptions", "-O2", "-Wall", "-Wextra", "-Werror", {force = true})
if has_config("sanitizers") then
    add_cxxflags("-fsanitize=address,undefined", "-fno-omit-frame-pointer", {force = true})
    add_ldflags("-fsanitize=address,undefined", {force = true})
end
for name, file in pairs({
    ["retained-bridge"] = "model.cpp", ["original-node"] = "original_node.cpp",
    ["guarded-child"] = "child_outer.cpp", ["renewal-lifecycle"] = "renewal_lifecycle.cpp",
    ["clean-cancellation"] = "cancellation.cpp",
    ["semantic-suffix"] = "../mounted_typed_suffix_semantics/model.cpp",
    ["view-publication"] = "../mounted_suffix_view_publication/model.cpp"
}) do
    target(name)
        set_kind("binary")
        add_files(file, "../../../src/fs/ext4_recovery.cpp", "../ext4_realtime_model.cpp")
        add_cxxflags("-fno-access-control", {force = true})
        if name == "original-node" then
            add_ldflags("/usr/lib/x86_64-linux-gnu/libext2fs.so.2", {force = true})
        end
    target_end()
end
for name, file in pairs({["node-freestanding"] = "compile.cpp",
    ["suffix-freestanding"] = "../mounted_typed_suffix_semantics/compile.cpp",
    ["publication-freestanding"] = "../mounted_suffix_view_publication/compile.cpp"}) do
    target(name)
        set_kind("static")
        add_files(file)
        add_cxxflags("-ffreestanding", "-fno-stack-protector", "-fno-use-cxa-atexit",
            "-fno-threadsafe-statics", "-fstack-usage", {force = true})
    target_end()
end
