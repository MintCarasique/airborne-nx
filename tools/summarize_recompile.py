from pathlib import Path
from collections import Counter
import json
import hashlib
import re
import struct
import tomllib
from generated_state import generated_fingerprint

root = Path(__file__).resolve().parents[1]
log = (root/'analysis/recompile.log').read_text()
unsupported = re.findall(r'Unrecognized instruction at (0x[0-9A-Fa-f]+): (\S+)', log)
errors = [line for line in log.splitlines() if line.startswith('ERROR:')]
missing = re.findall(r'Found a switch jump table at ([0-9A-F]+) with no switch table entry present', log)
rc = [line for line in log.splitlines() if 'RC bit enabled but no comparison was generated' in line]
cpp = list((root/'generated/ppc').glob('*.cpp'))
tables = tomllib.loads((root/'analysis/airborne_switch_tables_merged.toml').read_text())['switch']
blob = (root/'analysis/image/image_loader_normalized.bin').read_bytes()
pe = struct.unpack_from('<I', blob, 0x3c)[0]
assert blob[:2] == b'MZ' and blob[pe:pe+4] == b'PE\0\0'
count, optional = struct.unpack_from('<H', blob, pe+6)[0], struct.unpack_from('<H', blob, pe+20)[0]
sections = []
for i in range(count):
    at = pe+24+optional+i*40
    name = blob[at:at+8].split(b'\0')[0].decode('ascii')
    size, rva, raw_size, raw_offset = struct.unpack_from('<IIII', blob, at+8)
    sections.append(dict(name=name, address=f'0x{0x82000000+rva:08X}', size=size,
                         raw_size=raw_size, raw_offset=raw_offset))
report = dict(
    state='EXPERIMENTAL_NOT_RUNNABLE', generated_cpp_files=len(cpp),
    generated_cpp_bytes=sum(p.stat().st_size for p in cpp),
    generated_function_definitions=sum(p.read_text().count('PPC_FUNC_IMPL(') for p in cpp),
    function_count_note='Includes inferred boundaries and padding; not a verified original function count.',
    detected_switch_tables=len(tables),
    unsupported_instruction_occurrences=len(unsupported),
    unsupported_instruction_counts=dict(Counter(n for a,n in unsupported).most_common()),
    unsupported_instruction_sites=[dict(address=a, instruction=n) for a,n in unsupported],
    error_lines=len(errors), unique_errors=sorted(set(errors)),
    missing_switch_tables=missing, incomplete_record_bit_operations=rc,
    pe_sections=sections,
    loader_image_note='Decompressed mapped image; upstream loader normalizes import records and replaces function thunks. Not an untouched PE dump.',
    validation='Generation completed. No matching current-source syntax result attached yet; no complete game build, link or boot validation.'
)
syntax_path=root/'analysis/syntax_summary.json'
fingerprint=generated_fingerprint(root/'generated/ppc')
if syntax_path.exists():
    syntax=json.loads(syntax_path.read_text())
    if syntax.get('generated_fingerprint') == fingerprint:
        report['syntax_check']={k:syntax[k] for k in ('total','failed','scope','generated_fingerprint')}
        report['validation']=f"Host Clang syntax check: {syntax['total']} units, {syntax['failed']} failed. No object build, link, game execution or ARM64 validation."
for filename,key,fields in (
    ('object_build_summary.json','object_build',('scope','total','failed','archive_exit_code','archive_bytes','generated_fingerprint')),
    ('host_link_summary.json','host_link',('scope','imports','exit_code','stdout','executable_bytes','generated_fingerprint')),
):
    path=root/'analysis'/filename
    if path.exists():
        result=json.loads(path.read_text())
        if result.get('generated_fingerprint')==fingerprint:
            report[key]={k:result[k] for k in fields if k in result}
if 'object_build' in report:
    result=report['object_build']
    report['validation']=f"Host CPU object build: {result['total']} units, {result['failed']} failed."
    if 'host_link' in report:
        report['validation']+=f" Diagnostic full CPU link-check exit code: {report['host_link']['exit_code']}; 267 aborting import placeholders."
    report['validation']+=' No game boot, ARM64 game execution or Switch binary validation.'
probe_path=root/'analysis/host_probe_summary.json'
if probe_path.exists():
    probe=json.loads(probe_path.read_text())
    dependencies=probe.get('runtime_source_sha256',{})
    fresh=dependencies and all((root/p).exists() and hashlib.sha256((root/p).read_bytes()).hexdigest()==sha for p,sha in dependencies.items())
    if fresh and probe.get('generated_fingerprint')==fingerprint:
        report['startup_probe']={k:probe[k] for k in ('scope','imports_seen','implemented_function_imports','variable_configuration','results','negative_cases')}
        stop=probe['results'][1]['stderr']
        report['validation']+=' Diagnostic guest entry executed; '+stop
switch_path=root/'analysis/switch_probe_summary.json'
if switch_path.exists():
    switch=json.loads(switch_path.read_text())
    if switch.get('generated_fingerprint')==fingerprint:
        report['switch_probe']={k:switch[k] for k in ('scope','compiler','guest_leaf','memory_note','artifacts')}
for filename,key in (('switch_probe_result.json','switch_hardware_result'),('switch_memory_probe_result.json','switch_memory_hardware_result'),('switch_startup_probe_result.json','switch_startup_hardware_result'),('switch_entry_result.json','switch_entry_hardware_result')):
    hardware_path=root/'analysis/hardware'/filename
    if hardware_path.exists():
        hardware=json.loads(hardware_path.read_text())
        saved_log=root/hardware['log']
        if saved_log.exists() and hashlib.sha256(saved_log.read_bytes()).hexdigest()==hardware['log_sha256']:
            report[key]=hardware
memory_probe_path=root/'analysis/switch_memory_probe_summary.json'
if memory_probe_path.exists():
    memory_probe=json.loads(memory_probe_path.read_text())
    dependencies=memory_probe.get('sources_sha256',{})
    fresh=dependencies and all((root/p).exists() and hashlib.sha256((root/p).read_bytes()).hexdigest()==sha for p,sha in dependencies.items())
    if fresh and memory_probe.get('generated_fingerprint')==fingerprint:
        report['switch_memory_probe']={k:memory_probe[k] for k in ('scope','compiler','guest_leaf','memory_note','artifacts','build_id')}
startup_probe_path=root/'analysis/switch_startup_probe_summary.json'
if startup_probe_path.exists():
    startup_probe=json.loads(startup_probe_path.read_text())
    dependencies=startup_probe.get('sources_sha256',{})
    fresh=dependencies and all((root/p).exists() and hashlib.sha256((root/p).read_bytes()).hexdigest()==sha for p,sha in dependencies.items())
    if fresh and startup_probe.get('generated_fingerprint')==fingerprint:
        report['switch_startup_probe']={k:startup_probe[k] for k in ('scope','compiler','guest_leaf','memory_note','artifacts','build_id')}

entry_path=root/'analysis/switch_entry_summary.json'
if entry_path.exists():
    entry=json.loads(entry_path.read_text())
    dependencies=entry.get('sources_sha256',{})
    fresh=dependencies and all((root/p).exists() and hashlib.sha256((root/p).read_bytes()).hexdigest()==sha for p,sha in dependencies.items())
    if fresh and entry.get('generated_fingerprint')==fingerprint:
        report['switch_entry_runtime']={k:entry[k] for k in ('scope','build_id','compiler','archive_sha256','artifacts')}

for filename,key in (('switch_object_build_summary.json','switch_cpu_object_build'),('switch_link_summary.json','switch_cpu_link')):
    path=root/'analysis'/filename
    if not path.exists():continue
    target=json.loads(path.read_text())
    dependencies=target.get('runtime_sources_sha256',{})
    fresh=all((root/p).exists() and hashlib.sha256((root/p).read_bytes()).hexdigest()==sha for p,sha in dependencies.items())
    if fresh and target.get('generated_fingerprint')==fingerprint and not target.get('subset'):
        report[key]={k:v for k,v in target.items() if k not in ('results','flags')}

definitions=Counter()
traps=[]
for path in cpp:
    source=path.read_text()
    definitions.update(re.findall(r'PPC_FUNC_IMPL\(([^)]+)',source))
    last_instruction=''
    function=''
    for number,line in enumerate(source.splitlines(),1):
        match=re.search(r'PPC_FUNC_IMPL\(([^)]+)',line)
        if match: function=match[1]
        if line.lstrip().startswith('// '): last_instruction=line.strip()[3:]
        if '__builtin_debugtrap()' in line or '__builtin_trap()' in line:
            traps.append(dict(file=path.name,line=number,function=function,instruction=last_instruction))
report['duplicate_generated_definitions']=sum(n-1 for n in definitions.values())
report['explicit_runtime_trap_sites']=traps
(root/'analysis/recompile_summary.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps({k:v for k,v in report.items() if k not in ('unsupported_instruction_sites','unique_errors','pe_sections','incomplete_record_bit_operations')}, indent=2))
