# Vulkan port implementation

The selected target is **Horizon homebrew through Mesa NVK**, not Linux on
Switch. Dependency revisions are pinned in `config/graphics_dependencies.json`.
The existing v0.4 NRO, package and hardware evidence remain historical artifacts.

## Implemented

- A shared native Vulkan device implementation for Windows development and
  loaderless Horizon NVK. It checks the current shader ABI's device features,
  creates a graphics queue, command pool and 1280x720 RGBA8 image with real GPU
  memory, and cleans up resources in dependency order. Initialization and cleanup
  failures retain their operation/result. There is no synthetic frame or draw.
- `VdInitializeEngines` now initializes that native context, rather than returning
  success without a device. Guest interrupt/termination callbacks are registered,
  `VdGlobalDevice` has owned guest storage and the virtual Xenos clock is 500 MHz.
  The existing explicit VGA/720p profile supplies the requested XConfig video
  setting. Disabling system-command-buffer writeback is supported; enabling it
  stops until command retirement exists.
- Actual guest startup on the Windows Vulkan adapter passes the old engine-init
  boundary and reaches `VdInitializeRingBuffer` at LR 0x829B04E8. No command
  processing/drawing is claimed. The next ring address is physical 0x1FCF7000,
  size_log2=12 (4096 bytes). See `host_probe_summary.json` and the entry log.
- A bounded scanner/extractor for uncompressed XDK 0x102A11 shader containers,
  an isolated HLSL/SPIR-V compiler pipeline and a runtime library format/loader.
  Container tables and instruction ranges are checked before translation. Each
  shader's first error is retained; independent shaders continue.
- An Airborne-specific XenosRecomp patch handles reflected sampler arrays as
  separate hardware slots. Upstream only declared the first element. No NFS
  game-specific renderer/shader code was copied.
- Vertex inputs use unique Vulkan locations in original fetch-table order,
  keyed by fetch instruction address. Repeated semantic names do not alias the
  inputs. The runtime library exposes the same location/address/raw metadata.
- A pinned WSL NVK build script and integration into the Switch entry linker.
  It emits a dependency archive/development ELF, not another hardware test NRO.

The complete development target now links successfully: all generated Airborne
CPU code, shared startup services, Vulkan backend and the built Horizon NVK
archive are in `generated/switch-entry-next/airborne-nx-vulkan-dev.elf`
(121,720,480 bytes). Build ID, source hashes and archive hash are recorded in
`switch_entry_next_build_summary.json`. This is compile/link evidence only.

## Current shader evidence

`game_shader_inventory.json` records 1,036 unique bounded containers (329 vertex,
707 pixel) from 1,197 files, with 1,626 source occurrences. Signature matches
whose versions/ranges are invalid are retained as rejected candidates. Compressed
UE3 data is not decoded by this scanner, so this is not total game coverage.

`game_shader_translation.json`: 969 compile, 67 fail. The remaining failures have
missing vertex-fetch/interpolator metadata and all contain embedded WMV shader
source paths. The 12 material failures caused by duplicate semantic names were
resolved with unique fetch locations. Assertions were not disabled.
`game_spirv_validation.json` independently records 969 accepted modules under
SPIRV-Tools for Vulkan 1.2.

`generated/shaders/airborne-shaders.bin` contains the successful modules and
their original container keys. Its header explicitly records partial coverage,
969/1,036. The loader checks sizes, per-record and aggregate checksums, SPIR-V
instruction boundaries and exact original bytes on lookup. Missing shaders return
no match; malformed loads do not replace the old library. Local validation loads
the actual library, checks all 969 lookups and rejects 14 malformed inputs.

Compilation is not Xenos semantic equivalence. XenosRecomp's remaining game
assumptions (vertex formats, constant ABI, special fetch operations,
control flow and exports) must be reconciled with Airborne's render commands.
The library is prepared for runtime use; guest draws do not consume it yet.

## Build

The three checkouts live under ignored `tools/` directories. XenosRecomp's
submodules are required. Apply `tools/patches/xenosrecomp-airborne.patch` to its
pinned checkout. Keep the original MIT notices with reused dependency code.

```powershell
python tools/fetch_graphics_dependencies.py
python tools/extract_game_shaders.py
python tools/translate_game_shaders.py --jobs 6
python tools/validate_game_spirv.py
python tools/pack_game_shaders.py
python tools/build_nvk.py --jobs 6
python tools/build_switch_entry.py --validate-only
```

Translation intentionally exits nonzero while unsupported real shaders remain;
it still writes the complete per-shader report. Packing checks artifact hashes
and packages only successful results with an explicit partial header.

The NVK build uses WSL Clang/LLVM 18, Meson 1.9.2, Rust 1.90.0 (including
`aarch64-unknown-linux-gnu` std and `rustfmt`), bindgen 0.72.1 and cbindgen 0.29.2. Native tools
and cross artifacts are cached under `/root/.cache/airborne-nvk-*`; the staged
archive is `generated/nvk/libvulkan.a`. Dependency setup logs are in `analysis/`.
Meson 1.9's `sanity.rs` name is recognized by a local copy of the upstream Rust
wrapper; real driver targets still compile as ELF AArch64.

## Next implementation boundary

Implement the Xenos command ring and MMIO dispatch, shader selection and Vulkan
draw/pipeline/resource bindings. The NFS port provides useful architecture and
driver references, but its scene hooks, memory addresses, shader corpus and
performance claims do not transfer to Airborne. Do not fake ring consumption,
vblank completion, system buffers or unsupported draw/shader operations.

The first future hardware build must combine actual graphics progress with the
existing shutdown correction and detailed runtime diagnostics. No standalone
device/triangle/exit-only probe is needed. Native Windows Vulkan execution and
successful ARM64 linking are not hardware proof of NVK on this Switch.

Primary references:

- https://github.com/danfromtico/mesa-switch
- https://github.com/FermataRest/nfsmw-nx/blob/66611e3dc6e53f40807561f1e855f1eda241ef4a/docs/native-renderer.md
- https://github.com/hedge-dev/XenosRecomp/tree/990d03b28a27b50277ee5d8d942e1c5f873869d1
- https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc
- https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/xboxkrnl/xboxkrnl_xconfig.cc
