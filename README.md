# AirborneNX

A port of the Xbox 360 version of **Medal of Honor: Airborne** to the Nintendo Switch (homebrew), by static
recompilation of the PowerPC code to C++ with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), with the
Switch layer and the native renderer of the NFS: Most Wanted port ([nfsmw-nx](https://github.com/FermataRest/nfsmw-nx),
`tools/reference-nfsmw`).

> **The project is on hold** (October 2026). The game boots, plays its levels and is stable, but heavy scenes run at
> 10-15 FPS on the Switch and the sound stutters there. Why, with the measurements, and how to pick it up again:
> [docs/STATUS.md](docs/STATUS.md).

> The repository contains no game files and no code generated from them. You need your own Xbox 360 dump (a GOD
> container with `default.xex`). The game, the extracted files, the generated C++, shader libraries and builds are
> excluded by `.gitignore`; `dist/` (the SD card bundle) holds game data and is local only.

## State

| | |
|---|---|
| PC (Windows, D3D12 or Vulkan) | boots, menus, levels |
| Switch, Xenos emulation | works, < 1 FPS (the GPU emulation is far too slow there) |
| Switch, native renderer (default) | boots, menus, training jump 23-30 FPS, Adanti (`Hus_M1_P`) 10-30 FPS; no crashes in the last runs |
| Sound | fine above ~20 FPS; stutters in the heaviest scenes (the game's software mixer does not get enough CPU) |
| Text | German by default; English with an English `Coalesced.int` in `override/` (see STATUS.md) |

Documents:

- [docs/STATUS.md](docs/STATUS.md): final state, why the work stopped, what is left, how to resume.
- [docs/HARDWARE_NOTES.md](docs/HARDWARE_NOTES.md): every measurement on the console, run by run.
- [docs/NATIVE_RENDERER_NOTES.md](docs/NATIVE_RENDERER_NOTES.md): the native renderer and what MoH:A needed in it.
- [docs/ASSESSMENT.md](docs/ASSESSMENT.md): the original plan and the open questions it started from.

## Building and running on the PC

Needs VS Build Tools 2022 with the Windows 11 SDK, LLVM (clang 18+) in `C:\Program Files\LLVM`, Python and git;
cmake and ninja come from `tools/toolchain/venv`.

```powershell
# GOD container -> extracted/base (default.xex and MOHAGame/)
python tools/extract_god.py "<dump>/454107F6/00007000/<id>" extracted/base analysis/files.json
powershell -File tools\build_rexglue.ps1         # SDK (with tools/patches/rexglue-airborne.patch) + codegen
powershell -File tools\build_rexglue.ps1 -SkipSdk -SkipCodegen -App -Config RelWithDebInfo
powershell -File tools\run_rex.ps1 -Seconds 60   # log: rex/run.log
# Straight into a level, window screenshots, another GPU backend:
powershell -File tools\run_rex.ps1 -Seconds 50 -ShotsAt 30,48 -ShotDir <folder> "--cl=Tra_Jmp_P" "--gpu_backend=vulkan"
```

## Building for the Switch

```sh
python tools/fetch_switch_sdk.py                 # devkitPro (Linux binaries) into tools/toolchain/nx-sdk
python tools/build_nvk.py                        # static NVK (tools/mesa-switch + tools/patches/mesa-airborne.patch)
wsl --exec bash tools/build_switch.sh 15         # rex/out/switch/airborne.nro (after build_rexglue.ps1 codegen)
python tools/package_switch.py                   # dist/switch/airborne-nx/ and dist/airborne-nx.zip
python tools/nxlink.py dist/switch/airborne-nx/airborne-nx.nro -a <switch ip> -p airborne-nx/airborne-nx.nro \
    -s --log analysis/hardware/live.log -- --cl=Hus_M1_P
```

The game files go to `sdmc:/switch/airborne-nx/game/` (`tools/stage_switch_game.py`); the shader library
(`airborne_shaders.nfsp`, built from the game's shaders, not distributed) next to the NRO.

## Layout

| Path | Contents |
|---|---|
| `rex/` | the ReXGlue project: manifest, hand-made analysis fixes (`overrides.toml`, `missed_functions.toml`), the app |
| `rex/src/native/` | the native renderer (from nfsmw-nx; its cvars and log lines keep their Spanish names) |
| `tools/` | GOD/XEX extraction, build and run scripts, shader tools, NVK, devkitPro SDK, SD packaging, nxlink |
| `tools/patches/` | our changes to the ReXGlue SDK (`rexglue-airborne.patch`) and to Mesa (`mesa-airborne.patch`) |
| `tools/shaders_native/` | the Xenos shader translator (XenosRecomp) with its Vulkan and deko3d modes |
| `config/` | XenonRecomp configuration and pinned graphics dependency versions |
| `analysis/` | XEX image tables and small hardware results (raw logs stay local) |
| `docs/` | status, hardware notes, renderer notes |

## License

GPL-3.0 (`LICENSE`): the project uses the native renderer of the NFS: Most Wanted port (nfsmw-nx, GPL-3.0). The
ReXGlue SDK and our patch to it are BSD-3-Clause; the shader tools based on XenosRecomp are MIT; Mesa is MIT.
