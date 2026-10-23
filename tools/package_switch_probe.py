from pathlib import Path
import hashlib
import json
import shutil
import zipfile
import argparse
from generated_state import generated_fingerprint
root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser();parser.add_argument('--kind',choices=('cpu','memory','entry'),default='cpu');parser.add_argument('--entry-version',choices=('0.5','0.6','0.7','0.7.1'),help='Historical snapshot only; omit for the current stable install');options=parser.parse_args()
memory=options.kind=='memory'
entry=options.kind=='entry'
current=entry and options.entry_version is None
summary=root/'analysis'/('switch_entry_current_summary.json' if current else f'switch_entry_v{options.entry_version}_summary.json' if entry else 'switch_startup_probe_summary.json' if memory else 'switch_probe_summary.json')
report=json.loads(summary.read_text())
if report.get('hardware_observation'):raise RuntimeError('Preserve measured packages; build the current runtime to archive and replace it first.')
if report['generated_fingerprint']!=generated_fingerprint(root/'generated/ppc'):
    raise RuntimeError('Switch probe is stale')
for path,sha in report['sources_sha256'].items():
    if hashlib.sha256((root/path).read_bytes()).hexdigest()!=sha:raise RuntimeError('Changed probe source: '+path)
for path,record in report['artifacts'].items():
    if hashlib.sha256((root/path).read_bytes()).hexdigest()!=record['sha256']:raise RuntimeError('Changed artifact: '+path)
bundle=root/report.get('bundle_dir','dist/switch/airborne-nx-probe')
nro_name=report.get('nro_name','airborne-nx-probe.nro')
shutil.copyfile(root/'runtime/switch'/('README-airborne-nx.md' if current else 'README-vulkan-v0.7.1.md' if entry and options.entry_version=='0.7.1' else 'README-vulkan-v0.7.md' if entry and options.entry_version=='0.7' else 'README-vulkan-v0.6.md' if entry and options.entry_version=='0.6' else 'README-vulkan-entry.md' if entry else 'README-memory.md' if memory else 'README.md'),bundle/'README.txt')
files=[bundle/name for name in (nro_name,'image.bin','README.txt')]
if entry:files.append(bundle/'xex-header.bin')
(bundle/'SHA256SUMS.txt').write_text('\n'.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+p.name for p in files)+'\n',encoding='ascii')
files.append(bundle/'SHA256SUMS.txt')
archive=root/'dist'/('airborne-nx.zip' if current else f'airborne-nx-entry-v{options.entry_version}.zip' if entry else 'airborne-nx-startup-v0.3.zip' if memory else 'airborne-nx-probe.zip')
if archive.exists() and report.get('hardware_observation'):raise RuntimeError('Preserve hardware-tested package')
with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
    for p in files:z.write(p,p.relative_to(root/'dist').as_posix())
with zipfile.ZipFile(archive) as z:
    if z.testzip() is not None:raise RuntimeError('ZIP integrity check failed')
data=(bundle/nro_name).read_bytes()
if data[0x10:0x14]!=b'NRO0':raise RuntimeError('Invalid NRO header')
elf=(root/(report.get('elf') or next(p for p in report['artifacts'] if p.endswith('.elf')))).read_bytes()
if elf[:4]!=b'\x7fELF' or int.from_bytes(elf[18:20],'little')!=183:raise RuntimeError('Wrong ELF target')
report['package']=dict(path=str(archive.relative_to(root)),bytes=archive.stat().st_size,
    sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),nro_magic='NRO0',elf_machine='AArch64 (183)',
    status='READY_FOR_USER_HARDWARE_TEST_NOT_EXECUTED')
summary.write_text(json.dumps(report,indent=2))
print(f'Package ready: {archive}, {archive.stat().st_size} bytes. Hardware execution pending.')
