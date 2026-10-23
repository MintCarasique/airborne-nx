# Native startup graphics 0.6

Package: `dist/airborne-nx-entry-v0.6.zip`. Its ARM64 NRO has not run on Switch.
The successful hardware evidence for 0.5 remains separate and unchanged.

Windows game entry completes 1,024 PM4 packets before an explicit unmatched
WAIT_REG_MEM at physical `0x1fcf5004` (descriptor `0x1fcf5006`, endian 2):
observed 1, expected 0. Six extra trace rows come from resuming interrupted
indirect lists; they are not extra retired packets. No completion value is
substituted to bypass this wait.

The actual startup path now completes two native Vulkan depth/stencil clears,
one restricted resolve, one 1280x720 guest-pixel transfer with matching GPU
readback, and two callbacks to the title's registered GPU interrupt handler.
These are initialization pixels, initially black. No game menu, general shader
draws, gameplay or Vulkan graphics pipeline/raster execution is claimed.

## Implemented paths and limits

- Front-buffer conversion handles 32-bit RGBA8 linear/tiled Xenos addresses,
  endian controls and descriptor swizzles (the observed buffer uses B,G,R,one).
  Exact referenced pixel extent is checked against a live physical allocation.
- VdSwap emits an ordered private packet into the title's reserved command
  space. It does not present early at import time. Windows transfers to an
  offscreen image and reads pixels back. Horizon creates a VI surface and FIFO
  swapchain, waits for acquisition/submission and queues the actual guest pixels.
- The initial XDK rectangle is recognized only with exact shader bytes pinned
  by SHA256 during preparation, matching graphics state, vertices, D24S8 zero
  depth and stencil replacement, D3D half-pixel adjustment and full-surface
  coverage. It becomes a real VkCmdClearDepthStencilImage on a native 640x360
  image with four samples. A fence completes before the command retires.
- The following 1280x720 RGBA8 resolve reinterprets exactly the same completely
  zeroed EDRAM range. Its format, geometry, shaders and destination are checked.
  Only this proven zero case uses the CPU resolve; unknown EDRAM data or other
  rendering cannot take this path. Destination padding is preserved.
- PM4 interrupts suspend a blocked indirect list and deliver the registered
  guest callback after releasing the service lock. Its progress is retained,
  so earlier commands are not replayed. Guest registers, FP control, PCR TLS,
  CPU byte and IRQL are restored. Spinlocks store the guest PCR as owner.
- Bin mask/select, predication, fetch flush registers through 0x5002 and
  conditional memory/register writes support the startup command stream.
- The supervisor closes the text console before giving NVK the window. It
  joins guest threads, closes Vulkan and guest memory before reopening the text
  display. This revised ownership path still needs its first target execution.

## Local verification

1,279 shared service checks include the graphics spinlock contracts and failure
cases. Forty-one PM4/MMIO checks include indirect-list suspension/resumption
without duplicated writes, interrupt masks, conditional writes, presentation
failure without retirement and endian/ring boundary cases. Front-buffer checks
cover independent address vectors, tile bijection, partial tiles, all endian
modes, the title's swizzle and truncation. Two distinct 1280x720 patterns pass
real Vulkan transfer/readback. The real game entry additionally exercises the
native depth clear, resolve and offscreen transfer in order.

Two immediate startup shaders were reconstructed with restricted interfaces,
compiled to SPIR-V and accepted by spirv-val. The native clear path uses exact
microcode matching plus its separately evaluated passthrough semantics; it does
not execute those SPIR-V modules as a general game pipeline. Captured game code
stays in ignored generated files and the private entry log.

All runtime units compile and link for AArch64 against the complete CPU archive
and NVK. The NRO/ELF targets, ZIP integrity and current source/artifact hashes are
verified. Hardware-tested 0.4 and 0.5 packages remain preserved. Full identities
and checksums are in `switch_entry_v0.6_summary.json`.

The next hardware run is needed specifically for native queue execution and VI
window ownership/presentation. It is not another driver-init or memory-map probe.
Afterward the remaining completion/timing contract and general rendering can be
developed using this established path.


## Supplied 0.6 hardware evidence

The supplied log matches build `ba51b874a908715cb90a4725560b1a59231042e415fc5ec43a6004289d9c8f70`. Intact records show two successful GPU depth clears, the restricted EDRAM resolve, the 1280x720 VI surface and three-image swapchain, actual queue submission/fence completion, `vkQueuePresentKHR rc=0` and `DISPLAY_QUEUED_GUEST_PIXELS`. It reaches the same 1,024-packet unmatched wait as Windows. All recorded shutdown/join/unmap results succeed and the log ends CLEANUP=PASS / EXIT_ACTION=RETURN_TO_LOADER. Queueing pixels is not independent visual observation or game rendering validation.

The raw file also contains two corrupted text regions around interrupt handling, including invalid UTF-8 and a NUL at byte 1,706,341. The first invalid UTF-8 byte is 1,706,342 (zero-based). The explicit interrupt-dispatch text records are absent; missing lines are not reconstructed. The report is classified USER_REPORTED_PARTIAL_EVIDENCE_CORRUPTED_LOG, retaining individual intact graphics milestones. Concurrent logging is a possible cause, not an established diagnosis. A dedicated logging/flush mutex has been added for subsequent builds; target verification is pending. No isolated repeat hardware run is requested.

Original bytes are preserved as `analysis/hardware/switch_entry_v0.6_20c80321cb3f.txt`, SHA256 `20c80321cb3f4cee358d600b48e5719b1027dcacafcfb2f9966b3249749ee5d9`. Parsed details are in `analysis/hardware/switch_entry_v0.6_result.json`. The tested 0.6 NRO/ELF/ZIP remain unchanged. Earlier NOT_RUN descriptions refer to the pre-test state.
