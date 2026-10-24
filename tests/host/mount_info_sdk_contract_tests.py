"""Standalone exporter controls for the read-only mount inventory capability."""
from pathlib import Path
import tempfile
import unittest

import test_native_signal_export as fixtures


class MountInfoExportTests(unittest.TestCase):
    def test_exact_profile_and_wire_and_rehashed_deception(self):
        with tempfile.TemporaryDirectory() as directory:
            headers = Path(directory)
            fixtures.write_pathname_and_directory_headers(headers)
            uapi = headers / 'ok/uapi/syscall.h'
            profile = headers / fixtures.export_sdk.NATIVE_MOUNT_INFO_PROFILE_HEADER
            good = uapi.read_text()
            marker = profile.read_text()
            validate = fixtures.export_sdk.validate_native_mount_info_profile
            validate(headers)
            cases = [(uapi, good.replace('MINOR 16', 'MINOR 10')),
                     (uapi, good.replace('#define OK_UAPI_VERSION_MINOR 16',
                         '#define OK_UAPI_VERSION_MINOR 10 /* wrong compiled version */\n'
                         '/*\n#define OK_UAPI_VERSION_MINOR 16\n*/')),
                     (uapi, good.replace('MINOR 16', 'MINOR 17')),
                     (uapi, good.replace('uint32_t generation;', 'uintptr_t generation;')),
                     (uapi, good.replace('char path[OK_MOUNT_INFO_PATH_CAPACITY];', 'char *path;')),
                     (uapi, good.replace('uint32_t state;', 'uint32_t state; uint32_t extra;')),
                     (uapi, good.replace('#endif', '#define path hidden\n#endif')),
                     (uapi, good.replace('#endif', '#define OK_SYS_MOUNT_INFO 1034ULL\n#endif')),
                     (profile, marker.replace('wire_bytes = 288', 'wire_bytes = 289')),
                     (profile, marker + marker), (profile, '#if 0\n' + marker + '#endif\n')]
            for index, (path, source) in enumerate(cases):
                uapi.write_text(good)
                profile.write_text(marker)
                path.write_text(source)
                with self.subTest(case=index), self.assertRaises(ValueError):
                    validate(headers)
            uapi.write_text(good)
            profile.write_text(marker)
            profile.unlink()
            with self.assertRaisesRegex(ValueError, 'header missing'):
                validate(headers)


if __name__ == '__main__':
    unittest.main()
