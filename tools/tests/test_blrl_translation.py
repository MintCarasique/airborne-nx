from pathlib import Path
import json
import os
import subprocess

root=Path(__file__).resolve().parents[2]
out=root/'analysis/instruction_tests'
bin_dir=root/'tools/toolchain/llvm-mingw-20261020-ucrt-x86_64/bin'
env=dict(os.environ,PATH=str(bin_dir)+os.pathsep+os.environ['PATH'])
def run(args):
    p=subprocess.run([str(x) for x in args],cwd=root,env=env,capture_output=True,text=True)
    if p.returncode:raise RuntimeError(f'{p.returncode}: '+p.stdout+p.stderr)
    return p.stdout.strip()
(out/'blrl_cases.txt').write_text('test_blrl 4e800021\n')
run([out/'emit.exe',out/'blrl_cases.txt',out/'blrl_translated.cpp'])
prefix='''#include <cstdint>
struct PPCContext;
void test_indirect(PPCContext&,uint8_t*,uint32_t);
#define PPC_CALL_INDIRECT_FUNC(x) test_indirect(ctx,base,(x))
'''
path=out/'blrl_translated.cpp';path.write_text(prefix+path.read_text())
harness=r'''
#include "ppc_context.h"
#include <iostream>
extern PPC_FUNC(test_blrl);
static uint32_t expected;static bool failed;static size_t calls;
void test_indirect(PPCContext& ctx,uint8_t*,uint32_t target){
    if(target!=expected || ctx.lr!=0x10004 || ctx.ctr.u64!=0xabcdef0123456789ull)failed=true;
    ++calls;ctx.r3.u64=target^0x1234;ctx.lr=0xfedcba9876543210ull;
}
int main(){
    alignas(32) uint8_t memory[32]{};
    for(unsigned i=0;i<4096;++i){
        PPCContext ctx{};ctx.lr=0x1234567800000000ull+i;
        ctx.ctr.u64=0xabcdef0123456789ull;expected=i&~3u;
        test_blrl(ctx,memory);
        if(failed || calls!=i+1 || ctx.r3.u64!=(expected^0x1234) || ctx.lr!=0xfedcba9876543210ull)return 1;
    }
    std::cout<<calls<<" generated BLRL executions passed\n";
}
'''
(out/'blrl_harness.cpp').write_text(harness)
exe=out/'test_blrl.exe'
run([bin_dir/'clang++.exe','-std=c++17','-O2','-Wno-macro-redefined','-DSIMDE_ENABLE_NATIVE_ALIASES',
    '-I'+str(root/'generated/ppc'),'-I'+str(root/'tools/XenonRecomp/thirdparty/simde'),
    path,out/'blrl_harness.cpp','-o',exe])
result=run([exe]);(out/'blrl_results.json').write_text(json.dumps({'result':result},indent=2));print(result)
