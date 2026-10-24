#!/usr/bin/env python3
"""Compile and run independent next-data replay adversarial cases."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tests/host/ext4_next_data_replay_adversarial_tests.cpp"
REFERENCE = ROOT / "tests/host/ext4_next_data_replay_tests.cpp"
SUPPORT = ROOT / "tests/host/ext4_completion_support.hpp"
VIEW_SUPPORT = ROOT / "tests/host/ext4_view_test_support.hpp"
HEADERS = tuple(ROOT / "include/ok/fs" / name for name in (
    "ext4_readonly.hpp", "ext4_next_data_plan.hpp", "ext4_replay_view.hpp",
    "ext4_completion.hpp", "ext4_recovery.hpp"))
RECOVERY = ROOT / "src/fs/ext4_recovery.cpp"


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--input-sha256", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not sys.dont_write_bytecode or os.environ.get("PYTHONDONTWRITEBYTECODE") != "1":
        parser.error("run with PYTHONDONTWRITEBYTECODE=1 python3 -B")
    image, output = args.input.resolve(), args.output.resolve()
    if (not image.is_file() or digest(image) != args.input_sha256 or
            len(args.input_sha256) != 64):
        parser.error("exact input image digest differs")
    if output == ROOT or output.is_relative_to(ROOT) or ROOT.is_relative_to(output):
        parser.error("output must be outside the Kernel source tree")
    compiler = shutil.which("c++")
    if compiler is None:
        parser.error("host C++ compiler is unavailable")
    source_inputs = (SOURCE, REFERENCE, SUPPORT, VIEW_SUPPORT, RECOVERY, *HEADERS)
    before = {str(path): digest(path) for path in source_inputs}
    compiler_sha256 = digest(Path(compiler))
    output.mkdir(parents=True, exist_ok=False)
    executable = output / "ext4-next-data-replay-adversarial-tests"
    commands = [
        ([compiler, "-std=c++23", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
          "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
          "-I" + str(ROOT / "include"), str(SOURCE), str(RECOVERY), "-l:libext2fs.so.2",
          "-o", str(executable)], output / "compile.log"),
        ([str(executable), str(image)], output / "run.log"),
    ]
    results = []
    for argv, log in commands:
        with log.open("xb") as stream:
            completed = subprocess.run(argv, cwd=ROOT, stdout=stream,
                                       stderr=subprocess.STDOUT, check=False,
                                       env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1",
                                            "ASAN_OPTIONS": "detect_leaks=0"})
        results.append({"argv": argv, "returncode": completed.returncode,
                        "log": str(log), "log_sha256": digest(log)})
        if completed.returncode:
            break
    source_unchanged = before == {str(path): digest(path) for path in source_inputs}
    compiler_unchanged = compiler_sha256 == digest(Path(compiler))
    receipt = {"schema": 1, "scope": "independent-hosted-next-data-replay-adversarial",
               "status": "passed" if len(results) == 2 and all(not row["returncode"] for row in results)
               and source_unchanged and compiler_unchanged
               else "failed-preserved", "input": str(image), "input_sha256": digest(image),
               "source_files_sha256": before, "source_unchanged": source_unchanged,
               "compiler": compiler, "compiler_sha256": compiler_sha256,
               "compiler_unchanged": compiler_unchanged,
               "runner_sha256": digest(Path(__file__)),
               "commands": results, "sanitizers": ["address", "undefined"],
               "qemu": False, "hardware": False, "disk_writes_on_rejected_cases": False}
    target = output / "result.json"
    with target.open("x") as stream:
        stream.write(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    if receipt["status"] != "passed":
        raise SystemExit("hosted next-data replay adversarial cases failed; see " + str(target))
    print("PASS hosted next-data replay adversarial cases", digest(target))


if __name__ == "__main__":
    main()
