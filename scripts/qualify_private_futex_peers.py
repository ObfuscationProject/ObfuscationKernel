#!/usr/bin/env python3
"""Execute raw peer-futex ELF through an exact Debug SDK and SDK-only OS consumer.

The opt-in producer is built separately. This command never compiles Kernel
source and refuses an SDK that lacks the explicit peer-fixture define.
"""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess,select,time,sys,re
from private_futex_peer_evidence import validate
ap=argparse.ArgumentParser()
ap.add_argument('--arch',choices=('i386','x86_64','aarch64','arm32','rv64','rv32','loongarch64','mips','mips64','ppc'),required=True)
for n in ('os-source','os-source-inventory','sdk','elf','toolchain','qemu','out'):
 ap.add_argument('--'+n,type=Path,required=True)
for n in ('os-source-inventory-sha256','sdk-sha256','elf-sha256','qemu-sha256','compiler-sha256'):
 ap.add_argument('--'+n,required=True)
ap.add_argument('--timeout',type=float,default=100)
a=ap.parse_args()
if not 0<a.timeout<=600:raise ValueError('timeout must be in (0,600] seconds')
S=a.os_source.resolve();SDK=a.sdk.resolve();ELF=a.elf.resolve();R=a.out.resolve()
if any(R==p or p.is_relative_to(R) or R.is_relative_to(p) for p in (S,SDK)):raise ValueError('output overlaps source or SDK')
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
if sha(SDK/'manifest.json')!=a.sdk_sha256 or sha(ELF)!=a.elf_sha256:raise ValueError('SDK/ELF input hash mismatch')
sys.path.insert(0,str(S/'tools'))
from object_pressure_evidence import admit_sources
inventory=admit_sources(S,a.os_source_inventory,a.os_source_inventory_sha256)
R.mkdir(parents=True,exist_ok=False);C=R/'ObfuscationOS';C.mkdir()
r={'status':'preparing','scope':'Actual raw-ELF harness-created peer WAIT/WAKE on QEMU, SDK-only OS consumer, no clone/pthread/SMP/physical claim','arch':a.arch,'mode':'debug','sdk_only':True,'kernel_source_compiled_by_consumer':False,'inputs':{},'commands':[]}
def pin(p):r['inputs'][str(p)]=sha(p)
def save():(R/'result.json').write_text(json.dumps(r,indent=2)+'\n')
for n,h in inventory['files'].items():
 p=S/n;q=C/n;q.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,q)
 if sha(q)!=h:raise ValueError('source changed while copying: '+n)
 pin(q)
shutil.copy2(a.os_source_inventory,R/'os-source-inventory.json');pin(R/'os-source-inventory.json')
shutil.copytree(SDK,R/'sdk');SDK=R/'sdk'
for p in SDK.rglob('*'):
 if p.is_file():pin(p)
shutil.copy2(ELF,R/'peer.elf');pin(R/'peer.elf')
sys.path.insert(0,str(C/'tools'));from kernel_sdk import validate_sdk,ARCH_TRIPLES
sys.path.insert(0,str(C/'scripts'));from qemu_distro import command_for
TC=a.toolchain.resolve();Q=a.qemu.resolve();compiler=TC/'bin'/(ARCH_TRIPLES[a.arch]+'-g++')
if sha(Q)!=a.qemu_sha256 or sha(compiler)!=a.compiler_sha256:raise ValueError('tool identity mismatch')
pin(Q);pin(compiler)
manifest=validate_sdk(SDK,a.arch,'debug',compiler)
if 'OK_PRIVATE_FUTEX_PEER_FIXTURE' not in manifest['defines']:raise ValueError('SDK was not built with explicit peer fixture')
r['sdk_manifest_sha256']=sha(SDK/'manifest.json');r['elf_sha256']=sha(R/'peer.elf');save()
env={**os.environ,'XMAKE_ROOT':'y','XMAKE_GLOBALDIR':str(R/'xmake-global'),'PYTHONDONTWRITEBYTECODE':'1'}
def call(label,cmd):
 cmd=list(map(str,cmd))
 with (R/(label+'.log')).open('x') as f:p=subprocess.run(cmd,cwd=C,env=env,stdout=f,stderr=subprocess.STDOUT)
 r['commands'].append({'label':label,'argv':cmd,'returncode':p.returncode,'log_sha256':sha(R/(label+'.log'))});save();assert p.returncode==0,label
proc=None
try:
 call('configure',['xmake','f','-P',C,'-c','-y','-a',a.arch,'-m','debug','--systoolchain_dir='+str(TC),'--kernel_sdk='+str(SDK),'--ccache=n'])
 call('distro',['xmake','build','-P',C,'-j2','-v','distro_image'])
 B=C/'build'/a.arch/'debug';shutil.copy2(B/'distro/kernel.bin',R/'kernel-input.bin');pin(R/'kernel-input.bin');pin(B/'distro/kernel.elf')
 shutil.copytree(B/'rootfs',R/'root',symlinks=True);shutil.copy2(R/'peer.elf',R/'root/init');(R/'root/init').chmod(0o755)
 call('disk',['python3','-B',C/'tools/mkext4.py','--root',R/'root','--out',R/'rootfs.ext4.img'])
 call('fsck',['e2fsck','-fn',R/'rootfs.ext4.img']);shutil.copy2(R/'rootfs.ext4.img',R/'rootfs-input.ext4.img');pin(R/'rootfs-input.ext4.img')
 for name,h in r['inputs'].items():assert sha(Path(name))==h,name
 cmd=list(map(str,command_for(Q,a.arch,R/'kernel-input.bin',R/'rootfs.ext4.img','none')));r['qemu_command']=cmd;r['status']='running';save()
 proc=subprocess.Popen(cmd,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT);serial=bytearray();deadline=time.monotonic()+a.timeout;passed=False
 while time.monotonic()<deadline:
  ready,_,_=select.select([proc.stdout],[],[],0.25)
  if ready:
   b=os.read(proc.stdout.fileno(),65536)
   if not b:break
   serial+=b;(R/'serial.log').write_bytes(serial)
   if b'OK_PRIVATE_FUTEX_PEERS status=pass ' in serial:passed=True;break
   if b'OK_PRIVATE_FUTEX_PEERS status=fail' in serial or b'OK_TEST_FAIL' in serial:break
  if proc.poll() is not None:break
 if proc.poll() is None:proc.terminate()
 tail=proc.communicate(timeout=10)[0];serial+=tail;(R/'serial.log').write_bytes(serial)
 assert passed,'guest did not complete peer fixture'
 execution=validate(serial.decode(errors='replace'))
 r['execution']=execution
 for name,h in r['inputs'].items():assert sha(Path(name))==h,name
 r.update(status='passed',executed=True,traps=20,rounds=2,threads=4,actual_user_wakes=6,control_completions=4,serial_sha256=sha(R/'serial.log'),final_disk_sha256=sha(R/'rootfs.ext4.img'),qemu_returncode=proc.returncode);save()
except BaseException as e:
 if proc and proc.poll() is None:proc.terminate();proc.communicate(timeout=10)
 r.update(status='failed',error=repr(e));save();raise
print(json.dumps({k:v for k,v in r.items() if k!='inputs'},indent=2))
