"""Stage this user's existing game content beside the new Switch runtime.

Copies ordinary files (no links back to the extracted source), verifies content
and emits a manifest. No updater, ISO or executable from the Xbox is required.
"""
from pathlib import Path
import hashlib,json,shutil

root=Path(__file__).resolve().parents[1]
source=root/'extracted/base/MOHAGame'
bundle=root/'dist/switch/airborne-nx'
target=bundle/'game/MOHAGame'
if not target.resolve().is_relative_to((root/'dist/switch').resolve()):raise RuntimeError('Invalid staging destination')
def digest(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()

records=[]
for i,path in enumerate(sorted(source.rglob('*'))):
    if not path.is_file():continue
    relative=path.relative_to(source);output=target/relative;sha=digest(path)
    if not output.exists() or output.stat().st_size!=path.stat().st_size or digest(output)!=sha:
        output.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,output)
        if digest(output)!=sha:raise RuntimeError('Copied content differs: '+str(relative))
    records.append(dict(path=output.relative_to(bundle).as_posix(),bytes=path.stat().st_size,sha256=sha))
    if len(records)%200==0:print(f'Verified {len(records)} game files',flush=True)
(bundle/'GAME_SHA256SUMS.txt').write_text(''.join(r['sha256']+'  '+r['path']+'\n' for r in records),encoding='utf-8')
report=dict(scope='Private local game content staged for one SD transfer; no hardware execution',files=len(records),bytes=sum(r['bytes'] for r in records),bundle=str(bundle.relative_to(root)),records=records)
(root/'analysis/switch_entry_content.json').write_text(json.dumps(report,indent=2)+'\n')
print(f'Staged and verified {report["files"]} files, {report["bytes"]} bytes: {bundle}')
