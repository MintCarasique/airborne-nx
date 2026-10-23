"""Evaluate the restricted passthrough program at the first startup draw.

Analysis only: no draw is retired and no raster/depth result is fabricated.
Rejects instructions outside the observed straight-line fetch/MAX subset.
"""
from pathlib import Path
import argparse, hashlib, json, math, re, struct
from extract_game_shaders import ROOT

def evaluate(code,vertex,buffer_words):
    words=struct.unpack('>'+str(len(code)//4)+'I',code)
    registers=[[0.0]*4 for _ in range(32)];registers[0][0]=float(vertex)
    exports={};limit=len(words)//3;block=0;stride=None;ended=False
    while block<limit and not ended:
        a,b,c=words[block*3:block*3+3]
        for cf in (a|((b&65535)<<32),(b>>16)|(c<<16)):
            op=cf>>44
            if op in (0,12):continue
            if op not in (1,2):raise ValueError('Unsupported shader control flow')
            first,count,sequence=cf&4095,(cf>>12)&7,(cf>>16)&4095
            if first<=block or count>6 or first+count>len(words)//3:raise ValueError('Invalid shader EXEC bounds')
            limit=min(limit,first)
            for index in range(count):
                x,y,z=words[(first+index)*3:(first+index)*3+3]
                if sequence&(1<<(index*2)):
                    src,dst=(x>>5)&63,(x>>12)&63
                    if x&31 or (x>>20)&127 or src or dst>=32 or x&(1<<11) or x&(1<<18) or y&(1<<31) or (y>>24)&63:
                        raise ValueError('Unsupported vertex fetch addressing/modifier')
                    if not y&(1<<30):stride=z&255
                    if not stride or stride!=7:raise ValueError('Unexpected vertex stride')
                    fmt=(y>>16)&63
                    if fmt not in (38,57):raise ValueError('Unsupported startup vertex format')
                    address=vertex*stride+((z>>8)&0x7fffff);components=4 if fmt==38 else 3
                    if address+components>len(buffer_words):raise ValueError('Vertex fetch outside captured buffer')
                    value=[struct.unpack('>f',struct.pack('>I',v))[0] for v in buffer_words[address:address+components]]
                    for component in range(4):
                        swizzle=(y>>(component*3))&7
                        if swizzle<components:registers[dst][component]=value[swizzle]
                        elif swizzle==4:registers[dst][component]=0.0
                        elif swizzle==5:registers[dst][component]=1.0
                        elif swizzle!=7:raise ValueError('Invalid fetch component')
                else:
                    mask=(x>>16)&15;scalar_mask=(x>>20)&15
                    if not mask and not scalar_mask:continue
                    source1,source2=(z>>16)&255,(z>>8)&255
                    if scalar_mask or (z>>24)&31!=2 or z>>30!=3 or source1!=source2 or source1>=32 or y or x&0x3000040:
                        raise ValueError('ALU is not an unmodified MAX register with itself')
                    value=registers[source1][:]
                    if not all(math.isfinite(v) for v in value):raise ValueError('Nonfinite vertex requires full shader semantics')
                    destination=x&63
                    target=exports.setdefault(destination,[None]*4) if x&0x8000 else registers[destination]
                    for component in range(4):
                        if mask&(1<<component):target[component]=value[component]
            if op==2:ended=True;break
        block+=1
    if not ended or 62 not in exports or None in exports[62]:raise ValueError('No complete vertex position export')
    return exports[62]

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('trace',type=Path);args=parser.parse_args()
    trace=args.trace.read_bytes();text=trace.decode('utf-8-sig');code=bytearray();registers={};vertices={};draw=[]
    for line in text.splitlines():
        m=re.fullmatch(r'GPU_SHADER_WORDS stage=0 offset=(\d+) ([a-fA-F0-9 ]+)',line)
        if m:
            if int(m[1])==0:code=bytearray()
            if len(code)!=int(m[1])*4:raise ValueError('Incomplete shader capture')
            code.extend(bytes.fromhex(m[2].replace(' ','')))
        m=re.fullmatch(r'GPU_DRAW_REGISTER index=([a-fA-F0-9]+) value=([a-fA-F0-9]+)',line)
        if m:registers[int(m[1],16)]=int(m[2],16)
        m=re.fullmatch(r'GPU_DRAW_VERTEX_WORD slot=0 word=(\d+) value=([a-fA-F0-9]+)',line)
        if m:vertices[int(m[1])]=int(m[2],16)
        m=re.fullmatch(r'GPU_DRAW_WORD index=(\d+) value=([a-fA-F0-9]+)',line)
        if m:draw.append(int(m[2],16))
    if len(draw)!=1 or draw[0]!=0x30088 or registers.get(0x2208)!=5:raise ValueError('Unexpected startup primitive/mode')
    buffer=[vertices[i] for i in range(len(vertices))]
    positions=[evaluate(code,i,buffer) for i in range(3)]
    control=registers[0x2200]
    report=dict(scope='Restricted shader evaluation and captured state, not raster/rendering validation',
        trace_sha256=hashlib.sha256(trace).hexdigest(),vertex_code_sha256=hashlib.sha256(code).hexdigest(),
        primitive='rectangle list, 3 auto-indexed vertices',vertex_positions=positions,
        surface_pitch=registers[0x2000]&16383,samples={0:1,1:2,2:4}.get((registers[0x2000]>>16)&3),
        depth_base_tiles=registers[0x2002]&2047,depth_format=(registers[0x2002]>>16)&1,
        depth_test=bool(control&2),depth_write=bool(control&4),depth_compare=(control>>4)&7,
        stencil_test=bool(control&1),stencil_reference=registers[0x210c]&255,
        stencil_zpass=(control>>14)&7,viewport_control=hex(registers[0x2206]),clip_control=hex(registers[0x2204]),
        next='Implement native depth/stencil attachment and rectangle rasterization with Xenos pixel-center, clipping and MSAA rules; do not bypass this draw.')
    (ROOT/'analysis/startup_depth_draw.json').write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2))
if __name__=='__main__':main()
