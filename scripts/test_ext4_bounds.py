#!/usr/bin/env python3
"""Retain hosted ext4 parser and malformed-directory boundary regressions."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = out / 'source'
    files = {path for path in (root / 'include').rglob('*') if path.is_file()}
    files.update(root / name for name in (
        'src/fs/ext4.cpp', 'src/fs/ext4_fs.cpp', 'tests/host/filesystem_parser_bounds.cpp',
        'tests/host/filesystem_parser_bounds_xmake.lua', 'tests/host/ext4_realtime_model.cpp',
        'tests/host/ext4_realtime_model.hpp', 'scripts/test_ext4_bounds.py'))
    record = dict(status='running', scope='Hosted parser and directory I/O rejection; no guest or crash-recovery evidence',
                  sanitized=args.sanitize, source_sha256={}, commands=[])

    def save():
        (out / 'result.json').write_text(json.dumps(record, indent=2) + '\n')

    def run(argv, label):
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', XMAKE_ROOT='y',
                   XMAKE_GLOBALDIR=str(out / 'xmake-global'),
                   ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',
                   UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        argv = list(map(str, argv))
        log = out / (label + '.log')
        with log.open('wb') as stream:
            done = subprocess.run(argv, cwd=source, env=env, stdout=stream, stderr=subprocess.STDOUT)
        record['commands'].append(dict(argv=argv, returncode=done.returncode, log=log.name, log_sha256=sha(log)))
        save()
        if done.returncode:
            raise RuntimeError('failed command: ' + label)

    try:
        for path in sorted(files):
            relative = path.relative_to(root)
            target = source / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            digest = sha(path)
            shutil.copy2(path, target)
            if sha(target) != digest or sha(path) != digest:
                raise RuntimeError('source changed while retained: ' + str(relative))
            record['source_sha256'][str(relative)] = digest
        shutil.copyfile(source / 'tests/host/filesystem_parser_bounds_xmake.lua', source / 'xmake.lua')
        run(['xmake', 'f', '-P', source, '-c', '-y', '--ccache=n',
             '--sanitizers=' + ('y' if args.sanitize else 'n')], 'configure')
        run(['xmake', 'build', '-P', source, '-j2'], 'build')
        binary = source / 'artifacts/filesystem-parser-bounds'
        record['binary_sha256'] = sha(binary)
        run([binary], 'parser')
        payload = out / 'payload'
        payload.write_bytes(b'unchanged source\n')
        for size in (1024, 2048, 4096):
            disk = out / ('ext4-' + str(size) + '.img')
            run(['mke2fs', '-q', '-t', 'ext4', '-b', size, '-I', '256',
                 '-O', 'none,extent,filetype,sparse_super,large_file',
                 '-E', 'lazy_itable_init=0', '-m', '0', disk, '16M'], str(size) + '-mkfs')
            for index, command in enumerate(('mkdir /source-dir', 'mkdir /target-dir',
                                             'write ' + str(payload) + ' /source-file')):
                run(['debugfs', '-w', '-R', command, disk], str(size) + '-fixture-' + str(index))
            before = sha(disk)
            run([binary, disk], str(size) + '-directory')
            if sha(disk) != before:
                raise RuntimeError('directory rejection changed the source image')
            run(['e2fsck', '-fn', disk], str(size) + '-fsck')
        for relative, digest in record['source_sha256'].items():
            if sha(source / relative) != digest:
                raise RuntimeError('retained source changed: ' + relative)
        record['status'] = 'passed'
    except BaseException as error:
        record.update(status='failed', error=repr(error))
        save()
        raise
    save()
    print('PASS hosted ext4 parser bounds and 126 malformed-directory operations; sanitizer=' + str(args.sanitize))


if __name__ == '__main__':
    main()
