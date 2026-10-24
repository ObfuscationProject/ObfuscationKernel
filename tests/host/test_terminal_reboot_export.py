"""Exact producer reboot policy admission; no SDK publication or execution."""
from pathlib import Path
import argparse
import importlib.util
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('reboot_exporter', ROOT / 'tools/export_sdk.py')
exporter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(exporter)
ARCHES = ('i386', 'x86_64', 'aarch64', 'arm32', 'rv64', 'rv32', 'loongarch64', 'mips', 'mips64', 'ppc')
POLICY = 'OK_NATIVE_PC8042_REBOOT'


class TerminalRebootExportTests(unittest.TestCase):
    def test_all_twenty_default_tuples_and_only_explicit_x86_64_enable(self):
        for arch in ARCHES:
            for mode in ('debug', 'release'):
                with self.subTest(arch=arch, mode=mode):
                    self.assertEqual(exporter.terminal_reboot_backend(arch, []), 'unsupported')
                    if arch == 'x86_64':
                        self.assertEqual(exporter.terminal_reboot_backend(arch, [POLICY + '=1']), 'x86_64-pc-8042-v1')
                    else:
                        with self.assertRaises(ValueError): exporter.terminal_reboot_backend(arch, [POLICY + '=1'])

    def test_noncanonical_or_repeated_definition_is_refused(self):
        for definitions in ([POLICY], [POLICY + '=0'], [POLICY + '=01'], [POLICY + '=true'],
                            [POLICY + '=1', POLICY + '=1'], [POLICY + '(x)=1'], [' ' + POLICY + '=1']):
            with self.subTest(definitions=definitions), self.assertRaises(ValueError):
                exporter.terminal_reboot_backend('x86_64', definitions)

    def test_hidden_compiler_and_assembler_overrides_are_refused(self):
        for flags in (['-D' + POLICY], ['-D' + POLICY + '=1'], ['-D ' + POLICY + '=0'], ['-U' + POLICY],
                      ['-Wp,-D' + POLICY + '=1'], ['-Wp,-U,' + POLICY],
                      ['-D', POLICY + '=1'], ['-U', POLICY]):
            for kind in ('cxxflags', 'asflags'):
                with self.subTest(flags=flags, kind=kind), self.assertRaises(ValueError):
                    exporter.terminal_reboot_backend('x86_64', [], **{kind: flags})

    def test_policy_refusal_precedes_existing_sdk_mutation(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'sdk'
            output.mkdir()
            retained = output / 'manifest.json'
            retained.write_bytes(b'previous-sdk')
            args = argparse.Namespace(mode='release', define=[POLICY + '=0'], asflag=[], cxxflag=[],
                                      arch='x86_64', output=output)
            with self.assertRaisesRegex(ValueError, 'PC/8042'): exporter.export(args)
            self.assertEqual(retained.read_bytes(), b'previous-sdk')
            self.assertEqual(sorted(path.name for path in Path(directory).iterdir()), ['sdk'])


if __name__ == '__main__': unittest.main()
