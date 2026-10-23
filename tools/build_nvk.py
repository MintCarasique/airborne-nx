"""Build pinned Mesa NVK for Horizon using WSL and the project's devkitA64.

Prerequisites: tools/mesa-switch at the pinned revision, WSL Meson 1.9.2,
Clang/LLVM 18, SPIRV-Tools, Rust 1.90.0 with aarch64-unknown-linux-gnu,
bindgen 0.72.1, cbindgen 0.29.2, Mako, PyYAML and native development libraries.
No SDK probe/NRO is emitted. The output is the game's Vulkan dependency.
"""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import shlex

ROOT=Path(__file__).resolve().parents[1]
REVISION='0ff52079ca9e9e837ed27eb95a8d239f439a1864'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs',type=int,default=6)
    options=parser.parse_args()
    mesa=ROOT/'tools/mesa-switch'
    revision=subprocess.check_output(['git','-C',str(mesa),'rev-parse','HEAD'],text=True).strip()
    if revision!=REVISION or subprocess.check_output(['git','-C',str(mesa),'status','--porcelain'],text=True).strip():
        raise RuntimeError('Mesa revision/clean checkout mismatch')
    linux_root=subprocess.check_output(['wsl','--exec','wslpath','-a',str(ROOT)],text=True).strip()
    sdk=linux_root+'/tools/toolchain/nx-sdk/opt/devkitpro'
    source=linux_root+'/tools/mesa-switch'
    native='/root/.cache/airborne-nvk-native'
    cross='/root/.cache/airborne-nvk-cross'
    out=ROOT/'generated/nvk';out.mkdir(exist_ok=True)
    linux_out=linux_root+'/generated/nvk'
    cross_file=(mesa/'switch_cross_file.txt').read_text().replace('/opt/devkitpro',sdk)
    cross_file=cross_file.replace('/usr/local/libexec/rustc',linux_out+'/rustc')
    cross_file=cross_file.replace('/usr/local/libexec/bindgen',linux_out+'/bindgen')
    cross_file=cross_file.replace(sdk+'/portlibs/switch/bin/aarch64-none-elf-pkg-config',linux_out+'/pkg-config')
    (out/'cross.ini').write_text(cross_file,encoding='utf-8',newline='\n')
    pkg='#!/bin/bash\nset -o pipefail\nexport PKG_CONFIG_LIBDIR='+shlex.quote(sdk+'/portlibs/switch/lib/pkgconfig')+'\nexport PKG_CONFIG_PATH= PKG_CONFIG_DIR= PKG_CONFIG_SYSROOT_DIR=\n/usr/bin/pkg-config --static "$@" | sed '+shlex.quote('s|/opt/devkitpro|'+sdk+'|g')+'\n'
    (out/'pkg-config').write_text(pkg,encoding='utf-8',newline='\n')
    for name,filename in [('rustc','rustc-switch-wrapper.sh'),('bindgen','bindgen-switch-wrapper.sh')]:
        wrapper=(mesa/filename).read_bytes().replace(b'\r\n',b'\n')
        if name=='rustc':
            # Meson 1.9 calls its compiler-only sanity input sanity.rs.
            wrapper=wrapper.replace(b'*sanitycheckrs.rs*)',b'*sanitycheckrs.rs*|*/sanity.rs)')
        (out/name).write_bytes(wrapper)
    (out/'bindgen-atomic-shim.h').write_bytes((mesa/'bindgen-atomic-shim.h').read_bytes())
    env=dict(PATH='/root/.cache/airborne-nvk-python/bin:/root/.cargo/bin:/usr/lib/llvm-18/bin:/usr/bin:/bin',
             DEVKITPRO=sdk,LLVM_CONFIG='/usr/bin/llvm-config-18',CC='clang',CXX='clang++',
             MESA_SWITCH_RUST_TARGET='aarch64-unknown-linux-gnu',LIBCLANG_PATH='/usr/lib/llvm-18/lib',
             RUSTUP_TOOLCHAIN='1.90.0')

    def run(arguments,label):
        with (ROOT/'analysis'/('nvk-'+label+'.log')).open('w',encoding='utf-8') as log:
            process=subprocess.run(['wsl','--exec','env',*[k+'='+v for k,v in env.items()],*arguments],stdout=log,stderr=subprocess.STDOUT)
        if process.returncode:raise RuntimeError('NVK '+label+' failed; see analysis/nvk-'+label+'.log')
    run(['chmod','+x',linux_out+'/pkg-config',linux_out+'/rustc',linux_out+'/bindgen'],'prepare')
    # Rust's prebuilt Linux std asks for these libraries; actual compatibility
    # symbols are implemented in Mesa's rust_switch_stubs.c, not in these archives.
    for name in ('dl','rt','util'):
        run([sdk+'/devkitA64/bin/aarch64-none-elf-ar','rcs',linux_out+'/lib'+name+'.a'],'compat-'+name)
    native_options=['-Dvulkan-drivers=','-Dgallium-drivers=','-Dshader-cache=true','-Dplatforms=',
                    '-Dglx=disabled','-Degl=disabled','-Dopengl=false','-Dgles1=disabled','-Dgles2=disabled',
                    '-Dtools=[]','-Dllvm=enabled','-Dmesa-clc=enabled','-Dprecomp-compiler=enabled','-Dinstall-mesa-clc=true']
    # --reconfigure preserves incremental builds and also initializes a new dir.
    run(['meson','setup','--reconfigure',native,source,*native_options],'native-configure')
    run(['ninja','-C',native,'-j',str(options.jobs),'src/compiler/clc/mesa_clc','src/compiler/spirv/vtn_bindgen2'],'native-build')
    env['PATH']=native+'/src/compiler/clc:'+native+'/src/compiler/spirv:'+env['PATH']
    env['LIBRARY_PATH']=linux_out
    cross_options=['--cross-file',linux_out+'/cross.ini','--buildtype=release','-Doptimization=1',
                   '-Db_lto=false','-Db_ndebug=true','-Dvulkan-drivers=nouveau','-Dgallium-drivers=nouveau',
                   '-Dgallium-rusticl=false','-Dshader-cache=true','-Dplatforms=switch','-Dglx=disabled',
                   '-Degl=disabled','-Dopengl=false','-Dgles1=disabled','-Dgles2=disabled','-Dllvm=disabled',
                   '-Dshared-glapi=disabled','-Dshared-llvm=disabled','-Dmesa-clc=system','-Dprecomp-compiler=system',
                   '-Dcpp_rtti=false']
    run(['meson','setup','--reconfigure',cross,source,*cross_options],'cross-configure')
    run(['ninja','-C',cross,'-j',str(options.jobs),'src/nouveau/vulkan/libvulkan.a'],'cross-build')
    run(['cp',cross+'/src/nouveau/vulkan/libvulkan.a',linux_out+'/libvulkan.a'],'stage')
    archive=out/'libvulkan.a'
    report=dict(scope='Horizon NVK dependency compiled only; no hardware execution',revision=revision,
                archive=dict(path=str(archive.relative_to(ROOT)),bytes=archive.stat().st_size,
                             sha256=hashlib.sha256(archive.read_bytes()).hexdigest()),
                source='https://github.com/danfromtico/mesa-switch',native_build=native,cross_build=cross,
                cross_config_sha256=hashlib.sha256((out/'cross.ini').read_bytes()).hexdigest(),
                script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    (ROOT/'analysis/nvk_build_summary.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print('Built Horizon Vulkan dependency:',archive)


if __name__=='__main__':main()
