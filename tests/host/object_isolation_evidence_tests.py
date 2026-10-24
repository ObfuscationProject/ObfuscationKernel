#!/usr/bin/env python3
"""Hosted evidence-parser controls; these fixtures do not execute a guest."""
from pathlib import Path
import json
import re
import sys
import unittest
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'scripts'))
import test_native_object_isolation as harness

def replace_field(data, prefix, name, value):
    lines = data.splitlines(keepends=True)
    indices = [i for i, line in enumerate(lines) if line.startswith(prefix.encode())]
    if len(indices) != 1:
        raise ValueError('test construction requires unique prefix')
    i = indices[0]
    old = lines[i]
    lines[i], count = re.subn(rb' ' + name.encode() + rb'=0x[0-9a-f]{16}',
                             f' {name}=0x{value:016x}'.encode(), old)
    if count != 1 or lines[i] == old:
        raise ValueError('test construction did not change exactly one field')
    return b''.join(lines)


def cases(raw):
    for arch, data in raw.items():
        yield arch, 'valid-' + arch, data, True
        for prefix in ('OBJECT_BACKING', 'OBJECT_LIVE', 'OBJECT_CASE name=before',
                       'OBJECT_CASE name=protected', 'OBJECT_CASE name=after', 'OBJECT_RECLAIM', 'OBJECT_DONE'):
            selected = next(line for line in data.splitlines(keepends=True) if line.startswith(prefix.encode()))
            yield arch, 'missing-' + prefix, data.replace(selected, b''), False
            yield arch, 'duplicate-' + prefix, data.replace(selected, selected * 2), False
        yield arch, 'protected-wrong-fault-address', replace_field(data, 'OBJECT_CASE name=protected', 'fault', 7), False
        yield arch, 'protected-wrong-pc', replace_field(data, 'OBJECT_CASE name=protected', 'pc', 7), False
        yield arch, 'protected-no-fault', replace_field(data, 'OBJECT_CASE name=protected', 'kind', 0), False
        yield arch, 'protected-load-completed', replace_field(data, 'OBJECT_CASE name=protected', 'completed', 1), False
        yield arch, 'protected-sentinel-overwritten', replace_field(data, 'OBJECT_CASE name=protected', 'observed', 0xc3917a25), False
        yield arch, 'before-no-sample', replace_field(data, 'OBJECT_CASE name=before', 'observed', 0), False
        yield arch, 'after-no-completion', replace_field(data, 'OBJECT_CASE name=after', 'completed', 0), False
        yield arch, 'before-wrong-pointer', replace_field(data, 'OBJECT_CASE name=before', 'pointer', 0), False
        yield arch, 'after-wrong-syscall-vector', replace_field(data, 'OBJECT_CASE name=after', 'vector', 0xffff), False
        yield arch, 'retirement-failed', replace_field(data, 'OBJECT_RECLAIM', 'retired', 0), False
        yield arch, 'object-page-leaked', replace_field(data, 'OBJECT_RECLAIM', 'objects', 1), False
        yield arch, 'release-count-missing', replace_field(data, 'OBJECT_RECLAIM', 'releases', 3), False
        changed = replace_field(data, 'OBJECT_CASE name=protected', 'pc', 8)
        changed = replace_field(changed, 'OBJECT_CASE name=protected', 'load_pc', 8)
        yield arch, 'protected-pc-and-label-outside-user-code', changed, False
        changed = replace_field(data, 'OBJECT_CASE name=before', 'load_pc', 8)
        yield arch, 'inconsistent-load-label', changed, False
        lines = data.splitlines(keepends=True)
        at = next(i for i, line in enumerate(lines) if line.startswith(b'OBJECT_DONE'))
        yield arch, 'truncated-done', b''.join(lines[:at]) + lines[at].rstrip(b'\r\n'), False
        before = next(i for i, line in enumerate(lines) if line.startswith(b'OBJECT_CASE name=before'))
        after = next(i for i, line in enumerate(lines) if line.startswith(b'OBJECT_CASE name=after'))
        lines[before], lines[after] = lines[after], lines[before]
        yield arch, 'reversed-positive-controls', b''.join(lines), False
        yield arch, 'unknown-object-record', data + b'OBJECT_UNKNOWN status=pass\n', False
        if arch in ('mips', 'mips64', 'loongarch64'):
            yield arch, 'malformed-linker-range', data.replace(b'OBJECT_LIVE', b'OBJECT_RANGE corrupt\nOBJECT_LIVE'), False
        else:
            line = next(line for line in data.splitlines(keepends=True) if line.startswith(b'OBJECT_RANGE'))
            match = re.fullmatch(rb'OBJECT_RANGE base=(0x[0-9a-f]{16}) size=(0x[0-9a-f]{16})\r?\n', line)
            base, size = (int(value, 16) for value in match.groups())
            if size != 4194304:
                raise ValueError('review fixture assumptions require a single four-MiB range')
            def extent(begin, count):
                return f'OBJECT_RANGE base=0x{begin:016x} size=0x{count:016x}\n'.encode()
            yield arch, 'duplicate-overlapping-ranges', data.replace(line, extent(base, size // 2) * 2), False
            yield arch, 'unaligned-range-fragments', data.replace(line, extent(base, 1) + extent(base + 1, size - 1)), False
            yield arch, 'zero-sized-extra-range', data.replace(line, line + extent(base + size, 0)), False
            yield arch, 'range-after-live-object', data.replace(line, b'').replace(b'OBJECT_CASE name=before', line + b'OBJECT_CASE name=before'), False
        if arch in ('i386', 'x86_64'):
            for error in (1, 4, 7, 21):
                yield arch, 'x86-wrong-fault-error-' + str(error), replace_field(data, 'OBJECT_CASE name=protected', 'error', error), False
        if arch == 'aarch64':
            for error in (0x92000005, 0x9200004d):
                yield arch, 'aarch64-wrong-fault-error-' + str(error), replace_field(data, 'OBJECT_CASE name=protected', 'error', error), False


class ObjectIsolationEvidenceTests(unittest.TestCase):
    def test_retained_and_corrupted_records(self):
        fixture = ROOT/'tests/fixtures/object_isolation'
        layouts = json.loads((fixture/'layouts.json').read_text())
        self.assertEqual(set(layouts), set(harness.PROFILES))
        raw = {arch: (fixture/(arch+'.serial')).read_bytes() for arch in layouts}
        for arch, name, data, expected in cases(raw):
            with self.subTest(arch=arch, case=name):
                if expected:
                    self.assertEqual(harness.acceptance(arch, data, layouts[arch])['reclaim']['objects'], 0)
                else:
                    with self.assertRaises((ValueError, KeyError, UnicodeError)):
                        harness.acceptance(arch, data, layouts[arch])

    def test_layout_is_required_and_bounded(self):
        fixture = ROOT/'tests/fixtures/object_isolation'
        data = (fixture/'x86_64.serial').read_bytes()
        for layout in ({}, {'size':0,'load_offset':0}, {'size':4097,'load_offset':8},
                       {'size':30,'load_offset':30}, {'size':30,'load_offset':7}):
            with self.subTest(layout=layout), self.assertRaises(ValueError):
                harness.acceptance('x86_64', data, layout)

if __name__ == '__main__':
    unittest.main()
