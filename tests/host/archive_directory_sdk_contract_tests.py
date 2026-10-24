"""Bounded boot archive directory capability admission; no code execution claim."""
from pathlib import Path
import hashlib
import importlib.util
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


PRODUCER = load('archive_directory_producer', ROOT / 'ObfuscationKernel/tools/export_sdk.py')
CONSUMER = load('archive_directory_consumer', ROOT / 'ObfuscationOS/tools/kernel_sdk.py')
FIXTURE = load('archive_directory_sdk_fixture', ROOT / 'ObfuscationOS/tests/tools/test_kernel_sdk.py')
PROFILE = 'root-owned-ram-exact-sticky-sgid-v1'
HEADER = 'ok/fs/archive_directory_profile.hpp'
HEADERS = (HEADER, 'ok/fs/archive.hpp', 'ok/fs/vfs.hpp', 'ok/core/types.hpp')
SOURCE = ('#pragma once\n#include "ok/core/types.hpp"\n#include <string_view>\n'
          'namespace ok::fs\n{\n'
          'inline constexpr std::string_view boot_archive_directory_profile = "root-owned-ram-exact-sticky-sgid-v1";\n'
          'inline constexpr u32 boot_archive_directory_mode_mask = 03777;\n'
          'inline constexpr u32 boot_archive_regular_mode_mask = 0777;\n} // namespace ok::fs\n')


def misleading_sources():
    for line in SOURCE.splitlines(keepends=True):
        if not line.startswith('inline constexpr'):
            continue
        yield 'absent', SOURCE.replace(line, '')
        yield 'duplicate', SOURCE.replace(line, line + line)
        yield 'disabled', SOURCE.replace(line, '#if 0\n' + line + '#endif\n')
    for old, new in (('03777', '0777'), ('03777', '07777'), ('= 0777', '= 04777'),
                     ('namespace ok::fs', 'namespace unrelated'),
                     (PROFILE, 'root-owned-ram-setid-exec-v1')):
        yield 'wrong contract', SOURCE.replace(old, new)
    for separator in ('\u2028', '\u2029', '\x85', '\v', '\f', '\x1c'):
        yield 'false physical line separator', SOURCE.replace('\n', separator)
        yield 'unsupported leading control', separator + SOURCE
    yield 'comment inside profile literal', SOURCE.replace(PROFILE, PROFILE + '/*mismatch*/')
    yield 'comment splits preprocessing number', SOURCE.replace('03777', '03/**/777')
    yield 'comment', '/*\n' + SOURCE + '*/\n'
    yield 'raw literal', 'constexpr auto text = R"contract(\n' + SOURCE + ')contract";\n'
    yield 'conditional', '#if 0\n' + SOURCE + '#endif\n'
    yield 'macro', '#define boot_archive_directory_mode_mask other\n' + SOURCE
    yield 'spliced comment', '// hidden contract\\\n' + SOURCE
    yield 'trigraph', '// hidden contract??/\n' + SOURCE
    yield 'digraph', '%:if 0\n' + SOURCE + '%:endif\n'


class ArchiveDirectorySdkContract(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.headers = Path(self.temp.name)
        for name in HEADERS:
            path = self.headers / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(SOURCE if name == HEADER else name)

    def admit(self):
        PRODUCER.validate_boot_archive_directory_profile(self.headers)
        CONSUMER.validate_boot_archive_directory_profile(self.headers)

    def refuse(self):
        for validator in (PRODUCER.validate_boot_archive_directory_profile,
                          CONSUMER.validate_boot_archive_directory_profile):
            with self.assertRaises(ValueError):
                validator(self.headers)

    def test_exact_independent_contract_and_canonical_header(self):
        for module in (PRODUCER, CONSUMER):
            self.assertEqual(module.BOOT_ARCHIVE_DIRECTORY_PROFILE, PROFILE)
            self.assertEqual(module.BOOT_ARCHIVE_DIRECTORY_PROFILE_HEADERS, HEADERS)
            module.validate_boot_archive_directory_profile(ROOT / 'ObfuscationKernel/include')
        self.admit()

    def test_every_named_header_required(self):
        for name in HEADERS:
            path = self.headers / name
            saved = path.read_bytes()
            path.unlink()
            with self.subTest(name=name):
                self.refuse()
            path.write_bytes(saved)
        self.admit()

    def test_misleading_or_broadened_header_refused(self):
        path = self.headers / HEADER
        for label, source in misleading_sources():
            path.write_text(source)
            with self.subTest(label=label, source=source):
                self.refuse()

    def full_fixture(self):
        fixture = FIXTURE.KernelSdkTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')
        return fixture

    def test_full_sdk_manifest_requires_exact_capability(self):
        fixture = self.full_fixture()
        for value in (None, '', True, 'root-owned-ram-permissions-v0', 'setid-files-v1'):
            if value is None:
                fixture.manifest.pop('boot_archive_directory_profile', None)
            else:
                fixture.manifest['boot_archive_directory_profile'] = value
            fixture.save()
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, 'boot archive directory profile'):
                CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')

    def test_full_sdk_artifact_hash_and_inventory_required(self):
        fixture = self.full_fixture()
        for relative in HEADERS:
            name = 'include/' + relative
            path = fixture.sdk / name
            saved = path.read_bytes()
            digest = fixture.manifest['files'][name]
            path.unlink()
            with self.subTest(name=name, kind='missing'), self.assertRaises(ValueError):
                CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')
            path.write_bytes(saved + b' drift')
            with self.subTest(name=name, kind='hash'), self.assertRaises(ValueError):
                CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')
            path.write_bytes(saved)
            fixture.manifest['files'].pop(name)
            fixture.save()
            with self.subTest(name=name, kind='inventory'), self.assertRaises(ValueError):
                CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')
            fixture.manifest['files'][name] = digest
            fixture.save()
        CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')

    def test_full_sdk_rehashed_forgery_refused(self):
        fixture = self.full_fixture()
        name = 'include/' + HEADER
        path = fixture.sdk / name
        for label, source in misleading_sources():
            path.write_text(source)
            fixture.manifest['files'][name] = hashlib.sha256(path.read_bytes()).hexdigest()
            fixture.save()
            with self.subTest(label=label, source=source), self.assertRaisesRegex(ValueError, 'boot archive directory profile'):
                CONSUMER.validate_sdk(fixture.sdk, 'x86_64', 'debug')


if __name__ == '__main__':
    unittest.main()
