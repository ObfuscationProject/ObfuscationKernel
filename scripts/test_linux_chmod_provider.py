"""Retain repaired provider and independent unsupported-inode regression evidence."""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--sanitize',action='store_true');p.add_argument('--oracle',type=Path,required=True);a=p.parse_args()
kernel=Path(__file__).resolve().parents[1];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False);source=out/'input';source.mkdir()
sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
files=[p for p in (kernel/'include').rglob('*') if p.is_file()]
files += [kernel/n for n in ('src/fs/ext4_fs.cpp','src/syscall/linux.cpp','tests/host/chmod_provider_tests.cpp','tests/host/chmod_unsupported_inode_tests.cpp','tests/host/ext4_realtime_model.cpp','tests/host/ext4_realtime_model.hpp')]
for p in files:
 h=sha(p);target=source/p.relative_to(kernel);target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,target);assert sha(p)==sha(target)==h
shutil.copy2(__file__,source/'test_linux_chmod_provider.py')
lua=['set_project("chmod-provider-admission")','set_languages("cxx23")']
for name,test in [('provider','chmod_provider_tests.cpp'),('unsupported','chmod_unsupported_inode_tests.cpp')]:
 lua += [f'target("chmod-{name}")','set_kind("binary")','add_includedirs("include")','add_cxxflags("-O1","-g","-frtti","-Wall","-Wextra","-Werror","-ffunction-sections","-fdata-sections",{force=true})','add_ldflags("-Wl,--gc-sections",{force=true})','add_files("src/fs/ext4_fs.cpp","src/syscall/linux.cpp","tests/host/ext4_realtime_model.cpp",'+json.dumps('tests/host/'+test)+')']
 if a.sanitize:lua += ['add_cxxflags("-fsanitize=address,undefined","-fno-omit-frame-pointer",{force=true})','add_ldflags("-fsanitize=address,undefined",{force=true})']
 lua += ['target_end()']
(source/'xmake.lua').write_text('\n'.join(lua)+'\n');inputs={str(p.relative_to(source)):sha(p) for p in source.rglob('*') if p.is_file()}
r=dict(status='running',scope='Hosted ext4 checked mode provider and rejected i_size_high admission; no service/dispatcher/guest/SDK',sanitized=a.sanitize,source_sha256=inputs,commands=[],images={},binaries={})
def save():(out/'result.json').write_text(json.dumps(r,indent=2)+'\n')
def run(cmd,label):
 cmd=list(map(str,cmd));log=out/(label+'.log')
 with log.open('x') as stream:done=subprocess.run(cmd,cwd=source,env={**os.environ,'XMAKE_ROOT':'y','XMAKE_GLOBALDIR':str(out/'xmake-global'),'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'},stdout=stream,stderr=subprocess.STDOUT)
 r['commands'].append(dict(label=label,argv=cmd,returncode=done.returncode,log_sha256=sha(log)));save();assert done.returncode==0,log
save()
try:
 run(['xmake','f','-P',source,'-c','-y','--ccache=n'],'configure');run(['xmake','build','-P',source,'-j2','-v'],'build')
 for name in ('provider','unsupported'):
  binary=source/'build/linux/x86_64/release'/('chmod-'+name);r['binaries'][name]=dict(path=str(binary),sha256=sha(binary))
  for block in (1024,2048,4096):
   for stride in (128,256):
    label=f'{block}-{stride}';original=a.oracle.resolve()/label/'input.img';image=out/(label+'.img')
    if not image.exists():shutil.copy2(original,image)
    h=sha(image);r['images'][str(image)]=h;run([binary,image],name+'-'+label);assert sha(image)==sha(original)==h
 for n,h in inputs.items():assert sha(source/n)==h
 r['status']='passed'
except BaseException as e:r.update(status='failed',error=repr(e));save();raise
save();print('PASS six-layout checked provider and unsupported-inode errno regression')
