#!/usr/bin/env python3
"""Build a raw freestanding C++23 peer-futex ELF; no execution claim."""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess
PROFILES={
'i386':('i386-elf',['-m32','-march=i686']), 'x86_64':('x86_64-elf',['-m64','-mno-red-zone']),
'aarch64':('aarch64-elf',['-march=armv8-a']), 'arm32':('arm-none-eabi',['-march=armv7-a','-marm','-mfloat-abi=soft']),
'rv64':('riscv64-elf',['-march=rv64gc','-mabi=lp64d']), 'rv32':('riscv32-elf',['-march=rv32gc','-mabi=ilp32d']),
'loongarch64':('loongarch64-elf',['-march=loongarch64','-mabi=lp64d','-msimd=none']),
'mips':('mips-elf',['-march=mips32r2','-mabi=32','-EB','-mhard-float','-mno-abicalls','-G0']),
'mips64':('mips64-elf',['-march=mips64r2','-mabi=64','-EB','-mhard-float','-mno-abicalls','-G0']),
'ppc':('powerpc-eabi',['-mcpu=8548','-msoft-float','-mlong-double-64','-msdata=none'])}
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--arch',choices=PROFILES,required=True);ap.add_argument('--toolchain',type=Path,required=True);ap.add_argument('--out',type=Path,required=True);a=ap.parse_args()
 R=a.out.resolve();R.mkdir(parents=True,exist_ok=False);P=R/'source';P.mkdir();base=Path(__file__).resolve().parents[1]/'tests/native'
 names=['private_futex_peer_user.cpp','private_futex_peer_user.S','private_futex_peer_user.ld','private_futex_peer_shared.hpp']
 for n in names:shutil.copy2(base/n,P/n)
 triple,flags=PROFILES[a.arch];cxx=a.toolchain.resolve()/'bin'/(triple+'-g++');cc=a.toolchain.resolve()/'bin'/(triple+'-gcc');assert cxx.is_file() and cc.is_file()
 root=0x10000000 if a.arch in ('arm32','aarch64') else 0x40000000
 define='OK_ARCH_TARGET_'+a.arch.upper();q=json.dumps
 lua=['set_project("raw-private-futex-peer")','set_languages("cxx23")','toolchain("raw-peer")','set_kind("standalone")',f'set_toolset("cxx",{q(str(cxx))})',f'set_toolset("as",{q(str(cc))})',f'set_toolset("ld",{q(str(cxx))})','toolchain_end()','target("peer")','set_kind("binary")','set_toolchains("raw-peer")','set_filename("peer-unmarked.elf")','set_targetdir("../artifacts")',f'add_defines({q(define)})','add_files("private_futex_peer_user.cpp","private_futex_peer_user.S")']
 common=[*flags,'-ffreestanding','-fno-pic','-fno-pie']
 cpp=[*common,'-O2','-frtti','-fno-exceptions','-fno-stack-protector','-fno-use-cxa-atexit','-fno-threadsafe-statics','-Wall','-Wextra','-Werror']
 ld=[*flags,'-nostdlib','-static','-no-pie','-Wl,--build-id=none','-Wl,-z,max-page-size=0x1000','-Wl,-T,'+str(P/'private_futex_peer_user.ld'),'-Wl,--defsym,USER_BASE='+str(root)]
 if a.arch=='mips64':ld.append('-Wl,--oformat=elf64-bigmips')
 for kind,values in [('cxxflags',cpp),('asflags',common+['-D'+define]),('ldflags',ld)]:lua.append('add_'+kind+'('+','.join(map(q,values))+',{force=true})')
 lua.append('target_end()');(P/'xmake.lua').write_text('\n'.join(lua)+'\n')
 result={'status':'building','arch':a.arch,'executed':False,'source_sha256':{n:sha(P/n) for n in names},'compiler_sha256':sha(cxx),'commands':[]};env={**os.environ,'XMAKE_ROOT':'y','XMAKE_GLOBALDIR':str(R/'xmake-global')}
 def save():(R/'result.json').write_text(json.dumps(result,indent=2)+'\n')
 save()
 try:
  for label,cmd in [('configure',['xmake','f','-P',str(P),'-c','-y','-m','release','--ccache=n']),('build',['xmake','build','-P',str(P),'-j2','-v'])]:
   with (R/(label+'.log')).open('x') as f:r=subprocess.run(cmd,cwd=P,env=env,stdout=f,stderr=subprocess.STDOUT)
   result['commands'].append({'argv':cmd,'returncode':r.returncode,'log_sha256':sha(R/(label+'.log'))});save();assert r.returncode==0,label
  raw=R/'artifacts/peer-unmarked.elf';b=bytearray(raw.read_bytes());assert b[:4]==b'\x7fELF';b[7]=3;b[8]=1 if a.arch in ('mips','mips64') else 0
  final=R/'peer.elf';final.write_bytes(b);result.update(status='built-not-executed',unmarked_sha256=sha(raw),elf_sha256=sha(final));save()
 except BaseException as e:result.update(status='failed',error=repr(e));save();raise
 print(json.dumps(result,indent=2))
if __name__=='__main__':main()
