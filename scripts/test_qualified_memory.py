#!/usr/bin/env python3
"""Hosted boot-memory normalization and entry policy; no native allocator proof."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel-root',type=Path,default=Path(__file__).resolve().parents[1])
    parser.add_argument('--sanitize',action='store_true')
    args = parser.parse_args()
    stage = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='ok-qualified-memory-') as temporary:
        command=['g++','-std=c++23','-O2','-Wall','-Wextra','-Wpedantic','-Werror','-fno-exceptions']
        if args.sanitize: command+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        command+=['-I'+str(stage/'include'),'-I'+str(args.kernel_root.resolve()/'include'),str(stage/'tests/host/qualified_memory_tests.cpp'),'-o',str(Path(temporary)/'probe')]
        subprocess.run(command,check=True)
        subprocess.run([str(Path(temporary)/'probe')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
    return 0
if __name__=='__main__':raise SystemExit(main())
