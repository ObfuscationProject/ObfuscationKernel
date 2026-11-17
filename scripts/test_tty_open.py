#!/usr/bin/env python3
"""Exercise open() of /dev/ptmx, /dev/pts/N, /dev/tty and /dev/console through the actual PosixService, scheduler and
VFS with the kernel device filesystem mounted and attached: locked slaves, O_NOCTTY, automatic acquisition, ENXIO,
the errno order, ENOSPC with raw pairs, fstat, pts chmod/chown, rollback, and the Linux st_rdev encoding of all ten
profiles at the offsets of docs/abi/linux-stat-target-layouts.json. Hosted model evidence only; no guest execution."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import shlex
import shutil
import subprocess

TEST = 'tests/host/tty_open_tests.cpp'
SOURCES = ('src/fs/devfs.cpp', 'src/sched/scheduler.cpp', 'src/sched/descriptions.cpp', 'src/sched/process.cpp', 'src/posix/posix.cpp',
           'src/posix/fd.cpp', 'src/posix/path.cpp', 'src/posix/process.cpp', 'src/fs/vfs.cpp',
           'src/driver/driver.cpp', 'src/smp/smp.cpp', 'src/arch/arch.cpp', 'src/arch/terminal_power.cpp',
           'src/syscall/linux.cpp', 'src/memory/memory.cpp', 'src/net/native_udp.cpp', 'src/arch/x86_64/ops.cpp')
LAYOUTS = 'docs/abi/linux-stat-target-layouts.json'
ARCHITECTURES = ('i386', 'x86_64', 'aarch64', 'arm32', 'rv64', 'rv32', 'loongarch64', 'mips', 'mips64', 'ppc')
MARKER = r'^TTY_OPEN_PASS checks=[0-9]+ profiles=10 hosted=1 guest=0$'


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
    # The ten retained target layouts give the rdev offsets the codec is checked against.
    layouts = json.loads((root / LAYOUTS).read_text())['profiles']
    if sorted(row['architecture'] for row in layouts) != sorted(ARCHITECTURES):
        raise ValueError('stat target layouts must name the ten profiles')
    rows = []
    for row in layouts:
        layout = row['layout']
        if layout['statx.stx_rdev_major.size'] != 4 or layout['statx.stx_rdev_minor.size'] != 4:
            raise ValueError('unexpected statx rdev width for ' + row['architecture'])
        rows.append('    {"%s", %d, %d, %d, %d, %d},' % (
            row['architecture'], layout['statx.stx_rdev_major.offset'], layout['statx.stx_rdev_minor.offset'],
            layout.get('kstat.st_rdev.offset', 0), layout.get('kstat.st_rdev.size', 0), layout.get('kstat.size', 0)))
    generated = source / 'generated'
    generated.mkdir()
    (generated / 'tty_open_layouts.hpp').write_text(
        '#pragma once\n// Generated from ' + LAYOUTS + ' by scripts/test_tty_open.py.\nnamespace tests\n{\n'
        'struct RdevLayout\n{\n    const char *architecture;\n'
        '    unsigned long statx_major, statx_minor, kstat_rdev, kstat_rdev_size, kstat_size;\n};\n'
        'inline constexpr RdevLayout rdev_layouts[] = {\n' + '\n'.join(rows) + '\n};\n} // namespace tests\n')
    record = dict(status='running', scope='Hosted PosixService, scheduler, VFS and device filesystem model on the '
                  'x86_64 host profile, Linux open/stat codecs of all ten profiles; no syscall entry, guest, '
                  'hardware or SMP evidence.', layouts_sha256=sha(root / LAYOUTS),
                  source_sha256={str(p.relative_to(source)): sha(p) for p in sorted(source.rglob('*')) if p.is_file()})
    binary = out / 'tty-open-tests'
    command = [*shlex.split(args.cxx), '-std=c++23', '-O1', '-g', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
               '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-DOK_ARCH_TARGET_X86_64',
               *(['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else []),
               '-I', str(source / 'include'), '-I', str(source / 'generated'), *(str(source / name) for name in (TEST, *SOURCES)), '-o', str(binary)]
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
    print('PASS terminal node opens: ptmx/pts/tty/console, O_NOCTTY, acquisition, errno order, ENOSPC, fstat, '
          'chmod/chown, rollback, ten-profile Linux st_rdev; hosted model only, no guest execution', flush=True)


if __name__ == '__main__':
    main()
