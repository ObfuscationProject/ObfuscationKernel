#!/usr/bin/env python3
"""Exercise the kernel device filesystem's terminal nodes and /dev/pts directory: names, device numbers, modes,
locked-entry listing, strict lookup, stale entries, constant initialization and the namespace attachment, alone and
mounted in the actual VFS. Hosted model evidence only; no open path, guest or hardware claim."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import shlex
import shutil
import subprocess

TEST = 'tests/host/devfs_terminal_tests.cpp'
SOURCES = ('src/fs/devfs.cpp', 'src/sched/scheduler.cpp', 'src/sched/process.cpp', 'src/posix/posix.cpp',
           'src/posix/fd.cpp', 'src/posix/path.cpp', 'src/posix/process.cpp', 'src/fs/vfs.cpp',
           'src/driver/driver.cpp', 'src/smp/smp.cpp', 'src/arch/arch.cpp', 'src/arch/terminal_power.cpp',
           'src/syscall/linux.cpp', 'src/memory/memory.cpp', 'src/net/native_udp.cpp', 'src/arch/x86_64/ops.cpp')
MARKER = r'^DEVFS_TERMINAL_MODEL_PASS nodes=7 pts_capacity=8 locked_listed=1 strict_lookup=1 constinit=1 guest=0$'


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
    record = dict(status='running', scope='Hosted device filesystem and VFS model on the x86_64 host profile; '
                  'no open path, guest, hardware or SMP evidence.',
                  source_sha256={str(p.relative_to(source)): sha(p) for p in sorted(source.rglob('*')) if p.is_file()})
    binary = out / 'devfs-terminal-tests'
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
    print('PASS devfs terminal nodes: names, numbers, modes, locked listing, strict lookup, constinit; '
          'hosted model only, no guest execution', flush=True)


if __name__ == '__main__':
    main()
