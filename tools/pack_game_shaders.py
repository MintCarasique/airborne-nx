"""Package successful translations with their original lookup keys for runtime use.

Partial coverage is explicit in the file header; a missing shader is never
replaced with another shader. Private derived data stays in generated/.
"""
from pathlib import Path
import hashlib
import json
import struct
import zlib
from extract_game_shaders import ROOT


def key64(data):
    value = 0xcbf29ce484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    return value


def main():
    inventory_path = ROOT/'analysis/game_shader_inventory.json'
    translation_path = ROOT/'analysis/game_shader_translation.json'
    inventory = json.loads(inventory_path.read_text())
    translated = json.loads(translation_path.read_text())
    sha = lambda b: hashlib.sha256(b).hexdigest()
    if translated['inventory_sha256'] != sha(inventory_path.read_bytes()):
        raise RuntimeError('Shader inventory changed since translation')
    originals = {r['sha256']:r for r in inventory['shaders']}
    payload, count = bytearray(), 0
    for record in translated['results']:
        if record['status'] != 'COMPILED':
            continue
        source = originals[record['sha256']]
        original = (ROOT/source['path']).read_bytes()
        spirv = (ROOT/record['path']).read_bytes()
        if sha(original) != record['sha256'] or sha(spirv) != record['spirv']['sha256']:
            raise RuntimeError('Stale shader artifact: '+record['sha256'])
        payload += struct.pack('<Q6I', key64(original),len(original),len(spirv),
                               source['stage']=='vertex',record['spec_constants_mask'],
                               zlib.crc32(original),zlib.crc32(spirv))
        payload += original
        payload += bytes((-len(original)) % 4)
        payload += spirv
        count += 1
    header = struct.pack('<8s6I',b'AIRSPV01',1,32+len(payload),count,len(originals),zlib.crc32(payload),0)
    target = ROOT/'generated/shaders/airborne-shaders.bin'
    target.write_bytes(header+payload)
    report = dict(path=str(target.relative_to(ROOT)),bytes=target.stat().st_size,sha256=sha(target.read_bytes()),
                  compiled=count,corpus=len(originals),coverage='PARTIAL' if count<len(originals) else 'SCANNED_CORPUS_ONLY',
                  translation_sha256=sha(translation_path.read_bytes()),tool_sha256=sha(Path(__file__).read_bytes()))
    (ROOT/'analysis/game_shader_pack.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(f'Runtime shader library: {count}/{len(originals)} entries, {target.stat().st_size} bytes, {report["coverage"]}')


if __name__ == '__main__':
    main()
