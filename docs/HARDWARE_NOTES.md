# First run on ReXGlue (2026-10-23, build 8b57ce5+6cc5eaa4df8e, log `analysis/hardware/rexglue_*_result.txt`)
- Launched in application mode, handheld 1280x720; NVK (Mesa 26.2.4, git-0ff52079ca) creates the device; the guest
  memory arena is mapped; audio audout 48 kHz 2 channels; mounting 1249 game files from the SD card takes 2.65 s.
- The game passes the splash screen and reaches the menu (`Hus_BG_Bkd_Fairway`, menu music), no errors or crashes in
  ~2 min.
- Speed: by the user's impression < 1 FPS. Creating each graphics pipeline (shader translation + NAK) takes
  0.1-1.5 s; in the first ~40 s of the menu there are ~60 of them. FPS and per-thread CPU time are not yet written to
  the log.
- SDK profiler (`logs/rex/rex_perfil.log`, report every 10 s): in the menu 0.6-0.8 FPS at ~160 draws per frame; one
  guest thread `XThread2FE2D080` takes 100 % of a core, the GPU emulation thread ("GPU Commands") takes a few percent,
  total CPU 130-170 % out of 300 %. The bottleneck is the CPU of a single game thread, not GPU emulation. Clocks: CPU
  1020 MHz, GPU 460.8 MHz, memory 1600 MHz; 3 cores (mask 0x7). While the menu loads (pipeline compilation) 262 % CPU,
  0-0.1 FPS.
- Run 2 (aad0efd+4577d1b23b5d, nxlink, stack sampling): the same 0.7 FPS; local registers do not change FPS.
  Stacks: 87 % of the hot thread's time is `sub_829B0E38` (XDK `D3DQuery_GetData`, type 9 = occlusion), called in a
  wait loop from `sub_82453C88` (0x82453D70): UE3 spins until the GPU emulator writes the query result.
  The SDK shader cache on the SD card works: in the second run there is no pipeline compilation phase when the menu
  loads.
- Run 3 (aad0efd+a95feff5cf97, occlusion queries answer immediately): waiting on queries is gone from the stacks, FPS
  is the same 0.7. Now the hot thread waits for a GPU fence in the XDK (`sub_829AF948` -> `db16cyc` loop in
  `sub_829B6490`), and the process waits on NVK fences 1000 ms per second: bound by the real Tegra GPU, ~1.4 s of GPU
  per menu frame. Per frame: 167 game draws, 170 EDRAM transfers (23 RT, 115 additional draws), 10 resolves, scissor
  area ~126 screens.
- Run 4/5 (fbc5e09+6ce5dd5de8b5, `--gpu_timing=2`, timestamps x1.627), menu, GPU per frame:
  host render targets: draws 125-180 ms, EDRAM transfers 1180-1280 ms (~83 %), resolve 86-94 ms, other ~25 ms,
  total ~1500 ms, 0.7 FPS. FSI (`--render_target_path_vulkan=fsi`): no transfers (0.07 ms), but draws ~1000 ms,
  resolve ~19 ms, total ~1050 ms, 0.9-1.0 FPS. For comparison PC (RTX 4070 SUPER, host RT): draws ~5 ms, transfers
  3-6 ms. NVK does not advertise `VK_EXT_shader_stencil_export`: a transfer to depth = 1 depth pass + 8 stencil bit
  passes.
- Run 6 (f4b320c+4a88abf91c2f, `--cl=Tra_Jmp_P`): crash at the end of level loading. `AllocFixed`: committing 4 KB of
  guest memory -> `svcMapProcessCodeMemory` 0xCE01 (OutOfResource) at 411 MB backing / 658 MB with mirrors, then the
  game writes to 0x10. Cause: the SDK committed exactly the requested range, and UE3's per-page commits produced a
  separate kernel mapping for every 4 KB (+ one per mirror) - what runs out is kernel memory block objects, not bytes.
  Transfers in the menu by type: depth->stencil bit 631 ms/frame (56 draws), color->stencil bit 344 ms (40 draws),
  depth->depth 67 ms; each transfer draw costs ~10 ms of GPU regardless of area.
- Run 7 (c429d95+62b19985bf81, commit in 2 MB blocks): the tutorial level loaded, >5 min without memory errors
  (422 MB backing / 716 MB with mirrors). 0.1-0.3 FPS. GPU per frame in the level: total ~1090 ms, transfers 909 ms
  (depth->stencil bit 553 ms/32 draws, color->stencil bit 217 ms/24 draws), game draws 158 ms. All transfer types
  give ~25-35 Mpix/s regardless of the source.
- Run 8 (7d0cd99+4f709f05677d, `--nfsmw_renderizador=nativo`, shader library v3 186 MB as a separate file
  `sdmc:/switch/airborne-nx/airborne_shaders.nfsp`, log `analysis/hardware/live_native1.log`): the native renderer on
  hardware. Title screen 17-20 FPS (emulation: 0.7), heavier afterwards, 4-25 FPS. GPU 32-41 ms per Swap (scene
  20-32 ms, copies 3-4 ms, "320" 2 ms), CPU 150-270 % out of 400. 188 distinct shaders, 182 recognized (6 without
  containers), draw rejections 0, no crashes. Barriers around copies (`airborne_barreras_copias`) are enabled. One
  `DIFERENCIA` at IM_LOAD without memcmp (the guard disabled the shortcut). The picture from the console was not seen -
  a screenshot is needed.
- Run 9 (b251421+83083eb809bc, level `--cl=Tra_Jmp_P`, library v4, log `analysis/hardware/live_native_para.log`,
  `result.txt`, `rex_crash.log`): the level is playable, graphics generally correct (user: glow on the soldier,
  z-fighting on the ground). 4-29 FPS, CPU 220-290 % out of 300 available (3 cores, 1020 MHz, handheld), audio
  crackles. Crash after ~6 min: the UE3 render thread (command queue loop sub_82404CD8) read vtable 0x3E848C6D - the
  recompiler dropped `sync`/`lwsync`, ARM needs `dmb ish`.
- Runs 10-12 (stack sampler `perfil_pilas.flag` next to the NRO, log `live_native_pilas2.log`): the UE3 render thread
  spun waiting for the GPU in D3D (sub_829B6490, 90 %), Main - `Sleep(0)` loops; the GPU ring 63 % in `vkWaitForFences`
  from `GetResolved`: at address 15580000 formats 7 and 32 alternate, each time a synchronization with the GPU and a
  new image (2959 times in 4 min). After barriers, sleeping in waits and deferred images (b251421+7b6576b819e0,
  `live_native_park.log`): 7 recreations, CPU 115-250 %, descent 15-24 FPS instead of 10-16, near the ground 28-30
  (limit 30); now bound by the GPU: 33-131 ms per Swap in the airplane, almost no idle time.
- Runs 13-15 (2026-10-26, 5dd45df-dirty+9fbbfb14b9b0: sa2-nx generator passes, ME runtime waits, NVK with GPFIFO tail
  reserve; nxlink, `--cl=Tra_Jmp_P`, handheld; logs `analysis/hardware/live_5dd45df_{A_1280,B_960,C_912}.log`,
  each begins with the previous run's profiler report - `ForwardProfilerReport` reads the file before it is zeroed).
  FPS over 10-s windows in the level (12 windows after loading), GPU per Swap, transition to `Hus_M1_Briefing` from
  start: 1280x720 - average 22.8 FPS, GPU 19-26 ms, 188 s; 960x544 - 23.9 FPS, GPU 16-22 ms, 184 s;
  912x512 - 27.8 FPS (9 windows out of 12 at 29-30), GPU 11-19 ms, 175 s. The previous measurement
  (`live_native_park.log`) - 18.6 FPS on the same windows, the descent lasted longer. Scene in the corner: all frames
  (0 "whole"). No errors or crashes.
  Caveat: in A and B the native mixer self-test over ~35 s found 1 ULP in one float (call 1.1-1.26 million, in
  different directions: BA44B534/535, B879B3FE/FD) and disabled it for the rest of the session; in C it ran the whole
  time. So part of the difference between C and A/B is the mixer (RWAudioCore Dac 34-73 % of a core without it).
  audout dropouts per run: 20 / 30 / 14.
- Adanti (`Hus_M1_P`), 2026-10-26, 1280x720, handheld, logs `analysis/hardware/live_{866d,c681,neon,a016,839f}_hus*.log`,
  ELF for symbols in `analysis/hardware/elf/`:
  - 866d: mixer self-test three times in 30 s: "original" = one rounding (FMA), native = two (like Xenon).
    The generated code was built without `-ffp-contract=off` (the flag was set only in the SDK directory) - fixed in
    `rex/CMakeLists.txt`; after that (c681) there are no discrepancies.
  - Audio: audout dropouts 250-470 per 10 s in a heavy scene; RWAudioCore ~60 % of a core, does not wait in the
    kernel. The native mix-add was scalar (GCC did not vectorize it) and took ~25 % of the thread: NEON (005d) -
    dropouts 100-330, FPS 11-16.
    Priority 0x2C: `--guest_audio_thread_priority=0x2C` is read as 0 (decimal only: 44), and was set only from
    `Delay`, which the thread never reached - now also from object waits (a016). The game does not call
    `XAudioGetSpeakerConfig` (the stereo setting has no effect). `[XeAudio.XeAudioDevice] MaxChannels` 90 -> 32
    (Coalesced.ini override): thread 64-72 % -> 54-59 %, dropouts 216-443 -> 53-284, FPS the same.
  - a016: crash after 95 s - `InvalidFunctionTrap` 0x4354A232 from the UE3 render command queue loop (sub_82404CD8),
    as in run 9. The queue reader sub_823F8448 has no barrier between the offsets and the command: a hook with an
    acquire barrier (`rex/src/render_queue.cpp`, 839f) - two 5-min runs without a crash.
  - FPS bound in a heavy scene (10-11 FPS, CPU 307 % out of 300): the native renderer ring thread at 84 % of a core,
    ~4000-4500 draws per frame at ~14.5 us each (~60 ms per frame); GPU ~34 ms per Swap (~50 % load). In the ring: our
    translator ~15 %, memcpy/memcmp ~8 %, NVK ~12 % (vk_dynamic_graphics_state_copy 3.4 %). The main thread and the
    UE3 render thread ~55 % each, with sleeping (waiting for the ring). `StaticMeshCullDistanceBiasBase=0.6` and
    `bDynamicDecalsEnabled=False`: draws per frame did not decrease (median 4066 -> 4538 at another point), FPS the
    same.
- SDK: "Too few processor cores - scheduling will be wonky"; shared GPU memory - a full 512 MB buffer (no sparse).

# Measured on hardware (Switch, HOS 22.1.0, applet_type 0 = Application)

Source: user logs from the old runtime (`analysis/hardware/`; raw `.txt` files are kept locally, run notes were
deleted, see `git show 8ca98b1:analysis/`). The log does not name the console or the Atmosphere version.
Build tags: v0.1 ecb1e73175f9, v0.2 f6741f128e30 (memory), v0.3 811084736791, v0.4 7c22a4a387d5,
v0.5 fefdd112f4ab, v0.6 ba51b874a908, v0.7 6d74b214954e, e779 e77977c5ffe5, e05d e05d08748c72,
f4ba f4bac5364158, 6271 627190085788 (last measured).

## 1. Memory
- svcGetInfo at homebrew start (v0.3): TotalMemory 0xC7500000 (3188.5 MiB), UsedMemory 0xC710C000 (3184.5 MiB);
  Used is almost equal to Total immediately (hbloader heap), this is not "used by the game". Largest free heap region
  3 325 956 096 B (v0.3), 3 113 672 704 B (v0.5, after the runtime started).
- Process address regions (v0.3): alias 6 GiB (0x180000000), heap 8 GiB (0x200000000), ASLR 0xFF8000000.
- Reserving a contiguous 4 GiB of VA through libnx virtmem: succeeded (v0.2, v0.3). This is address space, not RAM.
- FAILURE (v0.2): `svcMapProcessMemory` with a source from the ordinary heap (type Normal=5) -> rc 0xD401 (module 1,
  description 106, InvalidMemoryState). Normal pages do not have CanMapProcess.
- FIX (v0.3, PASS 1569 checks): `svcMapProcessMemory`->`MapProcessCodeMemory` of the heap into a staging area
  (type 9 AliasCodeData), `SetProcessMemoryPermission(RW)` on staging (perm=3), then guest aliases from staging
  (type 15 SharedCode, RW). All map/unmap returned 0. Cleanup in reverse order: aliases, then returning pages to the
  heap.
- v0.3: physical windows A/C/E agree on scalar/vector/atomic/dcbz, 4 MiB heap x 1024 pages, guard pages (checked by
  a memory state query, without crashing).
- v0.4+: a 512 MiB staging physical memory pool is created and mapped in chunks (A/C/E only onto allocated slices);
  image 20 054 016 B, stack 256 KiB, 81 146 function mappings; no map/unmap failures (v0.4-6271).
- Virtual commits as AliasCodeData aliases followed by `SetMemoryPermission` work (v0.4+).
- Unmapping while worker threads were still alive was not done: exiting via `svcExitProcess` (v0.4) gave the user a
  standard HOS error after "+" (code unknown). Cooperative thread join -> unmap -> return from main: CLEANUP=PASS,
  EXIT_ACTION=RETURN_TO_LOADER (v0.5, v0.6, v0.7, e779, e05d, 6271).

## 2. Threads and time
- `threadCreate` with prio 44 (0x2C), core -2 (default), stack 64 KiB, join with a 3 s timeout: OK (v0.3).
- Prio 59 (0x3B) via `svcSetThreadPriority` was accepted and read back (rc 0) on all 20 THREAD_POLICY entries (6271).
  The core mask there is 0x1 everywhere (preferred=0), i.e. the log confirms core 0, not 0..2; the effect on speed was
  not measured.
- In the f4ba log: 15 ExCreateThread calls and 14 KeSetAffinityThread calls.
- Timer: `svcSleepThread(10 ms)` = 500 201 / 500 237 ticks at a diagnostic frequency of 50 MHz (~10.004 ms),
  monotonic (v0.1, v0.3).
- In ~120 s the game calls RtlEnterCriticalSection 419 399 times, KeWaitForSingleObject 1956 (6271): kernel imports
  are hot, they cannot be serialized or logged one by one.
- Line-by-line log flush to the SD card stalls startup: in 120 s not a single game file was opened (e05d). With a
  64 KiB buffer and a flush every 250 ms the first movie frame is at uptime 43.5 s (6271).

## 3. Video, NVK, VI
- NVK is visible as `NVIDIA Tegra X1 (GM20B) (NVK GM20B)`, vendor 10de, device 0fe0, apiVersion 4211042
  (Vulkan 1.4.98); int64, bufferDeviceAddress, runtimeDescriptorArray, gather, sampled-dynamic = 1 (v0.5, v0.6).
- VI surface: 1280x720, minImages = maxImages = 3, formats VkFormat 37 and 44, colorspace 0, usage 0x8009F
  (v0.6). Creating a 3-frame swapchain (FIFO), submit + fence, `vkQueuePresentKHR` rc 0 (v0.6).
- 1280x720 RGBA8 image: allocation 3 932 160 B (v0.5). Native depth/stencil clear 640x360 x4 MSAA and 720p resolve
  were executed on the GPU, fence completed (v0.6).
- VI FAILURE (v0.7): repeated `viOpenDefaultDisplay` from the vblank thread while the libnx window holds the Default
  display -> rc 0x1272 (module 114, description 9). libnx provides no public getter; fixed by borrowing the owner
  (intercepting `viOpenDisplay`/`viCloseDisplay`); the display id is then 83 (e779).
- vblank FAILURE (e779): `eventClear` on the auto-clearing event from `viGetDisplayVsyncEvent` -> rc 0xFA01
  (module 1, description 125, InvalidState). Fix: `eventWait` only. After that: 513 successful waits (e05d);
  one measured wait of 14.63 ms with a 100 ms timeout (e05d).
- Movie (6271, 120 s, watchdog): 475 movie frame draws; the interval between draws is mean 161.4 / median 99.6 /
  p95 219.3 ms (it includes game waits, this is not pure decoding). Stages, median: eDRAM resolve 14.9 ms,
  movie upload+draw+readback 14.2 ms, decode+present 42.1 ms (p95 52.2) - with a CPU readback on every frame.
  User: the intro is visible, "very slow" (f4ba, 6271).

## 4. Audio (6271)
- `audoutInitialize`/`audoutStartAudioOut` rc 0: 48 kHz, 2 channels, PCM16, buffer 1024 B (256 samples);
  `audoutAppendAudioOutBuffer` and `audoutWaitPlayFinish` rc 0. The game registers an XAudio client
  (callback 0x82AA3AF0) and submits frames of 6144 B (256 samples x 6 channels x float32); 4 callback credits.
  Audibility and underruns were not checked.

## 5. File I/O (6271)
- Content root `sdmc:/switch/airborne-nx/game/MOHAGame` (6.33 GB, 1242 files). All opens/reads succeeded (rc 0).
- SD read speed was not measured separately. `.xxx` packages are read in chunks of 0x20000 (128 KiB).
- No memory/file/audio errors on the movie path; `RESULT=FAIL` in 6271 is only the 120 s watchdog firing.

# What this confirms/supplements in the NFSMW platform-notes.md
- Confirms: ~3.2 GB of application memory, UsedMemory ~ Total from the very first moment (3184.5 of 3188.5 MiB for us).
- Confirms: the VI swapchain is exactly 3 images; 1280x720; VI window via libnx/NVK.
- Supplements: `MapProcessMemory` does not accept the Normal heap, rc 0xD401 (InvalidMemoryState), not 0xCE01; the
  workaround is `MapProcessCodeMemory` into staging + `SetProcessMemoryPermission(RW)`, then aliases (types 9 -> 15).
  Permissions on staging can be changed; the notes record that this is not possible on `MapProcessMemory` memory.
- Supplements: a 512 MiB physical staging pool + A/C/E aliases only onto allocated slices do not give 0xCE01 up to
  120 s of operation (6271); exceptions/the page fault handler were not used in our runtime.
- Supplements: prio 0x3B and 0x2C are accepted; core mask 0x1 is accepted (notes: 0x7 available). The effect was not
  measured.
- Supplements: repeated `viOpenDefaultDisplay` -> 0x1272; `eventClear` on the vsync event -> 0xFA01. A wait on it
  takes 14.6 ms (timeout 100 ms). `eventWait` does the reset itself.
- Supplements: NVK Vulkan API = 1.4.98; VI formats 37/44; usage 0x8009F.
- Supplements: the SD log volume is a real brake: line-by-line flush to the SD card (5.2 MB) left the game with no open
  files in 120 s.

# Game facts (independent of the runtime)
- XEX: title 454107F6, media 5E593EE0, base 0x82000000, entry 0x82C5C310, image 20 054 016 B, 267 named imports
  (xboxkrnl 316 + xam 228 entries); `default.xex` 6 270 976 B, LZX.
- Contents: 1196 `.xxx` (1179 UE3 packages, 17 audio/streaming with a different signature), 38 `.wmv` (37 ASF GUID,
  `MOHA_Shell_EAID_DEU.wmv` with a different header). `Coalesced.ini`/`Xbox360TOC.txt` are not plain text.
- Open order at startup (6271, paths `D:\MOHAGame\...`): `Xbox360TOC.txt` (78 447 B), `Config\Xenon\Cooked\Coalesced.ini`
  (308 494 B), `Localization\Coalesced.int` (477 789 B), `CookedMovies\MOHA_Shell_EAID.wmv` (8 797 581 B), then
  `CookedXenon\`: Core, Engine, EAKynapse, EngineFonts, EngineMaterials, EngineResources, GameFramework, IpDrv,
  MOHAEngineMaterials, MOHAGame, MOHAGame_LOC_int, MOHAUISkin, OnlineSubsystemLive, Plasma,
  PostProcessMaterialPackage, Shell, UI_Fonts, UI_MenuModules, UnrealScriptTest (.xxx).
- The first video is the intro WMV; the movie is drawn as 3 R8 planes (Y/U/V) with the game's own shaders (PC: 253
  draws/resolves versus Switch 475 until the watchdog). The first movie on Switch started at uptime 43.5 s (6271).
- Video: the game asks for mode 0x500 x 0x2D0 (1280x720); first VdSwap (PC and Switch v0.5): front buffer 0xFF657000,
  format 6, tiled, pitch 1280, swizzle B,G,R,1; 72 PM4 packets before it, 24 initial draws without output.
- Command ring: 32 KiB (encoded size 12), phys 0x1FCF7000 (observed on PC; in the Switch log v0.5/v0.6 the same
  fetch/ring). The wait at phys 0x1FCF5004 is the frame pacing: interrupt source 1 sets the flip, source 0 (vblank)
  releases it (PC).
- Video initialization: `VdInitializeEngines` (LR 0x829C0788) - the first boundary of v0.4; `VdSwap` LR 0x829B3B08.
- Shaders: XDK containers (magic 0x102A11) inside `.xxx`; 1036 unique found (329 VS + 707 PS) from 1197 files,
  969 translate to SPIR-V (passed spirv-val), 67 do not translate (WMV-internal shaders). This is PC analysis.
- Leaf 0x82C5B7D8 is a time function in ms (GetTickCount-like): intercepted as steady uptime ms, 32 bits,
  16 bytes of code verified (6271).
- Audio initialization: XMA table of 320 contexts x 64 B; PC stops at `XMADisableContext` (LR 0x82997EF8),
  then `NetDll_XNetStartup` (LR 0x82A98A54) - a PC result, not verified on Switch.
- Import counters over 120 s (f4ba): NtAllocateVirtualMemory 341, MmAllocatePhysicalMemoryEx 36, RtlMultiByteToUnicodeN
  1251, KeQueryPerformanceFrequency 51.
