#!/usr/bin/env python3
"""Generate UNIX message syscall numbers from the exact pinned musl archive."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import tarfile
PORTS = ('i386', 'x86_64', 'aarch64', 'arm', 'riscv64', 'riscv32', 'loongarch64', 'mips', 'mips64', 'powerpc')
ARCHIVE_SHA = 'a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4'
def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('--musl-archive', type=Path, required=True)
    cli.add_argument('--check', action='store_true')
    args = cli.parse_args()
    if hashlib.sha256(args.musl_archive.read_bytes()).hexdigest() != ARCHIVE_SHA:
        raise ValueError('Exact pinned musl archive required')
    root = Path(__file__).resolve().parents[1]
    prior = json.loads((root / 'docs/abi/linux-profiles.json').read_text())
    pins, rows = {}, []
    with tarfile.open(args.musl_archive, 'r:gz') as archive:
        def read(name):
            data = archive.extractfile('musl-1.2.5/' + name).read()
            pins[name] = hashlib.sha256(data).hexdigest()
            return data.decode()
        for port in PORTS:
            name = 'arch/' + port + '/bits/syscall.h.in'
            source = read(name)
            if pins[name] != prior['source_file_sha256']['musl/' + name]:
                raise ValueError('Predecessor syscall source pin mismatch')
            definitions = dict(re.findall(r'^#define\s+(__NR_\w+)\s+(\d+)\s*$', source, re.M))
            rows.append(tuple(int(definitions.get('__NR_' + operation, 0)) for operation in ('sendmsg', 'recvmsg', 'socketcall')))
        read('include/sys/socket.h')
        for port in ('generic', 'mips', 'mips64', 'powerpc'):
            read('arch/' + port + '/bits/socket.h')
        read('src/network/sendmsg.c'); read('src/network/recvmsg.c'); read('src/internal/syscall.h')
    lines = ['#pragma once', '', '// Generated from exact musl 1.2.5 archive; see docs/abi/linux-unix-rights-source.json.',
        '#include "ok/abi/linux_user.hpp"', '', 'namespace ok::abi::linux_user', '{',
        'struct UnixMessageNumbers { u32 sendmsg, recvmsg, socketcall; };',
        'inline constexpr std::array<UnixMessageNumbers, static_cast<usize>(Profile::count)> unix_message_numbers{{']
    lines += ['    {' + ', '.join(str(value) + 'u' for value in row) + '}, // ' + port for port, row in zip(PORTS, rows)]
    lines += ['}};', '} // namespace ok::abi::linux_user', '']
    output = '\n'.join(lines)
    provenance = dict(schema_version=1, musl_version='1.2.5', archive_sha256=ARCHIVE_SHA,
        scope='Bounded immediate UNIX stream SCM_RIGHTS byte codec; source inspection is not trap execution',
        source_file_sha256=pins)
    target = root / 'include/ok/abi/linux_unix_rights_data.hpp'
    manifest = root / 'docs/abi/linux-unix-rights-source.json'
    if args.check:
        if target.read_text() != output or json.loads(manifest.read_text()) != provenance:
            raise ValueError('Generated UNIX message provenance is stale')
    else:
        target.write_text(output)
        manifest.write_text(json.dumps(provenance, indent=2, sort_keys=True) + '\n')
    print('verified ten exact musl UNIX message syscall profiles')
if __name__ == '__main__': main()
