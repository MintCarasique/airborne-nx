"""Build a real libnx NRO with an isolated guest leaf and emitted half tests."""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import argparse
from generated_state import generated_fingerprint
root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser();parser.add_argument('--kind',choices=('cpu','memory'),default='cpu');parser.add_argument('--compile-only',action='store_true');options=parser.parse_args()
memory_probe=options.kind=='memory'
probe_version='0.3' if memory_probe else '0.1'
name='airborne-nx-startup-v0.3' if memory_probe else 'airborne-nx-probe'
summary_name='switch_startup_probe_summary.json' if memory_probe else 'switch_probe_summary.json'
build_log='switch_memory_probe_build.log' if memory_probe else 'switch_probe_build.log'
sdk=root/'tools/toolchain/nx-sdk/opt/devkitpro'
out=root/('generated/switch-memory-probe' if memory_probe else 'generated/switch-probe');out.mkdir(exist_ok=True)
bundle=root/'dist/switch'/name;bundle.mkdir(parents=True,exist_ok=True)
def linux(path):return subprocess.check_output(['wsl','--exec','wslpath','-a',str(path)],text=True).strip()
sdk_linux=linux(sdk)
compiler=sdk_linux+'/devkitA64/bin/aarch64-none-elf-g++'
def run(args):
    p=subprocess.run(['wsl','--exec','env','DEVKITPRO='+sdk_linux,*args],cwd=root,capture_output=True,text=True)
    with (root/'analysis'/build_log).open('a',encoding='utf-8') as f:f.write(p.stdout+p.stderr)
    if p.returncode:raise RuntimeError(p.stderr[-6000:])
    return p.stdout
(root/'analysis'/build_log).write_text('')
matches=[p for p in (root/'generated/ppc').glob('*.cpp') if 'PPC_FUNC_IMPL(__imp__sub_828B6D00)' in p.read_text()]
assert len(matches)==1
text=matches[0].read_text();start=text.index('PPC_FUNC_IMPL(__imp__sub_828B6D00)');end=text.index('{',start)+1;depth=1
while depth:
    depth+=(text[end]=='{')-(text[end]=='}');end+=1
prefix='#include <cstdint>\nuint8_t switch_probe_load8(uint32_t);\n#define PPC_LOAD_U8(x) switch_probe_load8(uint32_t(x))\n#include "ppc_recomp_shared.h"\n'
if memory_probe:prefix='#include "ppc_recomp_shared.h"\n'
(out/'leaf.cpp').write_text(prefix+text[start:end]+'\n',newline='\n')
harness=(root/'analysis/instruction_tests/half_harness.cpp').read_text().replace('int main(){','int run_half_tests(){')
assert harness.rstrip().endswith('}')
harness=harness.rstrip()[:-1]+'return 0;\n}\n'
(out/'half_harness.cpp').write_text(harness,newline='\n')
sources=[root/'runtime/switch/probe.cpp',out/'leaf.cpp',out/'half_harness.cpp',root/'analysis/instruction_tests/half_translated.cpp']
if memory_probe:sources=[root/'runtime/switch/memory_probe.cpp',out/'leaf.cpp',root/'analysis/instruction_tests/physical_translated.cpp']
flags=['-std=c++17','-O2','-march=armv8-a+crc+crypto','-mtune=cortex-a57','-mtp=soft','-fPIE',
       '-ffunction-sections','-fdata-sections','-fno-strict-aliasing','-ffp-contract=off','-fno-fast-math',
       '-fno-rtti','-fno-exceptions','-D__SWITCH__','-DSIMDE_ENABLE_NATIVE_ALIASES','-Wno-psabi',
       '-I'+linux(root/'generated/ppc'),'-I'+linux(root/'tools/XenonRecomp/thirdparty/simde'),'-I'+sdk_linux+'/libnx/include']
dependencies=[*sources,*([root/'runtime/switch/guest_memory.h',root/'runtime/switch/startup_services.h'] if memory_probe else [])]
source_hashes={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
build_id=hashlib.sha256(json.dumps(dict(sources=source_hashes,generated=generated_fingerprint(root/'generated/ppc'),flags=flags),sort_keys=True).encode()).hexdigest()
if memory_probe:flags.append('-DAIRBORNE_PROBE_BUILD_ID="'+build_id+'"')
objects=[]
for i,source in enumerate(sources):
    obj=out/f'unit{i}.o';run([compiler,*flags,'-c',linux(source),'-o',linux(obj)]);objects.append(obj)
if options.compile_only:
    record=dict(scope='AArch64 object compilation only; no new NRO produced or console run requested',
        kind=options.kind,build_id=build_id,sources_sha256=source_hashes,
        objects={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in objects})
    (root/'analysis'/('switch_'+options.kind+'_compile_summary.json')).write_text(json.dumps(record,indent=2))
    print(f'{len(objects)} AArch64 objects compiled; previous NRO retained.');raise SystemExit(0)
elf=out/(name+'.elf')
run([compiler,*flags,'-specs='+sdk_linux+'/libnx/switch.specs',*[linux(p) for p in objects],
    '-L'+sdk_linux+'/libnx/lib','-lnx','-Wl,--gc-sections','-o',linux(elf)])
nacp=out/(name+'.nacp')
run([sdk_linux+'/tools/bin/nacptool','--create','AirborneNX Startup Probe' if memory_probe else 'AirborneNX CPU Probe','AirborneNX',probe_version,linux(nacp)])
nro=bundle/(name+'.nro')
run([sdk_linux+'/tools/bin/elf2nro',linux(elf),linux(nro),'--nacp='+linux(nacp)])
shutil.copyfile(root/'analysis/image/image_loader_normalized.bin',bundle/'image.bin')
version=run([compiler,'--version']).splitlines()[0]
report=dict(scope='Switch ARM64 '+('startup' if memory_probe else options.kind)+' probe; compiled/linked only, not run on console; not a game port',
    build_id=build_id,
    probe_kind='startup' if memory_probe else 'cpu',probe_version=probe_version,bundle_dir=str(bundle.relative_to(root)),nro_name=nro.name,elf=str(elf.relative_to(root)),
    compiler=version,generated_fingerprint=generated_fingerprint(root/'generated/ppc'),
    guest_leaf='0x828B6D00',leaf_source=str(matches[0].relative_to(root)),
    memory_note='4 GiB VA reservation; image/stack/4 MiB heap and small A/C/E physical aliases use writable AliasCodeData staging mappings. Native workers/TLS/events/FP state, direct leaf execution with private guest stacks and R13 slots; no game entry or full heap runtime.' if memory_probe else 'Isolated leaf uses a checked byte-load callback into the image; no full 4 GiB guest map or game entry',
    sources_sha256=source_hashes,
    artifacts={str(p.relative_to(root)):dict(bytes=p.stat().st_size,sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in (nro,bundle/'image.bin',elf)})
(root/'analysis'/summary_name).write_text(json.dumps(report,indent=2))
print(f'Built {nro} ({nro.stat().st_size} bytes); physical Switch execution pending')
