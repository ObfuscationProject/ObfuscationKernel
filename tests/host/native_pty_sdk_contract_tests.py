"""SDK74 producer/consumer raw PTY admission models; no target execution."""
from pathlib import Path
import hashlib
import importlib.util
import tempfile
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


PRODUCER = load('native_pty_sdk_producer', ROOT / 'ObfuscationKernel/tools/export_sdk.py')
CONSUMER = load('native_pty_sdk_consumer', ROOT / 'ObfuscationOS/tools/kernel_sdk.py')
FIXTURES = load('native_pty_sdk_fixtures', ROOT / 'ObfuscationOS/tests/tools/test_kernel_sdk.py')
PROFILE = (ROOT / 'ObfuscationKernel/include' / PRODUCER.NATIVE_PTY_PROFILE_HEADER).read_text()
DECLARATIONS = ''.join(line for line in PROFILE.splitlines(keepends=True)
                       if line.startswith('inline constexpr'))


class NativePtySdkContract(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.headers = Path(self.temporary.name) / 'include'
        for name in PRODUCER.NATIVE_PTY_HEADERS:
            path = self.headers / name
            path.parent.mkdir(parents=True, exist_ok=True)
            # Placeholder implementation bytes belong only to this admission
            # fixture. They make no SchedulerState or execution claim.
            source = (PROFILE if name == PRODUCER.NATIVE_PTY_PROFILE_HEADER else
                      FIXTURES.UAPI_HEADER if name == 'ok/uapi/syscall.h' else
                      'inline constexpr u32 kernel_module_abi_version = 74;\n'
                      if name == 'ok/core/module.hpp' else name)
            path.write_text(source)
        self.profile = self.headers / PRODUCER.NATIVE_PTY_PROFILE_HEADER
        self.uapi = self.headers / 'ok/uapi/syscall.h'
        self.module = self.headers / 'ok/core/module.hpp'

    def accepted(self):
        PRODUCER.validate_native_pty_profile(self.headers)
        CONSUMER.validate_native_pty_profile(self.headers)

    def refused(self, diagnostic='(?:native PTY|ABI74|UAPI 0.16.0)'):
        for validator in (PRODUCER.validate_native_pty_profile,
                          CONSUMER.validate_native_pty_profile):
            with self.assertRaisesRegex(ValueError, diagnostic):
                validator(self.headers)

    def test_exact_profile_and_independent_fixture_agree(self):
        self.assertEqual(PRODUCER.NATIVE_PTY_PROFILE, CONSUMER.NATIVE_PTY_PROFILE)
        self.assertEqual(PRODUCER.NATIVE_PTY_HEADERS, CONSUMER.NATIVE_PTY_HEADERS)
        self.assertEqual(PRODUCER.NATIVE_PTY_DEFINES, dict(FIXTURES.PTY_DEFINITIONS))
        self.assertEqual(PRODUCER.NATIVE_PTY_DEFINES, CONSUMER.NATIVE_PTY_DEFINES)
        self.assertEqual(PROFILE, FIXTURES.PTY_HEADER)
        self.assertIn('native_sessions_pty = 0;', FIXTURES.SESSIONS_HEADER)
        self.assertIn('native_console_control_pty = 0;', FIXTURES.CONSOLE_CONTROL_HEADER)
        syscall = (ROOT / 'ObfuscationKernel/include/ok/syscall/syscall.hpp').read_text()
        self.assertIn('    pty_pair = 1035,\n', syscall)
        self.assertIn('    fsync = 74,\n', syscall)
        self.accepted()

    def test_every_named_dependency_is_required(self):
        for name in PRODUCER.NATIVE_PTY_HEADERS:
            path = self.headers / name
            source = path.read_bytes()
            path.unlink()
            with self.subTest(name=name):
                self.refused('native PTY header missing')
            path.write_bytes(source)
        self.accepted()

    def test_every_marker_requires_one_exact_typed_declaration(self):
        for line in DECLARATIONS.splitlines(keepends=True):
            field = line.split(' = ', 1)[0].rsplit(' ', 1)[1]
            for source in (PROFILE.replace(line, ''), PROFILE + line,
                           PROFILE.replace(line, '// ' + line),
                           PROFILE.replace(field, 'forged_' + field),
                           PROFILE.replace(line, line.replace('inline constexpr', 'extern')),
                           PROFILE.replace(line, line.replace('u32 ', 'int '))
                           if 'u32 ' in line else PROFILE.replace(line, line.replace('std::string_view', 'auto'))):
                self.profile.write_text(source)
                with self.subTest(field=field, source=source):
                    self.refused()
        self.profile.write_text(PROFILE)
        self.accepted()

    def test_wrong_stale_future_and_broadened_fields_are_refused(self):
        for line in DECLARATIONS.splitlines(keepends=True):
            if 'u32 ' not in line:
                mutated = line.replace(PRODUCER.NATIVE_PTY_PROFILE, 'full-linux-devpts-smp-v1')
            else:
                prefix, value = line.split(' = ')
                mutated = prefix + ' = ' + str(int(value.rstrip(';\n')) + 1) + ';\n'
            self.profile.write_text(PROFILE.replace(line, mutated))
            with self.subTest(line=line):
                self.refused()
        self.profile.write_text(PROFILE.replace('module_abi = 74', 'module_abi = 73'))
        self.refused()

    def test_preprocessor_literals_and_malformed_duplicates_are_refused(self):
        for source in FIXTURES.nondeclaration_marker_sources(DECLARATIONS):
            self.profile.write_text(source)
            with self.subTest(source=source):
                self.refused()
        for source in (PROFILE + '\v', PROFILE + '\\', PROFILE + '??',
                       PROFILE + '%:', PROFILE.replace('namespace', 'namespacé')):
            self.profile.write_text(source)
            with self.subTest(source=source):
                self.refused()

    def test_uapi_definitions_are_exact_and_unconditional(self):
        for name, value in FIXTURES.PTY_DEFINITIONS:
            line = '#define ' + name + ' ' + value + '\n'
            for source in (FIXTURES.UAPI_HEADER.replace(line, ''),
                           FIXTURES.UAPI_HEADER.replace(line, '// ' + line),
                           FIXTURES.UAPI_HEADER.replace(line, line + line),
                           FIXTURES.UAPI_HEADER.replace(line, line + '# define ' + name + ' 0\n'),
                           FIXTURES.UAPI_HEADER.replace(line, line + '#\tdefine ' + name + ' 0\n'),
                           FIXTURES.UAPI_HEADER.replace(line, '#define ' + name + ' 0\n'),
                           FIXTURES.UAPI_HEADER.replace(line, '#define ' + name + '(x) ' + value + '\n'),
                           FIXTURES.UAPI_HEADER.replace(line, '#if 0\n' + line + '#endif\n'),
                           FIXTURES.UAPI_HEADER.replace(line, line + '#undef ' + name + '\n'),
                           FIXTURES.UAPI_HEADER.replace(line, line.replace('#define', '#de\\\nfine'))):
                self.uapi.write_text(source)
                with self.subTest(name=name, source=source):
                    self.refused('(?:native PTY|UAPI version)')
        self.uapi.write_text(FIXTURES.UAPI_HEADER)
        self.accepted()

    def test_mixed_module_and_uapi_cohorts_are_refused(self):
        for version in (73, 75):
            self.module.write_text(f'inline constexpr u32 kernel_module_abi_version = {version};\n')
            with self.subTest(module_abi=version):
                self.refused('ABI74')
        self.module.write_text('inline constexpr u32 kernel_module_abi_version = 74;\n')
        for version in (15, 17):
            self.uapi.write_text(FIXTURES.UAPI_HEADER.replace('MINOR 16', f'MINOR {version}'))
            with self.subTest(uapi_minor=version):
                self.refused('UAPI 0.16.0')
        self.uapi.write_text(FIXTURES.UAPI_HEADER)
        self.accepted()

    def full_fixture(self):
        result = FIXTURES.KernelSdkTests()
        result.setUp()
        self.addCleanup(result.doCleanups)
        CONSUMER.validate_sdk(result.sdk, 'x86_64', 'debug')
        return result

    def test_full_consumer_requires_exact_manifest_profile(self):
        fixture = self.full_fixture()
        for value in (None, False, 1, {}, [], '', PRODUCER.NATIVE_PTY_PROFILE + '-widened'):
            if value is None:
                fixture.manifest.pop('native_pty_profile', None)
            else:
                fixture.manifest['native_pty_profile'] = value
            fixture.save()
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, 'native PTY profile'):
                CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')

    def test_full_consumer_requires_every_hashed_dependency(self):
        fixture = self.full_fixture()
        for relative in PRODUCER.NATIVE_PTY_HEADERS:
            name = 'include/' + relative
            path = fixture.sdk / name
            content = path.read_bytes()
            digest = fixture.manifest['files'].pop(name)
            fixture.save()
            with self.subTest(name=name, failure='missing digest'), self.assertRaisesRegex(ValueError, 'missing artifacts'):
                CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')
            fixture.manifest['files'][name] = digest
            fixture.save()
            path.unlink()
            with self.subTest(name=name, failure='missing file'), self.assertRaisesRegex(ValueError, 'artifact is missing'):
                CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')
            path.write_bytes(content + b' changed')
            with self.subTest(name=name, failure='checksum'), self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')
            path.write_bytes(content)
        CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')

    def test_full_consumer_refuses_rehashed_stale_and_mixed_contracts(self):
        fixture = self.full_fixture()
        for relative, old, new in (
                (PRODUCER.NATIVE_PTY_PROFILE_HEADER, 'module_abi = 74', 'module_abi = 73'),
                (PRODUCER.NATIVE_PTY_PROFILE_HEADER, 'native_pty_termios = 0', 'native_pty_termios = 1'),
                ('ok/uapi/syscall.h', 'OK_SYS_PTY_PAIR 1035ULL', 'OK_SYS_PTY_PAIR 1036ULL'),
                ('ok/uapi/syscall.h', 'MINOR 16', 'MINOR 15'),
                ('ok/core/module.hpp', 'version = 74', 'version = 73'),
                ('ok/user/native_sessions_profile.hpp', 'module_abi = 74', 'module_abi = 73'),
                ('ok/user/native_console_control_profile.hpp', 'module_abi = 74', 'module_abi = 73')):
            name = 'include/' + relative
            path = fixture.sdk / name
            original = path.read_text()
            self.assertIn(old, original)
            path.write_text(original.replace(old, new))
            fixture.manifest['files'][name] = hashlib.sha256(path.read_bytes()).hexdigest()
            fixture.save()
            with self.subTest(name=name, mutation=new), self.assertRaises(ValueError):
                CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')
            path.write_text(original)
            fixture.manifest['files'][name] = hashlib.sha256(path.read_bytes()).hexdigest()
            fixture.save()
        CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')

    def test_export_refuses_missing_and_stale_contract_before_construction(self):
        fixture = self.full_fixture()
        scheduler = fixture.sdk / 'include/ok/sched/scheduler.hpp'
        scheduler.write_text('user_init_pid = 1; create_init_process(); user_adoption_target();\n')
        args = types.SimpleNamespace(headers=fixture.sdk / 'include', mode='debug', arch='x86_64',
                                     define=['OK_NATIVE_SIGNAL_RUNTIME'], asflag=[], cxxflag=[])
        for relative in PRODUCER.NATIVE_PTY_HEADERS:
            path = args.headers / relative
            original = path.read_bytes()
            path.unlink()
            with patch.object(PRODUCER, 'construct_boot_archive') as construct, \
                    self.subTest(missing=relative), self.assertRaises((ValueError, FileNotFoundError)):
                PRODUCER.export(args)
            construct.assert_not_called()
            path.write_bytes(original)
        profile = args.headers / PRODUCER.NATIVE_PTY_PROFILE_HEADER
        for source in ('', PROFILE.replace('module_abi = 74', 'module_abi = 73')):
            profile.write_text(source)
            with patch.object(PRODUCER, 'construct_boot_archive') as construct, \
                    self.subTest(source=source), self.assertRaisesRegex(ValueError, 'native PTY'):
                PRODUCER.export(args)
            construct.assert_not_called()


if __name__ == '__main__':
    unittest.main()
