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
ERRORS += ['ESRCH']
ERRORS += ['ENOTTY']
CONSOLE_IOCTLS = ['TIOCSCTTY', 'TIOCGSID', 'TIOCGPGRP', 'TIOCSPGRP']
LINUX_FILES = ['Makefile', 'fs/select.c', 'kernel/signal.c', 'include/uapi/asm-generic/poll.h', 'arch/mips/include/uapi/asm/poll.h', 'arch/mips/include/uapi/asm/signal.h', 'include/uapi/linux/time_types.h', 'include/uapi/asm-generic/siginfo.h', 'include/uapi/linux/wait.h', 'kernel/time/time.c', 'fs/file.c', 'fs/read_write.c', 'kernel/time/hrtimer.c', 'kernel/time/posix-timers.c', 'include/linux/ktime.h', 'fs/fcntl.c', 'include/uapi/asm-generic/fcntl.h', 'fs/d_path.c', 'fs/namei.c', 'fs/open.c', 'include/uapi/linux/fs.h', 'include/uapi/asm-generic/posix_types.h', 'arch/mips/kernel/scall32-o32.S', 'arch/mips/include/uapi/asm/errno.h', 'arch/powerpc/include/asm/syscall.h', 'arch/arm/include/uapi/asm/unistd.h', 'fs/pipe.c', 'arch/mips/kernel/syscall.c', 'fs/inode.c', 'include/linux/types.h', 'fs/stat.c', 'include/uapi/linux/stat.h', 'arch/x86/include/uapi/asm/stat.h', 'include/uapi/asm-generic/stat.h']


LINUX_FILES += ['kernel/sys.c', 'kernel/uid16.c', 'include/linux/highuid.h', 'fs/exec.c', 'kernel/cred.c']
LINUX_FILES += ['kernel/fork.c', 'kernel/exit.c', 'include/linux/pid.h', 'include/linux/sched/signal.h']
LINUX_FILES += ['fs/ioctl.c', 'drivers/tty/tty_jobctrl.c', 'drivers/tty/tty_io.c',
                'include/uapi/asm-generic/ioctl.h', 'include/uapi/asm-generic/ioctls.h',
                'arch/mips/include/uapi/asm/ioctl.h', 'arch/mips/include/uapi/asm/ioctls.h',
                'arch/powerpc/include/uapi/asm/ioctl.h', 'arch/powerpc/include/uapi/asm/ioctls.h']


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
    relevant = set(CONSOLE_IOCTLS) | {'_IOC', '_IOR', '_IOW', '_IOC_TYPECHECK',
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


def console_ioctl_numbers(header, layout):
    macros, _ = ioctl_definitions(header)
    result = {}
    for name in CONSOLE_IOCTLS:
        expression = macros.get(name, '')
        if re.fullmatch(r'(?:0[xX][0-9a-fA-F]+|[0-9]+)[uU]?', expression):
            result[name] = ioctl_integer(expression, {})
            continue
        match = re.fullmatch(r"_IO([RW])\(\s*'([A-Za-z])'\s*,\s*(0[xX][0-9a-fA-F]+|[0-9]+)\s*,\s*int\s*\)", expression)
        if match is None:
            raise ValueError('unsupported checked console ioctl: ' + name + '=' + expression)
        kind, character, sequence = match.groups()
        sequence = ioctl_integer(sequence, {})
        if sequence > 255:
            raise ValueError('console ioctl sequence exceeds source field')
        result[name] = ((layout['read' if kind == 'R' else 'write'] << layout['dir_shift']) |
                        (ord(character) << layout['type_shift']) | (sequence << layout['nr_shift']) |
                        (4 << layout['size_shift']))
    if len(set(result.values())) != len(CONSOLE_IOCTLS):
        raise ValueError('checked console ioctl commands collide')
    return result


def checked_console_ioctls(linux_header, linux_generic, linux_override, musl_header):
    layout = ioctl_layout(linux_generic, linux_override)
    if layout != ioctl_layout(musl_header, musl=True):
        raise ValueError('Linux/musl console ioctl layout mismatch')
    values = console_ioctl_numbers(linux_header, layout)
    if values != console_ioctl_numbers(musl_header, layout):
        raise ValueError('Linux/musl console ioctl command mismatch')
    return values




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
        read('src/unistd/pipe.c')
        read('src/unistd/pipe2.c')
        read('src/unistd/mips/pipe.s')
        read('src/unistd/mips64/pipe.s')
        read('src/internal/pthread_impl.h')
        for wrapper in ('pthread_create', 'pthread_mutex_trylock', 'pthread_mutexattr_setrobust'):
            read('src/thread/' + wrapper + '.c')
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
            rows.append({'architecture':arch,'musl_port':port,'word_bytes':width,'byte_order':'big' if big else 'little',
                         'number_register':nr,'return_register':ret,'argument_registers':regs,'register_labels':labels,
                         'error_convention':error,'aligned_64bit_pairs':align,'syscall_numbers':numbers,'errno_numbers':errors,
                         'console_ioctl_numbers':ioctls,'console_ioctl_pid_bytes':4,
                         'linux_ioctl_crosschecks':list(ioctls),
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
                           'clock_nanosleep_time32': 'clock_nanosleep', 'clock_getres_time32': 'clock_getres', 'clock_settime32': 'clock_settime'}.get(operation, operation)
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
    manifest = {'schema_version':1,'codec_revision':2,'scope':'bootstrap-codec-model-only','musl_version':MUSL_VERSION,'musl_archive_sha256':MUSL_SHA256,
                'linux_reference_version':'6.13.4','linux_source_provenance':'local inspected files; full upstream archive authenticity not established',
                'source_file_sha256':dict(sorted(source_hashes.items())), 'profiles':rows,
                'limitations':['No native Linux personality, trap adapter, dispatch or guest musl execution.',
                               'Raw argument slots do not implement per-syscall pointer/layout/flag or clone/mmap offset semantics.',
                               'MIPS O32 indirect syscall and seven/eight-argument calls are outside this six-argument batch.',
                               'Legacy time structures and __kernel_timespec are distinct from musl public struct timespec padding.',
                               'Full stat, signal frames, TLS/auxv, remaining errno/flags/syscalls and architecture qualification remain open.',
                               'Register snapshots use DWARF integer numbering on x86, ISA integer register numbering elsewhere; they are not existing trap structures.']}
    lines = ['#pragma once', '', '// Generated by scripts/generate_linux_abi.py; do not edit numbers manually.', '// Source identity and qualification gaps: docs/abi/linux-profiles.json.', '#include "ok/core/types.hpp"', '#include <array>', '', 'namespace ok::abi::linux_user', '{',
             'enum class Profile : u8 { ' + ', '.join(row['architecture'] for row in rows) + ', count };',
             'enum class Operation : u8 { ' + ', '.join(OPERATIONS) + ', count };',
             'enum class Error : u8 { ' + ', '.join('e_' + name[1:].lower() for name in ERRORS) + ', count };',
             'enum class ConsoleIoctlOperation : u8 { acquire, get_sid, get_pgrp, set_pgrp, count };',
             'enum class ErrorConvention : u8 { negative, mips_a3, ppc_cr0_so };',
             'inline constexpr u32 unavailable_number = 0xffffffffU;',
             'struct ProfileData', '{', '    const char *name;', '    u8 word_bytes;', '    bool big_endian;', '    u8 number_register;', '    u8 return_register;', '    std::array<i8, 6> argument_registers;', '    ErrorConvention error_convention;', '    bool aligned_64bit_pairs;',
             '    std::array<u32, static_cast<usize>(Operation::count)> numbers;', '    std::array<u32, static_cast<usize>(Error::count)> errors;',
             '    std::array<u32, static_cast<usize>(ConsoleIoctlOperation::count)> console_ioctls;', '};',
             'inline constexpr std::array<ProfileData, static_cast<usize>(Profile::count)> profiles{{']
    for row in rows:
        lines += ['    {"' + row['architecture'] + '", ' + str(row['word_bytes']) + ', ' + str(row['byte_order']=='big').lower() + ', ' + str(row['number_register']) + ', ' + str(row['return_register']) + ', {' + ', '.join(map(str,row['argument_registers'])) + '}, ErrorConvention::' + row['error_convention'] + ', ' + str(row['aligned_64bit_pairs']).lower() + ',',
                  '     {' + ', '.join('unavailable_number' if value is None else str(value) + 'U' for value in row['syscall_numbers'].values()) + '},',
                  '     {' + ', '.join(str(value) + 'U' for value in row['errno_numbers'].values()) + '},',
                  '     {' + ', '.join(str(value) + 'U' for value in row['console_ioctl_numbers'].values()) + '}},']
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
