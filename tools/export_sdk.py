#!/usr/bin/env python3
"""Export a relocatable, architecture-specific kernel SDK from built artifacts."""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import tempfile
from pathlib import Path


def export(args: argparse.Namespace) -> None:
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".kernel-sdk-", dir=output.parent) as tmp:
        stage = Path(tmp)
        for name in ("lib", "boot", "image"):
            (stage / name).mkdir()
        shutil.copytree(args.headers, stage / "include")
        shutil.copy2(args.library, stage / "lib/libokernel.a")
        subprocess.run([str(args.ar), "rcs", str(stage / "lib/libokernel_boot.a"),
                        *[str(p) for p in args.boot_object]], check=True)
        shutil.copy2(args.linker_script, stage / "boot/linker.ld")
        shutil.copy2(args.kernel_bin, stage / "image/kernel.bin")
        shutil.copy2(args.kernel_elf, stage / "image/kernel.elf")
        if args.image_format == "bios-disk":
            if args.boot_sector is None:
                raise ValueError("bios-disk SDK requires --boot-sector")
            shutil.copy2(args.boot_sector, stage / "boot/boot16.bin")
        compiler_target = subprocess.check_output([str(args.compiler), "-dumpmachine"], text=True).strip()
        if compiler_target != args.triple:
            raise ValueError(f"compiler target {compiler_target} != {args.triple}")
        compiler_version = subprocess.check_output([str(args.compiler), "-dumpfullversion"], text=True).strip()
        manifest = {
            "schema": 1, "kernel_version": args.version, "abi_version": 2,
            "arch": args.arch, "mode": args.mode, "triple": args.triple,
            "image_format": args.image_format, "kernel_gui": "OK_ENABLE_KERNEL_GUI" in args.define, "compiler": {"family": "gcc", "version": compiler_version},
            "cxxflags": args.cxxflag, "asflags": args.asflag, "ldflags": args.ldflag, "defines": args.define,
            "files": {},
        }
        for artifact in sorted(stage.rglob("*")):
            if artifact.is_file():
                manifest["files"][artifact.relative_to(stage).as_posix()] = hashlib.sha256(artifact.read_bytes()).hexdigest()
        (stage / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        # Publish only a complete directory. A failed export leaves the previous
        # SDK untouched; readers can never observe partially copied artifacts.
        backup = output.with_name(output.name + ".previous")
        if backup.exists():
            shutil.rmtree(backup)
        if output.exists():
            output.rename(backup)
        try:
            stage.rename(output)
        except OSError:
            if backup.exists():
                backup.rename(output)
            raise
        if backup.exists():
            shutil.rmtree(backup)
    print(f"[kernel-sdk] exported {args.arch}/{args.mode}: {output}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("output", "headers", "library", "linker-script", "kernel-bin", "kernel-elf", "compiler", "ar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("arch", "mode", "triple", "image-format"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--version", default="0.1.0")
    parser.add_argument("--boot-sector", type=Path)
    parser.add_argument("--boot-object", type=Path, action="append", required=True)
    for name in ("cxxflag", "asflag", "ldflag", "define"):
        parser.add_argument("--" + name, action="append", default=[])
    export(parser.parse_args())


if __name__ == "__main__":
    main()
