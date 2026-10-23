"""Validated, version-specific supplements to XenonAnalyse switch discovery."""
from pathlib import Path
import hashlib
import json
import struct
import tomllib

root=Path(__file__).resolve().parents[1]
expected='8050b225e8c4ec7dfeb5aea23cce3e4123163480d243b45c7b3f3da6f05d98e6'
assert hashlib.sha256((root/'extracted/base/default.xex').read_bytes()).hexdigest()==expected
image=(root/'analysis/image/image_loader_normalized.bin').read_bytes()
base=0x82000000
def read(addr,size):
    assert base<=addr and addr+size<=base+len(image)
    return image[addr-base:addr-base+size]
def word(addr): return struct.unpack('>I',read(addr,4))[0]
def verify(addr,words):
    assert read(addr,len(words)*4)==b''.join(struct.pack('>I',w) for w in words),hex(addr)

# Byte-offset dispatch; both direct callers guard (unsigned) r3 <= 11.
verify(0x828b6d00,[0x3d808210,0x398cf098,0x7c0c18ae,0x3d80828b,0x398c6d28,
                   0x7d8c0214,0x7d8903a6,0x60000000,0x60000000,0x4e800420])
verify(0x828b6f7c,[0x386bffff,0x2b03000b,0x41990010,0x4bfffd79])
verify(0x828b738c,[0x386affff,0xa3cb0006,0x8bab0005,0x2b03000b,0x41990010,0x4bfff961])
offsets=read(0x820ff098,12)
assert offsets==bytes(range(0,192,16))
first=[0x828b6d28+v for v in offsets]
assert all(any(word(label+offset)==0x4e800020 for offset in (4,8,12)) for label in first)

# Halfword-offset dispatch. r24 is defined by a four-bit mask, then checked <=15.
verify(0x82cdd25c,[0x5558a73e,0x2b18000f,0x41990078])
verify(0x82cdd594,[0x3d808209,0x398c4170,0x5700083c,0x7c0c022e,0x3d8082ce,
                   0x398cd090,0x7d8c0214,0x7d8903a6,0x60000000,0x4e800420])
second=[0x82cdd090+v for v in struct.unpack('>16H',read(0x82094170,32))]
assert all(0x82cdce80<=label<0x82cdd5cc and label%4==0 for label in second)
tables=[dict(base=0x828b6d00,r=3,labels=first),dict(base=0x82cdd594,r=24,labels=second)]
automatic=(root/'analysis/airborne_switch_tables.toml').read_text()
known={s['base'] for s in tomllib.loads(automatic)['switch']}
assert not any(t['base'] in known for t in tables)
extra='\n# Airborne baseline-specific tables: tools/recover_airborne_switches.py\n'
for t in tables:
    extra+=f'[[switch]]\nbase = 0x{t["base"]:08X}\nr = {t["r"]}\nlabels = [\n'
    extra+=''.join(f'    0x{a:08X},\n' for a in t['labels'])+']\n'
(root/'analysis/airborne_switch_tables_merged.toml').write_text(automatic+extra,encoding='utf-8')
(root/'analysis/recovered_switches.json').write_text(json.dumps(tables,indent=2))
print('Recovered 12-entry byte-offset and 16-entry halfword-offset switches')
