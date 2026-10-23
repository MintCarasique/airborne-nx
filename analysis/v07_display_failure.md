# 0.7 hardware stop before movie rendering

Supplied user log: `hardware/switch_entry_v0.7_4e69253caac7.txt`, SHA256
`4e69253caac79b68fe0ae408c30971df117c72c6daceb8a1a2c99ab00064a224`.
Build matches 0.7. The complete UTF-8 log has no NULs. It records one initial
black-buffer presentation, then `viOpenDefaultDisplay/vblank rc=4722`
(`0x1272`, module 114, description 9). No movie draw or plane upload and no
game file open occurred. All recorded worker joins/cleanup succeed, ending
`CLEANUP=PASS` and `RETURN_TO_LOADER`. The user saw diagnostic screen, black
screen, final screen, with no intro. This is NOT a successful movie test.

The original implementation tried to reopen Default from wait_vblank after
the standard libnx native window already held it. libnx has no public getter
for this private display. The correction captures successful Default opens
through link-time viOpenDisplay/viCloseDisplay wrappers and borrows that same
live object. It never opens or closes a display to wait for vblank; it owns
only its separately acquired event. Failed open/close calls do not replace or
invalidate a valid owner. Successful owner close invalidates the borrowed view.
No display ID is guessed and no timer is substituted for a hardware event.

Primary libnx sources:
- https://github.com/switchbrew/libnx/blob/master/nx/source/display/default_window.c
- https://github.com/switchbrew/libnx/blob/master/nx/source/services/vi.c

The installed libnx archive was inspected: __nx_win_init/nwindowGetDefault
share the default-window object, with an external viOpenDisplay reference;
default display state is private. Check the final ELF to confirm link wrapping
and vblank references before exporting the patch. Local stub contract checks
exercise owner lifetime, failed operations and 100 borrows without extra opens
or borrowed closes. These are local/link checks, not Horizon execution.

Patch 0.7.1 reuses the already copied 0.7 game directory and records required
content checks before guest startup. One new run will exercise the corrected
real display event and, if startup proceeds, the previously untested native
movie path. Do not repeat unrelated initialization/CPU/memory probes.
