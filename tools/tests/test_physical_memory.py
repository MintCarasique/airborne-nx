from pathlib import Path
import os
import subprocess
root=Path(__file__).resolve().parents[2]
out=root/'analysis/instruction_tests'
bin_dir=root/'tools/toolchain/llvm-mingw-20261020-ucrt-x86_64/bin'
env=dict(os.environ,PATH=str(bin_dir)+os.pathsep+os.environ['PATH'])
def run(args):
    p=subprocess.run([str(x) for x in args],env=env,cwd=root,capture_output=True,text=True)
    if p.returncode:raise RuntimeError(p.stdout+p.stderr)
    return p.stdout
cases={'lwz':'80640000','stw':'90640000','lvx':'7c2020ce','stvx':'7c2021ce',
       'lwarx':'7c602028','stwcx':'7c60212d','dcbz':'7c0027ec'}
(out/'physical_cases.txt').write_text(''.join(f'test_physical_{name} {opcode}\n' for name,opcode in cases.items()))
run([out/'emit.exe',out/'physical_cases.txt',out/'physical_translated.cpp'])
exe=out/'test_physical_memory.exe'
run([bin_dir/'clang++.exe','-std=c++17','-O1','-Wno-macro-redefined','-DSIMDE_ENABLE_NATIVE_ALIASES',
    '-I'+str(root/'generated/ppc'),'-I'+str(root/'runtime/host'),'-I'+str(root/'tools/XenonRecomp/thirdparty/simde'),
    root/'tools/tests/test_physical_memory.cpp',out/'physical_translated.cpp','-o',exe])
print(run([exe]).strip())
