#!/usr/bin/env python3
"""Hosted admission/ownership bridge checks; no target CPU execution."""
import argparse,os,subprocess,tempfile
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--kernel-root',type=Path,default=Path(__file__).resolve().parents[1])
p.add_argument('--sanitize',action='store_true')
a=p.parse_args();stage=Path(__file__).resolve().parents[1];root=a.kernel_root.resolve()
with tempfile.TemporaryDirectory(prefix='ok-runtime-frame-bridge-') as temp:
    binary=Path(temp)/'probe'
    command=['g++','-std=c++23','-O1','-g','-Wall','-Wextra','-Wpedantic','-Werror']
    if a.sanitize:command+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
    command+=['-I'+str(stage/'include'),'-I'+str(root/'include'),str(stage/'tests/host/runtime_frame_bridge_tests.cpp'),str(root/'src/memory/memory.cpp'),'-o',str(binary)]
    subprocess.run(command,check=True)
    subprocess.run([str(binary)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
