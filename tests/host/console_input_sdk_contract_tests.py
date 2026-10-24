#!/usr/bin/env python3
"""Reject console lease SDK declarations that do not match the exported contract."""
from pathlib import Path
import importlib.util
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("console_export", ROOT / "tools/export_sdk.py")
EXPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXPORT)


class ConsoleInputContract(unittest.TestCase):
    def test_canonical_and_rejections(self):
        with tempfile.TemporaryDirectory(prefix="console-sdk-contract-") as directory:
            headers = Path(directory)
            for name in set(EXPORT.NATIVE_CONSOLE_INPUT_HEADERS + EXPORT.NATIVE_CREDENTIALS_HEADERS):
                destination = headers / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / "include" / name, destination)
            EXPORT.validate_native_console_input_profile(headers)
            profile = headers / EXPORT.NATIVE_CONSOLE_INPUT_PROFILE_HEADER
            canonical = profile.read_text()
            for name, text in {
                "old_profile": canonical.replace("noecho-v1", "noecho-v0"),
                "comment_impersonation": "/*\n" + canonical + "\n*/\n",
                "conditional": "#if 0\n" + canonical + "#endif\n",
                "redefinition": canonical + "#define native_console_epoch_limit 1\n",
                "limit": canonical.replace("0x3fffffff", "0xffffffff"),
                "splice": canonical.replace("native_console_input_profile", "native_console_input_\\\nprofile"),
                "trigraph": canonical + "??/\n",
                "nul": canonical + "\0\n",
                "nonascii": canonical + "// é\n",
            }.items():
                with self.subTest(profile=name):
                    profile.write_text(text)
                    with self.assertRaises(ValueError): EXPORT.validate_native_console_input_profile(headers)
            profile.write_text(canonical)
            uapi = headers / "ok/uapi/syscall.h"
            original = uapi.read_text()
            for name, value in EXPORT.NATIVE_CONSOLE_INPUT_DEFINES.items():
                line = "#define " + name + " " + value
                for change in (original.replace(line, line + "0"), original.replace(line, ""),
                               original.replace(line, line + "\n" + line),
                               original.replace(line, "#if 0\n" + line + "\n#endif")):
                    with self.subTest(uapi=name, change=change[-30:]):
                        uapi.write_text(change)
                        with self.assertRaises(ValueError): EXPORT.validate_native_console_input_profile(headers)
            uapi.write_text(original)
            for name in EXPORT.NATIVE_CONSOLE_INPUT_HEADERS:
                path = headers / name
                backup = path.read_bytes()
                path.unlink()
                with self.subTest(missing=name):
                    with self.assertRaises(ValueError): EXPORT.validate_native_console_input_profile(headers)
                path.write_bytes(backup)
            EXPORT.validate_native_console_input_profile(headers)


if __name__ == "__main__":
    unittest.main()
