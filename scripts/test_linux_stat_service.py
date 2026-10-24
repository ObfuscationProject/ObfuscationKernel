"""Hosted stat composition against explicit raw ext4 fixtures; no guest."""
from pathlib import Path
import argparse,hashlib,json,shutil,subprocess,os,re,struct,sys
from linux_stat_test_support import build_model
p=argparse.ArgumentParser()
p.add_argument('--output',type=Path,required=True)
p.add_argument('--sanitize',action='store_true')
p.add_argument('--oracle',type=Path,required=True)
a=p.parse_args()
root=Path(__file__).resolve().parents[1]
out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
source,binary,files,tools,model_receipt=build_model(root,out,'service',a.sanitize)
env = dict(os.environ, XMAKE_ROOT='y', XMAKE_GLOBALDIR=str(out / 'xmake-global'), ASAN_OPTIONS='detect_leaks=0:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
state = dict(status='running', scope='Hosted real common VFS/PosixService/Ext4 provider and stat codec composition; no guest, syscall dispatch or SDK qualification', sanitized=a.sanitize, model_receipt_sha256=sha(model_receipt), source_sha256=files, tools=tools, commands=[], rows=[], artifacts={})
def save():
    (out / 'result.json').write_text(json.dumps(state, indent=2) + '\n')
def run(cmd, label):
    cmd = list(map(str, cmd))
    log = out / (label + '.log')
    with log.open('w') as stream:
        r = subprocess.run(cmd, cwd=source, env=env, stdout=stream, stderr=subprocess.STDOUT)
    state['commands'].append(dict(argv=cmd, returncode=r.returncode, log_sha256=sha(log)))
    save()
    assert r.returncode == 0, log
    return log.read_text()
def encode(layout, kind, order, values):
    data = bytearray(layout[kind + '.size'])
    for name, value in values.items():
        prefix = kind + '.' + name
        at, width = layout[prefix + '.offset'], layout[prefix + '.size']
        if isinstance(value, tuple):
            assert width == 16
            data[at:at+8] = value[0].to_bytes(8, order, signed=True)
            data[at+8:at+12] = value[1].to_bytes(4, order)
        else:
            data[at:at+width] = value.to_bytes(width, order, signed=value < 0)
    return data
save()
try:
    state['binary_sha256'] = sha(binary)
    oracle = a.oracle.resolve()
    shutil.copy2(oracle / 'result.json', out / 'oracle.json')
    reference = json.loads((out / 'oracle.json').read_text())
    assert reference['status'] == 'pass'
    shutil.copy2(source / 'docs/abi/linux-stat-target-layouts.json', out / 'target-layouts.json')
    layouts = json.loads((out / 'target-layouts.json').read_text())['profiles']
    assert len(layouts) == 10
    state['oracle_receipt_sha256'] = sha(out / 'oracle.json')
    state['target_layouts_sha256'] = sha(out / 'target-layouts.json')
    compared = 0
    for block in (1024, 2048, 4096):
        for stride in (128, 256):
            label = f'{block}-{stride}'
            image = out / (label + '.img')
            shutil.copy2(oracle / label / 'input.img', image)
            image_sha = sha(image)
            rows = [r for r in reference['rows'] if (r['block_size'], r['inode_size']) == (block, stride)]
            assert len(rows) == 6 and {r['disk_sha256'] for r in rows} == {image_sha}
            raw = image.read_bytes()
            for mode in ('readonly', 'legacy'):
                folder = out / (label + '-' + mode)
                folder.mkdir()
                log = run([binary, image, folder, mode], label + '-' + mode)
                assert 'PASS hosted common service/provider/codec; profiles=10 guest=0 dispatcher=0' in log
                nodes = re.findall(r'^NODE (\w+) ((?:\d+ ){8}\d+)$', log, re.M)
                assert len(nodes) == 6, nodes
                observed = {name: list(map(int, values.split())) for name, values in nodes}
                devices = {(v[1], v[2]) for v in observed.values()}
                assert len(devices) == 1
                for entry in rows:
                    name = entry['name']
                    inode = (oracle / label / (name + '.inode')).read_bytes()
                    assert hashlib.sha256(inode).hexdigest() == entry['raw_sha256']
                    ino = entry['ino']
                    first = struct.unpack_from('<I', raw, 1044)[0]
                    ipg = struct.unpack_from('<I', raw, 1064)[0]
                    table = struct.unpack_from('<I', raw, (first+1)*block + ((ino-1)//ipg)*32 + 8)[0]
                    at = table*block + ((ino-1)%ipg)*stride
                    assert raw[at:at+stride] == inode
                    (folder / (name + '.inode')).write_bytes(inode)
                    u16 = lambda at: struct.unpack_from('<H', inode, at)[0]
                    u32 = lambda at: struct.unpack_from('<I', inode, at)[0]
                    expected = [ino, *next(iter(devices)), u16(0), u16(2) | (u16(120) << 16), u16(24) | (u16(122) << 16), u16(26), u32(4) | (u32(108) << 32), entry['blocks512']]
                    assert observed[name] == expected, (name, observed[name], expected)
                    major, minor = expected[1:3]
                    extra_size = u16(128) if stride == 256 else 0
                    def stamp(base, extra):
                        sec = struct.unpack_from('<i', inode, base)[0]
                        word = u32(extra) if extra+4 <= 128+extra_size else 0
                        return sec + ((word & 3) << 32), word >> 2
                    times = [stamp(8, 140), stamp(16, 136), stamp(12, 132)]
                    for profile in layouts:
                        arch = profile['architecture']
                        order = 'big' if arch in ('mips', 'mips64', 'ppc') else 'little'
                        values = dict(stx_mask=0x7ff, stx_blksize=block, stx_nlink=expected[6], stx_uid=expected[4], stx_gid=expected[5], stx_mode=expected[3], stx_ino=ino, stx_size=expected[7], stx_blocks=expected[8], stx_atime=times[0], stx_mtime=times[1], stx_ctime=times[2], stx_dev_major=major, stx_dev_minor=minor)
                        pairs = [('statx', 'statx', values)]
                        if arch in ('x86_64', 'aarch64', 'rv64'):
                            values = dict(st_dev=(minor & 255) | (major << 8) | ((minor & ~255) << 12), st_ino=ino, st_nlink=expected[6], st_mode=expected[3], st_uid=expected[4], st_gid=expected[5], st_size=expected[7], st_blksize=block, st_blocks=expected[8])
                            for letter, (sec, ns) in zip('amc', times):
                                values['st_' + letter + 'time_sec'] = sec
                                values['st_' + letter + 'time_nsec'] = ns
                            pairs.append(('kernel', 'kstat', values))
                        for suffix, kind, values in pairs:
                            path = folder / f'{name}-{arch}-{suffix}.bin'
                            if mode == 'legacy':
                                assert not path.exists()
                            else:
                                golden = encode(profile['layout'], kind, order, values)
                                assert path.read_bytes() == golden, path
                                (folder / f'{name}-{arch}-{suffix}.golden').write_bytes(golden)
                                compared += 1
                    state['rows'].append(dict(geometry=label, mode=mode, node=name, inode=ino, blocks512=entry['blocks512'], metadata_matches_raw=True))
                assert len(list(folder.glob('*.bin'))) == (78 if mode == 'readonly' else 0)
                assert sha(image) == image_sha
                save()
    for name, digest in files.items():
        assert sha(source / name) == digest, name
    for name, digest in tools.items():
        assert sha(Path(name)) == digest, name
    assert compared == 468 and len(state['rows']) == 72
    state.update(status='passed', complete_binary_comparisons=compared, legacy_codec_refusals=468)
    state['artifacts'] = {str(p.relative_to(out)): sha(p) for p in out.rglob('*') if p.is_file() and 'input' not in p.relative_to(out).parts and p.name != 'result.json'}
except BaseException as error:
    state.update(status='failed', error=repr(error))
    save()
    raise
save()
print('PASS 72 hosted service nodes; 468 complete output comparisons; 468 legacy codec refusals')
