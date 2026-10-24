"""Test source parsing/admission independently of downloaded source availability."""
from __future__ import annotations
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from generate_linux_abi import (PROFILES, definitions, generate, linux_numbers, number,
                                ioctl_definitions, ioctl_integer, ioctl_layout,
                                console_ioctl_numbers, checked_console_ioctls)

LINUX_IOCTL = """
#define _IOC_NRBITS 8
#define _IOC_TYPEBITS 8
# define _IOC_SIZEBITS 14
# define _IOC_DIRBITS 2
#define _IOC_NRSHIFT 0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT+_IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT+_IOC_TYPEBITS)
#define _IOC_DIRSHIFT (_IOC_SIZESHIFT+_IOC_SIZEBITS)
# define _IOC_NONE 0U
# define _IOC_READ 2U
# define _IOC_WRITE 1U
#define _IOC(dir,type,nr,size) (((dir)<<_IOC_DIRSHIFT)|((type)<<_IOC_TYPESHIFT)|((nr)<<_IOC_NRSHIFT)|((size)<<_IOC_SIZESHIFT))
#define _IOC_TYPECHECK(t) (sizeof(t))
#define _IOR(type,nr,argtype) _IOC(_IOC_READ,(type),(nr),(_IOC_TYPECHECK(argtype)))
#define _IOW(type,nr,argtype) _IOC(_IOC_WRITE,(type),(nr),(_IOC_TYPECHECK(argtype)))
"""
LINUX_IOCTL_OVERRIDE = """
#define _IOC_SIZEBITS 13
#define _IOC_DIRBITS 3
#define _IOC_NONE 1U
#define _IOC_READ 2U
#define _IOC_WRITE 4U
#include <asm-generic/ioctl.h>
"""
MUSL_IOCTL = """
#define _IOC(a,b,c,d) ( ((a)<<30) | ((b)<<8) | (c) | ((d)<<16) )
#define _IOC_NONE 0U
#define _IOC_READ 2U
#define _IOC_WRITE 1U
#define _IOR(a,b,c) _IOC(_IOC_READ,(a),(b),sizeof(c))
#define _IOW(a,b,c) _IOC(_IOC_WRITE,(a),(b),sizeof(c))
"""
GENERIC_CONSOLE_IOCTL = """
#define TIOCSCTTY 0x540E
#define TIOCGSID 0x5429 /* session of FD */
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
"""
ENCODED_CONSOLE_IOCTL = """
#define TIOCSCTTY 0x5480
#define TIOCGSID 0x7416
#define TIOCGPGRP _IOR('t', 119, int)
#define TIOCSPGRP _IOW('t', 118, int)
"""


class LinuxAbiGeneratorTests(unittest.TestCase):
    def test_console_ioctl_profiles_errno_and_source_pins(self):
        manifest = json.loads((ROOT / 'docs/abi/linux-profiles.json').read_text())
        self.assertEqual(len(manifest['profiles']), 10)
        self.assertEqual(manifest['codec_revision'], 2)
        for row in manifest['profiles']:
            arch = row['architecture']
            mips = arch in ('mips', 'mips64')
            encoded = mips or arch == 'ppc'
            self.assertEqual(tuple(row['console_ioctl_numbers'].values()),
                (0x5480 if mips else 0x540e, 0x7416 if mips else 0x5429,
                 0x40047477 if encoded else 0x540f, 0x80047476 if encoded else 0x5410))
            self.assertEqual(row['console_ioctl_pid_bytes'], 4)
            self.assertEqual(row['linux_ioctl_crosschecks'], list(row['console_ioctl_numbers']))
            self.assertEqual(row['errno_numbers']['ENOTTY'], 25)
            self.assertIn('ENOTTY', row['linux_errno_crosschecks'])
        for name in ('include/uapi/asm-generic/ioctl.h', 'include/uapi/asm-generic/ioctls.h',
                     'arch/mips/include/uapi/asm/ioctl.h', 'arch/mips/include/uapi/asm/ioctls.h',
                     'arch/powerpc/include/uapi/asm/ioctl.h', 'arch/powerpc/include/uapi/asm/ioctls.h',
                     'fs/ioctl.c', 'drivers/tty/tty_jobctrl.c', 'drivers/tty/tty_io.c'):
            self.assertIn('linux/' + name, manifest['source_file_sha256'])
        for name in ('arch/generic/bits/ioctl.h', 'arch/mips/bits/ioctl.h',
                     'arch/mips64/bits/ioctl.h', 'arch/powerpc/bits/ioctl.h',
                     'include/sys/ioctl.h', 'include/unistd.h', 'include/termios.h',
                     'src/misc/ioctl.c', 'src/unistd/tcgetpgrp.c',
                     'src/unistd/tcsetpgrp.c', 'src/termios/tcgetsid.c'):
            self.assertIn('musl/' + name, manifest['source_file_sha256'])

    def test_console_ioctl_source_expressions_and_unsigned_high_bit_vectors(self):
        generic = checked_console_ioctls(GENERIC_CONSOLE_IOCTL, LINUX_IOCTL, None,
                                        MUSL_IOCTL + GENERIC_CONSOLE_IOCTL)
        self.assertEqual(tuple(generic.values()), (0x540e, 0x5429, 0x540f, 0x5410))
        musl = MUSL_IOCTL.replace('<<30', '<<29').replace('_IOC_NONE 0U', '_IOC_NONE 1U').replace('_IOC_WRITE 1U', '_IOC_WRITE 4U')
        values = checked_console_ioctls(ENCODED_CONSOLE_IOCTL, LINUX_IOCTL,
                                        LINUX_IOCTL_OVERRIDE, musl + ENCODED_CONSOLE_IOCTL)
        self.assertEqual(tuple(values.values()), (0x5480, 0x7416, 0x40047477, 0x80047476))
        self.assertGreater(values['TIOCSPGRP'], 0x7fffffff)

    def test_console_ioctl_macro_parser_rejects_unchecked_expansion_and_duplicates(self):
        layout = ioctl_layout(LINUX_IOCTL, LINUX_IOCTL_OVERRIDE)
        for bad in ("_IOW('t', 118, long)", "_IOW('t', 118, unsigned int)",
                    "_IOW('t', 256, int)", "_IOWR('t', 118, int)", "OTHER", "0-1",
                    "__import__('os').system('false')"):
            with self.subTest(expression=bad), self.assertRaises(ValueError):
                console_ioctl_numbers(ENCODED_CONSOLE_IOCTL.replace("_IOW('t', 118, int)", bad), layout)
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            ioctl_definitions(GENERIC_CONSOLE_IOCTL + '#define TIOCSCTTY 0x540e\n')
        with self.assertRaisesRegex(ValueError, 'unbalanced'):
            ioctl_definitions('/* unterminated\n#define TIOCSCTTY 0x540e\n')
        macros, _ = ioctl_definitions('/*\n#define TIOCSCTTY 0x9999\n*/\n' + GENERIC_CONSOLE_IOCTL)
        self.assertEqual(macros['TIOCSCTTY'], '0x540E')

    def test_console_ioctl_function_layout_and_crosscheck_mutations_refused(self):
        for source in (LINUX_IOCTL.replace('(_IOC_READ,', '(_IOC_WRITE,'),
                       LINUX_IOCTL.replace('_IOC_NRBITS 8', '_IOC_NRBITS 7'),
                       LINUX_IOCTL.replace('_IOC_TYPECHECK(argtype)', 'sizeof(long)'),
                       LINUX_IOCTL.replace('(nr)<<_IOC_NRSHIFT', '(size)<<_IOC_NRSHIFT')):
            with self.subTest(source=source), self.assertRaises(ValueError):
                ioctl_layout(source)
        with self.assertRaisesRegex(ValueError, 'override'):
            ioctl_layout(LINUX_IOCTL, LINUX_IOCTL_OVERRIDE.replace('#include <asm-generic/ioctl.h>', ''))
        with self.assertRaisesRegex(ValueError, 'command mismatch'):
            checked_console_ioctls(GENERIC_CONSOLE_IOCTL, LINUX_IOCTL, None,
                                   MUSL_IOCTL + GENERIC_CONSOLE_IOCTL.replace('0x5429', '0x5428'))
        with self.assertRaises(ValueError):
            ioctl_layout(MUSL_IOCTL.replace('<<30', '<<28'), musl=True)
        with self.assertRaisesRegex(ValueError, 'layout mismatch'):
            checked_console_ioctls(GENERIC_CONSOLE_IOCTL, LINUX_IOCTL, LINUX_IOCTL_OVERRIDE,
                                   MUSL_IOCTL + GENERIC_CONSOLE_IOCTL)

    def test_console_ioctl_integer_reader_has_no_general_expression_execution(self):
        self.assertEqual(ioctl_integer('(SHIFT + 8)', {'SHIFT': '8U'}), 16)
        for expression, macros in [('True', {}), ('1<<32', {}), ('0-1', {}),
                                   ('4294967296', {}), ('1/0', {}), ('A', {'A': 'B', 'B': 'A'}),
                                   ("__import__('os').system('false')", {})]:
            with self.subTest(expression=expression), self.assertRaises(ValueError):
                ioctl_integer(expression, macros)

    def test_session_numbers_and_pinned_lifecycle_sources(self):
        manifest = json.loads((ROOT / 'docs/abi/linux-profiles.json').read_text())
        operations = ('setpgid', 'getpgid', 'getpgrp', 'setsid', 'getsid')
        legacy = (57, 132, 65, 66, 147)
        generic = (154, 155, None, 157, 156)
        expected = {'i386': legacy, 'x86_64': (109, 121, 111, 112, 124),
                    'aarch64': generic, 'arm32': legacy, 'rv64': generic,
                    'rv32': generic, 'loongarch64': generic,
                    'mips': (4057, 4132, 4065, 4066, 4151),
                    'mips64': (5107, 5119, 5109, 5110, 5122), 'ppc': legacy}
        self.assertEqual({row['architecture'] for row in manifest['profiles']}, set(expected))
        for row in manifest['profiles']:
            with self.subTest(architecture=row['architecture']):
                self.assertEqual(tuple(row['syscall_numbers'][name] for name in operations),
                                 expected[row['architecture']])
                for name in operations:
                    self.assertEqual(name in row['linux_number_crosschecks'],
                                     row['syscall_numbers'][name] is not None)
                self.assertEqual(row['errno_numbers']['ESRCH'], 3)
                self.assertIn('ESRCH', row['linux_errno_crosschecks'])
        for name in operations:
            self.assertIn('musl/src/unistd/' + name + '.c', manifest['source_file_sha256'])
        for path in ('musl/include/unistd.h', 'linux/kernel/sys.c', 'linux/fs/exec.c',
                     'linux/kernel/fork.c', 'linux/kernel/exit.c', 'linux/include/linux/pid.h',
                     'linux/include/linux/sched/signal.h'):
            self.assertIn(path, manifest['source_file_sha256'])

    def test_rename_numbers_and_pinned_sources(self):
        manifest = json.loads((ROOT / 'docs/abi/linux-profiles.json').read_text())
        expected = {
            'i386': (38, 302, 353), 'x86_64': (82, 264, 316),
            'aarch64': (None, 38, 276), 'arm32': (38, 329, 382),
            'rv64': (None, None, 276), 'rv32': (None, None, 276),
            'loongarch64': (None, None, 276), 'mips': (4038, 4295, 4351),
            'mips64': (5080, 5254, 5311), 'ppc': (38, 293, 357),
        }
        for row in manifest['profiles']:
            with self.subTest(architecture=row['architecture']):
                self.assertEqual(tuple(row['syscall_numbers'][name] for name in
                                       ('rename', 'renameat', 'renameat2')),
                                 expected[row['architecture']])
                for name in ('rename', 'renameat', 'renameat2'):
                    self.assertEqual(name in row['linux_number_crosschecks'],
                                     row['syscall_numbers'][name] is not None)
        for name in ('musl/src/stdio/rename.c', 'musl/src/unistd/renameat.c',
                     'musl/include/stdio.h', 'linux/fs/namei.c',
                     'linux/include/uapi/linux/fs.h'):
            self.assertIn(name, manifest['source_file_sha256'])

    def test_removal_numbers_and_pinned_wrapper_sources(self):
        manifest = json.loads((ROOT / 'docs/abi/linux-profiles.json').read_text())
        # Literal native Linux 6.13.4 vectors, including absent legacy calls
        # on asm-generic profiles and the distinct MIPS syscall bases.
        expected = {
            'i386': (10, 301, 40), 'x86_64': (87, 263, 84),
            'aarch64': (None, 35, None), 'arm32': (10, 328, 40),
            'rv64': (None, 35, None), 'rv32': (None, 35, None),
            'loongarch64': (None, 35, None), 'mips': (4010, 4294, 4040),
            'mips64': (5085, 5253, 5082), 'ppc': (10, 292, 40),
        }
        for row in manifest['profiles']:
            with self.subTest(architecture=row['architecture']):
                self.assertEqual(tuple(row['syscall_numbers'][name] for name in
                                       ('unlink', 'unlinkat', 'rmdir')), expected[row['architecture']])
                for name in ('unlink', 'unlinkat', 'rmdir'):
                    self.assertEqual(name in row['linux_number_crosschecks'],
                                     row['syscall_numbers'][name] is not None)
        for name in ('unlink', 'unlinkat', 'rmdir'):
            self.assertIn('musl/src/unistd/' + name + '.c', manifest['source_file_sha256'])
        self.assertIn('linux/fs/namei.c', manifest['source_file_sha256'])

    def test_saved_identity_numbers_and_width_families(self):
        manifest = json.loads((ROOT / 'docs/abi/linux-profiles.json').read_text())
        operations = ('setuid','setgid','setreuid','setregid','setresuid','setresgid','getresuid','getresgid')
        generic = (146,144,145,143,147,149,148,150)
        legacy = (23,46,70,71,164,170,165,171)
        expected = {'i386':legacy, 'arm32':legacy, 'x86_64':(105,106,113,114,117,119,118,120),
                    'aarch64':generic, 'rv32':generic, 'rv64':generic, 'loongarch64':generic,
                    'mips':(4023,4046,4070,4071,4185,4190,4186,4191),
                    'mips64':(5103,5104,5111,5112,5115,5117,5116,5118),
                    'ppc':(23,46,70,71,164,169,165,170)}
        for row in manifest['profiles']:
            self.assertEqual(tuple(row['syscall_numbers'][name] for name in operations), expected[row['architecture']])
            wide = tuple(row['syscall_numbers'][name+'32'] for name in operations)
            self.assertEqual(wide, (213,214,203,204,208,210,209,211) if row['architecture'] in ('i386','arm32') else (None,)*8)
        for path in ('linux/kernel/sys.c','linux/kernel/uid16.c','linux/include/linux/highuid.h',
                     'linux/kernel/cred.c','linux/security/commoncap.c','musl/src/unistd/setxid.c',
                     'musl/src/misc/getresuid.c','musl/src/misc/getresgid.c'):
            self.assertIn(path, manifest['source_file_sha256'])

    def test_numeric_aliases_and_bounded_expressions(self):
        macros = definitions('#define BASE 4000\n#define CALL (BASE+4)\n#define ALIAS CALL\n#define HEX 0xf0005\n')
        self.assertEqual(number('CALL', macros), 4004)
        self.assertEqual(number('ALIAS', macros), 4004)
        self.assertEqual(number('HEX', macros), 0xf0005)
        self.assertIsNone(number('MISSING', macros))

    def test_reject_cycles_code_and_out_of_range_expressions(self):
        for macros in ({'A':'B','B':'A'}, {'A':"__import__('os').system('false')"},
                       {'A':'0-1'}, {'A':'4294967295'}, {'A':'1<<20'}, {'A':'B'}):
            with self.subTest(macros=macros), self.assertRaises(ValueError):
                number('A', macros)

    def test_wrong_musl_archive_is_rejected_before_source_use(self):
        with tempfile.TemporaryDirectory() as work:
            source = Path(work) / 'bad.tar.gz'
            source.write_bytes(b'not the pinned release')
            with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                generate(source, Path(work) / 'no-linux-checkout')

    def test_linux_abi_table_selection_and_mips_base(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            ppc = root / 'arch/powerpc/kernel/syscalls/syscall.tbl'
            ppc.parent.mkdir(parents=True)
            ppc.write_text('4 common write sys_write\n246 32 clock_gettime sys_clock_gettime32\n247 64 clock_gettime sys_clock_gettime\n999 spu special sys_special\n')
            numbers, _ = linux_numbers(root, 'ppc')
            self.assertEqual(numbers, {'write':4, 'clock_gettime':246})
            mips = root / 'arch/mips/kernel/syscalls/syscall_o32.tbl'
            mips.parent.mkdir(parents=True)
            mips.write_text('4 o32 write sys_write\n1 n64 write sys_write\n')
            numbers, _ = linux_numbers(root, 'mips')
            self.assertEqual(numbers, {'write':4004})

    def test_valueless_guard_does_not_consume_next_macro(self):
        macros = definitions('#define HEADER_GUARD\n#define EPERM 1\n#define EROFS 30\n')
        self.assertEqual(number('EPERM', macros), 1)
        self.assertEqual(number('EROFS', macros), 30)
        self.assertNotIn('HEADER_GUARD', macros)

    def test_chmod_numbers_errno_and_sources(self):
        manifest = json.loads((ROOT / 'docs/abi/linux-profiles.json').read_text())
        expected = {
            'i386': (15, 94, 306, 452), 'x86_64': (90, 91, 268, 452),
            'aarch64': (None, 52, 53, 452), 'arm32': (15, 94, 333, 452),
            'rv32': (None, 52, 53, 452), 'rv64': (None, 52, 53, 452),
            'loongarch64': (None, 52, 53, 452), 'mips': (4015, 4094, 4299, 4452),
            'mips64': (5088, 5089, 5258, 5452), 'ppc': (15, 94, 297, 452),
        }
        for row in manifest['profiles']:
            arch = row['architecture']
            self.assertEqual(tuple(row['syscall_numbers'][op] for op in
                                   ('chmod', 'fchmod', 'fchmodat', 'fchmodat2')), expected[arch])
            self.assertEqual(row['errno_numbers']['EROFS'], 30)
            self.assertEqual(row['errno_numbers']['EOPNOTSUPP'], 122 if arch in ('mips', 'mips64') else 95)
            self.assertEqual(row['linux_errno_crosschecks'], list(row['errno_numbers']))
        for name in ('musl/src/stat/chmod.c', 'musl/src/stat/fchmod.c',
                     'musl/src/stat/fchmodat.c', 'musl/src/internal/procfdname.c',
                     'linux/fs/attr.c', 'linux/fs/ext4/inode.c', 'linux/kernel/groups.c',
                     'linux/include/linux/fs.h', 'linux/include/linux/cred.h',
                     'linux/include/linux/uidgid.h', 'linux/include/linux/capability.h',
                     'linux/include/uapi/linux/fcntl.h',
                     'linux/include/uapi/asm-generic/errno-base.h',
                     'linux/include/uapi/asm-generic/errno.h'):
            self.assertIn(name, manifest['source_file_sha256'])

    def test_manifest_remains_complete_model_matrix(self):
        manifest = json.loads((ROOT / 'docs/abi/linux-profiles.json').read_text())
        self.assertEqual([row['architecture'] for row in manifest['profiles']], [row[0] for row in PROFILES])
        self.assertEqual(len(manifest['profiles']), 10)
        for row in manifest['profiles']:
            self.assertEqual(row['linux_qualification'], 'unqualified')
            self.assertEqual(row['execution'], 'host-codec-model-only')
            self.assertEqual(row['kernel_timespec_bytes'], 16)
            for operation, value in row['syscall_numbers'].items():
                if value is not None and operation != 'arm_set_tls':
                    self.assertIn(operation, row['linux_number_crosschecks'])
        for digest in manifest['source_file_sha256'].values():
            self.assertRegex(digest, '^[0-9a-f]{64}$')
        self.assertTrue(manifest['limitations'])

    def test_robust_list_numbers_and_sources(self):
        manifest = json.loads((ROOT / 'docs/abi/linux-profiles.json').read_text())
        expected = {
            'i386': (311, 312), 'x86_64': (273, 274),
            'aarch64': (99, 100), 'arm32': (338, 339),
            'rv64': (99, 100), 'rv32': (99, 100), 'loongarch64': (99, 100),
            'mips': (4309, 4310), 'mips64': (5268, 5269), 'ppc': (300, 299),
        }
        for row in manifest['profiles']:
            operations = ('set_robust_list', 'get_robust_list')
            self.assertEqual(tuple(row['syscall_numbers'][op] for op in operations),
                             expected[row['architecture']])
            for operation in operations:
                self.assertIn(operation, row['linux_number_crosschecks'])
        for name in ('linux/kernel/futex/syscalls.c', 'linux/kernel/futex/core.c',
                     'linux/include/uapi/linux/futex.h', 'linux/include/linux/threads.h',
                     'linux/Documentation/locking/robust-futex-ABI.rst',
                     'musl/src/internal/pthread_impl.h', 'musl/src/thread/pthread_create.c',
                     'musl/src/thread/pthread_mutex_trylock.c',
                     'musl/src/thread/pthread_mutexattr_setrobust.c'):
            self.assertIn(name, manifest['source_file_sha256'])

    def test_robust_list_x86_64_excludes_x32_numbers(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            table = root / 'arch/x86/entry/syscalls/syscall_64.tbl'
            table.parent.mkdir(parents=True)
            table.write_text('273 64 set_robust_list sys_set_robust_list\n'
                             '274 64 get_robust_list sys_get_robust_list\n'
                             '530 x32 set_robust_list compat_sys_set_robust_list\n'
                             '531 x32 get_robust_list compat_sys_get_robust_list\n')
            numbers, _ = linux_numbers(root, 'x86_64')
            self.assertEqual(numbers, {'set_robust_list': 273, 'get_robust_list': 274})

    def test_timeout_errno_matches_all_ten_reference_abis(self):
        manifest = json.loads((ROOT / 'docs/abi/linux-profiles.json').read_text())
        expected = {'i386': 110, 'x86_64': 110, 'aarch64': 110, 'arm32': 110,
                    'rv64': 110, 'rv32': 110, 'loongarch64': 110,
                    'mips': 145, 'mips64': 145, 'ppc': 110}
        self.assertEqual({row['architecture']: row['errno_numbers']['ETIMEDOUT']
                          for row in manifest['profiles']}, expected)
        for row in manifest['profiles']:
            self.assertIn('ETIMEDOUT', row['linux_errno_crosschecks'])
        for name in ('linux/arch/mips/include/uapi/asm/errno.h',
                     'linux/include/uapi/asm-generic/errno.h',
                     'musl/arch/mips/bits/errno.h', 'musl/arch/mips64/bits/errno.h',
                     'musl/arch/generic/bits/errno.h'):
            self.assertIn(name, manifest['source_file_sha256'])

    def test_signal_numbers_preserve_time64_and_legacy_absences(self):
        manifest = json.loads((ROOT / 'docs/abi/linux-profiles.json').read_text())
        operations = ('kill', 'tkill', 'tgkill', 'rt_sigpending', 'rt_sigsuspend',
                      'rt_sigtimedwait', 'rt_sigtimedwait_time64', 'sigaltstack',
                      'sigreturn', 'rt_sigqueueinfo', 'rt_tgsigqueueinfo')
        # Literal Linux 6.13.4 native-ABI vectors, independently checked against
        # musl 1.2.5; RV32 has no time32 wait and no legacy sigreturn.
        expected = {
            'i386': (37, 238, 270, 176, 179, 177, 421, 186, 119, 178, 335),
            'x86_64': (62, 200, 234, 127, 130, 128, None, 131, None, 129, 297),
            'aarch64': (129, 130, 131, 136, 133, 137, None, 132, None, 138, 240),
            'arm32': (37, 238, 268, 176, 179, 177, 421, 186, 119, 178, 363),
            'rv64': (129, 130, 131, 136, 133, 137, None, 132, None, 138, 240),
            'rv32': (129, 130, 131, 136, 133, None, 421, 132, None, 138, 240),
            'loongarch64': (129, 130, 131, 136, 133, 137, None, 132, None, 138, 240),
            'mips': (4037, 4236, 4266, 4196, 4199, 4197, 4421, 4206, 4119, 4198, 4332),
            'mips64': (5060, 5192, 5225, 5125, 5128, 5126, None, 5129, None, 5127, 5291),
            'ppc': (37, 208, 250, 175, 178, 176, 421, 185, 119, 177, 322),
        }
        for row in manifest['profiles']:
            arch = row['architecture']
            with self.subTest(architecture=arch):
                self.assertEqual(tuple(row['syscall_numbers'][op] for op in operations), expected[arch])
                for operation, value in zip(operations, expected[arch]):
                    self.assertEqual(operation in row['linux_number_crosschecks'], value is not None)
        for name in ('kill', 'raise', 'sigpending', 'sigsuspend', 'sigtimedwait',
                     'sigaltstack', 'sigqueue'):
            self.assertIn('musl/src/signal/' + name + '.c', manifest['source_file_sha256'])
        self.assertIn('musl/src/thread/pthread_kill.c', manifest['source_file_sha256'])


if __name__ == '__main__':
    unittest.main()
