"""Producer/consumer SDK77 requeue contract models; no target execution."""
from pathlib import Path
import importlib.util
import hashlib
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


PRODUCER = load('futex_requeue_sdk_producer', ROOT / 'ObfuscationKernel/tools/export_sdk.py')
CONSUMER = load('futex_requeue_sdk_consumer', ROOT / 'ObfuscationOS/tools/kernel_sdk.py')
PROFILE = (ROOT / 'ObfuscationKernel/include' / PRODUCER.FUTEX_REQUEUE_PROFILE_HEADER).read_text()
DECLARATIONS = ''.join(line for line in PROFILE.splitlines(keepends=True) if line.startswith('inline constexpr'))
SDK_FIXTURES = load('futex_requeue_full_sdk_fixture', ROOT / 'ObfuscationOS/tests/tools/test_kernel_sdk.py')


class FutexRequeueSdkContract(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.headers = Path(self.temporary.name) / 'include'
        for name in PRODUCER.FUTEX_REQUEUE_PROFILE_HEADERS:
            path = self.headers / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(PROFILE if name == PRODUCER.FUTEX_REQUEUE_PROFILE_HEADER else name)
        self.profile = self.headers / PRODUCER.FUTEX_REQUEUE_PROFILE_HEADER

    def accepted(self):
        PRODUCER.validate_futex_requeue_profile(self.headers)
        CONSUMER.validate_futex_requeue_profile(self.headers)

    def refused(self, diagnostic="futex requeue"):
        for validator in (PRODUCER.validate_futex_requeue_profile,
                          CONSUMER.validate_futex_requeue_profile):
            with self.assertRaisesRegex(ValueError, diagnostic):
                validator(self.headers)

    def test_exact_real_profile_admitted_at_both_boundaries(self):
        self.assertEqual(PRODUCER.FUTEX_REQUEUE_PROFILE_HEADERS,
                         CONSUMER.FUTEX_REQUEUE_PROFILE_HEADERS)
        self.assertEqual(PRODUCER.FUTEX_REQUEUE_PROFILE,
                         CONSUMER.FUTEX_REQUEUE_PROFILE)
        self.assertEqual(DECLARATIONS, SDK_FIXTURES.FUTEX_REQUEUE_HEADER)
        self.accepted()

    def test_every_named_dependency_is_required(self):
        for name in PRODUCER.FUTEX_REQUEUE_PROFILE_HEADERS:
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
                self.refused("(?:futex requeue|SDK77 contract markers must be unconditional and free of local macros|SDK77 contract markers cannot use physical line continuations)")
        # A documentary raw literal cannot substitute for a declaration, and
        # does not hide the real canonical declaration placed after it.
        self.profile.write_text('constexpr auto example = R"example(\n' +
                                DECLARATIONS + ')example";\n' + PROFILE)
        self.accepted()

    def test_predecessor_future_and_broadened_contracts_are_refused(self):
        for before, after in (
                ('module_abi = 77', 'module_abi = 66'),
                ('module_abi = 77', 'module_abi = 78'),
                ('native_futex_requeue_word_bits = 32', 'native_futex_requeue_word_bits = 33'),
                ('native_futex_requeue_operation_count = 2', 'native_futex_requeue_operation_count = 3'),
                ('native_futex_requeue_linux_operation = 3', 'native_futex_requeue_linux_operation = 4'),
                ('native_futex_cmp_requeue_linux_operation = 4', 'native_futex_cmp_requeue_linux_operation = 5'),
                ('native_futex_requeue_private_flag = 128', 'native_futex_requeue_private_flag = 129'),
                ('native_futex_requeue_key_domains = 2', 'native_futex_requeue_key_domains = 3'),
                ('native_futex_requeue_waiter_capacity = 64', 'native_futex_requeue_waiter_capacity = 65'),
                ('native_futex_requeue_sequence_bits = 64', 'native_futex_requeue_sequence_bits = 65'),
                ('native_futex_requeue_original_continuation_preserved = 1', 'native_futex_requeue_original_continuation_preserved = 2'),
                ('native_futex_requeue_same_key_sequence_preserved = 1', 'native_futex_requeue_same_key_sequence_preserved = 2'),
                ('native_futex_requeue_zero_wake_minimum = 0', 'native_futex_requeue_zero_wake_minimum = 1'),
                ('native_futex_requeue_signed_negative_counts_rejected = 1', 'native_futex_requeue_signed_negative_counts_rejected = 2'),
                ('native_futex_requeue_transfer_pin_owners_retained = 1', 'native_futex_requeue_transfer_pin_owners_retained = 2'),
                ('native_futex_requeue_smp_atomic = 0', 'native_futex_requeue_smp_atomic = 1'),
                ('native_futex_requeue_priority_inheritance = 0', 'native_futex_requeue_priority_inheritance = 1'),
                ('native_futex_requeue_realtime_priority = 0', 'native_futex_requeue_realtime_priority = 1'),
                ('native_futex_requeue_file_backed = 0', 'native_futex_requeue_file_backed = 1'),
                ('native_futex_requeue_cross_process_clone_vm = 0', 'native_futex_requeue_cross_process_clone_vm = 1'),
                (PRODUCER.FUTEX_REQUEUE_PROFILE, "smp-file-backed-full-posix-v1")):
            self.profile.write_text(PROFILE.replace(before, after))
            with self.subTest(before=before, after=after):
                self.refused()

    def full_fixture(self):
        fixture = SDK_FIXTURES.KernelSdkTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')
        return fixture

    def test_full_OS_consumer_refuses_mismatched_manifest_profiles(self):
        fixture = self.full_fixture()
        for field in ('native_futex_requeue_profile', 'linux_futex_requeue_profile'):
            for value in (None, False, {}, '', PRODUCER.FUTEX_REQUEUE_PROFILE + '-widened'):
                fixture.manifest[field] = value
                fixture.save()
                with self.subTest(field=field, value=value), self.assertRaisesRegex(ValueError, 'futex requeue profile'):
                    CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')
            fixture.manifest[field] = PRODUCER.FUTEX_REQUEUE_PROFILE
            fixture.save()
        CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')

    def test_full_OS_consumer_requires_every_named_hashed_artifact(self):
        fixture = self.full_fixture()
        for relative in PRODUCER.FUTEX_REQUEUE_PROFILE_HEADERS:
            name = 'include/' + relative
            path = fixture.sdk / name
            content = path.read_bytes()
            digest = fixture.manifest['files'].pop(name)
            fixture.save()
            with self.subTest(header=relative, boundary='manifest'), self.assertRaisesRegex(ValueError, 'missing artifacts'):
                CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')
            fixture.manifest['files'][name] = digest
            fixture.save()
            path.unlink()
            with self.subTest(header=relative, boundary='missing file'), self.assertRaisesRegex(ValueError, 'artifact is missing'):
                CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')
            path.write_bytes(content + b' changed')
            with self.subTest(header=relative, boundary='checksum'), self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')
            path.write_bytes(content)
        CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')

    def test_full_OS_consumer_refuses_rehashed_malformed_source_contract(self):
        fixture = self.full_fixture()
        name = 'include/' + PRODUCER.FUTEX_REQUEUE_PROFILE_HEADER
        path = fixture.sdk / name
        for source in (DECLARATIONS.replace('module_abi = 77', 'module_abi = 66'),
                       DECLARATIONS.replace('module_abi = 77', 'module_abi = 78'),
                       DECLARATIONS.replace('native_futex_requeue_smp_atomic = 0', 'native_futex_requeue_smp_atomic = 1'),
                       DECLARATIONS.replace('native_futex_requeue_transfer_pin_owners_retained = 1', 'native_futex_requeue_transfer_pin_owners_retained = 0'),
                       DECLARATIONS + DECLARATIONS):
            path.write_text(source)
            fixture.manifest['files'][name] = hashlib.sha256(path.read_bytes()).hexdigest()
            fixture.save()
            with self.subTest(source=source), self.assertRaisesRegex(ValueError, 'futex requeue'):
                CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')

    def test_official_export_refuses_missing_and_stale_contract_before_construction(self):
        fixture = self.full_fixture()
        scheduler = fixture.sdk / 'include/ok/sched/scheduler.hpp'
        scheduler.write_text('user_init_pid = 1; create_init_process(); user_adoption_target();\n')
        args = types.SimpleNamespace(headers=fixture.sdk / 'include', mode='debug', arch='x86_64',
                                     define=['OK_NATIVE_SIGNAL_RUNTIME'], asflag=[], cxxflag=[])
        marker = args.headers / PRODUCER.FUTEX_REQUEUE_PROFILE_HEADER
        for source in (None, '', DECLARATIONS.replace('module_abi = 77', 'module_abi = 66')):
            if source is None:
                marker.unlink()
            else:
                marker.write_text(source)
            with patch.object(PRODUCER, 'construct_boot_archive') as construct, \
                    self.subTest(source=source), self.assertRaisesRegex(ValueError, 'futex requeue'):
                PRODUCER.export(args)
            construct.assert_not_called()


if __name__ == '__main__':
    unittest.main()
