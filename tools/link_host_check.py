"""Link the complete host CPU archive with fail-fast external-import placeholders."""
from pathlib import Path
import json
import os
import re
import subprocess
from generated_state import generated_fingerprint

root=Path(__file__).resolve().parents[1]
out=root/'generated/host'
build=json.loads((root/'analysis/object_build_summary.json').read_text())
if build['failed'] or build.get('archive_exit_code'):
    raise RuntimeError('CPU object archive has not built successfully')
if build['generated_fingerprint']!=generated_fingerprint(root/'generated/ppc'):
    raise RuntimeError('CPU archive is stale; run tools/compile_generated.py first')
imports=[]
for row in (root/'analysis/image/import_symbols.tsv').read_text().splitlines()[1:]:
    name,address,size=row.split('\t')
    if not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*',name):raise ValueError('Invalid symbol name')
    imports.append((name,int(address,16)))
source='''#include "ppc_recomp_shared.h"
#include <cstdio>
#include <cstdlib>
#include <unordered_set>
extern "C" uint64_t airborne_gpu_mmio_read(PPCContext&,uint32_t,unsigned){std::abort();}
extern "C" void airborne_gpu_mmio_write(PPCContext&,uint32_t,uint64_t,unsigned){std::abort();}
'''
for name,address in imports:
    source+=f'''PPC_FUNC({name}) {{
    std::fprintf(stderr,"Unimplemented Xbox 360 import: {name} at 0x{address:08X}\\n");
    std::abort();
}}
'''
source+='''int main() {
    size_t count=0; std::unordered_set<size_t> addresses;
    for(auto* mapping=PPCFuncMappings;mapping->host;++mapping){
        if(mapping->guest<PPC_CODE_BASE || mapping->guest>=PPC_CODE_BASE+PPC_CODE_SIZE)return 2;
        if(!addresses.insert(mapping->guest).second)return 3;
        ++count;
    }
    std::printf("Host CPU archive linked: %zu unique guest mappings; IMPORT_COUNT fail-fast import placeholders. No game entry invoked.\\n",count);
    return 0;
}
'''.replace('IMPORT_COUNT',str(len(imports)))
cpp=out/'host_link_check.cpp'
cpp.write_text(source,encoding='utf-8')
bin_dir=root/'tools/toolchain/llvm-mingw-20261020-ucrt-x86_64/bin'
env=dict(os.environ,PATH=str(bin_dir)+os.pathsep+os.environ['PATH'])
exe=out/'host_link_check.exe'
p=subprocess.run([str(bin_dir/'clang++.exe'),'-std=c++17','-O1','-Wno-macro-redefined',
    '-DSIMDE_ENABLE_NATIVE_ALIASES','-I'+str(root/'generated/ppc'),
    '-I'+str(root/'tools/XenonRecomp/thirdparty/simde'),str(cpp),
    str(root/'runtime/host/roundeven.cpp'),
    str(out/'libAirbornePPC.a'),'-o',str(exe)],env=env,cwd=root,capture_output=True,text=True)
(root/'analysis/host_link.log').write_text(p.stdout+p.stderr,encoding='utf-8')
if p.returncode:raise RuntimeError(p.stderr[-7000:])
p=subprocess.run([str(exe)],env=env,capture_output=True,text=True)
report=dict(scope='Complete x86_64 CPU link with aborting Xbox import placeholders; entry not called',
    generated_fingerprint=build['generated_fingerprint'],imports=len(imports),
    executable=str(exe.relative_to(root)),executable_bytes=exe.stat().st_size,
    exit_code=p.returncode,stdout=p.stdout.strip(),stderr=p.stderr.strip())
(root/'analysis/host_link_summary.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
if p.returncode:raise RuntimeError(f'Link-check execution failed: {report}')
print(p.stdout.strip())
