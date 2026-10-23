"""Execute emitted frsqrte code against PowerPC special cases and Decimal bounds."""
from pathlib import Path
import decimal
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
    if p.returncode: raise RuntimeError(f'{p.returncode}: '+p.stdout+p.stderr)
    return p.stdout.strip()
cases=[]
for d,b in ((13,0),(1,1),(0,13)):
    for rc in (0,1):
        cases.append((f'test_frsqrte_{d}_{b}_{rc}',d,b,rc,63<<26 | d<<21 | b<<11 | 26<<1 | rc))
(out/'frsqrte_cases.txt').write_text(''.join(f'{n} {c:08x}\n' for n,d,b,rc,c in cases))
run([out/'emit.exe',out/'frsqrte_cases.txt',out/'frsqrte_translated.cpp'])
rng=random.Random(0x46525351)
values=[0,1,0xfffffffffffff,0x10000000000000,0x3ff0000000000000,0x4000000000000000,
        0x4010000000000000,0x7fefffffffffffff,0x7ff0000000000000,
        0x7ff0000000000001,0x7ff8000000000001,0x7fffffffffffffff]
values += [v|(1<<63) for v in list(values)]
# Cover every finite normal exponent as well as both signs of random bit patterns.
values += [(e<<52)|rng.getrandbits(52) for e in range(1,2047)]
values += [rng.getrandbits(64) for _ in range(2048)]
presets=[0,0x0007f000,0x80,0x10,0x04000000,0x84000000,0xa1000000,0xd0000040]
invalid_mask=0x01f80700
decimal.getcontext().prec=80
def reference(bits,flags,old):
    magnitude=bits&0x7fffffffffffffff; fraction=bits&0xfffffffffffff
    exponent=bits&0x7ff0000000000000
    cause=0; suppress=False; kind=0
    if exponent==0x7ff0000000000000 and fraction:
        if not bits&0x8000000000000: cause=0x1000000;suppress=bool(flags&0x80)
        result=bits|0x8000000000000; classification=0x11
    elif magnitude==0:
        cause=0x4000000;suppress=bool(flags&0x10)
        result=(bits&(1<<63))|0x7ff0000000000000;classification=9 if bits>>63 else 5
    elif bits>>63:
        cause=0x200;suppress=bool(flags&0x80)
        result=0x7ff8000000000000;classification=0x11
    elif magnitude==0x7ff0000000000000: result=0;classification=2
    else:
        x=struct.unpack('>d',struct.pack('>Q',bits))[0]
        ref=float(decimal.Decimal(1)/decimal.Decimal.from_float(x).sqrt())
        result=struct.unpack('>Q',struct.pack('>d',ref))[0];classification=4;kind=1
    if cause:
        if not flags&cause: flags|=0x80000000
        flags|=cause
        flags=(flags&~0x20000000)|(0x20000000 if flags&invalid_mask else 0)
        enabled=any(flags&a and flags&b for a,b in ((0x20000000,0x80),(0x10000000,0x40),
                    (0x8000000,0x20),(0x4000000,0x10),(0x2000000,8)))
        flags=(flags&~0x40000000)|(0x40000000 if enabled else 0)
    if suppress:return old,flags,0
    return result,(flags&~0x7f000)|(classification<<12),kind
count=0
with (out/'frsqrte_data.txt').open('w') as stream:
    for index,(n,d,b,rc,c) in enumerate(cases):
        for i,bits in enumerate(values):
            initial=presets[i%len(presets)]
            old=bits if d==b else 0x1234000000000000+d
            result,flags,kind=reference(bits,initial,old)
            stream.write(' '.join(f'{x:x}' for x in (index,bits,initial,i%4,result,flags,kind))+'\n')
            count+=1
decl='\n'.join(f'extern PPC_FUNC({n});' for n,*_ in cases)
rows='\n'.join(f'{{{n},{d},{b},{rc}}},' for n,d,b,rc,c in cases)
fprs=','.join(f'&ctx.f{i}' for i in range(32))
harness=r'''
#include "ppc_context.h"
#include <fstream>
#include <iostream>
DECL
using Fn=void(*)(PPCContext&,uint8_t*);struct Test{Fn fn;int d,b,rc;};Test tests[]={ROWS};
int main(int argc,char**argv){
    if(argc!=2)return 2;std::ifstream input(argv[1]);
    size_t index,count=0;uint64_t bits,result;uint32_t initial,rounding,flags,kind;
    alignas(32) uint8_t memory[32]{};
    PPCFPSCRRegister originalRegister{};
    const auto original=originalRegister.getcsr();
    while(input>>std::hex>>index>>bits>>initial>>rounding>>result>>flags>>kind){
        PPCContext ctx{};PPCRegister* regs[]={FPRS};const auto&t=tests[index];
        for(int i=0;i<32;++i)regs[i]->u64=0x1234000000000000ull+i;
        regs[t.b]->u64=bits;ctx.fpscr.guestFlags=initial;
        ctx.fpscr.csr=original;
        ctx.fpscr.csr=(ctx.fpscr.csr&~PPCFPSCRRegister::RoundMask)|PPCFPSCRRegister::GuestToHost[rounding];
        ctx.fpscr.csr|=PPCFPSCRRegister::FlushMask;ctx.fpscr.setcsr(ctx.fpscr.csr);
        ctx.cr1.lt=0;ctx.cr1.gt=1;ctx.cr1.eq=0;ctx.cr1.so=1;
        t.fn(ctx,memory);
        if(kind){
            double ref;memcpy(&ref,&result,8);
            if(!std::isfinite(regs[t.d]->f64)||std::abs(regs[t.d]->f64/ref-1.0)>1.0/32.0){std::cerr<<"Estimate accuracy "<<index;return 3;}
        }else if(regs[t.d]->u64!=result){std::cerr<<"Special result "<<index<<' '<<std::hex<<bits;return 4;}
        if(ctx.fpscr.guestFlags!=flags){std::cerr<<"FPSCR flags "<<index;return 5;}
        if(t.rc){
            if(ctx.cr1.lt!=bool(flags&0x80000000)||ctx.cr1.gt!=bool(flags&0x40000000)||ctx.cr1.eq!=bool(flags&0x20000000)||ctx.cr1.so!=bool(flags&0x10000000))return 6;
        }else if(ctx.cr1.lt||!ctx.cr1.gt||ctx.cr1.eq||!ctx.cr1.so)return 7;
        for(int i=0;i<32;++i)if(i!=t.d && regs[i]->u64!=(i==t.b?bits:0x1234000000000000ull+i))return 8;
        if((ctx.fpscr.loadFromHost()&~3u)!=flags)return 9;
        if((ctx.fpscr.loadFromHost()&3u)!=rounding){std::cerr<<"Round-trip rounding "<<index<<" input "<<std::hex<<bits<<" expected "<<rounding<<" actual "<<(ctx.fpscr.loadFromHost()&3u)<<" csr "<<ctx.fpscr.getcsr();return 10;}
        ++count;
    }
    originalRegister.setcsr(original);
    // Test MTFSF field selection against independently constructed nibble masks.
    for(unsigned fm=0;fm<256;++fm){
        PPCFPSCRRegister reg{};reg.csr=original;reg.guestFlags=0x0001f000;
        uint32_t mask=0;for(unsigned nibble=0;nibble<8;++nibble)if(fm&(1u<<nibble))mask|=0xfu<<(nibble*4);
        const uint32_t value=0x00040003;
        reg.storeFromGuest(value,uint8_t(fm));
        uint32_t expected=(0x0001f000&~mask)|(value&mask&~3u);
        if(reg.guestFlags!=expected)return 11;
        if((reg.loadFromHost()&3u)!=((fm&1)?3u:PPCFPSCRRegister::HostToGuest[(original&PPCFPSCRRegister::RoundMask)>>PPCFPSCRRegister::RoundShift]))return 12;
        originalRegister.setcsr(original);
    }
    std::cout<<count<<" generated frsqrte executions and 256 FPSCR mask checks passed\n";
}
'''.replace('DECL',decl).replace('ROWS',rows).replace('FPRS',fprs)
(out/'frsqrte_harness.cpp').write_text(harness)
results={}
for mode,options in [('native',[]),('portable',['-DSIMDE_NO_NATIVE'])]:
    exe=out/f'test_frsqrte_{mode}.exe'
    run([bin_dir/'clang++.exe','-std=c++17','-O2','-Wno-macro-redefined','-DSIMDE_ENABLE_NATIVE_ALIASES',*options,
        '-I'+str(root/'generated/ppc'),'-I'+str(root/'tools/XenonRecomp/thirdparty/simde'),
        out/'frsqrte_translated.cpp',out/'frsqrte_harness.cpp','-o',exe])
    results[mode]=run([exe,out/'frsqrte_data.txt'])
(out/'frsqrte_results.json').write_text(json.dumps({'executions_per_mode':count,'results':results},indent=2))
print(json.dumps(results,indent=2))
