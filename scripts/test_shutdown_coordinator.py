#!/usr/bin/env python3
"""Build and run hosted shutdown mechanism adversaries through xmake."""
from pathlib import Path
import argparse, hashlib, json, os, shutil, subprocess

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    source = out / 'source'; source.mkdir()
    report = dict(schema=1, status='preparing', scope='Hosted coordinator model only. No native retirement, storage seal, platform execution, physical or SMP evidence.', sanitizer=args.sanitize, leak_detection=False, commands=[], inputs={}, outputs={})
    def save():
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    def run(argv, label):
        env = dict(os.environ, XMAKE_ROOT='y', XMAKE_TMPDIR=str(out/'xmake-tmp'), ASAN_OPTIONS='detect_leaks=0:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        for variable in ('CPATH','CPLUS_INCLUDE_PATH','C_INCLUDE_PATH','LIBRARY_PATH','GCC_EXEC_PREFIX','COMPILER_PATH'):
            env.pop(variable, None)
        command = list(map(str, argv))
        result = subprocess.run(command, cwd=source, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        log = out/(label+'.log'); log.write_bytes(result.stdout)
        report['commands'].append(dict(argv=command, returncode=result.returncode, log=log.name, log_sha256=sha(log)))
        save()
        if result.returncode: raise RuntimeError(label+' failed; retained '+str(log))
        return result.stdout.decode()
    save()
    try:
        for name in ('include/ok/core/types.hpp','include/ok/core/power.hpp','include/ok/core/shutdown.hpp','tests/host/shutdown_coordinator_tests.cpp','scripts/test_shutdown_coordinator.py'):
            path=root/name; target=source/name; target.parent.mkdir(parents=True,exist_ok=True)
            before=sha(path); shutil.copy2(path,target)
            if sha(target)!=before or sha(path)!=before: raise ValueError('source changed: '+name)
            report['inputs'][name]=before
        compiler=Path(shutil.which('g++')).resolve(); xmake=Path(shutil.which('xmake')).resolve()
        report['tools']={str(p):sha(p) for p in (compiler,xmake)}
        flags=['-Wall','-Wextra','-Wpedantic','-Werror','-frtti']
        link=[]
        if args.sanitize:
            flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie']
            link+=['-fsanitize=address,undefined','-no-pie']
        lua=['set_project("shutdown-coordinator-model")','set_languages("c++23")','set_optimize("fast")',
             'toolchain("shutdown-host")','set_kind("standalone")','set_toolset("cxx", '+json.dumps(str(compiler))+')',
             'set_toolset("ld", '+json.dumps(str(compiler))+')','toolchain_end()',
             'target("shutdown-model")','set_kind("binary")','set_toolchains("shutdown-host")','set_targetdir("bin")',
             'add_includedirs("include")','add_files("tests/host/shutdown_coordinator_tests.cpp")',
             'add_cxxflags('+','.join(map(json.dumps,flags))+', {force=true})']
        if link: lua+=['add_ldflags('+','.join(map(json.dumps,link))+', {force=true})']
        lua+=['target_end()']
        project=source/'xmake.lua';project.write_text('\n'.join(lua)+'\n');report['project_sha256']=sha(project)
        report['status']='running';save()
        run([xmake,'f','-P',source,'-c','-y','--ccache=n'],'configure')
        run([xmake,'build','-P',source,'-j2','-v','shutdown-model'],'build')
        binary=source/'bin/shutdown-model'; output=run([binary],'model')
        if 'SHUTDOWN_COORDINATOR_MODEL_PASS checks=' not in output or 'native_retirement=0 storage_seal=0 platform_execution=0' not in output:
            raise ValueError('missing exact model marker')
        report['outputs']={'binary_sha256':sha(binary),'output':output.strip()}
        for name,digest in report['inputs'].items():
            if sha(source/name)!=digest: raise ValueError('retained source changed: '+name)
        report['status']='passed';save();print(output.strip(),flush=True)
    except BaseException as error:
        report.update(status='failed',error=repr(error));save();raise
if __name__=='__main__': main()
