"""Link every generated guest CPU function against libnx on PC.

All Xbox imports deliberately abort. This validates target linking, never game
startup or service implementation. The ELF is not published as a hardware test.
"""
from pathlib import Path
import hashlib
import json
import re
import subprocess
from generated_state import generated_fingerprint

root=Path(__file__).resolve().parents[1]
build=json.loads((root/'analysis/switch_object_build_summary.json').read_text())
if build.get('subset') or build['failed'] or build.get('archive_exit_code',1):
    raise RuntimeError('A complete Switch CPU archive is required')
if build['generated_fingerprint']!=generated_fingerprint(root/'generated/ppc'):
    raise RuntimeError('Generated CPU code changed since archive build')
for path,digest in build['runtime_sources_sha256'].items():
    if hashlib.sha256((root/path).read_bytes()).hexdigest()!=digest:raise RuntimeError('Changed runtime dependency: '+path)
archive=root/build['archive']['path']
if hashlib.sha256(archive.read_bytes()).hexdigest()!=build['archive']['sha256']:
    raise RuntimeError('CPU archive checksum mismatch')
root_linux=subprocess.check_output(['wsl','--exec','wslpath','-a',str(root)],text=True).strip()
def linux(path):return root_linux+'/'+path.relative_to(root).as_posix()
sdk=root_linux+'/tools/toolchain/nx-sdk/opt/devkitpro'
prefix=['wsl','--exec','env','DEVKITPRO='+sdk]
out=root/'generated/switch-cpu'
source='''#include "ppc_recomp_shared.h"
#include <cstdio>
#include <cstdlib>
extern "C" uint64_t airborne_gpu_mmio_read(PPCContext&,uint32_t,unsigned){std::abort();}
extern "C" void airborne_gpu_mmio_write(PPCContext&,uint32_t,uint64_t,unsigned){std::abort();}
'''
imports=[]
for line in (root/'analysis/image/import_symbols.tsv').read_text().splitlines()[1:]:
    name,address,size=line.split('\t')
    if not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*',name):raise ValueError('Invalid import identifier')
    imports.append(name)
    source+=f'PPC_FUNC({name}) {{ std::fprintf(stderr,"UNIMPLEMENTED_IMPORT {name}\\n");std::abort(); }}\n'
source+='''int main() {
    size_t count=0;
    for(auto* p=PPCFuncMappings;p->host;++p) {
        if(p->guest<PPC_CODE_BASE || p->guest>=PPC_CODE_BASE+PPC_CODE_SIZE)return 2;
        ++count;
    }
    std::printf("LINK_CHECK mappings=%zu; imports abort; no guest entry called\\n",count);
    return count==81146 ? 0:3;
}
'''
cpp=out/'link_check.cpp';cpp.write_text(source,encoding='utf-8')
elf=out/'airborne-nx-cpu-link-check.elf'
flags=[*build['flags'],'-I'+linux(root/'generated/ppc')]
command=[*prefix,sdk+'/devkitA64/bin/aarch64-none-elf-g++',*flags,
    '-specs='+sdk+'/libnx/switch.specs',linux(cpp),linux(archive),'-L'+sdk+'/libnx/lib','-lnx',
    '-Wl,--gc-sections','-Wl,-Map='+linux(out/'link_check.map'),'-o',linux(elf)]
process=subprocess.run(command,capture_output=True,text=True)
log=root/'analysis/switch_link.log';log.write_text(process.stdout+process.stderr,encoding='utf-8')
report=dict(scope='Full Switch AArch64 CPU link with aborting Xbox imports; ELF not executed; no game entry/runtime/renderer',
    exit_code=process.returncode,compiler=build['compiler'],generated_fingerprint=build['generated_fingerprint'],
    archive_sha256=build['archive']['sha256'],imports=len(imports),log=str(log.relative_to(root)),
    runtime_sources_sha256=build['runtime_sources_sha256'],link_source_sha256=hashlib.sha256(cpp.read_bytes()).hexdigest())
if process.returncode:
    print(process.stderr[-8000:])
else:
    data=elf.read_bytes()
    if data[:4]!=b'\x7fELF' or int.from_bytes(data[18:20],'little')!=183:
        raise RuntimeError('Link output is not an AArch64 ELF')
    report['elf']=dict(path=str(elf.relative_to(root)),bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),machine=183)
    size=subprocess.check_output([*prefix,sdk+'/devkitA64/bin/aarch64-none-elf-size',linux(elf)],text=True)
    report['sections_size']=size.strip()
    print(size.strip())
(root/'analysis/switch_link_summary.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(f'Switch full CPU link exit={process.returncode}; imports={len(imports)}; not executed')
raise SystemExit(bool(process.returncode))
