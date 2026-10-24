"""Build an explicit repository-owned hosted stat suite and retain its identity."""
from pathlib import Path
import hashlib
import json
import subprocess
import sys


def build_model(root, output, suite, sanitize):
    folder = output / 'model'
    command = [sys.executable, str(root / 'scripts/test_linux_stat.py'),
               '--output', str(folder), '--suite', suite, '--build-only']
    if sanitize:
        command.append('--sanitize')
    log = output / 'model-build.log'
    with log.open('x') as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f'hosted {suite} build failed; see {log}')
    receipt = folder / 'result.json'
    record = json.loads(receipt.read_text())
    assert record['status'] == 'built-not-executed' and record['suites'] == [suite]
    assert record['sanitized'] == sanitize
    artifact = record['binaries'][suite]
    binary = Path(artifact['path'])
    assert binary.resolve().is_relative_to(folder.resolve())
    assert hashlib.sha256(binary.read_bytes()).hexdigest() == artifact['sha256']
    return folder / 'input', binary, record['source_sha256'], record['tools'], receipt
