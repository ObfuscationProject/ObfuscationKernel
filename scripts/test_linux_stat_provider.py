"""Hosted common ext4 allocation query tests against explicit offline fixtures."""
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
source,binary,files,tools,model_receipt=build_model(root,out,'provider',a.sanitize)
env=dict(os.environ,XMAKE_ROOT='y',ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
commands=[]
def run(cmd,label):
 cmd=list(map(str,cmd));log=out/(label+'.log')
 with log.open('w') as stream:r=subprocess.run(cmd,cwd=source,env=env,stdout=stream,stderr=subprocess.STDOUT)
 commands.append(dict(argv=cmd,returncode=r.returncode,log_sha256=sha(log)))
 (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n');assert r.returncode==0,log
oracle=a.oracle.resolve();reference=json.loads((oracle/'result.json').read_text());assert reference['status']=='pass'
rows=[];images={}
for block in (1024,2048,4096):
 for stride in (128,256):
  label=f'{block}-{stride}';image=out/(label+'.img');shutil.copy2(oracle/label/'input.img',image)
  relevant=[r for r in reference['rows'] if (r['block_size'],r['inode_size'])==(block,stride)];assert len(relevant)==6
  assert {r['disk_sha256'] for r in relevant}=={sha(image)};images[label]=sha(image)
  raw=image.read_bytes();u32=lambda at:struct.unpack_from('<I',raw,at)[0]
  first=u32(1024+20);ipg=u32(1024+40)
  for mode in ('readonly','maintained','legacy'):
   for entry in relevant:
    ino=entry['ino'];table=u32((first+1)*block+((ino-1)//ipg)*32+8);offset=table*block+((ino-1)%ipg)*stride
    tests=[(0,'qualified')]
    if entry['name']=='four':tests += [(28,'io'),(32,'unavailable'),(104,'unavailable'),(116,'unavailable'),(118,'unavailable'),(0,'read-fault'),(0,'logical-overflow')]
    if entry['name']=='fast':tests += [(28,'io'),(104,'unavailable')]
    if entry['name'] in ('sparse','dir','block'):
     tests += [(0,'capacity-'+str(maximum)) for maximum in (1,2,3)]
     tests += [(0,'depth-'+str(depth)) for depth in (4,5)]
    for field,result in tests:
     name=f'{label}-{mode}-{entry["name"]}-{field}-{result}'
     run([binary,image,mode,entry['name'],offset,field,result,entry['blocks512']],name)
     rows.append(dict(block_size=block,inode_size=stride,mode=mode,node=entry['name'],field=field,expected=result))
  assert sha(image)==images[label]
for name,h in files.items():assert sha(source/name)==h
for path,h in tools.items():assert sha(Path(path))==h
r=dict(status='pass',scope='Hosted provider allocation metadata query; no guest, syscall, SDK export, mutation or power-loss qualification',sanitized=a.sanitize,model_receipt_sha256=sha(model_receipt),rows=rows,source_sha256=files,tools=tools,binary_sha256=sha(binary),oracle_receipt_sha256=sha(oracle/'result.json'),images=images,commands=commands)
(out/'result.json').write_text(json.dumps(r,indent=2)+'\n');print('PASS',len(rows),'hosted provider query cases')
