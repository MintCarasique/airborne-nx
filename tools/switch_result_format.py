"""Parse the memory-probe protocol; log text is data, never executable instructions."""
import re

def parse_vulkan_entry_result(text,known_build_id):
    banner=re.match(r'AirborneNX ARM64 game entry v(0\.[56])\n',text)
    if not banner:raise ValueError('Not a supported Vulkan entry log')
    version=banner[1]
    lines=text.splitlines()
    builds=re.findall(r'^BUILD=([a-f0-9]{64})$',text,re.M)
    matches=len(builds)==1 and bool(known_build_id) and builds[0]==known_build_id
    environment=re.search(r'^HOS=([0-9a-fA-F]{8}) applet_type=(\d+) process_handle=([0-9a-fA-F]{8})$',text,re.M)
    imports=re.findall(r'^IMPORT (\w+) ',text,re.M)
    stops=[s for s in lines if s.startswith(('STARTUP_STOP ','THREAD_STOP '))]
    def failed_operation(line):
        for code in re.findall(r'\b(?:rc|\w+_rc)=([0-9a-fA-F]+|-\d+)\b',line):
            if not code.strip('0'):continue
            if code=='1000001003' and line.startswith(('GPU_NATIVE operation=vkAcquireNextImageKHR ','GPU_NATIVE operation=vkQueuePresentKHR ')):continue
            return True
        return False
    failures=[s for s in lines if s.startswith(('FAIL ','LOADER_ERROR ','NATIVE_FAULT ','WATCHDOG ','GPU_MEMORY_FAIL ')) or failed_operation(s)]
    results=re.findall(r'^RESULT=(\w+)$',text,re.M)
    adapters=[s for s in lines if s.startswith('GPU_ADAPTER ')]
    swaps=[s for s in lines if s.startswith('GPU_SWAP_REQUEST ')]
    if version=='0.5':
        expected_boundary=stops==['STARTUP_STOP VdSwap requires front-buffer resolve and Vulkan presentation LR=0x829b3b08 SP=0x1004f3e0 CTR=0x00000000']
    else:
        expected_boundary=len(stops)==1 and 'PM4 WAIT_REG_MEM blocked' in stops[0]
    depth_clear=any(s.startswith('GPU_EDRAM_DEPTH_CLEAR_COMPLETE ') for s in lines)
    resolve=any(s.startswith('GPU_EDRAM_RESOLVE_COMPLETE ') for s in lines)
    pixels_queued=any(s.startswith('GPU_NATIVE operation=DISPLAY_QUEUED_GUEST_PIXELS rc=0 ') for s in lines)
    interrupt=any(s.startswith('GPU_INTERRUPT_DISPATCH ') for s in lines)
    initialized=any(s.startswith('GPU_NATIVE_READY ') for s in lines)
    cleanup=('CLEANUP=PASS' in lines and 'SHUTDOWN graphics=PASS' in lines
        and 'SHUTDOWN guest_memory_close_rc=00000000' in lines
        and text.rstrip().endswith('EXIT_ACTION=RETURN_TO_LOADER'))
    complete=bool(matches and environment and initialized and swaps and expected_boundary
        and results==['STOPPED'] and 'EXIT_STATUS=10' in lines and cleanup and not failures
        and 'PREFLIGHT generated_frame_unwind=PASS' in lines
        and 'INPUT_SHA256 image=PASS bytes=20054016' in lines
        and 'INPUT_SHA256 xex_header=PASS bytes=16384' in lines)
    if version=='0.6':complete=complete and depth_clear and resolve and pixels_queued and interrupt
    hos=int(environment[1],16)&0x7fffffff if environment else None
    retired=re.findall(r'^GPU_RING_STOP .*? packets=(\d+) ',text,re.M)
    return dict(status=('USER_REPORTED_STARTUP_PIXELS_QUEUED' if version=='0.6' else 'USER_REPORTED_VULKAN_ENTRY_STOPPED') if complete else 'INCOMPLETE_OR_FAILED_LOG',
        probe_kind='entry',probe_version=version,build_id=builds[0] if len(builds)==1 else None,
        matches_known_build=matches,system_version=f'{hos>>16}.{(hos>>8)&255}.{hos&255}' if hos is not None else None,
        applet_type=int(environment[2]) if environment else None,adapters=adapters,
        vulkan_initialized=initialized,import_count=len(imports),imports_seen=list(dict.fromkeys(imports)),
        packets=int(retired[-1]) if retired else len(re.findall(r'^GPU_PACKET ',text,re.M)),
        packet_trace_records=len(re.findall(r'^GPU_PACKET ',text,re.M)),no_output_draws=len(re.findall(r'^GPU_DRAW_SKIPPED ',text,re.M)),
        swap_requests=swaps,stops=stops,failures=failures,first_failure=failures[0] if failures else None,
        cleanup_logged_pass=cleanup,loader_return_action_logged='EXIT_ACTION=RETURN_TO_LOADER' in lines,
        native_depth_clear_completed=depth_clear,known_zero_resolve_completed=resolve,guest_pixels_queued_to_vi=pixels_queued,gpu_interrupt_dispatched=interrupt,
        limits='User-supplied log. Vulkan startup/initial pixels only as individually recorded; no general game rendering or gameplay. A queued VI image is not independent visual observation. Cleanup logged before return; subsequent HOS/loader behavior is not observable from this file.')

def parse_entry_result(text,known_build_id):
    if not text.startswith('AirborneNX ARM64 game entry v0.4\n'):raise ValueError('Not a v0.4 entry log')
    lines=text.splitlines()
    build=re.findall(r'^BUILD=([a-f0-9]{64})$',text,re.M)
    matches=len(build)==1 and bool(known_build_id) and build[0]==known_build_id
    environment=re.search(r'^HOS=([0-9a-fA-F]{8}) applet_type=(\d+) process_handle=([0-9a-fA-F]{8})$',text,re.M)
    imports=re.findall(r'^IMPORT (\w+) ',text,re.M)
    entry=re.findall(r'^ENTRY 0x82c5c310 SP=0x1004ff00 R13=0x70021000$',text,re.M)
    stops=[s for s in lines if s.startswith(('STARTUP_STOP ','THREAD_STOP '))]
    failures=[s for s in lines if s.startswith(('FAIL ','LOADER_ERROR ','NATIVE_FAULT ','WATCHDOG '))]
    result=re.findall(r'^RESULT=(STOPPED|FAIL|PASS)$',text,re.M)
    ready='MEMORY_READY image=20054016 mappings=81146 stack=262144 bindings=277 leaf_cases=12 guards=2'
    complete=(matches and environment and len(entry)==1 and imports and stops and not failures
        and result==['STOPPED'] and lines[-1:]==['RESULT=STOPPED'] and ready in lines
        and 'PREFLIGHT generated_frame_unwind=PASS' in lines
        and 'INPUT_SHA256 image=PASS bytes=20054016' in lines
        and 'INPUT_SHA256 xex_header=PASS bytes=16384' in lines
        and 'CLEANUP=PROCESS_EXIT_REQUIRED' in lines and 'EXIT_STATUS=10' in lines)
    status='USER_REPORTED_GAME_ENTRY_STOPPED' if complete else 'USER_REPORTED_ENTRY_FAILURE' if result==['FAIL'] else 'INCOMPLETE_OR_UNVERIFIED_LOG'
    memory_failures=[s for s in lines if s.startswith('RUNTIME_MEM ') and re.search(r' rc=(?!00000000)[a-fA-F0-9]{8}\b',s)]
    unexpected=[s for s in lines if s in failures or s in memory_failures or s.startswith('RUNTIME_MEMORY_REJECT ')]
    hos=int(environment[1],16)&0x7fffffff if environment else None
    return dict(status=status,probe_kind='entry',probe_version='0.4',build_id=build[0] if len(build)==1 else None,
        matches_known_build=matches,entry_reported=bool(entry),imports_seen=list(dict.fromkeys(imports)),import_count=len(imports),
        last_import=imports[-1] if imports else None,stops=stops,failures=failures,memory_failures=memory_failures,
        first_failure=unexpected[0] if unexpected else stops[0] if stops else None,
        system_version=f'{hos>>16}.{(hos>>8)&255}.{hos&255}' if hos is not None else None,
        applet_type=int(environment[2]) if environment else None,
        limits='User-supplied bounded game-entry evidence, not independently observed. STOPPED is a runtime boundary, not a game PASS; no renderer/audio/gameplay validation.')


def parse_startup_result(text,known_build_id):
    if not text.startswith('AirborneNX ARM64 startup probe v0.3\n'):
        raise ValueError('Not a v0.3 startup probe log')
    lines=text.splitlines()
    build=re.findall(r'^BUILD=([a-f0-9]{64})$',text,re.M)
    result=re.findall(r'^RESULT=(PASS|FAIL)$',text,re.M)
    environment=re.search(r'^HOS=([0-9a-fA-F]{8}) applet_type=(\d+) process_handle=([0-9a-fA-F]{8})$',text,re.M)
    phase_rows=re.findall(r'^PHASE (\w+)=(PASS|FAIL|BLOCKED|SKIP)(?: dependency=[\w_]+)?$',text,re.M)
    phases=dict(phase_rows)
    required=('native_services','image_stack','guest_workers','physical_aliases','guest_heap','guards','guest_memory','cleanup')
    matches=len(build)==1 and bool(known_build_id) and build[0]==known_build_id
    complete=(matches and environment and result==['PASS'] and lines[-1:]==['RESULT=PASS']
        and len(phase_rows)==len(phases) and all(phases.get(p)=='PASS' for p in required)
        and phases.get('game_entry')=='BLOCKED' and 'FIRST_FAILURE=none' in lines
        and 'CHECKS=1569' in lines and not any(s.startswith('FAIL ') for s in lines))
    status='USER_REPORTED_HARDWARE_PASS' if complete else 'USER_REPORTED_HARDWARE_FAIL' if result==['FAIL'] else 'INCOMPLETE_OR_UNVERIFIED_LOG'
    hos=int(environment[1],16)&0x7fffffff if environment else None
    checks=re.search(r'^CHECKS=(\d+)$',text,re.M)
    return dict(status=status,probe_version='0.3',probe_kind='startup',build_id=build[0] if len(build)==1 else None,
        matches_known_build=matches,phases=phases,checks=int(checks[1]) if checks else None,
        system_version=f'{hos>>16}.{(hos>>8)&255}.{hos&255}' if hos is not None else None,
        applet_type=int(environment[2]) if environment else None,
        failures=[s for s in lines if s.startswith('FAIL ')],
        limits='User-supplied evidence. Startup prerequisites and isolated guest leaf only. R13 preservation and private scratch slots do not validate the full guest PCR/TLS ABI. Game entry, full allocator, GPU, audio and gameplay remain untested.')


def parse_memory_result(text,known_build_id):
    banner='AirborneNX ARM64 memory probe v0.2'
    if not text.startswith(banner+'\n'):raise ValueError('Not a v0.2 memory probe log')
    build=re.search(r'^BUILD=([a-f0-9]{64})$',text,re.M)
    environment=re.search(r'^HOS=([0-9a-fA-F]{8}) applet_type=(\d+) process_handle=([0-9a-fA-F]{8})$',text,re.M)
    checks=re.search(r'^CHECKS=(\d+)$',text,re.M)
    result=re.search(r'^RESULT=(PASS|FAIL)$',text,re.M)
    expected=(
        'PASS mapped image SHA256',
        'PASS mapped real guest function 828B6D00: 768 calls',
        'PASS physical A/C/E aliases: scalar/vector/atomic/cache-zero',
        'PASS stack guards=2',
        'PASS rejected overlapping/unaligned/out-of-range mappings',
        'PASS unmap and release',
    )
    lines=text.splitlines()
    complete=bool(build and environment and result and result[1]=='PASS' and
        lines[-1:]==['RESULT=PASS'] and checks and int(checks[1])==1569 and
        all(line in lines for line in expected) and re.search(r'^PASS VA reservation span=4294967296 base=[0-9a-f]{16}$',text,re.M) and
        not any(line.startswith('FAIL ') for line in lines))
    matches=bool(build and known_build_id and build[1]==known_build_id)
    status='USER_REPORTED_HARDWARE_PASS' if complete and matches else 'USER_REPORTED_HARDWARE_FAIL' if result and result[1]=='FAIL' else 'INCOMPLETE_OR_UNVERIFIED_LOG'
    hos=int(environment[1],16)&0x7fffffff if environment else None
    return dict(status=status,probe_version='0.2',probe_kind='memory',
        build_id=build[1] if build else None,matches_known_build=matches,
        system_version=f'{hos>>16}.{(hos>>8)&255}.{hos&255}' if hos is not None else None,
        applet_type=int(environment[2]) if environment else None,
        checks=int(checks[1]) if checks else None,
        failures=[line for line in lines if line.startswith('FAIL ')],
        limits='User-supplied log, not independently observed. Small memory/addressing probe only; no game entry, full heap, 512 MiB physical pool, GPU, audio or gameplay validation.')
