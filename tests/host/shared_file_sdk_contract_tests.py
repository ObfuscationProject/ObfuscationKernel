"""Official producer contract refusals, independent of target compilation."""
from pathlib import Path
import importlib.util
import tempfile
import types
import unittest
from unittest.mock import patch

path = Path(__file__).resolve().parents[2] / "tools/export_sdk.py"
spec = importlib.util.spec_from_file_location("shared_file_sdk_exporter", path)
exporter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(exporter)

PROFILE = 'inline constexpr std::string_view native_shared_file_profile = "bootcpu-volatile-sealed-size-write-eager-pages-v3";\ninline constexpr std::string_view linux_shared_file_profile = "bootcpu-volatile-sealed-size-write-eager-pages-v3";\ninline constexpr u32 native_shared_file_module_abi = 78;\ninline constexpr usize native_shared_file_page_size = 4096;\ninline constexpr u32 native_shared_file_supported_seals = 15;\n'


class SharedFileProducerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.headers = Path(self.temp.name) / "include"
        for relative in exporter.SHARED_FILE_PROFILE_HEADERS:
            path = self.headers / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(PROFILE if relative == exporter.SHARED_FILE_PROFILE_HEADER else relative)
        self.profile = self.headers / exporter.SHARED_FILE_PROFILE_HEADER

    def test_exact_shared_file_contract_is_admitted(self):
        exporter.validate_shared_file_profile(self.headers)

    def test_missing_and_duplicate_declarations_are_refused(self):
        for line in PROFILE.splitlines(keepends=True):
            for source in (PROFILE.replace(line, ""), PROFILE + line):
                self.profile.write_text(source)
                with self.subTest(line=line, source=source), self.assertRaisesRegex(ValueError, "shared-file"):
                    exporter.validate_shared_file_profile(self.headers)

    def test_old_future_and_broadened_contracts_are_refused(self):
        for before, after in (("module_abi = 78", "module_abi = 59"),
                              ("module_abi = 78", "module_abi = 64"),
                              ("module_abi = 78", "module_abi = 65"),
                              ("module_abi = 78", "module_abi = 58"),
                              ("module_abi = 78", "module_abi = 79"),
                              ("write-eager-pages-v3", "write-eager-pages-v2"),
                              ("page_size = 4096", "page_size = 8192"),
                              ("supported_seals = 15", "supported_seals = 7"),
                              ("supported_seals = 15", "supported_seals = 31"),
                              ("volatile-sealed-size-write-eager-pages-v3", "persistent-full-v1"),
                              ("volatile-sealed-size-write-eager-pages-v3", "volatile-sealed-size-eager-pages-v1")):
            self.profile.write_text(PROFILE.replace(before, after))
            with self.subTest(before=before, after=after), self.assertRaisesRegex(ValueError, "shared-file"):
                exporter.validate_shared_file_profile(self.headers)

    def test_renamed_and_comment_only_declarations_are_refused(self):
        for source in (PROFILE.replace(" native_shared_file_", " bogus_native_shared_file_")
                              .replace(" linux_shared_file_", " bogus_linux_shared_file_"),
                       "// " + PROFILE.replace("\n", "\n// "), "/*\n" + PROFILE + "*/\n"):
            self.profile.write_text(source)
            with self.subTest(source=source), self.assertRaisesRegex(ValueError, "shared-file"):
                exporter.validate_shared_file_profile(self.headers)

    def test_each_named_dependency_is_required(self):
        for relative in exporter.SHARED_FILE_PROFILE_HEADERS:
            path = self.headers / relative
            content = path.read_bytes()
            path.unlink()
            with self.subTest(relative=relative), self.assertRaisesRegex(ValueError, "contract header is missing"):
                exporter.validate_shared_file_profile(self.headers)
            path.write_bytes(content)

    def test_official_export_refuses_stale_and_duplicate_module_abi(self):
        header = self.headers / "ok/core/module.hpp"
        header.parent.mkdir(parents=True)
        args = types.SimpleNamespace(headers=self.headers, mode="debug", arch="x86_64",
                                     define=[], asflag=[], cxxflag=[])
        for source in [f"inline constexpr u32 kernel_module_abi_version = {version};\n"
                       for version in (*range(1, 78), 79)] + [
                       "inline constexpr u32 kernel_module_abi_version = 78;\n" * 2,
                       "inline constexpr u32 unrelated = 59;\n"]:
            header.write_text(source)
            with patch.object(exporter, "native_signal_profile", return_value="bounded-bootcpu-v1"), \
                    self.subTest(source=source), self.assertRaisesRegex(ValueError, "ABI78 header"):
                exporter.export(args)


if __name__ == "__main__":
    unittest.main()
