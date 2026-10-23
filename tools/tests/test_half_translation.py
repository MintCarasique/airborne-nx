"""FLOAT16_2 generated code: scalar Python references and Xenia golden vectors."""
from pathlib import Path
import bisect
import json
import math
import os
import random
import re
import struct
import subprocess

root=Path(__file__).resolve().parents[2]
out=root/'analysis/instruction_tests'
bin_dir=root/'tools/toolchain/llvm-mingw-20261020-ucrt-x86_64/bin'
env=dict(os.environ,PATH=str(bin_dir)+os.pathsep+os.environ['PATH'])
def run(args):
    p=subprocess.run([str(x) for x in args],cwd=root,env=env,capture_output=True,text=True)
    if p.returncode:raise RuntimeError(f'{p.returncode}: '+p.stdout+p.stderr)
    return p.stdout.strip()
cases=[]
for d,b in ((3,1),(1,1),(65,97),(97,65)):
    fields=(d&31)<<21 | (d&96)>>3 | (b&31)<<11 | (b&96)>>5
    for shift in range(4):
        cases.append((f'half_pack_{d}_{b}_{shift}',0,d,b,shift,6<<26 | (1552&0x730) | fields | 3<<18 | 1<<16 | shift<<6))
    for imm in range(12,16):
        cases.append((f'half_unpack_{d}_{b}_{imm}',1,d,b,0,6<<26 | (2032&0x7f0) | fields | imm<<16))
golden=[]
for filename in ('instr_vpkd3d128.s','instr_vupkd3d128.s'):
    source=(root/'analysis'/filename).read_text()
    for name,body in re.findall(r'^(test_[\w]+):\n(.*?)(?=^test_|\Z)',source,re.M|re.S):
        if '_float16_2' not in name:continue
        code=int(re.search(r'\.long (0x[0-9A-Fa-f]+)',body)[1],16)
        ins={int(reg):[int(w.strip(),16) for w in words.split(',')] for reg,words in re.findall(r'REGISTER_IN v(\d+) \[(.*?)\]',body)}
        expected={int(reg):[int(w.strip(),16) for w in words.split(',')] for reg,words in re.findall(r'REGISTER_OUT v(\d+) \[(.*?)\]',body)}
        fn='gold_'+name
        cases.append((fn,2,0,0,0,code));golden.append((fn,ins,expected))
(out/'half_cases.txt').write_text(''.join(f'{n} {c:08x}\n' for n,op,d,b,s,c in cases))
run([out/'emit.exe',out/'half_cases.txt',out/'half_translated.cpp'])
positive=[struct.unpack('>e',struct.pack('>H',h))[0] for h in range(0x7c00)]
rng=random.Random(0x48414c46)
inputs=[0,0x80000000,0x7f800000,0xff800000,0x7fc00001,0xff800001,
        0x477fe000,0x477fe001,0x47800000,0x33800000,0x337fffff]
inputs += [(e<<23)|m|sign for e in range(256) for m in (0,1,0x7fffff) for sign in (0,1<<31)]
inputs += [rng.getrandbits(32) for _ in range(4096)]
def tohalf(bits):
    sign=(bits>>16)&0x8000
    x=struct.unpack('>f',struct.pack('>I',bits&0x7fffffff))[0]
    if math.isnan(x):return sign|0x7c00|((bits&0x7fffff)>>13)|0x200
    if math.isinf(x):return sign|0x7c00
    return sign | min(0x7bff,bisect.bisect_right(positive,x)-1)
halfref=[]
for h in range(65536):
    value=struct.unpack('>e',struct.pack('>H',h))[0]
    if math.isnan(value):bits=((h&0x8000)<<16)|0x7fc00000|((h&1023)<<13)
    else:bits=struct.unpack('>I',struct.pack('>f',value))[0]
    halfref.append(bits)
declarations='\n'.join(f'extern PPC_FUNC({n});' for n,*_ in cases)
rows='\n'.join(f'{{{n},{op},{d},{b},{s}}},' for n,op,d,b,s,c in cases if op!=2)
def words(values):return '{'+','.join(f'0x{x:08x}u' for x in values)+'}'
goldrows=[]
for n,ins,expected in golden:
    goldrows.append('{'+n+','+','.join(words(table.get(reg,[0]*4)) for table in (ins,expected) for reg in (3,4))+'},')
arrays='uint32_t sourceBits[]='+words(inputs)+';\nuint16_t packedRef[]='+words([tohalf(v) for v in inputs])+';\nuint32_t unpackedRef[]='+words(halfref)+';\n'
harness=r'''
#include "ppc_context.h"
#include <iostream>
DECL
ARRAYS
using Fn=void(*)(PPCContext&,uint8_t*);
struct Test{Fn fn;int op,d,b,shift;};Test tests[]={ROWS};
struct Golden{Fn fn;uint32_t in3[4],in4[4],out3[4],out4[4];};Golden golden[]={GOLD};
int main(){
    alignas(32) uint8_t memory[32]{};size_t checks=0;
    PPCFPSCRRegister host{};const auto original=host.getcsr();
    for(auto&t:tests){
        const size_t n=t.op?65536:sizeof(sourceBits)/sizeof(sourceBits[0]);
        for(size_t k=0;k<n;++k){
            PPCContext ctx{};PPCVRegister* regs[]={POINTERS};ctx.fpscr.loadFromHost();
            ctx.vscr.value=0x10001;
            for(int reg=0;reg<128;++reg)for(int i=0;i<4;++i)regs[reg]->u32[i]=0x12340000+reg*16+i;
            if(t.op){regs[t.b]->u32[0]=(uint32_t(k)<<16)|uint16_t(65535-k);}
            else{regs[t.b]->u32[3]=sourceBits[k];regs[t.b]->u32[2]=sourceBits[(k+1)%n];}
            PPCVRegister expected=*regs[t.d],oldSource=*regs[t.b];
            if(t.op){expected.u32[3]=unpackedRef[k];expected.u32[2]=unpackedRef[65535-k];expected.u32[1]=0;expected.u32[0]=0x3f800000;}
            else{expected.u32[t.shift]=(uint32_t(packedRef[k])<<16)|packedRef[(k+1)%n];}
            t.fn(ctx,memory);
            if(memcmp(regs[t.d],&expected,16)){std::cerr<<"Half mismatch "<<t.op<<' '<<t.d<<' '<<t.shift<<' '<<k;return 1;}
            if(t.d!=t.b && memcmp(regs[t.b],&oldSource,16))return 2;
            if(ctx.vscr.value!=0x10001 || ctx.fpscr.guestFlags)return 3;
            ++checks;
        }
    }
    for(auto&t:golden){
        PPCContext ctx{};ctx.fpscr.loadFromHost();
        for(int i=0;i<4;++i){ctx.v3.u32[3-i]=t.in3[i];ctx.v4.u32[3-i]=t.in4[i];}
        t.fn(ctx,memory);
        for(int i=0;i<4;++i)if(ctx.v3.u32[3-i]!=t.out3[i] || ctx.v4.u32[3-i]!=t.out4[i])return 4;
        ++checks;
    }
    host.setcsr(original);
    std::cout<<checks<<" generated FLOAT16_2 executions passed\n";
}
'''.replace('DECL',declarations).replace('ARRAYS',arrays).replace('ROWS',rows).replace('GOLD','\n'.join(goldrows)).replace('POINTERS',','.join(f'&ctx.v{i}' for i in range(128)))
(out/'half_harness.cpp').write_text(harness)
results={}
for mode,flags in [('native',[]),('portable',['-DSIMDE_NO_NATIVE'])]:
    exe=out/f'test_half_{mode}.exe'
    run([bin_dir/'clang++.exe','-std=c++17','-O2','-Wno-macro-redefined','-DSIMDE_ENABLE_NATIVE_ALIASES',*flags,
         '-I'+str(root/'generated/ppc'),'-I'+str(root/'tools/XenonRecomp/thirdparty/simde'),
         out/'half_translated.cpp',out/'half_harness.cpp','-o',exe])
    results[mode]=run([exe])
(out/'half_results.json').write_text(json.dumps({'golden_vectors':len(golden),'results':results},indent=2))
print(json.dumps(results,indent=2))
