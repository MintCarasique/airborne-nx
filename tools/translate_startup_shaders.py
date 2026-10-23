"""Reconstruct bounded interfaces for straight-line XDK startup microcode.

Consumes our private runtime trace as data. This does not recover original
reflection or prove shader equivalence; unsupported CF/fetch/export forms fail.
All derived code is retained only under generated/.
"""
from pathlib import Path
import argparse, hashlib, json, os, re, struct, subprocess
from extract_game_shaders import inspect_container, ROOT
from translate_game_shaders import spirv_info

def describe(code,vertex):
    if not code or len(code)%12:raise ValueError('Invalid shader instruction length')
    words=struct.unpack('>'+str(len(code)//4)+'I',code)
    blocks=len(code)//12;limit=blocks;block=0;fetches={};exports=set();ended=False
    while block<limit:
        a,b,c=words[block*3:block*3+3]
        for cf in (a|((b&65535)<<32),(b>>16)|(c<<16)):
            op=cf>>44
            if op in (0,12):continue
            if op not in (1,2):raise ValueError('Startup shader requires complex CF: '+str(op))
            first,count,sequence=cf&4095,(cf>>12)&7,(cf>>16)&4095
            if first<=block or count>6 or first+count>blocks:raise ValueError('Invalid EXEC range')
            limit=min(limit,first);ended|=op==2
            for at in range(first,first+count):
                x,y,z=words[at*3:at*3+3]
                if sequence&(1<<((at-first)*2)):
                    if not vertex or x&31:raise ValueError('Unsupported texture/non-vertex fetch')
                    fetches[at]=dict(instruction=at,slot=((x>>20)&31)*3+((x>>25)&3),format=(y>>16)&63,
                        stride=z&255,offset=(z>>8)&0x7fffff,mini=bool(y&(1<<30)))
                elif x&0x8000:
                    for dest,mask in ((x&63,(x>>16)&15),((x>>8)&63,(x>>20)&15)):
                        if mask:exports.add(dest)
        block+=1
    if not ended:raise ValueError('No terminating EXEC')
    invalid_exports=any(d>=16 and d not in (62,63) for d in exports) if vertex else any(d>3 and d!=61 for d in exports)
    if invalid_exports:
        raise ValueError('Unsupported memory export')
    # Neutral POSITION semantics prevent texture-coordinate or packed-normal
    # heuristics in the upstream translator. Numeric decoding is a runtime task.
    return dict(vertex=vertex,fetches=list(fetches.values()),exports=sorted(exports),cf_blocks=limit)

def wrap(code,description):
    vertex=description['vertex'];fetches=description['fetches']
    interpolators=[d for d in description['exports'] if d<16] if vertex else []
    ct=bytearray(struct.pack('>I7I',76,28,0,0,1,28,0,0))
    ct+=struct.pack('>I4H2I',48,2,0,256 if vertex else 224,0,56,0)
    ct+=b'RawRegs\0'+struct.pack('>6HI',1,3,1,4,256 if vertex else 224,0,0)
    shader_at=36+len(ct)
    shader=bytearray(struct.pack('>6I',0,len(code),0,0xff00,0,len(interpolators)<<5))
    if vertex:
        shader+=struct.pack('>3I',0,len(fetches),0)
        shader+=b''.join(struct.pack('>I',f['instruction']) for f in fetches)
        shader+=b''.join(struct.pack('>I',(d<<8)|0x50|d) for d in interpolators)
    else:
        outputs=sum((1<<d) if d<4 else 16 for d in description['exports'])
        shader+=struct.pack('>2I',0,outputs)
    virtual=shader_at+len(shader)
    data=struct.pack('>9I',0x102a1100|int(vertex),virtual,len(code),0,36,0,shader_at,0,0)+ct+shader+code
    inspect_container(data)
    return data

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('trace',type=Path)
    parser.add_argument('--report',type=Path,default=ROOT/'analysis/startup_shader_translation.json')
    parser.add_argument('--output-dir',type=Path,default=ROOT/'generated/shaders/startup');args=parser.parse_args()
    trace=args.trace.read_bytes();text=trace.decode('utf-8-sig');captures={}
    for line in text.splitlines():
        m=re.fullmatch(r'GPU_SHADER_WORDS stage=([01]) offset=(\d+) ([a-fA-F0-9 ]+)',line)
        if not m:continue
        stage,offset=int(m[1]),int(m[2])
        if offset==0:captures[stage]=bytearray()
        if stage not in captures or offset*4!=len(captures[stage]):raise ValueError('Incomplete shader capture')
        captures[stage].extend(bytes.fromhex(m[3].replace(' ','')))
    llvm=ROOT/'tools/toolchain/llvm-mingw-20261020-ucrt-x86_64/bin'
    env=dict(os.environ,PATH=str(llvm)+os.pathsep+os.environ['PATH'])
    out=args.output_dir;out.mkdir(parents=True,exist_ok=True)
    xenos=ROOT/'tools/XenosRecomp';translator=ROOT/'generated/airborne-shader-translator/airborne-shader-translate.exe'
    results=[]
    for stage,code in sorted(captures.items()):
        description=describe(code,stage==0);digest=hashlib.sha256(code).hexdigest()
        source=out/(digest+'.bin');hlsl=out/(digest+'.hlsl');spv=out/(digest+'.spv')
        source.write_bytes(wrap(code,description));spv.unlink(missing_ok=True);hlsl.unlink(missing_ok=True)
        commands=[[str(translator),str(source),str(hlsl),str(xenos/'XenosRecomp/shader_common.h')],
            [str(xenos/'thirdparty/dxc-bin/bin/x64/dxc.exe'),'-spirv','-T','vs_6_0' if stage==0 else 'ps_6_0','-E','main','-HV','2021',
             '-fvk-use-dx-layout','-fspv-target-env=vulkan1.2','-all-resources-bound',*(['-fvk-invert-y'] if stage==0 else []),str(hlsl),'-Fo',str(spv)]]
        for command in commands:
            process=subprocess.run(command,env=env,capture_output=True,text=True,timeout=30)
            (out/(digest+'.log')).write_text(process.stdout+process.stderr)
            if process.returncode:raise RuntimeError(process.stderr[-3000:] or str(process.returncode))
        results.append(dict(code_sha256=digest,stage=stage,reconstructed_interface=description,spirv=spirv_info(spv)))
    report=dict(scope='Reconstructed startup shader interfaces; compile only, no semantic/rendering validation',trace_sha256=hashlib.sha256(trace).hexdigest(),results=results)
    args.report.write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2))
if __name__=='__main__':main()
