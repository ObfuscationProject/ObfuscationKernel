"""SDK80 producer/consumer terminal and job-control admission models (UAPI 0.17); no target execution."""
from pathlib import Path
import hashlib
import importlib.util
import os
import re
import types
import unittest
from unittest.mock import patch

# OK_KERNEL_ROOT and OK_OS_ROOT name the producer and consumer checkouts; the
# defaults are this file's checkout and its ObfuscationOS sibling in the
# umbrella repository.
KERNEL_ROOT = Path(os.environ.get('OK_KERNEL_ROOT') or Path(__file__).resolve().parents[2]).resolve()
OS_ROOT = Path(os.environ.get('OK_OS_ROOT') or Path(__file__).resolve().parents[3] / 'ObfuscationOS').resolve()
CONSUMER_FOUND = (OS_ROOT / 'tools/kernel_sdk.py').is_file()


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


PRODUCER = load('native_tty_sdk_producer', KERNEL_ROOT / 'tools/export_sdk.py')
CONSUMER = load('native_tty_sdk_consumer', OS_ROOT / 'tools/kernel_sdk.py') if CONSUMER_FOUND else None
FIXTURES = load('native_tty_sdk_fixtures', OS_ROOT / 'tests/tools/test_kernel_sdk.py') if CONSUMER_FOUND else None
# (manifest key, profile constant name, header constant name, header list name, validator, diagnostic)
CONTRACTS = (
    ('native_tty_profile', 'NATIVE_TTY_PROFILE', 'NATIVE_TTY_PROFILE_HEADER', 'NATIVE_TTY_HEADERS',
     'validate_native_tty_profile', 'native TTY'),
    ('native_job_control_profile', 'NATIVE_JOB_CONTROL_PROFILE', 'NATIVE_JOB_CONTROL_PROFILE_HEADER',
     'NATIVE_JOB_CONTROL_HEADERS', 'validate_native_job_control_profile', 'native job control'),
)
SHARED = ('NATIVE_TTY_PROFILE', 'NATIVE_TTY_PROFILE_HEADER', 'NATIVE_TTY_HEADERS', 'NATIVE_TTY_DEFINES',
          'NATIVE_TERMIOS_DEFINES', 'NATIVE_TERMIOS_RECORDS', 'NATIVE_JOB_CONTROL_PROFILE',
          'NATIVE_JOB_CONTROL_PROFILE_HEADER', 'NATIVE_JOB_CONTROL_HEADERS', 'NATIVE_JOB_CONTROL_DEFINES',
          'NATIVE_JOB_CONTROL_RECORDS', 'NATIVE_PTY_HEADERS', 'NATIVE_CONSOLE_CONTROL_HEADERS',
          'NATIVE_CONSOLE_CONTROL_DEFINES', 'NATIVE_SESSIONS_DEFINES', 'NATIVE_CONSOLE_INPUT_PROFILE')


@unittest.skipUnless(CONSUMER_FOUND, 'ObfuscationOS consumer not found at ' + str(OS_ROOT) + '; set OK_OS_ROOT')
class NativeTtySdkContract(unittest.TestCase):
    def setUp(self):
        # The consumer's fixture SDK holds every header the chained validators
        # read; placeholder implementation bytes make no execution claim.
        self.fixture = FIXTURES.KernelSdkTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.sdk = self.fixture.sdk
        self.headers = self.sdk / 'include'

    def rehash(self, path):
        self.fixture.manifest['files'][path.relative_to(self.sdk).as_posix()] = \
            hashlib.sha256(path.read_bytes()).hexdigest()
        self.fixture.save()

    def refused(self, validator, diagnostic=None):
        for module in (PRODUCER, CONSUMER):
            with self.assertRaisesRegex(ValueError, diagnostic or '.'):
                getattr(module, validator)(self.headers)
        with self.assertRaises(ValueError):
            CONSUMER.validate_sdk(self.sdk, 'x86_64', 'debug')

    def test_exact_contracts_and_independent_fixture_agree(self):
        for name in SHARED:
            self.assertEqual(getattr(PRODUCER, name), getattr(CONSUMER, name), name)
        self.assertEqual(PRODUCER.NATIVE_TTY_DEFINES, dict(FIXTURES.TTY_DEFINITIONS))
        self.assertEqual(PRODUCER.NATIVE_JOB_CONTROL_DEFINES, dict(FIXTURES.JOB_CONTROL_DEFINITIONS))
        for _, _, header, _, validator, _ in CONTRACTS:
            text = (KERNEL_ROOT / 'include' / getattr(PRODUCER, header)).read_text()
            self.assertEqual(text, (self.headers / getattr(PRODUCER, header)).read_text())
            for module in (PRODUCER, CONSUMER):
                getattr(module, validator)(self.headers)
                getattr(module, validator)(KERNEL_ROOT / 'include')
        # Every other terminal marker moved with UAPI 0.17 while the module ABI stays 80.
        for header in ('ok/user/native_pty_profile.hpp', 'ok/user/native_console_control_profile.hpp',
                       'ok/user/native_sessions_profile.hpp', 'ok/user/native_group_signal_profile.hpp',
                       'ok/sched/console_input_profile.hpp'):
            self.assertEqual((KERNEL_ROOT / 'include' / header).read_text(), (self.headers / header).read_text(), header)
        manifest = CONSUMER.validate_sdk(self.sdk, 'x86_64', 'debug')
        self.assertEqual(manifest['abi_version'], 80)
        self.assertEqual(manifest['native_tty_profile'], PRODUCER.NATIVE_TTY_PROFILE)
        self.assertEqual(manifest['native_job_control_profile'], PRODUCER.NATIVE_JOB_CONTROL_PROFILE)

    def test_every_marker_requires_one_exact_typed_declaration(self):
        for _, profile, header, _, validator, _ in CONTRACTS:
            path = self.headers / getattr(PRODUCER, header)
            good = path.read_text()
            declarations = [line for line in good.splitlines(keepends=True) if line.startswith('inline constexpr')]
            for line in declarations:
                field = line.split(' = ', 1)[0].rsplit(' ', 1)[1]
                if 'u32 ' in line:
                    prefix, value = line.split(' = ')
                    changed = prefix + ' = ' + str(int(value.rstrip(';\n')) ^ 1) + ';\n'
                    retyped = line.replace('u32 ', 'int ')
                else:
                    changed = line.replace(getattr(PRODUCER, profile), 'full-linux-tty-smp-v1')
                    retyped = line.replace('std::string_view', 'auto')
                for source in (good.replace(line, ''), good + line, good.replace(line, '// ' + line),
                               good.replace(field, 'forged_' + field), good.replace(line, changed),
                               good.replace(line, line.replace('inline constexpr', 'extern')),
                               good.replace(line, retyped)):
                    path.write_text(source)
                    with self.subTest(header=header, field=field, source=source[-100:]):
                        self.refused(validator)
            for source in ('#if 1\n' + good + '#endif\n', '/*\n' + good + '*/\n', good + '\\', good + '??',
                           good + '%:', good.replace('namespace', 'namespacé'), good.replace('\n', ' ')):
                path.write_text(source)
                with self.subTest(header=header, decoy=source[:40]):
                    self.refused(validator)
            path.write_text(good)
        for _, _, _, _, validator, _ in CONTRACTS:
            getattr(PRODUCER, validator)(self.headers)

    def test_every_named_dependency_is_required(self):
        for _, _, _, headers, validator, _ in CONTRACTS:
            for name in getattr(PRODUCER, headers):
                path = self.headers / name
                source = path.read_bytes()
                path.unlink()
                with self.subTest(name=name):
                    self.refused(validator)
                path.write_bytes(source)
        CONSUMER.validate_sdk(self.sdk, 'x86_64', 'debug')

    def test_uapi_definitions_records_and_conditionals_are_exact(self):
        cases = (('ok/uapi/syscall.h', PRODUCER.NATIVE_TTY_DEFINES, 'validate_native_tty_profile'),
                 ('ok/uapi/termios.h', PRODUCER.NATIVE_TERMIOS_DEFINES, 'validate_native_tty_profile'),
                 ('ok/uapi/syscall.h', {'OK_NATIVE_JOB_CONTROL_VERSION': '1'}, 'validate_native_job_control_profile'),
                 ('ok/uapi/job_control.h', PRODUCER.NATIVE_JOB_CONTROL_DEFINES, 'validate_native_job_control_profile'))
        for relative, defines, validator in cases:
            path = self.headers / relative
            good = path.read_text()
            for name, value in defines.items():
                line = '#define ' + name + ' ' + value + '\n'
                self.assertIn(line, good)
                for replacement in ('', '// ' + line, line + line, '#define ' + name + ' 99\n',
                                    '#define ' + name + '(x) ' + value + '\n', '#if 0\n' + line + '#endif\n',
                                    line + '#undef ' + name + '\n', line.replace('#define', '#de\\\nfine')):
                    path.write_text(good.replace(line, replacement))
                    with self.subTest(header=relative, name=name, replacement=replacement):
                        self.refused(validator)
            path.write_text(good)
        # A definition or record visible only to C++ consumers is refused: the
        # language branches of termios.h may hold only matching assertions.
        path = self.headers / 'ok/uapi/termios.h'
        good = path.read_text()
        cxx, c = '#ifdef __cplusplus\n', '#else\n'
        moved = ['#define ' + name + ' ' + value + '\n' for name, value in PRODUCER.NATIVE_TERMIOS_DEFINES.items()]
        moved += re.findall(r'struct ok_(?:termios|winsize) \{[^}]*\};\n', good)
        self.assertEqual(len(moved), len(PRODUCER.NATIVE_TERMIOS_DEFINES) + len(PRODUCER.NATIVE_TERMIOS_RECORDS))
        sources = [good.replace(text, '').replace(cxx, cxx + text) for text in moved]
        sources += [good.replace(text, '').replace(c, c + text) for text in moved[:1] + moved[-1:]]
        sources += [good.replace(text, '').replace(cxx, cxx + text).replace(c, c + text) for text in moved[:1]]
        sources += [good.replace(cxx, cxx + 'typedef int ok_extra;\n'),
                    good.replace(c, c + 'typedef int ok_extra;\n'),
                    good.replace(cxx, cxx + 'static_assert(1, "x"); typedef int ok_extra;\n'),
                    good.replace(cxx, cxx + 'static_assert(1, "x");\n'),
                    re.sub(r'#else\n_Static_assert[^\n]*\n', '#else\n', good)]
        for source in sources:
            self.assertIn(cxx, source)
            self.assertNotEqual(source, good)
            path.write_text(source)
            with self.subTest(header='ok/uapi/termios.h', branch=source[source.index(cxx):][:160]):
                self.refused('validate_native_tty_profile')
        path.write_text(good)
        for relative, old, new, validator in (
                ('ok/uapi/termios.h', 'uint32_t c_lflag;', 'uint64_t c_lflag;', 'validate_native_tty_profile'),
                ('ok/uapi/termios.h', 'uint8_t c_line;\n', '', 'validate_native_tty_profile'),
                ('ok/uapi/termios.h', 'uint16_t ws_ypixel;', 'uint16_t ws_ypixel; uint16_t pad;', 'validate_native_tty_profile'),
                ('ok/uapi/termios.h', '#else\n', '#elif 1\n', 'validate_native_tty_profile'),
                ('ok/uapi/job_control.h', 'uint64_t flags;', 'uint32_t flags;', 'validate_native_job_control_profile'),
                ('ok/uapi/job_control.h', '#endif\n', '#ifdef OK_EXTRA\n#endif\n#endif\n', 'validate_native_job_control_profile'),
                ('ok/uapi/signal.h', 'OK_SIGTERM 15', 'OK_SIGTERM 14', 'validate_native_job_control_profile')):
            path = self.headers / relative
            good = path.read_text()
            self.assertIn(old, good)
            path.write_text(good.replace(old, new))
            with self.subTest(header=relative, mutation=new):
                self.refused(validator)
            path.write_text(good)
        CONSUMER.validate_sdk(self.sdk, 'x86_64', 'debug')

    def test_mixed_module_and_uapi_cohorts_are_refused(self):
        module = self.headers / 'ok/core/module.hpp'
        uapi = self.headers / 'ok/uapi/syscall.h'
        good_module, good_uapi = module.read_text(), uapi.read_text()
        for _, _, _, _, validator, _ in CONTRACTS:
            for version in (79, 81):
                module.write_text(good_module.replace('= 80;', f'= {version};'))
                with self.subTest(validator=validator, module_abi=version):
                    self.refused(validator, 'ABI80')
            module.write_text(good_module)
            for version in (16, 18):
                uapi.write_text(good_uapi.replace('MINOR 17', f'MINOR {version}'))
                with self.subTest(validator=validator, uapi_minor=version):
                    self.refused(validator, 'UAPI 0.17.0')
            uapi.write_text(good_uapi)
        # Each validator also re-checks the markers it builds on.
        for header, validators in (
                ('ok/user/native_pty_profile.hpp', ('validate_native_tty_profile',)),
                ('ok/user/native_console_control_profile.hpp', ('validate_native_tty_profile',)),
                ('ok/user/native_group_signal_profile.hpp', ('validate_native_tty_profile',
                                                             'validate_native_job_control_profile')),
                ('ok/user/native_sessions_profile.hpp', ('validate_native_tty_profile',
                                                         'validate_native_job_control_profile'))):
            path = self.headers / header
            good = path.read_text()
            path.write_text(good.replace('module_abi = 80', 'module_abi = 79'))
            for validator in validators:
                with self.subTest(dependency=header, validator=validator):
                    self.refused(validator)
            path.write_text(good)
        CONSUMER.validate_sdk(self.sdk, 'x86_64', 'debug')

    def test_full_consumer_requires_exact_manifest_profiles(self):
        for key, profile, _, _, _, label in CONTRACTS:
            for value in (None, False, 1, {}, [], '', getattr(PRODUCER, profile) + '-widened'):
                if value is None:
                    self.fixture.manifest.pop(key, None)
                else:
                    self.fixture.manifest[key] = value
                self.fixture.save()
                with self.subTest(key=key, value=value), self.assertRaisesRegex(ValueError, label + ' profile'):
                    CONSUMER.validate_sdk(self.sdk, 'x86_64', 'debug')
            self.fixture.manifest[key] = getattr(PRODUCER, profile)
            self.fixture.save()
        CONSUMER.validate_sdk(self.sdk, 'x86_64', 'debug')

    def test_full_consumer_requires_every_hashed_dependency(self):
        for _, _, _, headers, _, _ in CONTRACTS:
            for relative in getattr(PRODUCER, headers):
                name = 'include/' + relative
                path = self.sdk / name
                content = path.read_bytes()
                digest = self.fixture.manifest['files'].pop(name)
                self.fixture.save()
                with self.subTest(name=name, failure='missing digest'), self.assertRaisesRegex(ValueError, 'missing artifacts'):
                    CONSUMER.validate_sdk(self.sdk, 'x86_64', 'debug')
                self.fixture.manifest['files'][name] = digest
                self.fixture.save()
                path.write_bytes(content + b' changed')
                with self.subTest(name=name, failure='checksum'), self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                    CONSUMER.validate_sdk(self.sdk, 'x86_64', 'debug')
                path.write_bytes(content)
        for relative, old, new in (
                (PRODUCER.NATIVE_TTY_PROFILE_HEADER, 'module_abi = 80', 'module_abi = 79'),
                (PRODUCER.NATIVE_TTY_PROFILE_HEADER, 'native_tty_smp = 0', 'native_tty_smp = 1'),
                (PRODUCER.NATIVE_JOB_CONTROL_PROFILE_HEADER, 'native_job_control_native_handlers = 0',
                 'native_job_control_native_handlers = 1'),
                ('ok/uapi/termios.h', '#define OK_NCCS 19', '#define OK_NCCS 32'),
                ('ok/uapi/job_control.h', '#define OK_WCONTINUED 8', '#define OK_WCONTINUED 4'),
                ('ok/uapi/syscall.h', 'OK_NATIVE_TTY_VERSION 1', 'OK_NATIVE_TTY_VERSION 2')):
            name = 'include/' + relative
            path = self.sdk / name
            original = path.read_text()
            self.assertIn(old, original)
            path.write_text(original.replace(old, new))
            self.rehash(path)
            with self.subTest(name=name, mutation=new), self.assertRaises(ValueError):
                CONSUMER.validate_sdk(self.sdk, 'x86_64', 'debug')
            path.write_text(original)
            self.rehash(path)
        CONSUMER.validate_sdk(self.sdk, 'x86_64', 'debug')

    def test_export_refuses_missing_and_stale_contract_before_construction(self):
        scheduler = self.headers / 'ok/sched/scheduler.hpp'
        scheduler.write_text('user_init_pid = 1; create_init_process(); user_adoption_target();\n')
        args = types.SimpleNamespace(headers=self.headers, mode='debug', arch='x86_64',
                                     define=['OK_NATIVE_SIGNAL_RUNTIME'], asflag=[], cxxflag=[])
        for _, _, header, headers, _, label in CONTRACTS:
            for relative in getattr(PRODUCER, headers):
                if relative == 'ok/sched/scheduler.hpp':
                    continue
                path = self.headers / relative
                original = path.read_bytes()
                path.unlink()
                with patch.object(PRODUCER, 'construct_boot_archive') as construct, \
                        self.subTest(missing=relative), self.assertRaises((ValueError, FileNotFoundError)):
                    PRODUCER.export(args)
                construct.assert_not_called()
                path.write_bytes(original)
            profile = self.headers / getattr(PRODUCER, header)
            good = profile.read_text()
            for source in ('', good.replace('module_abi = 80', 'module_abi = 79')):
                profile.write_text(source)
                with patch.object(PRODUCER, 'construct_boot_archive') as construct, \
                        self.subTest(header=header, source=source[:40]), self.assertRaisesRegex(ValueError, label):
                    PRODUCER.export(args)
                construct.assert_not_called()
            profile.write_text(good)


if __name__ == '__main__':
    unittest.main()
