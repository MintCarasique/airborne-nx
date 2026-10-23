"""Scan all CookedXenon UE3 packages (decompressing with ue3_decompress.exe) for XDK shader containers.
Run: python -I tools/ue3_scan_shaders.py   -> rex/out/ue3_shaders/{containers,inventory.json}"""
import importlib.util, hashlib, json, mmap, os, struct, subprocess, sys, time, collections
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('egs', ROOT / 'tools/extract_game_shaders.py')
egs = importlib.util.module_from_spec(spec); spec.loader.exec_module(egs)
OUT = ROOT / 'rex/out/ue3_shaders'; (OUT / 'containers').mkdir(parents=True, exist_ok=True); (OUT / 'tmp').mkdir(exist_ok=True)
EXE = OUT / 'bin/ue3_decompress.exe'; TMP = OUT / 'tmp/pkg.bin'
t0 = time.time(); found = {}; stats = collections.Counter(); failures = []; first_err = {}
def scan(data, name):
    pos = 0
    while (pos := data.find(b'\x10\x2a\x11', pos)) >= 0:
        start = pos; pos += 1
        try: rec = egs.inspect_container(data, start)
        except (ValueError, struct.error, UnicodeError): continue
        raw = bytes(data[start:start + rec['bytes']]); h = hashlib.sha256(raw).hexdigest()
        if h in found: found[h]['origins'].append([name, start]); continue
        (OUT / 'containers' / (h + '.bin')).write_bytes(raw)
        found[h] = dict(sha256=h, stage=rec['stage'], bytes=rec['bytes'], virtual_bytes=rec['virtual_bytes'],
                        physical_bytes=rec['physical_bytes'], code_bytes=rec['code_bytes'], origins=[[name, start]])
for p in sorted((ROOT / 'extracted/base/MOHAGame/CookedXenon').glob('*.xxx')):
    if p.stat().st_size == 0: continue
    r = subprocess.run([str(EXE), str(p), str(TMP)], capture_output=True, text=True)
    if r.returncode == 3: stats['skipped_nonpackage'] += 1; continue
    if r.returncode == 4: stats['uncompressed'] += 1; src = p
    elif r.returncode == 0: stats['compressed'] += 1; src = TMP
    else:
        stats['failed'] += 1; failures.append([p.name, r.returncode, r.stderr.strip()]); first_err.setdefault(r.returncode, [p.name, r.stderr.strip()]); continue
    with src.open('rb') as s, mmap.mmap(s.fileno(), 0, access=mmap.ACCESS_READ) as m: scan(m, p.name)
    if src == TMP: TMP.unlink()
inv = dict(stats=dict(stats), failures=failures, unique_containers=len(found), seconds=round(time.time() - t0),
           note='origin offsets are into the decompressed package image for compressed packages',
           shaders=sorted(found.values(), key=lambda r: r['sha256']))
(OUT / 'inventory.json').write_text(json.dumps(inv, indent=1))
old = {f.stem for f in (ROOT / 'generated/shaders/containers').glob('*.bin')}
new = [s for s in found.values() if s['sha256'] not in old]
print(dict(stats), 'unique', len(found), 'old', len(old), 'new', len(new), collections.Counter(s['stage'] for s in new), 'old_missing', len(old - set(found)))
print('first errors', first_err, 'secs', round(time.time() - t0))
