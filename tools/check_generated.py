"""Check all generated C++ translation units with host Clang (no linking)."""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
import os
import subprocess
from generated_state import generated_fingerprint

root=Path(__file__).resolve().parents[1]
bin_dir=root/'tools/toolchain/llvm-mingw-20261020-ucrt-x86_64/bin'
env=dict(os.environ,PATH=str(bin_dir)+os.pathsep+os.environ['PATH'])
logs=root/'analysis/syntax'
logs.mkdir(exist_ok=True)
files=sorted((root/'generated/ppc').glob('*.cpp'))
fingerprint=generated_fingerprint(root/'generated/ppc')
def check(path):
    p=subprocess.run([str(bin_dir/'clang++.exe'),'-std=c++17','-fsyntax-only',
        '-ferror-limit=20','-Wno-macro-redefined','-Wno-unused-value',
        '-DSIMDE_ENABLE_NATIVE_ALIASES','-I'+str(root/'tools/XenonRecomp/thirdparty/simde'),
        str(path)],env=env,capture_output=True,text=True)
    log=logs/(path.name+'.log')
    log.write_text(p.stdout+p.stderr,encoding='utf-8')
    return dict(file=path.name,exit_code=p.returncode,log=str(log.relative_to(root)))
results=[]
with ThreadPoolExecutor(max_workers=6) as pool:
    for i, future in enumerate(as_completed(pool.submit(check,p) for p in files),1):
        result=future.result()
        results.append(result)
        if result['exit_code'] or i%40==0 or i==len(files):
            print(f'{i}/{len(files)}: {result["file"]} exit {result["exit_code"]}',flush=True)
report=dict(scope='Host x86_64 Clang syntax-only; no objects, link, execution or ARM64 validation',
            generated_fingerprint=fingerprint,
            total=len(results),failed=sum(bool(r['exit_code']) for r in results),
            results=sorted(results,key=lambda r:r['file']))
(root/'analysis/syntax_summary.json').write_text(json.dumps(report,indent=2))
print(f'Completed: {report["total"]} translation units, {report["failed"]} failed',flush=True)
raise SystemExit(bool(report['failed']))
