#!/usr/bin/env python3
"""Generate bounded Linux ABI tables from explicit pinned musl/Linux source inputs."""
from __future__ import annotations
import argparse
import ast
import hashlib
import io
import json
from pathlib import Path
import re
import tarfile

MUSL_VERSION = '1.2.5'
MUSL_SHA256 = 'a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4'
PROFILES = [
    ('i386', 'i386', 4, False, 0, 0, [3, 1, 2, 6, 7, 5], 'negative', False, ['eax','ebx','ecx','edx','esi','edi','ebp']),
    ('x86_64', 'x86_64', 8, False, 0, 0, [5, 4, 1, 10, 8, 9], 'negative', False, ['rax','rdi','rsi','rdx','r10','r8','r9']),
    ('aarch64', 'aarch64', 8, False, 8, 0, [0, 1, 2, 3, 4, 5], 'negative', False, ['x8','x0','x1','x2','x3','x4','x5']),
    ('arm32', 'arm', 4, False, 7, 0, [0, 1, 2, 3, 4, 5], 'negative', True, ['r7','r0','r1','r2','r3','r4','r5']),
    ('rv64', 'riscv64', 8, False, 17, 10, [10, 11, 12, 13, 14, 15], 'negative', False, ['x17/a7','x10/a0','x11/a1','x12/a2','x13/a3','x14/a4','x15/a5']),
    ('rv32', 'riscv32', 4, False, 17, 10, [10, 11, 12, 13, 14, 15], 'negative', False, ['x17/a7','x10/a0','x11/a1','x12/a2','x13/a3','x14/a4','x15/a5']),
    ('loongarch64', 'loongarch64', 8, False, 11, 4, [4, 5, 6, 7, 8, 9], 'negative', False, ['r11/a7','r4/a0','r5/a1','r6/a2','r7/a3','r8/a4','r9/a5']),
    ('mips', 'mips', 4, True, 2, 2, [4, 5, 6, 7, -1, -1], 'mips_a3', True, ['r2/v0','r4/a0','r5/a1','r6/a2','r7/a3','stack+16','stack+20']),
    ('mips64', 'mips64', 8, True, 2, 2, [4, 5, 6, 7, 8, 9], 'mips_a3', False, ['r2/v0','r4/a0','r5/a1','r6/a2','r7/a3','r8/a4','r9/a5']),
    ('ppc', 'powerpc', 4, True, 0, 3, [3, 4, 5, 6, 7, 8], 'ppc_cr0_so', True, ['r0','r3','r4','r5','r6','r7','r8']),
]
OPERATIONS = ['read', 'write', 'readv', 'writev', 'openat', 'close', 'fstat', 'fstat64', 'newfstatat', 'fstatat64', 'statx', 'mmap', 'mmap2', 'munmap', 'mprotect', 'brk', 'exit', 'exit_group', 'set_tid_address', 'set_thread_area', 'arch_prctl', 'arm_set_tls', 'rt_sigaction', 'rt_sigprocmask', 'rt_sigreturn', 'futex', 'futex_time64', 'clock_gettime', 'clock_gettime32', 'clock_gettime64', 'getrandom', 'ioctl', 'fsync', 'fdatasync', 'getpid', 'getppid', 'gettid', 'getuid', 'geteuid', 'getgid', 'getegid', 'getuid32', 'geteuid32', 'getgid32', 'getegid32', 'fork', 'clone', 'execve', 'wait4', 'waitid', 'getcwd', 'chdir', 'fchdir', 'readlink', 'readlinkat', 'dup', 'dup2', 'dup3', 'fcntl', 'fcntl64', 'lseek', '_llseek', 'pipe', 'pipe2', 'nanosleep', 'nanosleep_time32', 'clock_nanosleep', 'clock_nanosleep_time32', 'clock_nanosleep_time64', 'clock_getres', 'clock_getres_time32', 'clock_getres_time64', 'poll', 'ppoll', 'ppoll_time64', 'open', 'creat', 'pread64', 'pwrite64', 'select', '_newselect', 'pselect6', 'pselect6_time64', 'getdents64', 'umask', 'mkdir', 'mkdirat', 'clock_settime', 'clock_settime32', 'clock_settime64', 'stat', 'lstat', 'chmod', 'fchmod', 'fchmodat', 'fchmodat2']
ERRORS = ['EPERM', 'ENOENT', 'EIO', 'EBADF', 'EAGAIN', 'ENOMEM', 'EACCES', 'EFAULT', 'EINVAL', 'ENOSYS', 'EOVERFLOW', 'EOPNOTSUPP', 'EROFS', 'ETIMEDOUT']
OPERATIONS += ['set_robust_list', 'get_robust_list']
OPERATIONS += ['setuid', 'setgid', 'setreuid', 'setregid', 'setresuid', 'setresgid', 'getresuid', 'getresgid',
               'setuid32', 'setgid32', 'setreuid32', 'setregid32', 'setresuid32', 'setresgid32', 'getresuid32', 'getresgid32']
OPERATIONS += ['kill', 'tkill', 'tgkill', 'rt_sigpending', 'rt_sigsuspend',
               'rt_sigtimedwait', 'rt_sigtimedwait_time64', 'sigaltstack',
               'sigreturn', 'rt_sigqueueinfo', 'rt_tgsigqueueinfo']
OPERATIONS += ['unlink', 'unlinkat', 'rmdir']
OPERATIONS += ['rename', 'renameat', 'renameat2']
OPERATIONS += ['truncate', 'ftruncate', 'truncate64', 'ftruncate64']
OPERATIONS += ['access', 'faccessat', 'faccessat2']
OPERATIONS += ['setpgid', 'getpgid', 'getpgrp', 'setsid', 'getsid']
OPERATIONS += ['uname', 'sysinfo', 'link', 'linkat', 'symlink', 'symlinkat', 'chown', 'lchown', 'fchown',
               'fchownat', 'chown32', 'lchown32', 'fchown32', 'vfork', 'statfs', 'statfs64', 'fstatfs',
               'fstatfs64', 'utimensat', 'utimensat_time64', 'mknod', 'mknodat']
OPERATIONS += ['getitimer', 'setitimer', 'alarm', 'timer_create', 'timer_settime', 'timer_settime32',
               'timer_settime64', 'timer_gettime', 'timer_gettime32', 'timer_gettime64',
               'timer_getoverrun', 'timer_delete']
ERRORS += ['ESRCH']
ERRORS += ['ENOTTY']
CONSOLE_IOCTLS = ['TIOCSCTTY', 'TIOCGSID', 'TIOCGPGRP', 'TIOCSPGRP']
ERRORS += ['ENXIO', 'ENOSPC']
# Order is the generated TtyIoctlOperation order.
TTY_IOCTLS = {'TCGETS': 'get_attributes', 'TCSETS': 'set_now', 'TCSETSW': 'set_drain', 'TCSETSF': 'set_flush',
              'TCSBRK': 'drain_break', 'TCFLSH': 'flush', 'TIOCGWINSZ': 'get_window', 'TIOCSWINSZ': 'set_window',
              'TIOCNOTTY': 'detach', 'FIONREAD': 'input_pending', 'TIOCGPTN': 'get_pty_number',
              'TIOCSPTLCK': 'set_pty_lock'}
# Generic termios names. Each named value becomes one map row; group values are
# matched under their group mask, so profiles with wider groups keep the extra
# encodings in the residue. CIBAUD rows are the CBAUD rows shifted by IBSHIFT.
TERMIOS_SINGLE = {
    'iflag': ['IGNBRK', 'BRKINT', 'IGNPAR', 'PARMRK', 'INPCK', 'ISTRIP', 'INLCR', 'IGNCR', 'ICRNL', 'IUCLC',
              'IXON', 'IXANY', 'IXOFF', 'IMAXBEL', 'IUTF8'],
    'oflag': ['OPOST', 'OLCUC', 'ONLCR', 'OCRNL', 'ONOCR', 'ONLRET', 'OFILL', 'OFDEL'],
    'cflag': ['CSTOPB', 'CREAD', 'PARENB', 'PARODD', 'HUPCL', 'CLOCAL', 'ADDRB', 'CMSPAR', 'CRTSCTS'],
    'lflag': ['ISIG', 'ICANON', 'XCASE', 'ECHO', 'ECHOE', 'ECHOK', 'ECHONL', 'NOFLSH', 'TOSTOP', 'ECHOCTL',
              'ECHOPRT', 'ECHOKE', 'FLUSHO', 'PENDIN', 'IEXTEN', 'EXTPROC'],
}
TERMIOS_BAUDS = ['B0', 'B50', 'B75', 'B110', 'B134', 'B150', 'B200', 'B300', 'B600', 'B1200', 'B1800', 'B2400',
                 'B4800', 'B9600', 'B19200', 'B38400', 'B57600', 'B115200', 'B230400', 'B460800', 'B500000',
                 'B576000', 'B921600', 'B1000000', 'B1152000', 'B1500000', 'B2000000', 'B2500000', 'B3000000',
                 'B3500000', 'B4000000', 'BOTHER']
TERMIOS_GROUPS = {
    'oflag': [('NLDLY', ['NL0', 'NL1']), ('CRDLY', ['CR0', 'CR1', 'CR2', 'CR3']),
              ('TABDLY', ['TAB0', 'TAB1', 'TAB2', 'TAB3']), ('BSDLY', ['BS0', 'BS1']),
              ('VTDLY', ['VT0', 'VT1']), ('FFDLY', ['FF0', 'FF1'])],
    'cflag': [('CBAUD', TERMIOS_BAUDS), ('CSIZE', ['CS5', 'CS6', 'CS7', 'CS8'])],
}
TERMIOS_FIELDS = ['iflag', 'oflag', 'cflag', 'lflag']
TERMIOS_CC = ['VINTR', 'VQUIT', 'VERASE', 'VKILL', 'VEOF', 'VTIME', 'VMIN', 'VSWTC', 'VSTART', 'VSTOP', 'VSUSP',
              'VEOL', 'VREPRINT', 'VDISCARD', 'VWERASE', 'VLNEXT', 'VEOL2']
# musl 1.2.5 defines neither; every other termios name must agree with musl.
TERMIOS_LINUX_ONLY = {'BOTHER': 'termios2 speed selector; musl has no BOTHER',
                      'ADDRB': 'address bit added in Linux 6.0; musl has no ADDRB'}
TERMIOS_TYPE_BYTES = {'unsigned char': 1, 'unsigned short': 2, 'unsigned int': 4}
LINUX_FILES = ['Makefile', 'fs/select.c', 'kernel/signal.c', 'include/uapi/asm-generic/poll.h', 'arch/mips/include/uapi/asm/poll.h', 'arch/mips/include/uapi/asm/signal.h', 'include/uapi/linux/time_types.h', 'include/uapi/asm-generic/siginfo.h', 'include/uapi/linux/wait.h', 'kernel/time/time.c', 'fs/file.c', 'fs/read_write.c', 'kernel/time/hrtimer.c', 'kernel/time/posix-timers.c', 'include/linux/ktime.h', 'fs/fcntl.c', 'include/uapi/asm-generic/fcntl.h', 'fs/d_path.c', 'fs/namei.c', 'fs/open.c', 'include/uapi/linux/fs.h', 'include/uapi/asm-generic/posix_types.h', 'arch/mips/kernel/scall32-o32.S', 'arch/mips/include/uapi/asm/errno.h', 'arch/powerpc/include/asm/syscall.h', 'arch/arm/include/uapi/asm/unistd.h', 'fs/pipe.c', 'arch/mips/kernel/syscall.c', 'fs/inode.c', 'include/linux/types.h', 'fs/stat.c', 'include/uapi/linux/stat.h', 'arch/x86/include/uapi/asm/stat.h', 'include/uapi/asm-generic/stat.h']


LINUX_FILES += ['kernel/sys.c', 'kernel/uid16.c', 'include/linux/highuid.h', 'fs/exec.c', 'kernel/cred.c']
LINUX_FILES += ['kernel/fork.c', 'kernel/exit.c', 'include/linux/pid.h', 'include/linux/sched/signal.h']
LINUX_FILES += ['kernel/time/itimer.c']
LINUX_FILES += ['fs/ioctl.c', 'drivers/tty/tty_jobctrl.c', 'drivers/tty/tty_io.c',
                'include/uapi/asm-generic/ioctl.h', 'include/uapi/asm-generic/ioctls.h',
                'arch/mips/include/uapi/asm/ioctl.h', 'arch/mips/include/uapi/asm/ioctls.h',
                'arch/powerpc/include/uapi/asm/ioctl.h', 'arch/powerpc/include/uapi/asm/ioctls.h']
LINUX_FILES += ['include/uapi/asm-generic/termbits.h', 'include/uapi/asm-generic/termbits-common.h',
                'include/uapi/asm-generic/termios.h', 'arch/mips/include/uapi/asm/termbits.h',
                'arch/powerpc/include/uapi/asm/termbits.h', 'arch/mips/include/uapi/asm/termios.h',
                'arch/powerpc/include/uapi/asm/termios.h', 'drivers/tty/tty_ioctl.c', 'drivers/tty/n_tty.c',
                'drivers/tty/tty_baudrate.c']


def sha(data):
    return hashlib.sha256(data).hexdigest()


def definitions(data):
    return dict(re.findall(r'^#define[ \t]+([A-Za-z_][A-Za-z_0-9]*)[ \t]+([^\n]+)', data, re.M))


def number(name, macros, seen=()):
    if name not in macros:
        return None
    if name in seen:
        raise ValueError('cyclic source macro ' + name)
    expression = macros[name].strip()
    def evaluate(node):
        if isinstance(node, ast.Constant) and type(node.value) is int:
            return node.value
        if isinstance(node, ast.Name):
            value = number(node.id, macros, (*seen, name))
            if value is not None:
                return value
        if isinstance(node, ast.BinOp) and isinstance(node.op, (ast.Add, ast.Sub)):
            left, right = evaluate(node.left), evaluate(node.right)
            return left + right if isinstance(node.op, ast.Add) else left - right
        raise ValueError(f'unsupported source macro expression {name}={expression}')
    value = evaluate(ast.parse(expression, mode='eval').body)
    if value < 0 or value >= 0xffffffff:
        raise ValueError('source macro outside syscall number range: ' + name)
    return value


def ioctl_definitions(data):
    """Read only the bounded ioctl macros; reject duplicate relevant definitions."""
    data = re.sub(r'/\*.*?\*/', '', data, flags=re.S)
    if '/*' in data or '*/' in data:
        raise ValueError('unbalanced checked ioctl source comment')
    data = re.sub(r'//[^\n]*', '', data).replace('\\\n', '')
    objects, functions = {}, {}
    relevant = set(CONSOLE_IOCTLS) | set(TTY_IOCTLS) | {'_IOC', '_IO', '_IOR', '_IOW', '_IOC_TYPECHECK',
        '_IOC_NRBITS', '_IOC_TYPEBITS', '_IOC_SIZEBITS', '_IOC_DIRBITS',
        '_IOC_NRSHIFT', '_IOC_TYPESHIFT', '_IOC_SIZESHIFT', '_IOC_DIRSHIFT',
        '_IOC_NONE', '_IOC_READ', '_IOC_WRITE'}
    for match in re.finditer(r'^\s*#\s*define[ \t]+([A-Za-z_]\w*)(\([^\n)]*\))?[ \t]+([^\n]+)', data, re.M):
        name, arguments, body = match.groups()
        if name not in relevant:
            continue
        if name in objects or name in functions:
            raise ValueError('duplicate checked ioctl macro: ' + name)
        if arguments is None:
            objects[name] = body.strip()
        else:
            functions[name] = (tuple(value.strip() for value in arguments[1:-1].split(',')),
                               re.sub(r'\s+', '', body))
    return objects, functions


def ioctl_integer(expression, macros, seen=()):
    # This is a source expression reader, never a C preprocessor or Python eval.
    expression = re.sub(r'\b(0[xX][0-9a-fA-F]+|[0-9]+)[uU]\b', r'\1', expression)
    def evaluate(node):
        if isinstance(node, ast.Constant) and type(node.value) is int:
            value = node.value
        elif isinstance(node, ast.Name) and node.id in macros and node.id not in seen:
            value = ioctl_integer(macros[node.id], macros, (*seen, node.id))
        elif isinstance(node, ast.BinOp) and isinstance(node.op, (ast.Add, ast.LShift)):
            left, right = evaluate(node.left), evaluate(node.right)
            if isinstance(node.op, ast.LShift) and right > 31:
                raise ValueError('ioctl shift outside unsigned32 range')
            value = left + right if isinstance(node.op, ast.Add) else left << right
        else:
            raise ValueError('unsupported checked ioctl integer expression: ' + expression)
        if value < 0 or value > 0xffffffff:
            raise ValueError('ioctl integer outside unsigned32 range')
        return value
    try:
        return evaluate(ast.parse(expression, mode='eval').body)
    except (SyntaxError, RecursionError) as error:
        raise ValueError('invalid checked ioctl integer expression') from error


def ioctl_layout(generic, override=None, *, musl=False):
    objects, functions = ioctl_definitions(generic)
    if musl:
        expected = {
            '_IOR': (('a', 'b', 'c'), '_IOC(_IOC_READ,(a),(b),sizeof(c))'),
            '_IOW': (('a', 'b', 'c'), '_IOC(_IOC_WRITE,(a),(b),sizeof(c))')}
        args, body = functions.get('_IOC', ((), ''))
        match = re.fullmatch(r'\(\(\(a\)<<([0-9]+)\)\|\(\(b\)<<8\)\|\(c\)\|\(\(d\)<<16\)\)', body)
        if args != ('a', 'b', 'c', 'd') or match is None:
            raise ValueError('unsupported checked musl _IOC definition')
        direction_shift = int(match[1])
        layout = dict(nr_shift=0, type_shift=8, size_shift=16, dir_shift=direction_shift,
                      nr_bits=8, type_bits=8, size_bits=direction_shift - 16, dir_bits=32 - direction_shift)
    else:
        expected = {
            '_IOC': (('dir', 'type', 'nr', 'size'), '(((dir)<<_IOC_DIRSHIFT)|((type)<<_IOC_TYPESHIFT)|((nr)<<_IOC_NRSHIFT)|((size)<<_IOC_SIZESHIFT))'),
            '_IOC_TYPECHECK': (('t',), '(sizeof(t))'),
            '_IOR': (('type', 'nr', 'argtype'), '_IOC(_IOC_READ,(type),(nr),(_IOC_TYPECHECK(argtype)))'),
            '_IOW': (('type', 'nr', 'argtype'), '_IOC(_IOC_WRITE,(type),(nr),(_IOC_TYPECHECK(argtype)))')}
        if override is not None:
            extra, extra_functions = ioctl_definitions(override)
            allowed = {'_IOC_SIZEBITS', '_IOC_DIRBITS', '_IOC_NONE', '_IOC_READ', '_IOC_WRITE'}
            if extra_functions or not set(extra) <= allowed or '#include <asm-generic/ioctl.h>' not in override:
                raise ValueError('unsupported checked Linux ioctl override')
            objects.update(extra)
        layout = {key.lower() + '_' + suffix.lower(): ioctl_integer(objects['_IOC_' + key + suffix], objects)
                  for suffix in ('SHIFT', 'BITS') for key in ('NR', 'TYPE', 'SIZE', 'DIR')}
    if any(functions.get(name) != value for name, value in expected.items()):
        raise ValueError('unsupported checked ioctl function definition')
    layout.update({name.lower(): ioctl_integer(objects['_IOC_' + name], objects)
                   for name in ('NONE', 'READ', 'WRITE')})
    if (layout['nr_bits'], layout['type_bits'], layout['nr_shift'], layout['type_shift'], layout['size_shift']) != (8, 8, 0, 8, 16) or \
            (layout['size_bits'], layout['dir_bits'], layout['dir_shift'], layout['none'], layout['read'], layout['write']) not in \
            ((14, 2, 30, 0, 2, 1), (13, 3, 29, 1, 2, 4)):
        raise ValueError('unsupported checked ioctl bit layout')
    return layout


def ioctl_numbers(header, layout, names, sizes, *, allow_none=False):
    """Encode literal, _IO and _IOR/_IOW requests; argument sizes come only from `sizes`."""
    macros, _ = ioctl_definitions(header)
    result = {}
    for name in names:
        expression = macros.get(name, '')
        if re.fullmatch(r'(?:0[xX][0-9a-fA-F]+|[0-9]+)[uU]?', expression):
            result[name] = ioctl_integer(expression, {})
            continue
        sequence = r"'([A-Za-z])'\s*,\s*(0[xX][0-9a-fA-F]+|[0-9]+)"
        bare = re.fullmatch(r"_IO\(\s*" + sequence + r"\s*\)", expression) if allow_none else None
        match = re.fullmatch(r"_IO([RW])\(\s*" + sequence + r"\s*,\s*([A-Za-z_][A-Za-z_0-9 ]*?(?:\[[0-9]+\])?)\s*\)",
                             expression)
        if bare is not None:
            kind, (character, number_text), size = 'none', bare.groups(), 0
        elif match is not None:
            direction, character, number_text, argument = match.groups()
            kind = 'read' if direction == 'R' else 'write'
            argument = re.sub(r'\s+', ' ', argument)
            if argument not in sizes:
                raise ValueError('unchecked ioctl argument type: ' + name + '=' + expression)
            size = sizes[argument]
        else:
            raise ValueError('unsupported checked ioctl: ' + name + '=' + expression)
        number_value = ioctl_integer(number_text, {})
        if number_value > 255:
            raise ValueError('ioctl sequence exceeds source field: ' + name)
        if size >= 1 << layout['size_bits']:
            raise ValueError('ioctl argument size exceeds source field: ' + name)
        result[name] = ((layout[kind] << layout['dir_shift']) | (ord(character) << layout['type_shift']) |
                        (number_value << layout['nr_shift']) | (size << layout['size_shift']))
    if len(set(result.values())) != len(names):
        raise ValueError('checked ioctl commands collide')
    return result


def console_ioctl_numbers(header, layout):
    try:
        return ioctl_numbers(header, layout, CONSOLE_IOCTLS, {'int': 4})
    except ValueError as error:
        raise ValueError(str(error).replace('checked ioctl', 'checked console ioctl')) from None


def checked_console_ioctls(linux_header, linux_generic, linux_override, musl_header):
    layout = ioctl_layout(linux_generic, linux_override)
    if layout != ioctl_layout(musl_header, musl=True):
        raise ValueError('Linux/musl console ioctl layout mismatch')
    values = console_ioctl_numbers(linux_header, layout)
    if values != console_ioctl_numbers(musl_header, layout):
        raise ValueError('Linux/musl console ioctl command mismatch')
    return values


def tty_ioctl_sizes(termios_bytes, winsize_bytes):
    """The only argument spellings accepted for terminal requests, with their checked sizes."""
    return {'int': 4, 'unsigned int': 4, 'struct winsize': winsize_bytes, f'char[{winsize_bytes}]': winsize_bytes,
            'struct termios': termios_bytes, f'char[{termios_bytes}]': termios_bytes}


def checked_tty_ioctls(linux_header, linux_generic, linux_override, musl_header, termios_bytes, winsize_bytes):
    layout = ioctl_layout(linux_generic, linux_override)
    if layout != ioctl_layout(musl_header, musl=True):
        raise ValueError('Linux/musl terminal ioctl layout mismatch')
    for source, expected in ((linux_generic, (('type', 'nr'), '_IOC(_IOC_NONE,(type),(nr),0)')),
                             (musl_header, (('a', 'b'), '_IOC(_IOC_NONE,(a),(b),0)'))):
        if ioctl_definitions(source)[1].get('_IO') != expected:
            raise ValueError('unsupported checked _IO definition')
    sizes = tty_ioctl_sizes(termios_bytes, winsize_bytes)
    values = ioctl_numbers(linux_header, layout, list(TTY_IOCTLS), sizes, allow_none=True)
    if values != ioctl_numbers(musl_header, layout, list(TTY_IOCTLS), sizes, allow_none=True):
        raise ValueError('Linux/musl terminal ioctl command mismatch')
    return values


def termios_definitions(*sources):
    """Read object-like macros and typedefs of termios headers in order.

    Comments and `#if 0` blocks are removed, `#undef` is honoured, and any other
    conditional contributes all of its definitions (the generator wants the
    _GNU_SOURCE view). A redefinition without `#undef` must repeat the body.
    """
    macros, typedefs = {}, {}
    for data in sources:
        data = re.sub(r'/\*.*?\*/', ' ', data, flags=re.S)
        if '/*' in data or '*/' in data:
            raise ValueError('unbalanced checked termios source comment')
        data = re.sub(r'//[^\n]*', '', data).replace('\\\n', '')
        disabled = 0
        for line in data.splitlines():
            directive = re.match(r'\s*#\s*(\w+)\s*(.*)', line)
            if directive and directive[1] in ('if', 'ifdef', 'ifndef'):
                if disabled or (directive[1] == 'if' and directive[2].strip() == '0'):
                    disabled += 1
                continue
            if directive and directive[1] in ('else', 'elif') and disabled:
                raise ValueError('unsupported #else in a disabled termios source block')
            if directive and directive[1] == 'endif':
                disabled = max(disabled - 1, 0)
                continue
            if disabled:
                continue
            if directive and directive[1] == 'undef':
                macros.pop(directive[2].strip(), None)
                continue
            if directive and directive[1] == 'define':
                match = re.fullmatch(r'([A-Za-z_]\w*)[ \t]+(\S[^\n]*?)\s*', directive[2])
                if match is None:
                    continue
                name, body = match.groups()
                if name in macros and macros[name] != body:
                    raise ValueError('conflicting termios macro redefinition: ' + name)
                macros[name] = body
                continue
            typedef = re.fullmatch(r'\s*typedef\s+(unsigned\s+(?:int|char|short))\s+(\w+)\s*;\s*', line)
            if typedef:
                typedefs[typedef[2]] = re.sub(r'\s+', ' ', typedef[1])
    return macros, typedefs


def termios_integer(name, macros, seen=()):
    if name not in macros:
        return None
    if name in seen:
        raise ValueError('cyclic termios macro ' + name)
    body = macros[name].strip()
    while body.startswith('(') and body.endswith(')'):
        body = body[1:-1].strip()
    literal = re.fullmatch(r'(0[xX][0-9a-fA-F]+|0[0-7]*|[1-9][0-9]*)[uU]?[lL]{0,2}', body)
    if literal:
        text = literal[1]
        value = int(text, 16) if text[:2].lower() == '0x' else int(text, 8) if text.startswith('0') else int(text)
    elif re.fullmatch(r'[A-Za-z_]\w*', body):
        value = termios_integer(body, macros, (*seen, name))
        if value is None:
            raise ValueError('unresolved termios macro alias: ' + name + '=' + body)
    else:
        raise ValueError(f'unsupported termios macro expression {name}={body}')
    if value < 0 or value > 0xffffffff:
        raise ValueError('termios macro outside unsigned32 range: ' + name)
    return value


def struct_layout(data, struct, typedefs, macros):
    """Natural-alignment layout of one struct; every member type must be a checked typedef."""
    data = re.sub(r'/\*.*?\*/', ' ', data, flags=re.S)
    bodies = re.findall(r'\b(?:struct|STRUCT)\s+' + struct + r'\s*\{(.*?)\}\s*;', data, flags=re.S)
    if len(bodies) != 1:
        raise ValueError('expected exactly one struct ' + struct)
    offset, alignment, members = 0, 1, {}
    for declaration in (part.strip() for part in bodies[0].split(';')):
        if not declaration:
            continue
        match = re.fullmatch(r'((?:unsigned\s+)?\w+)\s+(\w+(?:\s*,\s*\w+)*)\s*(?:\[\s*(\w+)\s*\])?', declaration)
        if match is None:
            raise ValueError('unsupported struct ' + struct + ' member: ' + declaration)
        spelled, names, count = match.groups()
        spelled = typedefs.get(spelled, re.sub(r'\s+', ' ', spelled))
        if spelled not in TERMIOS_TYPE_BYTES:
            raise ValueError('unchecked struct ' + struct + ' member type: ' + declaration)
        size = TERMIOS_TYPE_BYTES[spelled]
        elements = 1 if count is None else (int(count) if count.isdigit() else termios_integer(count, macros))
        if not elements:
            raise ValueError('unresolved struct ' + struct + ' array bound: ' + declaration)
        for member in re.split(r'\s*,\s*', names):
            offset = (offset + size - 1) // size * size
            alignment = max(alignment, size)
            members[member.lstrip('_')] = (offset, size, elements)
            offset += size * elements
    return members, (offset + alignment - 1) // alignment * alignment


def termios_profile(linux_sources, musl_sources):
    """Cross-check one Linux termbits family against its musl port and return its tables."""
    tables = []
    for sources in (linux_sources, musl_sources):
        macros, typedefs = termios_definitions(*sources)
        members, size = struct_layout('\n'.join(sources), 'termios', typedefs, macros)
        expected = {'c_iflag': 0, 'c_oflag': 4, 'c_cflag': 8, 'c_lflag': 12}
        if any(members.get(name, (None,))[0] != offset or members[name][1:] != (4, 1)
               for name, offset in expected.items()):
            raise ValueError('termios flag words are not four leading unsigned32 fields')
        if members.get('c_line', (0, 0, 0))[1:] != (1, 1) or members.get('c_cc', (0, 0, 0))[1] != 1:
            raise ValueError('termios c_line/c_cc are not bytes')
        speeds = [members.get(name) for name in ('c_ispeed', 'c_ospeed')]
        if set(members) - set(expected) - {'c_line', 'c_cc', 'c_ispeed', 'c_ospeed'}:
            raise ValueError('unexpected termios member')
        if (speeds[0] is None) != (speeds[1] is None) or \
                (speeds[0] is not None and (speeds[0][1:] != (4, 1) or speeds[1][0] != speeds[0][0] + 4)):
            raise ValueError('unsupported termios speed words')
        window, window_bytes = struct_layout('\n'.join(sources), 'winsize', typedefs, macros)
        if [value[1:] for value in window.values()] != [(2, 1)] * 4:
            raise ValueError('winsize is not four unsigned shorts')
        tables.append(dict(macros=macros, members=members, size=size, nccs=members['c_cc'][2],
                           speed=speeds[0][0] if speeds[0] else 0, window=window_bytes))
    linux, musl = tables
    if any(linux['members'][name][0] != musl['members'][name][0] for name in ('c_line', 'c_cc')) or \
            musl['nccs'] < linux['nccs'] or musl['size'] < linux['size'] or \
            (linux['speed'] and linux['speed'] != musl['speed']) or linux['window'] != musl['window']:
        raise ValueError('musl struct termios is not a prefix-compatible superset of the Linux kernel struct')
    checked, linux_only = [], []

    def value(name):
        result = termios_integer(name, linux['macros'])
        if result is None:
            raise ValueError('missing Linux termios name ' + name)
        other = termios_integer(name, musl['macros'])
        if other is None:
            if name not in TERMIOS_LINUX_ONLY:
                raise ValueError('missing musl termios name ' + name)
            linux_only.append(name)
        elif other != result:
            raise ValueError(f'Linux/musl termios mismatch: {name}: {result:#x} != {other:#x}')
        else:
            checked.append(name)
        return result

    cc = {name: value(name) for name in TERMIOS_CC}
    if len(set(cc.values())) != len(cc) or max(cc.values()) >= linux['nccs']:
        raise ValueError('termios control character slots collide or exceed NCCS')
    masks, values = {}, {}
    ibshift = termios_integer('IBSHIFT', linux['macros'])
    for field in TERMIOS_FIELDS:
        used = 0
        for name in TERMIOS_SINGLE[field]:
            bit = value(name)
            values[name] = (field, bit, bit)
            if bit.bit_count() != 1 or bit & used:
                raise ValueError('termios single flag is not a distinct bit: ' + name)
            used |= values[name][1]
        groups = list(TERMIOS_GROUPS.get(field, []))
        if field == 'cflag':
            groups.append(('CIBAUD', None))
        for group, names in groups:
            mask = value(group)
            if mask & used:
                raise ValueError('termios group overlaps another flag: ' + group)
            used |= mask
            masks[group] = (field, mask)
            if names is None:
                if ibshift is None or mask != masks['CBAUD'][1] << ibshift or \
                        termios_integer('CIBAUD', musl['macros']) != termios_integer('CBAUD', musl['macros']) << ibshift:
                    raise ValueError('CIBAUD is not CBAUD shifted by IBSHIFT')
                for baud in TERMIOS_BAUDS:
                    values['CIBAUD_' + baud] = (field, mask, values[baud][2] << ibshift)
                continue
            seen = set()
            for name in names:
                encoded = value(name)
                if encoded & ~mask or encoded in seen:
                    raise ValueError('termios group value outside its mask or repeated: ' + name)
                seen.add(encoded)
                values[name] = (field, mask, encoded)
    return dict(bytes=linux['size'], nccs=linux['nccs'], line_offset=linux['members']['c_line'][0],
                cc_offset=linux['members']['c_cc'][0], speed_offset=linux['speed'], winsize_bytes=linux['window'],
                musl_bytes=musl['size'], musl_nccs=musl['nccs'], cc=cc, masks=masks, values=values,
                checked=checked, linux_only=sorted(set(linux_only)))


def termios_rows(generic, profile):
    """One row per generic name: generic mask/value and the profile's mask/value."""
    rows = []
    for name, (field, mask, encoded) in generic['values'].items():
        other_field, other_mask, other_value = profile['values'][name]
        if other_field != field:
            raise ValueError('termios name changes field across profiles: ' + name)
        rows.append((name, field, mask, encoded, other_mask, other_value))
    return rows




def linux_numbers(linux, arch):
    tables = {
        'i386': ('arch/x86/entry/syscalls/syscall_32.tbl', {'i386'}, 0),
        'x86_64': ('arch/x86/entry/syscalls/syscall_64.tbl', {'common','64'}, 0),
        'arm32': ('arch/arm/tools/syscall.tbl', {'common','eabi'}, 0),
        'mips': ('arch/mips/kernel/syscalls/syscall_o32.tbl', {'o32'}, 4000),
        'mips64': ('arch/mips/kernel/syscalls/syscall_n64.tbl', {'n64'}, 5000),
        'ppc': ('arch/powerpc/kernel/syscalls/syscall.tbl', {'common','nospu','32'}, 0),
    }
    if arch not in tables:
        path = 'include/uapi/asm-generic/unistd.h'
        macros = definitions((linux / path).read_text())
        def generic_number(operation):
            name = '__NR_' + ('llseek' if operation == '_llseek' else operation)
            # asm-generic conditionally aliases old stat only if a platform
            # supplies __NR3264_stat. None of the retained generic profiles does.
            if operation in ('stat', 'lstat') and macros.get(name) == '__NR3264_' + operation and '__NR3264_' + operation not in macros:
                return None
            return number(name, macros)
        return {operation: generic_number(operation) for operation in OPERATIONS if operation != 'arm_set_tls'}, [path]
    path, abis, base = tables[arch]
    result = {}
    for line in (linux / path).read_text().splitlines():
        fields = line.split('#',1)[0].split()
        if len(fields) >= 3 and fields[0].isdigit() and fields[1] in abis:
            result[fields[2]] = base + int(fields[0])
    return result, [path]


def generate(archive_path: Path, linux: Path):
    archive_data = archive_path.read_bytes()
    if sha(archive_data) != MUSL_SHA256:
        raise ValueError('musl archive checksum mismatch')
    source_hashes = {}
    with tarfile.open(fileobj=io.BytesIO(archive_data), mode='r:gz') as archive:
        def read(name):
            stream = archive.extractfile('musl-' + MUSL_VERSION + '/' + name)
            if stream is None:
                raise ValueError('missing musl file ' + name)
            data = stream.read()
            source_hashes['musl/' + name] = sha(data)
            return data.decode()
        if read('VERSION').strip() != MUSL_VERSION:
            raise ValueError('musl version mismatch')
        read('include/signal.h')
        for wrapper in ('setuid', 'setgid', 'setreuid', 'setregid', 'setresuid', 'setresgid'):
            read('src/unistd/' + wrapper + '.c')
        for wrapper in ('getresuid', 'getresgid'):
            read('src/misc/' + wrapper + '.c')
        read('src/unistd/setxid.c')
        for wrapper in ('kill', 'raise', 'sigpending', 'sigsuspend', 'sigtimedwait',
                        'sigaltstack', 'sigqueue'):
            read('src/signal/' + wrapper + '.c')
        read('src/thread/pthread_kill.c')
        read('src/internal/emulate_wait4.c')
        read('include/fcntl.h')
        read('src/fcntl/fcntl.c')
        read('src/unistd/dup.c')
        read('src/unistd/dup2.c')
        read('src/unistd/dup3.c')
        read('src/unistd/lseek.c')
        read('src/unistd/pread.c')
        read('src/unistd/pwrite.c')
        for wrapper in ('stat', 'lstat', 'fstat', 'fstatat', 'chmod', 'fchmod', 'fchmodat'):
            read('src/stat/' + wrapper + '.c')
        read('src/internal/procfdname.c')
        read('src/stat/umask.c')
        read('src/stat/mkdir.c')
        read('src/stat/mkdirat.c')
        for wrapper in ('unlink', 'unlinkat', 'rmdir'):
            read('src/unistd/' + wrapper + '.c')
        read('src/stdio/rename.c')
        read('src/unistd/renameat.c')
        read('include/stdio.h')
        for wrapper in ('truncate', 'ftruncate'):
            read('src/unistd/' + wrapper + '.c')
        for wrapper in ('access', 'faccessat'):
            read('src/unistd/' + wrapper + '.c')
        for wrapper in ('setpgid', 'getpgid', 'getpgrp', 'setsid', 'getsid'):
            read('src/unistd/' + wrapper + '.c')
        read('include/unistd.h')
        read('include/sys/ioctl.h')
        read('include/termios.h')
        read('src/misc/ioctl.c')
        read('src/unistd/tcgetpgrp.c')
        read('src/unistd/tcsetpgrp.c')
        read('src/termios/tcgetsid.c')
        read('include/sys/stat.h')
        read('include/dirent.h')
        read('src/dirent/__dirent.h')
        read('arch/generic/bits/dirent.h')
        for wrapper in ('opendir', 'fdopendir', 'readdir', 'closedir', 'telldir', 'seekdir', 'rewinddir'):
            read('src/dirent/' + wrapper + '.c')
        read('src/internal/syscall.h')
        read('include/poll.h')
        read('src/select/poll.c')
        read('src/select/ppoll.c')
        read('src/select/select.c')
        read('src/select/pselect.c')
        read('src/internal/syscall.h')
        read('include/sys/select.h')
        read('arch/generic/bits/poll.h')
        read('arch/mips/bits/poll.h')
        read('arch/mips64/bits/poll.h')
        read('src/time/nanosleep.c')
        read('src/time/clock_nanosleep.c')
        read('src/time/clock_getres.c')
        read('src/time/clock_settime.c')
        for wrapper in ('setitimer', 'getitimer'):
            read('src/signal/' + wrapper + '.c')
        read('src/unistd/alarm.c')
        for wrapper in ('timer_create', 'timer_settime', 'timer_gettime', 'timer_getoverrun', 'timer_delete'):
            read('src/time/' + wrapper + '.c')
        read('src/unistd/pipe.c')
        read('src/unistd/pipe2.c')
        read('src/unistd/mips/pipe.s')
        read('src/unistd/mips64/pipe.s')
        read('src/internal/pthread_impl.h')
        for wrapper in ('pthread_create', 'pthread_mutex_trylock', 'pthread_mutexattr_setrobust'):
            read('src/thread/' + wrapper + '.c')
        # musl passes its own struct termios straight to TCGETS and TCSETS+act,
        # so the kernel copies exactly its own struct size into a musl prefix.
        tcgetattr = re.sub(r'\s+', '', read('src/termios/tcgetattr.c'))
        tcsetattr = re.sub(r'\s+', '', read('src/termios/tcsetattr.c'))
        if 'ioctl(fd,TCGETS,tio)' not in tcgetattr or 'if(act<0||act>2)' not in tcsetattr or \
                'returnioctl(fd,TCSETS+act,tio);' not in tcsetattr:
            raise ValueError('musl tcgetattr/tcsetattr no longer pass struct termios to TCGETS/TCSETS+act')
        musl_termios = read('include/termios.h')
        musl_alltypes = read('include/alltypes.h.in')
        linux_common = (linux / 'include/uapi/asm-generic/termbits-common.h').read_text()

        def linux_termios(family):
            base = 'arch/' + family + '/include/uapi/asm/' if family else 'include/uapi/asm-generic/'
            termbits = (linux / (base + 'termbits.h')).read_text()
            if '#include <asm-generic/termbits-common.h>' not in termbits:
                raise ValueError('Linux termbits no longer includes termbits-common.h: ' + base)
            return [linux_common, termbits, (linux / (base + 'termios.h')).read_text()]

        def musl_bits(port):
            try:
                return read(f'arch/{port}/bits/termios.h')
            except KeyError:
                return read('arch/generic/bits/termios.h')
        generic_termios = termios_profile(linux_termios(None), [musl_termios, musl_bits('generic'), musl_alltypes])
        rows = []
        for arch, port, width, big, nr, ret, regs, error, align, labels in PROFILES:
            macros = definitions(read(f'arch/{port}/bits/syscall.h.in'))
            try:
                read(f'arch/{port}/bits/fcntl.h')
            except KeyError:
                read('arch/generic/bits/fcntl.h')
            read(f'arch/{port}/syscall_arch.h')
            errno_path = f'arch/{port}/bits/errno.h'
            try:
                errmacros = definitions(read(errno_path))
            except KeyError:
                errmacros = definitions(read('arch/generic/bits/errno.h'))
            numbers = {operation: number('__ARM_NR_set_tls' if operation == 'arm_set_tls' else '__NR_' + operation, macros) for operation in OPERATIONS}
            errors = {name: number(name, errmacros) for name in ERRORS}
            if any(value is None for value in errors.values()):
                raise ValueError('missing errno data for ' + arch)
            try:
                ioctl_header = read(f'arch/{port}/bits/ioctl.h')
            except KeyError:
                ioctl_header = read('arch/generic/bits/ioctl.h')
            ioctl_family = 'mips' if arch in ('mips', 'mips64') else 'powerpc' if arch == 'ppc' else None
            ioctl_base = 'arch/' + ioctl_family + '/include/uapi/asm/' if ioctl_family else 'include/uapi/asm-generic/'
            ioctls = checked_console_ioctls((linux / (ioctl_base + 'ioctls.h')).read_text(),
                (linux / 'include/uapi/asm-generic/ioctl.h').read_text(),
                (linux / (ioctl_base + 'ioctl.h')).read_text() if ioctl_family else None, ioctl_header)
            termios = termios_profile(linux_termios(ioctl_family), [musl_termios, musl_bits(port), musl_alltypes])
            tty_ioctls = checked_tty_ioctls((linux / (ioctl_base + 'ioctls.h')).read_text(),
                (linux / 'include/uapi/asm-generic/ioctl.h').read_text(),
                (linux / (ioctl_base + 'ioctl.h')).read_text() if ioctl_family else None, ioctl_header,
                termios['bytes'], termios['winsize_bytes'])
            if len(set(tty_ioctls.values()) | set(ioctls.values())) != len(tty_ioctls) + len(ioctls):
                raise ValueError('terminal and console ioctl commands collide: ' + arch)
            if (tty_ioctls['TCSETSW'], tty_ioctls['TCSETSF']) != (tty_ioctls['TCSETS'] + 1, tty_ioctls['TCSETS'] + 2):
                raise ValueError('musl TCSETS+act does not reach TCSETSW/TCSETSF: ' + arch)
            named_slots = set(termios['cc'].values())
            rows.append({'architecture':arch,'musl_port':port,'word_bytes':width,'byte_order':'big' if big else 'little',
                         'number_register':nr,'return_register':ret,'argument_registers':regs,'register_labels':labels,
                         'error_convention':error,'aligned_64bit_pairs':align,'syscall_numbers':numbers,'errno_numbers':errors,
                         'console_ioctl_numbers':ioctls,'console_ioctl_pid_bytes':4,
                         'linux_ioctl_crosschecks':list(ioctls),
                         'tty_ioctl_numbers':tty_ioctls,'linux_tty_ioctl_crosschecks':list(tty_ioctls),
                         'termios_layout':{key:termios[key] for key in ('bytes','nccs','line_offset','cc_offset',
                                           'speed_offset','winsize_bytes','musl_bytes','musl_nccs')},
                         'termios_cc_slots':termios['cc'],
                         'termios_unnamed_cc_slots':[slot for slot in range(termios['nccs']) if slot not in named_slots],
                         'termios_group_masks':{name:mask for name,(field,mask) in termios['masks'].items()},
                         'termios_values':{name:encoded for name,(field,mask,encoded) in termios['values'].items()},
                         'termios_map':termios_rows(generic_termios, termios),
                         'linux_termios_crosschecks':list(dict.fromkeys(termios['checked'])),
                         'linux_only_termios':termios['linux_only'],
                         'kernel_timespec_bytes':16,'kernel_timespec_nsec_value_bits':width * 8,'kernel_timespec_nsec_high32_input_ignored':width == 4,
                         'linux_qualification':'unqualified','execution':'host-codec-model-only'})
    linux_files = set(LINUX_FILES)
    linux_files.update(('fs/readdir.c', 'include/linux/dirent.h', 'kernel/sys.c',
                        'kernel/futex/syscalls.c', 'kernel/futex/core.c', 'include/uapi/linux/futex.h',
                        'include/linux/threads.h', 'Documentation/locking/robust-futex-ABI.rst',
                        'kernel/time/timekeeping.c', 'include/linux/time64.h', 'security/commoncap.c',
                        'kernel/time/posix-cpu-timers.c', 'kernel/time/posix-clock.c',
                        'fs/attr.c', 'fs/ext4/inode.c', 'kernel/groups.c', 'include/linux/fs.h',
                        'include/linux/cred.h', 'include/linux/uidgid.h', 'include/linux/capability.h',
                        'include/uapi/linux/fcntl.h', 'include/uapi/asm-generic/errno-base.h',
                        'include/uapi/asm-generic/errno.h'))
    for row in rows:
        reference, paths = linux_numbers(linux, row['architecture'])
        linux_files.update(paths)
        checked = []
        for operation, value in row['syscall_numbers'].items():
            if value is None:
                continue
            if operation == 'arm_set_tls':
                arm = definitions((linux / 'arch/arm/include/uapi/asm/unistd.h').read_text())
                arm['__NR_SYSCALL_BASE'] = '0' # Selected ARM EABI, not the old OABI encoding.
                if number('__ARM_NR_set_tls', arm) != value:
                    raise ValueError('Linux/musl ARM EABI set_tls mismatch')
                checked.append(operation)
                continue
            source_name = {'clock_gettime32': 'clock_gettime', 'nanosleep_time32': 'nanosleep',
                           'clock_nanosleep_time32': 'clock_nanosleep', 'clock_getres_time32': 'clock_getres', 'clock_settime32': 'clock_settime',
                           'timer_settime32': 'timer_settime', 'timer_gettime32': 'timer_gettime'}.get(operation, operation)
            if operation == 'futex' and row['word_bytes'] == 4 and value == row['syscall_numbers'].get('futex_time64'):
                source_name = 'futex_time64'
            # Generic UAPI defines both word-size aliases to __NR3264_* numbers.
            actual = reference.get(source_name)
            if actual is None or actual != value:
                raise ValueError(f"Linux/musl syscall mismatch: {row['architecture']}/{operation}: {actual} != {value}")
            checked.append(operation)
        row['linux_number_crosschecks'] = checked
        errno_paths = ['include/uapi/asm-generic/errno-base.h',
                       'arch/mips/include/uapi/asm/errno.h' if row['architecture'] in ('mips', 'mips64')
                       else 'include/uapi/asm-generic/errno.h']
        linux_errors = {}
        for path in errno_paths:
            # The selected UAPI headers contain trailing C comments in macros.
            data = re.sub(r'/\*.*?\*/', '', (linux / path).read_text(), flags=re.S)
            linux_errors.update(definitions(data))
        for name, value in row['errno_numbers'].items():
            if number(name, linux_errors) != value:
                raise ValueError(f"Linux/musl errno mismatch: {row['architecture']}/{name}")
        row['linux_errno_crosschecks'] = list(row['errno_numbers'])
    for name in sorted(linux_files):
        source_hashes['linux/' + name] = sha((linux / name).read_bytes())
    version = (linux / 'Makefile').read_text()
    if not all(re.search(r'^' + key + r'\s*=\s*' + value + r'\s*$', version, re.M) for key,value in [('VERSION','6'),('PATCHLEVEL','13'),('SUBLEVEL','4')]):
        raise ValueError('Linux source must be 6.13.4')
    termios_map_rows = len(rows[0]['termios_map'])
    if any([row[:2] for row in profile['termios_map']] != [row[:2] for row in rows[0]['termios_map']] for profile in rows):
        raise ValueError('termios map rows differ in order across profiles')
    # The header carries the rows; the manifest keeps the values they are built from.
    maps = [{name: (field, mask, encoded, other_mask, other_value)
             for name, field, mask, encoded, other_mask, other_value in profile.pop('termios_map')} for profile in rows]
    manifest = {'schema_version':1,'codec_revision':3,'scope':'bootstrap-codec-model-only','musl_version':MUSL_VERSION,'musl_archive_sha256':MUSL_SHA256,
                'linux_reference_version':'6.13.4','linux_source_provenance':'local inspected files; full upstream archive authenticity not established',
                'source_file_sha256':dict(sorted(source_hashes.items())), 'profiles':rows,
                'limitations':['No native Linux personality, trap adapter, dispatch or guest musl execution.',
                               'Raw argument slots do not implement per-syscall pointer/layout/flag or clone/mmap offset semantics.',
                               'MIPS O32 indirect syscall and seven/eight-argument calls are outside this six-argument batch.',
                               'Legacy time structures and __kernel_timespec are distinct from musl public struct timespec padding.',
                               'Full stat, signal frames, TLS/auxv, remaining errno/flags/syscalls and architecture qualification remain open.',
                               'Register snapshots use DWARF integer numbering on x86, ISA integer register numbering elsewhere; they are not existing trap structures.',
                               'Termios maps cover named flag values only; other encodings, unnamed control slots and the PPC speed words are not interpreted.',
                               'BOTHER and ADDRB are Linux-only names (musl 1.2.5 defines neither); musl and Linux disagree on PPC CBAUDEX, which the map does not use.']}
    lines = ['#pragma once', '', '// Generated by scripts/generate_linux_abi.py; do not edit numbers manually.', '// Source identity and qualification gaps: docs/abi/linux-profiles.json.', '#include "ok/core/types.hpp"', '#include <array>', '', 'namespace ok::abi::linux_user', '{',
             'enum class Profile : u8 { ' + ', '.join(row['architecture'] for row in rows) + ', count };',
             'enum class Operation : u8 { ' + ', '.join(OPERATIONS) + ', count };',
             'enum class Error : u8 { ' + ', '.join('e_' + name[1:].lower() for name in ERRORS) + ', count };',
             'enum class ConsoleIoctlOperation : u8 { acquire, get_sid, get_pgrp, set_pgrp, count };',
             'enum class TtyIoctlOperation : u8 { ' + ', '.join(TTY_IOCTLS.values()) + ', count };',
             'enum class TermiosField : u8 { ' + ', '.join(TERMIOS_FIELDS) + ' };',
             'enum class TermiosName : u8 { ' + ', '.join(name.lower() for name in maps[0]) + ', count };',
             'struct TermiosLayout', '{', '    u8 bytes;', '    u8 nccs;', '    u8 line_offset;', '    u8 cc_offset;',
             '    u8 speed_offset; // 0: no speed words', '};',
             'struct TermiosMapRow', '{', '    TermiosField field;', '    u32 generic_mask;', '    u32 generic_value;',
             '    u32 profile_mask;', '    u32 profile_value;', '};',
             'inline constexpr u8 absent_termios_slot = 0xffU;',
             'inline constexpr usize termios_named_slots = ' + str(len(TERMIOS_CC)) + ';',
             'enum class ErrorConvention : u8 { negative, mips_a3, ppc_cr0_so };',
             'inline constexpr u32 unavailable_number = 0xffffffffU;',
             'struct ProfileData', '{', '    const char *name;', '    u8 word_bytes;', '    bool big_endian;', '    u8 number_register;', '    u8 return_register;', '    std::array<i8, 6> argument_registers;', '    ErrorConvention error_convention;', '    bool aligned_64bit_pairs;',
             '    std::array<u32, static_cast<usize>(Operation::count)> numbers;', '    std::array<u32, static_cast<usize>(Error::count)> errors;',
             '    std::array<u32, static_cast<usize>(ConsoleIoctlOperation::count)> console_ioctls;',
             '    std::array<u32, static_cast<usize>(TtyIoctlOperation::count)> tty_ioctls;',
             '    TermiosLayout termios;', '    std::array<u8, termios_named_slots> termios_slots;', '};',
             'inline constexpr std::array<ProfileData, static_cast<usize>(Profile::count)> profiles{{']
    for row in rows:
        lines += ['    {"' + row['architecture'] + '", ' + str(row['word_bytes']) + ', ' + str(row['byte_order']=='big').lower() + ', ' + str(row['number_register']) + ', ' + str(row['return_register']) + ', {' + ', '.join(map(str,row['argument_registers'])) + '}, ErrorConvention::' + row['error_convention'] + ', ' + str(row['aligned_64bit_pairs']).lower() + ',',
                  '     {' + ', '.join('unavailable_number' if value is None else str(value) + 'U' for value in row['syscall_numbers'].values()) + '},',
                  '     {' + ', '.join(str(value) + 'U' for value in row['errno_numbers'].values()) + '},',
                  '     {' + ', '.join(str(value) + 'U' for value in row['console_ioctl_numbers'].values()) + '},',
                  '     {' + ', '.join(hex(value) + 'U' for value in row['tty_ioctl_numbers'].values()) + '},',
                  '     {' + ', '.join(str(row['termios_layout'][key]) for key in
                                      ('bytes', 'nccs', 'line_offset', 'cc_offset', 'speed_offset')) + '},',
                  '     {' + ', '.join(str(value) for value in row['termios_cc_slots'].values()) + '}},']
    lines += ['}};', 'inline constexpr usize termios_map_rows = ' + str(termios_map_rows) + ';',
              '// One row per TermiosName; profiles index the same rows.',
              'inline constexpr std::array<std::array<TermiosMapRow, termios_map_rows>, static_cast<usize>(Profile::count)> termios_maps{{']
    for row, termios_map in zip(rows, maps):
        entries = ['{TermiosField::' + field + ', ' + ', '.join(hex(value) + 'U' for value in values) + '}'
                   for field, *values in termios_map.values()]
        lines += ['    {{ // ' + row['architecture']]
        lines += ['        ' + ', '.join(entries[index:index + 3]) + ',' for index in range(0, len(entries), 3)]
        lines += ['    }},']
    lines += ['}};', '} // namespace ok::abi::linux_user', '']
    return '\n'.join(lines), json.dumps(manifest,indent=2) + '\n'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--musl-archive',required=True,type=Path)
    parser.add_argument('--linux-source',required=True,type=Path)
    parser.add_argument('--check',action='store_true')
    args=parser.parse_args()
    try:
        header,manifest=generate(args.musl_archive,args.linux_source)
        root=Path(__file__).resolve().parents[1]
        for file,content in [(root/'include/ok/abi/linux_profile_data.hpp',header),(root/'docs/abi/linux-profiles.json',manifest)]:
            if args.check:
                if file.read_text() != content:
                    raise ValueError('generated ABI data is stale: ' + str(file))
            else:
                file.parent.mkdir(parents=True,exist_ok=True)
                file.write_text(content)
        print('Linux ABI generated data: ten profiles; codec/model only; Linux qualification unqualified')
        return 0
    except (OSError,ValueError,KeyError,tarfile.TarError) as error:
        parser.exit(1,str(error)+'\n')

if __name__ == '__main__':
    raise SystemExit(main())
