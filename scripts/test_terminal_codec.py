#!/usr/bin/env python3
"""Build and run the hosted terminal codec model (tests/host/terminal_codec_tests.cpp).

Copied-byte codec evidence only: the native 36-byte termios wire on both
endiannesses, the generated Linux TermiosCodec/TtyIoctlCodec on all ten
profiles, and the layout of ok/uapi/termios.h compiled as C and as C++. The
native termios macros are compared with the asm-generic values the ABI
generator read from Linux 6.13.4.
No user pointer, terminal object, guest or hardware is involved.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shlex
import subprocess

MARKER = (r'TERMINAL_CODEC_PASS linux_profiles=10 termios_layouts=3 roundtrip=1 residue=1 '
          r'native_wire_endians=2 hosted=1 guest=0')
SOURCES = ('tests/host/terminal_codec_tests.cpp', 'include/ok/abi/linux_termios.hpp', 'include/ok/abi/linux_user.hpp',
           'include/ok/abi/linux_profile_data.hpp', 'include/ok/core/types.hpp', 'include/ok/tty/termios.hpp',
           'include/ok/uapi/termios.h', 'include/ok/uapi/syscall.h', 'docs/abi/linux-profiles.json')
SCOPE = 'Hosted copied-byte terminal codec model; no user pointer, terminal object, guest or hardware execution.'
# The C view of the native struct: every member at its fixed offset, no padding.
LAYOUT_CHECK = '''#include <stddef.h>
#include "ok/uapi/termios.h"
_Static_assert(offsetof(struct ok_termios, c_oflag) == 4, "c_oflag");
_Static_assert(offsetof(struct ok_termios, c_cflag) == 8, "c_cflag");
_Static_assert(offsetof(struct ok_termios, c_lflag) == 12, "c_lflag");
_Static_assert(offsetof(struct ok_termios, c_line) == 16, "c_line");
_Static_assert(offsetof(struct ok_termios, c_cc) == 17, "c_cc");
_Static_assert(offsetof(struct ok_winsize, ws_ypixel) == 6, "ws_ypixel");
_Static_assert(OK_NCCS == 19 && OK_TCIFLUSH == 0 && OK_TCOFLUSH == 1 && OK_TCIOFLUSH == 2, "selectors");
int ok_termios_layout_checked;
'''
# The same header from freestanding C++ (kernel and OS C++ consumers), checked
# against the kernel-neutral termios values.
CXX_CHECK = '''#include "ok/uapi/termios.h"
#include "ok/tty/termios.hpp"
static_assert(sizeof(ok_termios) == 36 && sizeof(ok_winsize) == 8);
static_assert(OK_NCCS == ok::tty::nccs && OK_VLNEXT == static_cast<unsigned>(ok::tty::Cc::lnext));
static_assert(OK_ICRNL == ok::tty::iflag::icrnl && OK_IUTF8 == ok::tty::iflag::iutf8);
static_assert(OK_ONLCR == ok::tty::oflag::onlcr && OK_XTABS == ok::tty::oflag::xtabs);
static_assert(OK_CBAUD == ok::tty::cflag::cbaud && OK_HUPCL == ok::tty::cflag::hupcl);
static_assert(OK_ICANON == ok::tty::lflag::icanon && OK_IEXTEN == ok::tty::lflag::iexten);
int ok_termios_cxx_checked;
'''


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def native_macros(root):
    text = (root / 'include/ok/uapi/termios.h').read_text()
    return {name: int(value, 0) for name, value in re.findall(r'^#define OK_(\w+) (0x[0-9a-f]+|[0-9]+)U?$', text, re.M)}


def check_native_values(root):
    """Every flag and cc macro of ok/uapi/termios.h equals the generated asm-generic Linux value."""
    macros = native_macros(root)
    manifest = json.loads((root / 'docs/abi/linux-profiles.json').read_text())
    generic = next(row for row in manifest['profiles'] if row['architecture'] == 'x86_64')
    expected = dict(generic['termios_cc_slots'])
    expected.update({name: value for name, value in generic['termios_values'].items()
                     if not name.startswith('CIBAUD_')})
    expected.update(generic['termios_group_masks'])
    expected.update(NCCS=generic['termios_layout']['nccs'], XTABS=generic['termios_values']['TAB3'],
                    CBAUDEX=generic['termios_values']['BOTHER'], IBSHIFT=16, TCIFLUSH=0, TCOFLUSH=1, TCIOFLUSH=2)
    expected.pop('BOTHER')
    expected.pop('ADDRB')  # Linux-only (6.0+); the native UAPI does not name it
    if set(macros) != set(expected):
        raise ValueError('native termios macro set differs: ' + ', '.join(sorted(set(macros) ^ set(expected))))
    wrong = sorted(name for name in macros if macros[name] != expected[name])
    if wrong:
        raise ValueError('native termios macros differ from asm-generic Linux: ' + ', '.join(wrong))
    return len(macros)


def check_pins(root, cxx, sources):
    """Every project header the compiled sources include is pinned in SOURCES."""
    included = set()
    for source in sources:
        result = subprocess.run([*shlex.split(cxx), '-std=c++23', '-I' + str(root / 'include'), '-MM', str(source)],
                                check=True, capture_output=True, text=True)
        for token in result.stdout.replace('\\\n', ' ').split()[1:]:
            path = Path(token).resolve()
            if path.is_relative_to(root):
                included.add(path.relative_to(root).as_posix())
    missing = sorted(included - set(SOURCES))
    if missing:
        raise ValueError('compiled headers missing from SOURCES: ' + ', '.join(missing))
    return len(included)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--cc', default=os.environ.get('CC', 'gcc'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    if output.is_relative_to(root):
        raise SystemExit('--output must be outside the source tree')
    output.mkdir(parents=True, exist_ok=False)
    pins = {name: sha(root / name) for name in SOURCES}
    state = dict(status='running', scope=SCOPE, guest_execution=False, sanitizer=args.sanitize, source_sha256=pins)
    flags = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer'] \
        if args.sanitize else []
    binary = output / 'terminal-codec'
    try:
        state['native_macros_checked'] = check_native_values(root)
        layout = output / 'termios_layout.c'
        layout.write_text(LAYOUT_CHECK)
        subprocess.run([*shlex.split(args.cc), '-std=c11', '-ffreestanding', '-Wall', '-Wextra', '-Wpedantic',
                        '-Werror', '-I' + str(root / 'include'), '-c', str(layout), '-o', str(output / 'layout.o')],
                       check=True)
        header_cxx = output / 'termios_header.cpp'
        header_cxx.write_text(CXX_CHECK)
        subprocess.run([*shlex.split(args.cxx), '-std=c++23', '-ffreestanding', '-Wall', '-Wextra', '-Wpedantic',
                        '-Werror', '-I' + str(root / 'include'), '-c', str(header_cxx), '-o',
                        str(output / 'header_cxx.o')], check=True)
        state['cxx_header_checked'] = True
        state['pinned_headers_checked'] = check_pins(root, args.cxx, (root / SOURCES[0], header_cxx))
        subprocess.run([*shlex.split(args.cxx), '-std=c++23', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                        *flags, '-I' + str(root / 'include'), str(root / SOURCES[0]), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=600)
        (output / 'terminal-codec.log').write_text(result.stdout + result.stderr)
        lines = re.findall('^' + MARKER + '$', result.stdout, re.M)
        if len(lines) != 1:
            raise ValueError('missing exact completion marker')
        if any(sha(root / name) != digest for name, digest in pins.items()):
            raise ValueError('source changed during the run')
        state.update(status='passed', marker=lines[0])
    except BaseException as error:
        state.update(status='failed', error=repr(error))
        raise
    finally:
        (output / 'result.json').write_text(json.dumps(state, indent=2) + '\n')
    print(lines[0], flush=True)


if __name__ == '__main__':
    main()
