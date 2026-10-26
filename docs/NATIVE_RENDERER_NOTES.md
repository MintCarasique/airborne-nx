# Notes: the NFSMW native renderer and porting it to MoH:Airborne

Source: `tools/reference-nfsmw` (app\ GPL-3.0, sdk\ BSD-3, shaders\ MIT; see `THIRD_PARTY_NOTICES.md`).
Everything below was read from the code; **[inference]** marks my assumptions rather than facts from the code. Identifiers
in the NFSMW code are Spanish (`nativo`, `anillo` = ring, `destinos` = targets, `dibujos` = draws, `ganchos` = hooks).
The app\src\ files are `nfsmw_*` in `tools/reference-nfsmw/app/src/`. Line counts were taken with `wc -l`.

## 1. Architecture

### 1.1 `nfsmw_nativo_*` files (27 551 lines)

| File | Lines | Role |
|---|---|---|
| `nfsmw_nativo_sistema.cpp` / `.h` | 4893 / 40 | Class `SistemaGraficoNativo : IGraphicsSystem` (:708). GPU MMIO range (:455), ring thread `BucleAnillo` (:1623), vblank thread (:1599), PM4 parser `Paquete`/`PaqueteTipo3` (:1757-2200), register mirror `EscribirRegistro` (:1106), `WAIT_REG_MEM`, `EVENT_WRITE*`, `MEM_WRITE`, interrupts, `XE_SWAP` -> `Presentar` (:3509), `EmparejarDibujo` (:3096), reports. |
| `nfsmw_nativo_destinos.cpp` / `.h` | 6155 / 142 | Render targets (C2): `ObtenerDestino` (:3858) = plain VkImages keyed by (EDRAM base, format, pitch); resolve/clear `Copiar` (:~1385), resolve without a copy (image swap), depth `ObtenerProfundidad`, occlusion (`ConsultaOclusion` in sistema :939), frame presentation `Presentar` with a gamma ramp, 3 work slots (commands, fence, 64 MB upload), 64x64 write-back to guest memory (for exposure, :490-493), GPU timings. |
| `nfsmw_nativo_dibujos.cpp` / `.h` | 13074 / 352 | The main part (C3-C6): `Dibujar` (:2191) — draw state from registers, topology, vertices (fetch constants, byte swapping, deduplication), indices (NEON), texture cache (two-level, XXH3 fingerprint of the raw bytes, 2D/3D/cube detailing, mips), samplers (:9871), pipeline key `ClavePipeline` (:1975), `vkCreateGraphicsPipelines` (:11663), background prewarm (:11675-12063), file `cache/nfsmw_nativo_pipelines.bin` (:1129), dynamic constants UBO (:8034). |
| `nfsmw_nativo_ganchos.cpp` / `.h` | 1035 / 214 | "What the ring does not tell": hooks of the shader constructors `sub_8259BC90` (PS), `sub_8259C038` (VS) and `Draw*` write a record (vs, ps, args) into a queue; the ring matches it with `DRAW_INDX` (`EmparejarDibujo`). Mirror of the D3D device registers, `NOP` marker, game-side shadow map "vegetation" filter. |
| `nfsmw_nativo_shaders.cpp` / `.h` | 474 / 113 | Shader identification: loading the `.nfsp` library, parsing the XDK container (format 2005), PS lookup by XXH3 of the microcode with a mask, lists of vertex elements, samplers, PS outputs. |
| `nfsmw_nativo_texturas_pool.cpp` / `.h` | 405 / 238 | Pool of memory suballocations for textures (16 MB blocks), because each `vkAllocateMemory` on Horizon takes ~1.9 ms (comment :1-40 of the .h). |
| `nfsmw_nativo_vertices_dedupe.h` | 200 | Deduplication of vertex uploads within a frame (reset on `WAIT_REG_MEM`, sistema :1912). |
| `nfsmw_nativo_captura.cpp` / `.h` | 193 / 23 | PNG frame capture (debug only). |

Dependencies (not `nativo_`, but part of the native path): `nfsmw_d3d_trace.cpp` (790, XDK D3D map + Draw* hooks),
`nfsmw_d3d_registros_nativo.cpp` (2482, native FlushState dump + marker), `nfsmw_render_targets.cpp` (610, disabling
tiling/MSAA), `nfsmw_espera_anillo.cpp` (312) and `nfsmw_espera_fotograma.cpp` (439) — remove the game's busy-wait,
`nfsmw_shader_library.cpp/.h` (190, `.nfsp` format), `nfsmw_microcodigo_versiones.h` (IM_LOAD cache without memcmp),
`nfsmw_esperas_tiron.h`, `nfsmw_ajustes_graficos.*` (settings), `nfsmw_reflejo_demanda.h`.
Native replacements of hot game functions (all with a "guard"): `nfsmw_material_nativo.cpp` (969), `nfsmw_matrices_nativo.cpp`
(998), `nfsmw_eview_nativo.cpp` (865), `nfsmw_escenario_nativo.cpp` (857), `nfsmw_efecto_pasada_nativo.cpp` (1047),
`nfsmw_pegamento_nativo.cpp` (1393), `nfsmw_d3d_registros_nativo.cpp` (2482).

### 1.2 How this plugs into the SDK

- **The whole graphics system is replaced, not part of a plugin.** `OnPreSetup(RuntimeConfig&)` puts
  `config.graphics = nfsmw::nativo::CrearSistemaGrafico()` (`nfsmw_app.h:53-56`); `ReXApp` loads the GPU plugin (xenos)
  only if `config.graphics` is empty (`sdk/include/rex/rex_app.h:170`, `runtime.h:67-70`). Xenos emulation (CommandProcessor,
  EDRAM, shader translation) is not created at all.
- Interface `rex::system::IGraphicsSystem`: `SetupPresentation` (:719; creates `VulkanProvider` and `VulkanPresenter` from the SDK,
  feature `vulkan_native_shader_features` for the XenosRecomp SPIR-V), `SetupGuestGpu` (:739), `SetInterruptCallback` (:783),
  `InitializeRingBuffer(ptr, size_log2)` (:788), `EnableReadPointerWriteBack` (:797), `Shutdown`, `presenter()/provider()`.
- **The ring is read directly from guest memory** by a host thread (`XHostThread`): `lector.base = memory_->TranslatePhysical(base)`
  (:1699), the write pointer arrives through MMIO `CP_RB_WPTR` (`EscribirRegistroMmio` :872, store-release), the ring waits on a
  condvar for 4 ms; after a pass it writes the read pointer back to guest memory (`store_and_swap`, :1740) and wakes waiters
  (`AvisarProgresoAnillo`). Packets are parsed immediately (types 0/1/2/3, `INDIRECT_BUFFER` indirect buffers, depth is limited).
- MMIO: `memory_->AddVirtualMappedRange(0x7FC80000, ..., LeerMmio, EscribirMmio)` (:455, :751) — without it `REX_MM_LOAD_U32`
  reads garbage. Fixed register values as in the emulation (`graphics_system.cpp:232-262`).
- Bypassed from the SDK: CommandProcessor, `RenderTargetCache`/EDRAM, `PipelineCache`/`SpirvShaderTranslator`, texture cache,
  primitive processor. Kept from the SDK: Presenter (`GuestOutputRefreshContext`), Vulkan device/utilities, memory/xenos.h/
  registers.h (constants, PM4 opcodes), `rex/hook.h` (`REX_HOOK_RAW`, `__imp__sub_X`) for game intercepts.
- **What the game expects from the GPU and what was done by hand** (sistema :6-24): MMIO; read pointer; interrupts (vblank — a separate
  thread; `PM4_INTERRUPT`); GPU writes to memory (`MEM_WRITE`, `COND_WRITE`, `REG_TO_MEM`, `EVENT_WRITE_SHD`, `EVENT_WRITE_ZPD`);
  `SCRATCH_REG` -> `SCRATCH_ADDR`; `WAIT_REG_MEM` is executed by the ring thread with a wait ceiling (:1884-1932).
- Hooks on the game thread are needed because the ring does not carry: VS identity (D3D patches the VS microcode "in place"), the
  Draw -> shaders link (ganchos.h:1-12). For PS the identity is by microcode.
- Synchronization: a fence for each of the 3 slots; `Presentar` on `PM4_XE_SWAP` (VdSwap writes "SWAP", the physical address of the front buffer,
  the size; sistema :1861); the Swap texture is taken from fetch constant 0 (`TexturaSwap`, destinos.h:55).

## 2. Game-specific vs. general

| Component | General (Xenos/PM4/XDK D3D) | NFSMW-specific |
|---|---|---|
| PM4 parser, register mirror, MMIO, interrupts, vblank (`sistema`) | almost everything | only debug reads of `0x82A2D1AC`, `0x82A2CEE4` (:1045-1053); calls to `sub_82597690` (read ptr wait), `sub_82597960` (vblank count) — **XDK D3D addresses** |
| Resolve/clear/target keys (`destinos`) | copy logic (`GetResolveInfo`), gamma ramp, slots | constants: 1600 (shadow map, `kLadoSombras` :5836), GPU categories by size 1600/1280/640/320 (`dibujos.h:63-76`), "scene/reflection/cubemap", the "at least two maps" shadow pair, composition pass `sub_82442478`/`sub_824427F8`/`sub_82442908`/`sub_82448168` (:231-233, :472), 64x64 write-back for auto-exposure |
| Drawing (`dibujos`) | registers, topology, vertices, texture format, untile, pipeline cache, prewarm | a large "cielo" layer (sky, 140 mentions), "sombra" (99), "resplandor" (glow), "vegetacion", "reflejo", SPIR-V glow variants (`nfsmw_nativo_resplandor_*_spirv.h`, not in the repository) |
| Shaders (`shaders`, `shader_library`) | container reading, XXH3, PS outputs | container format 2005 (24-byte header), mask `kFetchConserva` |
| Hooks (`ganchos`, `d3d_trace`, `d3d_registros`) | the idea | all `0x825xxxxx` addresses (XDK D3D, below) |
| Native replacements (`material/matrices/eview/escenario/efecto/pegamento`) | template "native code + guard 50k-200k calls, then 1/4096" | 100 % NFSMW-specific (EA engine), port only the template |
| `render_targets`, `espera_*`, `recorte*`, `sombras_lod`, `escenario_lod` | the idea (remove tiling/MSAA, spin-waits) | 100 % specific |

### 2.1 Hard-coded NFSMW guest addresses (what to look for in the MoH:A `default.xex`)

**XDK D3D map** (`nfsmw_d3d_trace.cpp:144-177`, this is the list of functions to look for in MoH:A; in MoH:A the XDK code lives around
`0x829Axxxx-0x829Bxxxx`, e.g. `sub_829B0E38` = D3DQuery_GetData, `sub_829AF948` fence wait — from our code/notes):
CreateDevice `825A1658`, Swap `825989D8`, SyncToPresentationInterval `82598868`, PresentComposite `82598FC8`,
SetRingBufferParameters `825981E8`, Clear `8259A450`, ClearF `8259A500`, Resolve `82592538`, BeginTiling `825992F0`,
EndTiling `82599680`, SetRenderTarget `8258DB18`, SetDepthStencilSurface `8258DE80`, CreateTexture `8258A0A0`,
CreateSurface `8258A1C0`, SetTexture `8258A648`, Create/Lock VertexBuffer `82595570`/`82595620`, Create/Lock IndexBuffer
`825956D0`/`82595780`, Unlock `82595170`, Release `825953D8`, SetStreamSource `8258D968`, SetIndices `8258DA60`,
Create/SetVertexDeclaration `8259C470`/`8259C3D0`, SetVertexShader `8259C2A8`, SetPixelShader `8259BDC0`,
DrawVertices `82593A10`, DrawIndexedVertices `82593C50`, DrawVerticesUP `82593588`, DrawIndexedVerticesUP `82593940`,
**FlushState `825A40C0`**, SetGammaRamp `8258E138`, QueryBufferSpace `82597DA0`.

**D3D internals needed by the native renderer:**
- shader constructors: PS `8259BC90`, VS `8259C038` (ganchos.cpp:977-989); VS-fetch patcher `825A2FB8` (two calls from
  `825A3AF0` and `825A37D8`), VS output zeroing `825A36A8`, IM_LOAD write `825A3AF0`; dirty register dump `825A2AA0`
  (register mirror); `825A5D18` (read ptr check/GPU hang) and `82597690` (GPU wait) — espera_anillo.cpp;
  internal draw routine `825932D8` (rain drops/VisualTreatment, d3d_trace.cpp);
- occlusion: Issue `8258F810`, End `8258EA28`, GetData `8258F998` (sistema.cpp:930-933), sun occlusion `82225438`;
- offsets in the device object: VS `+0x4FE8`, PS `+0x3290` (ganchos.cpp:~57-58), RenderState tables `+96`, SamplerState
  `+484`, register mirror tree `+0x2000..0x4900` (ganchos.h:46-48), dump register `dev+0x28C0`;
- globals: device pointer `82A2CF60` (d3d_trace.cpp:72), video sizes `82A2CF68/6C/70/74`, output mode `82A2CF80`,
  `82A2D1AC` (renderer, AA mode), `82A2CEE4` (quality), two-thread frame flag `82A2CF40`, command list `82909650`,
  target descriptor table `82A4527C`, D3DPRESENT_PARAMETERS `828FBB70`.

**NFSMW engine specifics (no need to port, only as a reference):** AA modes `82458310` (constructor, mode table
+4/+28/+52/+76/+100), `824402F0` (XGetVideoMode), `82441990`, `82458850`, `8245D320`, `8245D5F8`, `8243C1C8`;
passes `82442478`, `824427F8`, `82442908`, `82443B18` (shadow pass), `82448168`; frame loop `824411B8`, `82445660`,
`82441CC8`, `82441F18`, Sleep(0) points `82441FB4`, `82441D24`, `82441DC4`; hot functions `824511E8`, `824538D0`,
`8243E358`, `824C2850`, `82448E80`, `82452730`, `82454B50`; view table `82A38070`; video decode `828C35D8`, `8278A518`,
`828C58C8`; CRT `826BE610`, `826BE1B0`.

## 3. Shaders

The NFSMW pipeline (`docs/shaders.md`, `shaders/README.md`):
1. Searching for containers by signature (`10 2A 0E 00` PS, `10 2A 0E 01` VS) in all files and in the XEX (`nfsmw_buscar_contenedores.cpp`);
   the scan order determines the names, it must not be changed.
2. Converting container 2005 -> 2008 (`nfsmw_contenedor.h`), because XenosRecomp expects the new format.
3. XenosRecomp (`shaders/XenosRecomp`, patches for `NFSMW_RECOMP`) -> HLSL with the shared `shader_common.h` (`nfsmw_hlsl.cpp`).
4. DXC -> SPIR-V (the script `nfsmw_regenerar_biblioteca_pcf.sh` does everything on the PC; WASM build for the web installer).
5. `nfsmw_empaquetar.cpp` -> `nfsmw_shaders.nfsp`: sorting by fingerprint, a format with an integrity check
   (`nfsmw_shader_library.cpp:78-186`). The library is derived from the game — it is not distributed.

**Key.** `huella = XXH3_64bits(the whole original container)` (library.cpp:78, :153), lookup by binary search + byte
comparison (a collision will not pick the wrong one). The container is stored in the library (`Shader{original, spirv, huella, vertices}`). During the game:
- **PS** is found by the microcode from `IM_LOAD`: XXH3 of the microcode with the mask `kFetchConserva` (shaders.cpp:40) -> `Identificar`;
- **VS** is NOT found by microcode (XDK D3D rebuilds the fetches and zeroes unused outputs), so the constructor hook
  remembers the original container for the object; `ShaderDeObjeto(object)` (ganchos.h) + the `FetchCoherentes`
  check (shaders.h:109); Draw* leaves a VS/PS record, the ring matches by primitive type and vertex count.
- **Missing shader:** `Identificar` returns nullptr, `Dibujar` returns false/`Rechazar` (the reason is logged once), the
  draw is skipped; there is no "substitution with another shader". Statistics `identificados/sin_identificar/ambiguos` (shaders.h:80).
- Variants go through specialization constants (alpha test 8 functions, constants source, 1/size of textures, R11G11B10 normals) and
  are part of the pipeline key. SPIR-V interface: push constants with buffer addresses, sets 0-2 for 2D/3D/cube textures, samplers
  in set 3; constants via a dynamic UBO (gave +18-23 % FPS). The XenosRecomp patch set for NFSMW: `g_NdcScale/g_NdcOffset`
  (screen pixels with `PA_CL_VTE_CNTL=0x400`), normals/tangents as float, `SetpInv`, merging `if (p0)`, `max(a,a)`.

**What we already have:** `tools/extract_game_shaders.py` (XDK containers `0x102A1100/1101` — this is already the "2008" format, no 2005
converter is needed; in our `pack` the key is FNV-1a 64 of the container, in NFSMW it is XXH3, the formats are incompatible:
`AIRSPV01` vs `NFSSPV`), `translate_game_shaders.py` (an isolated process per shader, keeps the first error),
`pack_game_shaders.py` (records: key, sizes, stage, spec-constant mask, crc), the patch `tools/patches/xenosrecomp-airborne.patch`
(86 lines, `shader_recompiler.cpp/.h`); 969 of 1036 shaders produced SPIR-V. **Missing** compared to NFSMW: (a) a note on
whether the MoH:A VS can also be rewritten by the XDK layer — to be checked; (b) metadata in the record (vertex elements, sampler
types, PS outputs, number of `OpKill`, size of the used constants) that NFSMW computes at load time (`EntradaShader`);
(c) mapping of the 67 untranslated shaders (which ones they are and what draws with them).

**XDK D3D in MoH:A (`analysis/d3d_map/identified.md`, verified against the code on 2026-10-23).** Shader constructors:
`sub_829A3158` (PS; r3 = container, returns the object; the pointer to the microcode in the object is at +24) and `sub_829A32A8` (VS;
the container is copied into the object at +872, the fetches are patched by `sub_829A18A8` once at creation; microcode at +32). NFSMW
had +12/+40 and a patcher on every draw (`sub_825A2FB8`) — we do not need a hook for it. The constructor hooks are enabled in
`native_hooks.cpp`. There are no draw records (the Draw* replacements from NFSMW): the ring takes VS/PS from its own identification by `IM_LOAD`
(`UsarIdentidadDelAnillo`); `Identificar` masks the fetch fields, so the VS is also found by microcode. MoH:A containers
are format 2008 (707 PS `0x102A1100`, 329 VS `0x102A1101`); `Leer` and `shader_library` now accept it, the microcode
is taken from `[physicalOffset, +size)` of the physical part. Library: `airborne_shaders.nfsp` next to the exe.

**The title screen matches D3D (2026-10-24, PC).** What else was needed after the first image — all of these are
Xenos/XDK mechanisms that the NFSMW renderer did not have:
- `copy_dest_exp_bias` resolve (UE3: −3) and the fetch constant `exp_adjust` — scaling at sampling (`<sampler>_Escala`,
  shared constants 122..137), the copy stays 1:1;
- EDRAM aliasing: the `*_AS_*` formats — the same image as the base format at the same base;
- when creating a VS, XDK rewrites the swizzle/format of the fetches and may reorder their registers; when linking VS+PS it silences
  unused exports — all of this is outside the shader identity; the original swizzle/register are stored in `ElementoVertice`;
- samplers that are not in the constants table are taken from the microcode tfetch (and declared by the translator);
- barriers around copies/blits (`airborne_barreras_copias`): without them the depth copy overlapped with particles on the desktop;
- gamma textures (sign 3) — via sRGB formats;
- the size of the resolved image = the size of the texture it is read through (pitch 352 vs texture 322 for bloom).
Debugging: RenderDoc (`renderdoccmd capture` + `qrenderdoc --python`), frame trace `nfsmw_nativo_diag_fotograma_s`,
comparison with an emulation capture. With the monitor off, native output does not reach `PrintWindow` — view the frames
through RenderDoc. Open: 6 unidentified shaders (internal D3D shaders for clearing with a rect list — type 8 is still
rejected), 54 shaders with 1D sampling do not compile, the heuristic for choosing a container with identical microcode
(the proper fix is a hook on the UE3 shader creation path), Switch has not been tested yet.

**First image (2026-10-23, PC).** What got in the way and how it was found (RenderDoc + `nfsmw_nativo_diag_fotograma_s`):
- Shaders: most UE3 packages are compressed with LZO1X (flag 2) — `tools/ue3_decompress.cpp` + `tools/ue3_scan_shaders.py`
  give 9989 containers (was 1036). Library: 9692 shaders, ~160 MB (limits raised to 32768/512 MB; for Switch
  lazy loading or compression will be needed later).
- When creating a VS, XDK MoH:A rewrites in `vfetch` not only the format/stride/offset but also the destination swizzle (float3 ->
  w = 1), so the swizzle was removed from the comparison mask, and the original swizzle is stored in `ElementoVertice::swizzle` for
  `g_InputRemap`. Without this, fullscreen quads collapsed to x = 1.
- Identical microcode occurs in containers with different constants tables; XenosRecomp drops a tfetch without a
  declared sampler — we pick the container with the largest number of samplers.
- Predicated tiling: the 1280x720 scene is drawn in tiles of 512 + 208 rows; the second tile is resolved with the base at row 512
  of the same texture — such copies now go into the containing texture.
- 7e3 (and `_AS_16_16_16_16`) on the host is RGBA16F: UE3 writes the scene depth (~3500) into alpha, Xbox clamps it to 2 bits.
  Spec bits 5/6/7/10 ask the PS to apply the 7e3 range to oC0..oC3 (`salida7e3`, needs the library from
  `tools/shaders_native`). Without this the DOF composite takes almost only the blurred image.
- Title screen: tonemapping — UE3 UberPostProcess (PS with BlurredImage/SceneColor/SceneDepth); UI shaders (logo,
  text) are not yet recognized: their containers are apparently among those that failed translation/DXC.

**The NFSMW translator on MoH:A containers (2026-10-23).** `nfsmw_hlsl.cpp` + their XenosRecomp (`-DNFSMW_RECOMP`) build with
clang++ on Windows (`-DNOMINMAX` is needed) and accept our 2008 containers without conversion: 969/1036 to HLSL, build in
`rex/out/native_shaders/`. All 67 failures are vertex shaders without declarations in the container:
- 50: `vfetch` from fetch constant 31 with an empty vertex elements list (manual fetch, not through a vertex declaration);
- 17: export to `o32` (memexport) with an empty interpolator list.
They need a fallback in the translator (format/stride/offset are in the vfetch instruction itself) and memexport support; what
they draw is to be found out from the ring once the native path starts finding shaders.

## 4. Unsupported / limited (important for UE3 + HDR)

From the NFSMW code (check the lines before relying on them):
- **Color target formats:** only `k_8_8_8_8` and `k_8_8_8_8_GAMMA` (destinos.cpp:1412-1415; dibujos.cpp:2308-2315 — otherwise
  `Rechazar(100+format)`). **No `k_2_10_10_10_FLOAT` (7e3), `k_16_16_16_16(_FLOAT)`, `k_2_10_10_10`, `k_32_FLOAT`** — MoH:A
  (HDR 7e3) will hit this right away.
- **Resolve destination formats:** only `k_8_8_8_8`, `_A`, `_AS_16_16_16_16` (destinos.cpp:1436-1443). No 3D/arrays as
  a target (`Rechazar(3)`), no float16/7e3 format, no resolve of a part with an arbitrary subrectangle (it clears the whole thing,
  destinos.cpp:22-23).
- **MSAA:** `RB_SURFACE_INFO` MSAA >1x is simply used as 1x (destinos.cpp:1394-1396, "MSAA hangs NVK on Switch");
  in NFSMW MSAA is also disabled in the game itself (render_targets.cpp: AA modes 3-5 are reduced to mode 2). MoH:A, judging by our
  notes (`ASSESSMENT.md`), uses 2x MSAA with tiled rendering.
- **EDRAM aliasing is not modeled:** one image per key (base, format, pitch) (destinos.cpp:3863). Games that reinterpret
  EDRAM (depth -> stencil transfer, 7e3 <-> depth; this is exactly what choked us in the emulation) require either explicit emulation
  of particular cases or interception at the RHI level. Conclusion: **the main architectural risk for UE3**.
- **Depth/stencil:** D24S8 or D32S8; stencil state is carried over (`dibujos.cpp:3320-3604`), but stencil copies from
  depth, `EDRAM_MODE` other than `kColorDepth` and 5 (depth-only) are rejected (`Rechazar(1)`, dibujos.cpp:2203-2205). The
  `kStencil/kCopy` variants and MRT >4 were not considered. A copy from depth gives a `k_24_8`/`k_24_8_FLOAT` texture (destinos.cpp:1396-1407,
  dibujos.cpp:1704); float24 depth is treated as D24 (precision is lost).
- **Primitives:** line list/strip, triangle list/fan/strip, quad list (-> triangles). No point list, rect list (UE3 uses
  `RectList` for fullscreen passes!), line loop, polygon-as-points/lines, immediate indices (`Rechazar(2,4,100+type)`,
  dibujos.cpp:2265-2286). `tipo == 13` quad list is handled, `RECT_LIST` (type 8) is not. **[inference]** UE3 on Xbox 360 draws
  quad/rect lists for post-processing — add them first.
- **Textures:** 2D/cube/3D (base), 2D mips exist, 3D mips do not; no vertex-fetch textures, no signed/gamma textures (dibujos.cpp:27-29).
  Supported formats: 8, 8_8, 8888, 2_10_10_10, 16, 16_16, 16F, 16_16_16_16(F), 32F..., DXT1/3/5, DXT5A, DXN
  (dibujos.cpp:1724-1790). No k_DXT3A/DXT5A_AS_16, k_CTX1, k_8_8_8_8_GAMMA in the sampler, k_24_8 as an arbitrary source.
- **Vertex formats:** as in the table at :1648-1700 (8888, 2_10_10_10, 10_11_11, 16*, 32*); the rest -> `Rechazar(300+format)`.
- **Memexport / stream-out:** none at all. **Occlusion:** a separate mechanism measuring in host samples from the previous frame
  (`EmpezarOclusion/TerminarOclusion`, destinos.h:64-70, sistema :939), for us it is currently "always visible" (`rex/src/occlusion_queries.cpp`).
  Occlusion on predicated rendering (`PM4_COND_EXEC`, `SET_BIN_*`) — only the bin masks (sistema :2037-2066), conditional execution
  of the ring has not been verified.
- **Tiling (BeginTiling/EndTiling):** not implemented, disabled in the game (render_targets.cpp). UE3 uses predicated tiling
  for 2x MSAA 720p — the same disabling will be needed through the device configuration, or `bin_select` handling.
- **Writing to guest memory:** only small resolves (64x64, 320x180) for NFSMW exposure; MoH:A, **[inference]**, has its own
  CPU reads (reflections, HDR auto-exposure) — its own list will be needed.
- **Gamma/PWL, colorkey:** NFSMW applies the ramp loaded by the game (`SetGammaRamp 8258E138`).
- **Crash checking:** `Rechazar(...)` is present on every unsupported branch, but a "black frame with a clear log"
  is sufficient only for the 8888 paths.

## 5. Proposed order for porting to MoH:A

First milestone: **the MoH:A menu through the native path on PC**.

| # | Step | Size | Notes |
|---|---|---|---|
| 1 | Copy the skeleton: `nfsmw_nativo_sistema/destinos/dibujos/ganchos/shaders/texturas_pool/vertices_dedupe` into `rex/src/`, rename the namespaces, GPL-3.0 license (reconcile with the project `LICENSE`), hook up `OnPreSetup(config.graphics=...)` (we already have `OnPreSetup`: `rex/src/airborne_app.h:58`), CMake. **Check** that our SDK contains `IGraphicsSystem`/`config.graphics`/`VulkanPresenter::kGuestOutput*` (we have `rexglue_setup_target(... GPU_PLUGINS xenos)`) | medium | Our SDK version may differ from `reference-nfsmw/sdk` (that one is a fork with a Switch layer). |
| 2 | MoH:A XDK D3D map: find the counterparts of the list in 2.1 (CreateDevice, Swap, SetRenderTarget, Draw*, FlushState, shader constructors, SetTexture, Resolve). Trace only at first (`nfsmw_d3d_trace.cpp` as a template) on PC | medium | The XDK code is a statically linked 2007 version; the addresses are different. Signatures from the notes: `sub_829B0E38` (GetQuery), `sub_829AF948` (fence wait) |
| 3 | Ring without drawing: PM4 parser, MMIO, interrupts, WAIT_REG_MEM, VdSwap -> showing a test color. The game must reach the menu as with emulation | small-medium | Almost no changes. |
| 4 | Shader library: adapt `EntradaShader`/`Cargar` to our `AIRSPV01` (FNV-1a key over the container -> can be kept, but XXH3 is faster); extend the record with metadata; work out how the MoH:A XDK changes the VS; PS by microcode; VS by constructor hooks | medium | Coverage 969/1036. |
| 5 | 8888 targets + 8888 resolve: only the menu/interface passes at first (UI is drawn in 8888 **[inference]**) | medium | 7e3 and MSAA may not be needed for the menus. |
| 6 | Drawing: RectList primitive type, texture formats, samplers, blend/depth/stencil; pipeline cache. Enable the `Rechazar` statistics and reach an empty "0 rejected" in the menu | medium-large | Correctness on PC first, pipeline cache later. |
| 7 | Take out tiling/MSAA (counterpart of `render_targets.cpp`) via the device/mode settings in UE3 | small-medium | Find the AA mode table. |
| 8 | Level: 7e3/FP16 targets, depth -> stencil transfer, k_24_8 sources, MRT, occlusion, stencil shadows/light | large | Needs a dedicated design: either emulation of EDRAM aliasing for particular cases, or a hook of the UE3 RHI layer. |
| 9 | Native replacements of hot UE3 functions + guards (NFSMW template), profile on Switch | large | Only after hardware measurements (see `AGENTS.md`). |
| 10 | Switch specifics: texture pool, prewarm, NEON untile, threads/priorities | medium | Already in the copied code; check on hardware with a batch build. |

### Main risks
1. **UE3 EDRAM reinterpretation** (depth<->stencil, 7e3): the native model "an image per (base, format, pitch)" cannot withstand it
   (section 4). We have already seen that exactly the depth/stencil transfer breaks the color (`cff5004`).
2. **7e3 and MSAA**: new target/resolve formats and disabling tiling are needed; without them the level will not be drawn.
3. **Vertex shader identity**: the XDK layer patches the microcode; hooks analogous to `8259BC90/8259C038/825A2FB8` are needed in MoH:A.
4. **RectList and other primitives** of UE3 post-processing (not supported in NFSMW).
5. **License**: `app/` is GPL-3.0; the code cannot be pasted in without reconciling our project's license (copying ideas/structures is OK,
   whole files are the project owner's decision).
6. **Measurements**: per `AGENTS.md`, hardware checks are batched; the first milestone is PC only.
