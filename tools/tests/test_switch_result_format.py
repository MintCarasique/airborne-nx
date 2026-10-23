import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from switch_result_format import parse_memory_result
build='a'*64
fixture=f'''AirborneNX ARM64 memory probe v0.2
Not a playable game.
BUILD={build}
HOS=00160100 applet_type=0 process_handle=00000007
PASS VA reservation span=4294967296 base=0000001000000000
PASS mapped image SHA256
PASS mapped real guest function 828B6D00: 768 calls
PASS physical A/C/E aliases: scalar/vector/atomic/cache-zero
PASS stack guards=2
PASS rejected overlapping/unaligned/out-of-range mappings
PASS unmap and release
CHECKS=1569
RESULT=PASS
'''
checks=0
def check(condition):
    global checks
    checks+=1
    assert condition
record=parse_memory_result(fixture,build)
check(record['status']=='USER_REPORTED_HARDWARE_PASS')
check(record['system_version']=='22.1.0' and record['applet_type']==0)
for missing in ('PASS mapped image SHA256','PASS physical A/C/E aliases: scalar/vector/atomic/cache-zero','PASS unmap and release'):
    check(parse_memory_result(fixture.replace(missing,''),build)['status']!='USER_REPORTED_HARDWARE_PASS')
for mutated in (fixture.replace('CHECKS=1569','CHECKS=1568'),fixture+'Unexpected trailer\n',fixture.replace('RESULT=PASS','FAIL cleanup rc=00001234\nRESULT=PASS')):
    check(parse_memory_result(mutated,build)['status']!='USER_REPORTED_HARDWARE_PASS')
check(parse_memory_result(fixture,'b'*64)['status']=='INCOMPLETE_OR_UNVERIFIED_LOG')
check(parse_memory_result(fixture,None)['status']=='INCOMPLETE_OR_UNVERIFIED_LOG')
failed=fixture[:fixture.index('PASS VA')]+'''FAIL svcMapProcessMemory rc=00001234
CHECKS=0
RESULT=FAIL
'''
record=parse_memory_result(failed,build)
check(record['status']=='USER_REPORTED_HARDWARE_FAIL' and record['failures']==['FAIL svcMapProcessMemory rc=00001234'])
check(parse_memory_result(failed.replace('RESULT=FAIL\n',''),build)['status']=='INCOMPLETE_OR_UNVERIFIED_LOG')
print(f'{checks} Switch log protocol tests passed using synthetic fixtures; no hardware result recorded')

from switch_result_format import parse_startup_result
startup=f"""AirborneNX ARM64 startup probe v0.3
BUILD={build}
HOS=00160100 applet_type=0 process_handle=00000007
PHASE native_services=PASS
PHASE image_stack=PASS
PHASE guest_workers=PASS
PHASE physical_aliases=PASS
PHASE guest_heap=PASS
PHASE guards=PASS
PHASE guest_memory=PASS
PHASE cleanup=PASS
PHASE game_entry=BLOCKED dependency=full_switch_runtime_and_graphics_service
FIRST_FAILURE=none
CHECKS=1569
RESULT=PASS
"""
check(parse_startup_result(startup,build)['status']=='USER_REPORTED_HARDWARE_PASS')
for phase in ('native_services','image_stack','guest_workers','physical_aliases','guest_heap','guards','guest_memory','cleanup'):
    for status in ('FAIL','BLOCKED','SKIP'):
        check(parse_startup_result(startup.replace(f'PHASE {phase}=PASS',f'PHASE {phase}={status}'),build)['status']!='USER_REPORTED_HARDWARE_PASS')
for bad in (startup+'trailer\n',startup.replace('FIRST_FAILURE=none','FIRST_FAILURE=FAIL mapping'),startup.replace('CHECKS=1569','CHECKS=0'),startup.replace('RESULT=PASS','RESULT=PASS\nRESULT=PASS'),startup.replace('PHASE cleanup=PASS','PHASE cleanup=PASS\nPHASE cleanup=PASS'),startup.replace('HOS=','HOS_BAD=')):
    check(parse_startup_result(bad,build)['status']!='USER_REPORTED_HARDWARE_PASS')
check(parse_startup_result(startup,None)['status']=='INCOMPLETE_OR_UNVERIFIED_LOG')
check(parse_startup_result(startup,'b'*64)['status']=='INCOMPLETE_OR_UNVERIFIED_LOG')
check(parse_startup_result(startup.replace('RESULT=PASS','RESULT=FAIL'),build)['status']=='USER_REPORTED_HARDWARE_FAIL')
print(f'{checks} total v0.2/v0.3 protocol checks passed; synthetic fixtures only')

from switch_result_format import parse_entry_result
entry=f"""AirborneNX ARM64 game entry v0.4
BUILD={build}
HOS=00160100 applet_type=0 process_handle=000481fa
PREFLIGHT generated_frame_unwind=PASS
INPUT_SHA256 image=PASS bytes=20054016
INPUT_SHA256 xex_header=PASS bytes=16384
MEMORY_READY image=20054016 mappings=81146 stack=262144 bindings=277 leaf_cases=12 guards=2
ENTRY 0x82c5c310 SP=0x1004ff00 R13=0x70021000
IMPORT VdInitializeEngines LR=0x829c0788
STARTUP_STOP Unimplemented import: VdInitializeEngines LR=0x829c0788
CLEANUP=PROCESS_EXIT_REQUIRED
EXIT_STATUS=10
RESULT=STOPPED
"""
check(parse_entry_result(entry,build)['status']=='USER_REPORTED_GAME_ENTRY_STOPPED')
check(parse_entry_result(entry,build)['last_import']=='VdInitializeEngines')
for marker in ('PREFLIGHT generated_frame_unwind=PASS','INPUT_SHA256 image=PASS bytes=20054016','INPUT_SHA256 xex_header=PASS bytes=16384','ENTRY 0x82c5c310 SP=0x1004ff00 R13=0x70021000','CLEANUP=PROCESS_EXIT_REQUIRED','EXIT_STATUS=10','MEMORY_READY image=20054016 mappings=81146 stack=262144 bindings=277 leaf_cases=12 guards=2'):
    check(parse_entry_result(entry.replace(marker,''),build)['status']!='USER_REPORTED_GAME_ENTRY_STOPPED')
for broken in (entry+'trailer\n',entry.replace('RESULT=STOPPED','RESULT=PASS'),entry.replace('RESULT=STOPPED','RESULT=STOPPED\nRESULT=STOPPED'),entry.replace('RESULT=STOPPED','FAIL service lock\nRESULT=STOPPED'),entry.replace('STARTUP_STOP','LOADER_ERROR')):
    check(parse_entry_result(broken,build)['status']!='USER_REPORTED_GAME_ENTRY_STOPPED')
check(parse_entry_result(entry,'b'*64)['status']=='INCOMPLETE_OR_UNVERIFIED_LOG')
check(parse_entry_result(entry,None)['status']=='INCOMPLETE_OR_UNVERIFIED_LOG')
check(parse_entry_result(entry.replace('RESULT=STOPPED','RESULT=FAIL'),build)['status']=='USER_REPORTED_ENTRY_FAILURE')
print(f'{checks} total probe/entry protocol checks passed using synthetic fixtures; no hardware evidence imported')
