"""Private ABI77 directory-create profile producer/consumer admission; no code loading."""
from pathlib import Path
import importlib.util
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value

PRODUCER = load('directory_create_sdk_producer', ROOT / 'ObfuscationKernel/tools/export_sdk.py')
CONSUMER = load('directory_create_sdk_consumer', ROOT / 'ObfuscationOS/tools/kernel_sdk.py')
PROFILE = (ROOT / 'ObfuscationKernel/include' / PRODUCER.DIRECTORY_CREATE_PROFILE_HEADER).read_text()
SDK_FIXTURES = load('directory_create_full_sdk_fixture', ROOT / 'ObfuscationOS/tests/tools/test_kernel_sdk.py')

class DirectoryCreateSdkContract(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.headers = Path(self.temporary.name) / 'include'
        for name in PRODUCER.DIRECTORY_CREATE_PROFILE_HEADERS:
            path = self.headers / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(PROFILE if name == PRODUCER.DIRECTORY_CREATE_PROFILE_HEADER else name)
        self.profile = self.headers / PRODUCER.DIRECTORY_CREATE_PROFILE_HEADER

    def accepted(self):
        PRODUCER.validate_directory_create_profile(self.headers)
        CONSUMER.validate_directory_create_profile(self.headers)

    def refused(self):
        for validator in (PRODUCER.validate_directory_create_profile, CONSUMER.validate_directory_create_profile):
            with self.assertRaises(ValueError):
                validator(self.headers)

    def test_exact_profile_admitted_by_both_boundaries(self):
        self.assertEqual(PRODUCER.DIRECTORY_CREATE_PROFILE_HEADERS, CONSUMER.DIRECTORY_CREATE_PROFILE_HEADERS)
        self.accepted()

    def test_all_required_named_headers_are_mandatory(self):
        for name in PRODUCER.DIRECTORY_CREATE_PROFILE_HEADERS:
            path = self.headers / name
            data = path.read_text()
            path.unlink()
            with self.subTest(header=name):
                self.refused()
            path.write_text(data)

    def test_missing_duplicate_conditional_and_macro_declarations_are_refused(self):
        for line in PROFILE.splitlines(keepends=True):
            if not line.startswith('inline constexpr'):
                continue
            for mutated in (PROFILE.replace(line, ''), PROFILE + line,
                            '#if 1\n' + PROFILE + '\n#endif\n',
                            '#define ext4_directory_create_module_abi 77\n' + PROFILE):
                self.profile.write_text(mutated)
                with self.subTest(declaration=line, mutation=mutated):
                    self.refused()
        self.profile.write_text(PROFILE)
        self.accepted()

    def test_commented_forged_and_raw_literal_markers_are_refused(self):
        for mutated in ('// ' + PROFILE.replace('\n', '\n// '),
                        '/*\n' + PROFILE + '*/\n',
                        PROFILE.replace('ext4_directory_create_', 'fake_directory_create_'),
                        'constexpr auto text = R"markers(\n' + PROFILE + ')markers";\n'):
            self.profile.write_text(mutated)
            with self.subTest(mutation=mutated):
                self.refused()

    def test_old_future_and_broadened_bounded_contracts_are_refused(self):
        for before, after in (('module_abi = 77', 'module_abi = 62'),
                              ('module_abi = 77', 'module_abi = 64'),
                              ('module_abi = 77', 'module_abi = 65'),
                              ('module_abi = 77', 'module_abi = 78'),
                              ('home_capacity = 8', 'home_capacity = 9'),
                              ('owner_buffers = 9', 'owner_buffers = 8'),
                              ('single-block-directory-create-ordered-free-init-v2', 'general-directory-create-v1')):
            self.profile.write_text(PROFILE.replace(before, after))
            with self.subTest(before=before, after=after):
                self.refused()

    def full_fixture(self):
        # Reuse fixture construction only; completed prior test methods are not
        # replayed by this new profile's admission checks.
        fixture = SDK_FIXTURES.KernelSdkTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')
        return fixture

    def test_full_consumer_requires_exact_profile_manifest_and_sdk_identity(self):
        fixture = self.full_fixture()
        for name in ('ext4_directory_create_profile',):
            original = fixture.manifest[name]
            for value in (None, False, '', 'full-persistent-unix-v1'):
                if value is None:
                    fixture.manifest.pop(name, None)
                else:
                    fixture.manifest[name] = value
                fixture.save()
                with self.subTest(name=name, value=value), self.assertRaises(ValueError):
                    CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')
            fixture.manifest[name] = original
            fixture.save()
        for arch, mode in (('aarch64', 'debug'), ('x86_64', 'release')):
            with self.subTest(arch=arch, mode=mode), self.assertRaises(ValueError):
                CONSUMER.validate_sdk(fixture.sdk, arch, mode)
        for version in (62, 63, 64, 65, 68, 69, 70, 71, 75, 76, 78):
            fixture.manifest['abi_version'] = version
            fixture.save()
            with self.subTest(version=version), self.assertRaises(ValueError):
                CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')

    def test_full_consumer_requires_every_hashed_named_profile_artifact(self):
        fixture = self.full_fixture()
        for relative in PRODUCER.DIRECTORY_CREATE_PROFILE_HEADERS:
            name = 'include/' + relative
            path = fixture.sdk / name
            content = path.read_bytes()
            digest = fixture.manifest['files'][name]
            path.unlink()
            with self.subTest(name=name, refusal='missing'), self.assertRaises(ValueError):
                CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')
            path.write_bytes(content + b' changed')
            with self.subTest(name=name, refusal='hash'), self.assertRaises(ValueError):
                CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')
            path.write_bytes(content)
            fixture.manifest['files'].pop(name)
            fixture.save()
            with self.subTest(name=name, refusal='manifest'), self.assertRaises(ValueError):
                CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')
            fixture.manifest['files'][name] = digest
            fixture.save()
        CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')

if __name__ == '__main__':
    unittest.main()
