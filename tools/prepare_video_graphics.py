"""Prepare private, pinned programs for the exercised opening movie draw."""
from pathlib import Path
import hashlib, struct

ROOT=Path(__file__).resolve().parents[1]

def prepare():
    codes=['07ffcff6703c3fda26cc9f36261ddaaf68c0e437db20fdc065e9bb85bc92039c',
           '95ca706397074dc718a1f8e56c53567165beaed5b08ae7cbd4ce6c19dd0ff009']
    programs=[('generated/shaders/video/vertex.spv','a9f1e8d2f1517c5432fdde623d773879ec4b188023bde102c4126350bf3108f2'),
              ('generated/shaders/spirv/89f26d6e64e88c93a6d02d94c95ec6f9cb32896da1e29b3b416d200335240d34.spv','2d7406266c37f65b5cd40876c1f4eda5cbd93d5c1922c5558ce5ad59b0c00c6d')]
    source='#pragma once\n#include <cstdint>\n'
    for i in range(2):
        for name,path,digest,endian in [(f'video_code_{i}',ROOT/f'generated/captures/video/stage{i}.bin',codes[i],'>'),
                                      (f'video_spirv_{i}',ROOT/programs[i][0],programs[i][1],'<')]:
            data=path.read_bytes()
            if hashlib.sha256(data).hexdigest()!=digest:raise RuntimeError('Movie shader changed: '+str(path))
            source+='static constexpr uint32_t '+name+'[]={'+','.join(hex(w) for w in struct.unpack(endian+str(len(data)//4)+'I',data))+'};\n'
    out=ROOT/'generated/host/video_graphics.h';out.write_text(source);return out

if __name__=='__main__':print(prepare())
