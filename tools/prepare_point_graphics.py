"""Pin the exercised fixed-position point programs and prepare private SPIR-V.

The Xenos rasterizer supplies the 1-pixel point size; Vulkan needs it exported
by the vertex stage. No game shader code is placed in tracked runtime sources.
"""
from pathlib import Path
import hashlib, json, struct, subprocess, os
from extract_game_shaders import inspect_container, ROOT

def prepare():
    report=json.loads((ROOT/'analysis/post_config_shader_translation.json').read_text())
    expected={0:'099625f3ea15a92e74e525503b3e41302fc268bc8845da6100c991f67321e4e3',1:'4913603d899eb3d5c8f5b3e2fa918ffb461320222f4748b233983ad8a2c98e25'}
    folder=ROOT/'generated/shaders/post-config';out=ROOT/'generated/host/post_config_graphics.h'
    source='#pragma once\n#include <cstdint>\n';records=[]
    for record in report['results']:
        stage=record['stage'];digest=record['code_sha256']
        if expected.get(stage)!=digest:raise RuntimeError('Point proof does not cover shader')
        data=(folder/(digest+'.bin')).read_bytes();info=inspect_container(data)
        at=info['virtual_bytes']+info['code_offset'];code=data[at:at+info['code_bytes']]
        if hashlib.sha256(code).hexdigest()!=digest:raise RuntimeError('Point microcode changed')
        spv=folder/(digest+'.spv')
        if stage==0:
            hlsl=(folder/(digest+'.hlsl')).read_text()
            if hlsl.count('void main(')!=1 or hlsl.count('oPos.xy += g_HalfPixelOffset * oPos.w;')!=1:raise RuntimeError('Unexpected vertex wrapper')
            hlsl=hlsl.replace('void main(','void main(\n\tout float oPointSize : PSIZE,',1)
            hlsl=hlsl.replace('oPos.xy += g_HalfPixelOffset * oPos.w;','oPos.xy += g_HalfPixelOffset * oPos.w;\n\toPointSize = 1.0;')
            adapted=folder/'point-size-one.hlsl';adapted.write_text(hlsl);spv=folder/'point-size-one.spv'
            command=[str(ROOT/'tools/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxc.exe'),'-spirv','-T','vs_6_0','-E','main','-HV','2021','-fvk-use-dx-layout','-fspv-target-env=vulkan1.2','-all-resources-bound','-fvk-invert-y',str(adapted),'-Fo',str(spv)]
            subprocess.run(command,check=True,capture_output=True)
        binary=spv.read_bytes()
        linux='/mnt/'+spv.drive[0].lower()+spv.as_posix()[2:]
        validated=subprocess.run(['wsl','--exec','spirv-val','--target-env','vulkan1.2',linux],capture_output=True,text=True)
        if validated.returncode:raise RuntimeError(validated.stderr)
        for name,blob,order in ((f'point_code_{stage}',code,'>'),(f'point_spirv_{stage}',binary,'<')):
            source+='static constexpr uint32_t '+name+'[]={'+','.join(hex(w) for w in struct.unpack(order+str(len(blob)//4)+'I',blob))+'};\n'
        records.append(dict(stage=stage,code_sha256=digest,spirv_sha256=hashlib.sha256(binary).hexdigest(),spirv_bytes=len(binary),point_size_export=stage==0,spirv_validation_exit=0))
    if len(records)!=2:raise RuntimeError('Both point stages required')
    out.write_text(source)
    (ROOT/'analysis/point_pipeline_shaders.json').write_text(json.dumps(dict(scope='Exact captured point shader pair, vertex point-size export supplied from raster state; compilation/validation only',results=records),indent=2)+'\n')
    return out
if __name__=='__main__':print(prepare())
