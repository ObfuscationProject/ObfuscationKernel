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
source,binary,files,tools,model_receipt=build_model(root,out,'copy-order',a.sanitize)
env = dict(os.environ, XMAKE_ROOT='y', XMAKE_GLOBALDIR=str(out / 'xmake-global'), ASAN_OPTIONS='detect_leaks=0:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
state = dict(status='running', scope='Hosted maintained symlink access and stat late-copy error order; callbacks model user memory; no guest, syscall dispatch or power-loss claim', sanitized=a.sanitize, model_receipt_sha256=sha(model_receipt), source_sha256=files, tools=tools, commands=[], rows=[], artifacts={})
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
    for block in (1024,2048,4096):
        for stride in (128,256):
            label=f'{block}-{stride}'
            image=out/(label+'.img');shutil.copy2(oracle/label/'input.img',image)
            entry=next(r for r in reference['rows'] if (r['block_size'],r['inode_size'],r['name'])==(block,stride,'fast'))
            assert sha(image)==entry['disk_sha256'];raw=image.read_bytes()
            first=struct.unpack_from('<I',raw,1044)[0];ipg=struct.unpack_from('<I',raw,1064)[0];ino=entry['ino']
            table=struct.unpack_from('<I',raw,(first+1)*block+((ino-1)//ipg)*32+8)[0];offset=table*block+((ino-1)%ipg)*stride
            for mode in ('late-output','bad-flags','unavailable-clock','nofollow'):
                inode=out/(label+'-'+mode+'.inode')
                log=run([binary,image,inode,offset,stride,mode],label+'-'+mode)
                assert 'PASS hosted late stat copy policy' in log
                if mode=='late-output':
                    data=inode.read_bytes();assert struct.unpack_from('<i',data,8)[0]==1800000100
                    if stride==256:assert struct.unpack_from('<I',data,140)[0]==123456789<<2
                state['rows'].append(dict(geometry=label,mode=mode,output_sha256=sha(inode)))
            assert sha(image)==entry['disk_sha256']
    for name, digest in files.items():
        assert sha(source / name) == digest, name
    for name, digest in tools.items():
        assert sha(Path(name)) == digest, name
    assert len(state['rows']) == 24
    state.update(status='passed',cases=24)
    state['artifacts'] = {str(p.relative_to(out)): sha(p) for p in out.rglob('*') if p.is_file() and 'input' not in p.relative_to(out).parts and p.name != 'result.json'}
except BaseException as error:
    state.update(status='failed', error=repr(error))
    save()
    raise
save()
print('PASS 24 hosted maintained symlink/stat copy-order cases')
