# Project assessment and plan (2026-10-23)

## What works

- GOD container and XEX extraction with hash verification (`tools/extract_god.py`).
- XenonRecomp recompilation: 317 files, ~81 thousand functions, AArch64 build.
- Two jump tables restored (`0x828B6D00`, `0x82CDD594`), XDK clock hook (`0x82C5B7D8`).
- Mesa NVK build for Horizon; frame output through VI works.
- Content packaging on SD, hardware run logs.

## Main problem: the graphics are fitting, not emulation

`gpu_startup_clear` / `gpu_startup_resolve` in `runtime/common/startup_services.inc` (old runtime, removed; `git show 8ca98b1:runtime/`) did not handle
Xenos commands in a general way. Every draw is checked against a previously known one: exact shader microcode words,
exact register values, exact vertex coordinates of the full-screen 1280×720 quad. Everything else is a stop.
This is enough to show a video, but the UE3 menu is hundreds of different draws. This is a dead end.

## Other problems

- All kernel imports (except TLS) go through one global `service_mutex` and a chain of ~114 `strcmp`;
  every call is written to the log. This serializes threads and slows the game down.
- Video passes through the CPU several times per frame: GPU readback → per-pixel write into guest EDRAM memory
  → repeated front buffer decoding on the CPU → output.
- The entire HLE layer is in one 110 KB file with global state.
- Many reports instead of code (`analysis/` — 131 MB of logs and JSON), duplicate numbered builds in `dist/`
  and `generated/` contrary to `AGENTS.md`.

## Key finding

`tools/reference-nfsmw` contains a ready-made port of NFS: Most Wanted (X360 → Switch) based on
[ReXGlue](https://github.com/rexglue/rexglue-sdk): full kernel HLE (xboxkrnl/xam), Xenos emulation
(Vulkan/D3D12), an XMA decoder, a Switch layer (memory, threads 0x3B, exceptions, NVK) and the guide
`docs/porting-another-game.md`. The current runtime was written from scratch in parallel with it.

## Plan

1. **Hygiene.** Git, cleanup of `dist/`/`generated/`, raw logs kept locally.
   **Done in git (2026-10-23):** removed the old runtime `runtime/`, its tests, the old pipeline scripts
   (probe, entry/probe builds, the fitted graphics `prepare_*`/`analyze_*`) and 22 notes in `analysis/`; hardware facts
   consolidated in [HARDWARE_NOTES.md](HARDWARE_NOTES.md). XenonRecomp is kept as an analysis tool (`recompile.ps1`).
   The local `dist/` (7.8 GB), `generated/` (2.5 GB) and raw `analysis/` logs are untouched — awaiting a decision.
2. **Move to ReXGlue (SDK from reference-nfsmw).**
   1. Generate a project for MoHA `default.xex`, carry over the jump tables and the clock hook.
      **Done (2026-10-23).** The SDK is built by `tools/build_rexglue.ps1` (LLVM clang + MSVC/Windows SDK;
      llvm-mingw does not work — FFmpeg and D3D12 in the SDK are built for the MSVC CRT). The project is `rex/`, codegen produces
      473 files (~254 MB of C++, not tracked in git). Manual fixes are in `rex/overrides.toml`: three functions that
      analysis did not reach (`0x82CA6540`, `0x82AA13A8`, `0x82AABB70`, called only by a tail `b` from thunks),
      and both jump tables, keyed by the `bctr` address (`0x828B6D24`: auto-detection read 17 entries instead of 12;
      `0x82CDD5B8`: matches). In total ReXGlue found 469 tables. The clock hook `0x82C5B7D8` was not carried over:
      ReXGlue itself updates `KeTimeStampBundle+16` with a 1 ms timer (`xboxkrnl_module.cpp`), which is what the leaf reads.
   2. Get to the menu and a level **on PC** with Xenos emulation.
      **In progress (2026-10-23).** `tools/build_rexglue.ps1 -App -Config RelWithDebInfo` builds `airborne.exe`,
      `tools/run_rex.ps1` runs it on `extracted/base`. The game passes the splash screens, loads `Shell`/`Menu_test`
      and draws the title screen (D3D12, Xenos emulation) with no FATAL within 75 s. It was necessary to declare 538 functions
      that ReXGlue did not reach: the XEX has no RTTI, and the vtable scanner finds 0 tables
      (`tools/missed_functions.py` → `rex/missed_functions.toml`, 32 data addresses excluded).
      **Dump defects (not the runtime):** `Localization/Coalesced.int` (= `.uke`) is a Russian translation (in the ciphertext
      the signature of the high byte of UTF-16 Cyrillic, 55–200‰ versus 3–10‰ between the other languages), its keys are not found:
      the UI shows `<?INT?UIStrings…?>`. In German (`user_language = 3` in `rex/airborne.toml`) the strings are in place:
      «START-TASTE DRÜCKEN». `CookedMovies/MOHA_Shell_EAID_DEU.wmv` is corrupted (no ASF structure; the other 37
      videos are intact), so the German version skips the EA intro. English requires a clean dump.
      **Menu and level (2026-10-23, user, keyboard):** the main menu can be navigated, the tutorial
      level loads (jumping from the C-47). Artifact: black ragged patches per triangle on characters (the instructor's
      face/torso/legs) — NaN/Inf in lighting or broken vertex data.
      **Backend comparison** (parachute descent scene, `--cl=Tra_Jmp_P` loads the level directly, without the menu):
      D3D12 — colors correct; Vulkan by default (host render targets) — posterization, orange shift, no
      dynamic light in the cabin; Vulkan with `render_target_path_vulkan=fsi` — correct. So the bug is in the host path
      of the Vulkan backend (not the CPU and not the game data); `direct_host_resolve=false` does not help. Both paths store 7e3
      as R16G16B16A16_FLOAT with hardware blending — a per-frame capture (RenderDoc) of D3D12 vs Vulkan is needed next.
      RenderDoc captures (`--renderdoc_capture_at`, descent frame): everything up to the pass with multiplicative blending
      (DstColor/Zero) into the 7e3 target matches; on it (Vulkan e7851 / D3D12 e7417) Vulkan corrupts the multiplier —
      the scene is orange, the shadows are hard patches. Pass input: R32F depth, R32F shadow map 864×864, 64×64 texture.
      **Cause found and fixed:** the pass turned out to be innocent — the culprit was the EDRAM depth→stencil transfer before it.
      Without `VK_EXT_shader_stencil_export` (NVIDIA does not have it) stencil is written in 8 per-bit passes with a kill by mask;
      for a depth source in kStencilBit mode the SPIR-V transfer generator did not fill in `packed` (the depth texture
      is not declared in this mode), the kill was not generated, all bits were set → stencil 0xFF → the R channel of 7e3 at
      maximum. The fix is in `vulkan/render_target_cache.cpp` (SDK patch): `packed = source_stencil` for this
      case. After it the Vulkan host path renders like D3D12.
      **Second Vulkan bug, fixed:** with D24FS8 the host viewport maps guest depth [0,1] to [0,0.5], which does not
      apply to `gl_FragDepth`; D3D12 divides oDepth by 2, the SPIR-V translator does not. The UE3 full-screen depth
      restore pass wrote doubled depth → all light passes (GreaterEqual) and particles (smoke,
      clouds) failed the depth test. The fix is in `spirv_translator_rb.cpp` (SDK patch).
      **Black patches on characters** (both backends, host render targets only; clean on D3D12 ROV): this is a failure of the
      GreaterEqual depth test in the light passes — their VS shifts the mesh by exactly 0.25 px in X and Y relative to the
      prepass (2×MSAA), and on slanted triangles of the skinned mesh this moves the depth ~2–5·10⁻⁶ below the
      stored one (restored from the R32F copy of the prepass). The vertices are clean, the CPU is not involved. `half_pixel_offset`,
      `native_2x_msaa`, `depth_float24_round` do not help. Hypothesis: the 2×MSAA depth resolve takes the wrong sample,
      not the one at whose position the game computes the 0.25 px offset. Not confirmed.
      For Switch: NVK declares VK_EXT_fragment_shader_interlock on GM20B (`cls_eng3d` 0xB197 = MAXWELL_B,
      `nvk_physical_device.c:260`), NAK lowers interlock for SM 5.3 — FSI on the console is available as a fallback path.
   3. Build for Switch with the SDK layer.
      **First run on the console (2026-10-23):** reaches the menu without errors, < 1 FPS (see HARDWARE_NOTES).
      Next: local registers in codegen (`cr/non_volatile/xer/ctr_as_local`, verified on PC), the SDK profile
      `logs/rex/rex_perfil.log`, live log through nxlink (`tools/nxlink.py -s`, Sphaira, 192.168.1.23).
      Known unresolved codegen jumps (they existed before the options, only on exception paths): `b 0x82C46AE0`
      from 0x82C4652C (the `__finally` tail in `__restgprlr_26`), `b 0x82DD1400` from funclet 0x82DD148C.
      **The bottleneck on Switch is EDRAM transfers** (see HARDWARE_NOTES): ~10 ms of GPU per transfer draw at any
      area (on PC ~0.03 ms). Skipping stencil transfers (`vulkan_transfer_skip_stencil`) breaks color: the game stores the low bits of the
      R channel of 7e3 in stencil when reinterpreting EDRAM. The next hypothesis is a slow path in NVK:
      NFSMW was built with mesa-switch at a different commit with its own patch (`tools/reference-nfsmw/mesa/`), we have NVK without it.
   4. Measure; then a custom renderer (UE3 D3D/RHI hooks), like `nfsmw_nativo_*`.
   **Instruction check against the ReXGlue generator (2026-10-23)** — our XenonRecomp fixes versus
   `sdk/src/codegen/builders`. Matching: vctuxs/vcfpuxws128, saturating add/sub/pack (except below), VMX shifts and
   comparisons, blrl, rotates, mfcr/mtcrf, vrfin. Diverging (the number of occurrences in MoHA is in parentheses):
   - `vpkuhus` (6, all in the vD==vB form) and `vpkuwus` (0): byte-wise writing corrupts vB lanes that have not been read yet —
     **a bug, to be fixed in the generator** (temporary vector).
   - `vupkd3d128` (49) / `vpkd3d128` (4) FLOAT16_2: ReXGlue encodes using the extended Xenos range
     (no Inf/NaN, denormals to 0), our tests expected IEEE. Check against Xenia which semantics is correct for the CPU.
   - `frsqrte` (1): no special cases and FPSCR, NaN is negative. `mtfsf`/`mffs` (5/5): FPSCR flags are lost.
   - VSCR.SAT is never set.
   Tests of the old fixes with reference vectors (Xenia golden for FLOAT16_2, frsqrte special cases) were removed together
   with the old runtime; take them from `git show 8ca98b1:tools/tests/`.

3. **Keep from the current work:** the extraction tools, the tables, the clock hook, the NVK build, the packaging,
   information from the hardware logs. Check the instruction implementations against the ReXGlue generator.

## Risks

MoH: Airborne (UE3, 2007) is noticeably heavier than NFS MW (2005). Switch: 3 cores at 1.02 GHz versus 6 Xenon threads
at 3.2 GHz. The menu and levels are achievable; a playable FPS will require long optimization and is not guaranteed.
