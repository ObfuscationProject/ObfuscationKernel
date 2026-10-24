"""Producer/consumer SDK74 wake-operation contract models; no target execution."""
from pathlib import Path
import importlib.util
import tempfile
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


PRODUCER = load('futex_wake_operation_sdk_producer', ROOT / 'ObfuscationKernel/tools/export_sdk.py')
CONSUMER = load('futex_wake_operation_sdk_consumer', ROOT / 'ObfuscationOS/tools/kernel_sdk.py')
PROFILE = (ROOT / 'ObfuscationKernel/include' / PRODUCER.FUTEX_WAKE_OPERATION_PROFILE_HEADER).read_text()
DECLARATIONS = ''.join(line for line in PROFILE.splitlines(keepends=True) if line.startswith('inline constexpr'))
SDK_FIXTURES = load('futex_wake_operation_full_sdk_fixture', ROOT / 'ObfuscationOS/tests/tools/test_kernel_sdk.py')


class FutexWakeOperationSdkContract(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.headers = Path(self.temporary.name) / 'include'
        for name in PRODUCER.FUTEX_WAKE_OPERATION_PROFILE_HEADERS:
            path = self.headers / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(PROFILE if name == PRODUCER.FUTEX_WAKE_OPERATION_PROFILE_HEADER else name)
        self.profile = self.headers / PRODUCER.FUTEX_WAKE_OPERATION_PROFILE_HEADER

    def accepted(self):
        PRODUCER.validate_futex_wake_operation_profile(self.headers)
        CONSUMER.validate_futex_wake_operation_profile(self.headers)

    def refused(self, diagnostic="futex wake-operation"):
        for validator in (PRODUCER.validate_futex_wake_operation_profile,
                          CONSUMER.validate_futex_wake_operation_profile):
            with self.assertRaisesRegex(ValueError, diagnostic):
                validator(self.headers)

    def test_exact_real_profile_admitted_at_both_boundaries(self):
        self.assertEqual(PRODUCER.FUTEX_WAKE_OPERATION_PROFILE_HEADERS,
                         CONSUMER.FUTEX_WAKE_OPERATION_PROFILE_HEADERS)
        self.assertEqual(PRODUCER.FUTEX_WAKE_OPERATION_PROFILE,
                         CONSUMER.FUTEX_WAKE_OPERATION_PROFILE)
        self.assertEqual(DECLARATIONS, SDK_FIXTURES.FUTEX_WAKE_OPERATION_HEADER)
        self.accepted()

    def test_every_named_dependency_is_required(self):
        for name in PRODUCER.FUTEX_WAKE_OPERATION_PROFILE_HEADERS:
            path = self.headers / name
            content = path.read_bytes()
            path.unlink()
            with self.subTest(name=name):
                self.refused()
            path.write_bytes(content)
        self.accepted()

    def test_missing_duplicate_and_untyped_markers_are_refused(self):
        for line in DECLARATIONS.splitlines(keepends=True):
            field = line.split(' = ', 1)[0].rsplit(' ', 1)[1]
            for mutated in (PROFILE.replace(line, ''), PROFILE + line,
                            PROFILE.replace(line, '// ' + line),
                            PROFILE.replace(field, 'forged_' + field),
                            PROFILE.replace(line, line.replace('inline constexpr', 'extern'))):
                self.profile.write_text(mutated)
                with self.subTest(field=field, mutation=mutated):
                    self.refused()
        self.profile.write_text(PROFILE)
        self.accepted()

    def test_conditional_macro_raw_literal_and_malformed_duplicates_are_refused(self):
        for mutated in SDK_FIXTURES.nondeclaration_marker_sources(DECLARATIONS):
            self.profile.write_text(mutated)
            with self.subTest(mutation=mutated):
                self.refused("(?:futex wake-operation|SDK74 contract markers must be unconditional and free of local macros|SDK74 contract markers cannot use physical line continuations)")
        # A documentary raw literal cannot substitute for a declaration, and
        # does not hide the real canonical declaration placed after it.
        self.profile.write_text('constexpr auto example = R"example(\n' +
                                DECLARATIONS + ')example";\n' + PROFILE)
        self.accepted()

    def test_predecessor_future_and_broadened_contracts_are_refused(self):
        for before, after in (('module_abi = 74', 'module_abi = 65'),
                              ('module_abi = 74', 'module_abi = 75'),
                              ('word_bits = 32', 'word_bits = 64'),
                              ('operand_bits = 12', 'operand_bits = 16'),
                              ('shift_bits = 5', 'shift_bits = 6'),
                              ('arithmetic_count = 5', 'arithmetic_count = 8'),
                              ('comparison_count = 6', 'comparison_count = 16'),
                              ('wake_passes = 2', 'wake_passes = 1'),
                              ('key_domains = 2', 'key_domains = 3'),
                              ('nonpositive_wake_minimum = 1', 'nonpositive_wake_minimum = 0'),
                              ('smp_atomic = 0', 'smp_atomic = 1'),
                              ('file_backed = 0', 'file_backed = 1'),
                              ('cross_process_clone_vm = 0', 'cross_process_clone_vm = 1'),
                              (PRODUCER.FUTEX_WAKE_OPERATION_PROFILE, 'smp-file-backed-full-posix-v1')):
            self.profile.write_text(PROFILE.replace(before, after))
            with self.subTest(before=before, after=after):
                self.refused()

    def full_fixture(self):
        fixture = SDK_FIXTURES.KernelSdkTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')
        return fixture

    def test_official_export_refuses_missing_and_stale_contract_before_construction(self):
        fixture = self.full_fixture()
        scheduler = fixture.sdk / 'include/ok/sched/scheduler.hpp'
        scheduler.write_text('user_init_pid = 1; create_init_process(); user_adoption_target();\n')
        args = types.SimpleNamespace(headers=fixture.sdk / 'include', mode='debug', arch='x86_64',
                                     define=['OK_NATIVE_SIGNAL_RUNTIME'], asflag=[], cxxflag=[])
        marker = args.headers / PRODUCER.FUTEX_WAKE_OPERATION_PROFILE_HEADER
        for source in (None, '', DECLARATIONS.replace('module_abi = 74', 'module_abi = 65')):
            if source is None:
                marker.unlink()
            else:
                marker.write_text(source)
            with patch.object(PRODUCER, 'construct_boot_archive') as construct, \
                    self.subTest(source=source), self.assertRaisesRegex(ValueError, 'futex wake-operation'):
                PRODUCER.export(args)
            construct.assert_not_called()


if __name__ == '__main__':
    unittest.main()
