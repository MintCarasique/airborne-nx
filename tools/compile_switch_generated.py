"""Compile the complete generated game CPU code with the actual Switch GCC.

This produces a private AArch64 archive, not a runnable game/NRO. Import/runtime
services and graphics must be linked separately. All cache inputs are hashed.
"""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, as_completed
import argparse
import hashlib
import json
import subprocess
from generated_state import generated_fingerprint

root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--jobs',type=int,default=3)
parser.add_argument('--limit',type=int,help='Diagnostic subset only; never writes the full-build summary/archive')
args=parser.parse_args()
if args.jobs<1 or args.limit is not None and args.limit<1:parser.error('positive counts required')
sdk=root/'tools/toolchain/nx-sdk/opt/devkitpro'
def linux(path):
    return subprocess.check_output(['wsl','--exec','wslpath','-a',str(path)],text=True).strip()
root_linux=linux(root)
sdk_linux=root_linux+'/tools/toolchain/nx-sdk/opt/devkitpro'
compiler=sdk_linux+'/devkitA64/bin/aarch64-none-elf-g++'
prefix=['wsl','--exec','env','DEVKITPRO='+sdk_linux]
out=root/'generated/switch-cpu';objects=out/'objects';objects.mkdir(parents=True,exist_ok=True)
logs=root/'analysis/switch_object_build';logs.mkdir(exist_ok=True)
def posix(path):return root_linux+'/'+path.relative_to(root).as_posix()
flags=['-std=c++17','-O1','-march=armv8-a+crc+crypto','-mtune=cortex-a57','-mtp=soft','-fPIE',
    '-ffunction-sections','-fdata-sections','-fno-strict-aliasing','-ffp-contract=off','-fno-fast-math',
    '-fno-rtti','-fno-exceptions','-D__SWITCH__','-DSIMDE_ENABLE_NATIVE_ALIASES','-Wno-psabi',
    '-I'+posix(root/'tools/XenonRecomp/thirdparty/simde'),'-I'+sdk_linux+'/libnx/include']
compat=root/'runtime/switch/compiler_compat.h'
mmio=root/'runtime/gpu/guest_mmio.h'
flags+=['-include',posix(compat),'-D__builtin_rotateleft32=nx_ppc_rotl32','-D__builtin_rotateleft64=nx_ppc_rotl64']
flags+=['-include',posix(mmio)]
version=subprocess.check_output([*prefix,compiler,'--version'],text=True).splitlines()[0]
context=hashlib.sha256()
for path in sorted((root/'generated/ppc').glob('*.h')):context.update(path.read_bytes())
context.update(compat.read_bytes())
context.update(mmio.read_bytes())
context.update(json.dumps(flags).encode());context.update(version.encode())
context_key=context.hexdigest()
sources=sorted((root/'generated/ppc').glob('*.cpp'))
if args.limit:sources=sources[:args.limit]
def compile_one(path):
    target=objects/(path.stem+'.o');state=target.with_suffix('.json')
    key=hashlib.sha256(path.read_bytes()).hexdigest()+context_key
    if target.exists() and state.exists():
        saved=json.loads(state.read_text())
        if saved.get('key')==key and saved.get('object_sha256')==hashlib.sha256(target.read_bytes()).hexdigest():
            return dict(file=path.name,exit_code=0,cached=True,object=str(target.relative_to(root)),sha256=saved['object_sha256'])
    process=subprocess.run([*prefix,compiler,*flags,'-c',posix(path),'-o',posix(target)],capture_output=True,text=True)
    log=logs/(path.name+'.log');log.write_text(process.stdout+process.stderr,encoding='utf-8')
    result=dict(file=path.name,exit_code=process.returncode,cached=False,object=str(target.relative_to(root)),log=str(log.relative_to(root)))
    if not process.returncode:
        result['sha256']=hashlib.sha256(target.read_bytes()).hexdigest()
        state.write_text(json.dumps(dict(key=key,object_sha256=result['sha256'])))
    return result
results=[]
with ThreadPoolExecutor(max_workers=args.jobs) as pool:
    for index,future in enumerate(as_completed(pool.submit(compile_one,p) for p in sources),1):
        result=future.result();results.append(result)
        if result['exit_code'] or index%10==0 or index==len(sources):
            print(f'{index}/{len(sources)}: {result["file"]} exit={result["exit_code"]} cached={result["cached"]}',flush=True)
report=dict(scope='Switch AArch64 game CPU objects only; not linked to runtime, not executed; no game boot',
    compiler=version,flags=flags,generated_fingerprint=generated_fingerprint(root/'generated/ppc'),
    subset=bool(args.limit),total=len(results),failed=sum(r['exit_code']!=0 for r in results),results=sorted(results,key=lambda r:r['file']))
report['runtime_sources_sha256']={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in (compat,mmio)}
if not args.limit and not report['failed']:
    response=out/'objects.rsp';response.write_text('\n'.join('"'+posix(root/r['object'])+'"' for r in report['results']))
    archive=out/'libAirbornePPC.a'
    if archive.exists():archive.unlink()  # Derived file within fixed output directory.
    process=subprocess.run([*prefix,sdk_linux+'/devkitA64/bin/aarch64-none-elf-ar','rcs',posix(archive),'@'+posix(response)],capture_output=True,text=True)
    report['archive_exit_code']=process.returncode
    if process.returncode:report['archive_error']=process.stderr
    else:report['archive']=dict(path=str(archive.relative_to(root)),bytes=archive.stat().st_size,sha256=hashlib.sha256(archive.read_bytes()).hexdigest())
summary='switch_object_subset_summary.json' if args.limit else 'switch_object_build_summary.json'
(root/'analysis'/summary).write_text(json.dumps(report,indent=2),encoding='utf-8')
print(f'Complete: {report["total"]} units, {report["failed"]} failures, subset={bool(args.limit)}',flush=True)
raise SystemExit(bool(report['failed'] or not args.limit and report.get('archive_exit_code',1)))
