"""Describe the actually bound pointer-loaded video shaders and private planes.

Never infer active programs from older immediate loads. Reconstruct the small
vertex interface; retain the pixel shader's original XDK sampler reflection.
Compilation and source capture do not demonstrate native video presentation.
"""
from pathlib import Path
import sys, re, json, hashlib, struct, subprocess, os
from extract_game_shaders import ROOT, inspect_container
from translate_startup_shaders import describe, wrap
from translate_game_shaders import spirv_info

def main():
    capture=ROOT/'generated/captures/video';trace=ROOT/'analysis/host_probe_entry.log';data=json.loads((capture/'state.json').read_text());regs=data['registers']
    active={};vertices={}
    for line in trace.read_text().splitlines():
        m=re.fullmatch(r'GPU_SHADER_WORDS stage=([01]) offset=(\d+) ([a-fA-F0-9 ]+)',line)
        if m:
            stage,offset=int(m[1]),int(m[2]);words=[int(x,16) for x in m[3].split()]
            if offset==0:active[stage]=[]
            if len(active[stage])!=offset:raise RuntimeError('Incomplete active shader trace')
            active[stage]+=words
        m=re.fullmatch(r'GPU_DRAW_VERTEX_WORD slot=95 word=(\d+) value=([a-fA-F0-9]+)',line)
        if m:vertices[int(m[1])]=int(m[2],16)
    codes=[(capture/f'stage{i}.bin').read_bytes() for i in range(2)]
    for i,code in enumerate(codes):
        if code!=struct.pack('>'+str(len(active[i]))+'I',*active[i]):raise RuntimeError('Capture/active shader mismatch')
    if set(vertices)!=set(range(30)):raise RuntimeError('Incomplete 6-vertex capture')
    (capture/'vertices.bin').write_bytes(struct.pack('<30I',*(vertices[i] for i in range(30))))
    hashes=[hashlib.sha256(code).hexdigest() for code in codes]
    if hashes!=['07ffcff6703c3fda26cc9f36261ddaaf68c0e437db20fdc065e9bb85bc92039c','95ca706397074dc718a1f8e56c53567165beaed5b08ae7cbd4ce6c19dd0ff009']:raise RuntimeError('Unrecognized video programs')
    description=describe(codes[0],True);folder=ROOT/'generated/shaders/video';folder.mkdir(exist_ok=True)
    source=folder/'vertex.bin';hlsl=folder/'vertex.hlsl';spv=folder/'vertex.spv';source.write_bytes(wrap(codes[0],description))
    llvm=ROOT/'tools/toolchain/llvm-mingw-20261020-ucrt-x86_64/bin';env=dict(os.environ,PATH=str(llvm)+os.pathsep+os.environ['PATH'])
    xenos=ROOT/'tools/XenosRecomp'
    commands=[[str(ROOT/'generated/airborne-shader-translator/airborne-shader-translate.exe'),str(source),str(hlsl),str(xenos/'XenosRecomp/shader_common.h')],
        [str(xenos/'thirdparty/dxc-bin/bin/x64/dxc.exe'),'-spirv','-T','vs_6_0','-E','main','-HV','2021','-fvk-use-dx-layout','-fspv-target-env=vulkan1.2','-all-resources-bound','-fvk-invert-y',str(hlsl),'-Fo',str(spv)]]
    for command in commands:
        result=subprocess.run(command,env=env,capture_output=True,text=True,timeout=30)
        if result.returncode:raise RuntimeError(result.stderr or result.stdout)
    inventory=json.loads((ROOT/'analysis/game_shader_inventory.json').read_text())['shaders']
    pixel=next(record for record in inventory if record['code_sha256']==hashes[1])
    packed=(ROOT/pixel['path']).read_bytes();info=inspect_container(packed);start=info['virtual_bytes']+info['code_offset']
    if packed[start:start+info['code_bytes']]!=codes[1]:raise RuntimeError('Original reflected pixel shader does not match active code')
    translations=json.loads((ROOT/'analysis/game_shader_translation.json').read_text())['results']
    pixel_translation=next(record for record in translations if record['sha256']==pixel['sha256'])
    if pixel_translation['status']!='COMPILED':raise RuntimeError('Original pixel shader did not compile')
    pixel_spv=ROOT/pixel_translation['path']
    if spirv_info(pixel_spv)['sha256']!=pixel_translation['spirv']['sha256']:raise RuntimeError('Pixel SPIR-V changed')
    for path in (spv,pixel_spv):
        linux='/mnt/'+path.drive[0].lower()+path.as_posix()[2:]
        result=subprocess.run(['wsl','--exec','spirv-val','--target-env','vulkan1.2',linux],capture_output=True,text=True)
        if result.returncode:raise RuntimeError(result.stderr)
    planes=[]
    for i in range(3):
        fetch=regs[0x4800+6*i:0x4806+6*i];width=(fetch[2]&8191)+1;height=((fetch[2]>>13)&8191)+1;pitch=((fetch[0]>>22)&511)*32
        raw=(capture/f'plane{i}.bin').read_bytes()
        if len(raw)!=pitch*height or width>pitch:raise RuntimeError('Invalid plane capture extent')
        active_bytes=b''.join(raw[row*pitch:row*pitch+width] for row in range(height))
        planes.append(dict(plane=i,width=width,height=height,pitch=pitch,bytes=len(raw),sha256=hashlib.sha256(raw).hexdigest(),active_min=min(active_bytes),active_max=max(active_bytes),format=fetch[1]&63,tiled=bool(fetch[0]>>31),endian=(fetch[1]>>6)&3))
    report=dict(scope='Captured actual YUV draw data and active shaders; compile/validation only, no video rendering/presentation claim',trace_sha256=hashlib.sha256(trace.read_bytes()).hexdigest(),initiator=data['initiator'],
        vertex=dict(code_sha256=hashes[0],interface=description,spirv=spirv_info(spv),spirv_validation_exit=0),
        pixel=dict(code_sha256=hashes[1],original_container_sha256=pixel['sha256'],constants=pixel['constants'],spirv=pixel_translation['spirv'],spirv_validation_exit=0),planes=planes,
        vertices=[[struct.unpack('>f',struct.pack('>I',vertices[i*5+j]))[0] for j in range(5)] for i in range(6)])
    (ROOT/'analysis/video_draw_analysis.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))

if __name__=='__main__':main()
