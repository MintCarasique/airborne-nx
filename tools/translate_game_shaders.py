"""Translate extracted game shaders to HLSL and Vulkan SPIR-V on the PC.

Each shader runs in an isolated process: retain the first error and continue
independent shaders. Compilation does not establish Xenos rendering equivalence.
"""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import argparse
import hashlib
import json
import os
import struct
import subprocess
from extract_game_shaders import inspect_container, ROOT


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def spirv_info(path):
    data = path.read_bytes()
    if len(data) < 20 or len(data) % 4:
        raise ValueError('invalid SPIR-V byte length')
    words = struct.unpack('<' + str(len(data)//4) + 'I', data)
    if words[0] != 0x07230203 or not words[3] or words[4]:
        raise ValueError('invalid SPIR-V header')
    at, capabilities, entries = 5, [], []
    while at < len(words):
        length, opcode = words[at] >> 16, words[at] & 0xffff
        if not length or length > len(words) - at:
            raise ValueError('invalid SPIR-V instruction range')
        if opcode == 17:
            capabilities.append(words[at+1])
        if opcode == 15:
            entries.append(words[at+1])
        at += length
    return dict(bytes=len(data), sha256=digest(path), version=hex(words[1]),
                capabilities=sorted(set(capabilities)), execution_models=entries)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    llvm = ROOT / 'tools/toolchain/llvm-mingw-20261020-ucrt-x86_64/bin'
    env = dict(os.environ, PATH=str(llvm)+os.pathsep+os.environ['PATH'])
    cmake = ROOT / 'tools/toolchain/venv/Scripts/cmake.exe'
    out = ROOT / 'generated/shaders'
    build = ROOT / 'generated/airborne-shader-translator'
    log_dir = out / 'logs'
    log_dir.mkdir(parents=True, exist_ok=True)
    for command in ([str(cmake), '-S', str(ROOT/'tools/shaders'), '-B', str(build), '-G', 'Ninja',
                     '-DCMAKE_CXX_COMPILER='+str(llvm/'clang++.exe'), '-DCMAKE_BUILD_TYPE=Debug',
                     '-DCMAKE_MAKE_PROGRAM='+str(ROOT/'tools/toolchain/venv/Scripts/ninja.exe')],
                    [str(cmake), '--build', str(build), '-j', str(args.jobs)]):
        process = subprocess.run(command, env=env, capture_output=True, text=True)
        with (log_dir/'translator-build.log').open('a', encoding='utf-8') as log:
            log.write(process.stdout+process.stderr)
        if process.returncode:
            raise RuntimeError(process.stderr[-3000:]+process.stdout[-3000:])
    translator = build / 'airborne-shader-translate.exe'
    xenos = ROOT / 'tools/XenosRecomp'
    pin=json.loads((ROOT/'config/graphics_dependencies.json').read_text())['xenos_recomp']
    revision=subprocess.check_output(['git','-C',str(xenos),'rev-parse','HEAD'],text=True).strip()
    patch=subprocess.check_output(['git','-C',str(xenos),'diff','--binary'])
    if revision!=pin['commit'] or hashlib.sha256(patch).hexdigest()!=pin['patch_sha256']:
        raise RuntimeError('XenosRecomp revision/Airborne patch mismatch')
    header = xenos / 'XenosRecomp/shader_common.h'
    dxc = xenos / 'thirdparty/dxc-bin/bin/x64/dxc.exe'
    inventory = ROOT / 'analysis/game_shader_inventory.json'
    records = json.loads(inventory.read_text())['shaders']
    for folder in ('hlsl', 'spirv'):
        (out/folder).mkdir(exist_ok=True)

    def translate(record):
        source = ROOT / record['path']
        key = record['sha256']
        result = dict(sha256=key, code_sha256=record['code_sha256'], stage=record['stage'], status='FAILED')
        hlsl, spirv = out/'hlsl'/(key+'.hlsl'), out/'spirv'/(key+'.spv')
        # Prevent stale output from being mistaken for this invocation's success.
        hlsl.unlink(missing_ok=True)
        spirv.unlink(missing_ok=True)
        messages = []
        try:
            if digest(source) != key:
                raise ValueError('shader input hash mismatch')
            inspect_container(source.read_bytes())
            profile = 'vs_6_0' if record['stage'] == 'vertex' else 'ps_6_0'
            commands = [[str(translator), str(source), str(hlsl), str(header)],
                        [str(dxc), '-spirv', '-T', profile, '-E', 'main', '-HV', '2021',
                         '-fvk-use-dx-layout', '-fspv-target-env=vulkan1.2', '-all-resources-bound',
                         *(['-fvk-invert-y'] if record['stage']=='vertex' else []),
                         str(hlsl), '-Fo', str(spirv)]]
            for index, command in enumerate(commands):
                process = subprocess.run(command, env=env, capture_output=True, text=True, timeout=30)
                messages.append(process.stdout+process.stderr)
                if process.returncode:
                    result.update(failed_step='xenos_to_hlsl' if index==0 else 'hlsl_to_spirv', exit_code=process.returncode)
                    raise ValueError(messages[-1][-3000:] or 'translator exited without diagnostic')
                if index == 0:
                    result.update(json.loads(process.stdout))
                    result['hlsl_sha256'] = digest(hlsl)
            result.update(status='COMPILED', spirv=spirv_info(spirv),
                          path=str(spirv.relative_to(ROOT)))
        except (ValueError, OSError, subprocess.TimeoutExpired) as error:
            result['error'] = str(error)
        (log_dir/(key+'.txt')).write_text('\n'.join(messages), encoding='utf-8')
        return result

    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        results = list(pool.map(translate, records))
    failures = [r for r in results if r['status'] != 'COMPILED']
    report = dict(scope='Real game shader compilation only; no rendering/semantic-equivalence claim',
                  inventory_sha256=digest(inventory), translator_sha256=digest(translator),
                  xenos_commit=revision, xenos_patch_sha256=hashlib.sha256(patch).hexdigest(),
                  xenos_source_sha256={str(p.relative_to(xenos)):digest(p) for p in sorted((xenos/'XenosRecomp').glob('*')) if p.is_file()},
                  shared_header_sha256=digest(header), dxc_sha256=digest(dxc),
                  tool_sha256=digest(Path(__file__)),
                  compiled=len(results)-len(failures), failed=len(failures),
                  first_error=failures[0] if failures else None, results=results)
    (ROOT/'analysis/game_shader_translation.json').write_text(json.dumps(report,indent=2), encoding='utf-8')
    print(f'{report["compiled"]}/{len(results)} actual game shaders compiled to SPIR-V; {len(failures)} failures')
    if failures:
        print('First failure:', failures[0].get('error','unknown')[-1200:])
        raise SystemExit(1)


if __name__ == '__main__':
    main()
