# Status: on hold (2026-10-26)

The port works but is not playable in the heavy parts of the game, and the remaining options cannot close the gap on
this console. This page is the summary to start from when the work resumes. The measurements behind every number are
in [HARDWARE_NOTES.md](HARDWARE_NOTES.md); the renderer is described in [NATIVE_RENDERER_NOTES.md](NATIVE_RENDERER_NOTES.md).

Last build: commit after `8f10715` (see `git log`), Switch NRO packaged by `tools/package_switch.py` with
`guest_audio_thread_priority = 44` and the 32-channel `Coalesced.ini` override. All numbers below: Switch at stock
clocks (CPU 1020 MHz, GPU 460.8 MHz, handheld), 1280x720 unless stated.

## What works

- Boot, movies, menus, level loading; the training jump (`Tra_Jmp_P`) at 23-30 FPS; Adanti (`Hus_M1_P`, the first
  mission) at 20-30 FPS in quiet parts and 10-15 FPS in battles. No crash in the three 5-minute Adanti runs since the render queue fix.
- Native renderer (PM4 ring to Vulkan on NVK, from the NFSMW port) with MoH:A's needs: 7e3 HDR targets, EDRAM
  aliasing (4X, AS formats), resolve exponent bias, sRGB gamma textures, rectangle lists, shaders without containers,
  scene drawn at a lower resolution (`airborne_scene_resolution`).
- Sound through the game's own software mixer, with the hot mix-add kernel in native NEON code (bit-exact, checked
  against the original at run time).
- Recompiled code with the sa2-nx / masseffect-nx generator passes and the Mass Effect runtime waits;
  `-ffp-contract=off` for the game code (it had been fusing multiply-adds that the Xenon rounds twice).
- English text: the Xbox dump's `Coalesced.int` holds Russian text; an English one built from the PC version's
  `.int` values over the Xbox keys (`tools/coalesced.py`, see commit `111c504`) goes to
  `sdmc:/switch/airborne-nx/override/MOHAGame/Localization/`. It is game data and is not in the repository.

## Why the work stopped

1. **Not enough CPU.** A homebrew NRO gets three Cortex-A57 cores at 1020 MHz (core 3 belongs to the system; using it,
   or overclocking, needs system modules this project chose not to require). The Xbox 360 game was written for three
   3.2 GHz PowerPC cores with two hardware threads each, plus dedicated hardware the Switch has to emulate on the
   same cores: the XMA audio decoder and the Xenos GPU's command processor. In Adanti's battles all three cores are
   at 300 % and every thread is short of time:

   | Thread (heavy scene, 10-11 FPS) | Share of a core |
   |---|---|
   | native renderer ring (Xenos PM4 to Vulkan) | 84 % |
   | game main thread | 57 % |
   | UE3 render thread | 54 % |
   | game audio mixer (RWAudioCore Dac) | 54-59 % (with 32 voices; 64-72 % with 90) |
   | XMA decoder | 20-40 % |
   | vertex copies | 7-19 % |

   With the CPU overclocked (1785 MHz, GPU 921.6 MHz; an earlier build, not the default setup) the same level held a
   steady ~18 FPS.

2. **The game draws too much for a command-level translation.** Heavy scenes issue 4,000-4,500 draws per frame. The
   renderer works below the game, on the Xenos command stream, so each draw costs about 16 µs on the ring thread:
   6.5 µs inside NVK, 3.1 µs copying and byte-swapping vertices and constants, 2.3 µs indices, 4.4 µs pipeline
   lookup, textures, passes and state. That is ~60 ms per frame on one core: 10-15 FPS before anything else runs.
   The GPU itself needs only ~34 ms per frame (about 50 % busy).

3. **No source code.** Every change has to be made in the recompiled binary or below it. The engine cannot be told
   to batch, cull or simplify: the UE3 settings that should have cut draws (`StaticMeshCullDistanceBiasBase`,
   dynamic decals) changed nothing measurable, and the 30 FPS frame limit and the 90-voice mixer are the game's own.

4. **The remaining options are small or very large.** Measured and estimated (details in HARDWARE_NOTES.md):
   - NVK patches from nfsmw-nx: upstream mesa-switch already contains most of them; ~0 left.
   - Caching vertices and indices across frames: ~40 % of the copied bytes repeat, but checking them costs a hash of
     the same bytes; about -1 µs per draw.
   - Smaller draw-path optimizations: -0.5 to -1 µs per draw.
   - All of the above: an estimated +15-20 % in the heaviest scenes (to 12-13 FPS).
   - Moving the renderer to deko3d (see below): replaces NVK's 6.5 µs per draw with an estimated 2-3 µs, weeks of
     work; with everything together, an estimated 14-16 FPS in the heaviest scenes. Not 30.

## What a faster console changes

A Switch 2 homebrew environment (when one exists) would change the picture: more and much faster CPU cores
(Cortex-A78C) and a newer GPU. The recompiled code, the runtime and the renderer are plain AArch64 and Vulkan, so
most of this tree should carry over; the parts tied to the Switch 1 (NVK's Horizon backend for the Tegra X1, the
deko3d work, `svc` priorities, core masks) would need a review. The first thing to measure there is the ring
thread's time per draw and the audio thread's share of a core.

## Known issues

- Sound stutters when the frame rate drops below ~20 FPS (the mixer runs out of CPU, it does not wait on anything).
- A rare crash in UE3's render command queue (stale vtable from `sub_82404CD8`) was seen twice; an acquire barrier in
  its reader (`rex/src/render_queue.cpp`) fixed it in the runs since, but three runs do not prove it gone.
- The WMV movies are garbled with the native renderer on the PC (the Switch draws them).
- 54 of the 10,066 game shaders do not translate (1D textures, 3D fetches on 2D samplers); none showed up as missing.
- `airborne_scene_resolution` sizes must be multiples of 80x16 (a 912-wide scene left a black stripe).

## How to resume

1. Read this page, then the last sections of HARDWARE_NOTES.md (the 2026-10-26 runs).
2. Rebuild: README.md has the PC and Switch steps. `tools/build_rexglue.ps1` applies `tools/patches/rexglue-airborne.patch`
   to `tools/reference-nfsmw` (nfsmw-nx at `66611e3`); `tools/build_nvk.py` applies `mesa-airborne.patch` to
   mesa-switch at `0ff5207`.
3. Measuring: `tools/nxlink.py ... --profile_stacks=true` streams the log and the SDK profiler's 1 kHz stack samples;
   keep the ELF of each build (`rex/out/switch/airborne`) to symbolize them. The console's FTP (Sphaira, port 5000)
   gives the SD card's logs (`logs/rex/rex_crash.log`, `result.txt`) and takes `override/` files.
4. deko3d, step P0 is done (commit `8f10715`): the shader translator has the nfsmw-nx deko3d mode, and
   `tools/build_shader_library_dk.sh` builds the deko3d library; all 77,900 default variants of the 10,012 shaders
   compile to DKSH with uam (0 failures). The next steps (device and presenter, then the renderer backend behind the
   draw-request boundary of nfsmw-nx's `deko3d-implementation` branch, with MoH:A's target features ported to it)
   follow that branch's `docs/HowToDeko3d.md`. The SDK side of the deko3d build mode (`REXGLUE_SWITCH_DEKO3D`) is
   already in our SDK patch.
5. Keep the renderer's Spanish identifiers until a merge with nfsmw-nx is no longer wanted: every rename is a conflict.
