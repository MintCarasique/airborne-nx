# Autoclear vblank event correction

Measured build e77977c5ffe544b074dd9f288f4d8a7c24f2ada8b770779a0ed153a667710ff9
successfully borrows libnx's display ID 83 and obtains its vsync event. It then
calls eventClear before waiting, returning 64001 / 0xFA01: kernel module 1,
description 125 (InvalidState). No file opens or movie draws occurred. The user
reports the same diagnostic/black/final-screen sequence. Cleanup succeeds.
Raw log and SHA256 are retained in `hardware/switch_entry_e77977c5ffe5_result.json`.

This is an implementation error: installed libnx event.h explicitly says not to
call eventClear for autoclear events. viGetDisplayVsyncEvent loads its returned
read handle using eventLoadRemote(..., true). Manual clearing an unsignalled
read handle immediately calls svcResetSignal, which can return InvalidState.

The correction uses eventWait alone. SDK eventWait constructs an autoclear
waiter; waitObjects first waits for signalling, then resets, retrying the
InvalidState competing-consumer race through the cancelled-wait path while
retaining the timeout. No timer or fabricated interrupt replaces the VI event.
The new log records read/write handles, autoclear/active flags, elapsed wait,
result/module/description and close state. Only the owned event is closed;
the borrowed native-window display remains owned by libnx.

Audit includes the installed SDK, not just upstream source: event.o and wait.o
were extracted from the actual libnx archive and disassembled. Records are
`libnx_event_contract_disassembly.log` and `libnx_wait_contract_disassembly.log`.
The binary waitObjects contains the ResetSignal call after WaitSynchronization
and the 0xFA01-to-0xEC01 retry path. Installed result.h confirms InvalidState=125.
The rebuilt application must have no direct eventClear/ResetSignal call in its
wait_vblank method, while retaining SDK eventWait and native event cleanup.

Primary sources:
- https://github.com/switchbrew/libnx/blob/master/nx/include/switch/kernel/event.h
- https://github.com/switchbrew/libnx/blob/master/nx/source/kernel/event.c
- https://github.com/switchbrew/libnx/blob/master/nx/source/kernel/wait.c
- https://github.com/switchbrew/libnx/blob/master/nx/source/services/vi.c

The corrected build uses the same airborne-nx folder/NRO/result.txt names and
existing game data. Old measured artifacts are archived before replacement.
Cross-compilation/link inspection does not establish target wait success or
movie playback. The last measured target state remains STOP_BEFORE_MOVIE until
a new user log demonstrates otherwise.
