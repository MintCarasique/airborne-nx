"""Prepare exact private shader fingerprints for the restricted startup clear."""
import hashlib,json,struct
from pathlib import Path
from extract_game_shaders import ROOT,inspect_container

def prepare():
    report=json.loads((ROOT/'analysis/startup_shader_translation.json').read_text())
    source='#pragma once\n#include <cstdint>\n'
    for record in report['results']:
        digest=record['code_sha256'];stage=record['stage']
        expected={0:'93488cb9a7bbbb2f0a8bc9cf9cc6b4111102ccaba9e76d0a16ef65184ea0402b',1:'4913603d899eb3d5c8f5b3e2fa918ffb461320222f4748b233983ad8a2c98e25'}
        if expected.get(stage)!=digest:raise RuntimeError('Startup clear proof does not cover this shader')
        path=ROOT/'generated/shaders/startup'/(digest+'.bin');data=path.read_bytes()
        info=inspect_container(data);start=info['virtual_bytes']+info['code_offset'];code=data[start:start+info['code_bytes']]
        if hashlib.sha256(code).hexdigest()!=digest:raise RuntimeError('Startup code checksum mismatch')
        words=struct.unpack('>'+str(len(code)//4)+'I',code)
        source+=f'static constexpr uint32_t startup_shader_{stage}[]={{'+','.join(hex(w) for w in words)+'};\n'
    resolve=(ROOT/'generated/shaders/startup/resolve-vertex-code.bin').read_bytes()
    if hashlib.sha256(resolve).hexdigest()!='586168ec589613862294dae90f866303312abb8756318fa8d8633c8562a83cc0':
        raise RuntimeError('Startup resolve proof does not cover this shader')
    words=struct.unpack('>'+str(len(resolve)//4)+'I',resolve)
    source+='static constexpr uint32_t startup_resolve_vertex[]={'+','.join(hex(w) for w in words)+'};\n'
    out=ROOT/'generated/host/startup_shader_profile.h';out.write_text(source)
    return out
if __name__=='__main__':print(prepare())
