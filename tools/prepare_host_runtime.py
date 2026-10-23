"""Recover import slots and startup metadata from this pinned XEX/normalized image."""
from pathlib import Path
import hashlib
import json
import re
import struct

root=Path(__file__).resolve().parents[1]
image=(root/'analysis/image/image_loader_normalized.bin').read_bytes()
xex=(root/'extracted/base/default.xex').read_bytes()
if hashlib.sha256(xex).hexdigest()!='8050b225e8c4ec7dfeb5aea23cce3e4123163480d243b45c7b3f3da6f05d98e6':
    raise RuntimeError('Unexpected baseline XEX')
if hashlib.sha256(image).hexdigest()!='c9349a9389fbee7d2b838189df72433df901ef3918053241adbb3e6844961c48':
    raise RuntimeError('Unexpected normalized image; re-audit loader output before changing this pin')
u=lambda at:struct.unpack_from('>I',xex,at)[0]
headers={u(24+i*8):u(28+i*8) for i in range(u(20))}
at=headers[0x103ff]
strings=xex[at+12:at+12+u(at+4)]
names=[]
offset=0
for _ in range(u(at+8)):
    end=strings.index(0,offset)
    names.append(strings[offset:end].decode('ascii'))
    offset=(end+4)&~3
symbols={int(a,16):name for name,a,_ in (line.split('\t') for line in
    (root/'analysis/image/import_symbols.tsv').read_text().splitlines()[1:])}
at+=12+u(at+4)
records=[]
for library in names:
    table=root/'tools/XenonRecomp/XenonUtils/xbox'/('xboxkrnl_table.inc' if library=='xboxkrnl.exe' else 'xam_table.inc')
    exports={int(n,16):(name,kind) for n,name,kind in re.findall(
        r'XE_EXPORT\([^,]+,\s*(0x[\da-fA-F]+),\s*(\w+),\s*(\w+)\)',table.read_text())}
    count=struct.unpack_from('>H',xex,at+38)[0]
    name_index=struct.unpack_from('>H',xex,at+36)[0]
    if names[name_index]!=library or u(at)!=40+count*4:raise RuntimeError('Unexpected import library layout')
    pending=None
    for i in range(count):
        address=u(at+40+i*4)
        if address in symbols:
            if pending is None or pending['kind']!='function':raise RuntimeError('Thunk without function slot')
            if symbols[address]!='__imp__'+pending['name']:raise RuntimeError('Import name mismatch')
            pending['thunk']=address
            pending=None
            continue
        if pending and pending['kind']=='function':raise RuntimeError('Function slot has no thunk')
        data=struct.unpack_from('<I',image,address-0x82000000)[0]
        if data>>24:raise RuntimeError('Unexpected import encoding')
        name,kind=exports[data&0xffff]
        record=dict(library=library,name=name,ordinal=data&0xffff,slot=address,
                    kind='variable' if kind=='kVariable' else 'function')
        records.append(record)
        pending=record
    at+=40+count*4
    if pending and pending['kind']=='function':raise RuntimeError('Trailing function slot without thunk')
assert sum(r['kind']=='function' for r in records)==267
assert all('thunk' in r for r in records if r['kind']=='function')
assert sum(r['kind']=='variable' for r in records)==10
tls=dict(zip(('slot_count','raw_address','data_size','raw_size'),struct.unpack_from('>IIII',xex,headers[0x20104])))
report=dict(xex_sha256=hashlib.sha256(xex).hexdigest(),image_sha256=hashlib.sha256(image).hexdigest(),
    image_bytes=len(image),image_base=headers[0x10201],entry=headers[0x10100],
    stack_size=headers[0x20200],tls=tls,imports=records)
(root/'analysis/runtime_manifest.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
out=root/'generated/host';out.mkdir(exist_ok=True)
source='#pragma once\nstruct ImportBinding { const char* name; uint32_t slot, thunk; };\n'
source+='static const ImportBinding import_bindings[] = {\n'
for r in records:source+=f'{{"{r["name"]}",0x{r["slot"]:08x},0x{r.get("thunk",0):08x}}},\n'
source+='};\n'
(out/'runtime_bindings.h').write_text(source,encoding='utf-8')
source='#include "diagnostic.h"\n'
for r in records:
    if r['kind']=='function':source+=f'PPC_FUNC(__imp__{r["name"]}) {{ host_import("{r["name"]}",ctx,base); }}\n'
(out/'runtime_imports.cpp').write_text(source,encoding='utf-8')
print(f'Recovered {len(records)} imports; stack {report["stack_size"]} bytes; TLS {tls}')
from prepare_startup_graphics import prepare
prepare()
from prepare_point_graphics import prepare as prepare_point
prepare_point()
from prepare_video_graphics import prepare as prepare_video
prepare_video()
