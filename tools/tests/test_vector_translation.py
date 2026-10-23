"""Execute actual recompiler output against a scalar guest-byte-order oracle."""
from pathlib import Path
import json
import os
import subprocess

root = Path(__file__).resolve().parents[2]
out = root/'analysis/instruction_tests'
out.mkdir(exist_ok=True)
bin_dir = root/'tools/toolchain/llvm-mingw-20261020-ucrt-x86_64/bin'
env = dict(os.environ, PATH=str(bin_dir)+os.pathsep+os.environ['PATH'])
upstream = root/'tools/XenonRecomp'
build = upstream/'build'

def run(args):
    result = subprocess.run([str(a) for a in args], cwd=root, env=env, capture_output=True, text=True)
    with (out/'build.log').open('a', encoding='utf-8') as log:
        log.write(result.stdout+result.stderr)
    if result.returncode:
        raise RuntimeError(f'{args[0]} failed ({result.returncode}): {result.stderr[-3000:]}')
    return result.stdout

(out/'build.log').write_text('', encoding='utf-8')
includes = [upstream/'XenonRecomp', upstream/'XenonAnalyse', upstream/'XenonUtils',
            upstream/'thirdparty/simde', upstream/'thirdparty/disasm',
            upstream/'thirdparty/fmt/include', upstream/'thirdparty/tomlplusplus/include',
            upstream/'thirdparty/xxHash']
objects = [build/'XenonRecomp/CMakeFiles/XenonRecomp.dir/recompiler.cpp.obj',
           build/'XenonRecomp/CMakeFiles/XenonRecomp.dir/recompiler_config.cpp.obj',
           build/'XenonAnalyse/libLibXenonAnalyse.a', build/'XenonUtils/libXenonUtils.a',
           build/'thirdparty/disasm/libdisasm.a', build/'thirdparty/fmt/libfmt.a',
           build/'thirdparty/xxHash/cmake_unofficial/libxxhash.a']
run([bin_dir/'clang++.exe', '-std=c++17', '-O2', '-include', 'cstdlib',
     *['-I'+str(i) for i in includes], root/'tools/tests/emit_instruction_cases.cpp',
     *objects, '-o', out/'emit.exe'])
ops = {'vslh':324, 'vsrah':836, 'vspltish':844, 'vandc':1092, 'vnor':1284,
       'vmaxsh':322, 'vminsh':834, 'vavguh':1090, 'vsrh':580, 'vsrab':772, 'vsububm':1024}
ops.update({'vsubshs':1856,'vaddsbs':768,'vaddsws':896,'vpkswss':462,'vpkswus':334,
            'vpkshss':398,'vpkuhus':142,'vcmpequh':70,'vcmpgtsh':838,'vcmpgtsw':902,
            'vcmpgtuw':646,'vrlh':68,'vaddshs':832,'vaddubs':512,'vadduws':640,
            'vsubsws':1920,'vsububs':1536,'vpkshus':270,'vcmpgtuh':582})
ops['vsel128']=848
cases = []
for op, xo in ops.items():
    if op == 'vspltish':
        for imm in range(32):
            cases.append((f'test_{op}_{imm}', op, 3, 0, 0, imm, 4<<26 | 3<<21 | imm<<16 | xo))
    else:
        for d,a,b in ((3,1,2), (1,1,2), (2,1,2), (1,1,1)):
            cases.append((f'test_{op}_{d}{a}{b}', op, d,a,b,0, (5 if op=='vsel128' else 4)<<26 | d<<21 | a<<16 | b<<11 | xo))
            if op.startswith('vcmp'):
                cases.append((f'test_{op}_{d}{a}{b}_rc',op,d,a,b,1,4<<26 | d<<21 | a<<16 | b<<11 | xo | 1024))
(out/'cases.txt').write_text(''.join(f'{n} {code:08x}\n' for n,op,d,a,b,imm,code in cases))
run([out/'emit.exe', out/'cases.txt', out/'translated.cpp'])
declarations = '\n'.join(f'extern PPC_FUNC({n});' for n,*_ in cases)
rows = '\n'.join(f'{{"{n}", {n}, {list(ops).index(op)}, {d}, {a}, {b}, {imm}}},' for n,op,d,a,b,imm,c in cases)
harness = r'''
#include "ppc_context.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <random>
DECLARATIONS
using Fn = void (*)(PPCContext&, uint8_t*);
struct Test { const char* name; Fn fn; int op,d,a,b,imm; };
Test tests[] = { ROWS };
using Guest = std::array<uint8_t,16>;
uint16_t half(const Guest& g, int i) { return uint16_t(g[i*2])<<8 | g[i*2+1]; }
int sign16(uint16_t v) { return v & 0x8000 ? int(v)-65536 : v; }
int sign8(uint8_t v) { return v & 0x80 ? int(v)-256 : v; }
// Division expresses arithmetic right shift independently of host signed shifts.
int sar(int value, unsigned shift) {
    int divisor = 1 << shift;
    return value < 0 ? -((-value + divisor-1) / divisor) : value / divisor;
}
void put(Guest& g, int i, uint16_t v) { g[i*2]=uint8_t(v>>8); g[i*2+1]=uint8_t(v); }
uint32_t word(const Guest& g,int i) { return uint32_t(half(g,i*2))<<16 | half(g,i*2+1); }
int64_t sign32(uint32_t v) { return v & 0x80000000u ? int64_t(v)-0x100000000ll : v; }
void putWord(Guest& g,int i,uint32_t v) { put(g,i*2,uint16_t(v>>16)); put(g,i*2+1,uint16_t(v)); }
int64_t clamp(int64_t v,int64_t lo,int64_t hi,uint32_t& sat) {
    if(v<lo || v>hi) sat|=1;
    return std::max(lo,std::min(hi,v));
}
Guest oracle(const Test& t, const Guest& a, const Guest& b, const Guest& old, uint32_t& sat) {
    Guest r{};
    for(int i=0;i<8;++i) {
        uint16_t x=half(a,i), y=half(b,i), v=0;
        switch(t.op) {
        case 0: v=uint16_t(uint32_t(x) * (1u << (y%16))); break;
        case 1: v=uint16_t(sar(sign16(x),y%16)); break;
        case 2: v=uint16_t(t.imm<16?t.imm:t.imm-32); break;
        case 5: v=uint16_t(std::max(sign16(x),sign16(y))); break;
        case 6: v=uint16_t(std::min(sign16(x),sign16(y))); break;
        case 7: v=uint16_t((uint32_t(x)+y+1)/2); break;
        case 8: v=uint16_t(x/(1u<<(y%16))); break;
        case 11: v=uint16_t(clamp(sign16(x)-sign16(y),-32768,32767,sat)); break;
        case 18: v=x==y?65535:0; break;
        case 19: v=sign16(x)>sign16(y)?65535:0; break;
        case 22: { unsigned k=y%16; v=uint16_t(uint32_t(x)*(1u<<k)+uint32_t(x)/(1u<<(16-k))); break; }
        case 23: v=uint16_t(clamp(sign16(x)+sign16(y),-32768,32767,sat)); break;
        case 29: v=x>y?65535:0; break;
        }
        put(r,i,v);
    }
    for(int i=0;i<16;++i) {
        switch(t.op) {
        case 3: r[i]=uint8_t(a[i]&uint8_t(~b[i])); break;
        case 4: r[i]=uint8_t(~(a[i]|b[i])); break;
        case 9: r[i]=uint8_t(sar(sign8(a[i]),b[i]%8)); break;
        case 10: r[i]=uint8_t(int(a[i])-int(b[i])); break;
        case 12: r[i]=uint8_t(clamp(sign8(a[i])+sign8(b[i]),-128,127,sat)); break;
        case 24: r[i]=uint8_t(clamp(int(a[i])+int(b[i]),0,255,sat)); break;
        case 27: r[i]=uint8_t(clamp(int(a[i])-int(b[i]),0,255,sat)); break;
        case 30: r[i]=uint8_t((a[i]&uint8_t(~old[i])) | (b[i]&old[i])); break;
        }
    }
    for(int i=0;i<4;++i) {
        if(t.op==13) putWord(r,i,uint32_t(clamp(sign32(word(a,i))+sign32(word(b,i)),-2147483648ll,2147483647ll,sat)));
        if(t.op==25) putWord(r,i,uint32_t(clamp(int64_t(word(a,i))+word(b,i),0,4294967295ll,sat)));
        if(t.op==26) putWord(r,i,uint32_t(clamp(sign32(word(a,i))-sign32(word(b,i)),-2147483648ll,2147483647ll,sat)));
        if(t.op==20) putWord(r,i,sign32(word(a,i))>sign32(word(b,i))?0xffffffffu:0);
        if(t.op==21) putWord(r,i,word(a,i)>word(b,i)?0xffffffffu:0);
    }
    if(t.op==14 || t.op==15) {
        for(int i=0;i<8;++i) {
            auto& src=i<4?a:b;
            put(r,i,uint16_t(clamp(sign32(word(src,i%4)),t.op==14?-32768:0,t.op==14?32767:65535,sat)));
        }
    }
    if(t.op==16 || t.op==17 || t.op==28) {
        for(int i=0;i<16;++i) {
            auto& src=i<8?a:b;
            int64_t v=t.op==17?half(src,i%8):sign16(half(src,i%8));
            r[i]=uint8_t(clamp(v,t.op==16?-128:0,t.op==16?127:255,sat));
        }
    }
    return r;
}
int main() {
    std::mt19937 random(0x41495242);
    alignas(32) uint8_t memory[32]{};
    size_t checks=0;
    const uint16_t edges[]={0,1,7,8,15,16,31,32,0x7fff,0x8000,0xfffe,0xffff};
    for(const auto& t:tests) {
        for(int iteration=0;iteration<1200;++iteration) {
            PPCContext ctx{};
            const uint32_t initialSAT[]={0,1,0x10000,0x10001};
            ctx.vscr.value=initialSAT[iteration%4];
            ctx.cr6.lt=1; ctx.cr6.gt=1; ctx.cr6.eq=0; ctx.cr6.so=1;
            PPCVRegister* registers[]={&ctx.v0,&ctx.v1,&ctx.v2,&ctx.v3};
            Guest source[4];
            for(int reg=0;reg<4;++reg) {
                for(int i=0;i<8;++i) {
                    auto value=iteration<144 ? edges[(iteration/(reg+1)+i+reg)%12] : uint16_t(random());
                    put(source[reg],i,value);
                }
            }
            if(iteration%11==0) source[2]=source[1];
            if(iteration%11==1) { source[1].fill(0); source[2].fill(255); }
            if(iteration%11==2) { source[1].fill(255); source[2].fill(0); }
            if(iteration%11==3) { source[1].fill(0); source[2].fill(0); }
            for(int reg=0;reg<4;++reg) {
                // XenonRecomp stores all 16 guest vector bytes in reverse order.
                auto* vr=registers[reg];
                for(int i=0;i<16;++i) vr->u8[15-i]=source[reg][i];
            }
            uint32_t expectedSAT=ctx.vscr.value;
            auto expected=oracle(t,source[t.a],source[t.b],source[t.d],expectedSAT);
            t.fn(ctx,memory);
            if(ctx.vscr.value!=expectedSAT) { std::cerr<<t.name<<" SAT mismatch\n"; return 2; }
            bool record=(t.op==18 || t.op==19 || t.op==20 || t.op==21 || t.op==29) && t.imm;
            auto all=std::all_of(expected.begin(),expected.end(),[](uint8_t v){return v==255;});
            auto none=std::all_of(expected.begin(),expected.end(),[](uint8_t v){return v==0;});
            if(ctx.cr6.lt!=(record?all:1) || ctx.cr6.gt!=(record?0:1) ||
               ctx.cr6.eq!=(record?none:0) || ctx.cr6.so!=(record?0:1)) {
                std::cerr<<t.name<<" CR6 mismatch\n"; return 3;
            }
            for(int reg=0;reg<4;++reg) {
                auto wanted=reg==t.d?expected:source[reg];
                auto* vr=registers[reg];
                for(int i=0;i<16;++i) {
                    if(vr->u8[15-i]!=wanted[i]) {
                        std::cerr<<t.name<<" iteration "<<iteration<<" register "<<reg<<" guest byte "<<i<<" mismatch\n";
                        return 1;
                    }
                }
            }
            ++checks;
        }
    }
    std::cout<<checks<<" generated instruction executions passed\n";
}
'''.replace('DECLARATIONS',declarations).replace('ROWS',rows)
(out/'harness.cpp').write_text(harness)
results = {}
for mode, flags in [('native',[]), ('portable',['-DSIMDE_NO_NATIVE'])]:
    exe=out/f'test_{mode}.exe'
    run([bin_dir/'clang++.exe','-std=c++17','-O2','-Wno-macro-redefined',
         '-DSIMDE_ENABLE_NATIVE_ALIASES', *flags,
         '-I'+str(root/'generated/ppc'), '-I'+str(upstream/'thirdparty/simde'),
         out/'translated.cpp',out/'harness.cpp','-o',exe])
    results[mode]=run([exe]).strip()
(out/'results.json').write_text(json.dumps(results,indent=2))
print(json.dumps(results,indent=2))
