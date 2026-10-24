"""Persistent-sync capability parsing only; no provider or hardware execution."""
from pathlib import Path
import importlib.util
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("persistent_export", ROOT / "tools/export_sdk.py")
EXPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXPORT)


class PersistentSyncContract(unittest.TestCase):
    def test_exact_profile_and_adversarial_sources(self):
        with tempfile.TemporaryDirectory(prefix="persistent-sync-contract-") as directory:
            headers = Path(directory)
            names = set(EXPORT.NATIVE_PERSISTENT_SYNC_HEADERS + EXPORT.NATIVE_CONSOLE_INPUT_HEADERS + EXPORT.NATIVE_CREDENTIALS_HEADERS)
            for name in names:
                target = headers / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / "include" / name, target)
            EXPORT.validate_native_persistent_sync_profile(headers)
            profile = headers / EXPORT.NATIVE_PERSISTENT_SYNC_PROFILE_HEADER
            canonical = profile.read_text()
            changes = [canonical.replace("device-flush-v1", "device-flush-v0"),
                       canonical.replace("version = 1", "version = 0"),
                       canonical.replace("device-flush-v1", "device-flush-v1/*different*/"),
                       canonical.replace("= 1;", "= 0/**/1;"),
                       "/*\n" + canonical + "*/\n", "#if 0\n" + canonical + "#endif\n",
                       'constexpr auto fake = R"(\n' + canonical + ')";\n',
                       canonical + "#define native_persistent_sync_version 1\n",
                       canonical.replace("native_persistent_sync_profile", "native_persistent_\\\nsync_profile")]
            changes += [canonical.replace("\n", separator) for separator in ("\u2028", "\u2029", "\x85", "\v", "\f", "\x1c")]
            changes += [canonical + control for control in ("\0", "é", "??/\n")]
            for line in canonical.split("\n"):
                if line.startswith("inline constexpr"):
                    changes += [canonical.replace(line, ""), canonical.replace(line, line + "\n" + line)]
            for changed in changes:
                with self.subTest(changed=changed):
                    profile.write_text(changed)
                    with self.assertRaises(ValueError): EXPORT.validate_native_persistent_sync_profile(headers)
            profile.write_text(canonical)
            uapi = headers / "ok/uapi/syscall.h"
            original = uapi.read_text()
            for line in ("#define OK_NATIVE_PERSISTENT_SYNC_VERSION 1", "#define OK_SYS_SYNC_PERSISTENT 1031ULL"):
                for changed in (original.replace(line, ""), original.replace(line, line + "0"),
                                original.replace(line, line + "\n" + line),
                                original.replace(line, "#if 0\n" + line + "\n#endif")):
                    uapi.write_text(changed)
                    with self.assertRaises(ValueError): EXPORT.validate_native_persistent_sync_profile(headers)
            uapi.write_text(original)
            for name in names:
                path = headers / name
                saved = path.read_bytes(); path.unlink()
                with self.subTest(missing=name), self.assertRaises((ValueError, FileNotFoundError)):
                    EXPORT.validate_native_persistent_sync_profile(headers)
                path.write_bytes(saved)
            EXPORT.validate_native_persistent_sync_profile(headers)


if __name__ == "__main__":
    unittest.main()
