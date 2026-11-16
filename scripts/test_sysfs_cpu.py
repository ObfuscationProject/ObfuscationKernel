#!/usr/bin/env python3
"""Exercise the CPU sysfs provider (/sys/devices/system/cpu): the tree, cpulists for UP, SMP, offline and wedged
views, modes, strict lookup, directory paths, constant initialization and the kernfs open semantics through the actual
PosixService open as root and uid 1000. Hosted model evidence only; no write route, hotplug, guest or hardware claim."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import shlex
import shutil
import subprocess

TEST = 'tests/host/sysfs_cpu_tests.cpp'
SOURCES = ('src/fs/sysfs.cpp', 'src/sched/scheduler.cpp', 'src/sched/process.cpp', 'src/posix/posix.cpp',
           'src/posix/fd.cpp', 'src/posix/path.cpp', 'src/posix/process.cpp', 'src/fs/vfs.cpp',
           'src/driver/driver.cpp', 'src/smp/smp.cpp', 'src/arch/arch.cpp', 'src/arch/terminal_power.cpp',
           'src/syscall/linux.cpp', 'src/memory/memory.cpp', 'src/net/native_udp.cpp', 'src/arch/x86_64/ops.cpp')
MARKER = r'^SYSFS_CPU_MODEL_PASS nodes=12 refused_opens=7 cpulist_max=19 constinit=1 guest=0$'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = out / 'source'
    shutil.copytree(root / 'include', source / 'include')
    shutil.copytree(root / 'src', source / 'src')
    (source / TEST).parent.mkdir(parents=True)
    shutil.copy2(root / TEST, source / TEST)
    record = dict(status='running', scope='Hosted CPU sysfs provider, VFS and open model on the x86_64 host profile; '
                  'no write route, hotplug service, guest, hardware or SMP evidence.',
                  source_sha256={str(p.relative_to(source)): sha(p) for p in sorted(source.rglob('*')) if p.is_file()})
    binary = out / 'sysfs-cpu-tests'
    command = [*shlex.split(args.cxx), '-std=c++23', '-O1', '-g', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
               '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-DOK_ARCH_TARGET_X86_64',
               *(['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else []),
               '-I', str(source / 'include'), *(str(source / name) for name in (TEST, *SOURCES)), '-o', str(binary)]
    try:
        subprocess.run(command, check=True)
        result = subprocess.run([str(binary)], check=True, stdout=subprocess.PIPE, text=True)
        (out / 'run.log').write_text(result.stdout)
        if not re.search(MARKER, result.stdout, re.M):
            raise ValueError('missing exact success marker')
        record.update(status='passed', output=result.stdout.strip(), binary_sha256=sha(binary))
    except BaseException as error:
        record.update(status='failed', error=repr(error))
        raise
    finally:
        (out / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    print('PASS CPU sysfs: tree, cpulists, modes, strict lookup, kernfs opens, constinit; '
          'hosted model only, no guest execution', flush=True)


if __name__ == '__main__':
    main()
