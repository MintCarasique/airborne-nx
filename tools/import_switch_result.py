"""Record a user-supplied CPU probe log as evidence, without executing its content."""
from pathlib import Path
import argparse
import hashlib
import json
import re

root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser();parser.add_argument('result',type=Path);args=parser.parse_args()
data=args.result.read_bytes()
decoding_error=None
try:text=data.decode('utf-8-sig')
except UnicodeDecodeError as error:
    decoding_error=dict(first_invalid_byte=error.start,reason=error.reason,nul_offsets=[i for i,b in enumerate(data) if b==0])
    # Retain raw bytes below. Escapes permit partial evidence extraction without
    # inventing missing lines or silently turning damaged text into a pass.
    text=data.decode('utf-8-sig',errors='backslashreplace')
text=text.replace('\r\n','\n')
if text.startswith(('AirborneNX ARM64 game entry v0.5\n','AirborneNX ARM64 game entry v0.6\n')):
    from switch_result_format import parse_vulkan_entry_result
    version=re.match(r'AirborneNX ARM64 game entry v(0\.[56])\n',text)[1]
    summary=root/'analysis'/f'switch_entry_v{version}_summary.json'
    build=json.loads(summary.read_text()) if summary.exists() else {}
    report=parse_vulkan_entry_result(text,build.get('build_id'))
    if decoding_error:
        report.update(log_integrity='CORRUPTED_TEXT',decoding_error=decoding_error,parsed_status=report['status'],status='USER_REPORTED_PARTIAL_EVIDENCE_CORRUPTED_LOG')
        report['limits']+=' Raw bytes contain invalid UTF-8; missing/corrupted lines were not reconstructed. Individual intact stage records remain partial evidence.'
    directory=root/'analysis/hardware';directory.mkdir(exist_ok=True)
    digest=hashlib.sha256(data).hexdigest();destination=directory/(f'switch_entry_v{version}_'+digest[:12]+'.txt')
    destination.write_bytes(data)
    report.update(log=str(destination.relative_to(root)),log_sha256=digest,source='User-supplied log')
    result_path=directory/f'switch_entry_v{version}_result.json'
    result_path.write_text(json.dumps(report,indent=2),encoding='utf-8')
    if report['matches_known_build']:
        build['hardware_observation']=dict(status=report['status'],log=report['log'],log_sha256=digest,
            result=str(result_path.relative_to(root)))
        build['package']['status']=report['status']
        summary.write_text(json.dumps(build,indent=2),encoding='utf-8')
    print(json.dumps(report,indent=2));raise SystemExit(0)
if text.startswith('AirborneNX ARM64 game entry v0.4\n'):
    from switch_result_format import parse_entry_result
    summary=root/'analysis/switch_entry_summary.json'
    known=json.loads(summary.read_text()).get('build_id') if summary.exists() else None
    report=parse_entry_result(text,known)
    directory=root/'analysis/hardware';directory.mkdir(exist_ok=True)
    digest=hashlib.sha256(data).hexdigest();destination=directory/('switch_entry_v0.4_'+digest[:12]+'.txt')
    destination.write_bytes(data)
    report.update(log=str(destination.relative_to(root)),log_sha256=digest,source='User-supplied log')
    (directory/'switch_entry_result.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps(report,indent=2));raise SystemExit(0)
if text.startswith('AirborneNX ARM64 startup probe v0.3\n'):
    from switch_result_format import parse_startup_result
    summary=root/'analysis/switch_startup_probe_summary.json'
    known=json.loads(summary.read_text()).get('build_id') if summary.exists() else None
    report=parse_startup_result(text,known)
    directory=root/'analysis/hardware';directory.mkdir(exist_ok=True)
    digest=hashlib.sha256(data).hexdigest();destination=directory/('switch_startup_v0.3_'+digest[:12]+'.txt')
    destination.write_bytes(data)
    report.update(log=str(destination.relative_to(root)),log_sha256=digest,source='User-supplied log')
    (directory/'switch_startup_probe_result.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps(report,indent=2));raise SystemExit(0)
if text.startswith('AirborneNX ARM64 memory probe v0.2\n'):
    from switch_result_format import parse_memory_result
    summary=root/'analysis/switch_memory_probe_summary.json'
    known=json.loads(summary.read_text()).get('build_id') if summary.exists() else None
    report=parse_memory_result(text,known)
    directory=root/'analysis/hardware';directory.mkdir(exist_ok=True)
    digest=hashlib.sha256(data).hexdigest();destination=directory/('switch_memory_v0.2_'+digest[:12]+'.txt')
    destination.write_bytes(data)
    report.update(log=str(destination.relative_to(root)),log_sha256=digest,source='User-supplied log')
    (directory/'switch_memory_probe_result.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps(report,indent=2));raise SystemExit(0)
version=re.search(r'^AirborneNX ARM64 CPU probe v([\d.]+)$',text,re.M)
environment=re.search(r'^HOS=([0-9a-fA-F]{8}) applet_type=(\d+)$',text,re.M)
leaf=re.search(r'^PASS real guest function 828B6D00: (\d+) calls$',text,re.M)
half=re.search(r'^PASS FLOAT16_2: (\d+) generated executions$',text,re.M)
clock=re.search(r'^PASS timebase freq=(\d+) delta=(\d+)$',text,re.M)
passed=bool(version and environment and leaf and half and clock and
    version[1]=='0.1' and int(leaf[1])==768 and int(half[1])==1138867 and
    int(clock[1])==50000000 and int(clock[2])>0 and
    'PASS image SHA256' in text.splitlines() and text.splitlines()[-1:] == ['RESULT=PASS'])
if not passed:raise RuntimeError('Log does not report all expected v0.1 checks passing; inspect manually')
directory=root/'analysis/hardware';directory.mkdir(exist_ok=True)
digest=hashlib.sha256(data).hexdigest();destination=directory/('switch_probe_v0.1_'+digest[:12]+'.txt')
destination.write_bytes(data)
hos=int(environment[1],16)
report=dict(status='USER_REPORTED_HARDWARE_PASS',probe_version=version[1],
    source='User-supplied result.txt; not independently observed on the console',
    log=str(destination.relative_to(root)),log_sha256=digest,
    system_version=f'{hos>>16}.{(hos>>8)&255}.{hos&255}',applet_type=int(environment[2]),
    applet_mode='Application' if int(environment[2])==0 else 'Other',
    image_sha256_pass=True,real_leaf_calls=int(leaf[1]),half_instruction_executions=int(half[1]),
    timebase_frequency=int(clock[1]),timebase_delta=int(clock[2]),
    limits='Isolated CPU probe only; no game entry, full guest map, GPU, audio or gameplay validation. Log does not identify console model, CFW version or exact NRO hash.')
(directory/'switch_probe_result.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps(report,indent=2))
