"""Compile all generated CPU code into a host archive; no game runtime or boot."""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
import subprocess
from generated_state import generated_fingerprint

root=Path(__file__).resolve().parents[1]
bin_dir=root/'tools/toolchain/llvm-mingw-20261020-ucrt-x86_64/bin'
env=dict(os.environ,PATH=str(bin_dir)+os.pathsep+os.environ['PATH'])
source=root/'generated/ppc'
out=root/'generated/host'
objects=out/'objects'
objects.mkdir(parents=True,exist_ok=True)
logs=root/'analysis/object_build'
logs.mkdir(exist_ok=True)
flags=['-std=c++17','-O1','-fno-strict-aliasing','-ffp-contract=off','-fno-fast-math',
       '-ffunction-sections','-fdata-sections','-Wno-macro-redefined','-Wno-unused-value',
       '-DSIMDE_ENABLE_NATIVE_ALIASES','-I'+str(root/'tools/XenonRecomp/thirdparty/simde')]
compiler=bin_dir/'clang++.exe'
mmio=root/'runtime/gpu/guest_mmio.h'
flags+=['-include',str(mmio)]
compiler_version=subprocess.check_output([str(compiler),'--version'],env=env,text=True).splitlines()[0]
header_hash=hashlib.sha256()
header_hash.update(mmio.read_bytes())
for p in sorted(source.glob('*.h')):
    header_hash.update(p.read_bytes())
header_hash.update(json.dumps(flags).encode())
header_hash.update(compiler_version.encode())
context_key=header_hash.hexdigest()
files=sorted(source.glob('*.cpp'))
def compile_unit(path):
    target=objects/(path.stem+'.obj')
    state=target.with_suffix('.json')
    with path.open('rb') as stream:
        key=hashlib.file_digest(stream,'sha256').hexdigest()+context_key
    if target.exists() and state.exists() and json.loads(state.read_text()).get('key')==key:
        return dict(file=path.name,exit_code=0,cached=True,object=str(target.relative_to(root)))
    p=subprocess.run([str(compiler),*flags,'-c',str(path),'-o',str(target)],cwd=root,env=env,capture_output=True,text=True)
    log=logs/(path.name+'.log');log.write_text(p.stdout+p.stderr,encoding='utf-8')
    if not p.returncode:state.write_text(json.dumps({'key':key}))
    return dict(file=path.name,exit_code=p.returncode,cached=False,object=str(target.relative_to(root)),log=str(log.relative_to(root)))
results=[]
with ThreadPoolExecutor(max_workers=6) as pool:
    for i,future in enumerate(as_completed(pool.submit(compile_unit,p) for p in files),1):
        result=future.result();results.append(result)
        if result['exit_code'] or i%20==0 or i==len(files):
            print(f'{i}/{len(files)}: {result["file"]} exit {result["exit_code"]}',flush=True)
report=dict(scope='Host x86_64-w64-mingw32 CPU objects and archive; no Switch binary or game runtime',
            compiler=compiler_version,flags=flags,total=len(results),
            failed=sum(bool(r['exit_code']) for r in results),generated_fingerprint=generated_fingerprint(source),
            results=sorted(results,key=lambda r:r['file']))
report['runtime_sources_sha256']={str(mmio.relative_to(root)):hashlib.sha256(mmio.read_bytes()).hexdigest()}
if not report['failed']:
    response=out/'objects.rsp'
    response.write_text('\n'.join('"'+str((root/r['object']).as_posix())+'"' for r in report['results']))
    archive=out/'libAirbornePPC.a'
    # Recreate the derived archive to avoid keeping stale members after shrinkage.
    if archive.exists():archive.unlink()
    p=subprocess.run([str(bin_dir/'llvm-ar.exe'),'rcs',str(archive),'@'+str(response)],env=env,capture_output=True,text=True)
    report['archive_exit_code']=p.returncode
    if p.returncode:report['archive_error']=p.stderr
    else:
        with archive.open('rb') as stream: digest=hashlib.file_digest(stream,'sha256').hexdigest()
        report['archive']=dict(path=str(archive.relative_to(root)),bytes=archive.stat().st_size,sha256=digest)
(root/'analysis/object_build_summary.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(f'Complete: {report["total"]} object builds, {report["failed"]} failures; archive status {report.get("archive_exit_code","not built")}',flush=True)
raise SystemExit(bool(report['failed'] or report.get('archive_exit_code',1)))
