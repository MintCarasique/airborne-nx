"""Inspect XEX metadata and verify GOD block hashes without changing input."""
from pathlib import Path
from collections import Counter
import hashlib
import json
import struct

root = Path(__file__).resolve().parents[1]
game = root / 'extracted/base'
blob = (game / 'default.xex').read_bytes()
u32 = lambda o: struct.unpack_from('>I', blob, o)[0]
u16 = lambda o: struct.unpack_from('>H', blob, o)[0]
headers = {u32(24+i*8): u32(28+i*8) for i in range(u32(20))}
version = lambda v: f'{v >> 28}.{(v >> 24) & 15}.{(v >> 8) & 65535}.{v & 255}'
execution = headers[0x40006]
fmt = headers[0x3ff]
sec = u32(16)
report = {
    'xex': {
        'magic': blob[:4].decode(), 'size': len(blob),
        'sha256': hashlib.sha256(blob).hexdigest(),
        'title_id': f'{u32(execution+12):08X}',
        'media_id': f'{u32(execution):08X}',
        'version': version(u32(execution+4)),
        'base_version': version(u32(execution+8)),
        'entry_point': f'0x{headers[0x10100]:08X}',
        'image_base': f'0x{headers[0x10201]:08X}',
        'image_size': u32(sec+4), 'header_size': u32(8),
        'encryption_type': u16(fmt+4), 'compression_type': u16(fmt+6),
        'original_pe_name': blob[headers[0x183ff]+4:headers[0x183ff]+u32(headers[0x183ff])].split(b'\0')[0].decode(),
    }
}
o = headers[0x200ff]
report['static_libraries'] = []
for at in range(o+4, o+u32(o), 16):
    report['static_libraries'].append({'name': blob[at:at+8].rstrip(b'\0').decode(),
      'version': f'{u16(at+8)}.{u16(at+10)}.{u16(at+12)}.{blob[at+15]}'})
o = headers[0x103ff]
string_size = u32(o+4)
strings = blob[o+12:o+12+string_size]
names = []
at = 0
while at < len(strings):
    end = strings.index(0, at)
    names.append(strings[at:end].decode())
    at = (end+4) & ~3
at = o+12+string_size
report['import_libraries'] = []
for _ in range(u32(o+8)):
    report['import_libraries'].append({'name': names[u16(at+36)],
      'record_count': u16(at+38), 'version': version(u32(at+28)),
      'minimum_version': version(u32(at+32))})
    at += u32(at)

manifest = json.loads((root/'analysis/files.json').read_text())
for entry in manifest:
    path = game/entry['path']
    assert path.stat().st_size == entry['size'], path
    with path.open('rb') as stream:
        assert hashlib.file_digest(stream, 'sha256').hexdigest() == entry['sha256'], path
report['files'] = {'count': len(manifest), 'total_bytes': sum(e['size'] for e in manifest),
    'extensions': dict(Counter(Path(e['path']).suffix.lower() for e in manifest)),
    'extracted_sha256_recheck': 'passed'}

tu = next(root.glob('**/TU/TU_*'))
data = tu.read_bytes()
be = lambda o: struct.unpack_from('>I', data, o)[0]
report['title_update'] = {'path': str(tu.relative_to(root)),
    'sha256': hashlib.sha256(data).hexdigest(), 'media_id': f'{be(0x354):08X}',
    'version': version(be(0x358)), 'base_version': version(be(0x35c)),
    'title_id': f'{be(0x360):08X}', 'applied': False}

parts = sorted(root.glob('**/*.data/Data*'))
checked_blocks = checked_tables = 0
errors = []
for part in parts:
    with part.open('rb') as stream:
        master = stream.read(0x1000)
        group = 0
        while table := stream.read(0x1000):
            if len(table) != 0x1000:
                errors.append(f'{part.name}: incomplete table {group}')
                break
            if hashlib.sha1(table).digest() != master[group*20:group*20+20]:
                errors.append(f'{part.name}: table hash {group}')
            checked_tables += 1
            payload = stream.read(0xcc000)
            for i in range(0, len(payload), 0x1000):
                block = payload[i:i+0x1000]
                j = i//0x1000
                if len(block) != 0x1000 or hashlib.sha1(block).digest() != table[j*20:j*20+20]:
                    errors.append(f'{part.name}: data hash {group}/{j}')
                checked_blocks += 1
            group += 1
    print(f'Verified {part.name}', flush=True)
report['god_integrity'] = {'parts': len(parts), 'data_blocks_checked': checked_blocks,
    'hash_tables_checked': checked_tables, 'errors': errors,
    'scope': 'Data blocks against group tables; group tables against part master. Package signature and master-chain not validated.'}
(root/'analysis/audit.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps(report, indent=2))
