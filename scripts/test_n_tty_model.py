#!/usr/bin/env python3
"""Build and run the hosted N_TTY value-template model (tests/host/n_tty_model_tests.cpp).

Model evidence only: directed Linux n_tty cases, a seeded differential run and
the raw-pair FIFO equivalence. No terminal object, runtime, guest or hardware runs.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shlex
import subprocess

MARKER = (r'N_TTY_MODEL_PASS canonical=4096 echo=512 model_steps=100000 utf8_erase=1 isig=1 vmin_vtime=1 '
          r'opost=1 raw_equivalence=1 constexpr=1 guest=0')
SOURCES = ('tests/host/n_tty_model_tests.cpp', 'include/ok/tty/n_tty.hpp', 'include/ok/tty/termios.hpp',
           'include/ok/core/types.hpp')
SCOPE = 'Hosted N_TTY value-template model; no terminal object, runtime wiring, guest or hardware execution.'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
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
    binary = output / 'n-tty-model'
    try:
        subprocess.run([*shlex.split(args.cxx), '-std=c++23', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                        *flags, '-I' + str(root / 'include'), str(root / SOURCES[0]), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=600)
        (output / 'n-tty-model.log').write_text(result.stdout + result.stderr)
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
