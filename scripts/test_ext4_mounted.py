#!/usr/bin/env python3
"""Check the bounded mounted metadata provider using private hosted images."""
from pathlib import Path
import argparse, hashlib, json, os, shutil, subprocess, struct
from test_ext4_owned import PROFILES, TARGET_FLAGS
from hosted_closure import include_closure

SOURCES = ['src/fs/ext4_recovery.cpp', 'src/memory/memory.cpp', 'src/net/native_udp.cpp',
           'src/sched/scheduler.cpp', 'src/sched/process.cpp',
           'src/posix/posix.cpp', 'src/posix/fd.cpp', 'src/posix/path.cpp', 'src/posix/process.cpp',
           'src/fs/vfs.cpp', 'src/driver/driver.cpp', 'src/smp/smp.cpp', 'src/arch/arch.cpp', 'src/arch/terminal_power.cpp',
           'src/arch/x86_64/ops.cpp', 'src/syscall/linux.cpp', 'src/syscall/syscall.cpp',
           'tests/host/ext4_realtime_model.cpp']

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--clean-image', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--cross', action='store_true')
    parser.add_argument('--toolchain-root', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=False)
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    report = {'scope': 'hosted-mounted-metadata-only', 'status': 'running', 'commands': [],
              'sanitize': args.sanitize, 'asan_options': os.environ.get('ASAN_OPTIONS', ''),
              'native_execution': False, 'posix_mutation': False, 'physical_durability': False}
    def save(): (output/'result.json').write_text(json.dumps(report, indent=2)+'\n')
    def run(command, label, allowed=(0,)):
        record = {'argv': [str(x) for x in command], 'label': label}; report['commands'].append(record); save()
        result = subprocess.run(record['argv'], capture_output=True, text=True)
        (output/(label+'.log')).write_text(result.stdout+result.stderr)
        record['returncode'] = result.returncode; save()
        if result.returncode not in allowed: raise RuntimeError(label+' failed')
        return result.stdout+result.stderr
    try:
        snapshot = output/'sources'; shutil.copytree(root/'include', snapshot/'include')
        files = SOURCES + ['tests/host/'+name for name in ['ext4_mounted_tests.cpp', 'ext4_mounted_invariant_tests.cpp',
                 'ext4_mounted_persistent_invariants.cpp',
                 'ext4_mounted_support.hpp', 'ext4_owned_support.hpp', 'ext4_transaction_support.hpp']]
        files += ['scripts/test_ext4_mounted.py', 'scripts/test_ext4_owned.py', 'src/arch/ops_private.hpp',
                  'tests/host/ext4_mounted_compile.cpp', 'tests/host/ext4_realtime_model.hpp']
        for name in include_closure(root, files):
            target=snapshot/name;target.parent.mkdir(parents=True, exist_ok=True);shutil.copyfile(root/name,target)
        report['source_sha256']={str(f.relative_to(snapshot)):digest(f) for f in sorted(snapshot.rglob('*')) if f.is_file()}
        clean=output/'clean-input.img';shutil.copyfile(args.clean_image,clean);report['input_sha256']=digest(clean)
        image=output/'multi.img';shutil.copyfile(clean,image)
        payload=output/'payload';payload.write_bytes(b'mounted fixture data');payload.chmod(0o644)
        for index in range(4):run(['debugfs','-w','-R',f'write {payload} /other{index}',image],f'add-inode-{index}')
        run(['e2fsck','-fn',image],'input-fsck');report['multi_sha256']=digest(image)
        namespace=output/'namespace.img';shutil.copyfile(image,namespace)
        run(['debugfs','-w','-R','mkdir /child',namespace],'add-directory')
        run(['debugfs','-w','-R','symlink /seed-link seed',namespace],'add-symlink')
        run(['e2fsck','-fn',namespace],'namespace-fsck');report['namespace_sha256']=digest(namespace)
        flags=['g++','-std=c++23','-O1','-g','-Wall','-Wextra','-Wpedantic','-Werror','-ffunction-sections',
               '-fdata-sections','-Wl,--gc-sections','-DOK_ARCH_TARGET_X86_64','-I'+str(snapshot/'include')]
        if args.sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        for name,private in [('ext4_mounted_tests',False),('ext4_mounted_invariant_tests',True),
                             ('ext4_mounted_persistent_invariants',True)]:
            binary=output/name
            run([*flags,*(['-fno-access-control'] if private else []),snapshot/('tests/host/'+name+'.cpp'),
                 *(snapshot/f for f in SOURCES),'-Wl,-l:libext2fs.so.2','-pthread','-o',binary],'compile-'+name)
            text=run([binary,image,*([] if private else [output/'model',namespace])],name);print(text, end='', flush=True)
        run(['e2fsck','-fn',output/'model/completed.img'],'completed-fsck')
        recovery=output/'recovered.img';shutil.copyfile(output/'model/published.img',recovery)
        run(['e2fsck','-fy','-E','journal_only',recovery],'recover-publication', (0,1));run(['e2fsck','-fn',recovery],'recovered-fsck')
        expected=(output/'model/completed.img').read_bytes();actual=recovery.read_bytes()
        allowed=set(range(1072,1078))|set(range(1088,1092))|set(range(1400,1408))|set(range(2044,2048))
        differences=[i for i in range(len(expected)) if expected[i]!=actual[i]]
        if any(i not in allowed for i in differences):raise ValueError('e2fsprogs differs outside explicit bookkeeping')
        report['reference_bookkeeping_byte_differences']=differences
        report['synthetic_escape_fixture']='unused reserved inode bytes carry magic; byte-oracle only, not fsck-clean or Linux-mount evidence'
        if args.cross:
            toolchains=args.toolchain_root.resolve() if args.toolchain_root else root/'toolchains'
            for arch,triple,elf_class,endian,machine in PROFILES:
                compiler=toolchains/triple/'bin'/(triple+'-g++');obj=output/(arch+'.o')
                run([compiler,'-std=c++23','-O2','-Wall','-Wextra','-Wpedantic','-Werror','-fno-exceptions','-frtti',
                     '-ffreestanding','-fno-threadsafe-statics','-fno-use-cxa-atexit',*TARGET_FLAGS[arch],
                     '-DOK_ARCH_TARGET_'+arch.upper(),'-I'+str(snapshot/'include'),'-c',
                     snapshot/'tests/host/ext4_mounted_compile.cpp','-o',obj],'cross-'+arch)
                data=obj.read_bytes();order='<' if endian==1 else '>'
                if data[:4]!=b'\x7fELF' or data[4:6]!=bytes((elf_class,endian)) or struct.unpack_from(order+'H',data,18)[0]!=machine:
                    raise ValueError('cross-object identity mismatch: '+arch)
        if digest(clean)!=report['input_sha256'] or digest(image)!=report['multi_sha256']:raise ValueError('input changed')
        report['status']='passed'
    except Exception as error: report.update(status='failed',error=str(error))
    finally:
        report['artifact_sha256']={str(f.relative_to(output)):digest(f) for f in sorted(output.rglob('*')) if f.is_file() and f.name!='result.json'};save()
    return 0 if report['status']=='passed' else 1

if __name__=='__main__':raise SystemExit(main())
