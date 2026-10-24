#!/usr/bin/env python3
"""Hosted xmake regression for partial block-device completion failures."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="ok-block-cache-") as directory:
        source = Path(directory)
        shutil.copytree(root / "include", source / "include")
        paths = ["tests/host/block_cache_failure_tests.cpp", "tests/kernel/roadmap/block_cache_failure.hpp",
                 "src/fs/storage.cpp", "src/smp/smp.cpp", "src/arch/arch.cpp", "src/arch/x86_64/ops.cpp",
                 "src/arch/ops_private.hpp"]
        for name in paths:
            destination = source / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(root / name, destination)
        files = ", ".join('"' + name + '"' for name in paths if name.endswith(".cpp"))
        (source / "xmake.lua").write_text('set_languages("c++23")\n'
            'target("block-cache")\nset_kind("binary")\nset_targetdir("artifacts")\n'
            'add_defines("OK_ARCH_TARGET_X86_64")\nadd_includedirs("include")\n'
            'add_files(' + files + ')\n'
            'add_cxxflags("-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", '
            '"-fno-omit-frame-pointer", "-fno-pie", "-ffunction-sections", "-fdata-sections", {force=true})\n'
            'add_ldflags("-fsanitize=address,undefined", "-no-pie", "-Wl,--gc-sections", {force=true})\ntarget_end()\n')
        environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1")
        for argv in (["xmake", "f", "-c", "-y", "--ccache=n"], ["xmake", "build", "-j2"],
                     [str(source / "artifacts/block-cache")]):
            subprocess.run(argv, cwd=source, env=environment, check=True)
    print("PASS hosted block cache partial read/write failure controls")


if __name__ == "__main__":
    main()
