#!/usr/bin/env python3
"""Xmake hosted directory-mode composition and all-ten freestanding compile evidence."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
from test_ext4_mounted import SOURCES
from test_ext4_owned import PROFILES, TARGET_FLAGS


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--clean-image', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--toolchain-root', type=Path)
    args = p.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    source = out / 'source'
    for name in ('include', 'src', 'tests/host'):
        shutil.copytree(root / name, source / name)
    for name in ('test_ext4_directory_mode.py', 'test_ext4_journalled_mode.py', 'test_ext4_mounted.py', 'test_ext4_owned.py'):
        (source / 'scripts').mkdir(exist_ok=True)
        shutil.copy2(root / 'scripts' / name, source / 'scripts' / name)
    inputs = {str(f.relative_to(source)): sha(f) for f in sorted(source.rglob('*')) if f.is_file()}
    report = dict(status='running', scope='hosted actual VFS/POSIX/provider and Linux codecs; no guest, SDK or hardware',
                  sdk_abi_source=30, sanitized=args.sanitize, source_sha256=inputs, commands=[], profiles={}, binaries={}, tools={})
    def save():
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    def pin(path):
        path = Path(path).resolve(); report['tools'][str(path)] = sha(path); return path
    def run(argv, label, cwd=source):
        log = out / (label + '.log')
        env = dict(os.environ, XMAKE_ROOT='y', XMAKE_GLOBALDIR=str(out/'xmake-global'),
                   PYTHONDONTWRITEBYTECODE='1', ASAN_OPTIONS='detect_leaks=0:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
        for key in ('CPATH','CPLUS_INCLUDE_PATH','C_INCLUDE_PATH','LIBRARY_PATH','GCC_EXEC_PREFIX','COMPILER_PATH'):
            env.pop(key, None)
        argv = list(map(str, argv))
        with log.open('xb') as stream:
            done = subprocess.run(argv, cwd=cwd, env=env, stdout=stream, stderr=subprocess.STDOUT)
        report['commands'].append(dict(argv=argv, cwd=str(cwd), returncode=done.returncode, log=log.name, log_sha256=sha(log)))
        save()
        if done.returncode: raise RuntimeError(str(log))
        return log.read_text()
    try:
        image = out / 'namespace.img'; shutil.copyfile(args.clean_image, image)
        report['original_image'] = dict(path=str(args.clean_image.resolve()), sha256=sha(args.clean_image))
        payload = out / 'payload'; payload.write_bytes(b'journalled stored metadata fixture')
        for index, request in enumerate((f'write {payload} /other0', 'symlink /seed-link seed', 'mkdir /child', f'write {payload} /child/leaf', 'symlink /child-link child', 'set_inode_field /child uid 1000', 'set_inode_field /child gid 1001', 'set_inode_field /child mode 040755')):
            run(['debugfs','-w','-R',request,image],f'fixture-{index}')
        run(['e2fsck','-fn',image],'fixture-fsck')
        report['input_sha256'] = sha(image)
        cxx = pin('/usr/bin/g++'); xmake = pin(shutil.which('xmake'))
        pin(subprocess.check_output([cxx,'-print-prog-name=cc1plus'],text=True).strip())
        project = ['set_project("directory-mode-hosted")', 'set_languages("cxx23")', 'add_rules("mode.debug", "mode.release")',
                   'target("directory-mode")', 'set_kind("binary")', 'set_targetdir("bin")', 'add_includedirs("include")',
                   'add_defines("OK_ARCH_TARGET_X86_64")',
                   'add_cxxflags("-O1","-g","-frtti","-Wall","-Wextra","-Wpedantic","-Werror","-ffunction-sections","-fdata-sections",{force=true})',
                   'add_ldflags("-Wl,--gc-sections","-Wl,-l:libext2fs.so.2","-pthread",{force=true})']
        project += ['add_files('+json.dumps(f)+')' for f in ['tests/host/ext4_directory_mode_tests.cpp', *SOURCES]]
        if args.sanitize:
            project += ['add_cxxflags("-fsanitize=address,undefined","-fno-omit-frame-pointer",{force=true})',
                        'add_ldflags("-fsanitize=address,undefined",{force=true})']
        project += ['target_end()']
        (source/'xmake.lua').write_text('\n'.join(project)+'\n')
        report['host_project_sha256'] = sha(source/'xmake.lua')
        run([xmake,'f','-P',source,'-c','-y','-m','release','--ccache=n','--cxx='+str(cxx),'--ld='+str(cxx)],'host-configure')
        run([xmake,'build','-P',source,'-j2','-v'],'host-build')
        binary = source/'bin/directory-mode'; report['binaries']['host'] = dict(path=str(binary),sha256=sha(binary))
        text = run([binary,image],'host-run')
        assert 'profiles=10 hosted=1 user_execution=0 hardware=0' in text
        if args.toolchain_root:
            for arch,triple,elf_class,endian,machine in PROFILES:
                compiler = pin(args.toolchain_root/triple/'bin'/(triple+'-g++'))
                pin(subprocess.check_output([compiler,'-print-prog-name=cc1plus'],text=True).strip())
                work = out/arch; work.mkdir()
                flags = ['-O2','-Wall','-Wextra','-Wpedantic','-Werror','-ffreestanding','-fno-exceptions','-frtti',
                         '-fno-stack-protector','-fno-use-cxa-atexit','-fno-threadsafe-statics',*TARGET_FLAGS[arch]]
                lines = ['set_project("directory-mode-target")','set_languages("cxx23")',
                         'toolchain("journalled-target")','set_kind("standalone")','set_toolset("cxx",'+json.dumps(str(compiler))+')','toolchain_end()',
                         'target("mounted-mode")','set_kind("object")','set_toolchains("journalled-target")',
                         'add_defines("OK_KERNEL_FREESTANDING","OK_ARCH_TARGET_'+arch.upper()+'")',
                         'add_includedirs('+json.dumps(str(source/'include'))+')',
                         'add_files('+json.dumps(str(source/'tests/host/ext4_mounted_compile.cpp'))+')',
                         'add_cxxflags('+','.join(map(json.dumps,flags))+',{force=true})','target_end()']
                (work/'xmake.lua').write_text('\n'.join(lines)+'\n')
                run([xmake,'f','-P',work,'-c','-y','--ccache=n'],arch+'-configure',work)
                run([xmake,'build','-P',work,'-j2','-v'],arch+'-build',work)
                objects = list((work/'build/.objs/mounted-mode').rglob('*.o'))
                assert len(objects)==1
                obj = objects[0]; data = obj.read_bytes()
                assert data[:4]==b'\x7fELF' and data[4:6]==bytes((elf_class,endian))
                assert struct.unpack_from('<H' if endian==1 else '>H',data,18)[0]==machine
                report['profiles'][arch]=dict(object=str(obj.relative_to(out)),sha256=sha(obj),project_sha256=sha(work/'xmake.lua'))
        assert sha(image)==report['input_sha256']
        for name,digest in inputs.items(): assert sha(source/name)==digest,name
        for name,digest in report['tools'].items(): assert sha(name)==digest,name
        report['status']='passed'
    except BaseException as error:
        report.update(status='failed',error=repr(error)); save(); raise
    save(); print('PASS directory-mode hosted composition and selected target compiles')


if __name__ == '__main__':
    main()
