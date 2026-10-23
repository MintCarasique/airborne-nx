"""Build the real guest-entry runtime; never execute it or overwrite old probes."""
from pathlib import Path
import hashlib
import json
import subprocess
import shutil
import argparse
import zipfile
from generated_state import generated_fingerprint

parser=argparse.ArgumentParser();parser.add_argument('--validate-only',action='store_true');options=parser.parse_args()
version='0.7.4'
root=Path(__file__).resolve().parents[1]
release_summary=root/'analysis/switch_entry_current_summary.json'
if not options.validate_only and release_summary.exists() and json.loads(release_summary.read_text()).get('hardware_observation'):
    previous=json.loads(release_summary.read_text());history=root/'dist/archive';history.mkdir(exist_ok=True)
    snapshot=history/(previous['build_id']+'.zip')
    if not snapshot.exists():
        paths=dict(previous['artifacts'])
        if previous.get('package'):paths[previous['package']['path']]=previous['package']
        for path,record in paths.items():
            if hashlib.sha256((root/path).read_bytes()).hexdigest()!=record['sha256']:raise RuntimeError('Measured artifact changed before archival: '+path)
        with zipfile.ZipFile(snapshot,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as saved:
            saved.writestr('build-summary.json',json.dumps(previous,indent=2))
            for path in paths:saved.write(root/path,Path(path).as_posix())
        print('Preserved measured build: '+str(snapshot))
    with zipfile.ZipFile(snapshot) as saved:
        if saved.testzip() is not None or json.loads(saved.read('build-summary.json'))!=previous:raise RuntimeError('Invalid measured-build archive')
build=json.loads((root/'analysis/switch_object_build_summary.json').read_text())
if build.get('subset') or build['failed'] or build.get('archive_exit_code',1):
    raise RuntimeError('A complete Switch CPU archive is required')
if build['generated_fingerprint']!=generated_fingerprint(root/'generated/ppc'):
    raise RuntimeError('Generated CPU code changed since archive build')
for path,digest in build['runtime_sources_sha256'].items():
    if hashlib.sha256((root/path).read_bytes()).hexdigest()!=digest:raise RuntimeError('Changed runtime dependency: '+path)
archive=root/build['archive']['path']
if hashlib.sha256(archive.read_bytes()).hexdigest()!=build['archive']['sha256']:
    raise RuntimeError('CPU archive checksum mismatch')
nvk_summary=root/'analysis/nvk_build_summary.json'
if not nvk_summary.exists():raise RuntimeError('Build the Horizon Vulkan dependency first: python tools/build_nvk.py')
nvk=json.loads(nvk_summary.read_text())
nvk_archive=root/nvk['archive']['path']
if hashlib.sha256(nvk_archive.read_bytes()).hexdigest()!=nvk['archive']['sha256']:
    raise RuntimeError('Horizon NVK archive checksum mismatch')
root_linux=subprocess.check_output(['wsl','--exec','wslpath','-a',str(root)],text=True).strip()
def linux(path):return root_linux+'/'+path.relative_to(root).as_posix()
sdk=root_linux+'/tools/toolchain/nx-sdk/opt/devkitpro'
prefix=['wsl','--exec','env','DEVKITPRO='+sdk]
out=root/'generated/switch-cpu'

subprocess.run(['python',str(root/'tools/prepare_host_runtime.py')],check=True)
manifest=json.loads((root/'analysis/runtime_manifest.json').read_text())
out=root/('generated/switch-entry-next' if options.validate_only else 'generated/switch-entry');out.mkdir(exist_ok=True)
bundle=root/'dist/switch/airborne-nx'
if not options.validate_only:bundle.mkdir(parents=True,exist_ok=True)
for filename in ('runtime_bindings.h','runtime_imports.cpp','startup_shader_profile.h','post_config_graphics.h','video_graphics.h'):
    shutil.copyfile(root/'generated/host'/filename,out/filename)
header=(root/'extracted/base/default.xex').read_bytes()[:0x4000]
pins='#pragma once\n#include <cstdint>\n'
for name,digest in (('image',manifest['image_sha256']),('header',hashlib.sha256(header).hexdigest())):
    pins+='static const uint8_t runtime_'+name+'_sha256[32]={'+','.join('0x'+digest[i:i+2] for i in range(0,64,2))+'};\n'
(out/'runtime_pins.h').write_text(pins)
sources=[root/'runtime/switch/entry_runtime.cpp',out/'runtime_imports.cpp',root/'runtime/switch/unwind_bridge.cpp',root/'runtime/gpu/vulkan_device.cpp',root/'runtime/switch/display_lifecycle.cpp']
dependencies=[*sources,*sorted((root/'runtime/switch').glob('*.h')),root/'runtime/common/startup_services.inc',
    *sorted((root/'runtime/host').glob('*.h')),*sorted((root/'runtime/gpu').glob('*.h')),*sorted((root/'runtime/gpu').glob('*.inc')),*sorted((root/'runtime/audio').glob('*.h')),*sorted((root/'runtime/io').glob('*.h')),*sorted((root/'runtime/net').glob('*.h')),
    root/'tools/mesa-switch/include/vulkan/vulkan_core.h',out/'runtime_bindings.h',out/'runtime_pins.h',out/'startup_shader_profile.h',out/'post_config_graphics.h',out/'video_graphics.h',root/'tools/build_switch_entry.py']
hashes={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
build_id=hashlib.sha256(json.dumps(dict(sources=hashes,archive=build['archive']['sha256'],nvk=nvk['archive']['sha256']),sort_keys=True).encode()).hexdigest()
flags=[f for f in build['flags'] if f!='-fno-exceptions']
flags+=['-fexceptions','-D_POSIX_C_SOURCE=200809L','-I'+linux(root/'generated/ppc'),'-I'+linux(root/'runtime/host'),'-I'+linux(out),'-DAIRBORNE_ENTRY_BUILD_ID="'+build_id+'"']
flags+=['-I'+linux(root/'tools/mesa-switch/include')]
log=root/'analysis/switch_entry_build.log';log.write_text('')
def run(arguments):
    process=subprocess.run([*prefix,*arguments],capture_output=True,text=True)
    with log.open('a',encoding='utf-8') as output:output.write(process.stdout+process.stderr)
    if process.returncode:raise RuntimeError(process.stderr[-7500:])
    return process.stdout
objects=[]
for index,source in enumerate(sources):
    target=out/f'unit{index}.o'
    unit_flags=flags if index!=2 else build['flags']+['-I'+linux(root/'generated/ppc')]
    run([sdk+'/devkitA64/bin/aarch64-none-elf-g++',*unit_flags,'-c',linux(source),'-o',linux(target)]);objects.append(target)
for path,digest in hashes.items():
    if hashlib.sha256((root/path).read_bytes()).hexdigest()!=digest:
        raise RuntimeError('Source changed while compiling; rebuild before recording artifacts: '+path)
elf=out/('airborne-nx-vulkan-dev.elf' if options.validate_only else 'airborne-nx.elf')
run([sdk+'/devkitA64/bin/aarch64-none-elf-g++',*flags,'-specs='+sdk+'/libnx/switch.specs',
    *[linux(p) for p in objects],linux(archive),linux(nvk_archive),'-L'+sdk+'/portlibs/switch/lib',
    '-lexpat','-lzstd','-lz','-L'+sdk+'/libnx/lib','-lnx','-Wl,--gc-sections',
    '-Wl,--wrap=viOpenDisplay,--wrap=viCloseDisplay','-o',linux(elf)])
for path,digest in hashes.items():
    if hashlib.sha256((root/path).read_bytes()).hexdigest()!=digest:
        raise RuntimeError('Source changed while linking; rebuild before exporting: '+path)
if options.validate_only:
    record=dict(scope='Vulkan runtime + Horizon NVK cross-compile/link only; no new NRO/package or target execution',build_id=build_id,nvk_archive_sha256=nvk['archive']['sha256'],
        sources_sha256=hashes,elf=dict(path=str(elf.relative_to(root)),sha256=hashlib.sha256(elf.read_bytes()).hexdigest(),bytes=elf.stat().st_size))
    (root/'analysis/switch_entry_next_build_summary.json').write_text(json.dumps(record,indent=2))
    print('Revised runtime compiled/linked; previous hardware-tested NRO and package retained.');raise SystemExit(0)
nacp=out/'entry.nacp';run([sdk+'/tools/bin/nacptool','--create','AirborneNX','AirborneNX',version,linux(nacp)])
nro=bundle/'airborne-nx.nro'
run([sdk+'/tools/bin/elf2nro',linux(elf),linux(nro),'--nacp='+linux(nacp)])
shutil.copyfile(root/'analysis/image/image_loader_normalized.bin',bundle/'image.bin')
(bundle/'xex-header.bin').write_bytes(header)
report=dict(scope='Real game entry with native Vulkan movie shaders, planar YUV uploads, RGBA EDRAM resolve and guest-buffer VI presentation; general menu rendering and XMA decoding incomplete; target execution pending',
    build_id=build_id,probe_kind='entry',probe_version=version,compiler=build['compiler'],generated_fingerprint=build['generated_fingerprint'],nvk_archive_sha256=nvk['archive']['sha256'],
    archive_sha256=build['archive']['sha256'],sources_sha256=hashes,
    bundle_dir=str(bundle.relative_to(root)),nro_name=nro.name,
    artifacts={str(p.relative_to(root)):dict(bytes=p.stat().st_size,sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in (nro,elf,bundle/'image.bin',bundle/'xex-header.bin')})
release_summary.write_text(json.dumps(report,indent=2))
print(f'Built {nro}, {nro.stat().st_size} bytes; not executed on Switch.')
