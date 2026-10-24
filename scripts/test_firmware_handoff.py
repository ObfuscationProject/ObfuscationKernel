#!/usr/bin/env python3
"""Hosted typed discovery tests; these do not qualify firmware RAM allocation."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--kernel-root',type=Path,default=Path(__file__).resolve().parents[1])
p.add_argument('--sanitize',action='store_true')
a=p.parse_args(); stage=Path(__file__).resolve().parents[1]; root=a.kernel_root.resolve()
with tempfile.TemporaryDirectory(prefix='ok-firmware-handoff-') as temp:
    command=['g++','-std=c++23','-O2','-Wall','-Wextra','-Wpedantic','-Werror','-fno-exceptions']
    if a.sanitize: command+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
    command += ['-I'+str(stage/'include'),'-I'+str(root/'include'),str(stage/'tests/host/firmware_handoff_tests.cpp'),str(root/'src/boot/fdt.cpp'),'-o',str(Path(temp)/'probe')]
    subprocess.run(command,check=True)
    subprocess.run([str(Path(temp)/'probe')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
