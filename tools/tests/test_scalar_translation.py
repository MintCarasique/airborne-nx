"""Test emitted GPR/CR operations; integer references use Python arbitrary precision."""
from pathlib import Path
import json
import os
import random
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
for op in ('eqv','mulhd'):
    for d,a,b in ((6,4,5),(4,4,5),(5,4,5)):
        for rc in (0,1):
            code=(31<<26 | a<<21 | d<<16 | b<<11 | 284<<1 | rc) if op=='eqv' else (31<<26 | d<<21 | a<<16 | b<<11 | 73<<1 | rc)
            cases.append((f'test_{op}_{d}{a}{b}_{rc}',op,d,a,b,rc,code))
for op,xo in (('cror',449),('crorc',417)):
    for d in range(32):
        for variant,(a,b) in enumerate((((d+5)%32,(d+11)%32),(d,(d+11)%32),((d+5)%32,d),((d+5)%32,(d+6)%32))):
            cases.append((f'test_{op}_{d}_{variant}',op,d,a,b,0,19<<26 | d<<21 | a<<16 | b<<11 | xo<<1))
(out/'scalar_cases.txt').write_text(''.join(f'{n} {code:08x}\n' for n,op,d,a,b,rc,code in cases))
run([out/'emit.exe',out/'scalar_cases.txt',out/'scalar_translated.cpp'])
decl='\n'.join(f'extern PPC_FUNC({n});' for n,*_ in cases)
rows='\n'.join(f'{{{n},{("eqv","mulhd","cror","crorc").index(op)},{d},{a},{b},{rc}}},' for n,op,d,a,b,rc,c in cases)
mask=(1<<64)-1
rng=random.Random(0x41495242)
edges=[0,1,2,(1<<32)-1,1<<32,(1<<63)-1,1<<63,mask-1,mask]
signed=lambda v:v-(1<<64) if v>>63 else v
count=0
with (out/'scalar_data.txt').open('w') as stream:
    for index,(n,op,d,a,b,rc,code) in enumerate(cases):
        for i in range(512):
            x=edges[i%len(edges)] if i<81 else rng.getrandbits(64)
            y=edges[i//len(edges)%len(edges)] if i<81 else rng.getrandbits(64)
            state=rng.getrandbits(32)
            if op=='eqv':expected=~(x^y)&mask
            elif op=='mulhd':expected=((signed(x)*signed(y))>>64)&mask
            else:
                abit=(state>>(31-a))&1;bbit=(state>>(31-b))&1
                result=abit | (bbit if op=='cror' else 1-bbit)
                expected=(state&~(1<<(31-d))) | result<<(31-d)
            stream.write(f'{index} {x:x} {y:x} {state:x} {expected:x}\n')
            count+=1
source=r'''
#include "ppc_context.h"
#include <fstream>
#include <iostream>
DECL
using Fn=void(*)(PPCContext&,uint8_t*);
struct Test{Fn fn;int op,d,a,b,rc;};
Test tests[]={ROWS};
int main(int argc,char**argv){
    if(argc!=2)return 2;
    std::ifstream input(argv[1]);
    size_t index,count=0; uint64_t x,y,state,expected;
    alignas(32) uint8_t memory[32]{};
    while(input>>std::dec>>index>>std::hex>>x>>y>>state>>expected){
        const auto&t=tests[index]; PPCContext ctx{};
        PPCRegister* r[]={&ctx.r0,&ctx.r1,&ctx.r2,&ctx.r3,&ctx.r4,&ctx.r5,&ctx.r6};
        PPCCRRegister* cr[]={&ctx.cr0,&ctx.cr1,&ctx.cr2,&ctx.cr3,&ctx.cr4,&ctx.cr5,&ctx.cr6,&ctx.cr7};
        uint8_t* bits[32];
        for(int i=0;i<8;++i){bits[i*4]=&cr[i]->lt;bits[i*4+1]=&cr[i]->gt;bits[i*4+2]=&cr[i]->eq;bits[i*4+3]=&cr[i]->so;}
        for(int i=0;i<32;++i)*bits[i]=uint8_t((state>>(31-i))&1);
        if(t.op<2){r[t.a]->u64=x;r[t.b]->u64=y;ctx.xer.so=uint8_t(count%2);}
        t.fn(ctx,memory);
        if(t.op<2){
            if(r[t.d]->u64!=expected){std::cerr<<"GPR mismatch "<<index;return 3;}
            if(t.d!=t.a && r[t.a]->u64!=x)return 4;
            if(t.d!=t.b && r[t.b]->u64!=y)return 5;
            if(t.rc){
                if(ctx.cr0.lt!=bool(expected>>63)||ctx.cr0.gt!=bool(expected && !(expected>>63))||ctx.cr0.eq!=(expected==0)||ctx.cr0.so!=ctx.xer.so)return 6;
            }
            for(int i=t.rc?4:0;i<32;++i)if(*bits[i]!=((state>>(31-i))&1))return 7;
        }else{
            uint32_t actual=0;for(int i=0;i<32;++i)actual|=uint32_t(*bits[i])<<(31-i);
            if(actual!=expected){std::cerr<<"CR mismatch "<<index;return 8;}
        }
        ++count;
    }
    std::cout<<count<<" generated scalar instruction executions passed\n";
}
'''.replace('DECL',decl).replace('ROWS',rows)
(out/'scalar_harness.cpp').write_text(source)
exe=out/'test_scalar.exe'
run([bin_dir/'clang++.exe','-std=c++17','-O2','-Wno-macro-redefined','-DSIMDE_ENABLE_NATIVE_ALIASES',
     '-I'+str(root/'generated/ppc'),'-I'+str(root/'tools/XenonRecomp/thirdparty/simde'),
     out/'scalar_translated.cpp',out/'scalar_harness.cpp','-o',exe])
result=run([exe,out/'scalar_data.txt'])
(out/'scalar_results.json').write_text(json.dumps({'executions':count,'result':result},indent=2))
print(result)
