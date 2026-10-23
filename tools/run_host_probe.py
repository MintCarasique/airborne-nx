"""Build and run a bounded diagnostic guest-memory/startup probe, never a game boot."""
from pathlib import Path
import hashlib
import json
import os
import re
from collections import Counter
import subprocess
from generated_state import generated_fingerprint

root=Path(__file__).resolve().parents[1]
bin_dir=root/'tools/toolchain/llvm-mingw-20261020-ucrt-x86_64/bin'
env=dict(os.environ,PATH=str(bin_dir)+os.pathsep+os.environ['PATH'])
subprocess.run(['python',str(root/'tools/prepare_host_runtime.py')],cwd=root,check=True)
manifest=json.loads((root/'analysis/runtime_manifest.json').read_text())
build=json.loads((root/'analysis/object_build_summary.json').read_text())
fingerprint=generated_fingerprint(root/'generated/ppc')
archive=root/build['archive']['path']
if build['failed'] or build['archive_exit_code'] or fingerprint!=build['generated_fingerprint']:
    raise RuntimeError('Rebuild CPU archive first')
if hashlib.sha256(archive.read_bytes()).hexdigest()!=build['archive']['sha256']:
    raise RuntimeError('CPU archive checksum mismatch')
for path,digest in build.get('runtime_sources_sha256',{}).items():
    if hashlib.sha256((root/path).read_bytes()).hexdigest()!=digest:raise RuntimeError('Changed CPU runtime dependency: '+path)
out=root/'generated/host';exe=out/'startup_probe.exe'
sources=[root/'runtime/host/diagnostic.cpp',root/'runtime/host/roundeven.cpp',root/'runtime/host/self_test.cpp',out/'runtime_imports.cpp',root/'runtime/gpu/vulkan_device.cpp']
args=[str(bin_dir/'clang++.exe'),'-std=c++17','-O1','-fno-strict-aliasing','-ffp-contract=off',
    '-Wno-macro-redefined','-DSIMDE_ENABLE_NATIVE_ALIASES',
    '-I'+str(root/'generated/ppc'),'-I'+str(out),'-I'+str(root/'runtime/host'),'-I'+str(root/'tools/mesa-switch/include'),
    '-I'+str(root/'tools/XenonRecomp/thirdparty/simde'),*[str(s) for s in sources],str(archive),'-o',str(exe)]
p=subprocess.run(args,cwd=root,env=env,capture_output=True,text=True,timeout=120)
(root/'analysis/host_probe_build.log').write_text(p.stdout+p.stderr,encoding='utf-8')
if p.returncode:raise RuntimeError(p.stderr[-7000:])
results=[]
import_calls=[]
for mode in ('--validate','--entry'):
    p=subprocess.run([str(exe),str(root/'analysis/image/image_loader_normalized.bin'),
        str(root/'extracted/base/default.xex'),mode],env=env,cwd=root,capture_output=True,text=True,timeout=110)
    log=root/'analysis'/('host_probe_'+mode[2:]+'.log')
    log.write_text(p.stdout+p.stderr,encoding='utf-8')
    if mode=='--entry':import_calls=re.findall(r'^IMPORT (\w+)',p.stdout,re.M)
    lines=p.stdout.strip().splitlines()
    excerpt=lines if len(lines)<=20 else lines[:2]+['... full trace in '+log.name+' ...']+lines[-16:]
    results.append(dict(mode=mode,exit_code=p.returncode,stdout='\n'.join(excerpt),stderr=p.stderr.strip(),log=str(log.relative_to(root)),log_sha256=hashlib.sha256(log.read_bytes()).hexdigest()))
negative=[]
bad=out/'invalid_image.bin';bad.write_bytes(b'MZ\0\0')
for image_path,xex_path in (
    (bad,root/'extracted/base/default.xex'),
    (root/'analysis/image/image_loader_normalized.bin',bad),
):
    p=subprocess.run([str(exe),str(image_path),str(xex_path),'--validate'],env=env,cwd=root,capture_output=True,text=True,timeout=30)
    negative.append(dict(exit_code=p.returncode,stderr=p.stderr.strip()))
dependencies=[*sources,root/'runtime/common/startup_services.inc',*sorted((root/'runtime/host').glob('*.h')),*sorted((root/'runtime/gpu').glob('*.h')),*sorted((root/'runtime/gpu').glob('*.inc')),*sorted((root/'runtime/audio').glob('*.h')),*sorted((root/'runtime/io').glob('*.h')),*sorted((root/'runtime/net').glob('*.h')),out/'runtime_bindings.h',out/'startup_shader_profile.h',out/'post_config_graphics.h',out/'video_graphics.h']
report=dict(scope='Windows x64 diagnostic loader and bounded startup; no game boot or Switch execution',
    generated_fingerprint=fingerprint,image_sha256=manifest['image_sha256'],
    runtime_source_sha256={str(s.relative_to(root)):hashlib.sha256(s.read_bytes()).hexdigest() for s in dependencies},
    imports_seen=list(dict.fromkeys(import_calls)),import_call_counts=dict(Counter(import_calls)),
    trace_count_policy='Hot TLS accesses use aggregate IMPORT_TOTAL counters; per-call trace counts are not total invocations',
    implemented_function_imports=sorted(set(re.findall(r'std::strcmp\(name,"(\w+)"\)',(root/'runtime/common/startup_services.inc').read_text()))),
    variable_configuration='Module, command line, graphics device slot and virtual Xenos clock bound; debug/cert monitors disabled; other variables guarded',
    negative_cases=negative,results=results)
(root/'analysis/host_probe_summary.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
for r in results:
    print(f'{r["mode"]}: exit={r["exit_code"]}; {r["stderr"][-1600:]}')
if results[0]['exit_code'] or results[1]['exit_code']!=10 or any(r['exit_code']!=1 for r in negative):
    raise RuntimeError('Unexpected probe result; inspect host_probe_summary.json')
