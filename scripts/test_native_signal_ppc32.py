#!/usr/bin/env python3
"""Retain and run PPC32 codec, policy, admission, persistent readiness and ELF models with xmake/ASan/UBSan.

These are hosted models. Optional ELF arguments are metadata inputs, never code
execution. The preserved models overlap prior isolated author-stage evidence.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess

MODELS = {
    'codec': 'tests/host/native_signal_frame_ppc32_tests.cpp',
    'delivery': 'tests/host/native_signal_delivery_ppc32_tests.cpp',
    'readiness': 'tests/host/native_signal_readiness_ppc32_tests.cpp',
    'persistent': 'tests/host/native_signal_persistent_ppc32_tests.cpp',
    'elf': 'tests/host/native_ppc32_elf_admission_tests.cpp',
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--linux-elf', type=Path, required=True)
    parser.add_argument('--native-elf', type=Path, required=True)
    parser.add_argument('--elf-corpus', type=Path, required=True)
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--model', choices=MODELS, action='append', default=[])
    args = parser.parse_args()
    selected_models = args.model or list(MODELS)
    root = Path(__file__).resolve().parents[1]
    out = args.output.resolve()
    if out.is_relative_to(root):
        raise ValueError('model evidence must be outside the source tree')
    out.mkdir(parents=True, exist_ok=False)
    source = out / 'source'
    record = dict(status='preparing', scope=__doc__, source_sha256={}, tools={},
                  elf_inputs=[], commands=[], models=[], guest_execution=False,
                  smp=False, physical=False, selected_models=selected_models)

    def save():
        temporary = out / 'result.json.tmp'
        temporary.write_text(json.dumps(record, indent=2) + '\n')
        temporary.replace(out / 'result.json')

    def copy(original, destination):
        if original.is_symlink() or not original.is_file():
            raise ValueError('explicit regular input required: ' + str(original))
        digest = sha(original)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(original, destination)
        if sha(original) != digest or sha(destination) != digest:
            raise ValueError('source changed while copied: ' + str(original))
        return digest

    save()
    try:
        paths = [root / name for name in MODELS.values()]
        paths += [root / 'tests/fixtures/signals/linux_ppc32_layout.hpp', root / 'tests/fixtures/signals/ppc32_trampoline_baseline.hpp']
        paths += [Path(__file__).resolve()]
        paths += sorted(p for p in (root / 'include').rglob('*') if p.is_file())
        for path in paths:
            relative = str(path.relative_to(root))
            record['source_sha256'][relative] = copy(path, source / relative)
        selected_elf = []
        for label, path in (('linux', args.linux_elf), ('native', args.native_elf)):
            original = path.resolve()
            retained = out / 'elf-inputs' / (label + '.elf')
            digest = copy(original, retained)
            selected_elf.append(retained)
            record['elf_inputs'].append(dict(source=str(original), retained=str(retained), sha256=digest))
        corpus = out / 'elf-inputs/corpus'
        for label in ('native-init', 'static-musl-futex', 'static-musl-masks', 'musl-build-probe'):
            original = (args.elf_corpus / (label + '.elf')).resolve()
            retained = corpus / (label + '.elf')
            digest = copy(original, retained)
            record['elf_inputs'].append(dict(source=str(original), retained=str(retained), sha256=digest))
        programs = {}
        for name, command in (('xmake', 'xmake'), ('cxx', args.cxx)):
            resolved = shutil.which(command)
            if resolved is None:
                raise ValueError('required tool not found: ' + command)
            path = Path(resolved).resolve()
            programs[name] = path
            record['tools'][str(path)] = sha(path)
        cxx = json.dumps(str(programs['cxx']))
        lines = ['set_project("ppc32-signal-models")', 'set_languages("c++23")',
                 'toolchain("model-host")', 'set_kind("standalone")',
                 f'set_toolset("cxx", {cxx})', f'set_toolset("ld", {cxx})', 'toolchain_end()']
        for name, model in MODELS.items():
            lines += [f'target("{name}")', 'set_kind("binary")', 'set_toolchains("model-host")',
                      'set_targetdir("artifacts")', f'add_files("{model}")', 'add_includedirs("include")',
                      'add_cxxflags("-O1", "-g", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", {force=true})',
                      'add_ldflags("-fsanitize=address,undefined", "-no-pie", {force=true})', 'target_end()']
            if name in ('readiness', 'persistent'):
                lines.insert(len(lines) - 1, 'add_defines("OK_PPC32_ADMISSION_MODEL=1")')
        (source / 'xmake.lua').write_text('\n'.join(lines) + '\n')
        record['generated_project_sha256'] = sha(source / 'xmake.lua')
        env = {**os.environ, 'XMAKE_ROOT': 'y', 'XMAKE_GLOBALDIR': str(out / 'xmake-global'),
               'ASAN_OPTIONS': 'detect_leaks=0:halt_on_error=1', 'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
        for name in ('CPATH', 'CPLUS_INCLUDE_PATH', 'C_INCLUDE_PATH', 'LIBRARY_PATH', 'GCC_EXEC_PREFIX', 'COMPILER_PATH'):
            env.pop(name, None)

        def run(argv, label):
            log = out / (label + '.log')
            with log.open('x') as stream:
                completed = subprocess.run(list(map(str, argv)), cwd=source, env=env,
                                           stdout=stream, stderr=subprocess.STDOUT)
            record['commands'].append(dict(argv=list(map(str, argv)), returncode=completed.returncode,
                                           log=log.name, log_sha256=sha(log)))
            save()
            if completed.returncode:
                raise RuntimeError(label + ' failed; see retained log')
            return log.read_text()

        record['status'] = 'running'
        run([programs['xmake'], 'f', '-P', source, '-c', '-y', '--ccache=n'], 'configure')
        for name in selected_models:
            run([programs['xmake'], 'build', '-P', source, '-j2', '-v', name], 'build-' + name)
        for name in selected_models:
            binary = source / 'artifacts' / name
            arguments = selected_elf if name == 'readiness' else selected_elf[:1] if name == 'persistent' else [corpus] if name == 'elf' else []
            output = run([binary, *arguments], name)
            if 'PASS' not in output.splitlines()[-1]:
                raise ValueError('missing model completion: ' + name)
            record['models'].append(dict(name=name, binary_sha256=sha(binary), output=output))
        for relative, digest in record['source_sha256'].items():
            if sha(source / relative) != digest:
                raise ValueError('retained source changed: ' + relative)
        record['status'] = 'passed'
    except BaseException as error:
        record.update(status='failed', error=repr(error))
        save()
        raise
    save()
    print('PASS PPC32 hosted models; target_execution=0 metadata_is_not_loading=1')


if __name__ == '__main__':
    main()
