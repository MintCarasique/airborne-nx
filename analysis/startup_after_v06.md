# Startup development after measured 0.6

The old wait on physical `1fcf5004` is frame pacing. GPU source-1 interrupts queue flips; source-0 vblank interrupts let the guest retire them. The runtime now suspends an unmatched display wait, delivers the interrupt, and resumes the exact nested command position without replaying earlier effects. The guest handler clears its own flag. Windows uses an explicitly logged virtual 60 Hz clock. Switch waits on `viGetDisplayVsyncEvent`, owns a VI service/display/event reference, and releases those resources during cleanup. No guest completion flag is forged.

The actual Windows entry passes that boundary and reaches audio startup. Implemented contracts include a physical table of 320 XMA contexts with 64-byte slots, context allocation/release, the little-endian XMA table-base MMIO read, counted kernel semaphores, object-based thread resume, and wait-any/wait-all over kernel events and semaphores. Wait-all checks availability atomically and consumes nothing on timeout; asynchronous wakeup is exercised locally. Other XMA MMIO and decoding requests still stop explicitly.

XAudio registration opens a real PCM device and allocates an independent guest callback worker with stack, PCR, TLS, and the indirect argument wrapper used by the ABI. Four callback credits bound outstanding frame requests. Actual submitted planar big-endian float frames are converted to stereo PCM16 at 48 kHz; centre/surround channels mix at -3 dB, LFE is omitted, non-finite samples become zero, and clipping is bounded. Windows uses waveOut; Switch uses audout. Completion credits are returned only after the native device releases the buffer. This initial output implementation serializes individual 256-sample buffers and may underrun; a queued streaming backend remains necessary before claiming production audio quality. It does not decode XMA.

The runtime declares a legacy virtual XAM ABI version `2.0.1888.0`; this is a compatibility profile, not a claim about the Horizon OS version or a complete Xbox dashboard. Later dynamically resolved XAM extensions are unavailable. Category volume defaults to unity with no dashboard changes; stereo speaker configuration matches the native output.

Current Windows entry stop: `NetDll_XNetStartup`, LR `82a98a54`. No game audio frame submission or non-initialization game frame has been observed. Network bootstrap and general Xenos rendering remain outstanding. No PC game files are required by the present blocker.

Validation:

- `analysis/host_probe_summary.json`: 1,304 service checks, bounded actual guest execution ending with diagnostic exit 10 at the named import, and invalid-input checks. This is not a game-boot pass.
- `analysis/command_ring_validation.log`: 44 PM4/MMIO checks, including exact interrupt/display resumption and XMA MMIO interception.
- `analysis/front_buffer_validation.log`: address/endian/bounds checks, two complete Vulkan pixel-transfer/readback comparisons and virtual vblank waits; no game frame claimed.
- `analysis/pcm_output_validation.log`: one silent 256-sample stereo frame sent through the real Windows output device and successful reset/unprepare/close. PCM conversion vectors also exercise polarity, centre/surround mapping, LFE omission, NaN/infinity handling and clipping.
- `analysis/switch_entry_next_build_summary.json`: source and ELF fingerprints for the final cross-compile/link validation. This confirms building, not execution on Switch.

Native audio and VI failures log the operation/result and relevant memory information; Switch cleanup wakes dispatcher/audio workers, joins all retained workers before device/memory teardown, and logs audio shutdown separately. The logger mutex correction remains unverified on hardware. The measured 0.6 NRO, ELF, ZIP, Build ID and raw log are preserved. No isolated hardware rerun is requested.
