#!/usr/bin/env python3
"""Snapshot-bound hosted maintained-ext4 timestamp events, failures and raw images.

Uses xmake and six ext4 layouts. Fault cache/durable images are retained as
lossless changed-sector deltas against explicit retained operation inputs.
This is a provider model; no kernel, target machine or syscall is executed.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import sys


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(4 * 1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def verify_times(image, oracle):
    data = image.read_bytes()
    u16 = lambda at: struct.unpack_from('<H', data, at)[0]
    u32 = lambda at: struct.unpack_from('<I', data, at)[0]
    size, per, stride = 1024 << u32(1048), u32(1064), u16(1112)
    records = []
    for row in csv.reader(oracle.read_text().splitlines(), delimiter='\t'):
        number, *expected = map(int, row)
        group, index = divmod(number - 1, per)
        descriptor = (u32(1044) + 1) * size + group * 32
        at = u32(descriptor + 8) * size + index * stride
        extent = u16(at + 128) if stride == 256 else 0
        assert 0 <= extent <= stride - 128 and extent % 4 == 0
        decoded = []
        for offset, extra, minimum in ((8, 140, 16), (16, 136, 12), (12, 132, 8)):
            seconds = struct.unpack_from('<i', data, at + offset)[0]
            nanos = 0
            if extent >= minimum:
                word = u32(at + extra)
                seconds += (word & 3) * (1 << 32)
                nanos = word >> 2
            assert 0 <= nanos < 1_000_000_000
            decoded.extend((seconds, nanos))
        assert decoded == expected, (image, number, decoded, expected)
        records.append({'inode': number, 'extra_isize': extent, 'access': decoded[:2],
                        'modification': decoded[2:4], 'change': decoded[4:]})
    assert records
    return records


def seed_distinct_times(image):
    """Independent fixture construction, outside the provider and its codec."""
    data = bytearray(image.read_bytes())
    u16 = lambda at: struct.unpack_from('<H', data, at)[0]
    u32 = lambda at: struct.unpack_from('<I', data, at)[0]
    size, per, stride = 1024 << u32(1048), u32(1064), u16(1112)
    for number in (2, 11):  # Root and mke2fs lost+found.
        group, index = divmod(number - 1, per)
        at = u32((u32(1044) + 1) * size + group * 32 + 8) * size + index * stride
        assert u16(at) & 0o170000 == 0o040000
        extent = u16(at + 128) if stride == 256 else 0
        for field, (offset, extra, minimum) in enumerate(((8, 140, 16), (16, 136, 12), (12, 132, 8))):
            struct.pack_into('<I', data, at + offset, 1_600_000_000 + number * 10 + field)
            if extent >= minimum:
                struct.pack_into('<I', data, at + extra, (700_000_000 + number * 10 + field) << 2)
    image.write_bytes(data)


def reconstruct(base, delta):
    """Read strictly increasing uint64 offset + 512 bytes, with no implicit data."""
    result = bytearray(base)
    encoded = delta.read_bytes()
    assert len(encoded) % 520 == 0
    previous = -512
    for at in range(0, len(encoded), 520):
        offset = struct.unpack_from('<Q', encoded, at)[0]
        assert offset % 512 == 0 and previous < offset and offset + 512 <= len(result)
        payload = encoded[at + 8:at + 520]
        assert payload != base[offset:offset + 512]
        result[offset:offset + 512] = payload
        previous = offset
    return hashlib.sha256(result).hexdigest()


def verify_faults(folder):
    records, bases, traces = [], {}, {}
    seen = set()
    rows = list(csv.DictReader((folder / 'cases.tsv').open(), delimiter='\t'))
    for row in rows:
        op, point, effect = row['operation'], int(row['point']), int(row['effect'])
        if op not in bases:
            bases[op] = ((folder / (op + '-initial.img')).read_bytes(),
                         (folder / (op + '-initial-durable.img')).read_bytes())
            traces[op] = [line.split('\t') for line in (folder / (op + '-trace.tsv')).read_text().splitlines()]
        trace = traces[op]
        assert 1 <= point <= len(trace) and effect in (0, 1, 2)
        assert (op, point, effect) not in seen
        seen.add((op, point, effect))
        assert trace[point - 1] == [str(point), row['transport'], row['samples']]
        assert row['transport'] in ('r', 'w', 'f') and int(row['samples']) in (0, 1)
        cache = folder / row['cache_delta']
        durable = folder / row['durable_delta']
        assert cache.parent == folder and durable.parent == folder
        records.append({**row, 'cache_sha256': reconstruct(bases[op][0], cache),
                        'durable_sha256': reconstruct(bases[op][1], durable)})
    expected = {(op, i + 1, effect) for op, trace in traces.items() for i in range(len(trace)) for effect in range(3)}
    assert seen == expected and len(traces) == 28
    assert {row['transport'] for row in rows} == {'r', 'w', 'f'}
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    environment = os.environ.copy()
    environment.update(XMAKE_GLOBALDIR=str(output / 'xmake-global'), XMAKE_ROOT='y')
    if args.sanitize:
        environment['ASAN_OPTIONS'] = 'detect_leaks=0:halt_on_error=1'
        environment['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
    report = {'scope': 'hosted-maintained-ext4-provider-model', 'status': 'running',
              'native_execution': False, 'syscall_execution': False, 'physical_durability': False,
              'power_loss_atomicity': False, 'smp_execution': False, 'sanitize': args.sanitize,
              'synthetic_capacity_images_fsck_qualified': False,
              'fault_storage': 'lossless 512-byte-sector deltas against per-operation cache/durable initial images',
              'asan_options': environment.get('ASAN_OPTIONS'), 'ubsan_options': environment.get('UBSAN_OPTIONS'),
              'platform': platform.platform(), 'commands': [], 'cases': [], 'tools': {}}

    def save():
        (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')

    def run(argv, label, cwd=None):
        argv = [str(item) for item in argv]
        result = subprocess.run(argv, cwd=cwd, env=environment, text=True, capture_output=True)
        log = output / (label + '.log')
        log.write_text(result.stdout + result.stderr)
        report['commands'].append({'argv': argv, 'cwd': str(cwd) if cwd else None,
                                   'returncode': result.returncode, 'log': log.name, 'log_sha256': sha(log)})
        save()
        if result.returncode:
            raise RuntimeError(f'{label} failed: {log}')
        return result.stdout

    try:
        for name in ('xmake', args.cxx, 'mke2fs', 'e2fsck', sys.executable):
            executable = shutil.which(name)
            assert executable, f'missing tool: {name}'
            path = Path(executable).resolve()
            report['tools'][name] = {'path': str(path), 'sha256': sha(path)}
        compiler = report['tools'][args.cxx]['path']
        run([compiler, '--version'], 'compiler-version')
        run(['xmake', '--version'], 'xmake-version')
        # e2fsprogs version invocations can write to stderr and return success.
        run(['mke2fs', '-V'], 'mke2fs-version')
        run(['e2fsck', '-V'], 'e2fsck-version')
        snapshot = output / 'sources'
        files = [*sorted((root / 'include').rglob('*')),
                 root / 'src/fs/ext4_fs.cpp', root / 'tests/host/ext4_maintained_times_tests.cpp',
                 root / 'tests/host/ext4_realtime_model.cpp', root / 'tests/host/ext4_realtime_model.hpp',
                 root / 'scripts/test_ext4_maintained_times.py']
        files = [p for p in files if p.is_file()]
        identities = {str(p.relative_to(root)): sha(p) for p in files}
        for path in files:
            copied = snapshot / path.relative_to(root)
            copied.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, copied)
        report['source_sha256'] = {str(p.relative_to(snapshot)): sha(p) for p in snapshot.rglob('*') if p.is_file()}
        assert report['source_sha256'] == identities
        assert identities == {str(p.relative_to(root)): sha(p) for p in files}, 'source changed while taking snapshot'
        project = output / 'project'
        project.mkdir()
        flags = ['-O1' if args.sanitize else '-O2', '-g', '-Wall', '-Wextra', '-Wpedantic', '-Werror']
        if args.sanitize:
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        q = json.dumps
        lua = ['set_project("ext4-maintained-hosted")', 'set_languages("c++23")',
               'set_policy("build.ccache", false)', 'target("ext4-maintained-times")',
               '    set_kind("binary")', '    set_toolset("cxx", ' + q(compiler) + ')',
               '    set_toolset("ld", ' + q(compiler) + ')',
               '    set_targetdir(' + q(str(output / 'bin')) + ')',
               '    set_objectdir(' + q(str(output / 'build/objects')) + ')',
               '    set_dependir(' + q(str(output / 'build/dependencies')) + ')',
               '    add_defines("OK_ARCH_TARGET_X86_64")',
               '    add_includedirs(' + q(str(snapshot / 'include')) + ')',
               '    add_cxxflags(' + ', '.join(map(q, flags)) + ', {force = true})']
        if args.sanitize:
            lua.append('    add_ldflags("-fsanitize=address,undefined", {force = true})')
        for source in ('src/fs/ext4_fs.cpp', 'tests/host/ext4_maintained_times_tests.cpp', 'tests/host/ext4_realtime_model.cpp'):
            lua.append('    add_files(' + q(str(snapshot / source)) + ')')
        (project / 'xmake.lua').write_text('\n'.join(lua) + '\n')
        run(['xmake', 'f', '-P', project, '-o', output / 'build', '-m', 'release', '--toolchain=gcc', '-y'], 'configure', project)
        run(['xmake', '-P', project, '-v', '-y'], 'build', project)
        binary = output / 'bin/ext4-maintained-times'
        report['binary_sha256'] = sha(binary)
        for size in (1024, 2048, 4096):
            for stride in (128, 256):
                label = f'block{size}-inode{stride}'
                folder = output / label
                folder.mkdir()
                image = folder / 'input.img'
                run(['mke2fs', '-q', '-t', 'ext4', '-b', size, '-I', stride,
                     '-O', 'none,extent,filetype,sparse_super,large_file',
                     '-E', 'lazy_itable_init=0', '-m', '0', image, '16M'], label + '-mkfs')
                seed_distinct_times(image)
                original = sha(image)
                log = run([binary, image, folder], label + '-test')
                assert log.count('PASS maintained ext4 hosted timestamps') == 1 and sha(image) == original
                counts = [json.loads(line.removeprefix('COUNTS ')) for line in log.splitlines() if line.startswith('COUNTS ')]
                assert len(counts) == 1
                case = {'block_size': size, 'inode_size': stride, 'status': 'verifying', 'counts': counts[0],
                        'raw_timestamp_records': {}, 'fsck_images': []}
                names = ['prepared', 'events', 'growth', 'symlink-variants', 'node-cache', 'file-extents', 'directory-extents']
                if stride == 256:
                    names += [f'mixed-{extent}' for extent in (0, 4, 8, 12, 16, 24, 128)]
                for name in names:
                    case['raw_timestamp_records'][name] = verify_times(folder / (name + '.img'), folder / (name + '-times.tsv'))
                # Fault outputs are uncertain states, never asserted fsck-clean.
                fsck_inputs = [image, folder / 'prepared.img', folder / 'events.img', folder / 'growth-initial.img', folder / 'growth.img']
                fsck_inputs += [folder / 'symlink-variants.img', folder / 'node-cache.img']
                fsck_inputs += [folder / f'{name}{suffix}.img' for name in ('file-extents', 'directory-extents') for suffix in ('-initial', '')]
                if stride == 256:
                    fsck_inputs += [folder / f'mixed-{extent}{suffix}.img' for extent in (0, 4, 8, 12, 16, 24, 128) for suffix in ('-initial', '')]
                fsck_inputs += sorted((folder / 'faults').glob('*-success.img'))
                for disk in fsck_inputs:
                    identity = sha(disk)
                    run(['e2fsck', '-fn', disk], label + '-' + disk.stem + '-fsck')
                    assert sha(disk) == identity
                    case['fsck_images'].append({'path': str(disk.relative_to(output)), 'sha256': identity})
                faults = verify_faults(folder / 'faults')
                assert len(faults) == counts[0]['faults']
                fault_receipt = folder / 'fault-images.json'
                fault_receipt.write_text(json.dumps(faults, indent=2) + '\n')
                case.update(status='passed', fault_images_sha256=sha(fault_receipt))
                report['cases'].append(case)
                save()
                print('PASS', label, counts[0], flush=True)
        assert len(report['cases']) == 6
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        report['artifact_sha256'] = {str(p.relative_to(output)): sha(p) for p in sorted(output.rglob('*'))
                                     if p.is_file() and p != output / 'result.json'}
        save()


if __name__ == '__main__':
    main()
