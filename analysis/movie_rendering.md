# Native movie rendering, development 0.7

The original guest reads/decodes the supplied WMV. Its actual vertex/pixel
programs execute in Vulkan using three R8 images and reflected Y/U/V bindings.
The completed RGBA8 attachment owns the movie EDRAM surface. The guarded copy
command resolves actual pixels into guest tiled memory with the requested
red/blue swap; the display fetch applies its own swizzle before presentation.
No intro is omitted and no unsupported draw is acknowledged as successful.

Windows recorded 253 movie draws/resolves. A real presented frame is saved as
`generated/captures/video/presented.png`: the RUSBOX intro from this supplied
copy, not a recreated screen. Native pixel checks compare 2,764,800 pixels
(actual frame, chroma/luma gradient, white with resource reuse); maximum byte
error is one, with tolerance two. Video resource cleanup completes. Host service
checks: 1,370. These are Windows measurements, not Switch execution.

Current stop: `XMADisableContext`, LR `82997ef8`, during audio initialization.
The complete clip, XMA sound, menu drawing and gameplay remain unproven. An early
movie stop is expected; speed/correctness on Switch are unknown.

Other locally fixed blockers: 64 KiB physical protection rounding retains the
allocation page size; initial-thread priority uses a real native handle;
semaphore handles have counted waits/releases and overflow checks; idle critical
sections can be initialized again, with independent owner/waiter accounting.
Frequent lock-call traces are sampled with full counters to reduce SD writes.
Movie memory failures log ranges and Horizon memory states/attributes/permissions.

Version 0.7 is built/packaged but NOT EXECUTED ON SWITCH. Its ready local folder
contains 1,242 verified game files (6,329,205,186 bytes) for one SD transfer.
The small ZIP contains only the runtime/instructions. See the bundled README.
The recorded 0.6 artifact/ZIP hashes were verified unchanged. Do not repeat old
CPU/memory/initialization probes; the new target check is actual movie rendering.

Exact trace/source/capture hashes, build ID and package data are in
`movie_rendering_progress.json` and `switch_entry_v0.7_summary.json`. Earlier
resource-loading reports are historical. The prior stale-immediate-shader video
analysis was superseded by pointer-load tracing; its real pixel shader is in
the validated shader inventory. Primary references: title binary/captures and
the pinned SDK's `rex/graphics/xenos.h` and `registers.h` definitions.
