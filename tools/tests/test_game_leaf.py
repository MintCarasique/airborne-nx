"""Run one real, recompiled Airborne leaf function on a mapped guest image."""
from pathlib import Path
import json
import os
import subprocess

root=Path(__file__).resolve().parents[2]
out=root/'analysis/instruction_tests'
bin_dir=root/'tools/toolchain/llvm-mingw-20261020-ucrt-x86_64/bin'
env=dict(os.environ,PATH=str(bin_dir)+os.pathsep+os.environ['PATH'])
units=[p for p in (root/'generated/ppc').glob('ppc_recomp.*.cpp')
       if 'PPC_FUNC_IMPL(__imp__sub_828B6D00)' in p.read_text()]
assert len(units)==1
text=units[0].read_text()
start=text.index('PPC_FUNC_IMPL(__imp__sub_828B6D00)')
end=text.index('{',start)+1
depth=1
while depth:
    depth+=(text[end]=='{')-(text[end]=='}')
    end+=1
# Compile the verbatim implementation in isolation: COFF weak aliases retain
# unrelated functions in a whole generated unit, pulling in the rest of the game.
(out/'game_leaf.cpp').write_text('#include "ppc_recomp_shared.h"\n'+text[start:end]+'\n')
source=r'''
#define NOMINMAX
#include <windows.h>
#include "ppc_recomp_shared.h"
#include <fstream>
#include <iostream>
#include <vector>
extern "C" PPC_FUNC(__imp__sub_828B6D00);
int main(int argc,char**argv){
    if(argc!=2)return 2;
    std::ifstream input(argv[1],std::ios::binary|std::ios::ate);
    if(!input)return 3;const auto size=input.tellg();input.seekg(0);
    std::vector<uint8_t> image(size);input.read((char*)image.data(),size);
    auto* base=(uint8_t*)VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS);
    if(!base)return 4;
    if(!VirtualAlloc(base+0x82000000ull,image.size(),MEM_COMMIT,PAGE_READWRITE))return 5;
    memcpy(base+0x82000000ull,image.data(),image.size());
    size_t checks=0;
    for(unsigned trial=0;trial<64;++trial){
        for(unsigned index=0;index<12;++index){
            PPCContext ctx{};ctx.r3.u64=index;ctx.r1.u64=0x12345678;
            ctx.ctr.u64=0xfedcba98+trial;ctx.r12.u64=trial;
            __imp__sub_828B6D00(ctx,base);
            if(ctx.r3.u32!=0x820f56c0u+4*index){std::cerr<<"Guest leaf result mismatch "<<index;return 6;}
            if(ctx.r1.u64!=0x12345678)return 7;
            ++checks;
        }
    }
    VirtualFree(base,0,MEM_RELEASE);
    std::cout<<checks<<" real Airborne leaf executions passed\n";
}
'''
(out/'game_leaf_harness.cpp').write_text(source)
exe=out/'test_game_leaf.exe'
args=[bin_dir/'clang++.exe','-std=c++17','-O1','-ffunction-sections','-fdata-sections',
      '-Wno-macro-redefined','-Wno-unused-value','-DSIMDE_ENABLE_NATIVE_ALIASES',
      '-I'+str(root/'generated/ppc'),'-I'+str(root/'tools/XenonRecomp/thirdparty/simde'),
      out/'game_leaf.cpp',out/'game_leaf_harness.cpp','-Wl,--gc-sections','-o',exe]
p=subprocess.run([str(x) for x in args],cwd=root,env=env,capture_output=True,text=True)
(out/'game_leaf_build.log').write_text(p.stdout+p.stderr)
if p.returncode:raise RuntimeError(p.stderr[-5000:])
p=subprocess.run([str(exe),str(root/'analysis/image/image_loader_normalized.bin')],env=env,capture_output=True,text=True)
if p.returncode:raise RuntimeError(f'{p.returncode}: '+p.stdout+p.stderr)
(out/'game_leaf_results.json').write_text(json.dumps({'unit':units[0].name,'function':'0x828B6D00',
    'executions':768,'result':p.stdout.strip(),'scope':'Host x86_64; guest leaf only, not game boot'},indent=2))
print(p.stdout.strip())
