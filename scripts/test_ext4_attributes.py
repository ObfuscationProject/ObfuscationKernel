#!/usr/bin/env python3
"""Compare provider attributes to raw ext4 bytes and independent debugfs output."""
import argparse, hashlib, json, os, re, shutil, struct, subprocess
from pathlib import Path

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--sanitize', action='store_true')
    args = p.parse_args(); root = Path(__file__).resolve().parents[1]
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    report = {'scope': 'hosted-file-backed-ext4-attributes', 'status': 'running', 'fixtures': [], 'commands': [], 'asan_options': os.environ.get('ASAN_OPTIONS', '')}
    def save(): (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    def run(command, name):
        result = subprocess.run([str(x) for x in command], text=True, capture_output=True)
        (out / (name + '.log')).write_text(result.stdout + result.stderr)
        report['commands'].append({'argv': [str(x) for x in command], 'label': name, 'returncode': result.returncode}); save()
        if result.returncode: raise RuntimeError(name)
        return result.stdout
    def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
    save()
    try:
        binary = out / 'ext4-attributes'
        run(['g++', '-std=c++23', '-O1', '-Wall', '-Wextra', '-Werror', '-frtti', '-fno-exceptions', '-DOK_ARCH_TARGET_X86_64', '-I' + str(root/'include'), root/'tests/host/ext4_attributes_tests.cpp', root/'src/fs/ext4_fs.cpp', root/'tests/host/ext4_realtime_model.cpp', *(['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else []), '-o', binary], 'compile')
        seed = out/'seed'; seed.mkdir(); (seed/'sample').write_bytes(b'sample\n'); (seed/'sample').chmod(0o640)
        for block_size in (1024, 2048, 4096):
            for inode_size in (128, 256):
                label = f'{block_size}-{inode_size}'; disk = out/(label+'.img')
                run(['mke2fs', '-q', '-t', 'ext4', '-b', block_size, '-I', inode_size, '-O', 'none,extent,filetype,sparse_super,large_file', '-E', 'lazy_itable_init=0', '-m', '0', '-d', seed, disk, '16M'], label+'-format')
                stat = run(['debugfs', '-R', 'stat /sample', disk], label+'-inode')
                number = int(re.search(r'Inode:\s+(\d+)', stat)[1]); raw = bytearray(disk.read_bytes())
                u32 = lambda at: struct.unpack_from('<I', raw, at)[0]
                group, index = divmod(number - 1, u32(1064)); table = u32((u32(1044) + 1)*block_size + group*32 + 8)
                at = table*block_size + index*inode_size
                struct.pack_into('<III', raw, at+8, 0xffffffff, 0, 0x80000000)
                if inode_size == 256:
                    struct.pack_into('<H', raw, at+128, 32)
                    struct.pack_into('<III', raw, at+132, 0, (123 << 2) | 1, (999999999 << 2) | 3)
                disk.write_bytes(raw)
                # Independent signed integer oracle, without the C++ helper.
                times = []
                for base, extension in ((8, 140), (16, 136), (12, 132)):
                    seconds = struct.unpack_from('<i', raw, at+base)[0]
                    extra = u32(at+extension) if inode_size == 256 else 0
                    times += [seconds + (extra & 3)*4294967296, extra >> 2]
                run(['debugfs', '-R', 'stat /sample', disk], label+'-oracle')
                before = sha(disk)
                command = [binary, disk, 'read', number, *times, inode_size]
                run(command, label+'-read'); assert sha(disk) == before
                run(['e2fsck', '-fn', disk], label+'-readonly-fsck')
                report['fixtures'].append({'image': str(disk), 'sha256': before, 'block_size': block_size, 'inode_size': inode_size, 'inode': number, 'inode_offset': at, 'times': times}); save()
                writable = out/(label+'-writable.img'); shutil.copyfile(disk, writable)
                run([binary, writable, 'write', number, *times, inode_size], label+'-write')
                run(['e2fsck', '-fn', writable], label+'-writable-fsck')
                if inode_size == 256:
                    invalid = out/(label+'-invalid.img'); struct.pack_into('<I', raw, at+140, 1000000000 << 2); invalid.write_bytes(raw)
                    invalid_sha = sha(invalid); run([binary, invalid, 'invalid', number, *times, inode_size], label+'-invalid'); assert sha(invalid) == invalid_sha
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error=str(error)); raise
    finally: save()
if __name__ == '__main__': main()
