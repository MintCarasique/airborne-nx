"""Exercise emitted vctuxs/VMX128 conversions against Python float64 references."""
from pathlib import Path
import json
import math
import os
import random
import struct
import subprocess

root=Path(__file__).resolve().parents[2]
out=root/'analysis/instruction_tests'
bin_dir=root/'tools/toolchain/llvm-mingw-20261020-ucrt-x86_64/bin'
env=dict(os.environ,PATH=str(bin_dir)+os.pathsep+os.environ['PATH'])
def run(args):
    p=subprocess.run([str(x) for x in args],cwd=root,env=env,capture_output=True,text=True)
    if p.returncode: raise RuntimeError(p.stdout+p.stderr)
    return p.stdout.strip()
cases=[]
for vmx in (False,True):
    pairs=((3,1),(1,1),(65,97),(97,65)) if vmx else ((3,1),(1,1),(31,30),(30,31))
    for d,b in pairs:
        for scale in range(32):
            code=(6<<26 | 624 | (d&31)<<21 | (d&96)>>3 | (b&31)<<11 | (b&96)>>5 | scale<<16) if vmx else (4<<26 | 906 | d<<21 | b<<11 | scale<<16)
            cases.append((f'test_ctu_{int(vmx)}_{d}_{b}_{scale}',d,b,scale,code))
(out/'conversion_cases.txt').write_text(''.join(f'{n} {c:08x}\n' for n,d,b,s,c in cases))
run([out/'emit.exe',out/'conversion_cases.txt',out/'conversion_translated.cpp'])
edges=[0,0x80000000,1,0x80000001,0x007fffff,0x807fffff,0x00800000,0x80800000,
       0x3f000000,0xbf000000,0x3f7fffff,0xbf7fffff,0x3f800000,0xbf800000,
       0x4f7fffff,0x4f800000,0xcf800000,0x7f800000,0xff800000,0x7fc00000,
       0x7f800001,0xffc00000,0x7f7fffff,0xff7fffff]
rng=random.Random(0x435455)
count=0
with (out/'conversion_data.txt').open('w') as stream:
    for index,(n,d,b,scale,code) in enumerate(cases):
        for iteration in range(300):
            bits=[edges[(iteration+i)%len(edges)] if iteration<96 else rng.getrandbits(32) for i in range(4)]
            sat=(0,1,0x10000,0x10001)[iteration%4]
            initial=sat; expected=[]
            for raw in bits:
                value=struct.unpack('>f',struct.pack('>I',raw))[0]
                if math.isnan(value): number=0
                elif math.isinf(value): number=0 if value<0 else 0xffffffff; sat|=1
                else:
                    # binary32 values scale exactly in binary64, including subnormals.
                    integer=math.trunc(math.ldexp(value,scale))
                    number=max(0,min(0xffffffff,integer))
                    if number!=integer: sat|=1
                expected.append(number)
            stream.write(' '.join(f'{x:x}' for x in [index,initial,*bits,sat,*expected])+'\n')
            count+=1
decl='\n'.join(f'extern PPC_FUNC({n});' for n,*_ in cases)
rows='\n'.join(f'{{{n},{d},{b}}},' for n,d,b,s,c in cases)
pointers=','.join(f'&ctx.v{i}' for i in range(128))
harness=r'''
#include "ppc_context.h"
#include <fstream>
#include <iostream>
DECL
using Fn=void(*)(PPCContext&,uint8_t*);
struct Test{Fn fn;int d,b;}; Test tests[]={ROWS};
int main(int argc,char**argv){
    if(argc!=2)return 2;
    std::ifstream input(argv[1]);
    size_t index,count=0; uint32_t initial,raw[4],sat,expected[4];
    alignas(32) uint8_t memory[32]{};
    while(input>>std::hex>>index>>initial>>raw[0]>>raw[1]>>raw[2]>>raw[3]>>sat>>expected[0]>>expected[1]>>expected[2]>>expected[3]){
        PPCContext ctx{}; PPCVRegister* regs[]={POINTERS};
        const auto&t=tests[index];ctx.vscr.value=initial;
        for(int i=0;i<4;++i)regs[t.b]->u32[3-i]=raw[i];
        t.fn(ctx,memory);
        if(ctx.vscr.value!=sat){std::cerr<<"Conversion SAT mismatch "<<index;return 3;}
        for(int i=0;i<4;++i){
            if(regs[t.d]->u32[3-i]!=expected[i]){std::cerr<<"Conversion value mismatch "<<index;return 4;}
            if(t.d!=t.b && regs[t.b]->u32[3-i]!=raw[i])return 5;
        }
        ++count;
    }
    std::cout<<count<<" generated conversion executions passed\n";
}
'''.replace('DECL',decl).replace('ROWS',rows).replace('POINTERS',pointers)
(out/'conversion_harness.cpp').write_text(harness)
results={}
for mode,flags in [('native',[]),('portable',['-DSIMDE_NO_NATIVE'])]:
    exe=out/f'test_conversion_{mode}.exe'
    run([bin_dir/'clang++.exe','-std=c++17','-O2','-Wno-macro-redefined','-DSIMDE_ENABLE_NATIVE_ALIASES',*flags,
         '-I'+str(root/'generated/ppc'),'-I'+str(root/'tools/XenonRecomp/thirdparty/simde'),
         out/'conversion_translated.cpp',out/'conversion_harness.cpp','-o',exe])
    results[mode]=run([exe,out/'conversion_data.txt'])
(out/'conversion_results.json').write_text(json.dumps({'executions_per_mode':count,'results':results},indent=2))
print(json.dumps(results,indent=2))
