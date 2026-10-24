#!/usr/bin/env python3
"""Run independent ordinary-mode actual Kernel admin lifecycle with hosted hardware mocks."""
from pathlib import Path
import argparse, hashlib, json, os, resource, shutil, subprocess

DIRECTORIES = ('core','apps','driver','fs','gui','interrupt','ipc','memory','net','posix','sched','smp','syscall','user')
def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source',type=Path,required=True);ap.add_argument('--inventory',type=Path,required=True)
    ap.add_argument('--inventory-sha256',required=True);ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--sanitize',action='store_true');ap.add_argument('--cxx',default='g++')
    args=ap.parse_args();resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    root=args.source.resolve();inventory=args.inventory.resolve();out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    source=out/'source';stage=Path(__file__).resolve().parent
    record=dict(status='preparing',scope='Hosted x86_64 actual Kernel::kill_process/runtime with mapping, copy and trap mocks. No native, isolation, SMP or physical evidence.',source=str(root),inventory=str(inventory),inventory_sha256=args.inventory_sha256,sanitize=args.sanitize,source_sha256={},commands=[],rows=[])
    def save():
        tmp=out/'result.json.tmp';tmp.write_text(json.dumps(record,indent=2)+'\n');tmp.replace(out/'result.json')
    def run(argv,label):
        env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1',XMAKE_ROOT='y',ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        for key in ('CPATH','CPLUS_INCLUDE_PATH','C_INCLUDE_PATH','LIBRARY_PATH','GCC_EXEC_PREFIX','COMPILER_PATH'):env.pop(key,None)
        argv=list(map(str,argv));p=subprocess.run(argv,cwd=source,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        log=out/(label+'.log');log.write_bytes(p.stdout);record['commands'].append(dict(argv=argv,returncode=p.returncode,log=log.name,log_sha256=sha(log)));save()
        if p.returncode:raise RuntimeError(label+' failed; retained '+str(log))
        return p.stdout.decode()
    save()
    try:
        if sha(inventory)!=args.inventory_sha256:raise ValueError('inventory digest mismatch')
        declared=json.loads(inventory.read_text())['files']
        actual={str(p.relative_to(root)):sha(p) for p in sorted(root.rglob('*')) if p.is_file()}
        if actual!=declared:raise ValueError('complete source inventory mismatch')
        sources=[]
        for directory in DIRECTORIES:
            sources.extend(str(p.relative_to(root)) for p in sorted((root/'src'/directory).rglob('*.cpp')) if p.name not in ('entry.cpp','kernel_main.cpp','runtime.cpp'))
        sources.extend(['src/arch/arch.cpp','src/arch/x86_64/ops.cpp'])
        files={root/name for name in sources}|{root/'src/user/runtime.cpp',root/'tests/host/native_clock_model.hpp'}
        for base in ('include','src','tests'):
            files.update(p for p in (root/base).rglob('*') if p.is_file() and p.suffix in ('.hpp','.h','.inc'))
        for path in sorted(files):
            name=str(path.relative_to(root));target=source/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,target)
            if sha(target)!=declared[name]:raise ValueError('snapshot mismatch '+name)
            record['source_sha256'][name]=declared[name]
        test=stage/'admin_runtime_review_tests.cpp';name='tests/host/admin_runtime_review_tests.cpp'
        shutil.copy2(test,source/name);record['source_sha256'][name]=sha(test)
        record['review_test_sha256']=sha(test);record['runner_sha256']=sha(__file__)
        cxx=Path(shutil.which(args.cxx) or args.cxx).resolve();xmake=Path(shutil.which('xmake') or '').resolve()
        if not cxx.is_file() or not xmake.is_file():raise ValueError('xmake and compiler required')
        record['tools']={str(p):sha(p) for p in (cxx,xmake)}
        flags=['-frtti','-Wall','-Wextra','-Wpedantic','-Werror','-ffunction-sections','-fdata-sections'];ld=['-Wl,--gc-sections']
        if args.sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie'];ld+=['-fsanitize=address,undefined','-no-pie']
        lua=['set_project("independent-admin-review")','set_languages("c++23")','set_optimize("fast")','toolchain("review-host")','    set_kind("standalone")','    set_toolset("cxx", '+json.dumps(str(cxx))+')','    set_toolset("ld", '+json.dumps(str(cxx))+')','toolchain_end()']
        for mode in ('release','debug'):
            definitions=['OK_ARCH_TARGET_X86_64','OK_NATIVE_SIGNAL_RUNTIME']
            if mode=='debug':definitions.append('OK_ENABLE_TEST_POINTS')
            lua += ['target("admin-review-'+mode+'")','    set_kind("binary")','    set_toolchains("review-host")','    add_includedirs("include")','    set_targetdir("bin")','    add_defines('+', '.join(json.dumps(s) for s in definitions)+')','    add_cxxflags('+', '.join(json.dumps(s) for s in flags)+', {force=true})','    add_ldflags('+', '.join(json.dumps(s) for s in ld)+', {force=true})','    add_files('+', '.join(json.dumps(s) for s in [name,*sources])+')','target_end()']
        (source/'xmake.lua').write_text('\n'.join(lua)+'\n');record['generated_xmake_sha256']=sha(source/'xmake.lua');record['status']='running';save()
        run([xmake,'f','-P',source,'-c','-y','--ccache=n'],'configure')
        for mode in ('release','debug'):
            run([xmake,'build','-P',source,'-j2','-v','admin-review-'+mode],mode+'-build')
            binary=source/'bin'/('admin-review-'+mode);output=run([binary],mode+'-run')
            if 'ADMIN_REVIEW_PASS checks=' not in output:raise ValueError('missing ordinary admin pass marker')
            record['rows'].append(dict(mode=mode,status='passed',binary_sha256=sha(binary),output=output.strip()));save();print('PASS',mode,'hosted actual ordinary Kernel admin review',flush=True)
        for name,digest in record['source_sha256'].items():
            if sha(source/name)!=digest:raise ValueError('retained snapshot changed '+name)
        if {str(p.relative_to(root)):sha(p) for p in sorted(root.rglob('*')) if p.is_file()}!=declared:raise ValueError('upstream source changed during audit')
        record['status']='passed'
    except BaseException as exc:record.update(status='failed',error=repr(exc));save();raise
    save()
if __name__=='__main__':main()
