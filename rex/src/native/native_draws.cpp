// nfsmw - native renderer, steps C3, C4, C5c and C6 (see native_draws.h).
//
// What it covers
//   - Shaders: modules of the NFSSPV library with the XenosRecomp interface
//     (shader_common.h and shader_recompiler.cpp): push constants with three
//     buffer addresses (VS, PS and shared constants), 2D, 3D and cubemap
//     textures in sets 0-2 and samplers in set 3, with no size limit;
//     specialization constant 0 with R11G11B10 normals (bit 0) and alpha test (bit 1).
//   - Vertices: the input comes from the fetches of the VS patched by D3D, each
//     element found by its destination register (C5b). Guest data is converted
//     to host words with the fetch constant's byte order, like the emulation
//     (spirv_translator_fetch.cpp), and uploaded to the frame buffer.
//   - Indices: VGT_DMA_BASE/SIZE or automatic, with VGT_INDX_OFFSET. Quad lists
//     become triangles v0 v1 v2 / v0 v2 v3 (primitive_processor.cpp).
//   - 2D textures by fetch constant: linear or tiled, formats 8, 8_8, 8888,
//     2_10_10_10, 16, 16_16, 16F, 32F, DXT1/3/5, DXT5A and DXN, with the fetch
//     constant's swizzle in the view. C2's resolved textures are sampled as they
//     are. The content is compared once per frame. The game's mip levels are
//     included (nfsmw_nativo_mipmaps), packed as on the Xbox 360; without them,
//     foliage and asphalt looked grainy in the distance.
//   - State: blending, color mask, depth, stencil, culling, viewport (without the
//     half pixel: the shader adds it, g_HalfPixelOffset) and window scissor.
//   - Clipping disabled (draws in pixels, like the videos): viewport the size of
//     the render target and the transform in the VS with g_NdcScale/g_NdcOffset,
//     which only the library regenerated with the NFSMW XenosRecomp has.
//
// Not covered (rejected or substituted, with the cause logged once)
//   Mips of 3D textures, vertex textures, signed or gamma textures, points,
//   rectangle lists, line loops and vertex formats without a direct Vulkan
//   equivalent.

#include "native_draws.h"
#include "shaders/rect_list_gs.h"  // AirborneNX: rectangle lists
#include "native_targets.h"  // FormatoDestinoColorSoportado
#include "stall_waits.h"

#include "native_vertex_dedupe.h"
#include "native_texture_pool.h"

#include "graphics_settings.h"
#if __has_include("nfsmw_nativo_resplandor_energia_spirv.h") && __has_include("nfsmw_nativo_resplandor_suave_spirv.h")
#include "nfsmw_nativo_resplandor_energia_spirv.h"
#include "nfsmw_nativo_resplandor_suave_spirv.h"
#else
// The two variants of the glow bright pass are derived from a game shader and are not distributed with the
// sources. Without them the glow setting keeps the game's own shader.
#define NFSMW_NO_GLOW_VARIANTS 1
static const uint32_t kSpirvGlowEnergy[1] = {0};
static const uint32_t kSpirvGlowSmooth[1] = {0};
#endif
#include "native_shaders.h"
#include "native_hooks.h"  // nfsmw_d3d_vegetacion_juego
#include "reflection_on_demand.h"  // nfsmw_reflejo_visibilidad
#include "shader_library.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>
#include <rex/ui/vulkan/util.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <fstream>
#include <set>
#include <string>
#include <system_error>  // the bind thread
#include <unordered_map>
#include <unordered_set>
#include <vector>

/*
 * xxHash reads with memcpy (XXH_FORCE_MEMORY_ACCESS 0). The method it picks for GCC (1) reads through
 * 64- and 32-bit pointers without may_alias, and GCC may move that read ahead of the write of the data
 * being hashed (strict aliasing). The texture key read claves[4] before writing it, and the same texture
 * was created several times. Same hash values; on AArch64, the same LDR.
 */
#if defined(XXH_IMPLEM_13a8737387)
#error "xxhash.h ya se ha incluido con su implementacion antes de este punto: XXH_FORCE_MEMORY_ACCESS 0 llegaria tarde"
#endif
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

#if REX_PLATFORM_SWITCH
// Only for RexSwitchSetCurrentThreadPriorityOk (texture bind thread). That header deliberately does not
// include switch.h (same as in nfsmw_nativo_sistema.cpp).
#include "core/threading_switch.h"  // SDK src/ is on the include path (rex/CMakeLists.txt)
#endif

/*
 * p_000139, the final composite, is a single full-screen quad with twelve texture samples: 2.8-3.2 real
 * ms, 72-80 % of all post-processing. Seven of those twelve are the radial speed blur. Turning it off
 * removes them and leaves the center tap.
 */
/*
 * Vegetation in the shadow map.
 *
 * The four alpha-tested pixel shaders of the shadow pass (n36, n68, n99, n103) are trees, bushes and wire
 * fences. Measured on PC with an A/B test: they are 55 % of the pass's draws but only 12 % of its
 * triangles.
 *
 * The pass cost follows the triangle count, and removing them is worth -1.2 ms of GPU but -3.1 ms of CPU,
 * which is also at 96 % of a core.
 *
 * The right constant is 0.556 real ms per 10,000 triangles, not the 0.80 first estimated (fit over 17
 * race intervals with a constant open area, r2 = 0.982; see nfsmw_recorte_sombras.cpp). With it, 12 % of
 * the triangles would be -0.44 ms, so about -0.76 ms of the -1.2 ms measured here is not geometry: it is
 * the vegetation's alpha pixel shader, which in the shadow map runs in full only to produce the cutout.
 *
 * What is lost: trees and fences stop casting shadows. Buildings, cars and the road keep theirs. It shows
 * on a forest track.
 */
/*
 * Enabled by default. It used to be enabled only by a line in a local toml, and the toml overrides the
 * default: shipping without that line would silently lose the improvement. Three cvars were found in
 * that state; one of them was worth 2 ms and had been inactive for six builds.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_mosaico_rapido, true, "NFSMW",
                    "Renderizador nativo (24/09, build 167): desenmosaicado de texturas de 16 en 16 bytes con el "
                    "calculo de la fila hecho una vez. Los primeros 2000 niveles se comprueban contra el de siempre "
                    "y, si uno difiere, se apaga solo. false = el de siempre, bloque a bloque");

/*
 * The FramebufferDe cache forgets destroyed views. It is keyed by handle values (render pass, 5 views
 * and size) and used to release nothing until the destructor. When C2 destroys an image (ObtenerResuelta
 * recreates it with another size or format at the same address, and those textures end up as render
 * targets through image swaps), its framebuffers stayed behind and, if Vulkan gave the same handle to a
 * new view, a framebuffer created on the dead view was returned. Destruir now notifies through
 * OlvidarVista and the framebuffers that use the view are destroyed. false = previous behaviour (nothing
 * is forgotten).
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_framebuffers_olvidan_vistas, true, "NFSMW",
                    "Renderizador nativo (25/09, build 184): al destruir una imagen de C2 se destruyen los "
                    "framebuffers que usan su vista, para que un handle reutilizado no devuelva uno viejo. false = "
                    "como antes");

REXCVAR_DEFINE_INT32(nfsmw_nativo_huellas_kb_fotograma, 6144, "NFSMW",
                     "Renderizador nativo (24/09, build 165): KB de texturas ESTABLES que el anillo vuelve a "
                     "comprobar como mucho en un fotograma; las que no caben esperan 1-2 fotogramas (8 veces "
                     "seguidas como mucho). Reparte las comprobaciones que coincidian en el mismo fotograma. "
                     "0 = sin limite, como antes");

/*
 * Sampled recheck of stable textures. See PrepararTextura and HuellaMuestra.
 * Enabled by default: its guard verifies itself and switches off at the first mismatch.
 */
REXCVAR_DEFINE_INT32(nfsmw_nativo_huellas_muestreo, 8, "NFSMW",
                     "Renderizador nativo (25/09, build 170): las texturas ESTABLES se recomprueban con la huella de "
                     "una muestra (primer y ultimo bloque de 4 KB y 1 de cada 8, base y mips) en vez de con todos sus "
                     "bytes; si la muestra cambia se sigue por el camino completo. N = 1 de cada N recomprobaciones de "
                     "cada textura sigue siendo completa, y las 3000 primeras hacen las dos huellas: con un solo "
                     "desacuerdo se apaga solo. 0 = siempre la huella completa, como antes")
    .range(0, 64);

REXCVAR_DEFINE_BOOL(nfsmw_sombras_sin_vegetacion, true, "NFSMW",
                    "No dibujar en el mapa de sombras lo que lleva prueba de alfa: arboles, arbustos y "
                    "vallas de alambre. Son el 55 % de los dibujos del pase y el 12 % de sus triangulos, "
                    "asi que ahorra poca GPU (-1,2 ms) y bastante CPU (-3,1 ms). Los arboles dejan de "
                    "proyectar sombra");

/*
 * Radial blur of the final composite, removed by default (true).
 *
 * The Xbox 360 blurs the screen edges at speed (visible at 200 km/h in a capture of the 360 version),
 * and without it the port looks sharper. Restoring it was measured in a race: with the blur and the
 * 9-sample PCF the GPU went from 25.1 to 27.3 real ms per frame and became the bottleneck again, with
 * twice as many frames of 36 ms or more (4.9 % vs 9.6 %) and no streak above 40 FPS. The difference is
 * hard to notice next to the 360 footage, so it stays removed. false = keep the blur, as on the Xbox 360.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_sin_desenfoque, true, "NFSMW",
                    "Quitar el desenfoque radial de la composicion final. Son 7 de los 12 muestreos del "
                    "unico cuadrilatero a pantalla completa del posproceso (1,5-2,0 ms reales). Se pierde "
                    "el difuminado de los bordes al acelerar y con el NOS: la imagen queda mas nitida");

REXCVAR_DEFINE_BOOL(nfsmw_nativo_omitir_sombras, false, "NFSMW",
                    "Renderizador nativo (prueba de FPS): no graba los dibujos del mapa de sombras "
                    "(destino solo de profundidad de 1600 o mas de pitch); recorte visible");
// Enabled by default. On the console (A and B alternating every 30 s), the UBO intervals run 18-23 %
// faster than the neighbouring pointer intervals at the same draw load.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_constantes_ubo, true, "NFSMW",
                    "Renderizador nativo: los shaders leen sus constantes de UBO dinamicos (banco de constantes en "
                    "Maxwell) en vez de por puntero de 64 bits. Mismos bytes: no cambia la imagen. Necesita la "
                    "biblioteca de shaders con SPEC_CONSTANT_CONSTANTES_UBO; false vuelve al puntero");
REXCVAR_DEFINE_INT32(nfsmw_nativo_constantes_ubo_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (prueba): con N > 0 alterna las constantes entre puntero (tramos pares) y "
                     "UBO (tramos impares) cada N segundos y anota cada cambio, para comparar capturas de una escena "
                     "quieta en la misma ejecucion");
/*
 * Descriptor set 4 by differences (the work is done in NVK, see mesa/parche_nvk_set4.py).
 *
 * vkCmdBindDescriptorSets for set 4 costs 1.3-1.4 us per call in a race and happens in 70-78 % of the
 * draws ("C6 subetapas" report), and it leaves four cbuf rebinds for the Draw. Almost all of it is NVK:
 * four writes to the root table (one per word of the dynamic descriptors, even when only the low part of
 * the address changes) and an 80-slot walk that dirties every cbuf of the set even when only one offset
 * changed. With the patch, NVK sends only the words that change and rebinds only the cbufs whose
 * descriptor changes. It is exact (the GPU ends up with the same bindings), and NVK checks it against
 * what it really bound in each slot: on a DIFERENCIA it fixes that draw, switches off for the session and
 * ControlSet4 reports it in the log as an error.
 * This cvar requests or withdraws it from NVK on each submission; with an unpatched Mesa (or on PC) it
 * does nothing. false = NVK binds the whole set, as usual.
 */
// Enabled by default. Measured: 1 root-table write per bind instead of 4, although the set 4 bind still
// costs 1.31-1.45 us (1.29-1.63 before). At one point the NVK guard seemed to check no cbuf at all; that
// was a bug in its own sampling ("1 in 4,096 draws of the buffer", and no buffer reaches 4,096 draws),
// not a sign that there was nothing to check. In its first ~40 s it checked 386,271 cbufs without a
// single difference, and the Draw rebound 38-45 % fewer cbufs. The patched Mesa (p03 in
// mesa/c186_dibujo) samples with the process-wide count.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_set4_diferencias, true, "NFSMW",
                    "Renderizador nativo (25/09, build 184): el conjunto 4 (constantes por UBO) por diferencias en "
                    "NVK: solo se mandan y se reenlazan los descriptores que cambian. Mismo resultado (se comprueba "
                    "sola). Necesita el Mesa con parche_nvk_set4; false = como siempre");
REXCVAR_DEFINE_INT32(nfsmw_nativo_set4_diferencias_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (prueba, build 184): con N > 0 alterna el conjunto 4 por diferencias "
                     "(tramos impares) y como siempre (tramos pares) cada N segundos y anota cada cambio, para "
                     "comparar C6 subetapas en la misma ejecucion. 0 = no alterna");
/*
 * The draw path in NVK (Mesa with p01-p04 and p06 from mesa/c186_dibujo).
 *
 * The ring is the bottleneck, and ~3.4 us of each draw is spent inside NVK: vkCmdDrawIndexed 1.4-1.6 us
 * per call, vkCmdBindPipeline 3-3.7 and set 4 1.3 ("C6 subetapas" report). Four exact improvements (the
 * GPU receives the same commands with the same data), each with its own guard in NVK that compares
 * against the usual path for the first 20,000 uses and then 1 in 1,024, and switches off on a DIFERENCIA:
 *   - emission: each command written in one go (Draw, cbuf rebinds, root table, BIND_VB), same bytes;
 *   - cbufs: only the slots that can get dirty, and no cbuf flush when nothing is dirty;
 *   - dynamic: only the dynamic state groups with dirty bits;
 *   - prefetch: cache hints (PRFM) for the pipeline and shaders before they are used; the app also hands
 *     the pipeline to NVK as soon as it knows it (after PipelineDe), several us before vkCmdBindPipeline.
 * Estimated for the four together: 0.6-1.5 us per draw (~1.1-2.8 ms of ring time per frame). The
 * per-part measurement (CNTPCT clock, 1 in N calls) gives the real breakdown in "C6 NVK por partes".
 * With an unpatched Mesa (or on PC) none of this does anything.
 */
REXCVAR_DEFINE_INT32(nfsmw_nativo_nvk_medir, 64, "NFSMW",
                     "Renderizador nativo (26/09, build 186): mide por partes dentro de NVK 1 de cada N llamadas "
                     "(potencia de 2; C6 NVK por partes cada 10 s). 0 = no mide")
    .range(0, 4096);
REXCVAR_DEFINE_BOOL(nfsmw_nativo_nvk_medir_fallos, false, "NFSMW",
                    "Renderizador nativo (prueba, build 186): lee antes lo que va a usar el enlace de pipeline y el "
                    "flush de shaders y lo mide aparte (cuanto son fallos de cache). Hace trabajo de mas: solo para "
                    "medir");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_nvk_emision, true, "NFSMW",
                    "Renderizador nativo (26/09, build 186): NVK escribe cada orden del dibujo de una vez, con los "
                    "mismos bytes (se comprueba sola). false = como siempre");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_nvk_cbufs, true, "NFSMW",
                    "Renderizador nativo (26/09, build 186): NVK solo mira las casillas de cbufs que pueden ensuciarse "
                    "y se salta el flush de cbufs si no hay nada sucio (se comprueba sola). false = como siempre");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_nvk_dinamico, true, "NFSMW",
                    "Renderizador nativo (26/09, build 186): NVK solo emite los grupos de estado dinamico con bits "
                    "sucios (se comprueba sola). false = como siempre");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_nvk_precarga, true, "NFSMW",
                    "Renderizador nativo (26/09, build 186): pistas de cache (PRFM) del pipeline y de los shaders antes "
                    "de usarlos, y el pipeline pedido tras PipelineDe. No cambia nada. false = sin pistas");
REXCVAR_DEFINE_INT32(nfsmw_nativo_nvk_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (prueba, build 186): con N > 0 alterna las cuatro mejoras de NVK (tramos "
                     "impares encendidas, pares apagadas) cada N segundos, para comparar en la misma ejecucion. "
                     "0 = no alterna");

#if REX_PLATFORM_SWITCH
/*
 * The state of set 4 by differences lives in NVK (struct nvk_switch_set4 in nvk_cmd_buffer.h,
 * mesa/parche_nvk_set4.py). Same field order and types, with a version: it is a contract. Weak symbol:
 * with an unpatched Mesa the address is null and ControlSet4 reports it once.
 */
extern "C" {
struct NvkSwitchSet4Counts {
  uint64_t difference_bindings;
  uint64_t complete_bindings;
  uint64_t root_writes;
  uint64_t root_dwords;
  uint64_t dirty_cbufs;
  uint64_t saved_cbufs;
  uint64_t draws;
  uint64_t checks;
  uint64_t differences;
};
struct NvkSwitchSet4 {
  int32_t version;
  int32_t request;
  int32_t environment;
  int32_t disabled;
  NvkSwitchSet4Counts total;
};
extern NvkSwitchSet4 nvk_switch_set4 __attribute__((weak));
}
static_assert(sizeof(NvkSwitchSet4) == 16 + 9 * 8, "NvkSwitchSet4 tiene que medir lo mismo que en NVK");
#endif
#if REX_PLATFORM_SWITCH
/*
 * The draw path in NVK (struct nvk_switch_dibujo in nvk_cmd_buffer.h, p01 in mesa/c186_dibujo). Same
 * field order and types, with a version: it is a contract. Weak symbols: with an unpatched Mesa the
 * addresses are null and ControlDibujoNvk reports it once.
 */
extern "C" {
struct NvkSwPart {
  uint64_t times;
  uint64_t ticks;  // CNTPCT_EL0, a ticks_por_segundo
};
struct NvkSwImprovement {
  int32_t request;  // written by the app: 1 yes, 0 no
  int32_t disabled;  // 1: its guard saw a DIFERENCIA
  uint64_t uses;
  uint64_t validated_items;
  uint64_t differences;
  uint64_t without_check;
};
struct NvkSwitchDraw {
  int32_t version;
  int32_t do_measure;
  int32_t measure_failures;
  int32_t environment;
  uint64_t per_second_ticks;
  NvkSwPart parts[16];   // enum nvk_sw_parte
  uint64_t counts[13];    // enum nvk_sw_cuenta
  NvkSwImprovement improvements[5];  // emission, cbufs, dynamic, set4_rapido (not applied), prefetch
};
extern NvkSwitchDraw nvk_draw_switch __attribute__((weak));
void vk_switch_precargar_pipeline(VkPipeline pipeline) __attribute__((weak));
}
static_assert(sizeof(NvkSwitchDraw) == 24 + 16 * 16 + 13 * 8 + 5 * 40, "NvkSwitchDibujo: el mismo tamano que en NVK");
#endif
/*
 * Sampler caches valid across frames.
 *
 * With three work slots, this cache once turned the race leaderboard (names and distances) into a smear;
 * with two slots it did not show, and disabling the cache with three slots made the text perfect again.
 *
 * The cause: two paths set no validity horizon at all. Resolved textures (a render target read back as a
 * texture) set `valido_hasta = UINT64_MAX`, "valid forever while the generation does not change", and
 * the race leaderboard is exactly that: a render target the game rewrites every frame. With three work
 * slots the CPU runs two frames ahead and the descriptor slot of an old view was reused, hence the smear.
 * With two slots the distance was not enough for it to show.
 *
 * Fixed by setting `valido_hasta = fotograma_` on those two paths (see the comments in PrepararTextura).
 * The other paths were already tied to `textura.siguiente`, the same policy that decides when the content
 * is rechecked: nothing that was there before is relaxed.
 *
 * What it buys: the cold path of the `texturas` stage costs 13.5 us and runs 266 times per frame =
 * 3.6 ms. The hot path (2,893 hits) is indistinguishable from zero. With the cache valid across frames,
 * the cold ones drop to those expiring in that frame (~45-70), i.e. 2.6-3.0 ms.
 *
 * What to check, in motion (not paused): the race leaderboard with names and distances, the HUD and the
 * rear-view mirror. If anything looks blurry or delayed, disable this cvar in the toml.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_cache_texturas_entre_fotogramas, true, "NFSMW",
                    "Renderizador nativo: las caches de samplers valen entre fotogramas (build 128) mientras no toque "
                    "comprobar el contenido de la textura. false: caducan en cada fotograma, como antes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Uploads the game's mip levels. With only the base level of each texture, distant surfaces looked grainy
// compared with the Xbox 360. This also fixes the base level of small textures with packed mips, which
// does not start at the base address.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_mipmaps, true, "NFSMW",
                    "Renderizador nativo: sube los niveles de mip que trae el juego (como en la Xbox 360). false: solo "
                    "el nivel base, como antes de la build 136")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// A mip level read from the wrong place (packed tail offset, row or slice alignment) causes no Vulkan
// errors, only smudges in the distance. The game's mips are reductions of the base level, so their
// average color has to resemble the base level's.
// The texture cache used to release nothing. On the console (8 laps) it went from 62 MB at the start of
// the race to 392 MB after 12 minutes without levelling off (2,578 textures); with mipmaps it grows 25 %
// faster. Above the limit, the textures unused for the longest are evicted.
// 384 MB was once too much for the console: the cache reached 313 MB after 14 minutes and the GPU ran out
// of memory before getting near the limit: nvMapCreate failed even for 64 KB and the screen went black
// with the audio still playing. The limit was then lowered to 192 MB, to leave room for the render
// targets, guest memory and the rest.
/*
 * 192 -> 384, based on the GPU memory budget report.
 *
 * On the console: "monton 0 (GPU): 478 MB usados de 1375 MB presupuestados (tamano 2391 MB); la cache
 * de texturas lleva 150 MB de 192". So more does fit: almost 900 MB of the budget is untouched.
 *
 * And the 192 limit was doing harm: "cache de texturas cerca del limite: se sueltan frias a goteo (200
 * en total)" in every report, with 53 MB of texture uploads every 10 s. That is evicting textures only
 * to upload them again right away, and each re-upload is a spike on the ring thread. It matches the
 * stutters: 3.5-5 % of the frames exceed 50 ms.
 *
 * The reason for lowering it to 192 still stands (the GPU ran out of memory after 14 minutes with the
 * cache at 313 MB), but that was with 384 and without the emergency path that exists now (stop the GPU,
 * release half the cache and retry). With 478 of 1375 MB used, 384 leaves a 700 MB margin.
 */
/* 384 -> 512 on the Switch, with nfsmw_resolucion_interna = "automatico" (720p handheld and 1080p docked).
 * In races at 1024x576 the cache already reached 272-281 MB, and at 720p and 1080p the resolved render targets
 * are larger, so the resolution and the limit go up together: raising only the resolution once filled the cache,
 * and releasing textures to upload them again caused stutters. GPU memory on the console: 514 MB used of
 * 1,492 MB budgeted. nfsmw.toml has the same value. */
#if REX_PLATFORM_SWITCH
constexpr int32_t kMbMaxTexturesPerDefault = 512;
#else
constexpr int32_t kMbMaxTexturesPerDefault = 384;
#endif
REXCVAR_DEFINE_INT32(nfsmw_nativo_texturas_mb_max, kMbMaxTexturesPerDefault, "NFSMW",
                     "Renderizador nativo: MB de texturas a partir de los que se sueltan las que llevan mas tiempo sin "
                     "usarse (al menos 120 fotogramas), hasta bajar al 75 %. Si el juego las vuelve a pedir, se suben "
                     "otra vez. 0 = sin limite, como antes de la build 144")
    .range(0, 4096)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Creating a new texture without stalling the ring thread.
 *
 * With the memory pool, creating a texture no longer asks the system for memory, but vkBindImageMemory
 * still makes two ioctls: the plane's VA with its pte_kind and the mapping of the pool chunk
 * (nvk_image.c:1696-1710; all our textures are tiled with pte_kind GENERIC_16BX2,
 * nil/image.rs:439-440). In a race that is ~0.6 ms of wall time per texture, and entering a new zone
 * brings 33-45 at once: 19-27 ms of stalled ring in that frame.
 * With this, the ring does the CPU part and the bind thread does vkBindImageMemory while the ring carries
 * on with the draws; before closing the upload buffer it waits for whatever is missing and records the
 * barrier and the copy in that same buffer. The GPU receives the same thing in the same submission: no
 * placeholder textures and no lower mips.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_texturas_enlace_hilo, true, "NFSMW",
                    "Renderizador nativo (25/09, build 184): el vkBindImageMemory de las texturas nuevas (reserva de "
                    "direccion y mapeo en Horizon, ~0,6 ms cada una) va en un hilo aparte y el anillo sigue grabando; "
                    "antes de enviar se espera lo que falte. Mismo resultado (se comprueba sola). false = en el anillo")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_nativo_texturas_enlace_hilo_prioridad, 0x2D, "NFSMW",
                     "Renderizador nativo (build 184): prioridad del hilo de enlaces de texturas. 0x2D, la del anillo: "
                     "por encima del invitado (0x3B) para que un ioctl terminado no espere nucleo, y fuera de las "
                     "franjas del audio (0x2B) y de la presentacion (0x2C). Casi todo el tiempo duerme en el ioctl")
    .range(0x2C, 0x3B)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Hashing and untiling of new textures, off the ring thread.
 *
 * During zone-change stutters the ring spends 5.3-8.7 ms of the frame on new textures: the XXH3 of guest
 * memory (~37 %), untiling and byte swapping (~63 %, 1.8-3.1 ms per MB), plus the copy to the upload
 * buffer. With this, the ring only copies the bytes the hash covers (the same memory at the same point as
 * the inline path: a snapshot) and a thread does the hash, the untiling, the byte swap and the copy to the
 * upload buffer on that copy. Before submitting, the ring collects whatever is missing and does itself
 * whatever the thread has not started. The barrier and the copy are recorded where they always are: the
 * GPU receives the same bytes in the same submission, with no placeholder textures and no frame of delay.
 * It verifies itself (observing phase, then 1 in 128) and switches off with REXLOG_ERROR on any
 * DIFERENCIA. false = everything on the ring, as before.
 */
// Disabled by default. Its guard tripped at start-up (3 jobs not collected when switching upload buffers)
// and switched it off; also, analysis showed that texture creation causes no race stutter (56 us per
// texture on the ring) and that the zone-change stutter is limited by the game itself.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_texturas_huella_hilo, false, "NFSMW",
                    "Renderizador nativo (26/09, build 185): la huella (XXH3), el desenmosaicado y el orden de bytes de "
                    "las texturas nuevas los hace un hilo aparte sobre una copia de la memoria del invitado tomada en el "
                    "anillo; antes de enviar se recoge lo que falte. Mismo resultado (se comprueba sola). false = en el "
                    "anillo, como antes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_nativo_texturas_huella_hilo_prioridad, 0x2E, "NFSMW",
                     "Renderizador nativo (build 185): prioridad del hilo de huellas de texturas. 0x2E: por debajo del "
                     "anillo (0x2D), que lo desaloja en cuanto tiene trabajo, y por encima del invitado (0x3B), que asi "
                     "no lo deja sin nucleo; fuera del audio (0x2B) y de la presentacion (0x2C)")
    .range(0x2C, 0x3B)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_nativo_texturas_huella_hilo_nucleo, -2, "NFSMW",
                     "Renderizador nativo (build 185): nucleo preferido del hilo de huellas. -2 = automatico: el 2, o el "
                     "1 si el anillo corre en el 2 (en el nucleo del anillo, ocupado, casi no correria); -1 = el de por "
                     "defecto del proceso; 0-2 = uno concreto. No es exclusivo")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_nativo_texturas_huella_hilo_mb, 16, "NFSMW",
                     "Renderizador nativo (build 185): MB para las copias de la memoria del invitado de las texturas "
                     "nuevas de un envio. Lo que no cabe se prepara en el anillo, como antes")
    .range(4, 64)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Measurement only: new textures whose content repeats a live one. See AnotarContenidoTextura. It changes
 * no decision of the ring: it only counts, and reports every 10 s in the "C3 reutilizar por contenido"
 * line.
 */
REXCVAR_DEFINE_BOOL(airborne_edram_4x_alias, true, "AirborneNX",
                    "Native renderer: draw 4X MSAA surfaces into the 1X surface of twice the pitch "
                    "(EDRAM aliasing) and draw rectangle lists without face culling")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(airborne_salida_7e3, true, "AirborneNX",
                    "Native renderer: apply the 7e3 range in the pixel shader to outputs that go to 7e3 render targets "
                    "(RGBA16F on the host)");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_diag_reutilizar, true, "NFSMW",
                    "Renderizador nativo (26/09, build 186, solo medida): cuenta cuantas texturas nuevas tienen el "
                    "mismo contenido y forma que otra viva de la cache (el juego recarga los packs en otra direccion) "
                    "y cuantas de esas otras llevan mas de 120 fotogramas sin usarse. No cambia nada. false = sin "
                    "contar")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Pipeline prewarming. A pipeline that is not in the Vulkan cache is compiled on the fly on the ring: 68-159 ms
 * each on the console (56 of them in the first race, 5.2 s of stutter). It happens the first time after any
 * change to the shader library, the driver or the key, and on a fresh install. See BuclePrecalentado.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_pipelines_precalentar, true, "NFSMW",
                    "Renderizador nativo (26/09, build 186): al arrancar, un hilo de la prioridad mas baja vuelve a "
                    "crear en la cache de Vulkan los pipelines que el anillo creo en partidas anteriores "
                    "(su lista va en cache/nfsmw_nativo_pipelines.bin), con la misma funcion que el anillo, y los destruye: cuando el "
                    "anillo los pide ya estan compilados (sin los tirones de 70-160 ms por pipeline de la primera "
                    "carrera tras un cambio). No cambia ningun pipeline ni ningun dibujo. false = sin precalentar (la "
                    "lista se sigue guardando)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Testing only. Pretends the GPU runs out of memory on one in N texture allocations, to check that the
// emergency path (stop the GPU, release half the cache and retry) really works. 0 does nothing, which is the
// normal setting.
REXCVAR_DEFINE_INT32(nfsmw_nativo_prueba_sin_memoria_cada, 0, "NFSMW",
                     "Renderizador nativo (solo pruebas): finge que falta memoria en la GPU una de cada N reservas "
                     "de textura, para ejercitar la recuperacion. 0 = apagado")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nfsmw_nativo_diag_mips, false, "NFSMW",
                    "Renderizador nativo (diagnostico): compara el color medio de cada nivel de mip con el de la base en "
                    "las texturas DXT1/3/5 y 8888 y anota las que no se parecen (un nivel leido de otro sitio). No "
                    "cambia la imagen")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nfsmw_nativo_prueba_depth_clamp, false, "NFSMW",
                    "Renderizador nativo (solo pruebas, 17/09 build 149): depthClampEnable en todos los pipelines. Imita "
                    "lo que parece hacer NVK en la consola con lo que queda detras del plano lejano (el sol de las "
                    "consultas de oclusion de la escena)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_nativo_resplandor_suave_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (prueba, build 150): con N > 0 va rotando el resplandor del cielo original, "
                     "natural y suave (nfsmw_resplandor_cielo) cada N segundos y anota cada cambio, para comparar "
                     "capturas del mismo sitio");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_prueba_oclusion_siempre, false, "NFSMW",
                    "Renderizador nativo (solo pruebas, 17/09 build 150): los dibujos sin color de una consulta de "
                    "oclusion en un destino de 640 o mas pasan la prueba de profundidad siempre. Separa un sol que no "
                    "cubre pixeles de uno tapado por la profundidad. Cambia lo que ve el juego")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * This test has been answered: there is nothing to gain.
 *
 * A fit over 17 race intervals gives, for the shadow pass, raw ms = 0.0342 x thousands of triangles + 0.023
 * (r2 = 0.982). The intercept is 0.037 real ms: with both 1600x1600 maps open (5.12 Mtexels, i.e. 20.5 MB of
 * loadOp = LOAD per frame) and zero triangles, the pass costs nothing measurable. So on this GPU the
 * loadOp = LOAD of a depth attachment is not paid for: Maxwell does not load the tile up front like a tiled
 * GPU, it reads on demand. The theory of "130-165 MB per frame moved for nothing" further below does not
 * hold for the shadow map depth.
 *
 * On ZCULL: in NVK (nvk_cmd_draw.c, `use_zcull`) a pass with loadOp = CLEAR enables ZCULL even when the
 * image has no plane (ephemeral, without LOAD/STORE between passes, which a map drawn whole every time does
 * not need), while DONT_CARE, which this cvar uses, does not: the condition is
 * `zcull_plane || loadOp == CLEAR`, and DONT_CARE is neither. CLEAR has the same visual risk as DONT_CARE and
 * leaves the map at 1.0 (far) instead of garbage, so it is the better of the two. It still does not pay off:
 * the ZCULL ceiling here is those 0.037 ms, because the pass is pure geometry. For ZCULL to cover the map,
 * the renderArea has to span the full 1600x1600: the ZCULL region comes from render->area, not from the
 * image size.
 */
/*
 * Enabled by default. It used to be enabled only by a line in a local toml, and the toml overrides the
 * default: shipping without that line would silently lose the improvement. Three cvars were found in
 * that state; one of them was worth 2 ms and had been inactive for six builds.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_pase_sombras_sin_load, true, "NFSMW",
                    "Renderizador nativo (prueba de FPS): abre el pase del mapa de sombras sin cargar su "
                    "contenido previo (loadOp = DONT_CARE). Medido en la 112: no hay nada que ganar, el "
                    "loadOp de la profundidad no se paga en esta GPU (el pase con 0 triangulos cuesta "
                    "0,037 ms)");
REXCVAR_DEFINE_INT32(nfsmw_nativo_omitir_sombras_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (prueba): con N > 0 omite las sombras en los tramos impares "
                     "de N segundos y anota cada cambio, para comparar capturas del mismo sitio");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_diag_vertices_repetidos, false, "NFSMW",
                    "Renderizador nativo (diagnostico): cuenta los bytes de vertices que repiten "
                    "direccion, tamano y contenido en el mismo fotograma o respecto a uno anterior");
// On the console, the vertex copy was ~3 of the ring thread's ~9 us per draw ("subidas" stage), and with the
// game's busy-waits removed there are free cores.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_subidas_hilo, true, "NFSMW",
                    "Renderizador nativo: las copias de vertices al bufer de subida las hace un hilo aparte "
                    "mientras el hilo del anillo sigue grabando; se esperan antes de enviar el trabajo a la "
                    "GPU y antes de devolver el puntero de lectura al juego")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * The ring no longer waits for a copy thread that has no core (see EsperarSubidas).
 *
 * In a race, the ring waited 14.7 ms for the copy thread during an alley stutter (3,965 draws) and 17.9 ms in
 * the next one, and the game spent 21.1 ms without room in the ring. The thread ran at 0x3B, below the two
 * game threads (0x3A), and with the CPU at 278 % out of 300 it sat ready without a core: of those ms, only
 * ~6-7 were copying.
 */
/*
 * Copy thread priority. It used to run at its creation priority (0x3B, below the two game threads at 0x3A)
 * and, with the ring's help path (nfsmw_nativo_subidas_ayuda), the ring always reached the copies first: the
 * thread copied 0 MB and the ring 100 % (1.4-1.8 GB, ~1 s every 10 s: ~3 ms per frame on the thread that is
 * the bottleneck). At 0x2E, like the hash thread, it takes the copies as soon as they are queued, on another
 * core; the ring (0x2D) preempts it if they share a core, and the help path only handles what is left when
 * waiting.
 */
REXCVAR_DEFINE_INT32(nfsmw_nativo_subidas_hilo_prioridad, 0x2E, "NFSMW",
                     "Renderizador nativo (26/09, build 187): prioridad del hilo de copias de vertices. 0x2E: por "
                     "debajo del anillo (0x2D) y por encima del invitado (0x3A-0x3B), para que copie mientras el "
                     "anillo graba. 0x3B = como hasta la 186")
    .range(0x2C, 0x3B)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * The copy thread on a different core from the ring. At the Heritage & Omega exit the thread only did 20 % of
 * the copies: the ring, with higher priority (0x2D) and at 94 %, did not leave it its core, and did them
 * itself while waiting (2.5-5 ms per frame). With its preferred core elsewhere, the thread takes CPU from the
 * game threads (0x3A, lower priority), which spend 97 % of their time waiting for the ring. Only the preferred
 * core is set: the affinity mask is untouched and the kernel can still move it (same as the hash thread).
 */
REXCVAR_DEFINE_INT32(nfsmw_nativo_subidas_hilo_nucleo, -2, "NFSMW",
                     "Renderizador nativo (26/09, build 191): nucleo preferido del hilo de copias de vertices. -2 = uno "
                     "distinto del del anillo; -1 = sin preferencia (como hasta la 190); 0-2 = ese")
    .range(-2, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * 16-bit indices with hand-written NEON (IndicesDe16). At one point the compiler stopped vectorizing that
 * loop (0 vector rev16 in Dibujar, 7 before) and the "indices" substage rose to 2.95 us per draw (the whole
 * stage had been 1.2-1.3 us): ~3 ms of ring time per frame. Same results (the same indices in the same order,
 * the same minimum and maximum). Guard: the first 20,000 draws, then 1 in 4,096, are compared with the plain
 * loop; on a DIFERENCIA the plain loop is kept for the session.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_indices_neon, true, "NFSMW",
                    "Renderizador nativo (26/09, build 187): los indices de 16 bits se copian y se miden (minimo y "
                    "maximo) con NEON, 16 por vuelta. Mismo resultado (se comprueba sola). false = el bucle de siempre")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nfsmw_nativo_subidas_ayuda, true, "NFSMW",
                    "Renderizador nativo (26/09, build 185): cuando el hilo del anillo tiene que esperar las copias "
                    "de vertices, copia el mismo las que el hilo de copias aun no ha empezado y espera la que este en "
                    "curso con herencia de prioridad. Mismos datos (se comprueba sola). false = espera como antes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(nfsmw_nativo_diag_vacias_grises, false, "NFSMW",
                    "Renderizador nativo: las texturas que aun no se soportan (cubo, 3D, "
                    "formatos pendientes) se muestrean grises en vez de a cero (solo pruebas)");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_inv_tamano_tex, true, "NFSMW",
                    "Renderizador nativo (18/09, build 164): los shaders toman 1/tamano de textura de las "
                    "constantes en vez de preguntarselo a la textura en cada muestreo con desplazamiento. Es "
                    "la misma cuenta con el mismo numero: la imagen no cambia. Necesita una biblioteca de "
                    "shaders regenerada con el ayudante nuevo");
/*
 * Enabled by default: a single texel instead of the 3x3, measured and long enabled in a local toml. Three
 * cvars were only enabled through a local toml, and the toml overrides the default: shipping without that
 * line would lose the improvement without anyone noticing.
 */
/*
 * The cheap PCF was briefly disabled by default because, with a single shadow map sample, walls and garage
 * doors showed diagonal bands (shadow acne) that the Xbox 360 does not have; from a distance they looked
 * blurry. The 9-sample PCF fixed nothing: the grid looked the same with 9 samples as with 1, because it was
 * shadow map acne, which the depth slope bias removes (nfsmw_nativo_sombras_sesgo_pendiente). The 9 samples
 * only cost GPU time: together with the blur, race GPU time went from 25.1 to 27.3 real ms. So the cheap
 * PCF is enabled by default again.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_pcf_barato, true, "NFSMW",
                    "Renderizador nativo (20/09): un solo muestreo del mapa de sombras en vez del patron "
                    "3x3 a medio texel. El humo (p_000101) hace once por pixel en rectangulos de pantalla "
                    "completa y es el 21 % de la escena. El borde de la sombra queda algo menos suave "
                    "(25/09, build 183: true por defecto otra vez; las bandas de las paredes eran acne y las "
                    "quita el desplazamiento de profundidad). Necesita la biblioteca de shaders regenerada");

/*
 * Anisotropic filtering, beyond the Xbox 360 (nfsmw_nativo_anisotropico).
 *
 * The game requests trilinear without anisotropy on all its textures ("C4 filtros pedidos": anisotropy 0,
 * bias 0), and the 435 sampling instructions of its 74 pixel shaders do not change that (all of them "use
 * the constant", read from the original binaries). That is why, on the 360 too, surfaces seen at a grazing
 * angle or from a distance look blurry and gain detail as you get closer: the white garage door in the alley,
 * for example. With N > 1, linear samplers with mips get anisotropy N (capped by the device). Single-level
 * ones (resolved targets, shadow map, screen effects) and point samplers stay as they were. It costs GPU time
 * in the scene: measured in "C2: GPU por Swap ... escena". 0 = trilinear, as on the 360.
 */
// 0 by default. At 8x it did not remove the streaks on the alley doors, and the scene went up ~2 ms of GPU
// time together with the 9 shadow samples.
REXCVAR_DEFINE_INT32(nfsmw_nativo_anisotropico, 0, "NFSMW",
                     "Renderizador nativo (25/09, build 179): filtrado anisotropico de las texturas del mundo "
                     "(samplers lineales con mips). 0 = como la Xbox 360 (trilineal, borroso de lado y a "
                     "distancia); 2, 4, 8 o 16 = mas nitido, con coste de GPU")
    .range(0, 16)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

/*
 * Alley door diagnostic (nfsmw_nativo_diag_mip_minimo).
 *
 * The garage door slats and the brickwork showed streaks and ripples that the Xbox 360 does not have, and
 * they did not go away at native 720p nor with 8x anisotropy. With N > 0, textures with mips start N levels
 * lower even up close (the sampler raises its minLod). With the car stopped in front of a door: if with 1, 2
 * or 3 it looks blurry but with its slats in place, its mip levels are fine and the streaks come from
 * elsewhere (the shadow); if streaks, breaks or a pattern that is not the door's show up, that level is
 * uploaded wrong. It can be changed at runtime from the settings menu: it is part of the sampler key and,
 * when it changes, the per-texture sampler caches are invalidated. 0 = normal.
 */
/*
 * Our own depth bias in the shadow map (the acne on the alley doors).
 *
 * Garage doors and walls showed a fine grid of dots and diagonal streaks that the Xbox 360 does not have.
 * Diagnosed with captures taken standing in front of a door: without the shadow map the grid disappears;
 * with 9 samples or with 1 it looks the same; and with the minimum mip at 1, 2 or 3 the texture blurs as
 * expected but the grid stays just as sharp. It is shadow acne: the surface shadows itself. The game sets no
 * depth bias in that pass (0 "desplazamiento de profundidad" lines in every log). Our own is added only to
 * the shadow map draws, and only if the game does not set one. Too high a value detaches the shadows from the
 * objects casting them. Both values can be changed at runtime from the settings menu. 0 and 0 = previous
 * behaviour.
 */
REXCVAR_DEFINE_INT32(nfsmw_nativo_sombras_sesgo_pendiente, 20, "NFSMW",
                     "Renderizador nativo (25/09, build 181): desplazamiento de profundidad por pendiente en el mapa de "
                     "sombras, en decimas (20 = 2,0). Quita el acne (rejilla de puntos en diagonal en paredes y puertas "
                     "de garaje). Demasiado alto despega las sombras de los objetos. 0 = como antes. Se cambia en marcha")
    .range(0, 100);
REXCVAR_DEFINE_INT32(nfsmw_nativo_sombras_sesgo_constante, 0, "NFSMW",
                     "Renderizador nativo (25/09, build 181): desplazamiento de profundidad constante en el mapa de "
                     "sombras, en miles de unidades de 24 bits (1 = 1000). 0 = sin constante. Se cambia en marcha")
    .range(0, 100);

REXCVAR_DEFINE_INT32(nfsmw_nativo_diag_mip_minimo, 0, "NFSMW",
                     "Diagnostico (25/09, build 180): las texturas con mips empiezan N niveles mas abajo, tambien de "
                     "cerca, para ver si sus niveles de mip estan bien. 0 = normal; 1, 2 o 3 para mirar. Se cambia en "
                     "marcha")
    .range(0, 4);
/*
 * Splitting the frame into two submissions.
 *
 * Measured on the console: there is a single vkQueueSubmit per frame, at the end, from Presentar. The GPU
 * runs out of work from the end of one frame until the CPU sends the next, and that is ~4 real ms of idle
 * time per frame ("hueco entre trabajos", constant in a steady race).
 *
 * Submitting as soon as the shadow pass closes lets the GPU start on the shadows while the CPU records the
 * scene. It does not change a single pixel: it is the same work in two pieces.
 */
/*
 * Disabled by default. The idea above is sound but the measurement ruled it out: the gap between
 * submissions went up 3.61 ms when the frame was split in two (two vkQueueSubmit calls = twice the GPU
 * start-up latency, and the second piece cannot start until the CPU finishes recording it). The default
 * follows the measurement: what helps ships enabled and what makes things worse ships disabled. The setting
 * stays so the test can be repeated.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_enviar_tras_sombras, false, "NFSMW",
                    "Renderizador nativo (20/09): enviar el trabajo a la GPU en cuanto se cierra el pase de "
                    "sombras, en vez de todo junto al final del fotograma. La GPU deja de estar parada "
                    "esperando a que la CPU acabe de grabar");
REXCVAR_DEFINE_INT32(nfsmw_nativo_pcf_barato_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (20/09): con N > 0 alterna cada N segundos el muestreo del mapa de "
                     "sombras entre el patron 3x3 y un solo texel, y lo anota. Sirve para medirlo A/B en la "
                     "MISMA carrera: comparar entre carreras distintas no vale porque el tramo cambia");
REXCVAR_DEFINE_INT32(nfsmw_nativo_inv_tamano_tex_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (prueba, build 164): con N > 0 alterna cada N segundos tomar "
                     "1/tamano de las constantes o preguntarselo a la textura");
REXCVAR_DEFINE_INT32(nfsmw_nativo_estadisticas_por_dibujo_s, 0, "NFSMW",
                     "Renderizador nativo (diagnostico, build 159): con N > 0 mide un fotograma de cada N "
                     "segundos con una consulta por dibujo y reparte los fragmentos por pixel shader");
REXCVAR_DEFINE_INT32(nfsmw_nativo_prueba_tijera, 0, "NFSMW",
                     "Renderizador nativo (prueba, build 159): recorta a 1x1 los dibujos de una categoria "
                     "(1 sombras, 2 escena, 3 cubo, 4 posproceso). Se manda todo el estado y todos los "
                     "dibujos pero no se sombrea: la diferencia de tiempo es el suelo por dibujo. ROMPE LA "
                     "IMAGEN mientras esta puesta: solo para medir");
REXCVAR_DEFINE_INT32(nfsmw_nativo_prueba_tijera_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (prueba, build 159): con N > 0 alterna cada N segundos la tijera "
                     "de nfsmw_nativo_prueba_tijera");
/*
 * Separating the texture unit from the ALU (scene pass analysis).
 *
 * The game requests mipmapMode LINEAR on almost every sampler (mag/min/mip filters 1/1/1). On Maxwell a
 * trilinear sample takes the TMU two cycles and a bilinear one only one, so this halves the texture unit's
 * work without removing a single ALU instruction or shader sample. It is the only clean lever to decide
 * which of the two dominates the 18.94 ms of the scene. The jump between mip levels is visible when moving
 * away: for measurement only.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_prueba_mip_puntual, false, "NFSMW",
                    "Renderizador nativo (prueba, 21/09): el filtro ENTRE niveles de mip pasa de lineal a "
                    "puntual (trilineal -> bilineal). La TMU hace la mitad de trabajo y la ALU no cambia: "
                    "si la escena baja manda el muestreo, si no se mueve manda la ALU. Se ve el salto de "
                    "mip al alejarse: solo para medir");
REXCVAR_DEFINE_INT32(nfsmw_nativo_prueba_mip_puntual_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (prueba, 21/09): con N > 0 alterna cada N segundos "
                     "nfsmw_nativo_prueba_mip_puntual (los dos samplers conviven en la cache, no hay "
                     "parones al cambiar)");
/*
 * Do not upload the same vertices twice in the same frame.
 *
 * Each draw copies the [vmin..vmax] range of each binding to the upload buffer, byte-swapped. There was no
 * cache: the same piece of geometry was copied again in full every time it was drawn. Measured on PC with
 * nfsmw_nativo_diag_vertices_repetidos:
 *     5954.9 MB copied; 2584.2 MB repeated within the same frame and 3329.2 MB equal to an earlier
 *     frame  ->  99.0-99.6 % of the bytes are byte-for-byte repeats, across eight reports.
 * On the console that is 8.4 MB of vertices per frame at 2518 MB/s into uncached memory = 3.3 ms of
 * writing alone, split between the ring and the copy thread.
 *
 * This only does the safe half: repeats within the same frame (41-49 % of the bytes). And it is exact, not a
 * gamble: on the Xbox 360 the GPU reads the vertices when it executes the draw, so the game cannot rewrite a
 * range that is already referenced without first synchronizing with the GPU. The only point where it can is
 * a guest wait (WAIT_REG_MEM), and there everything recorded is forgotten (g_sincronizaciones_anillo).
 *
 * If geometry ever looks stuck or stretched, this is the first thing to disable.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_dedupe_vertices, true, "NFSMW",
                    "Renderizador nativo (21/09): si dos dibujos del mismo fotograma piden el mismo rango de "
                    "vertices, se sube una sola vez. El informe C6 anota aciertos y MB ahorrados");

/*
 * No vkCmdBindVertexBuffers in single-binding draws.
 *
 * Each draw copies its vertices to a new spot in the upload buffer, so its binding changed almost every
 * time: in a race, 0.32-0.46 us per call in 66-98 % of the draws (0.21-0.34 us per draw). In NVK that is
 * three wrapper functions (vk_common_CmdBindVertexBuffers -> 2EXT -> 3KHR) and 5 words with an MME macro per
 * binding.
 *
 * With a single binding, the copy is allocated at a multiple of the stride, the upload buffer stays bound
 * at 0 and the draw is shifted with vertexOffset (indexed) or firstVertex (non-indexed). The GPU reads the
 * same bytes: (offset / stride + i - vmin) * stride = offset + (i - vmin) * stride. The NFSMW shaders do not
 * read SV_VertexID (XenosRecomp only declares it with UNLEASHED_RECOMP and the library is generated with
 * NFSMW_RECOMP), so the base index is not visible anywhere. NVK passes vertexOffset/firstVertex as is to
 * the draw macro on every Draw, with or without this change: recording the draw costs the same.
 *
 * These still bind as usual: draws with two or more bindings, those that reuse (dedupe) a copy that does not
 * start at a multiple of their stride, and the deferred sky, which saves and replays its own binding (and
 * when emitted leaves enlaces_grabados_ at 0, so the next draw binds at 0 again).
 *
 * Guard (ComprobarBaseCero): the first 200,000 draws on this path, then 1 in 4,096, redo the computation
 * backwards in 64 bits and check the limits. On a single difference, that draw binds as usual, DIFERENCIA
 * is written to the log and the path switches off for the session. What the guard cannot see is the GPU:
 * if anything looked wrong, nfsmw_nativo_vertices_base_cero = false restores the usual path.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_vertices_base_cero, true, "NFSMW",
                    "Renderizador nativo (25/09, build 179): en los dibujos de un solo enlace de vertices, el bufer de "
                    "subida se queda enlazado en 0 y el dibujo se desplaza con vertexOffset/firstVertex, sin un "
                    "vkCmdBindVertexBuffers por dibujo. La GPU lee los mismos bytes. false = como antes");

REXCVAR_DEFINE_BOOL(nfsmw_nativo_ps_solo_alfa, true, "NFSMW",
                    "Renderizador nativo (18/09, build 158): en las pasadas sin destino de color, compila el "
                    "pixel shader sin las escrituras de color. La imagen no cambia (Vulkan las descarta) y el "
                    "driver borra por muerto lo que solo alimentaba el color: queda la prueba de alfa");
REXCVAR_DEFINE_INT32(nfsmw_nativo_ps_solo_alfa_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (prueba, build 158): con N > 0 alterna cada N segundos compilar el "
                     "pixel shader con y sin las escrituras de color en las pasadas sin color");

/*
 * Depth test before shading, where it can be done without changing the image.
 *
 * The problem, measured. The scene costs 13.86 raw ms = 22.55 real ms (in a race) and it is pure shading:
 * 6.5 M fragments over 0.92 M pixels, so every screen pixel is shaded 7 times. And our own counter says
 * that 28 % of the color draws prevent early rejection: they have an alpha test or a real kill.
 *
 * Why that costs so much. A pixel shader that can discard forces the hardware to shade first and test
 * depth afterwards (late-Z): otherwise a discarded fragment would already have written its Z. So everything
 * with alpha (smoke, particles, glass, decals) is shaded in full even when it is behind a building. In NVK
 * that is literal: nvk_shader.c only sets SET_API_MANDATED_EARLY_Z when the module declares
 * EarlyFragmentTests, and our modules never declare it.
 *
 * The part that can be fixed, and why it is pixel-identical. The reason for late-Z is the Z write, not the
 * test. If the draw writes neither depth nor stencil, moving the test earlier cannot corrupt anything: there
 * is nothing extra to write. The shader still discards the color the same way. So for every draw with the
 * Z test on, Z write off and stencil off, declaring EarlyFragmentTests is free and exact, and the GPU stops
 * shading what is hidden.
 *
 * Deliberately left out: opaque alpha-tested draws (vegetation) write Z, so they are not touched; and
 * neither are draws inside a game occlusion query, because testing earlier would change the samples it
 * counts, and the game reads them.
 *
 * The "C6 Z temprana" report says how many draws are fixed and how many cannot be because they write
 * depth: that second figure is the exact size of what only a depth pre-pass would solve.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_z_temprana, true, "NFSMW",
                    "Renderizador nativo (20/09): en los dibujos que prueban la profundidad pero NO la "
                    "escriben (humo, particulas, cristales, calcomanias), declarar EarlyFragmentTests en el "
                    "pixel shader para que la GPU pruebe la profundidad ANTES de sombrear en vez de despues. "
                    "Sin esto, cualquier shader con prueba de alfa o kill sombrea todos sus fragmentos "
                    "aunque queden tapados. La imagen es identica: no hay escritura de Z que adelantar");
REXCVAR_DEFINE_INT32(nfsmw_nativo_z_temprana_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (prueba, 20/09): con N > 0 alterna cada N segundos la prueba de "
                     "profundidad temprana, para medirla A/B en la MISMA carrera");
/*
 * The sky is drawn first and shades the whole screen for nothing.
 *
 * Measured, not estimated (FRAGMENT_SHADER_INVOCATIONS counter per draw, 20 frames): the sky dome is a
 * single draw per frame, 480 indices = 160 triangles, and yet it invokes 0.83 M fragments: 90 % of the
 * screen. Its pixel shader has 5 texture samples. Cost ~4.8 real ms of the scene's 19.2 ms, 25 %.
 *
 * Why. The game draws it before the world. At that point the Z-buffer is empty, so nothing can discard it:
 * it shades the whole screen and then the world paints over it.
 *
 * Why deferring it does not change a single pixel. The draw is opaque (ONE/ZERO blending: it does not read
 * the target) and already tests depth with LEQUAL without writing it (RB_DEPTHCONTROL 00700732). So without
 * deferral, wherever the world covers it, the sky's result is overwritten; deferred, in the same place, the
 * depth test discards it before shading. The final color is the same in both cases, pixel for pixel, and
 * depth is not touched before or after because the sky does not write it.
 *
 * Where it is emitted. In the same pass, as soon as a draw arrives that it cannot be moved past (one with
 * blending, which does read the background color, or an opaque one that does not write depth, which the
 * sky would cover because the Z-buffer would not have changed), or when the pass closes, whichever comes
 * first. Every copy or resolve of the target goes through TerminarPase (nfsmw_nativo_destinos.cpp), so it
 * is emitted there too before anything reads the color.
 *
 * The only thing that could show, and how to recognize it. If the game drew something at exactly the same
 * depth as the dome (far plane z), the LEQUAL test would let the sky through and cover it, where without
 * deferral it would be the other way round. Nothing should be there (the sky is the farthest thing in the
 * scene), but if the sky is ever seen eating very distant geometry, this is why: disable the cvar.
 */
/*
 * A first version broke the image, and this was why.
 *
 * It produced flickering, badly rendered geometry and odd colors in the menus and on the car. The counter
 * said it unambiguously:
 *
 *     "C6 cielo aplazado: 7,00 detectados por fotograma, 1,00 aplazados de verdad"
 *
 * The analysis expected 0.9 draws per frame for that pixel shader. In reality there are seven: the sky dome
 * is not the only user of that shader. Deferring one of the seven breaks the order of the other six, and
 * that is where the artifacts came from.
 *
 * What was missing: the fingerprint identifies the shader, not the draw. Two draws with the same shader and
 * similar state are indistinguishable by this criterion. And the counter that would have exposed it
 * (detected per frame) shipped in the same build as the change instead of before it. The dome draw has to
 * be identified as such (by index count, 480 = 160 triangles, by being the first of the pass, and by
 * checking that it really covers the screen), and verified with the counter before anything is deferred.
 */
/*
 * Enabled again, but it no longer decides alone.
 *
 * With this cvar on, earlier versions broke the image because seven draws share the shader's fingerprint.
 * Now the self-checking guard sits above it: even with the cvar on, nothing is deferred until 90
 * consecutive frames have been seen with a single dome, and as soon as two show up it switches off for the
 * rest of the session. See kCieloFotogramasPrueba and CerrarFotogramaDeLaGuardiaDelCielo. Enabling it
 * cannot break the image; at worst it does nothing.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_cielo_aplazado, true, "NFSMW",
                    "Renderizador nativo (22/09): aplazar el dibujo del cielo hasta despues de los opacos "
                    "del mismo pase, en vez de dibujarlo el primero sobre un Z-buffer vacio. Es un solo "
                    "dibujo opaco que prueba la profundidad y no la escribe, asi que la imagen es identica: "
                    "lo que hoy se sobreescribe, manana lo descarta la prueba de profundidad");
/*
 * Draws that paint nothing and are still shaded.
 *
 * Two exact cases, both read from the draw's own registers:
 *
 *  1. Blending is "0 x source + 1 x destination" (ADD) on every written channel: the result is the
 *     destination as is. The draw cannot change a single color pixel.
 *  2. The alpha test function is 0 (NEVER): the recompiled shader always calls clip() (alphaTestValue
 *     returns -1 for case 0), so every fragment dies, and dies before writing depth.
 *
 * In both cases, if the draw also writes neither depth nor stencil, it leaves no trace of any kind and can
 * be skipped entirely. If it writes depth, only its color write is removed (mask 0), which already saves
 * the blending and the write.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_saltar_invisibles, true, "NFSMW",
                    "Renderizador nativo (20/09): saltarse los dibujos que no pueden cambiar ni un pixel "
                    "(mezcla que copia el destino, o prueba de alfa con la funcion NUNCA). La imagen es "
                    "identica por definicion");
/*
 * Small per-draw savings on the ring thread. Each has its own cvar; the first three have a self-checking
 * guard (the first 200,000 cases, then 1 in 4,096, also go through the usual path and are compared; on a
 * difference, DIFERENCIA in the log and the saving is switched off).
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_encuadre_cache, true, "NFSMW",
                    "Renderizador nativo (25/09, build 179): el viewport, el ndc y la tijera de un dibujo se reutilizan "
                    "mientras no cambien los registros del encuadre (su generacion) ni el pase. Se comprueba sola. "
                    "false = se calculan en cada dibujo, como antes");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_alto_util_memo, true, "NFSMW",
                    "Renderizador nativo (25/09, build 179): el alto util del destino se apunta en el elemento del mapa "
                    "del ultimo pitch, sin buscarlo en cada dibujo. Se comprueba sola. false = como antes");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_clave_pase_rapida, true, "NFSMW",
                    "Renderizador nativo (25/09, build 179): si los destinos de un dibujo son los mismos bytes que los "
                    "del pase abierto no se recalcula su XXH3. Se comprueba sola. false = XXH3 en cada dibujo");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_cvars_por_fotograma, true, "NFSMW",
                    "Renderizador nativo (25/09, build 179): nfsmw_nativo_ps_solo_alfa y nfsmw_nativo_sin_ps_sin_color "
                    "(y sus alternancias) se leen una vez por fotograma y no en cada dibujo sin color. false = como "
                    "antes");
REXCVAR_DEFINE_INT32(nfsmw_nativo_sin_ps_sin_color_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (prueba, build 157): con N > 0 alterna cada N segundos montar el "
                     "pipeline con y sin etapa de fragmentos en los dibujos sin color, para comparar capturas "
                     "del mismo sitio con el juego en pausa");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_sin_ps_sin_color, true, "NFSMW",
                    "Renderizador nativo (17/09, build 157): en los dibujos que no escriben ningun color, "
                    "monta el pipeline sin etapa de fragmentos si el pixel shader no puede descartar pixeles "
                    "ni escribe profundidad. La imagen no cambia y la GPU no sombrea (mapa de sombras)");
REXCVAR_DEFINE_STRING(nfsmw_nativo_diag_omitir_ps, "", "NFSMW",
                      "Renderizador nativo: numeros de PS separados por comas cuyos dibujos no "
                      "se graban (solo pruebas, para localizar un dibujo)");
// A review of other Switch projects found that in NVK for Tegra the memory type the SDK picks for uploads
// (without HOST_CACHED) is an NvMap without CPU caching, and wine-nx measured on the console that writing
// there is slow. All vertex copies go to this buffer, so it can be chosen for an A/B test.
REXCVAR_DEFINE_INT32(nfsmw_nativo_subida_memoria, 0, "NFSMW",
                     "Renderizador nativo: memoria del bufer de subida. 0 = la que elige el SDK (en la Switch, sin "
                     "cache de CPU), 1 = con cache de CPU (se publica con vkFlushMappedMemoryRanges antes de "
                     "enviar), 2 = sin cache de CPU")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nfsmw_nativo_compartidas_cache, true, "NFSMW",
                    "Renderizador nativo (23/09, build 156): las constantes compartidas de cada dibujo (488 bytes) "
                    "en un bufer pequeno CON cache de CPU, aparte del de subida. Medido en la 155: escribirlas sin "
                    "cache costaba 2,0 us por dibujo y con cache 0,4. Solo con las constantes por UBO")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Direct-mapped pipeline cache.
 *
 * PipelineDe costs 0.52-0.76 us per draw in a race ("C6 subetapas"), more than BindVertexBuffers. The
 * one-entry shortcut misses on ~1 in 3 draws (the same ones that then call BindPipeline), and each miss is
 * an XXH3 of 80 bytes, a division by libstdc++'s prime bucket count and 2-3 jumps to nodes scattered across
 * the heap, which with the ring streaming megabytes through the cache almost always miss all the way to
 * memory: ~1.3 us per miss.
 *
 * In front of the map sits a table of 256 contiguous slots (22 KB) indexed by the low bits of the same
 * XXH3, holding the full key: a hit is one memcmp on a single slot. It only stores key -> VkPipeline pairs
 * already in the map, and the map never erases, so it cannot return anything other than what the map
 * would. Guard: the first 200,000 hits, then 1 in 4,096, also look up the map and must get the same
 * VkPipeline; on a difference, DIFERENCIA in the log and it switches off for the session. false = map only,
 * as before.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_pipelines_directa, true, "NFSMW",
                    "Renderizador nativo (25/09, build 179): cache directa de 256 pipelines delante del mapa de "
                    "PipelineDe. Mismo resultado que el mapa (se comprueba sola). false = solo el mapa, como antes");
/*
 * Measurement only, it changes no draw. What changes at each vkCmdBindPipeline of the ring: shaders, vertex
 * input, formats, specialization, or only fixed pipeline state (blending, masks, Z, stencil, cull face,
 * topology, bias, primitive restart). The state-only ones are those Vulkan dynamic state would avoid. Two
 * "C6 cambios de pipeline" lines every 20 s. false = nothing is counted.
 */
// Disabled by default: it was only a measurement and it has already produced its data.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_contar_cambios_pipeline, false, "NFSMW",
                    "Renderizador nativo (25/09, build 184): cuenta que cambia en cada vkCmdBindPipeline (solo "
                    "medida, no cambia ningun dibujo). Lineas C6 cambios de pipeline cada 20 s. false = no se cuenta");
/*
 * Dynamic state, phase 0a. A draw's pipeline is looked up with its key in canonical form (Canonizar):
 * whatever PipelineDe does not read, or reads but Vulkan ignores (the blend equation without blendEnable,
 * the half of the equation whose channels the mask does not write, the Z function without a Z test, stencil
 * operations without stencil, targets not in the pass), is set to a fixed value. The pipeline is the same in
 * everything Vulkan looks at: fewer pipelines and fewer vkCmdBindPipeline calls (the counter's "no effect"
 * category). Guard: the first 200,000 key changes, then 1 in 4,096, compare the fixed state of both keys
 * field by field (RellenarEstadoFijo, the relevant part of PipelineDe); on a difference, DIFERENCIA in the
 * log and it switches off for the session. false = the usual key.
 */
// Disabled by default. Measured: PipelineDe got more expensive and no avoidable bind was measured.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_clave_canonica, false, "NFSMW",
                    "Renderizador nativo (25/09, build 184): el pipeline se busca con la clave en forma canonica (solo "
                    "lo que Vulkan mira): menos pipelines y menos vkCmdBindPipeline. Se comprueba sola. false = la "
                    "clave de siempre");
/*
 * Dynamic state, phase 0b. EmpezarPase no longer forgets the bound pipeline. Vulkan keeps the binding and
 * the dynamic state across passes of the same command buffer, and NVK only marks state as dirty when a pass
 * begins (nvk_cmd_buffer_dirty_render_pass). The same key carries the same formats and therefore a
 * compatible render pass (compatibility ignores loadOp and storeOp). A new command buffer still starts with
 * nothing bound. It removes the repeated vkCmdBindPipeline of a pass's first draw (the counter's "same key
 * after starting a pass"). false = forgotten at every pass, as before.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_pipeline_entre_pases, true, "NFSMW",
                    "Renderizador nativo (25/09, build 184): el pipeline enlazado se conserva al empezar un pase del "
                    "mismo bufer de comandos (Vulkan lo conserva). false = se vuelve a enlazar en cada pase, como "
                    "antes");
/*
 * Dynamic state, phase 1 (EDS1/EDS2, core in Vulkan 1.3; the console reports API 1.3.354). Cull mode, front
 * face, topology (within its class), Z test, write and function, stencil with its operations, depth bias
 * and primitive restart leave the pipeline: they are set with vkCmdSet* and only when they change. Two draws
 * that only differ in those share a pipeline and need no new vkCmdBindPipeline (3-4.5 us each on the
 * console). Pipelines in this mode carry a flag in the key (relleno2) and do not mix with the usual ones;
 * the mode is decided once per command buffer. Guard: the first 200,000 key changes, then 1 in 4,096,
 * compare what was set with what the usual pipeline would carry (RellenarEstadoFijo); on a difference,
 * DIFERENCIA in the log and it switches off for the session (pipelines with all state baked in come back,
 * and the draw with the difference already goes out with its own). false = everything in the pipeline, as
 * before.
 */
// Disabled by default. Measured: each BindPipeline went from 2.95 to 4.36 us and PipelineDe from 0.21 to
// 0.47 us per draw, for only 13 % fewer binds: a net loss of ~1 ms of ring time per frame.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_estado_dinamico, false, "NFSMW",
                    "Renderizador nativo (25/09, build 184): cara, frente, topologia, Z, estencil, sesgo y reinicio "
                    "con vkCmdSet* (EDS1/EDS2 de Vulkan 1.3) en vez de en el pipeline: menos vkCmdBindPipeline. Se "
                    "comprueba sola. false = todo en el pipeline, como antes");
/*
 * Dynamic state, phase 2 (VK_EXT_extended_dynamic_state3; NVK exposes it on Maxwell and the SDK enables it
 * with ui_vulkan_estado_dinamico3.patch). Blending (blendEnable), its equation and each target's color mask
 * are set with vkCmdSet* and stop splitting pipelines: among the first 64 pipelines created, the 12
 * state-only variants differed only in blending. If the SDK or the device does not provide it, it is not
 * used, and one log line says so. Same guard as phase 1. false = blending in the pipeline, as before.
 */
// Disabled by default, for the same measured reason as nfsmw_nativo_estado_dinamico (it goes with it).
REXCVAR_DEFINE_BOOL(nfsmw_nativo_estado_dinamico3, false, "NFSMW",
                    "Renderizador nativo (25/09, build 184): mezcla, ecuacion y mascara de color con vkCmdSet* "
                    "(VK_EXT_extended_dynamic_state3) en vez de en el pipeline. Se comprueba sola. false = en el "
                    "pipeline, como antes");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_diag_memoria_subida, true, "NFSMW",
                    "Renderizador nativo: al crear el bufer de subida mide una vez cuantos MB/s se escriben desde la "
                    "CPU en cada tipo de memoria visible (8 MB) y cuanto cuesta publicarlos")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The 30 FPS guard lives in nfsmw_recorte_sombras.cpp, which owns the lever.
namespace nfsmw::guard30 {
bool WithoutShadows(bool user);
}  // namespace nfsmw::guardia30

namespace nfsmw::native_renderer {

// Incremented by the ring thread when it handles a WAIT_REG_MEM (nfsmw_nativo_sistema.cpp). That is the
// point where vertex deduplication must forget what it recorded.
std::atomic<uint32_t> g_ring_syncs{0};
namespace {

/*
 * A pass's category from its render target, in one place.
 *
 * It used to be decided by width: "1600 or more" meant shadows. With the scene at 1920
 * (nfsmw_1080p_prueba) that no longer tells them apart and the scene was counted as shadows, so its
 * category vanished from the report. The shadow map is recognized for what it is: a depth-only target, with
 * no color at all.
 */
inline uint32_t TargetCategory(uint32_t pitch, const uint64_t* keys) {
  const bool color = keys[0] || keys[1] || keys[2] || keys[3];
  if (!color && keys[4] && pitch >= 1600) {
    return kGpuShadows;
  }
  if (pitch >= 1280) {
    return keys[4] ? kGpuScene : kGpuSceneWithoutDepth;
  }
  if (pitch >= 640) {
    return kGpuReflection;
  }
  if (pitch >= 320) {
    return kGpu320;
  }
  return kGpuLesser;
}

namespace gr = rex::graphics;
namespace xenos = rex::graphics::xenos;
using rex::ui::vulkan::VulkanDevice;

constexpr uint32_t kRegConstantesVs = 0x4000;
constexpr uint32_t kRegConstantesPs = 0x4400;
constexpr uint32_t kRegFetch = 0x4800;
constexpr uint32_t kRegBools = 0x4900;
constexpr uint32_t kConstantsRegs = 0x400;  // 256 constantes x 4

constexpr uint32_t kHeapCapacity[4] = {4096, 16, 64, 512};  // 2D, 3D, cubo, samplers
// Work slots. The ones actually used are chosen by nfsmw_nativo_ranuras_trabajo; this is the room reserved
// for them, and it has to match the array in nfsmw_nativo_destinos.cpp.
constexpr size_t kJobSlots = 3;
constexpr VkDeviceSize kUploadSize = VkDeviceSize(64) << 20;
// Separate buffer for the shared constants, per slot. ~1,400 draws x 512 bytes = 0.7 MB per frame; if it
// fills up, the work is submitted just as with the upload buffer.
constexpr VkDeviceSize kSharedSize = VkDeviceSize(4) << 20;  // per work slot (there are three)
// The pipeline cache and the prewarm list go in a single file in <NRO folder>/cache/ (nothing loose next to
// the NRO, and a single pipelines .bin). Header "NFPC", version, list bytes and cache bytes (two uint32
// and two uint64), then both parts: the list as is (its "NFPL" header and the records) and the
// vkGetPipelineCacheData data.
constexpr const char* kCacheFolder = "cache";
constexpr const char* kPipelinesFile = "nfsmw_nativo_pipelines.bin";
constexpr uint32_t kPipelinesFileMagic = 0x4350464Eu;  // "NFPC" en little-endian
constexpr uint32_t kPipelinesFileVersion = 1;
constexpr size_t kPipelinesFileHeader = 2 * sizeof(uint32_t) + 2 * sizeof(uint64_t);
// The two files used by earlier versions, next to the NRO: if the new one does not exist yet they are read
// once (so the existing cache is not lost) and deleted when the new one is written.
constexpr const char* kOldCacheFile = "nfsmw_nativo_pipelines.bin";
constexpr const char* kOldListFile = "nfsmw_nativo_pipelines_lista.bin";
// The pipeline prewarm list. It holds no game data: state keys, formats and fingerprints. A header of four
// uint32 ("NFPL", version, record size and record count) followed by the records (RegistroPipeline).
constexpr uint32_t kPipelinesListMagic = 0x4C50464Eu;  // "NFPL" en little-endian
constexpr uint32_t kPipelinesListVersion = 1;
constexpr size_t kListHeader = 4 * sizeof(uint32_t);
constexpr size_t kMaxListRegs = 4096;
// 296 bytes: NFSMW's shader_common.h (g_NdcScale at +280 and g_NdcOffset at +288).
// Shared constants: texture and sampler indices (0-63), booleans, texcoords, half pixel, alpha threshold
// (68) and function (69), NDC (64-73) and g_InputRemap for the 16 locations (74-89).
// 90 words up to g_InputRemap (bytes 296..359) and 32 more for 1/size of the 16 texture slots (bytes
// 360..487), which avoid querying the texture size on every sample.
// AirborneNX: 16 more words (bytes 488..551): the value scale of each texture slot (2^exp_adjust of the
// fetch constant times the resolve bias of the texture), which the shaders multiply every sample by
// (<sampler>_Escala, tools/shaders_native). They default to 1.
constexpr uint32_t kSharedWords = 138;
constexpr uint32_t kSizeInvWord = 90;
constexpr uint32_t kScaleWord = 122;
// Constants through a dynamic UBO (nfsmw_nativo_constantes_ubo). The bit is SPEC_CONSTANT_CONSTANTES_UBO
// from shader_common.h, and the sizes are the blocks the shaders declare: 256 and 224 float4, and 23 shared
// float4.
constexpr uint32_t kSpecConstantesUbo = uint32_t(1) << 8;
// Internal bit of the pipeline key that no shader reads: the bright pass (PS n137, container
// kHuellaBrightPass = p_000094) uses the nfsmw_resplandor_cielo variant: natural
// (p_000094_resplandor_energia.hlsl) or soft (p_000094_resplandor_suave.hlsl).
// The shaders take 1/texture size from the shared constants (SPEC_CONSTANT_INV_TAMANO_TEX in XenosRecomp)
// and do not query the texture size on every sample.
constexpr uint32_t kSpecTexSizeInv = uint32_t(1) << 9;
constexpr uint32_t kSpecGlowNatural = uint32_t(1) << 12;
constexpr uint32_t kSpecGlowSmooth = uint32_t(1) << 13;
// The pixel shader is compiled without its color writes (pass without a color target).
constexpr uint32_t kSpecOnlyAlpha = uint32_t(1) << 14;
// Cheap PCF. The shaders that sample the shadow map use a 3x3 pattern at half-texel offsets (nine samples
// per pixel; eleven in p_000101, the smoke, which is full-screen quads and 21 % of the scene). With this bit
// the eight outer offsets are set to zero, the nine samples become identical and the compiler merges them
// into one. Shadow edges lose some smoothness. Needs the library regenerated with
// shaders/nfsmw_regenerar_biblioteca_pcf.sh.
constexpr uint32_t kSpecCheapPcf = uint32_t(1) << 15;
// Bits 16-18 = the alpha test function (0-6). See SPEC_CONSTANT_ALPHA_FUNC_SHIFT in shader_common.h: it
// removes the 7-case switch from every pixel shader.
constexpr uint32_t kSpecOffsetAlphaFunction = 16;
// The radial blur of the final composite (see SPEC_CONSTANT_SIN_DESENFOQUE).
constexpr uint32_t kSpecWithoutBlur = uint32_t(1) << 19;
// Internal bit of the pipeline key that no shader reads (shader_common.h goes up to bit 19). It marks that
// the pixel shader module is the copy with OpExecutionMode EarlyFragmentTests.
constexpr uint32_t kSpecZEarly = uint32_t(1) << 20;
// AirborneNX: internal bit of the pipeline key no shader reads: the draw is a rectangle list, drawn as a triangle
// list with the rect_list_gs geometry shader that adds each rectangle's fourth corner.
constexpr uint32_t kSpecRectList = uint32_t(1) << 21;
// nfsmw_nativo_sombra_minimo. SPEC_CONSTANT_SOMBRA_MINIMO from shader_common.h: tfetch2DSombraMin takes the
// minimum of the shadow map and its pair, which goes in the 3D index word of that register.
constexpr uint32_t kSpecShadowMin = uint32_t(1) << 23;
constexpr uint64_t kCompositionFingerprint = 0x19C0C358044A29BFull;  // p_000139, the race VisualTreatment
// nfsmw_tratamiento_visual. The final composite (kHuellaComposicion, p_000139) tints each channel with a polynomial
// curve (Coeffs0..3 = c6..c9, evaluated at MISCMAP1.w), mixes in a desaturated part with its x component and adds a
// vignette (VisualEffectVignette.x = c1). With a neutral curve (Coeffs0 = (0, 1, 1, 1) and Coeffs1..3 = 0) and the
// vignette at 0 the picture has no filter; the glow (g_fBloomScale, c4) and the brightness of the fades
// (CombinedBrightness, c10) stay the same. suave: halfway between the game's values and the neutral ones (the curve is
// linear in its coefficients). The registers are those of the microcode, the same in every edition.
constexpr uint32_t kHandlingBytes = 11 * 16;  // c0 to c10
void ApplyVisualTreatment(float* c, int mode) {
  static constexpr float kNeutral[4][4] = {{0, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
  const float f = mode == 2 ? 1.0f : 0.5f;
  for (int k = 0; k < 4; ++k) {
    for (int i = 0; i < 4; ++i) {
      float& v = c[(6 + k) * 4 + i];
      v += (kNeutral[k][i] - v) * f;
    }
  }
  c[1 * 4 + 0] *= 1.0f - f;
}

/*
 * The render target height comes from the pitch, not from what the game uses.
 *
 * nfsmw_nativo_destinos.cpp creates every render target with `alto = max(720, pitch)`, because the real
 * height is not known when it is created. Measured result:
 *
 *   scene    pitch 1280 -> image 1280x1280, the game uses 1280x720
 *   cubemap  pitch  320 -> image  320x720,  the game uses  320x256
 *   bloom    pitch  320 -> image  320x720,  the game uses  320x180
 *   bloom    pitch  160 -> image  160x720,  the game uses  160x90
 *
 * And since the passes are opened with loadOp = LOAD and closed with storeOp = STORE, that whole area is
 * read and written in each of the ~27 pass openings per frame: 18.81 Mtexels opened when the game uses
 * ~8.5. That is ~130-165 MB per frame moved for nothing, over a 21.3 GB/s bus that the three cores also
 * share.
 *
 * The Xbox 360 paid none of this: its framebuffer lived in 10 MB of on-chip EDRAM at 256 GB/s, and the
 * resolve was a hardware operation. We move it back and forth.
 *
 * It is fixed through the renderArea, not the image size. Vulkan only loads and stores the renderArea;
 * everything outside is preserved. So the height does not have to be guessed when the image is created:
 * opening the pass over the rectangle the game really uses is enough.
 *
 * How that height is known without guessing: from the draws' scissor. The maximum seen per pitch is kept,
 * only grows, and is rounded up to a multiple of 64. Until there is data the whole pass is opened, so the
 * first frame never clips too much.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_pase_area_util, true, "NFSMW",
                    "Renderizador nativo (20/09): abrir cada pase sobre el rectangulo que el juego usa de "
                    "verdad en vez de sobre la imagen entera. El alto de los destinos se deduce del pitch, "
                    "asi que la escena abre 1280x1280 para dibujar 1280x720 y el cubo 320x720 para dibujar "
                    "320x256. La imagen es identica: Vulkan solo carga y guarda el renderArea");
constexpr uint64_t kBrightPassFingerprint = 0xE849A9F6D3323B87ull;
/*
 * The pixel shader of the sky dome (nfsmw_nativo_cielo_aplazado).
 *
 * The fingerprint is the XXH3 of the shader's original container, not of the translated SPIR-V: it
 * identifies the game's shader and does not change when the library is regenerated (checked: the same
 * 28AA3CDAC6C19705 in nfsmw_validado_predicados and in nfsmw_fusion3, with SPIR-V of 1,859 and 3,306
 * words). It is the same mechanism as kHuellaBrightPass and kHuellaComposicion.
 *
 * What it is, beyond doubt: its constant table declares CloudIntensity, SkyAlphaTag and Brightness, and
 * four samplers (DIFFUSEMAP, MISCMAP1, MISCMAP2, MISCMAP3) with 5 samples. It is the only shader in the
 * library (152 containers) that names the sky: it cannot be confused with any other. In the log it shows
 * up as PS n29.
 */
constexpr uint64_t kSkyFingerprint = 0x28AA3CDAC6C19705ull;
// The sky dome is 480 indices = 160 triangles, measured in the log (the C6 diag line of the draw with
// PS n29). That is the mark that tells the dome draw apart from the other six that share its pixel
// shader, which were the ones that broke the image in an earlier version.
constexpr uint32_t kSkyDomeIndices = 480;
/*
 * How long the sky guard's test lasts, counting only the frames in which the dome appears (menus and
 * loading screens do not count, see CerrarFotogramaDeLaGuardiaDelCielo). 90 frames are about 3 seconds
 * of racing: enough not to decide on four samples, and short enough for the saving to start almost as
 * the race begins. All 90 must have exactly one dome: as soon as two appear in the same frame (the
 * earlier failure) it switches off for good.
 */
constexpr uint32_t kSkyTestFrames = 90;
constexpr uint32_t kSkyFramesWithOne = 90;
constexpr VkDeviceSize kUboBytesVs = 256 * 16;
constexpr VkDeviceSize kUboBytesPs = 224 * 16;
constexpr VkDeviceSize kSharedUboBytes = 35 * 16;  // AirborneNX: was 31 (+ the 16 slot scales)
constexpr uint32_t kIdentityRemap = 0xFFF;
constexpr uint32_t kMaxVerticesPerDraw = uint32_t(1) << 20;
constexpr uint32_t kPhysicalMemory = 0x20000000;
// Times a statement in the timed draws (C6 subetapas).
#define NFSMW_SUB(k, ...) \
  do { \
    if (time_) { \
      const auto t0_sub_ = std::chrono::steady_clock::now(); \
      __VA_ARGS__; \
      sub_ns_[k] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>( \
                                 std::chrono::steady_clock::now() - t0_sub_) \
                                 .count()); \
      ++sub_n_[k]; \
    } else { \
      __VA_ARGS__; \
    } \
  } while (0)

/*
 * From 8 to 64. Each timed draw reads the clock about 20 times (Etapa, NFSMW_SUB, samplers and
 * vertices). At 1 in 8 that averaged 0.15-0.4 us on every draw of the ring, and in the alley there are
 * 2,500-4,000 per frame. "C6 etapas", "C6 subetapas" and "ms copiando vertices" divide (or rescale) by
 * the draws that actually carried a stopwatch, so their scale does not change: they just have 8 times
 * fewer samples (~600-1,400 per second in a race).
 */
constexpr uint32_t kStopwatchEvery = 64;  // draws per stage-timed draw (power of 2)
constexpr size_t kCopiesPerWarning = 64;   // copies queued between wake-ups of the copy thread (power of 2)
// Copies taken in one batch (nfsmw_nativo_subidas_ayuda). In a race they are ~2 KB each: a batch is
// ~30 KB, about 20 us. That is how long the ring may have to wait for the thread, already with its
// priority lent.
constexpr size_t kCopiesPerChunk = 16;
// Waits with pending copies in the observing phase (the ring waits as before and checks the marks)
// before it starts helping. In the menus that is a few seconds.
constexpr uint64_t kPeekingCopiesWaits = 512;

// One guest vertex binding into the upload buffer, as host words with the fetch constant's byte order.
// Done by the ring thread or by the copy thread (nfsmw_nativo_subidas_hilo).
struct CopyJob {
  const uint8_t* src = nullptr;
  uint8_t* target = nullptr;
  uint32_t word_data = 0;
  xenos::Endian orden = xenos::Endian::kNone;
};

void CopyVertices(const CopyJob& t) {
  // The fields go into local variables: the destination is a byte pointer and, as far as the compiler
  // knows, writing through it could change the structure itself. With t.* inside the loop it was not
  // vectorized (935 ms per 5.3 GB of vertices in a race, against 490 ms with the loop inside Dibujar).
  const uint8_t* const src = t.src;
  uint8_t* const target = t.target;
  const uint32_t word_data = t.word_data;
  const xenos::Endian orden = t.orden;
  if (orden == xenos::Endian::k8in32) {
    for (uint32_t i = 0; i < word_data; ++i) {
      uint32_t v;
      std::memcpy(&v, src + size_t(i) * 4, 4);
      v = std::byteswap(v);
      std::memcpy(target + size_t(i) * 4, &v, 4);
    }
  } else {
    for (uint32_t i = 0; i < word_data; ++i) {
      uint32_t v;
      std::memcpy(&v, src + size_t(i) * 4, 4);
      v = xenos::GpuSwap(v, orden);
      std::memcpy(target + size_t(i) * 4, &v, 4);
    }
  }
}

// Allocator that leaves whatever resize adds uninitialized: indices_, convertidos_ and indices16_ are
// fully rewritten right after, and std::vector's zero fill was 1.4 % of the ring thread on PC.
template <class T>
struct NoInitAllocator : std::allocator<T> {
  using value_type = T;
  NoInitAllocator() = default;
  template <class U>
  NoInitAllocator(const NoInitAllocator<U>&) noexcept {}
  template <class U>
  struct rebind {
    using other = NoInitAllocator<U>;
  };
  template <class U>
  void construct(U* p) noexcept(std::is_nothrow_default_constructible_v<U>) {
    ::new (static_cast<void*>(p)) U;
  }
  template <class U, class... Args>
  void construct(U* p, Args&&... args) {
    ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
  }
};

using BufferAddressFn = VkDeviceAddress(VKAPI_PTR*)(VkDevice, const VkBufferDeviceAddressInfo*);
using CopyImageFn = void(VKAPI_PTR*)(VkCommandBuffer, VkImage, VkImageLayout, VkImage,
                                        VkImageLayout, uint32_t, const VkImageCopy*);

float Float(uint32_t value) {
  return std::bit_cast<float>(value);
}

// Arithmetic of the Xenos mip level layout (pipeline/texture/util.cpp: GetPackedMipLevel,
// GetPackedMipOffset and GetGuestTextureLayout).
uint32_t CeilingLog2(uint32_t v) {
  return v <= 1 ? 0 : 32 - uint32_t(std::countl_zero(v - 1));
}

uint32_t Log2Floor(uint32_t v) {
  return v ? 31 - uint32_t(std::countl_zero(v)) : 0;
}

/*
 * The sample used to recheck a stable texture (nfsmw_nativo_huellas_muestreo). From a region of guest
 * memory, its first 4 KB block, its last one and one in every kMuestraCada are read, counted from the
 * start of the region. The base and the mips of a texture start at 4 KB-aligned addresses, so each block
 * is a whole page. That reads ~13-19 % of the bytes of textures of 64 KB or more; for textures of few
 * blocks the sample is almost everything, and PrepararTextura does not use it if it exceeds half. Tested
 * on PC: BytesMuestra matches what is read for every size from 1 byte to 300 KB, and a one-byte change is
 * seen if and only if it falls in a sampled block.
 */
constexpr uint64_t kSampleBlock = 4096;
constexpr uint64_t kSampleEvery = 8;

// Bytes HuellaMuestra reads in a region of that size.
inline uint64_t SampleBytes(uint64_t bytes) {
  if (!bytes) {
    return 0;
  }
  const uint64_t is_last = (bytes - 1) / kSampleBlock;
  return (is_last + kSampleEvery - 1) / kSampleEvery * kSampleBlock + (bytes - is_last * kSampleBlock);
}

// Chained XXH3 of blocks 0, 8, 16... below the last one, and of the last one (which may be partial).
inline uint64_t SampleFingerprint(const uint8_t* payload, uint64_t bytes, uint64_t seed) {
  if (!bytes) {
    return seed;
  }
  const uint64_t is_last = (bytes - 1) / kSampleBlock;
  uint64_t fingerprint = seed;
  for (uint64_t b = 0; b < is_last; b += kSampleEvery) {
    fingerprint = XXH3_64bits_withSeed(payload + b * kSampleBlock, size_t(kSampleBlock), fingerprint);
  }
  return XXH3_64bits_withSeed(payload + is_last * kSampleBlock, size_t(bytes - is_last * kSampleBlock), fingerprint);
}

// First level of the packed tail: once the short side is 16 texels or less.
uint32_t PackedLevel(uint32_t width_px, uint32_t height_px) {
  const uint32_t l = CeilingLog2(std::min(width_px, height_px));
  return l > 4 ? l - 4 : 0;
}

// Blocks from the start of the packed tail to a level of a 2D texture; 0 if the level is not packed.
void PackedOffset(uint32_t width_px, uint32_t height_px, uint32_t block, uint32_t mip_level, uint32_t& x,
                               uint32_t& y) {
  const uint32_t l2_width = CeilingLog2(width_px);
  const uint32_t l2_height = CeilingLog2(height_px);
  const uint32_t l2 = std::min(l2_width, l2_height);
  x = 0;
  y = 0;
  if (l2 > 4 + mip_level) {
    return;
  }
  const uint32_t base = l2 > 4 ? l2 - 4 : 0;
  const uint32_t m = mip_level - base;
  if (m < 3) {
    if (l2_width > l2_height) {
      y = 16u >> m;  // wider than tall: levels are stacked vertically
    } else {
      x = 16u >> m;
    }
  } else if (l2_width > l2_height) {
    x = (1u << (l2_width - base)) >> (m - 2);
  } else {
    y = (1u << (l2_height - base)) >> (m - 2);
  }
  x /= block;
  y /= block;
}

// Address of a block in a texture tiled in 32x32 blocks, copied from GetTiledOffset2D
// (graphics/pipeline/texture/util.cpp). pitch in blocks.
int32_t Tile2dOffset(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bytes + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bytes;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// Fast untiling. During race stutters the ring spent ~30 ms preparing 5 MB of new textures (about 6 ms
// per MB): LeerNivel called DesplazamientoMosaico2D and a variable-size memcpy per block. Here, with the
// block size fixed at compile time:
//   - what depends on the row (y) is computed once per row;
//   - within a 16-byte group of the tiling the blocks are contiguous in the source (only the 4 low bits
//     of micro change), so 16 bytes are copied at once (8 for textures with 1 byte per block).
// It gives exactly the same addresses as DesplazamientoMosaico2D (checked block by block on PC).
template <uint32_t kLog2>
void UntileLevel(const uint8_t* src, uint32_t pitch, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by,
                        uint32_t bx_host, uint8_t* target) {
  constexpr uint32_t kBytes = 1u << kLog2;
  constexpr uint32_t kGroup = (16u >> kLog2) < 8u ? (16u >> kLog2) : 8u;  // blocks contiguous in the source
  const int32_t row_macros = int32_t(((pitch + 31) & ~uint32_t(31)) >> 5);
  for (uint32_t row = 0; row < by; ++row) {
    const int32_t y = int32_t(oy + row);
    const int32_t macro_y = (y >> 5) * row_macros;
    const int32_t micro_y = (y & 0xE) << 2;
    const int32_t y1 = (y & 1) << 4;
    const int32_t y16 = (y & 16) << 7;
    const int32_t y8 = (y & 8) >> 2;
    uint8_t* output = target + size_t(row) * bx_host * kBytes;
    uint32_t column = 0;
    while (column < bx) {
      const int32_t x = int32_t(ox + column);
      const int32_t macro = ((x >> 5) + macro_y) << (kLog2 + 7);
      const int32_t micro = ((x & 7) + micro_y) << kLog2;
      const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + y1;
      const int32_t byte_offset = ((offset & ~0x1FF) << 3) + y16 + ((offset & 0x1C0) << 2) +
                                     (((y8 + (x >> 3)) & 3) << 6) + (offset & 0x3F);
      if (kGroup > 1 && (uint32_t(x) % kGroup) == 0 && column + kGroup <= bx) {
        std::memcpy(output + size_t(column) * kBytes, src + byte_offset, kGroup * kBytes);
        column += kGroup;
      } else {
        std::memcpy(output + size_t(column) * kBytes, src + byte_offset, kBytes);
        ++column;
      }
    }
  }
}

// Byte swap of a whole texture in one go (instead of GpuSwap word by word, with the switch on the order
// inside the loop). Same result as GpuSwap: for 16-bit units only k8in16 changes anything; for 32-bit
// units, k8in16, k8in32 and k16in32. It only touches complete units, like the plain loop.
inline void ChangeOrderBytes(uint8_t* payload, size_t bytes, uint32_t unit, uint32_t orden) {
  constexpr uint32_t k8in16 = 1, k8in32 = 2, k16in32 = 3;  // xenos::Endian
  size_t n = unit == 2 ? bytes & ~size_t(1) : bytes & ~size_t(3);
  if ((unit == 2 && orden != k8in16) || (unit != 2 && unit != 4) || orden == 0) {
    return;
  }
  size_t i = 0;
#if defined(__aarch64__)
  for (; i + 16 <= n; i += 16) {
    const uint8x16_t v = vld1q_u8(payload + i);
    uint8x16_t r;
    if (unit == 2 || orden == k8in16) {
      r = vrev16q_u8(v);
    } else if (orden == k8in32) {
      r = vrev32q_u8(v);
    } else {
      r = vreinterpretq_u8_u16(vrev32q_u16(vreinterpretq_u16_u8(v)));
    }
    vst1q_u8(payload + i, r);
  }
#endif
  if (unit == 2 || orden == k8in16) {
    for (; i + 2 <= n; i += 2) {
      std::swap(payload[i], payload[i + 1]);
    }
  } else if (orden == k8in32) {
    for (; i + 4 <= n; i += 4) {
      std::swap(payload[i], payload[i + 3]);
      std::swap(payload[i + 1], payload[i + 2]);
    }
  } else if (orden == k16in32) {
    for (; i + 4 <= n; i += 4) {
      std::swap(payload[i], payload[i + 2]);
      std::swap(payload[i + 1], payload[i + 3]);
    }
  }
}

// The same for a 3D texture tiled in 32x32x4 blocks, copied from GetTiledOffset3D
// (graphics/pipeline/texture/util.cpp:438-459). pitch and height in blocks.
int32_t Tile3dOffset(int32_t x, int32_t y, int32_t z, uint32_t pitch, uint32_t height_px,
                                uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  height_px = (height_px + 31) & ~uint32_t(31);
  const int32_t outer_macro = ((y >> 4) + (z >> 2) * int32_t(height_px >> 4)) * int32_t(pitch >> 5);
  const int32_t macro = ((((x >> 5) + outer_macro) << (log2_bytes + 6)) & 0xFFFFFFF) << 1;
  const int32_t micro = (((x & 7) + ((y & 6) << 2)) << (log2_bytes + 6)) >> 6;
  const int32_t outer = ((y >> 3) + (z >> 2)) & 1;
  const int32_t offset1 = outer + ((((x >> 3) + (outer << 1)) & 3) << 1);
  const int32_t offset2 = ((macro + (micro & ~15)) << 1) + (micro & 15) +
                          ((z & 3) << (log2_bytes + 6)) + ((y & 1) << 4);
  int32_t address = (offset1 & 1) << 3;
  address += (offset2 >> 6) & 7;
  address <<= 3;
  address += offset1 & ~1;
  address <<= 2;
  address += offset2 & ~511;
  address <<= 3;
  address += offset2 & 63;
  return address;
}

// USAGE_LOCATIONS de XenosRecomp (shader_recompiler.cpp).
int32_t UseLocation(uint8_t use, uint8_t index) {
  switch (use) {
    case 0:  // posicion
      return index == 0 ? 0 : (index == 1 ? 15 : -1);
    case 3:  // normal
      return index == 0 ? 1 : -1;
    case 6:  // tangente
      return index == 0 ? 2 : -1;
    case 7:  // binormal
      return index == 0 ? 3 : -1;
    case 5:  // texcoord
      return index < 4 ? 4 + index : (index < 8 ? 12 + (index - 4) : -1);
    case 10:  // color
      return index == 0 ? 8 : (index == 1 ? 11 : -1);
    case 2:  // indices de mezcla
      return index == 0 ? 9 : -1;
    case 1:  // pesos de mezcla
      return index == 0 ? 10 : -1;
    default:
      return -1;
  }
}

// USAGE_TYPES: these inputs are uint4 in the translated shaders.
bool IntEntry(uint8_t use) {
  // Only BLENDINDICES. Since the nfsmw_validado_normales library, normals, tangents and binormals are
  // float4: NFSMW stores them as 16-bit integers (format 26) or as floats, and with uint4 the shader's
  // asfloat gave degenerate directions (seen in the rear-view mirror).
  return use == 2;
}

uint32_t VertexComponents(uint32_t format_id) {
  switch (format_id) {
    case 33:
    case 36:
      return 1;
    case 25:
    case 31:
    case 34:
    case 37:
      return 2;
    case 16:
    case 17:
    case 57:
      return 3;
    default:
      return 4;
  }
}

// Components written by a fetch swizzle (the ones that are not 7), as a 4-bit mask.
uint32_t WrittenMask(uint32_t swizzle) {
  uint32_t mask = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (((swizzle >> (i * 3)) & 0x7) != 7) {
      mask |= 1u << i;
    }
  }
  return mask;
}

// g_InputRemap code: the SPIR-V writes r[i] = entrada[orig[i]] and the fetch patched by D3D writes
// r[i] = dato[parcheado[i]] (or 0 / 1). For each written component, the host input at orig[i] has to
// come from parcheado[i]. 7 = the same component.
uint32_t RemapCode(uint32_t original, uint32_t patched) {
  uint32_t code = kIdentityRemap;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t o = (original >> (i * 3)) & 0x7;
    const uint32_t d = (patched >> (i * 3)) & 0x7;
    if (o <= 3 && d != 7) {
      code = (code & ~(uint32_t(0x7) << (o * 3))) | (d << (o * 3));
    }
  }
  return code;
}

VkFormat AttributeFormat(uint32_t format_id, bool int_entry, bool with_sign, bool integer_value,
                         bool red_blue, bool& r11g11b10) {
  r11g11b10 = false;
  const auto choose = [&](VkFormat unorm, VkFormat snorm, VkFormat uscaled, VkFormat sscaled,
                          VkFormat uint_, VkFormat sint) {
    if (int_entry) {
      return with_sign ? sint : uint_;
    }
    if (integer_value) {
      return with_sign ? sscaled : uscaled;
    }
    return with_sign ? snorm : unorm;
  };
  switch (format_id) {
    case 6:  // k_8_8_8_8
      return red_blue ? choose(VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SNORM,
                                VK_FORMAT_B8G8R8A8_USCALED, VK_FORMAT_B8G8R8A8_SSCALED,
                                VK_FORMAT_B8G8R8A8_UINT, VK_FORMAT_B8G8R8A8_SINT)
                       : choose(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM,
                                VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_SSCALED,
                                VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT);
    case 7:  // k_2_10_10_10
      if (red_blue) break;
      return choose(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_SNORM_PACK32,
                    VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_A2B10G10R10_SSCALED_PACK32,
                    VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_A2B10G10R10_SINT_PACK32);
    case 16:  // k_10_11_11: packed normal decoded by the shader itself
      if (!int_entry || red_blue) break;
      r11g11b10 = true;
      return VK_FORMAT_R32_UINT;
    case 25:  // k_16_16
      if (red_blue) break;
      return choose(VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_USCALED,
                    VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT);
    case 26:  // k_16_16_16_16
      if (red_blue) break;
      return choose(VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM,
                    VK_FORMAT_R16G16B16A16_USCALED, VK_FORMAT_R16G16B16A16_SSCALED,
                    VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT);
    case 31:  // k_16_16_FLOAT
      return int_entry || red_blue ? VK_FORMAT_UNDEFINED : VK_FORMAT_R16G16_SFLOAT;
    case 32:  // k_16_16_16_16_FLOAT
      return int_entry || red_blue ? VK_FORMAT_UNDEFINED : VK_FORMAT_R16G16B16A16_SFLOAT;
    case 33:  // k_32
      return int_entry && !red_blue ? (with_sign ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT)
                                         : VK_FORMAT_UNDEFINED;
    case 34:  // k_32_32
      return int_entry && !red_blue
                 ? (with_sign ? VK_FORMAT_R32G32_SINT : VK_FORMAT_R32G32_UINT)
                 : VK_FORMAT_UNDEFINED;
    case 35:  // k_32_32_32_32
      return int_entry && !red_blue
                 ? (with_sign ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT)
                 : VK_FORMAT_UNDEFINED;
    // Floats: a uint4 input reinterprets the bits (asfloat in the shader).
    case 36:  // k_32_FLOAT
      return red_blue ? VK_FORMAT_UNDEFINED
                       : (int_entry ? VK_FORMAT_R32_UINT : VK_FORMAT_R32_SFLOAT);
    case 37:  // k_32_32_FLOAT
      return red_blue ? VK_FORMAT_UNDEFINED
                       : (int_entry ? VK_FORMAT_R32G32_UINT : VK_FORMAT_R32G32_SFLOAT);
    case 57:  // k_32_32_32_FLOAT
      return red_blue ? VK_FORMAT_UNDEFINED
                       : (int_entry ? VK_FORMAT_R32G32B32_UINT : VK_FORMAT_R32G32B32_SFLOAT);
    case 38:  // k_32_32_32_32_FLOAT
      return red_blue ? VK_FORMAT_UNDEFINED
                       : (int_entry ? VK_FORMAT_R32G32B32A32_UINT
                                         : VK_FORMAT_R32G32B32A32_SFLOAT);
    default:
      break;
  }
  return VK_FORMAT_UNDEFINED;
}

constexpr uint16_t kSwizzleRRRR = 0;
constexpr uint16_t kSwizzleRGGG = (1 << 3) | (1 << 6) | (1 << 9);
constexpr uint16_t kSwizzleRGBA = (1 << 3) | (2 << 6) | (3 << 9);
constexpr uint16_t kSwizzleBGRA = 2 | (1 << 3) | (0 << 6) | (3 << 9);

struct TextureFormat {
  VkFormat format_id = VK_FORMAT_UNDEFINED;
  uint8_t block = 1;       // texels per block side
  uint8_t bytes = 1;        // bytes per block
  uint8_t order_unit = 0;  // 0 = no byte swap, else 2 or 4 bytes
  uint16_t swizzle_host = kSwizzleRGBA;
};

// Same host formats as the emulation (vulkan/texture_cache.cpp:123-397).
// AirborneNX: sRGB variant of a texture format, or VK_FORMAT_UNDEFINED.
VkFormat SrgbFormat(VkFormat format_id) {
  switch (format_id) {
    case VK_FORMAT_R8G8B8A8_UNORM:
      return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM:
      return VK_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
      return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case VK_FORMAT_BC2_UNORM_BLOCK:
      return VK_FORMAT_BC2_SRGB_BLOCK;
    case VK_FORMAT_BC3_UNORM_BLOCK:
      return VK_FORMAT_BC3_SRGB_BLOCK;
    default:
      return VK_FORMAT_UNDEFINED;
  }
}

bool TextureFormatOf(uint32_t format_id, TextureFormat& f) {
  switch (format_id) {
    case 2:  // k_8
    case 8:  // k_8_A
      f = {VK_FORMAT_R8_UNORM, 1, 1, 0, kSwizzleRRRR};
      return true;
    case 10:  // k_8_8
      f = {VK_FORMAT_R8G8_UNORM, 1, 2, 2, kSwizzleRGGG};
      return true;
    case 6:   // k_8_8_8_8
    case 50:  // k_8_8_8_8_AS_16_16_16_16
      f = {VK_FORMAT_R8G8B8A8_UNORM, 1, 4, 4, kSwizzleRGBA};
      return true;
    case 7:   // k_2_10_10_10
    case 54:  // k_2_10_10_10_AS_16_16_16_16
      f = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 1, 4, 4, kSwizzleRGBA};
      return true;
    case 18:  // k_DXT1
    case 51:
      f = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 8, 2, kSwizzleRGBA};
      return true;
    case 19:  // k_DXT2_3
    case 52:
      f = {VK_FORMAT_BC2_UNORM_BLOCK, 4, 16, 2, kSwizzleRGBA};
      return true;
    case 20:  // k_DXT4_5
    case 53:
      f = {VK_FORMAT_BC3_UNORM_BLOCK, 4, 16, 2, kSwizzleRGBA};
      return true;
    case 49:  // k_DXN
      f = {VK_FORMAT_BC5_UNORM_BLOCK, 4, 16, 2, kSwizzleRGGG};
      return true;
    case 59:  // k_DXT5A
      f = {VK_FORMAT_BC4_UNORM_BLOCK, 4, 8, 2, kSwizzleRRRR};
      return true;
    case 24:  // k_16
      f = {VK_FORMAT_R16_UNORM, 1, 2, 2, kSwizzleRRRR};
      return true;
    case 25:  // k_16_16
      f = {VK_FORMAT_R16G16_UNORM, 1, 4, 2, kSwizzleRGGG};
      return true;
    case 26:  // k_16_16_16_16
      f = {VK_FORMAT_R16G16B16A16_UNORM, 1, 8, 2, kSwizzleRGBA};
      return true;
    case 30:  // k_16_FLOAT
      f = {VK_FORMAT_R16_SFLOAT, 1, 2, 2, kSwizzleRRRR};
      return true;
    case 31:  // k_16_16_FLOAT
      f = {VK_FORMAT_R16G16_SFLOAT, 1, 4, 2, kSwizzleRGGG};
      return true;
    case 32:  // k_16_16_16_16_FLOAT
      f = {VK_FORMAT_R16G16B16A16_SFLOAT, 1, 8, 2, kSwizzleRGBA};
      return true;
    case 36:  // k_32_FLOAT
      f = {VK_FORMAT_R32_SFLOAT, 1, 4, 4, kSwizzleRRRR};
      return true;
    case 37:  // k_32_32_FLOAT
      f = {VK_FORMAT_R32G32_SFLOAT, 1, 8, 4, kSwizzleRGGG};
      return true;
    case 38:  // k_32_32_32_32_FLOAT
      f = {VK_FORMAT_R32G32B32A32_SFLOAT, 1, 16, 4, kSwizzleRGBA};
      return true;
    default:
      return false;
  }
}

bool IsDepth(VkFormat format_id) {
  return format_id == VK_FORMAT_D24_UNORM_S8_UINT || format_id == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

// GuestToHostSwizzle: the fetch constant's swizzle, mapped through the channels the host format has.
VkComponentMapping ComponentsMapping(uint32_t swizzle_guest, uint16_t swizzle_host) {
  VkComponentMapping mapping{};
  VkComponentSwizzle* output[4] = {&mapping.r, &mapping.g, &mapping.b, &mapping.a};
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t value = (swizzle_guest >> (i * 3)) & 0x7;
    if (value <= 3) {
      const uint32_t host = (swizzle_host >> (value * 3)) & 0x7;
      *output[i] = VkComponentSwizzle(VK_COMPONENT_SWIZZLE_R + host);
    } else if (value == 4) {
      *output[i] = VK_COMPONENT_SWIZZLE_ZERO;
    } else {
      *output[i] = VK_COMPONENT_SWIZZLE_ONE;
    }
  }
  return mapping;
}

struct VertexAttribute {
  uint32_t loc_index;
  uint32_t bind_index;
  VkFormat format_id;
  uint32_t offset;
};

struct VertexBinding {
  uint32_t ranura;   // fetch constant de vertices (0-95)
  uint32_t stride_bytes;  // bytes
};

struct VertexInput {
  std::vector<VertexAttribute> attributes;
  std::vector<VertexBinding> bindings;
  std::array<uint32_t, 16> remaps{};  // g_InputRemap by location
  uint32_t specialization = 0;
  uint64_t fingerprint = 0;
};

// Copy of the SPIR-V without the writes to the color targets (output variables with Location 0..3).
// Returns empty if the module cannot be walked or there was nothing to remove.
std::vector<uint32_t> PruneColorWrites(const std::vector<uint32_t>& spirv, uint32_t& removed) {
  constexpr uint32_t kMagic = 0x07230203u;
  constexpr uint32_t kOpEntryPoint = 15, kOpDecorate = 71, kOpVariable = 59, kOpStore = 62;
  constexpr uint32_t kOpAccessChain = 65, kOpInBoundsAccessChain = 66;
  constexpr uint32_t kDecoracionBuiltIn = 11, kDecoracionLocation = 30, kDecoracionIndex = 29;
  constexpr uint32_t kOutputStore = 3;
  removed = 0;
  if (spirv.size() < 5 || spirv[0] != kMagic) {
    return {};
  }
  std::unordered_set<uint32_t> con_location, discarded;
  // 1) decorations: Location 0..3 marks a color target; BuiltIn or Index != 0 rule it out (gl_FragDepth
  // and the second blend source are not touched).
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t word_data = spirv[i] >> 16;
    const uint32_t code = spirv[i] & 0xFFFF;
    if (word_data == 0 || i + word_data > spirv.size()) {
      return {};
    }
    if (code == kOpDecorate && word_data >= 3) {
      const uint32_t goal = spirv[i + 1];
      const uint32_t decoracion = spirv[i + 2];
      if (decoracion == kDecoracionLocation && word_data >= 4 && spirv[i + 3] <= 3) {
        con_location.insert(goal);
      } else if (decoracion == kDecoracionBuiltIn ||
                 (decoracion == kDecoracionIndex && word_data >= 4 && spirv[i + 3] != 0)) {
        discarded.insert(goal);
      }
    }
    i += word_data;
  }
  // 2) pointers to those targets: the output variable and whatever is derived from it through
  // OpAccessChain.
  std::unordered_set<uint32_t> pointers;
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t word_data = spirv[i] >> 16;
    const uint32_t code = spirv[i] & 0xFFFF;
    if (code == kOpVariable && word_data >= 4 && spirv[i + 3] == kOutputStore) {
      const uint32_t id = spirv[i + 2];
      if (con_location.count(id) && !discarded.count(id)) {
        pointers.insert(id);
      }
    } else if ((code == kOpAccessChain || code == kOpInBoundsAccessChain) && word_data >= 4 &&
               pointers.count(spirv[i + 3])) {
      pointers.insert(spirv[i + 2]);
    }
    i += word_data;
  }
  if (pointers.empty()) {
    return {};
  }
  // 3) the writes to those pointers are dropped. The rest of the module is copied as is: the output
  // variable stays declared and in the entry point's interface, which is valid even if it is never written.
  std::vector<uint32_t> output;
  output.reserve(spirv.size());
  output.insert(output.end(), spirv.begin(), spirv.begin() + 5);
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t word_data = spirv[i] >> 16;
    const uint32_t code = spirv[i] & 0xFFFF;
    const bool outside = code == kOpStore && word_data >= 3 && pointers.count(spirv[i + 1]);
    if (outside) {
      ++removed;
    } else {
      output.insert(output.end(), spirv.begin() + i, spirv.begin() + i + word_data);
    }
    i += word_data;
  }
  (void)kOpEntryPoint;
  return removed ? output : std::vector<uint32_t>();
}

/*
 * The same module, but declaring OpExecutionMode EarlyFragmentTests.
 *
 * In SPIR-V the execution modes have their own section, right after the OpEntryPoint instructions and
 * before the debug strings and decorations. So it is enough to insert the instruction after the module's
 * last OpExecutionMode (there is always at least one, OriginUpperLeft, which is what DXC emits). No new
 * ids are created, so the header's "bound" does not change and the module stays valid.
 *
 * Returns empty (and then the normal module is used) if the module has no fragment entry point, if it
 * already declared it, or if it writes gl_FragDepth (DepthReplacing): in that last case testing earlier
 * would change the result, because the Z being tested is computed by the shader itself.
 */
std::vector<uint32_t> WithEarlyTests(const std::vector<uint32_t>& spirv, const char*& reason) {
  constexpr uint32_t kMagic = 0x07230203u;
  constexpr uint32_t kOpEntryPoint = 15, kOpExecutionMode = 16;
  constexpr uint32_t kFragmentModel = 4;
  constexpr uint32_t kEarlyMode = 9, kDepthReplacingMode = 12;
  reason = "";
  if (spirv.size() < 5 || spirv[0] != kMagic) {
    reason = "no es SPIR-V";
    return {};
  }
  uint32_t entry = 0;
  size_t where = 0;  // first word after the execution mode section
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t word_data = spirv[i] >> 16;
    const uint32_t code = spirv[i] & 0xFFFF;
    if (word_data == 0 || i + word_data > spirv.size()) {
      reason = "modulo mal formado";
      return {};
    }
    if (code == kOpEntryPoint && word_data >= 3) {
      if (spirv[i + 1] == kFragmentModel) {
        if (entry) {
          reason = "mas de un punto de entrada de fragmentos";
          return {};
        }
        entry = spirv[i + 2];
      }
      if (where < i + word_data) {
        where = i + word_data;
      }
    } else if (code == kOpExecutionMode && word_data >= 3) {
      if (spirv[i + 2] == kEarlyMode) {
        reason = "ya la declaraba";
        return {};
      }
      if (spirv[i + 2] == kDepthReplacingMode) {
        reason = "escribe gl_FragDepth";
        return {};
      }
      if (where < i + word_data) {
        where = i + word_data;
      }
    }
    i += word_data;
  }
  if (!entry || !where) {
    reason = "sin punto de entrada de fragmentos";
    return {};
  }
  std::vector<uint32_t> output;
  output.reserve(spirv.size() + 3);
  output.insert(output.end(), spirv.begin(), spirv.begin() + where);
  output.push_back((3u << 16) | kOpExecutionMode);
  output.push_back(entry);
  output.push_back(kEarlyMode);
  output.insert(output.end(), spirv.begin() + where, spirv.end());
  return output;
}

struct PipelineKey {
  uint32_t vs = 0;
  uint32_t ps = 0;
  uint64_t entry = 0;
  uint32_t topo = 0;
  uint32_t specialization = 0;
  uint32_t formats[5] = {};
  uint32_t blend[4] = {};
  uint32_t masks = 0;
  uint32_t depth_value = 0;
  uint32_t rasterizado = 0;
  // The key is hashed and compared byte by byte (PipelineDe). With 76 bytes of fields and 8-byte alignment
  // it had 4 bytes of uninitialized implicit padding: stack garbage that made identical keys differ and
  // created duplicate pipelines (125-134, and 193-203 in a later version, with no change in the image).
  uint32_t padding = 0;
  uint32_t fill2 = 0;
};
static_assert(std::has_unique_object_representations_v<PipelineKey>,
              "ClavePipeline no puede tener relleno implicito: se hashea y se compara byte a byte");

/*
 * One pipeline of the prewarm list (nfsmw_nativo_pipelines_precalentar).
 *
 * What is needed to recreate it in another session exactly as the ring created it: its key as PipelineDe
 * looks it up; the fingerprint of its two shaders in the library (if the library has changed, clave.vs
 * and clave.ps are no longer the same shaders and the record is skipped); and its vertex input, which the
 * key only carries as a fingerprint. It is stored on disk byte for byte, so it cannot have implicit
 * padding.
 */
struct AttributeRecord {
  uint32_t loc_index = 0;
  uint32_t bind_index = 0;
  uint32_t format_id = 0;  // VkFormat
  uint32_t offset = 0;
};

struct PipelineRecord {
  static constexpr uint32_t kMaxAttributes = 16;
  static constexpr uint32_t kMaxBindings = 16;
  PipelineKey key;
  uint64_t vs_fingerprint = 0;  // nfsmw::native::Shader::huella
  uint64_t ps_fingerprint = 0;  // 0 without a fragment stage (clave.ps == 0)
  uint32_t n_attributes = 0;
  uint32_t n_bindings = 0;
  AttributeRecord attributes[kMaxAttributes] = {};
  uint32_t strides[kMaxBindings] = {};
};
static_assert(std::has_unique_object_representations_v<PipelineRecord>,
              "RegistroPipeline se guarda en disco byte a byte: sin relleno implicito");

struct Texture {
  NativeImage img;
  uint32_t layer_count = 1;  // 6 for cubemaps
  uint32_t background = 0;  // slices of 3D textures; 0 for the rest
  uint64_t fingerprint = 0;
  uint64_t frame = UINT64_MAX;
  uint64_t raw_fingerprint = 0;  // XXH3 of the guest bytes (2D and cubemaps)
  uint64_t next = 0;     // frame of the next check
  uint32_t interval = 1;     // frames between checks: 1 to 32
  uint32_t deferrals = 0;  // consecutive checks deferred by the budget
  // Sample fingerprint (HuellaMuestra) of the same content as huella_cruda if muestra_valida, and how many
  // consecutive rechecks have been accepted on it alone (nfsmw_nativo_huellas_muestreo).
  uint64_t sample_fingerprint = 0;
  bool valid_sample = false;
  uint8_t consecutive_samples = 0;
  bool do_upload = false;
  std::vector<uint8_t> payload;  // levels already laid out for the host: level after level and, in each, layer after layer
  uint32_t levels = 1;        // mip levels of the host image
  std::array<uint32_t, 16> level_offset{};  // data bytes up to each level
  uint64_t bytes = 0;          // what it counts in bytes_texturas_ (for eviction)
  // 1 + its index in en_vuelo_ while the bind thread runs its vkBindImageMemory; 0 = no. While set, the
  // image exists but has no memory: no command may reference it until RecogerEnlaces.
  uint32_t in_flight = 0;
  // Its data and huella_cruda are prepared by the hash thread (nfsmw_nativo_texturas_huella_hilo). 0 = no;
  // 1 + its index in huellas_planeadas_ until SubirTextura; kTrabajoHuellaPublicado until RecogerHuellas
  // sets its huella_cruda. While nonzero it is not evicted and its huella_cruda is not valid yet.
  uint32_t job_fingerprint = 0;
  // Measurement only (nfsmw_nativo_diag_reutilizar): its shape without the address (when the image is
  // created), its address (only for the log), the content key it is filed under in vivas_por_contenido_
  // (0 = none) and whether it is new and its first fingerprint has not been measured yet.
  uint64_t content_shape = 0;
  uint64_t content_key = 0;
  uint32_t address = 0;
  bool content_per_measure = false;
};

struct View {
  VkImage img = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  uint32_t ranura = 0;
  uint32_t heap = 0;  // 0 2D textures, 2 cubemaps
};

class VulkanDrawerImpl final : public VulkanDrawer {
 public:
  VulkanDrawerImpl(const VulkanDevice* dev, rex::memory::Memory* mem,
                    TargetsContext* contexto)
      : vk_device_(dev),
        dfn_(dev->functions()),
        device_(dev->device()),
        mem_(mem),
        contexto_(contexto) {}

  ~VulkanDrawerImpl() override {
    StopPrewarmed();  // uses the pipeline cache and the layout: first
    StopCopies();
    StopBindings();  // in-flight vkBindImageMemory calls finish before any image is destroyed
    StopFingerprints();  // the hash thread writes to the upload buffer: join it before releasing that
    SaveCachePipelines();
    StopCacheWriter();  // writes whatever is still pending
    for (auto& [key, par] : pipelines_) {
      dfn_.vkDestroyPipeline(device_, par.second, nullptr);
    }
    if (cache_pipelines_ != VK_NULL_HANDLE) {
      destroy_cache_(device_, cache_pipelines_, nullptr);
    }
    for (auto& [key, framebuffer] : framebuffers_) {
      dfn_.vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    for (VkFramebuffer framebuffer : fb_retired_) {  // the ones OlvidarVista retired
      dfn_.vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    for (auto& [key, pass] : passes_) {
      dfn_.vkDestroyRenderPass(device_, pass, nullptr);
    }
    for (auto& [entry, spv_module] : modules_) {
      dfn_.vkDestroyShaderModule(device_, spv_module, nullptr);
    }
    for (auto& [entry, spv_module] : alpha_only_modules_) {
      if (spv_module != VK_NULL_HANDLE) {
        dfn_.vkDestroyShaderModule(device_, spv_module, nullptr);
      }
    }
    for (auto& [entry, spv_module] : early_modules_z_) {
      if (spv_module != VK_NULL_HANDLE) {
        dfn_.vkDestroyShaderModule(device_, spv_module, nullptr);
      }
    }
    for (VkShaderModule spv_module : variants_modules_) {
      if (spv_module != VK_NULL_HANDLE) {
        dfn_.vkDestroyShaderModule(device_, spv_module, nullptr);
      }
    }
    if (rect_list_gs_ != VK_NULL_HANDLE) {
      dfn_.vkDestroyShaderModule(device_, rect_list_gs_, nullptr);
    }
    for (auto& [key, view] : views_) {
      dfn_.vkDestroyImageView(device_, view.view, nullptr);
    }
    for (auto& [key, texture] : textures_) {
      DestroyImage(texture.img);
    }
    for (auto& [key, par] : samplers_) {
      dfn_.vkDestroySampler(device_, par.first, nullptr);
    }
    for (NativeImage& empty_image : empty_) {
      DestroyImage(empty_image);
    }
    // The pool's slabs are released after destroying every image that lives in them. The other way round
    // would free memory that the VkImages still have bound.
    textures_pool_.Finish();
    if (empty_sampler_ != VK_NULL_HANDLE) dfn_.vkDestroySampler(device_, empty_sampler_, nullptr);
    if (layout_pipeline_ != VK_NULL_HANDLE)
      dfn_.vkDestroyPipelineLayout(device_, layout_pipeline_, nullptr);
    if (pool_ != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorPool(device_, pool_, nullptr);
    if (pool_ubo_ != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorPool(device_, pool_ubo_, nullptr);
    if (layout_ubo_ != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorSetLayout(device_, layout_ubo_, nullptr);
    for (VkDescriptorSetLayout layout : layouts_) {
      if (layout != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorSetLayout(device_, layout, nullptr);
    }
    for (const UploadBuffer& s : bufs_shared_) {
      if (s.payload) dfn_.vkUnmapMemory(device_, s.mem);
      if (s.buf != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, s.buf, nullptr);
      if (s.mem != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, s.mem, nullptr);
    }
    for (const UploadBuffer& s : uploads_) {
      if (s.payload) dfn_.vkUnmapMemory(device_, s.mem);
      if (s.buf != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, s.buf, nullptr);
      if (s.mem != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, s.mem, nullptr);
    }
  }

  bool Initialize() {
    const auto& props = vk_device_->properties();
    const std::pair<bool, const char*> requirements[] = {
        {props.shaderInt64, "shaderInt64"},
        {props.bufferDeviceAddress, "bufferDeviceAddress"},
        {props.runtimeDescriptorArray, "runtimeDescriptorArray"},
        {props.shaderSampledImageArrayDynamicIndexing,
         "shaderSampledImageArrayDynamicIndexing"},
        {props.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound"},
        {props.descriptorBindingSampledImageUpdateAfterBind,
         "descriptorBindingSampledImageUpdateAfterBind"},
        {props.descriptorBindingUpdateUnusedWhilePending,
         "descriptorBindingUpdateUnusedWhilePending"},
    };
    for (const auto& [present, name] : requirements) {
      if (!present) {
        REXLOG_ERROR("[nativo] C6: el dispositivo Vulkan no tiene {}: no se dibuja", name);
        return false;
      }
    }
    buffer_address_ = reinterpret_cast<BufferAddressFn>(
        vk_device_->vulkan_instance()->functions().vkGetDeviceProcAddr(device_,
                                                                          "vkGetBufferDeviceAddress"));
    copy_image_ = reinterpret_cast<CopyImageFn>(
        vk_device_->vulkan_instance()->functions().vkGetDeviceProcAddr(device_, "vkCmdCopyImage"));
    LoadCachePipelines();
    LoadDynamicState();  // dynamic state phases 1 and 2
    if (!buffer_address_ || !CreateUpload() || !CreateDescriptors()) {
      return false;
    }
    // The pool is created after CrearDescriptores (where texturas_mb_max_ is read) and before CrearVacias,
    // so the three empty images can already come from it. The slabs are prewarmed here, while the game is
    // still loading: a 32 MB memset during a race would be a 10-15 ms stutter.
    textures_pool_.Start(vk_device_, textures_max_mb_);
    return CreateEmpty();
  }

  bool Draw(const DrawRequest& p) override {
    VegetationVerdict(p);  // the ring's versus the game's (nfsmw_d3d_vegetacion_juego)
    // Stage stopwatch on 1 in kCronometroCada draws: reading the clock 12 times per draw took almost a
    // quarter of the ring thread's busy CPU on PC, and on the Switch each read costs more. The C6 averages
    // come from the sample.
    time_ = (++counter_stopwatch_ & (kStopwatchEvery - 1)) == 0;
    auto mark = time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const uint32_t* r = p.regs;
    if (!r || !p.vs) {
      return false;
    }
    // --- Tipo de primitiva ---------------------------------------------------
    const uint32_t edram_mode = r[gr::XE_GPU_REG_RB_MODECONTROL] & 0x7;
    if (edram_mode != uint32_t(xenos::EdramMode::kColorDepth) && edram_mode != 5) {
      return Reject(1, "modo EDRAM sin color ni profundidad");
    }
    // Mode 5 (depth only): the Xenos does not run the pixel shader (IsPixelShaderNeededWithRasterization,
    // graphics/util/draw.cpp:125-129). Without a PS there are no textures, pixel constants or alpha test,
    // and the pipeline has no fragment stage. The race shadows work this way, and some arrive with the PS
    // object set to 0.
    const ShaderEntry* ps =  // can be removed if it writes no color and does not discard
        edram_mode == uint32_t(xenos::EdramMode::kColorDepth) ? p.ps : nullptr;
    if (!ps && edram_mode == uint32_t(xenos::EdramMode::kColorDepth)) {
      return false;
    }
    // Diagnostic: PS whose draws are skipped (nfsmw_nativo_diag_omitir_ps).
    const std::string& skip = diag_skip_text_ps_;
    if (skip != skip_text_) {
      skip_text_ = skip;
      skip_ps_.clear();
      uint32_t number = 0;
      bool hay = false;
      for (char c : skip + ",") {
        if (c >= '0' && c <= '9') {
          number = number * 10 + uint32_t(c - '0');
          hay = true;
        } else if (hay) {
          skip_ps_.insert(number);
          number = 0;
          hay = false;
        }
      }
      if (!skip_ps_.empty()) {
        REXLOG_WARN("[nativo] C6 diag: no se dibujan los PS {}", skip_text_);
      }
    }
    if (ps && !skip_ps_.empty() && skip_ps_.count(ps->number)) {
      return true;
    }
    const uint32_t initiator = r[gr::XE_GPU_REG_VGT_DRAW_INITIATOR];
    const uint32_t type = initiator & 0x3F;
    const uint32_t src_select = (initiator >> 6) & 0x3;
    const bool indices32 = (initiator >> 11) & 0x1;
    const uint32_t num = initiator >> 16;
    if (!num) {
      return true;
    }
    VkPrimitiveTopology topo;
    bool quads = false;
    bool rect_list = false;
    bool supports_reset = false;
    switch (type) {
      case 2:
        topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        break;
      case 3:
        topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        supports_reset = true;
        break;
      case 4:
        topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        break;
      case 5:
        topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        supports_reset = true;
        break;
      case 6:
        topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        supports_reset = true;
        break;
      case 13:
        topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        quads = true;
        break;
      case 8:  // AirborneNX: rectangle list (UE3's depth restore before the full-screen lighting, XDK clears)
        if (rect_list_gs_ == VK_NULL_HANDLE) {
          return Reject(100 + type, "tipo de primitiva todavia no soportado");
        }
        topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        rect_list = true;
        break;
      default:
        return Reject(100 + type, "tipo de primitiva todavia no soportado");
    }
    if (src_select == uint32_t(xenos::SourceSelect::kImmediate)) {
      return Reject(2, "indices inmediatos");
    }
    // Clipping disabled on the Xenos: the position may come in pixels (videos). It is drawn with a viewport
    // the size of the render target and the transform in the VS, like the emulation
    // (graphics/util/draw.cpp:341-363).
    const bool without_clip = (r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL] >> 16) & 0x1;
    const uint32_t sc_mode = r[gr::XE_GPU_REG_PA_SU_SC_MODE_CNTL];
    if (((sc_mode >> 3) & 0x3) == 2 && ((sc_mode >> 5) & 0x7) != 2) {
      return Reject(4, "poligonos dibujados como puntos o lineas");
    }

    const VertexInput* entry = EntryFor(p);
    if (!entry) {
      return false;
    }
    Stage(0, mark);
    std::chrono::steady_clock::time_point t_indices_185 = mark;  // C6 subetapas 15-18

    // --- Destinos --------------------------------------------------------------
    // AirborneNX: a 4X MSAA surface keeps each pixel's samples as a 2x2 block of EDRAM, so in 1X terms it is
    // twice as wide and tall. It is drawn into the 1X surface of twice the pitch with the viewport scaled by 2
    // (StartPass), which aliases what other passes draw at the same base: the XDK clears UE3's 1280-pitch
    // depth through a 640-pitch 4X rectangle. 2X surfaces keep their own pixel space (they only reach 1X
    // ones through resolves).
    // With airborne_edram_4x_alias off, 4X surfaces keep their own pixel space as before.
    const bool msaa_4x = edram_4x_alias_ &&
                         ((r[gr::XE_GPU_REG_RB_SURFACE_INFO] >> 16) & 0x3) == uint32_t(xenos::MsaaSamples::k4X);
    const uint32_t pitch = (r[gr::XE_GPU_REG_RB_SURFACE_INFO] & 0x3FFF) * (msaa_4x ? 2 : 1);
    const uint32_t reg_mask =
        edram_mode == uint32_t(xenos::EdramMode::kColorDepth) ? r[gr::XE_GPU_REG_RB_COLOR_MASK] : 0;
    static constexpr uint32_t kInfoColor[4] = {
        gr::XE_GPU_REG_RB_COLOR_INFO, gr::XE_GPU_REG_RB_COLOR1_INFO,
        gr::XE_GPU_REG_RB_COLOR2_INFO, gr::XE_GPU_REG_RB_COLOR3_INFO};
    uint64_t keys[5] = {};
    uint32_t masks = 0;
    bool has_target = false;
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t mask = (reg_mask >> (i * 4)) & 0xF;
      if (!mask || !ps || !((ps->outputs >> i) & 0x1)) {
        continue;
      }
      const uint32_t info = r[kInfoColor[i]];
      const uint32_t format_id = (info >> 16) & 0xF;
      if (!SupportedColorTargetFormat(format_id)) {
        return Reject(200 + format_id, "formato de destino de color todavia no soportado");
      }
      keys[i] = (uint64_t(1) << 63) | (uint64_t(info & 0xFFF) << 24) | (uint64_t(format_id) << 16) |
                  pitch;
      masks |= mask << (i * 4);
      has_target = true;
    }
    /*
     * Draws that cannot change a single pixel (nfsmw_nativo_saltar_invisibles).
     *
     * Two cases, read from this draw's own registers without any heuristics:
     *
     *  - Blending is "0 x source + 1 x destination" (ADD) on the written channels: the result is the
     *    destination as is. It is checked per channel group because the mask may write only RGB or only
     *    alpha, and then the other group's factors do not matter.
     *  - The alpha test function is 0 = NEVER: alphaTestValue (shader_common.h) returns -1 and clip() kills
     *    every fragment, before depth is written.
     *
     * If there is also no depth or stencil left to write, the whole draw is unnecessary: it is dropped. If
     * it writes depth it is drawn anyway and only counted, because setting the color mask to 0 would send it
     * down the "no color" path (kSpecSoloAlfa), where nfsmw_sombras_sin_vegetacion drops the whole draw and
     * its depth would be lost. Removing the color write saves too little to be worth it.
     * The pruning is skipped while an occlusion query is open: the game reads those samples.
     */
    if (skip_invisible_ && masks) {
      static constexpr uint32_t kBlendOf[4] = {
          gr::XE_GPU_REG_RB_BLENDCONTROL0, gr::XE_GPU_REG_RB_BLENDCONTROL1,
          gr::XE_GPU_REG_RB_BLENDCONTROL2, gr::XE_GPU_REG_RB_BLENDCONTROL3};
      const uint32_t control_color_inv = r[gr::XE_GPU_REG_RB_COLORCONTROL];
      const bool never_alpha =
          ((control_color_inv >> 3) & 0x1) && (control_color_inv & 0x7) == 0;  // function NEVER
      uint32_t usable_masks = 0;
      for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t mask_i = (masks >> (i * 4)) & 0xF;
        if (!mask_i) {
          continue;
        }
        const uint32_t m = r[kBlendOf[i]] & 0x1FFF1FFF;
        const bool color_copy = (m & 0x1F) == 0 && ((m >> 8) & 0x1F) == 1 && ((m >> 5) & 0x7) == 0;
        const bool alpha_copy =
            ((m >> 16) & 0x1F) == 0 && ((m >> 24) & 0x1F) == 1 && ((m >> 21) & 0x7) == 0;
        const bool rgb_write = (mask_i & 0x7) != 0 && !color_copy;
        const bool alpha_write = (mask_i & 0x8) != 0 && !alpha_copy;
        if (rgb_write || alpha_write) {
          usable_masks |= mask_i << (i * 4);
        }
      }
      if ((!usable_masks || never_alpha) && !open_occlusion_) {
        // With the NEVER function even the depth write dies (clip() runs in the shader, before the merger),
        // so the draw leaves no trace of any kind. With the blending that copies the destination, the draw can
        // only be dropped if it writes neither Z nor stencil either.
        const uint32_t dc = r[gr::XE_GPU_REG_RB_DEPTHCONTROL];
        const bool trace_leaves =
            !never_alpha && ((((dc >> 1) & 0x1) && ((dc >> 2) & 0x1)) || (dc & 0x1));
        ++counts_z_[never_alpha ? kInvisibleAlfa : kBlendInvisible];
        if (!trace_leaves) {
          return true;
        }
        ++counts_z_[kColorOnlyInvisible];  // drawn anyway: only counts what is left to gain
      }
    }
    // With no color to write, the pixel shader can only have an effect by discarding pixels (alpha test or
    // its own kill) or by writing depth. If it does neither, the pipeline has no fragment stage and the image
    // is identical: the GPU saves shading the whole shadow map.
    // Of the draws that do write color, how many can use early depth rejection and how many force shading
    // before testing. That is what decides whether a depth pre-pass would help at all.
    if (masks && ps && has_target) {
      const uint32_t now_control_color = r[gr::XE_GPU_REG_RB_COLORCONTROL];
      const bool alpha_test =
          ((now_control_color >> 3) & 0x1) && (now_control_color & 0x7) != 7;
      const bool discards = alpha_test || ps->discards || (ps->outputs & 0x10);
      if (pitch >= 1600) {
        // shadow map: already counted separately
      } else if (discards) {
        ++scene_with_discard_;
      } else {
        ++scene_without_discard_;
      }
    }
    bool only_alpha = false;
    if (!masks && ps) {
      const uint32_t now_control_color = r[gr::XE_GPU_REG_RB_COLORCONTROL];
      const bool alpha_test =
          ((now_control_color >> 3) & 0x1) && (now_control_color & 0x7) != 7;
      if (!alpha_test && !ps->discards && !(ps->outputs & 0x10)) {
        ++useless_ps_draws_;
        if (cvars_per_frame_ ? without_ps_without_frame_color_ : NoPsNoColor()) {
          ps = nullptr;
        }
      } else {
        ++required_ps_draws_;
        // Diagnostic: in the shadow map the pixel shader reads constant c1 (g_bShadowMapAlphaEnabled). With
        // that constant at 0 it neither samples nor discards: the stage would be unnecessary.
        if (pitch >= 1600) {
          (Float(r[kRegConstantesPs + 4]) != 0.0f ? ++shadows_active_alpha_ : ++shadows_alpha_disabled_);
        }
        // Needed for the alpha test or a kill, but its color goes nowhere: it is compiled without those
        // writes.
        if (cvars_per_frame_ ? frame_alpha_only_ps_ : PsAlphaOnly()) {
          only_alpha = true;
        }
      }
    }
    /*
     * Shadow map vegetation discard, as early as possible.
     *
     * This same discard used to live 640 lines further down, right before the pipeline lookup. So these
     * draws were paid in full and then thrown away: index conversion, vertex sources, PrepararTextura for all
     * their samplers, upload buffer allocation, pass change, vertex copy, index and constant memcpy,
     * viewport, scissor and pipeline lookup.
     *
     * And it is not a handful of draws: 604 per frame are dropped, 31 % of those that come in (the
     * `dibujos_ps_necesario` counter matches the lost ones exactly across twelve race intervals). At
     * ~8.5 us per incoming draw, that is 3-5 ms.
     *
     * Careful when measuring this: the divisor of `C6 etapas` is the recorded draws (~1,370), not the
     * incoming ones (~1,970), even though the label says otherwise. Multiplying by the incoming ones inflates
     * the budget by 45 %.
     *
     * The occlusion guard is free and removes the only doubt: with a game occlusion query open the draw
     * counts even if it is not visible, so there it takes the long path and the usual discard drops it.
     */
    // AirborneNX: only in the shadow map (pitch >= 1600, as everywhere else here) and only for shaders that do
    // not write depth. A colorless draw whose PS writes oDepth is UE3's depth restore before its full-screen
    // lighting; skipping it let the lights of hidden surfaces add up (see-through walls and characters).
    const bool vegetation_early = only_alpha && without_vegetation_ && !open_occlusion_ && pitch >= 1600 &&
                                  !(ps->outputs & 0x10);
    // The ring's verdict for the nfsmw_d3d_vegetacion_juego guard (VeredictoVegetacion) must be this same
    // decision. With the settings read on every draw (cvars_por_fotograma_ false) the alternation can change
    // between the two reads: then no comparison is made.
    if (vegetation_computed_ && cvars_per_frame_ && vegetation_ring_ != vegetation_early) {
      vegetation_detail_.early = vegetation_early;
      TrackModelVegetation(vegetation_flags_, vegetation_detail_);
    }
    if (vegetation_early) {
      ++soon_vegetation_draws_;
      return true;
    }
    const uint32_t depth_control = r[gr::XE_GPU_REG_RB_DEPTHCONTROL];
    if (depth_control & 0x3) {  // stencil o z
      const uint32_t info = r[gr::XE_GPU_REG_RB_DEPTH_INFO];
      keys[4] = (uint64_t(1) << 62) | (uint64_t(info & 0xFFF) << 24) |
                  (uint64_t((info >> 16) & 0x1) << 16) | pitch;
      has_target = true;
    }
    if (!has_target || !pitch) {
      // Diagnostic: a draw inside an occlusion query that writes nothing counts no samples.
      if (open_occlusion_ && pitch >= 640 && draw_occlusion_warnings_ < 8) {
        ++draw_occlusion_warnings_;
        REXLOG_INFO("[nativo] C2 oclusion dibujo {}: SIN DESTINO (no se dibuja): VS n{} PS n{} tipo {} cuenta {} pitch {} "
                    "mascara {:08X} profundidad {:08X} modo EDRAM {}",
                    draw_occlusion_warnings_, p.vs->number, ps ? int(ps->number) : -1, type, num, pitch,
                    reg_mask, depth_control, edram_mode);
      }
      return true;  // writes nothing visible
    }
    // FPS test nfsmw_nativo_omitir_sombras: the shadow map is depth-only and 1600 wide. With
    // nfsmw_nativo_omitir_sombras_alternar_s = N they are skipped in the odd N-second intervals.
    const bool shadows_target =  // also used for its depth bias
        pitch >= 1600 && keys[4] && !keys[0] && !keys[1] && !keys[2] && !keys[3];
    if (shadows_target) {
      // Step 2 of the 30 FPS guard. Removing the whole pass does not flicker; skipping it on 2 of every
      // 3 frames does, which is why that step no longer exists.
      bool skip = nfsmw::guard30::WithoutShadows(REXCVAR_GET(nfsmw_nativo_omitir_sombras));
      const int32_t alternar = REXCVAR_GET(nfsmw_nativo_omitir_sombras_alternar_s);
      if (!skip && alternar > 0) {
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - shadows_start_)
                                  .count();
        skip = (secs / alternar) % 2 == 1;
        if (skip != shadows_skipped_) {
          shadows_skipped_ = skip;
          REXLOG_INFO("[nativo] prueba de sombras: {} (fotograma {})",
                      skip ? "sin sombras" : "con sombras", frame_);
        }
      }
      if (skip) {
        return true;
      }
    }

    CutSubstage(15, t_indices_185);  // render targets and discards
    // --- Rango de vertices e indices --------------------------------------------
    const uint32_t byte_offset = r[gr::XE_GPU_REG_VGT_INDX_OFFSET] & 0xFFFFFF;
    const bool restart_flag = supports_reset && ((sc_mode >> 21) & 0x1);
    // Depth bias as in the emulation with host render targets (GetPreferredFacePolygonOffset,
    // graphics/util/draw.cpp:92-118).
    float bias_scale = 0.0f, bias_offset = 0.0f;
    if (type >= 4) {  // poligonos
      if (((sc_mode >> 11) & 0x1) && !(sc_mode & 0x1)) {
        bias_scale = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE]);
        bias_offset = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET]);
      }
      if (((sc_mode >> 12) & 0x1) && !((sc_mode >> 1) & 0x1) && bias_scale == 0.0f &&
          bias_offset == 0.0f) {
        bias_scale = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE]);
        bias_offset = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET]);
      }
    } else if ((sc_mode >> 13) & 0x1) {
      bias_scale = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE]);
      bias_offset = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET]);
    }
    // Constant scaled by the guest format's minimum value (2^24-1 for D24S8, 2^24 for D24FS8) and slope
    // from subpixels to pixels (vulkan/command_processor.cpp:5786-5799).
    float bias[2] = {
        bias_offset * (((r[gr::XE_GPU_REG_RB_DEPTH_INFO] >> 16) & 0x1)
                                    ? float(uint32_t(1) << 24)
                                    : float((uint32_t(1) << 24) - 1)),
        bias_scale * (1.0f / 16.0f)};
    // nfsmw_nativo_sombras_sesgo_*, only in the shadow map and only if the game does not set its own.
    if (shadows_target && type >= 4 && bias[0] == 0.0f && bias[1] == 0.0f &&
        (shadows_constant_bias_ != 0 || shadows_slope_bias_ != 0)) {
      bias[0] = float(shadows_constant_bias_) * 1000.0f;
      bias[1] = float(shadows_slope_bias_) * 0.1f;
    }
    const bool with_bias = bias[0] != 0.0f || bias[1] != 0.0f;
    if (with_bias && bias_warnings_ < 8 &&
        std::memcmp(bias, warned_bias_, sizeof(bias)) != 0) {
      ++bias_warnings_;
      std::memcpy(warned_bias_, bias, sizeof(bias));
      REXLOG_INFO("[nativo] C6: desplazamiento de profundidad: escala {} desplazamiento {} "
                  "(modo {:08X}) -> constante {} pendiente {}",
                  bias_scale, bias_offset, sc_mode, bias[0], bias[1]);
    }
    CutSubstage(16, t_indices_185);  // depth bias
    uint32_t vmin = UINT32_MAX;
    uint32_t vmax = 0;
    bool with_indices = false;
    bool indices16 = false;  // in indices16_ instead of indices_
    indices_.clear();
    if (src_select == uint32_t(xenos::SourceSelect::kDMA)) {
      const uint32_t byte_size = r[gr::XE_GPU_REG_VGT_DMA_SIZE];
      const auto orden = static_cast<xenos::Endian>(byte_size >> 30);
      const uint32_t bytes = indices32 ? 4 : 2;
      const uint32_t base = r[gr::XE_GPU_REG_VGT_DMA_BASE] & ~(bytes - 1);
      if (num > (byte_size & 0xFFFFFF) || uint64_t(base & 0x1FFFFFFF) + uint64_t(num) * bytes >
                                               kPhysicalMemory) {
        if (indices_warnings_ < 8) {
          ++indices_warnings_;
          REXLOG_WARN("[nativo] C6 diag: indices: cuenta {} palabras {} VGT_DMA_SIZE {:08X} "
                      "VGT_DMA_BASE {:08X} 32 bits {} tipo {} VS n{} PS n{}",
                      num, byte_size & 0xFFFFFF, byte_size, r[gr::XE_GPU_REG_VGT_DMA_BASE], indices32,
                      type, p.vs->number, ps ? int(ps->number) : -1);
        }
        return Reject(7, "indices fuera de su bufer");
      }
      const uint8_t* payload = mem_->TranslatePhysical(base & 0x1FFFFFFF);
      const uint32_t reset_index = r[gr::XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX] & 0xFFFFFF;
      if (!indices32 && !restart_flag && !byte_offset && !quads &&
          (orden == xenos::Endian::k8in16 || orden == xenos::Endian::kNone)) {
        // The normal case in NFSMW: 16 bits without restart or offset. They are uploaded as 16 bits, with a
        // branch-free loop the compiler can vectorize.
        indices16_.resize(num);
        uint16_t* output = indices16_.data();
        uint32_t min_value = 0xFFFF;
        uint32_t max_value = 0;
        // With hand-written NEON and its guard (IndicesDe16, nfsmw_nativo_indices_neon).
        Indices16(payload, num, output, orden == xenos::Endian::k8in16, min_value, max_value);
        vmin = min_value;
        vmax = max_value;
        indices16 = true;
      } else {
        indices_.resize(num);
        uint32_t* output = indices_.data();
        for (uint32_t i = 0; i < num; ++i) {
          uint32_t v;
          if (indices32) {
            uint32_t raw;
            std::memcpy(&raw, payload + size_t(i) * 4, 4);
            v = xenos::GpuSwap(raw, orden) & 0xFFFFFF;
          } else {
            uint16_t raw;
            std::memcpy(&raw, payload + size_t(i) * 2, 2);
            v = xenos::GpuSwap(raw, orden);
          }
          if (restart_flag && v == reset_index) {
            output[i] = UINT32_MAX;
            continue;
          }
          v = (v + byte_offset) & 0xFFFFFF;
          vmin = std::min(vmin, v);
          vmax = std::max(vmax, v);
          output[i] = v;
        }
      }
      if (vmin > vmax) {
        return true;  // resets only
      }
      with_indices = true;
    } else {
      vmin = byte_offset;
      vmax = byte_offset + num - 1;
      if (quads) {
        indices_.resize(num);
        for (uint32_t i = 0; i < num; ++i) {
          indices_[i] = byte_offset + i;
        }
        with_indices = true;
      }
    }
    if (vmax - vmin >= kMaxVerticesPerDraw) {
      return Reject(8, "rango de vertices demasiado grande");
    }
    if (quads) {
      const size_t n = indices_.size() / 4;
      converted_.resize(n * 6);
      for (size_t q = 0; q < n; ++q) {
        const uint32_t* i = &indices_[q * 4];
        uint32_t* o = &converted_[q * 6];
        o[0] = i[0]; o[1] = i[1]; o[2] = i[2];
        o[3] = i[0]; o[4] = i[2]; o[5] = i[3];
      }
      indices_.swap(converted_);
    }
    CutSubstage(17, t_indices_185);  // indices
    // The indices stay as they come: vkCmdDrawIndexed subtracts vmin through vertexOffset.
    const uint32_t vertices = vmax - vmin + 1;

    // Bytes of each vertex binding in guest memory.
    struct VertexSource {
      const uint8_t* payload;
      xenos::Endian orden;
      uint32_t bytes;
      uint64_t address;  // physical, of the first byte used
    };
    std::array<VertexSource, 16> sources{};
    if (entry->bindings.size() > sources.size()) {
      return Reject(9, "demasiados streams de vertices");
    }
    VkDeviceSize bytes_vertices = 0;
    for (size_t b = 0; b < entry->bindings.size(); ++b) {
      const VertexBinding& bind_index = entry->bindings[b];
      const uint32_t d0 = r[kRegFetch + bind_index.ranura * 2];
      const uint32_t d1 = r[kRegFetch + bind_index.ranura * 2 + 1];
      if ((d0 & 0x3) != uint32_t(xenos::FetchConstantType::kVertex)) {
        return Reject(10, "fetch constant de vertices invalida");
      }
      const uint64_t address = uint64_t(d0 & 0x1FFFFFFC);
      const uint64_t available = uint64_t((d1 >> 2) & 0xFFFFFF) * 4;
      const uint64_t start = uint64_t(vmin) * bind_index.stride_bytes;
      uint64_t required = uint64_t(vertices) * bind_index.stride_bytes;
      if (start + required > available) {
        Warn(11, "vertices mas alla del final de su bufer: se recorta");
        required = available > start ? (available - start) / bind_index.stride_bytes * bind_index.stride_bytes : 0;
      }
      if (!required || address + start + required > kPhysicalMemory) {
        return Reject(12, "vertices fuera de la memoria");
      }
      sources[b] = {mem_->TranslatePhysical(uint32_t(address + start)),
                     static_cast<xenos::Endian>(d1 & 0x3), uint32_t(required),
                     address + start};
      bytes_vertices += (required + 3) & ~VkDeviceSize(3);
    }

    // Diagnostic: one line per combination of VS, PS and render target (at most 32).
    if (diagnostics_ < 32) {
      const uint64_t diagnostic_key = (uint64_t(p.vs->number) << 44) ^
                                         (uint64_t(ps ? ps->number + 1 : 0) << 32) ^ keys[0] ^
                                         (without_clip ? 1 : 0);
      if (diagnosed_.insert(diagnostic_key).second) {
        ++diagnostics_;
        float position[4] = {};
        for (const VertexAttribute& a : entry->attributes) {
          if (a.loc_index != 0) {
            continue;
          }
          const VertexSource& src = sources[a.bind_index];
          if (src.bytes >= a.offset + 16) {
            for (uint32_t k = 0; k < 4; ++k) {
              uint32_t word_value;
              std::memcpy(&word_value, src.payload + a.offset + k * 4, 4);
              position[k] = Float(xenos::GpuSwap(word_value, src.orden));
            }
          }
          break;
        }
        REXLOG_INFO(
            "[nativo] C6 diag: VS n{} PS n{} tipo {} cuenta {} {} VTE {:08X} CLIP {:08X} "
            "vp x {}+-{} y {}+-{} z {}+{} ventana {:08X} sc {:08X} superficie {:08X} "
            "color0 {:08X} mascara {:08X} profundidad {:08X} edram {} vtx {:08X} "
            "pos0 ({:.3f} {:.3f} {:.3f} {:.3f}){}",
            p.vs->number, ps ? int(ps->number) : -1, type, num,
            src_select == 0 ? "indices" : "auto",
            r[gr::XE_GPU_REG_PA_CL_VTE_CNTL], r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL],
            Float(r[gr::XE_GPU_REG_PA_CL_VPORT_XOFFSET]),
            Float(r[gr::XE_GPU_REG_PA_CL_VPORT_XSCALE]),
            Float(r[gr::XE_GPU_REG_PA_CL_VPORT_YOFFSET]),
            Float(r[gr::XE_GPU_REG_PA_CL_VPORT_YSCALE]),
            Float(r[gr::XE_GPU_REG_PA_CL_VPORT_ZOFFSET]),
            Float(r[gr::XE_GPU_REG_PA_CL_VPORT_ZSCALE]), r[gr::XE_GPU_REG_PA_SC_WINDOW_OFFSET],
            sc_mode, r[gr::XE_GPU_REG_RB_SURFACE_INFO], r[gr::XE_GPU_REG_RB_COLOR_INFO],
            r[gr::XE_GPU_REG_RB_COLOR_MASK], depth_control, edram_mode,
            r[gr::XE_GPU_REG_PA_SU_VTX_CNTL], position[0], position[1], position[2], position[3],
            without_clip ? " [sin recorte]" : "");
      }
    }

    CutSubstage(18, t_indices_185);  // vertices and diagnostics
    Stage(1, mark);
    // --- Textures and samplers ----------------------------------------------------
    uint32_t shared[kSharedWords] = {};
    for (uint32_t i = 0; i < 16; ++i) {
      WriteScale(shared, i, 1.0f);
    }
    VkDeviceSize textures_bytes = 0;
    DiscardPlannedFingerprints();  // those of the previous draw that never reached SubirTextura
    textures_to_upload_.clear();
    const auto t_samplers =
        time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    /*
     * nfsmw_nativo_profundidad_perezosa. In the final composite without blur, the depth sample (HEIGHTMAP)
     * is dead: C2 is told so that requesting it does not force a copy. The same value decides
     * kSpecSinDesenfoque further down, so textures and specialization always agree.
     */
    const bool composition_without_blur =
        ps && ps->shader && ps->shader->fingerprint == kCompositionFingerprint && without_frame_blur_;
    if (composition_without_blur) {
      contexto_->DeadDepthReads(true);
    }
    bool reflection_reads = false;  // nfsmw_reflejo_visibilidad
    for (const SamplerShader& sampler :
         ps ? std::span<const SamplerShader>(ps->samplers) : std::span<const SamplerShader>()) {
      if (sampler.reg_index >= 16) {
        continue;
      }
      const uint32_t* fetch = r + kRegFetch + uint32_t(sampler.reg_index) * 6;
      // nfsmw_reflejo_visibilidad. Here and not in TexturaResuelta, which, because of the caches, only sees
      // the first draw of each frame that samples it. Without checking the dimension: over-counting only
      // means the reflection gets drawn.
      if (measure_visibility_ && (fetch[0] & 0x3) == uint32_t(xenos::FetchConstantType::kTexture) &&
          ((fetch[1] & 0xFFFFF000u) & 0x1FFFFFFFu) == nfsmw::demand_reflection::kAddress) {
        reflection_reads = true;
      }
      ++prepared_samplers_;
      bool without_cache = false;  // depth requested but not sampled, kept out of the caches
      // Per-register cache: same fetch constant, same frame and no C2 copies in between.
      CacheSampler& cache = cache_samplers_[sampler.reg_index];
      // Valid until cache.valido_hasta (with the cross-frame cache disabled, only its own frame)
      if ((cache_between_frames_ ? frame_ <= cache.valid_until : cache.frame == frame_) &&
          cache.generacion == textures_generation_ &&
          std::memcmp(cache.fetch.data(), fetch, sizeof(cache.fetch)) == 0) {
        shared[cache.heap * 16 + sampler.reg_index] = cache.ranura;
        shared[48 + sampler.reg_index] = cache.sampler;
        WriteInvSize(shared, sampler.reg_index, cache.width_px, cache.height_px);
        WriteScale(shared, sampler.reg_index, cache.scale);
        ++samplers_cache_;
        continue;
      }
      uint32_t texture_slot = 0;
      uint32_t heap = 0;
      uint32_t ranura_sampler = 0;
      uint32_t host_width = 0, host_height = 0;
      float scale = 1.0f;
      // Second cache, keyed by the whole fetch constant within the same frame and generation: a register
      // changes texture between draws, but textures repeat a lot within a frame, and PrepararTextura already
      // queued their upload the first time.
      CacheSampler& por_fetch =
          cache_fetch_[XXH3_64bits(fetch, sizeof(uint32_t) * 6) & (cache_fetch_.size() - 1)];
      uint64_t valid_until = frame_;
      if ((cache_between_frames_ ? frame_ <= por_fetch.valid_until : por_fetch.frame == frame_) &&
          por_fetch.generacion == textures_generation_ &&
          std::memcmp(por_fetch.fetch.data(), fetch, sizeof(por_fetch.fetch)) == 0) {
        texture_slot = por_fetch.ranura;
        heap = por_fetch.heap;
        ranura_sampler = por_fetch.sampler;
        valid_until = por_fetch.valid_until;
        host_width = por_fetch.width_px;
        host_height = por_fetch.height_px;
        scale = por_fetch.scale;
        ++samplers_cache_fetch_;
      } else {
        // Why the table misses ("C6 cache por fetch" report, every 10 s).
        if (std::memcmp(por_fetch.fetch.data(), fetch, sizeof(por_fetch.fetch)) != 0) {
          ++(por_fetch.frame == UINT64_MAX ? empty_failures_fetch_ : collision_failures_fetch_);
        } else if (por_fetch.generacion != textures_generation_) {
          ++generation_failures_fetch_;
        } else {
          ++expired_failures_fetch_;
        }
        bool point_sampling = false;
        PrepareTexture(fetch, texture_slot, heap, textures_bytes, point_sampling, valid_until,
                        host_width, host_height, scale);
        ranura_sampler = RanuraSampler(fetch, point_sampling);
        por_fetch.frame = frame_;
        por_fetch.generacion = textures_generation_;
        std::memcpy(por_fetch.fetch.data(), fetch, sizeof(por_fetch.fetch));
        por_fetch.ranura = texture_slot;
        por_fetch.heap = heap;
        por_fetch.sampler = ranura_sampler;
        por_fetch.valid_until = valid_until;
        por_fetch.width_px = host_width;
        por_fetch.height_px = host_height;
        por_fetch.scale = scale;
        // A depth requested without being sampled is not stored in the caches: the next draw that really
        // samples it has to go through TexturaResuelta, which is where it is recorded if it was deferred.
        if (composition_without_blur && point_sampling) {
          por_fetch.frame = 0;
          por_fetch.valid_until = 0;
          valid_until = 0;
          without_cache = true;
        }
      }
      shared[heap * 16 + sampler.reg_index] = texture_slot;
      shared[48 + sampler.reg_index] = ranura_sampler;
      WriteInvSize(shared, sampler.reg_index, host_width, host_height);
      WriteScale(shared, sampler.reg_index, scale);
      cache.frame = frame_;
      cache.generacion = textures_generation_;
      std::memcpy(cache.fetch.data(), fetch, sizeof(cache.fetch));
      cache.ranura = texture_slot;
      cache.heap = heap;
      cache.sampler = ranura_sampler;
      cache.valid_until = valid_until;
      cache.width_px = host_width;
      cache.height_px = host_height;
      cache.scale = scale;
      if (without_cache) {
        cache.frame = 0;  // with valido_hasta at 0 it is not valid across frames either
      }
    }
    if (composition_without_blur) {
      contexto_->DeadDepthReads(false);
    }
    /*
     * nfsmw_nativo_sombra_minimo. If this draw samples the shadow map texture with cars (the game's
     * textura[0], 086AE000) and C2 holds it with only the cars, its pair (the world map) goes in the 3D
     * index word of that register (the shadow map is 2D: that word is unused) and the pipeline carries
     * kSpecSombraMinimo: tfetch2DSombraMin takes the minimum of the two, the same value the copy would have
     * left. While C2 is observing, the pair is the texture itself (the same texel), so the pipelines with
     * the bit already exist when applying starts. Every read is reported to C2, which decides and watches.
     */
    bool shadow_min_draw = false;
    if (ps && !ps->samplers.empty()) {
      if (const uint32_t cars_dir = contexto_->CarShadowAddress()) {
        for (const SamplerShader& s : ps->samplers) {
          if (s.reg_index >= 16) {
            continue;
          }
          const uint32_t* f = r + kRegFetch + uint32_t(s.reg_index) * 6;
          if ((f[0] & 0x3) != uint32_t(xenos::FetchConstantType::kTexture) ||
              (((f[1] >> 12) << 12) & 0x1FFFFFFF) != cars_dir) {
            continue;
          }
          const bool capable = ps->shadow_min && s.shadows_map &&
                             ((f[5] >> 9) & 0x3) == uint32_t(xenos::DataDimension::k2DOrStacked);
          const NativeImage* shader_pair = contexto_->CarShadowCompanion(capable, ps->number);
          if (!shader_pair) {
            continue;
          }
          const uint32_t pair_slot =
              ViewSlot(shader_pair->img, shader_pair->format_id, (f[3] >> 1) & 0xFFF, kSwizzleRRRR);
          if (!pair_slot) {
            contexto_->CarShadowCompanion(false, ps->number);  // no view, no minimum: C2 records it
            continue;
          }
          shared[16 + s.reg_index] = pair_slot;
          shadow_min_draw = true;
        }
      }
    }

    if (time_) {  // C6 subetapas: the sampler loop inside the texturas stage
      sub_ns_[14] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - t_samplers)
                                  .count());
      ++sub_n_[14];
    }
    Stage(2, mark);
    // --- Upload buffer space ----------------------------------------------------------
    const VkDeviceSize bytes_indices =
        indices16 ? VkDeviceSize(indices16_.size()) * 2 : VkDeviceSize(indices_.size()) * 4;
    // With a single binding the copy goes to a multiple of the stride: up to stride - 4 bytes of padding.
    const VkDeviceSize zero_base_slot =
        zero_vertices_base_ && entry->bindings.size() == 1 ? VkDeviceSize(entry->bindings[0].stride_bytes) : 0;
    const VkDeviceSize required = zero_base_slot + bytes_vertices + bytes_indices + textures_bytes +
                                    2 * VkDeviceSize(kConstantsRegs) * 4 +
                                    kSharedWords * 4 + 64 * 8 +
                                    // With UBOs the blocks go whole and aligned to alineacion_ubo_
                                    (use_ubo_ ? kUboBytesVs + kUboBytesPs + kSharedUboBytes +
                                                     3 * ubo_alignment_
                                               : 0);
    if (required > kUploadSize) {
      return Reject(13, "dibujo mayor que el bufer de subida");
    }
    const bool full_shared =
        separately_shared_ && use_ubo_ &&
        used_shared_ + kSharedUboBytes + ubo_alignment_ > kSharedSize;
    if (used_upload_ + required > kUploadSize || full_shared) {
      const auto before_send = std::chrono::steady_clock::now();
      const bool sent = contexto_->SendAndWait();
      ++full_sends_;
      full_sends_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - before_send)
                                        .count());
      if (!sent) {
        return false;
      }
    }

    // --- Render pass ----------------------------------------------------------------
    /*
     * No XXH3 on every draw (nfsmw_nativo_clave_pase_rapida). If the 40 bytes of render targets are the
     * same ones that produced pase_clave_ (EmpezarPase stores them next to it), their XXH3 is pase_clave_.
     * Otherwise it is computed as usual: the pass change decision is the same as before in every case,
     * collisions included.
     */
    if (msaa_4x) {  // AirborneNX: a 4X pass has its own scale, so it is not the 1X pass of the same targets
      for (uint64_t& k : keys) {
        if (k) k |= uint64_t(1) << 61;  // bit 62 marks the depth key, 63 the color keys
      }
    }
    uint64_t pass_key;
    if (fast_pass_key_ && valid_keys_pass_ && std::memcmp(keys, keys_pass_, sizeof(keys)) == 0) {
      const uint64_t n = ++fast_pass_keys_;
      pass_key = (n <= kPassKeysToCheck || (n & 4095) == 0) ? CheckPassKey(keys, n) : key_pass_;
    } else {
      pass_key = XXH3_64bits(keys, sizeof(keys));
    }
    if (!active_pass_ || pass_key != key_pass_ ||
        generation_pass_ != contexto_->CommandsGeneration()) {
      const auto before_pass = std::chrono::steady_clock::now();
      if (generation_pass_ != contexto_->CommandsGeneration()) {
        ++passes_per_generation_;
      } else if (!active_pass_ && pass_key == key_pass_) {
        ++resumed_passes_;
      } else {
        ++passes_per_target_;
      }
      FinishPass();
      // The pass change is split. TerminarPase closes the previous pass; EmpezarPase finds or creates the
      // render targets (it may create images, record barriers and clear them), builds the render pass and
      // the framebuffer, and opens the pass.
      const auto after_finish = std::chrono::steady_clock::now();
      if (time_) {
        stages_ns_[8] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            after_finish - before_pass).count());
      }
      const bool started = StartPass(r, keys, pitch, pass_key);
      const auto after_pass = std::chrono::steady_clock::now();
      passes_ns_ += uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(after_pass - before_pass).count());
      // The pass change, separate from what the stage costs on each draw.
      if (time_) {
        stages_ns_[7] += uint64_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(after_pass - before_pass).count());
      }
      if (!started) {
        return false;
      }
    }
    const VkCommandBuffer cmd = commands_pass_;
    if (generation_recording_ != contexto_->CommandsGeneration()) {
      generation_recording_ = contexto_->CommandsGeneration();
      bound_pipeline_ = VK_NULL_HANDLE;
      eds_valid_ = false;  // phases 1 and 2: nothing is set in the new buffer
      valid_bound_key_ = false;  // New command buffer (ContarCambioPipeline)
      bound_sets_ = false;
      bound_ubo_ = false;
      recorded_state_ = false;
      recorded_bindings_ = 0;  // different command buffer: nothing is bound
    }

    Stage(3, mark);
    // --- Uploads: textures, vertices, indices and constants ------------------------------
    for (Texture* texture : textures_to_upload_) {
      if (!UploadTexture(*texture)) {
        return false;
      }
    }
    std::array<VkDeviceSize, 16> offsets_vertices{};
    // If the guest has waited for the GPU since the last draw, it may legally have rewritten an already
    // referenced range: what was recorded is no longer valid.
    if (active_dedupe_) {
      const uint32_t sync = g_ring_syncs.load(std::memory_order_relaxed);
      if (sync != view_sync_dedupe_) {
        view_sync_dedupe_ = sync;
        dedupe_.Forget();
      }
    }
    const auto before_vertices =
        time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    for (size_t b = 0; b < entry->bindings.size(); ++b) {
      const VertexSource& src = sources[b];
      if (diag_repeated_vertices_) {
        TrackRepeatedVertices(src.address, src.bytes, uint32_t(src.orden), src.payload);
      }
      VkDeviceSize offset;
      // If this same range was already copied in this frame, its place in the upload buffer is reused and
      // nothing is copied. See native_vertex_dedupe.h.
      if (active_dedupe_ &&
          dedupe_.Lookup(src.address, src.bytes, uint32_t(src.orden), offset)) {
        offsets_vertices[b] = offset;
        continue;
      }
      // With a single binding, at a multiple of its stride (nfsmw_nativo_vertices_base_cero).
      if (zero_vertices_base_ && entry->bindings.size() == 1) {
        ReserveMultiple(src.bytes, entry->bindings[0].stride_bytes, offset);
      } else {
        Reserve(src.bytes, 4, offset);
      }
      const CopyJob job{src.payload, data_upload_ + offset, src.bytes / 4, src.orden};
      if (!active_copies_ || !EnqueueCopy(job)) {
        CopyVertices(job);
      }
      bytes_vertices_ += src.bytes;
      offsets_vertices[b] = offset;
      if (active_dedupe_) {
        dedupe_.Track(src.address, src.bytes, uint32_t(src.orden), offset);
      }
    }
    if (time_) {
      ns_vertices_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - before_vertices)
                                   .count());
    }
    VkDeviceSize offset_indices = 0;
    if (with_indices) {
      Reserve(bytes_indices, 4, offset_indices);
      uploaded_indices_bytes_ += bytes_indices;
      std::memcpy(data_upload_ + offset_indices,
                  indices16 ? static_cast<const void*>(indices16_.data())
                                : static_cast<const void*>(indices_.data()),
                  size_t(bytes_indices));
    }
    // Only the registers each shader reads (EntradaShader::constantes_bytes). With the same generation, the
    // copy is reused if it already covers what this shader needs.
    const uint32_t bytes_vs = std::min<uint32_t>(p.vs->constantes_bytes, kConstantsRegs * 4);
    if (constantes_vs_generacion_ != p.generacion_constantes_vs ||
        epoch_vs_constants_ != upload_epoch_ || bytes_vs > constantes_vs_bytes_) {
      // Why they are uploaded again (measurement only; each upload is another set 4 offset).
      ++vs_reuploaded_[constantes_vs_generacion_ != p.generacion_constantes_vs ? 0
                      : epoch_vs_constants_ != upload_epoch_                 ? 1
                                                                              : 2];
      // With UBOs the block goes whole (the driver may read the full range) and aligned to the device minimum
      Reserve(use_ubo_ ? std::max<VkDeviceSize>(bytes_vs, kUboBytesVs) : bytes_vs, use_ubo_ ? ubo_alignment_ : 16,
               constantes_vs_offset_);
      std::memcpy(data_upload_ + constantes_vs_offset_, r + kRegConstantesVs, bytes_vs);
      constantes_vs_generacion_ = p.generacion_constantes_vs;
      epoch_vs_constants_ = upload_epoch_;
      constantes_vs_bytes_ = bytes_vs;
    }
    const uint32_t bytes_ps =
        ps ? std::min<uint32_t>(ps->constantes_bytes, kConstantsRegs * 4) : 0;
    if (ps && (constantes_ps_generacion_ != p.generacion_constantes_ps ||
               epoch_ps_constants_ != upload_epoch_ || bytes_ps > constantes_ps_bytes_)) {
      ++ps_reuploaded_[constantes_ps_generacion_ != p.generacion_constantes_ps ? 0
                      : epoch_ps_constants_ != upload_epoch_                 ? 1
                                                                              : 2];
      Reserve(use_ubo_ ? std::max<VkDeviceSize>(bytes_ps, kUboBytesPs) : bytes_ps, use_ubo_ ? ubo_alignment_ : 16,
               constantes_ps_offset_);
      std::memcpy(data_upload_ + constantes_ps_offset_, r + kRegConstantesPs, bytes_ps);
      constantes_ps_generacion_ = p.generacion_constantes_ps;
      epoch_ps_constants_ = upload_epoch_;
      constantes_ps_bytes_ = bytes_ps;
    }
    TreatedCompositionCopy(ps, r, bytes_ps);  // nfsmw_tratamiento_visual (see the function)

    Stage(4, mark);
    // --- Viewport, tijera y constantes compartidas ---------------------------------------
    /*
     * The framing (viewport, ndc and scissor), cached (nfsmw_nativo_encuadre_cache).
     *
     * The viewport, the ndc and the scissor are a function of the framing registers (PA_CL_VTE_CNTL,
     * PA_CL_VPORT_*, PA_SC_WINDOW_OFFSET/SCISSOR, PA_CL_CLIP_CNTL and PA_SU_SC_MODE_CNTL:
     * EsRegistroDeEncuadre in nfsmw_nativo_sistema.cpp, which bumps generacion_encuadre on every path that
     * writes them) and of the pass size and scale (pase_serie_). In a race the framing really changes
     * 0.07-0.14 times per draw ("C6 generaciones"). The depth bias is not included: it depends on
     * PA_SU_POLY_OFFSET_*, RB_DEPTH_INFO and the primitive type, which do not bump that generation.
     */
    const uint32_t vte = r[gr::XE_GPU_REG_PA_CL_VTE_CNTL];
    VkViewport viewport{};
    float ndc[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    VkRect2D scissor{};
    uint32_t framing_empty = 0;
    const bool framing_in_cache = framing_cache_ && framing_valid_ &&
                                   p.framing_generation == generation_framing_ && series_pass_ == framing_series_pass_;
    bool check_framing = false;
    if (framing_in_cache) {
      const uint64_t n = ++framing_hits_;
      check_framing = n <= kFramingsToCheck || (n & 4095) == 0;
      ++framing_hits_report_;
    }
    if (framing_in_cache && !check_framing) {
      viewport = framing_viewport_;
      std::memcpy(ndc, framing_ndc_, sizeof(ndc));
      scissor = framing_scissor_;
      framing_empty = framing_empty_;
    } else {
      framing_empty = ComputeFraming(r, vte, without_clip, sc_mode, viewport, ndc, scissor);
      ++framing_calculations_report_;
      if (check_framing) {
        CompareFraming(viewport, ndc, scissor, framing_empty);  // with the stored values, before overwriting them
      }
      framing_viewport_ = viewport;
      std::memcpy(framing_ndc_, ndc, sizeof(ndc));
      framing_scissor_ = scissor;
      framing_empty_ = framing_empty;
      generation_framing_ = p.framing_generation;
      framing_series_pass_ = series_pass_;
      framing_valid_ = true;
    }
    if (framing_empty != 0) {
      return true;  // empty viewport or scissor: as before
    }

    uint32_t specialization = entry->specialization;
    if (rect_list) {
      specialization |= kSpecRectList;
    }
    if (composition_without_blur) {  // the same value that decided the textures
      specialization |= kSpecWithoutBlur;
    }
#ifndef NFSMW_NO_GLOW_VARIANTS
    if (ps && ps->shader && ps->shader->fingerprint == kBrightPassFingerprint) {
      const int mode = GlowSky();
      specialization |= mode == 1 ? kSpecGlowNatural : mode == 2 ? kSpecGlowSmooth : 0;
    }
#endif
    const uint32_t control_color = r[gr::XE_GPU_REG_RB_COLORCONTROL];
    float alpha_threshold = 0.0f;
    uint32_t alpha_function = 7;  // always
    // The 8 Xenos functions (0 never, 1 <, 2 ==, 3 <=, 4 >, 5 !=, 6 >=, 7 always) with alphaTestValue
    // (nfsmw_validado_normales library). Without a PS (mode 5) there is no test.
    if (ps && ((control_color >> 3) & 0x1) && (control_color & 0x7) != 7) {
      alpha_threshold = Float(r[gr::XE_GPU_REG_RB_ALPHA_REF]);
      alpha_function = control_color & 0x7;
      specialization |= 0x2;
      // The function goes in the pipeline, not in the constants. RB_COLORCONTROL was already part of the key,
      // so this creates no pipelines that did not already exist.
      specialization |= (alpha_function & 0x7u) << kSpecOffsetAlphaFunction;
    }
    // AirborneNX: 7e3 render targets are RGBA16F on the host. The PS applies the 7e3 range to the outputs
    // that go to them (salida7e3 in shader_common.h: RGB in [0, 31.875], alpha in 2 bits), or the 16-bit
    // alpha keeps values the Xbox 360 would clamp (UE3 writes the scene depth there, ~3500).
    if (ps && REXCVAR_GET(airborne_salida_7e3)) {
      static constexpr uint32_t kInfoColor7e3[4] = {gr::XE_GPU_REG_RB_COLOR_INFO, gr::XE_GPU_REG_RB_COLOR1_INFO,
                                                    gr::XE_GPU_REG_RB_COLOR2_INFO, gr::XE_GPU_REG_RB_COLOR3_INFO};
      static constexpr uint32_t kSpecE3Output7[4] = {uint32_t(1) << 5, uint32_t(1) << 6, uint32_t(1) << 7,
                                                     uint32_t(1) << 10};
      const uint32_t color_mask = r[gr::XE_GPU_REG_RB_COLOR_MASK];
      for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t format_id = (r[kInfoColor7e3[i]] >> 16) & 0xF;
        // Only the real 7e3 format: k_2_10_10_10_FLOAT_AS_16_16_16_16 keeps 16 bits per channel in EDRAM (the
        // emulation shows UE3's scene depth, ~8500, surviving in its alpha).
        if (((color_mask >> (i * 4)) & 0xF) &&
            format_id == uint32_t(xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT)) {
          specialization |= kSpecE3Output7[i];
        }
      }
    }
    shared[64] = (r[kRegBools] & 0xFFFF) | ((r[kRegBools + 4] & 0xFFFF) << 16);
    shared[65] = 0;  // g_SwappedTexcoords
    if (!(r[gr::XE_GPU_REG_PA_SU_VTX_CNTL] & 0x1)) {  // PixelCenter::kD3DZero
      const float medio[2] = {1.0f / viewport.width, -1.0f / std::abs(viewport.height)};
      std::memcpy(&shared[66], medio, sizeof(medio));
    }
    std::memcpy(&shared[68], &alpha_threshold, sizeof(alpha_threshold));
    shared[69] = alpha_function;  // g_AlphaFunction
    std::memcpy(&shared[70], ndc, sizeof(ndc));
    std::copy(entry->remaps.begin(), entry->remaps.end(), shared + 74);
    VkDeviceSize shared_offset;
    /*
     * How many draws really change the shared constants.
     *
     * This block is 488 bytes that are zeroed, filled, compared and, if they changed, copied twice, once
     * into memory without CPU caching (2654 MB/s). With ~2,400 draws per frame that is ~4.7 MB of traffic
     * per frame on the thread that is already at 96 % of a core.
     *
     * Which fix applies depends on the count: if almost no draw changes the block, the comparison is the
     * waste; if almost all do, the double memcpy is. They are two different fixes, and counting costs one
     * increment.
     */
    ++peeked_shared_;
    const auto t_shared =
        time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (epoch_shared_ == upload_epoch_ &&
        std::memcmp(shared, shared_previous_, sizeof(shared)) == 0) {
      shared_offset = offset_shared_;  // the same as the previous draw
    } else {
      ++changed_shared_;
      if (separately_shared_ && use_ubo_) {
        // To the separate CPU-cached buffer; binding 2 of set 4 points to it.
        shared_offset = (used_shared_ + ubo_alignment_ - 1) & ~(ubo_alignment_ - 1);
        used_shared_ = shared_offset + kSharedUboBytes;
        std::memcpy(data_shared_ + shared_offset, shared, sizeof(shared));
      } else {
        Reserve(use_ubo_ ? kSharedUboBytes : kSharedWords * 4, use_ubo_ ? ubo_alignment_ : 16,
                 shared_offset);
        std::memcpy(data_upload_ + shared_offset, shared, sizeof(shared));
      }
      std::memcpy(shared_previous_, shared, sizeof(shared));
      offset_shared_ = shared_offset;
      epoch_shared_ = upload_epoch_;
    }
    if (time_) {
      sub_ns_[12] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - t_shared)
                                  .count());
      ++sub_n_[12];
    }

    // --- Pipeline ---------------------------------------------------------------------------
    PipelineKey key;
    key.vs = p.vs->number + 1;
    key.ps = ps ? ps->number + 1 : 0;  // 0: mode 5, no fragment stage
    key.entry = entry->fingerprint;
    key.topo = uint32_t(topo);
    if (use_ubo_) {
      specialization |= kSpecConstantesUbo;  // the shaders read from set 4
    }
    if (tex_size_inv_) {
      specialization |= kSpecTexSizeInv;
    }
    if (cheap_pcf_) {
      specialization |= kSpecCheapPcf;
    }
    if (only_alpha && ps) {
      // If vegetation is excluded from the shadow map, this whole draw is unnecessary.
      // AirborneNX: only in the shadow map and not for shaders that write depth (see vegetation_early).
      if (without_vegetation_ && pitch >= 1600 && !(ps->outputs & 0x10)) {
        return true;
      }
      specialization |= kSpecOnlyAlpha;
    }
    /*
     * Test depth before shading where it costs nothing.
     *
     * A pixel shader that can discard (alpha test or its own kill) forces the hardware to shade first and
     * test Z afterwards: otherwise a discarded fragment would already have written its depth. But that only
     * matters if the draw writes depth. If it only tests it (smoke, particles, glass, decals, lights),
     * testing earlier is exact: there is no write to move, the shader still discards the color, and the GPU
     * stops shading what is hidden.
     *
     * The counters are kept even with the setting off, so the report says the same in both halves of an A/B
     * test, and the "NO se pueden porque escriben profundidad" line measures exactly what a depth pre-pass
     * would have to fix.
     */
    if (ps && keys[4] && masks) {
      const bool test_z = ((depth_control >> 1) & 0x1) != 0;
      const bool write_z = test_z && ((depth_control >> 2) & 0x1) != 0;
      const bool stencil_state = (depth_control & 0x1) != 0;
      const bool write_z_ps = (ps->outputs & 0x10) != 0;
      const bool late_one = (specialization & 0x2) || ps->discards || write_z_ps;
      if (!test_z) {
        // Without a depth test there is nothing to move earlier.
      } else if (!late_one) {
        ++counts_z_[kZEarlyAlready];
      } else if (write_z || write_z_ps) {
        ++counts_z_[kZWriteZ];
      } else if (stencil_state) {
        ++counts_z_[kZStencil];
      } else if (open_occlusion_) {
        ++counts_z_[kZOclusion];  // the game reads those samples: leave it alone
      } else if (ps->shader && ps->shader->fingerprint != kBrightPassFingerprint &&
                 ps->shader->fingerprint != kCompositionFingerprint) {
        ++counts_z_[kZSet];
        if (z_early_) {
          specialization |= kSpecZEarly;
        }
      }
    }
    // nfsmw_nativo_sombra_minimo. The shadow map pair bit (see above, textures).
    if (shadow_min_draw) {
      specialization |= kSpecShadowMin;
    }
    /*
     * nfsmw_nativo_sombra_minimo. In the car pass of the shadow map, C2 needs to know whether this draw
     * leaves in the depth buffer exactly the minimum of what was there and of its fragments: no stencil, no
     * occlusion query and, if it writes Z, a NEVER, LESS or LEQUAL test. The game's register
     * (RB_DEPTHCONTROL) is checked, not the pipeline key. EmpezarPase checks that the pass is depth-only.
     */
    if (shadow_cars_pass_) {
      const uint32_t cars_control_z = keys[4] ? depth_control : 0;
      const bool cars_write_z = ((cars_control_z >> 1) & 0x1) && ((cars_control_z >> 2) & 0x1);
      const uint32_t cars_function_z = (cars_control_z >> 4) & 0x7;
      contexto_->CarShadowDraw(!(cars_control_z & 0x1) && !open_occlusion_ &&
                                          (!cars_write_z || cars_function_z == 0 || cars_function_z == 1 ||
                                           cars_function_z == 3),
                                      cars_control_z);
    }
    key.specialization = specialization;
    std::copy(std::begin(formats_pass_), std::end(formats_pass_), std::begin(key.formats));
    static constexpr uint32_t kBlend[4] = {
        gr::XE_GPU_REG_RB_BLENDCONTROL0, gr::XE_GPU_REG_RB_BLENDCONTROL1,
        gr::XE_GPU_REG_RB_BLENDCONTROL2, gr::XE_GPU_REG_RB_BLENDCONTROL3};
    for (uint32_t i = 0; i < 4; ++i) {
      if (keys[i]) {
        key.blend[i] = r[kBlend[i]] & 0x1FFF1FFF;
      }
    }
    key.masks = masks;
    key.depth_value = keys[4] ? depth_control : 0;
    if (key.depth_value && open_occlusion_ && pitch >= 640 && !masks &&
        REXCVAR_GET(nfsmw_nativo_prueba_oclusion_siempre)) {
      key.depth_value |= 0x7 << 4;  // test: Z function always
    }
    key.rasterizado = (sc_mode & 0x7) | (restart_flag ? 0x8 : 0) | (with_bias ? 0x10 : 0);
    // AirborneNX: rectangles are not polygons for face culling (the emulation's IsPrimitivePolygonal): the
    // depth restore comes with back-face culling on and would be dropped.
    if (rect_list && edram_4x_alias_) {  // together with the 4X aliasing (they need each other)
      key.rasterizado &= ~uint32_t(0x3);
    }
    VkPipeline pipeline = VK_NULL_HANDLE;
    // Looked up with ClaveDeBusqueda (phase 0a: canonical form; phases 1 and 2: without the state set through
    // vkCmdSet*). clave stays raw: the deferred sky (opaco_en_todos), the counter and the dynamic state read
    // it.
    NFSMW_SUB(11, pipeline = PipelineDe(SearchKey(key), *entry, p));
    if (pipeline == VK_NULL_HANDLE) {
      return false;
    }
#if REX_PLATFORM_SWITCH
    // The pipeline about to be bound, prefetched into the cache several us before its vkCmdBindPipeline
    // (Mesa patch p06; PRFM only: it reads and writes nothing and cannot fail).
    if (nvk_app_preload_ && pipeline != bound_pipeline_ && vk_switch_precargar_pipeline) {
      vk_switch_precargar_pipeline(pipeline);
    }
#endif

    Stage(5, mark);
    /*
     * The three dynamic state values that used to be computed inside the recording block are computed here,
     * without recording anything. They are pure computations on the draw's registers; they are needed
     * earlier so they can be saved if this draw is the sky and gets deferred (see below).
     */
    // Scissor test. All the state and the draw are sent as usual; the only change is that there are no
    // pixels to shade. It breaks the image: only for measuring the per-draw floor.
    VkRect2D final_scissor = scissor;
    if (const uint32_t scissor_category = test_scissor_; scissor_category != 0) {
      const uint32_t category = TargetCategory(pitch, keys);
      static constexpr uint32_t kCategoryOf[5] = {0, kGpuShadows, kGpuScene, kGpu320,
                                                   kGpuSceneWithoutDepth};
      if (scissor_category <= 4 && category == kCategoryOf[scissor_category]) {
        final_scissor.extent = {1, 1};
      }
    }
    /*
     * Record how far down the game draws in this render target. It only grows, so the pass is never opened
     * smaller than what has already been seen drawn.
     */
    if (usable_area_) {
      const uint32_t hasta = uint32_t(std::max(0, scissor.offset.y + int32_t(scissor.extent.height)));
      const uint32_t redondeado = (hasta + 63u) & ~63u;
      uint32_t& pointed_at = UsableHeightOf(pitch);  // without a map lookup on every draw
      if (redondeado > pointed_at) {
        pointed_at = redondeado;
      }
    }
    const float constant_blend[4] = {Float(r[gr::XE_GPU_REG_RB_BLEND_RED]),
                                       Float(r[gr::XE_GPU_REG_RB_BLEND_GREEN]),
                                       Float(r[gr::XE_GPU_REG_RB_BLEND_BLUE]),
                                       Float(r[gr::XE_GPU_REG_RB_BLEND_ALPHA])};
    const uint32_t front_stencil = r[gr::XE_GPU_REG_RB_STENCILREFMASK];
    const uint32_t back_stencil = ((depth_control >> 7) & 0x1)
                                       ? r[gr::XE_GPU_REG_RB_STENCILREFMASK_BF]
                                       : front_stencil;
    /*
     * The sky, deferred until after the opaque draws (nfsmw_nativo_cielo_aplazado).
     *
     * How it is recognized, and why it cannot be mistaken for another draw. Three conditions at once, none of
     * them a magic position or draw count:
     *
     *  1. The pixel shader is the sky's, by the fingerprint of its original container (kHuellaCielo). It is
     *     the only one of the library's 152 shaders whose constant table names CloudIntensity and
     *     SkyAlphaTag. The fingerprint does not depend on the SPIR-V translation or on the library order.
     *  2. The draw writes color and is opaque on all its targets: blending is "1 x source + 0 x destination"
     *     (ADD) on the channels the mask writes, exactly the criterion PipelineDe uses to decide
     *     blendEnable. Opaque = it does not read the existing color, so whatever was painted before it does
     *     not matter.
     *  3. It tests depth and does not write it, and there is no stencil. Without a Z write, deferring it
     *     cannot change what later draws see; with the test on, in its new place the Z test discards it
     *     where the world used to overwrite it.
     *
     * If any of the three fails (another version of the game, another state) the draw takes the usual path
     * and nothing happens.
     *
     * It is not deferred while a game occlusion query is open: the game reads those samples and moving it
     * would change the count (the same guard the early Z uses).
     */
    // A draw with no color to write does not read the target: it does not count as transparent.
    bool opaque_in_all = true;
    for (uint32_t i = 0; i < 4 && opaque_in_all; ++i) {
      const uint32_t mask_i = (masks >> (i * 4)) & 0xF;
      if (!mask_i) {
        continue;
      }
      const uint32_t m = key.blend[i];
      const bool direct_color = (m & 0x1F) == 1 && ((m >> 8) & 0x1F) == 0 && ((m >> 5) & 0x7) == 0;
      const bool direct_alpha =
          ((m >> 16) & 0x1F) == 1 && ((m >> 24) & 0x1F) == 0 && ((m >> 21) & 0x7) == 0;
      const bool blend_enabled =
          (((mask_i & 0x7) != 0) && !direct_color) || (((mask_i & 0x8) != 0) && !direct_alpha);
      if (blend_enabled) {
        opaque_in_all = false;
      }
    }
    const bool draw_test_z = ((depth_control >> 1) & 0x1) != 0;
    const bool draw_write_z = draw_test_z && ((depth_control >> 2) & 0x1) != 0 &&
                                  keys[4] != 0;
    bool is_sky = false;
    if (ps && ps->shader && ps->shader->fingerprint == kSkyFingerprint && masks) {
      ++sky_seen_;
      const bool stencil_state = (depth_control & 0x1) != 0;
      /*
       * Scene pass only. The same dome is also drawn in the car's cubemap and in the reflection, and there it
       * is not touched: the rear-view mirror works and is left alone, and the measured saving (4.8 of the
       * scene's 19.2 ms) is the scene's. This line is what keeps the cubemap out of it.
       */
      const bool in_scene = TargetCategory(pitch, keys) == kGpuScene;
      /*
       * The fingerprint is not enough, and that broke an earlier version.
       *
       * The fingerprint identifies the shader, not the draw. The counter said so: "7,00 detectados por
       * fotograma" when the analysis expected 0.9. Seven draws use that pixel shader, and deferring one broke
       * the order of the other six: flickering and odd colors.
       *
       * Now it must also be the dome:
       *   - the index count measured by the analysis of the SPIR-V and the log: 480 = 160 triangles;
       *   - and it must be the first draw of the pass (the dome is painted on the empty Z-buffer, which is
       *     exactly why it shades the whole screen and is then covered).
       *
       * Both are cheap to check and both belong to the dome, not to the shader. If the counter still said
       * more than one per frame, nothing is deferred: there is a guard below.
       */
      const uint32_t draw_indices =
          uint32_t(indices16 ? indices16_.size() : indices_.size());
      const bool dome_geometry = draw_indices == kSkyDomeIndices;
      const bool pass_first = draws_in_pass_ == 0;
      /* The full criterion, without the cvar or the guard: what is measured during the test. */
      const bool is_dome = in_scene && opaque_in_all && draw_test_z &&
                              !draw_write_z && !stencil_state && !(ps->outputs & 0x10) &&
                              !open_occlusion_ && dome_geometry && pass_first;
      if (is_dome) {
        ++sky_candidates_;  // the ones matching the dome's marks: this must be 1
        /*
         * The guard counts here, whether it defers or not. Two in the same frame is exactly what broke the
         * image before, so as soon as it happens it switches off and stays off for the whole session.
         */
        if (++sky_guard_in_frame_ > 1 && sky_guard_ != kSkyDiscarded) {
          sky_guard_ = kSkyDiscarded;
          REXLOG_WARN("[nativo] C6 cielo: la guardia ve {} domos en el MISMO fotograma; no se aplaza "
                      "nada en toda la sesion (es el fallo de la 137). La imagen queda intacta",
                      sky_guard_in_frame_);
        }
      }
      is_sky = sky_deferred_ && is_dome && sky_guard_ == kSkyDeferring;
      if (!is_sky && sky_deferred_) {
        ++sky_no_deferrable_;  // only with the setting on: measures criterion failures, not the cvar
      }
    }
    /*
     * Which draws the sky can be moved past, and why that does not change a single pixel.
     *
     * The sky can only skip past a draw D if both hold:
     *
     *  - D is opaque: it does not read the existing color, so wherever D paints, the final color is its own
     *    whether or not the sky was underneath. A draw with blending does read the background: the sky goes
     *    first.
     *  - D writes depth: after D the Z-buffer holds D's z, which is closer than the dome, so the sky's LEQUAL
     *    test discards it exactly where D painted. If D did not write Z, the Z-buffer would stay as it was
     *    and the sky would be painted over D: that would change the image.
     *
     * So the sky is emitted as soon as the first draw that fails either condition arrives. In practice that
     * is the first transparent draw, because what follows the sky is the opaque world; the counter separates
     * the two reasons so this can be checked in the log.
     */
    if (sky_pending_) {
      if (is_sky) {
        ++sky_two_in_pass_;  // another sky in the same pass: emit the earlier one now, keeping the order
        EmitDeferredSky(cmd, kSkyPerOtherSky);
      } else if (!(opaque_in_all && draw_write_z)) {
        EmitDeferredSky(cmd, opaque_in_all ? kSkyPerWithoutZ : kSkyPerBlend);
      }
    }
    // --- Grabar ---------------------------------------------------------------------------------
    if (is_sky && reflection_reads) {
      nfsmw::demand_reflection::TrackVisible(false);  // the deferred sky is not measured
    }
    if (is_sky) {
      // The arguments of the vkCmd* calls this draw would have emitted are saved; nothing is recorded. The
      // upload buffer is an allocator that only moves forward (Reservar) and is not reset until UsarRanura,
      // which always comes after AntesDeEnviar -> TerminarPase: when the sky is emitted, its offsets still
      // point to the same data.
      DeferredSky& c = sky_;
      c.pipeline = pipeline;
      c.buf = upload_;
      c.usa_ubo = use_ubo_;
      c.push[0] = address_upload_ + constantes_vs_offset_;
      c.push[1] = address_upload_ + constantes_ps_offset_;
      c.push[2] = address_upload_ + shared_offset;
      c.offsets_ubo = use_ubo_ ? std::array<uint32_t, 3>{uint32_t(constantes_vs_offset_),
                                                          uint32_t(constantes_ps_offset_),
                                                          uint32_t(shared_offset)}
                                : std::array<uint32_t, 3>{0, 0, 0};
      c.ranura_ubo = current_slot_;
      c.viewport = viewport;
      c.scissor = final_scissor;
      std::memcpy(c.blend, constant_blend, sizeof(c.blend));
      std::memcpy(c.bias, bias, sizeof(c.bias));
      c.with_stencil = keys[4] != 0;
      c.stencil[0] = front_stencil;
      c.stencil[1] = back_stencil;
      c.n_bindings = uint32_t(entry->bindings.size());
      c.offsets_vertices = offsets_vertices;
      c.with_indices = with_indices;
      c.indices16 = indices16;
      c.indices = uint32_t(indices16 ? indices16_.size() : indices_.size());
      c.first_index = uint32_t(offset_indices / (indices16 ? 2 : 4));
      c.vmin = vmin;
      c.num = num;
      c.ps_mas_uno = ps ? ps->number + 1 : 0;
      c.category = TargetCategory(pitch, keys);
      c.draws_on_defer = draws_in_pass_;
      c.eds_mode = eds_mode_;  // phases 1 and 2: its pipeline lacks this state, so save it all
      if (eds_mode_) {
        EdsStateFor(key, c.eds, eds_mode_);
      }
      sky_pending_ = true;
      ++sky_deferred_count_;
      // Counted as if it had been recorded: it will be recorded before the pass closes.
      ++draws_per_category_[c.category];
      triangles_per_category_[c.category] += (with_indices ? c.indices : num) / 3;
      Stage(6, mark);
      ++drawn_;
      // dibujos_en_pase_ is not incremented here: this draw is not recorded yet. It is incremented in
      // EmitirCieloAplazado, and the difference with dibujos_al_aplazar gives the draws that went ahead of
      // it, which is exactly the geometry that now covers it.
      timed_drawn_ += time_ ? 1 : 0;
      return true;
    }
    if (pipeline != bound_pipeline_) {
      if (count_pipeline_changes_) {  // Measurement only (nfsmw_nativo_contar_cambios_pipeline)
        CountPipelineChange(key, bound_pipeline_ == VK_NULL_HANDLE);
      }
      NFSMW_SUB(0, dfn_.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline));
      bound_pipeline_ = pipeline;
    }
    // Phases 1 and 2. The state that no longer goes in the pipeline, before the draw. If the guard sees a
    // difference, dynamic state switches off and this same draw is bound with its usual pipeline.
    if (eds_mode_ && !FixDynamicState(cmd, key)) {
      NFSMW_SUB(11, pipeline = PipelineDe(SearchKey(key), *entry, p));
      if (pipeline == VK_NULL_HANDLE) {
        return false;
      }
      NFSMW_SUB(0, dfn_.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline));
      bound_pipeline_ = pipeline;
      valid_bound_key_ = false;  // the counter does not classify this bind
    }
    if (!bound_sets_) {
      NFSMW_SUB(1, dfn_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_pipeline_, 0, 4,
                                                sets_.data(), 0, nullptr));
      bound_sets_ = true;
    }
    // Dynamic state and push constants repeat a lot between consecutive draws: they are only recorded if
    // they change (recording: 0.4 us per draw in a race).
    const bool recorded = recorded_state_;
    /*
     * With constants through UBOs, the push constants are dead.
     *
     * Every access in shader_common.h is `NFSMW_UBO ? conjunto 4 : g_PushConstants...`, and NFSMW_UBO is
     * specialization constant 1<<8, fixed when the pipeline is created: DXC folds the ternary and the shader
     * does not even reference g_PushConstants. Recording them meant recording something nobody reads.
     *
     * And it is not free: on Maxwell B, NVK has no hardware root table (nvk_use_hw_root_table requires
     * Turing), so each vkCmdPushConstants is a LOAD_CONSTANT_BUFFER_OFFSET plus the array, emitted on the
     * spot. The UBO is active in 100 % of the measured submissions.
     */
    if (!use_ubo_) {
      const uint64_t push[3] = {address_upload_ + constantes_vs_offset_,
                                address_upload_ + constantes_ps_offset_,
                                address_upload_ + shared_offset};
      if (!recorded || std::memcmp(push, recorded_push_, sizeof(push)) != 0) {
        dfn_.vkCmdPushConstants(cmd, layout_pipeline_,
                                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                sizeof(push), push);
        std::memcpy(recorded_push_, push, sizeof(push));
      }
    }
    // Set 4 (dynamic UBOs). Always bound, because the new shaders use it statically: with the real offsets
    // if nfsmw_nativo_constantes_ubo is set, and 0 otherwise. Only recorded when something changes.
    {
      const std::array<uint32_t, 3> offsets_ubo =
          use_ubo_ ? std::array<uint32_t, 3>{uint32_t(constantes_vs_offset_), uint32_t(constantes_ps_offset_),
                                              uint32_t(shared_offset)}
                    : std::array<uint32_t, 3>{0, 0, 0};
      ++set4_draws_;  // C6 set 4 report
      if (!bound_ubo_ || bound_ubo_slot_ != current_slot_ || offsets_ubo != bound_offsets_ubo_) {
        // Which offsets change at each bind (measurement only). What NVK saves by differences depends on it:
        // every cbuf whose offset does not change is one rebind less in the Draw.
        if (!bound_ubo_ || bound_ubo_slot_ != current_slot_) {
          ++set4_first_;
        } else {
          ++set4_changes_[(offsets_ubo[0] != bound_offsets_ubo_[0] ? 1u : 0u) |
                          (offsets_ubo[1] != bound_offsets_ubo_[1] ? 2u : 0u) |
                          (offsets_ubo[2] != bound_offsets_ubo_[2] ? 4u : 0u)];
        }
        NFSMW_SUB(2, dfn_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_pipeline_, 4, 1,
                                                  &sets_ubo_[current_slot_], 3, offsets_ubo.data()));
        bound_offsets_ubo_ = offsets_ubo;
        bound_ubo_slot_ = current_slot_;
        bound_ubo_ = true;
      }
    }
    if (!recorded || std::memcmp(&viewport, &recorded_viewport_, sizeof(viewport)) != 0) {
      NFSMW_SUB(3, dfn_.vkCmdSetViewport(cmd, 0, 1, &viewport));
      recorded_viewport_ = viewport;
    }
    if (!recorded || std::memcmp(&final_scissor, &recorded_scissor_, sizeof(final_scissor)) != 0) {
      NFSMW_SUB(4, dfn_.vkCmdSetScissor(cmd, 0, 1, &final_scissor));
      recorded_scissor_ = final_scissor;
    }
    if (!recorded ||
        std::memcmp(constant_blend, recorded_blend_, sizeof(constant_blend)) != 0) {
      NFSMW_SUB(5, dfn_.vkCmdSetBlendConstants(cmd, constant_blend));
      std::memcpy(recorded_blend_, constant_blend, sizeof(constant_blend));
    }
    // Also without bias: the state is dynamic in every pipeline and has to be recorded.
    if (!recorded || std::memcmp(bias, recorded_bias_, sizeof(bias)) != 0) {
      NFSMW_SUB(6, dfn_.vkCmdSetDepthBias(cmd, bias[0], 0.0f, bias[1]));
      std::memcpy(recorded_bias_, bias, sizeof(bias));
    }
    if (!recorded) {
      valid_recorded_stencil_ = false;
      recorded_indices_type_ = VK_INDEX_TYPE_MAX_ENUM;
    }
    if (keys[4]) {
      const uint32_t front_face = front_stencil;  // computed before the recording block
      const uint32_t back_face = back_stencil;
      if (!valid_recorded_stencil_ || front_face != recorded_stencil_[0] ||
          back_face != recorded_stencil_[1]) {
        NFSMW_SUB(7, {
          dfn_.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, front_face & 0xFF);
          dfn_.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, back_face & 0xFF);
          dfn_.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, (front_face >> 8) & 0xFF);
          dfn_.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, (back_face >> 8) & 0xFF);
          dfn_.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, (front_face >> 16) & 0xFF);
          dfn_.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, (back_face >> 16) & 0xFF);
        });
        recorded_stencil_[0] = front_face;
        recorded_stencil_[1] = back_face;
        valid_recorded_stencil_ = true;
      }
    }
    recorded_state_ = true;
    /*
     * nfsmw_nativo_vertices_base_cero. With a single binding whose copy starts at a multiple of its stride,
     * the upload buffer is bound at 0 (once per command buffer, or after the deferred sky) and the draw is
     * shifted with primer_vertice. The others bind as usual.
     */
    bool zero_base = false;
    uint32_t first_vertex = 0;  // offset / stride: where the copy starts, in vertices
    if (zero_vertices_base_ && entry->bindings.size() == 1) {
      const VkDeviceSize stride_bytes = entry->bindings[0].stride_bytes;
      if (offsets_vertices[0] % stride_bytes == 0) {
        zero_base = CheckZeroBase(offsets_vertices[0], stride_bytes, sources[0].bytes, vmin, first_vertex);
      } else {
        ++misaligned_zero_base_;
      }
    } else if (entry->bindings.size() > 1) {
      ++bindings_several_zero_base_;
    }
    if (!entry->bindings.empty()) {
      // 16 identical pointers were filled in on every draw to bind one or two. It is filled when the upload
      // buffer changes (once per work slot), not 2,345 times per frame.
      if (vertices_buffers_[0] != upload_) {
        vertices_buffers_.fill(upload_);
        recorded_bindings_ = 0;  // the buffer changed: earlier bindings are stale
      }
      /*
       * And do not bind again if it is exactly what is already bound.
       *
       * In NVK each binding is 5 dwords plus an invocation of the MME macro NVK_MME_BIND_VB
       * (nvk_cmd_draw.c:4658), with no redundancy check inside: ~2,800 MME macros per frame. With vertex
       * deduplication 28-33 % of the bindings reuse the same offset, so two consecutive draws of the same
       * mesh give exactly the same offsets.
       */
      // On the base-zero path what gets bound is the whole buffer, from 0.
      static constexpr VkDeviceSize kBindingInZero = 0;
      const VkDeviceSize* to_bind = zero_base ? &kBindingInZero : offsets_vertices.data();
      const uint32_t n_bindings = uint32_t(entry->bindings.size());
      bool is_equal = n_bindings == recorded_bindings_;
      for (uint32_t i = 0; is_equal && i < n_bindings; ++i) {
        is_equal = to_bind[i] == recorded_offsets_[i];
      }
      if (!is_equal) {
        NFSMW_SUB(8, dfn_.vkCmdBindVertexBuffers(cmd, 0, n_bindings, vertices_buffers_.data(), to_bind));
        ++recorded_bindings_zero_base_;
        recorded_bindings_ = n_bindings;
        for (uint32_t i = 0; i < n_bindings; ++i) {
          recorded_offsets_[i] = to_bind[i];
        }
      } else {
        ++saved_bindings_;
      }
      ++zero_base_total_;
      draws_zero_base_ += zero_base ? 1 : 0;
    }
    // With a game occlusion query open, this draw counts toward it.
    if (open_occlusion_ && occlusion_query_ == UINT32_MAX) {
      NFSMW_SUB(13, occlusion_query_ = contexto_->StartOcclusionQuery());
    }
    // The first 12, then one every 2 s (up to 150), to follow the sun over the course of a race.
    if (open_occlusion_ && pitch >= 640 && draw_occlusion_warnings_ < 16 &&
        (draw_occlusion_warnings_ < 12 ||
         std::chrono::steady_clock::now() - last_draw_occlusion_warning_ >= std::chrono::seconds(2))) {
      ++draw_occlusion_warnings_;
      last_draw_occlusion_warning_ = std::chrono::steady_clock::now();
      REXLOG_INFO("[nativo] C2 oclusion dibujo {}: consulta del host {}: VS n{} PS n{} tipo {} cuenta {} pitch {} "
                  "mascaras {:08X} profundidad {:08X} (escribe {}, prueba {}, funcion z {}) viewport {:.1f},{:.1f} "
                  "{:.1f}x{:.1f} z {:.3f}-{:.3f} tijera {},{} {}x{} con indices {}",
                  draw_occlusion_warnings_, occlusion_query_ == UINT32_MAX ? -1 : int64_t(occlusion_query_),
                  p.vs->number, ps ? int(ps->number) : -1, type, num, pitch, masks, depth_control,
                  (depth_control >> 2) & 0x1, (depth_control >> 1) & 0x1, (depth_control >> 4) & 0x7,
                  viewport.x, viewport.y, viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth,
                  scissor.offset.x, scissor.offset.y, scissor.extent.width, scissor.extent.height, with_indices);
      // Diagnostic: positions of the first vertices and VS constants c0-c6, to compute the sun's z against
      // the scene depth.
      std::string positions;
      for (const VertexAttribute& a : entry->attributes) {
        if (a.loc_index != 0) {
          continue;
        }
        const VertexSource& src = sources[a.bind_index];
        const uint32_t stride_bytes = entry->bindings[a.bind_index].stride_bytes;
        for (uint32_t v = 0; v < std::min<uint32_t>(num, 4); ++v) {
          const uint64_t from = uint64_t(v) * stride_bytes + a.offset;
          if (from + 16 > src.bytes) {
            break;
          }
          positions += " (";
          for (uint32_t k = 0; k < 4; ++k) {
            uint32_t word_value;
            std::memcpy(&word_value, src.payload + from + k * 4, 4);
            positions += fmt::format("{}{:.7g}", k ? "," : "", Float(xenos::GpuSwap(word_value, src.orden)));
          }
          positions += ")";
        }
        positions += fmt::format(" formato {} zancada {}", int(a.format_id), stride_bytes);
        break;
      }
      std::string constantes;
      for (uint32_t k = 0; k < 7; ++k) {
        constantes += fmt::format(" c{}=(", k);
        for (uint32_t c = 0; c < 4; ++c) {
          constantes += fmt::format("{}{:.7g}", c ? "," : "", Float(r[kRegConstantesVs + k * 4 + c]));
        }
        constantes += ")";
      }
      REXLOG_INFO("[nativo] C2 oclusion dibujo {} (geometria): VTE {:08X} CLIP {:08X} sc {:08X} RB_DEPTH_INFO {:08X} "
                  "pase {}x{} ndc ({:.5g},{:.5g},{:.5g},{:.5g}) funcion z del pipeline {} vmin {}; posiciones{}; "
                  "constantes VS{}",
                  draw_occlusion_warnings_, vte, r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL], sc_mode,
                  r[gr::XE_GPU_REG_RB_DEPTH_INFO], pass_width_, pass_height_, ndc[0], ndc[1], ndc[2], ndc[3],
                  (key.depth_value >> 4) & 0x7, vmin, positions, constantes);
    }
    // Draws and triangles per render target type (same classification as the GPU time).
    {
      const uint32_t category = TargetCategory(pitch, keys);
      const uint32_t draw_vertices = with_indices ? uint32_t(indices16 ? indices16_.size() : indices_.size())
                                                   : num;
      ++draws_per_category_[category];
      triangles_per_category_[category] += draw_vertices / 3;
    }
    // In the diagnostic frame, one query per draw with its pixel shader.
    // The diagnostic window is decided by the context, at the Swap. With the diagnostic off (the normal
    // case) this does not even reach the call.
    const uint32_t draw_query =
        !diag_draw_stats_
            ? UINT32_MAX
            : contexto_->StartDrawStats(
                  ps ? ps->number + 1 : 0,
                  TargetCategory(pitch, keys));
    /*
     * nfsmw_reflejo_visibilidad. An occlusion query around this draw alone, if it samples the reflection:
     * 0 samples = it wrote nothing to any target. Two of the same type cannot be open at once, so while a
     * game query is open it is not measured and counts as visible. Every kTestigoCada frames the final
     * composite, which paints the whole screen, is measured the same way: it must produce samples (the
     * guard, in nfsmw_recortes_carrera.cpp).
     */
    uint32_t visibility_query = UINT32_MAX;
    if (reflection_reads) {
      if (!open_occlusion_ && occlusion_query_ == UINT32_MAX) {
        visibility_query = contexto_->StartVisibilityQuery(false);
      }
      if (visibility_query == UINT32_MAX) {
        nfsmw::demand_reflection::TrackVisible(false);
      }
    } else if (pending_sentinel_ && ps && ps->shader && ps->shader->fingerprint == kCompositionFingerprint &&
               !open_occlusion_ && occlusion_query_ == UINT32_MAX) {
      visibility_query = contexto_->StartVisibilityQuery(true);
      if (visibility_query != UINT32_MAX) {
        pending_sentinel_ = false;
        // While the first 8 are checked, one every 2 frames: the guard finishes in ~0.5 s of racing.
        next_sentinel_ =
            frame_ + (nfsmw::demand_reflection::CheckedVisibility() ? kSentinelEvery : uint64_t(2));
      }
    }
    if (with_indices) {
      // The upload buffer is bound once per index type and each draw uses firstIndex.
      const VkIndexType indices_type = indices16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
      if (indices_type != recorded_indices_type_) {
        NFSMW_SUB(9, dfn_.vkCmdBindIndexBuffer(cmd, upload_, 0, indices_type));
        recorded_indices_type_ = indices_type;
      }
      // On the base-zero path, vertexOffset also carries where the copy starts (in vertices).
      const int32_t vertices_offset =
          zero_base ? int32_t(int64_t(first_vertex) - int64_t(vmin)) : -int32_t(vmin);
      NFSMW_SUB(10, dfn_.vkCmdDrawIndexed(cmd, uint32_t(indices16 ? indices16_.size() : indices_.size()),
                                          1, uint32_t(offset_indices / (indices16 ? 2 : 4)),
                                          vertices_offset, 0));
    } else {
      NFSMW_SUB(10, dfn_.vkCmdDraw(cmd, num, 1, zero_base ? first_vertex : 0, 0));  // firstVertex
    }
    if (visibility_query != UINT32_MAX) {
      contexto_->FinishVisibilityQuery(visibility_query);
    }
    if (draw_query != UINT32_MAX) {
      contexto_->FinishDrawStats(draw_query);
    }
    Stage(6, mark);
    if (time_) {
      ++sub_samples_;
      SubstagesReport();
    }
    ++drawn_;
    ++draws_in_pass_;  // to know at which position of the pass the sky ends up emitted
    timed_drawn_ += time_ ? 1 : 0;
    return true;
  }

  /*
   * Emits the deferred sky draw (nfsmw_nativo_cielo_aplazado).
   *
   * It records exactly the same vkCmd* calls it would have recorded in its original place, with the
   * values saved then. It relies on nothing recorded afterwards: it sends all of this draw's dynamic
   * state, and when done it invalidates the tracked state so the next draw records its own again. That
   * way the order cannot slip through an "already set" comparison.
   *
   * The upload buffer offsets are still valid because Reservar only moves forward and the buffer is not
   * reset until UsarRanura, which always comes after AntesDeEnviar -> TerminarPase.
   */
  void EmitDeferredSky(VkCommandBuffer cmd, uint32_t reason) {
    if (!sky_pending_ || cmd == VK_NULL_HANDLE) {
      return;
    }
    sky_pending_ = false;
    const DeferredSky& c = sky_;
    ++sky_emitted_[reason < kSkyReasons ? reason : kSkyReasons - 1];
    const uint64_t position = draws_in_pass_ - c.draws_on_defer;
    sky_sum_position_ += position;
    sky_position_max_ = std::max(sky_position_max_, position);
    dfn_.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.pipeline);
    bound_pipeline_ = c.pipeline;
    if (c.eds_mode) {  // phases 1 and 2: all the state its pipeline lacks
      EmitDynamicState(cmd, c.eds, true, c.eds_mode);
    }
    eds_valid_ = false;  // the next draw sets all of its own again
    valid_bound_key_ = false;  // The sky does not keep its key (ContarCambioPipeline)
    if (!bound_sets_) {
      dfn_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_pipeline_, 0, 4,
                                   sets_.data(), 0, nullptr);
      bound_sets_ = true;
    }
    if (!c.usa_ubo) {
      dfn_.vkCmdPushConstants(cmd, layout_pipeline_,
                              VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                              sizeof(c.push), c.push);
    }
    dfn_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_pipeline_, 4, 1,
                                 &sets_ubo_[c.ranura_ubo], 3, c.offsets_ubo.data());
    dfn_.vkCmdSetViewport(cmd, 0, 1, &c.viewport);
    dfn_.vkCmdSetScissor(cmd, 0, 1, &c.scissor);
    dfn_.vkCmdSetBlendConstants(cmd, c.blend);
    dfn_.vkCmdSetDepthBias(cmd, c.bias[0], 0.0f, c.bias[1]);
    if (c.with_stencil) {
      dfn_.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, c.stencil[0] & 0xFF);
      dfn_.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, c.stencil[1] & 0xFF);
      dfn_.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, (c.stencil[0] >> 8) & 0xFF);
      dfn_.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, (c.stencil[1] >> 8) & 0xFF);
      dfn_.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, (c.stencil[0] >> 16) & 0xFF);
      dfn_.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, (c.stencil[1] >> 16) & 0xFF);
    }
    if (c.n_bindings) {
      std::array<VkBuffer, 16> buffers{};
      buffers.fill(c.buf);
      dfn_.vkCmdBindVertexBuffers(cmd, 0, c.n_bindings, buffers.data(), c.offsets_vertices.data());
    }
    const uint32_t query = !diag_draw_stats_
                                  ? UINT32_MAX
                                  : contexto_->StartDrawStats(c.ps_mas_uno, c.category);
    if (c.with_indices) {
      const VkIndexType type = c.indices16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
      dfn_.vkCmdBindIndexBuffer(cmd, c.buf, 0, type);
      dfn_.vkCmdDrawIndexed(cmd, c.indices, 1, c.first_index, -int32_t(c.vmin), 0);
    } else {
      dfn_.vkCmdDraw(cmd, c.num, 1, 0, 0);
    }
    if (query != UINT32_MAX) {
      contexto_->FinishDrawStats(query);
    }
    ++draws_in_pass_;
    // Nothing tracked is valid any more: the dynamic state, the bindings and the index type are the sky's.
    // estado_grabado_ = false makes the next draw also re-record stencil and indices.
    recorded_state_ = false;
    bound_ubo_ = false;
    recorded_bindings_ = 0;
    valid_recorded_stencil_ = false;
    recorded_indices_type_ = VK_INDEX_TYPE_MAX_ENUM;
  }

  // Adds the time since the mark to the stage and moves the mark to now.
  /*
   * Measurement only. What takes the time inside the ring's grabar and pipeline stages (4.1 and 2.5 us per
   * recorded draw): each Vulkan command, PipelineDe and the shared constants block, on the same timed
   * draws (1 in 64). "C6 subetapas" line every 10 s: us per draw, us per call and calls per timed draw.
   */
  // Its lines, like those of the other ring reports, go to the report thread (NFSMW_INFORME_ANILLO).
  void SubstagesReport() {
    const auto now_time = std::chrono::steady_clock::now();
    if (now_time < sub_next_) {
      return;
    }
    const bool is_first = sub_next_ == std::chrono::steady_clock::time_point{};
    sub_next_ = now_time + std::chrono::seconds(10);
    if (is_first || !sub_samples_) {
      sub_ns_.fill(0);
      sub_n_.fill(0);
      sub_samples_ = 0;
      return;
    }
    static constexpr const char* kNames[19] = {
        "BindPipeline", "sets 0-3", "set 4 (UBO)", "Viewport", "Scissor", "BlendConstants", "DepthBias",
        "Stencil (6)", "BindVertexBuffers", "BindIndexBuffer", "Draw", "PipelineDe", "compartidas",
        "oclusion", "samplers (etapa texturas)",
        // The four segments of the "indices" stage (CortarSubetapa)
        "etapa indices: destinos y descartes", "etapa indices: desplazamiento de profundidad",
        "etapa indices: indices", "etapa indices: vertices y diagnostico"};
    std::string line;
    double total = 0.0;
    for (size_t k = 0; k < 19; ++k) {
      if (!sub_n_[k]) {
        continue;
      }
      const double per_draw = double(sub_ns_[k]) / 1e3 / double(sub_samples_);
      total += per_draw;
      line += fmt::format(" | {} {:.2f} us/dibujo ({:.2f} us x {:.2f} por dibujo)", kNames[k], per_draw,
                           double(sub_ns_[k]) / 1e3 / double(sub_n_[k]),
                           double(sub_n_[k]) / double(sub_samples_));
    }
    NFSMW_RING_REPORT("[nativo] C6 subetapas ({} dibujos cronometrados; suma {:.2f} us por dibujo){}", sub_samples_,
                total, line);
    sub_ns_.fill(0);
    sub_n_.fill(0);
    sub_samples_ = 0;
  }

  /*
   * The base-zero path of a draw (nfsmw_nativo_vertices_base_cero). Returns false if it has to bind as
   * usual. Self-checking guard: the first kBaseCeroAComprobar draws, then 1 in 4,096, redo the computation
   * backwards in 64 bits (the first vertex the GPU reads falls on the first byte of the copy, with and
   * without indices), and check that vertexOffset fits in a signed 32-bit value and that the copy fits in
   * the buffer. On a single difference: that draw binds as usual, DIFERENCIA in the log and the path is off
   * for the session.
   */
  bool CheckZeroBase(VkDeviceSize offset, VkDeviceSize stride_bytes, uint64_t bytes, uint32_t vmin,
                         uint32_t& first_vertex) {
    const VkDeviceSize first_item = offset / stride_bytes;
    if (first_item > VkDeviceSize(INT32_MAX)) {
      return false;  // does not fit in firstVertex/vertexOffset (cannot happen with a 64 MB buffer)
    }
    const uint64_t n = ++checked_zero_base_;
    if (n <= kZeroBaseToCheck || (n & 4095) == 0) {
      const int64_t byte_offset = int64_t(first_item) - int64_t(vmin);  // the vertexOffset of indexed draws
      const bool ok = first_item * stride_bytes == offset &&
                        (byte_offset + int64_t(vmin)) * int64_t(stride_bytes) == int64_t(offset) &&
                        byte_offset >= int64_t(INT32_MIN) && byte_offset <= int64_t(INT32_MAX) &&
                        offset + bytes <= kUploadSize && offset + bytes <= real_size_upload_;
      if (!ok) {
        zero_vertices_base_disabled_ = true;
        zero_vertices_base_ = false;
        REXLOG_ERROR("[nativo] C6 vertices base cero: DIFERENCIA (offset {} zancada {} bytes {} vmin {} primero {}): "
                     "este dibujo y los demas de la sesion enlazan como siempre",
                     offset, stride_bytes, bytes, vmin, first_item);
        return false;
      }
      if (n == kZeroBaseToCheck) {
        NFSMW_RING_REPORT("[nativo] C6 vertices base cero: {} dibujos comprobados, 0 diferencias; sigue comprobando 1 de "
                    "cada 4096", n);
      }
    }
    first_vertex = uint32_t(first_item);
    return true;
  }

  /*
   * Set 4 by differences, once per submission (UsarRanura). Tells NVK whether it is wanted
   * (nfsmw_nativo_set4_diferencias and the test alternation) and, if the NVK guard has seen a DIFERENCIA,
   * reports it in the log as an error once: NVK has already switched it off for the rest of the session.
   */
  void ControlSet4() {
    bool request = REXCVAR_GET(nfsmw_nativo_set4_diferencias);
    const int32_t alternar = REXCVAR_GET(nfsmw_nativo_set4_diferencias_alternar_s);
    if (alternar > 0) {
      const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                                             set4_toggle_start_)
                                .count();
      request = (secs / alternar) % 2 == 1;
    }
#if REX_PLATFORM_SWITCH
    NvkSwitchSet4* const nvk = &nvk_switch_set4;
    const bool with_patch = nvk != nullptr && nvk->version == 1;
#else
    const bool with_patch = false;
#endif
    if (request != set4_request_ || !set4_tracked_request_) {
      set4_request_ = request;
      set4_tracked_request_ = true;
      REXLOG_INFO("[nativo] C6 set 4 por diferencias (build 184): {} (fotograma {}){}",
                  request ? "pedido a NVK" : "NO pedido: NVK enlaza el conjunto entero", frame_,
                  with_patch ? "" : "; este NVK no tiene parche_nvk_set4 (Mesa anterior o PC): enlaza como siempre");
    }
#if REX_PLATFORM_SWITCH
    if (with_patch) {
      __atomic_store_n(&nvk->request, request ? 1 : 0, __ATOMIC_RELAXED);
      if (!set4_warned_difference_ && __atomic_load_n(&nvk->disabled, __ATOMIC_RELAXED) != 0) {
        set4_warned_difference_ = true;
        REXLOG_ERROR("[nativo] C6 set 4 por diferencias: DIFERENCIA vista por la guardia de NVK ({} cbufs mal "
                     "enlazados, cada uno arreglado en su dibujo; detalle en rex_stderr.log). Apagado para el resto "
                     "de la sesion: NVK enlaza el conjunto entero, como siempre",
                     __atomic_load_n(&nvk->total.differences, __ATOMIC_RELAXED));
      }
    }
#endif
  }

  /*
   * The draw path in NVK, once per submission (like ControlSet4). Tells NVK what to measure and which
   * improvements are wanted (they apply from the next command buffer) and, if an improvement's guard has
   * seen a DIFERENCIA, reports it in the log as an error once: NVK has already switched it off for the
   * rest of the session.
   */
  void NvkDrawControl() {
#if REX_PLATFORM_SWITCH
    NvkSwitchDraw* const nvk = &nvk_draw_switch;
    if (nvk == nullptr || nvk->version != 1) {
      nvk_app_preload_ = false;
      if (!nvk_tracked_draw_) {
        nvk_tracked_draw_ = true;
        REXLOG_INFO("[nativo] C6 NVK por dibujo (build 186): este NVK no tiene el contrato nvk_switch_dibujo (Mesa "
                    "anterior): sin medida ni mejoras");
      }
      return;
    }
    int32_t do_measure = std::max<int32_t>(0, REXCVAR_GET(nfsmw_nativo_nvk_medir));
    if (do_measure > 0) {
      do_measure = int32_t(std::bit_floor(uint32_t(do_measure)));  // NVK wants a power of 2
    }
    bool improvements = true;
    const int32_t alternar = REXCVAR_GET(nfsmw_nativo_nvk_alternar_s);
    if (alternar > 0) {
      const auto secs =
          std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - nvk_start_draw_)
              .count();
      improvements = (secs / alternar) % 2 == 1;
    }
    const int32_t requests[5] = {improvements && REXCVAR_GET(nfsmw_nativo_nvk_emision) ? 1 : 0,
                                improvements && REXCVAR_GET(nfsmw_nativo_nvk_cbufs) ? 1 : 0,
                                improvements && REXCVAR_GET(nfsmw_nativo_nvk_dinamico) ? 1 : 0,
                                0,  // the fast set 4 path (p05) is not applied
                                improvements && REXCVAR_GET(nfsmw_nativo_nvk_precarga) ? 1 : 0};
    __atomic_store_n(&nvk->do_measure, do_measure, __ATOMIC_RELAXED);
    __atomic_store_n(&nvk->measure_failures, REXCVAR_GET(nfsmw_nativo_nvk_medir_fallos) ? 1 : 0, __ATOMIC_RELAXED);
    for (size_t m = 0; m < 5; ++m) {
      __atomic_store_n(&nvk->improvements[m].request, requests[m], __ATOMIC_RELAXED);
    }
    nvk_app_preload_ = requests[4] != 0;
    if (!nvk_tracked_draw_ || improvements != nvk_requested_improvements_) {
      nvk_tracked_draw_ = true;
      nvk_requested_improvements_ = improvements;
      REXLOG_INFO("[nativo] C6 NVK por dibujo (build 186): mejoras {} (emision {}, cbufs {}, dinamico {}, precarga {}); "
                  "medida 1 de cada {} llamadas{}; entorno NVK_SWITCH_DIBUJO {} (fotograma {})",
                  improvements ? "pedidas" : "NO pedidas", requests[0], requests[1], requests[2], requests[4], do_measure,
                  REXCVAR_GET(nfsmw_nativo_nvk_medir_fallos) ? " con los fallos de cache aparte" : "",
                  __atomic_load_n(&nvk->environment, __ATOMIC_RELAXED), frame_);
    }
    static constexpr const char* kNames[5] = {"emision", "cbufs", "dinamico", "set 4 rapido", "precarga"};
    for (size_t m = 0; m < 5; ++m) {
      if (!nvk_warned_disabled_[m] && __atomic_load_n(&nvk->improvements[m].disabled, __ATOMIC_RELAXED) != 0) {
        nvk_warned_disabled_[m] = true;
        REXLOG_ERROR("[nativo] C6 NVK por dibujo: DIFERENCIA vista por la guardia de «{}» ({} comprobaciones "
                     "distintas; detalle en rex_stderr.log). Apagada para el resto de la sesion: NVK lo hace como "
                     "siempre",
                     kNames[m], __atomic_load_n(&nvk->improvements[m].differences, __ATOMIC_RELAXED));
      }
    }
#endif
  }

  /*
   * Every 10 s, the NVK figures. Per part, us per measured call and us per measured draw; per measured
   * draw, how much work it carries; per pipeline bind, how many state copies changed nothing; and per
   * improvement, uses, checks and differences. Totals are from command buffers that have already finished.
   */
  void NvkDrawReport() {
#if REX_PLATFORM_SWITCH
    const auto now_time = std::chrono::steady_clock::now();
    if (now_time - nvk_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool is_first = nvk_report_ == std::chrono::steady_clock::time_point{};
    nvk_report_ = now_time;
    NvkSwitchDraw* const nvk = &nvk_draw_switch;
    if (nvk == nullptr || nvk->version != 1) {
      return;
    }
    uint64_t times[16], ticks[16], counts[13], improvements[5][4];
    for (size_t i = 0; i < 16; ++i) {
      const uint64_t v = __atomic_load_n(&nvk->parts[i].times, __ATOMIC_RELAXED);
      const uint64_t t = __atomic_load_n(&nvk->parts[i].ticks, __ATOMIC_RELAXED);
      times[i] = v - nvk_parts_previous_[i][0];
      ticks[i] = t - nvk_parts_previous_[i][1];
      nvk_parts_previous_[i][0] = v;
      nvk_parts_previous_[i][1] = t;
    }
    for (size_t i = 0; i < 13; ++i) {
      const uint64_t c = __atomic_load_n(&nvk->counts[i], __ATOMIC_RELAXED);
      counts[i] = c - nvk_counts_previous_[i];
      nvk_counts_previous_[i] = c;
    }
    for (size_t m = 0; m < 5; ++m) {
      const uint64_t c[4] = {__atomic_load_n(&nvk->improvements[m].uses, __ATOMIC_RELAXED),
                             __atomic_load_n(&nvk->improvements[m].validated_items, __ATOMIC_RELAXED),
                             __atomic_load_n(&nvk->improvements[m].differences, __ATOMIC_RELAXED),
                             __atomic_load_n(&nvk->improvements[m].without_check, __ATOMIC_RELAXED)};
      for (size_t k = 0; k < 4; ++k) {
        improvements[m][k] = c[k] - nvk_improvements_previous_[m][k];
        nvk_improvements_previous_[m][k] = c[k];
      }
    }
    if (is_first) {
      return;
    }
    const uint64_t tps = __atomic_load_n(&nvk->per_second_ticks, __ATOMIC_RELAXED);
    const double us_tick = tps ? 1e6 / double(tps) : 0.0;
    const uint64_t draws = counts[0];  // NVK_SW_C_DIBUJOS_MEDIDOS
    // us per measured call and, in brackets, us per measured draw.
    const auto part = [&](size_t i) {
      return fmt::format("{:.2f} [{:.2f}]", times[i] ? double(ticks[i]) * us_tick / double(times[i]) : 0.0,
                         draws ? double(ticks[i]) * us_tick / double(draws) : 0.0);
    };
    const auto per_draw = [&](size_t i) { return draws ? double(counts[i]) / double(draws) : 0.0; };
    if (draws) {
      NFSMW_RING_REPORT(
          "[nativo] C6 NVK por partes (build 186; us por llamada medida [us por dibujo medido]; {} dibujos medidos): "
          "dibujo entero {} | push desc {} | dinamico {} | toque shaders {} | shaders {} | cbufs {} | emitir {} | "
          "BindPipeline: shaders {} toque {} copia del estado {} | sets {} (raiz {} ensuciar {}) | tabla raiz {} | "
          "trozo nuevo {} | BindVertexBuffers {}",
          draws, part(0), part(1), part(2), part(3), part(4), part(5), part(6), part(7), part(8),
          part(9), part(10), part(11), part(12), part(13), part(14), part(15));
      NFSMW_RING_REPORT(
          "[nativo] C6 NVK por dibujo medido: {:.1f} dwords; {:.2f} con estado dinamico sucio ({:.1f} bits); {:.2f} con "
          "shaders sucios; {:.2f} cbufs reenlazados | pipelines: {} enlaces medidos, {:.1f} % copias que no cambiaron "
          "nada, {:.2f} bits nuevos por enlace | {} subidas a la tabla raiz ({:.1f} palabras cada una), {} trozos nuevos",
          per_draw(1), per_draw(2), counts[2] ? double(counts[3]) / double(counts[2]) : 0.0, per_draw(4),
          per_draw(5), counts[6], counts[6] ? 100.0 * double(counts[7]) / double(counts[6]) : 0.0,
          counts[6] ? double(counts[8]) / double(counts[6]) : 0.0, counts[10],
          counts[10] ? double(counts[11]) / double(counts[10]) : 0.0, counts[12]);
    }
    static constexpr size_t kShown[4] = {0, 1, 2, 4};  // without the fast set 4 (p05), which is not applied
    static constexpr const char* kNames[5] = {"emision", "cbufs", "dinamico", "set 4 rapido", "precarga"};
    std::string texto;
    for (const size_t m : kShown) {
      const char* state = __atomic_load_n(&nvk->improvements[m].disabled, __ATOMIC_RELAXED) != 0 ? "APAGADA por la guardia"
                           : __atomic_load_n(&nvk->improvements[m].request, __ATOMIC_RELAXED) == 0 ? "no pedida"
                                                                                              : "encendida";
      texto += fmt::format(" | {} ({}): {} usos, {} comprobadas iguales, {} distintas, {} sin comprobar", kNames[m],
                           state, improvements[m][0], improvements[m][1], improvements[m][2], improvements[m][3]);
    }
    NFSMW_RING_REPORT("[nativo] C6 NVK mejoras (build 186, ultimos 10 s){}", texto);
#endif
  }

  /*
   * Every 10 s, set 4. What changes at each bind (measured here, with or without the NVK patch) and, if
   * NVK has it, what is saved: root table writes per bind (4 without it), cbufs not rebound and the state
   * of its guard.
   */
  void Set4Report() {
    const auto now_time = std::chrono::steady_clock::now();
    if (now_time - set4_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool is_first = set4_report_ == std::chrono::steady_clock::time_point{};
    set4_report_ = now_time;
    uint64_t bindings = set4_first_;
    for (const uint64_t n : set4_changes_) {
      bindings += n;
    }
    if (!is_first && set4_draws_ && bindings) {
      const auto pct = [bindings](uint64_t n) { return 100.0 * double(n) / double(bindings); };
      NFSMW_RING_REPORT(
          "[nativo] C6 set 4: {:.2f} enlaces por dibujo ({} en {} dibujos); cambia solo VS {:.1f} %, solo PS "
          "{:.1f} %, solo compartidas {:.1f} %, VS+PS {:.1f} %, VS+compartidas {:.1f} %, PS+compartidas {:.1f} %, "
          "las tres {:.1f} %, tras bufer, ranura o cielo {:.1f} % | constantes subidas otra vez: VS {} por "
          "generacion, {} por epoca y {} porque el shader lee mas; PS {}, {} y {}",
          double(bindings) / double(set4_draws_), bindings, set4_draws_, pct(set4_changes_[1]),
          pct(set4_changes_[2]), pct(set4_changes_[4]), pct(set4_changes_[3]), pct(set4_changes_[5]),
          pct(set4_changes_[6]), pct(set4_changes_[7]), pct(set4_first_), vs_reuploaded_[0], vs_reuploaded_[1],
          vs_reuploaded_[2], ps_reuploaded_[0], ps_reuploaded_[1], ps_reuploaded_[2]);
#if REX_PLATFORM_SWITCH
      if (NvkSwitchSet4* const nvk = &nvk_switch_set4; nvk != nullptr && nvk->version == 1) {
        const uint64_t counts[9] = {__atomic_load_n(&nvk->total.difference_bindings, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.complete_bindings, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.root_writes, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.root_dwords, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.dirty_cbufs, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.saved_cbufs, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.draws, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.checks, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.differences, __ATOMIC_RELAXED)};
        uint64_t d[9];
        for (size_t i = 0; i < 9; ++i) {
          d[i] = counts[i] - set4_nvk_previous_[i];
          set4_nvk_previous_[i] = counts[i];
        }
        const char* const state = __atomic_load_n(&nvk->disabled, __ATOMIC_RELAXED) != 0 ? "APAGADO por la guardia"
                                   : __atomic_load_n(&nvk->environment, __ATOMIC_RELAXED) == 0
                                       ? "apagado por NVK_SWITCH_DYN_UBO_DELTA"
                                   : !set4_request_ ? "apagado por el cvar"
                                                   : "encendido";
        NFSMW_RING_REPORT(
            "[nativo] C6 set 4 por diferencias (NVK, {}): {} enlaces por diferencias y {} enteros; {:.2f} escrituras "
            "a la tabla raiz por enlace (antes 4) con {:.2f} palabras; cbufs ensuciados {} y sin ensuciar {} ({:.1f} % "
            "menos); guardia: {} cbufs comprobados ({} dibujos por el camino nuevo), {} diferencias",
            state, d[0], d[1], d[0] ? double(d[2]) / double(d[0]) : 0.0, d[0] ? double(d[3]) / double(d[0]) : 0.0,
            d[4], d[5], d[4] + d[5] ? 100.0 * double(d[5]) / double(d[4] + d[5]) : 0.0, d[7], d[6], d[8]);
      }
#endif
    }
    set4_changes_.fill(0);
    set4_first_ = 0;
    set4_draws_ = 0;
    vs_reuploaded_.fill(0);
    ps_reuploaded_.fill(0);
  }

  // Every 10 s, how many draws go without binding their own offset (base zero).
  void ZeroBaseReport() {
    const auto now_time = std::chrono::steady_clock::now();
    if (now_time - zero_base_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool is_first = zero_base_report_ == std::chrono::steady_clock::time_point{};
    zero_base_report_ = now_time;
    if (!is_first && zero_base_total_) {
      NFSMW_RING_REPORT("[nativo] C6 vertices base cero ({}): {} de {} dibujos con vertices sin enlazar su propio "
                  "desplazamiento ({:.1f} %); {} con varios enlaces y {} con la copia del dedupe fuera de un "
                  "multiplo de la zancada; {:.2f} vkCmdBindVertexBuffers por dibujo",
                  zero_vertices_base_disabled_ ? "APAGADO por la guardia"
                  : zero_vertices_base_       ? "encendido"
                                              : "apagado",
                  draws_zero_base_, zero_base_total_, 100.0 * double(draws_zero_base_) / double(zero_base_total_),
                  bindings_several_zero_base_, misaligned_zero_base_,
                  double(recorded_bindings_zero_base_) / double(zero_base_total_));
    }
    draws_zero_base_ = 0;
    zero_base_total_ = 0;
    bindings_several_zero_base_ = 0;
    misaligned_zero_base_ = 0;
    recorded_bindings_zero_base_ = 0;
  }

  /*
   * The framing of a draw, moved out of Dibujar unchanged (nfsmw_nativo_encuadre_cache). Returns 0 if it
   * has to draw, 1 if the viewport is empty and 2 if the scissor is empty (in both cases Dibujar returns
   * without drawing, as before; the scissor is left at zero). Same operations in the same order. The only
   * multiplications GCC could fuse with an add or subtract (ancho * 0.5 and alto * 0.5) are exact, so
   * fused or not they give the same bits.
   */
  uint32_t ComputeFraming(const uint32_t* r, uint32_t vte, bool without_clip, uint32_t sc_mode, VkViewport& viewport,
                            float ndc[4], VkRect2D& scissor) {
    float scale_x = (vte & 0x1) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_XSCALE]) : 1.0f;
    float center_x = (vte & 0x2) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_XOFFSET]) : 0.0f;
    float scale_y = (vte & 0x4) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_YSCALE]) : 1.0f;
    float center_y = (vte & 0x8) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_YOFFSET]) : 0.0f;
    const float scale_z = (vte & 0x10) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_ZSCALE]) : 1.0f;
    const float center_z = (vte & 0x20) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_ZOFFSET]) : 0.0f;
    const uint32_t window = r[gr::XE_GPU_REG_PA_SC_WINDOW_OFFSET];
    const int32_t window_x = int32_t((window & 0x7FFF) << 17) >> 17;
    const int32_t window_y = int32_t(((window >> 16) & 0x7FFF) << 17) >> 17;
    if ((sc_mode >> 16) & 0x1) {
      center_x += float(window_x);
      center_y += float(window_y);
    }
    // With the shadow map drawn smaller (nfsmw_nativo_sombras_escala), the guest still speaks in
    // 1600-pixel units. The viewport and the scissor are multiplied by the pass scale, and with that the
    // geometry lands where it should; the rest (ndc, half pixel) already comes from the pass size.
    if (scale_pass_ != 1.0f) {
      scale_x *= scale_pass_;
      center_x *= scale_pass_;
      scale_y *= scale_pass_;
      center_y *= scale_pass_;
    }
    viewport = VkViewport{};
    viewport.x = center_x - std::abs(scale_x);
    viewport.width = 2.0f * std::abs(scale_x);
    // A negative YSCALE (the D3D norm) gives a positive height; a positive one, an inverted height.
    viewport.y = center_y + scale_y;
    viewport.height = -2.0f * scale_y;
    if (!((r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL] >> 19) & 0x1)) {
      Warn(14, "espacio de recorte OpenGL (dx_clip_space_def = 0): Z sin ajustar");
    }
    viewport.minDepth = std::clamp(center_z, 0.0f, 1.0f);
    viewport.maxDepth = std::clamp(center_z + scale_z, 0.0f, 1.0f);
    // g_NdcScale (x, y) and g_NdcOffset (x, y): identity when clipping is on.
    ndc[0] = 1.0f;
    ndc[1] = 1.0f;
    ndc[2] = 0.0f;
    ndc[3] = 0.0f;
    if (without_clip) {
      const float width_px = float(pass_width_);
      const float height_px = float(pass_height_);
      ndc[0] = scale_x * 2.0f / width_px;
      ndc[2] = (center_x - width_px * 0.5f) * 2.0f / width_px;
      // DXC flips Y at the end of the VS (-fvk-invert-y): the D3D Y goes here.
      ndc[1] = -scale_y * 2.0f / height_px;
      ndc[3] = -(center_y - height_px * 0.5f) * 2.0f / height_px;
      viewport.x = 0.0f;
      viewport.y = 0.0f;
      viewport.width = width_px;
      viewport.height = height_px;
    }
    scissor = VkRect2D{};
    if (viewport.width < 1.0f || std::abs(viewport.height) < 1.0f) {
      return 1;
    }
    const uint32_t tl = r[gr::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL];
    const uint32_t br = r[gr::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
    int32_t x0 = int32_t(tl & 0x3FFF), y0 = int32_t((tl >> 16) & 0x3FFF);
    int32_t x1 = int32_t(br & 0x3FFF), y1 = int32_t((br >> 16) & 0x3FFF);
    if (!((tl >> 31) & 0x1)) {
      x0 += window_x;
      y0 += window_y;
      x1 += window_x;
      y1 += window_y;
    }
    if (scale_pass_ != 1.0f) {  // also in guest pixels
      x0 = int32_t(std::floor(float(x0) * scale_pass_));
      y0 = int32_t(std::floor(float(y0) * scale_pass_));
      x1 = int32_t(std::ceil(float(x1) * scale_pass_));
      y1 = int32_t(std::ceil(float(y1) * scale_pass_));
    }
    x0 = std::clamp(x0, 0, int32_t(pass_width_));
    x1 = std::clamp(x1, 0, int32_t(pass_width_));
    y0 = std::clamp(y0, 0, int32_t(pass_height_));
    y1 = std::clamp(y1, 0, int32_t(pass_height_));
    if (x1 <= x0 || y1 <= y0) {
      return 2;
    }
    scissor = VkRect2D{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
    return 0;
  }

  // The C1 guard. The current computation against the saved one, byte by byte.
  void CompareFraming(const VkViewport& viewport, const float ndc[4], const VkRect2D& scissor, uint32_t is_empty) {
    ++framing_checked_;
    const bool is_equal = is_empty == framing_empty_ &&
                       std::memcmp(&viewport, &framing_viewport_, sizeof(viewport)) == 0 &&
                       std::memcmp(ndc, framing_ndc_, sizeof(framing_ndc_)) == 0 &&
                       std::memcmp(&scissor, &framing_scissor_, sizeof(scissor)) == 0;
    if (!is_equal) {
      framing_cache_disabled_ = true;
      framing_cache_ = false;
      REXLOG_ERROR("[nativo] C6 encuadre en cache: DIFERENCIA en la comprobacion {} (vacio {} / {}; viewport {},{} "
                   "{}x{} / {},{} {}x{}; tijera {},{} {}x{} / {},{} {}x{}). Apagada para el resto de la sesion: este "
                   "dibujo usa el recalculado",
                   framing_checked_, is_empty, framing_empty_, viewport.x, viewport.y, viewport.width,
                   viewport.height, framing_viewport_.x, framing_viewport_.y, framing_viewport_.width,
                   framing_viewport_.height, scissor.offset.x, scissor.offset.y, scissor.extent.width,
                   scissor.extent.height, framing_scissor_.offset.x, framing_scissor_.offset.y,
                   framing_scissor_.extent.width, framing_scissor_.extent.height);
    } else if (framing_checked_ == kFramingsToCheck) {
      NFSMW_RING_REPORT("[nativo] C6 encuadre en cache: {} aciertos comprobados contra el calculo, 0 diferencias; sigue "
                  "comprobando 1 de cada 4096", framing_checked_);
    }
  }

  /*
   * The alto_util_ element for that pitch (nfsmw_nativo_alto_util_memo). The pitch is the pass's and only
   * changes with the pass, so the last one is remembered. References to unordered_map elements are not
   * invalidated by insertion (only by erasure, and alto_util_ never erases). Guard: the first
   * kAltoUtilAComprobar reuses, then 1 in 4,096, also look it up in the map and must get the same element.
   */
  uint32_t& UsableHeightOf(uint32_t pitch) {
    if (!active_memo_usable_height_) {
      return usable_height_[pitch];
    }
    if (memo_usable_height_ == nullptr || pitch != memo_pitch_usable_height_) {
      memo_usable_height_ = &usable_height_[pitch];
      memo_pitch_usable_height_ = pitch;
      return *memo_usable_height_;
    }
    const uint64_t n = ++hits_memo_usable_height_;
    if (n <= kUsableHeightToCheck || (n & 4095) == 0) {
      uint32_t* const from_map_table = &usable_height_[pitch];
      if (from_map_table != memo_usable_height_) {
        memo_usable_height_disabled_ = true;
        active_memo_usable_height_ = false;
        memo_usable_height_ = nullptr;
        REXLOG_ERROR("[nativo] C6 alto util recordado: DIFERENCIA (pitch {}, comprobacion {}): no es el elemento "
                     "del mapa. Apagado para el resto de la sesion", pitch, n);
        return *from_map_table;
      }
      if (n == kUsableHeightToCheck) {
        NFSMW_RING_REPORT("[nativo] C6 alto util recordado: {} comprobaciones contra el mapa, 0 diferencias; sigue "
                    "comprobando 1 de cada 4096", n);
      }
    }
    return *memo_usable_height_;
  }

  /*
   * The C3 guard. With the same render target bytes, the XXH3 must be pase_clave_. Returns the computed
   * one, which is the one that decides for this draw.
   */
  uint64_t CheckPassKey(const uint64_t keys[5], uint64_t n) {
    const uint64_t computed = XXH3_64bits(keys, sizeof(uint64_t) * 5);
    if (computed != key_pass_) {
      fast_pass_key_disabled_ = true;
      fast_pass_key_ = false;
      REXLOG_ERROR("[nativo] C6 clave del pase: DIFERENCIA (comprobacion {}: {:016X} calculada, {:016X} guardada). "
                   "Apagada para el resto de la sesion", n, computed, key_pass_);
    } else if (n == kPassKeysToCheck) {
      NFSMW_RING_REPORT("[nativo] C6 clave del pase: {} reutilizadas comprobadas con XXH3, 0 diferencias; sigue "
                  "comprobando 1 de cada 4096", n);
    }
    return computed;
  }

  // Every 10 s, how much the small per-draw savings save.
  void DetailsReport() {
    const auto now_time = std::chrono::steady_clock::now();
    if (now_time - details_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool is_first = details_report_ == std::chrono::steady_clock::time_point{};
    details_report_ = now_time;
    const uint64_t framings = framing_hits_report_ + framing_calculations_report_;
    if (!is_first && framings) {
      NFSMW_RING_REPORT("[nativo] C6 minucias (build 179): encuadre de la cache en {} de {} dibujos ({:.1f} %; {}{}); "
                  "clave del pase sin XXH3 {} veces ({}); alto util recordado {} veces ({}); ajustes de los "
                  "dibujos sin color {}",
                  framing_hits_report_, framings,
                  100.0 * double(framing_hits_report_) / double(framings),
                  framing_cache_disabled_ ? "APAGADA por la guardia" : framing_cache_ ? "encendida" : "apagada",
                  framing_checked_ >= kFramingsToCheck ? ", guardia superada" : ", comprobando",
                  fast_pass_keys_ - fast_pass_keys_previous_,
                  fast_pass_key_disabled_ ? "APAGADA por la guardia" : fast_pass_key_ ? "encendida" : "apagada",
                  hits_memo_usable_height_ - hits_memo_usable_height_previous_,
                  memo_usable_height_disabled_ ? "APAGADO por la guardia" : active_memo_usable_height_ ? "encendido" : "apagado",
                  cvars_per_frame_ ? "una vez por fotograma" : "en cada dibujo");
    }
    framing_hits_report_ = 0;
    framing_calculations_report_ = 0;
    fast_pass_keys_previous_ = fast_pass_keys_;
    hits_memo_usable_height_previous_ = hits_memo_usable_height_;
  }

  // The guard of the direct-mapped pipeline cache. The same key in the map (the usual path) must give the
  // same VkPipeline as the slot. Otherwise it switches off and returns false.
  bool CheckPipelineSlot(uint64_t fingerprint, const PipelineKey& key, VkPipeline in_slot, uint64_t n) {
    ++checked_direct_pipelines_;
    VkPipeline from_map_table = VK_NULL_HANDLE;
    if (const auto it = pipelines_.find(fingerprint);
        it != pipelines_.end() && std::memcmp(&it->second.first, &key, sizeof(key)) == 0) {
      from_map_table = it->second.second;
    }
    if (from_map_table != in_slot) {
      direct_pipelines_disabled_ = true;
      direct_pipelines_ = false;
      REXLOG_ERROR("[nativo] C6 pipelines: DIFERENCIA entre la cache directa y el mapa (comprobacion {}, VS {} PS {} "
                   "especializacion {:08X}). Apagada para el resto de la sesion: manda el mapa",
                   n, key.vs, key.ps, key.specialization);
      return false;
    }
    if (n == kPipelinesToCheck) {
      NFSMW_RING_REPORT("[nativo] C6 pipelines: {} aciertos de la cache directa comprobados contra el mapa, 0 "
                  "diferencias; sigue comprobando 1 de cada 4096", n);
    }
    return true;
  }

  /*
   * Measurement only (nfsmw_nativo_contar_cambios_pipeline). What changes at each vkCmdBindPipeline.
   *
   * BindPipeline costs 3.3-4.5 us per call in a race (0.3-0.4 per draw), and on each one NVK copies all
   * the pipeline's fixed state (vk_dynamic_graphics_state_copy, ~80 groups). If the change is state only,
   * with dynamic state (vkCmdSet*) no new pipeline would be needed. Before touching anything, the counts
   * and kinds have to be known:
   *  - cull mode, topology within the same class, Z test/write/function, stencil, bias and restart are
   *    EDS1/EDS2, core in Vulkan 1.3: the console (API 1.3.354) already provides them without touching
   *    the SDK;
   *  - blending, color masks and topology of another class need VK_EXT_extended_dynamic_state3, which NVK
   *    exposes on Maxwell and the SDK only enables with ui_vulkan_estado_dinamico3.patch.
   * The new key is compared with the last one bound in the same command buffer, field by field and
   * without XXH3. Blending, masks and depth are compared in canonical form (EstadoCanonico: only what
   * PipelineDe really reads). If only bits PipelineDe does not use differ, the pipeline is effectively the
   * same: no effect. Cost: an 80-byte memcmp and about 60 operations per bind (0.3-0.4 binds per draw). It
   * changes nothing.
   */
  static uint32_t TopologyClass(uint32_t topo) {
    switch (topo) {
      case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
        return 0;
      case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
      case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
      case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
      case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
        return 1;
      case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
        return 3;
      default:
        return 2;  // triangulos: lista, tira y abanico
    }
  }

  // What PipelineDe really reads from a key's blending, masks and depth, with its same criterion
  // (blendEnable per target; depthWriteEnable = test and write; the back face copies the front).
  static void CanonicalState(const PipelineKey& c, uint32_t blend[4], uint32_t& masks, uint32_t& depth_value) {
    masks = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      blend[i] = 0;
      if (!c.formats[i]) {
        continue;  // PipelineDe skips color targets not in the pass
      }
      const uint32_t mask = (c.masks >> (i * 4)) & 0xF;
      masks |= mask << (i * 4);
      const uint32_t m = c.blend[i];
      const bool direct_color = (m & 0x1F) == 1 && ((m >> 8) & 0x1F) == 0 && ((m >> 5) & 0x7) == 0;
      const bool direct_alpha = ((m >> 16) & 0x1F) == 1 && ((m >> 24) & 0x1F) == 0 && ((m >> 21) & 0x7) == 0;
      if (((mask & 0x7) && !direct_color) || ((mask & 0x8) && !direct_alpha)) {
        blend[i] = m;  // blendEnable: factors count (without blending Vulkan ignores them)
      }
    }
    uint32_t d = c.formats[4] ? c.depth_value : 0;  // without a depth target PipelineDe ignores it
    d &= ~0x8u;                                      // PipelineDe does not read bit 3
    if (!((d >> 1) & 0x1)) {
      d &= ~0x74u;  // without a Z test, write and function do not count
    }
    if (!(d & 0x1)) {
      d &= 0x77u;  // sin estencil no cuentan sus funciones ni sus operaciones
    } else if (!((d >> 7) & 0x1)) {
      d &= 0x000FFFF7u;  // without its own back face state, the back copies the front
    }
    depth_value = d;
  }

  // Classifies a vkCmdBindPipeline from Dibujar against the last key bound in this buffer.
  // tras_empezar_pase: EmpezarPase had forgotten the bound pipeline (pipeline_enlazado_ set to
  // VK_NULL_HANDLE).
  void CountPipelineChange(const PipelineKey& incoming, bool after_pass_start) {
    uint64_t* const n = pipeline_changes_.data();
    ++n[kBindingsChange];
    n[kChangeAfterPass] += after_pass_start ? 1 : 0;
    const PipelineKey& a = bound_key_;
    constexpr uint32_t kAlphaTestBits = 0x2u | (0x7u << kSpecOffsetAlphaFunction);
    if (!valid_bound_key_) {
      ++n[kChangeFirst];
    } else if (std::memcmp(&a, &incoming, sizeof(incoming)) == 0) {
      ++n[kIdenticalChange];  // only after EmpezarPase: Vulkan kept the bound pipeline across passes
    } else if (a.vs != incoming.vs || a.ps != incoming.ps) {
      ++n[kShadersChange];
    } else if (a.entry != incoming.entry) {
      ++n[kEntryChange];
    } else if (std::memcmp(a.formats, incoming.formats, sizeof(a.formats)) != 0) {
      ++n[kFormatsChange];
    } else if (const uint32_t spec = a.specialization ^ incoming.specialization; spec != 0) {
      ++n[(spec & ~kAlphaTestBits) == 0                      ? kAlphaSpecChange
          : (spec & ~(kAlphaTestBits | kSpecZEarly)) == 0 ? kEarlySpecZChange
                                                              : kOtherSpecChange];
    } else {
      // Same shaders, input, formats and specialization: only fixed pipeline state changes.
      uint32_t blend_to[4], n_blend[4], masks_to, n_masks, prof_a, prof_n;
      CanonicalState(a, blend_to, masks_to, prof_a);
      CanonicalState(incoming, n_blend, n_masks, prof_n);
      const bool topo = a.topo != incoming.topo;
      const bool topo_class = TopologyClass(a.topo) != TopologyClass(incoming.topo);
      const bool blend = std::memcmp(blend_to, n_blend, sizeof(blend_to)) != 0;
      const bool masks = masks_to != n_masks;
      const uint32_t prof = prof_a ^ prof_n;
      const uint32_t rast = a.rasterizado ^ incoming.rasterizado;
      if (!topo && !blend && !masks && !prof && !rast) {
        ++n[kChangeWithoutEffect];  // only bits PipelineDe does not read: effectively the same pipeline
        n[kChangeWithoutEffectAfterPass] += after_pass_start ? 1 : 0;
      } else {
        const bool eds12 = !blend && !masks && !topo_class;
        ++n[kStateOnlyChange];
        n[kStateChangeAfterPass] += after_pass_start ? 1 : 0;
        n[kEds12Change] += eds12 ? 1 : 0;
        n[kEds12ChangeAfterPass] += (eds12 && after_pass_start) ? 1 : 0;
        n[kTopologyField] += topo ? 1 : 0;
        n[kTopologyClassField] += topo_class ? 1 : 0;
        n[kBlendField] += blend ? 1 : 0;
        n[kMasksField] += masks ? 1 : 0;
        n[kFieldZ] += (prof & 0x76u) ? 1 : 0;
        n[kStencilField] += (prof & ~0x76u) ? 1 : 0;
        n[kFaceField] += (rast & 0x7u) ? 1 : 0;
        n[kResetField] += (rast & 0x8u) ? 1 : 0;
        n[kBiasField] += (rast & 0x10u) ? 1 : 0;
      }
    }
    bound_key_ = incoming;
    valid_bound_key_ = true;
  }

  // Every 20 s, what changes at each vkCmdBindPipeline of the ring (measurement only). Reads its cvar on
  // every call: InformePipelinesDirecta calls it, once per frame.
  void PipelineChangesReport(std::chrono::steady_clock::time_point now_time) {
    const bool bump = REXCVAR_GET(nfsmw_nativo_contar_cambios_pipeline);
    if (bump != count_pipeline_changes_) {
      count_pipeline_changes_ = bump;
      valid_bound_key_ = false;  // what was tracked while not counting cannot be compared
    }
    if (now_time - changes_report_ < std::chrono::seconds(20)) {
      return;
    }
    const bool is_first = changes_report_ == std::chrono::steady_clock::time_point{};
    const double secs = std::chrono::duration<double>(now_time - changes_report_).count();
    changes_report_ = now_time;
    const uint64_t draws = drawn_ - draws_changes_previous_;
    const uint64_t frames = frame_ - frames_changes_previous_;
    draws_changes_previous_ = drawn_;
    frames_changes_previous_ = frame_;
    const std::array<uint64_t, kNChanges> n = pipeline_changes_;
    pipeline_changes_.fill(0);
    if (is_first || !count_pipeline_changes_ || !n[kBindingsChange] || !draws || !frames) {
      return;
    }
    const double bindings = double(n[kBindingsChange]);
    const double f = double(frames);
    const auto per_draw = [&](uint64_t x) { return double(x) / double(draws); };
    const auto pct = [&](uint64_t x) { return 100.0 * double(x) / bindings; };
    const uint64_t spec = n[kAlphaSpecChange] + n[kEarlySpecZChange] + n[kOtherSpecChange];
    NFSMW_RING_REPORT(
        "[nativo] C6 cambios de pipeline (build 184, solo medida): {} vkCmdBindPipeline en {:.0f} s ({:.3f} por "
        "dibujo, {:.1f} por fotograma; {} son el primero de un pase) | sin clave anterior {} | la MISMA clave tras "
        "empezar pase {} ({:.1f} %) | cambian shaders {} ({:.1f} %) | otra entrada {} | otros formatos {} | "
        "especializacion {} ({:.1f} %: prueba de alfa {}, con Z temprana {}, otra {}) | sin efecto (bits que "
        "PipelineDe no lee) {} ({:.1f} %) | SOLO ESTADO {} ({:.1f} %, {:.3f} por dibujo): EDS1/EDS2 sin tocar el SDK "
        "{}, el resto pide EDS3",
        n[kBindingsChange], secs, per_draw(n[kBindingsChange]), bindings / f, n[kChangeAfterPass],
        n[kChangeFirst], n[kIdenticalChange], pct(n[kIdenticalChange]), n[kShadersChange], pct(n[kShadersChange]),
        n[kEntryChange], n[kFormatsChange], spec, pct(spec), n[kAlphaSpecChange], n[kEarlySpecZChange],
        n[kOtherSpecChange], n[kChangeWithoutEffect], pct(n[kChangeWithoutEffect]), n[kStateOnlyChange],
        pct(n[kStateOnlyChange]), per_draw(n[kStateOnlyChange]), n[kEds12Change]);
    // Avoidable binds per frame with each fix, each one separately (those of the first draw of a pass are
    // not avoided by dynamic state or by the canonical key while EmpezarPase forgets the bound pipeline).
    constexpr double kUsPerBinding = 3.0;  // BindPipeline: 3.3-4.5 us per call in a race
    const uint64_t canonical = n[kChangeWithoutEffect] - n[kChangeWithoutEffectAfterPass];
    const uint64_t eds12 = n[kEds12Change] - n[kEds12ChangeAfterPass];
    const uint64_t eds123 = n[kStateOnlyChange] - n[kStateChangeAfterPass];
    NFSMW_RING_REPORT(
        "[nativo] C6 cambios de pipeline, campos del solo estado: topologia {} (de clase {}), mezcla {}, mascaras {}, "
        "Z {}, estencil {}, cara {}, reinicio {}, sesgo {}; {} de ellos el primero de un pase | enlaces evitables por "
        "fotograma (a ~3 us cada uno): {:.1f} con la clave canonica ({:.2f} ms), {:.1f} sin olvidar el pipeline al "
        "empezar pase ({:.2f} ms), {:.1f} con EDS1/EDS2 ({:.2f} ms), {:.1f} con EDS1/EDS2/EDS3 ({:.2f} ms)",
        n[kTopologyField], n[kTopologyClassField], n[kBlendField], n[kMasksField], n[kFieldZ],
        n[kStencilField], n[kFaceField], n[kResetField], n[kBiasField], n[kStateChangeAfterPass],
        double(canonical) / f, double(canonical) / f * kUsPerBinding / 1000.0, double(n[kIdenticalChange]) / f,
        double(n[kIdenticalChange]) / f * kUsPerBinding / 1000.0, double(eds12) / f,
        double(eds12) / f * kUsPerBinding / 1000.0, double(eds123) / f, double(eds123) / f * kUsPerBinding / 1000.0);
  }

  // Every 10 s, the hit rate of the direct-mapped pipeline cache.
  void DirectPipelinesReport() {
    const auto now_time = std::chrono::steady_clock::now();
    PipelineChangesReport(now_time);  // reads its cvar every frame and writes every 20 s
    TryPrewarm();       // starts the thread as soon as the library is loaded
    PrewarmedReport(now_time);  // every 10 s, if there is anything new, and its guard
    if (now_time - direct_pipelines_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool is_first = direct_pipelines_report_ == std::chrono::steady_clock::time_point{};
    direct_pipelines_report_ = now_time;
    const uint64_t hits = hits_direct_pipelines_ - hits_direct_pipelines_previous_;
    const uint64_t failures = failures_direct_pipelines_ - failures_direct_pipelines_previous_;
    hits_direct_pipelines_previous_ = hits_direct_pipelines_;
    failures_direct_pipelines_previous_ = failures_direct_pipelines_;
    if (!is_first && (hits || failures)) {
      NFSMW_RING_REPORT("[nativo] C6 pipelines (build 179): cache directa {} aciertos y {} fallos ({:.1f} % de las "
                  "busquedas que no resuelve el atajo de una entrada); {} comprobadas contra el mapa ({})",
                  hits, failures, 100.0 * double(hits) / double(hits + failures),
                  checked_direct_pipelines_,
                  direct_pipelines_disabled_ ? "APAGADA por la guardia"
                  : checked_direct_pipelines_ >= kPipelinesToCheck ? "guardia superada"
                                                                           : "comprobando");
    }
  }

  /*
   * The "indices" stage once rose from ~1.4 to ~2.6 us per draw without any change to its code. Four
   * cuts inside it (C6 subetapas 15-18), only on the timed draws (1 in 64): they cost four clock reads on
   * those draws, so the "indices" stage reads ~0.2-0.3 us higher than without them.
   */
  void CutSubstage(size_t k, std::chrono::steady_clock::time_point& t) {
    if (!time_) {
      return;
    }
    const auto now_time = std::chrono::steady_clock::now();
    sub_ns_[k] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now_time - t).count());
    ++sub_n_[k];
    t = now_time;
  }

  void Stage(size_t stage_idx, std::chrono::steady_clock::time_point& mark) {
    if (!time_) {
      return;
    }
    const auto now_time = std::chrono::steady_clock::now();
    stages_ns_[stage_idx] += uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now_time - mark).count());
    mark = now_time;
  }

  // Diagnostic nfsmw_nativo_diag_vertices_repetidos: vertex bytes that repeat address, size, byte order
  // and content within the same frame or relative to an earlier frame.
  void TrackRepeatedVertices(uint64_t address, uint32_t bytes, uint32_t orden,
                               const uint8_t* payload) {
    const auto before = std::chrono::steady_clock::now();
    const uint64_t fingerprint = XXH3_64bits(payload, bytes);
    const uint64_t key =
        XXH3_64bits_withSeed(&address, sizeof(address), (uint64_t(bytes) << 2) | orden);
    if (seen_vertices_.size() > 200000) {
      seen_vertices_.clear();  // memory cap of the diagnostic
    }
    SeenVertices& seen = seen_vertices_[key];
    if (seen.fingerprint == fingerprint && seen.frame == frame_) {
      frame_repeated_bytes_ += bytes;
    } else if (seen.fingerprint == fingerprint && seen.frame != UINT64_MAX &&
               seen.frame < frame_) {
      previous_equal_bytes_ += bytes;
    }
    seen.frame = frame_;
    seen.fingerprint = fingerprint;
    ns_hash_vertices_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - before)
                                      .count());
  }

  void FinishPass() override {
    if (active_pass_) {
      /*
       * Fallback path of the deferred sky. If nothing has emitted it yet (no transparent draw arrived, or the
       * pass closes because of a copy, a resolve or a submission), it is emitted here, inside the pass and
       * before vkCmdEndRenderPass. Losing the sky would be a visible and serious bug, so this is the last
       * place it can be, and it is always reached: the command buffer is only closed through EnviarTrabajo
       * -> AntesDeEnviar -> TerminarPase.
       */
      if (sky_pending_) {
        EmitDeferredSky(commands_pass_, kSkyPerPassEnd);
      }
      // A host occlusion query cannot stay open outside its pass.
      if (occlusion_query_ != UINT32_MAX) {
        contexto_->FinishOcclusionQuery(occlusion_query_);
        occlusion_query_ = UINT32_MAX;
      }
      const auto before_end = std::chrono::steady_clock::now();
      dfn_.vkCmdEndRenderPass(commands_pass_);
      if (pass_stats_ != UINT32_MAX) {
        contexto_->FinishStats(pass_stats_);
        pass_stats_ = UINT32_MAX;
      }
      ns_render_pass_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - before_end).count());
      const uint32_t closed_category = pass_category_;
      active_pass_ = false;
      // Just in case. A pending sky with the pass already closed cannot be recorded; it is counted so it
      // shows up in the report instead of disappearing silently.
      if (sky_pending_) {
        sky_pending_ = false;
        ++sky_lost_;
      }
      // As soon as the shadow pass closes, submit to the GPU. Once per frame: the flag keeps reopened passes
      // from submitting again. EnviarYEsperar submits and keeps recording in the next slot; it waits for
      // nothing (the name is misleading).
      if (closed_category == kGpuShadows && !sent_after_shadows_ && contexto_ &&
          REXCVAR_GET(nfsmw_nativo_enviar_tras_sombras)) {
        sent_after_shadows_ = true;
        ++sends_after_shadows_;
        contexto_->SendAndWait();
      }
    }
  }

  void BeforeSend() override {
    WaitUploads();  // deferred vertex copies, before flushing the mapping and submitting
    FinishPass();
    // Whatever is still on the texture bind thread, with its views, descriptors, barriers and copies, goes
    // into this upload buffer before it is closed (nfsmw_nativo_texturas_enlace_hilo). It is the last thing
    // recorded: after the pass closes (which may emit the deferred sky), and meanwhile the thread has kept
    // binding.
    CollectBindings(true);
    // The data of the new textures prepared by the hash thread, into this upload buffer before the mapping
    // is flushed and it is submitted (nfsmw_nativo_texturas_huella_hilo). Their barrier and copy are
    // already recorded.
    CollectFingerprints();
    if (used_upload_ && !coherent_upload_) {
      rex::ui::vulkan::util::FlushMappedMemoryRange(vk_device_, memory_upload_, type_upload_, 0,
                                                    real_size_upload_, used_upload_);
    }
    if (separately_shared_ && used_shared_ && !coherent_shared_) {
      rex::ui::vulkan::util::FlushMappedMemoryRange(vk_device_, memory_shared_, type_shared_, 0,
                                                    real_size_shared_, used_shared_);
      published_bytes_shared_ += used_shared_;
    }
  }

  void UseSlot(uint32_t ranura) override {
    WaitUploads();  // already empty after AntesDeEnviar; just in case, before switching buffers
    CopiesReport();  // every 10 s, who made the vertex copies and how long the ring waited
    CacheFetchReport();  // every 10 s, the per-fetch sampler table
    // A texture may arrive here with its bind in flight (it was prepared before this submission's first
    // Grabar) and that is normal; what it cannot have is its data already in the upload buffer just
    // submitted.
    CheckBindingsOnBufferChange();
    CheckFingerprintsOnBufferChange();  // before resetting the upload buffer
    const UploadBuffer& s = uploads_[ranura % uploads_.size()];
    upload_ = s.buf;
    memory_upload_ = s.mem;
    real_size_upload_ = s.real_size;
    data_upload_ = s.payload;
    address_upload_ = s.address;
    current_slot_ = uint32_t(ranura % uploads_.size());  // this slot's UBO set
    used_upload_ = 0;
    if (separately_shared_) {
      const UploadBuffer& c = bufs_shared_[current_slot_];
      data_shared_ = c.payload;
      memory_shared_ = c.mem;
      real_size_shared_ = c.real_size;
      used_shared_ = 0;
    }
    ++upload_epoch_;
    ++frame_;
    CloseSkyGuardFrame();
    // nfsmw_reflejo_visibilidad, once per submission (the guard may switch it off mid-session).
    measure_visibility_ = nfsmw::demand_reflection::MeasureVisibility();
    if (measure_visibility_ && frame_ >= next_sentinel_) {
      pending_sentinel_ = true;
    }
    // The upload buffer is reset here, so the recorded offsets are no longer valid. This also covers
    // EnviarYEsperar, which goes through here.
    dedupe_.NewFrame(frame_);
    sent_after_shadows_ = false;  // the post-shadow submission is once per frame
    /*
     * Diagnostic cvars are read once per frame, not per draw.
     *
     * REXCVAR_GET is not a variable read: it is FLAGS_##name##_storage_(), an out-of-line function call
     * (cvar.h:343). Four of them were being made per draw (one of them per vertex binding, not per draw),
     * and one returned a std::string that was then compared. With 2,345 draws per frame that shows, and it
     * serves no purpose: none of them changes within a frame.
     */
    diag_skip_text_ps_ = REXCVAR_GET(nfsmw_nativo_diag_omitir_ps);
    diag_repeated_vertices_ = REXCVAR_GET(nfsmw_nativo_diag_vertices_repetidos);
    active_dedupe_ = REXCVAR_GET(nfsmw_nativo_dedupe_vertices);
    zero_vertices_base_ = REXCVAR_GET(nfsmw_nativo_vertices_base_cero) && !zero_vertices_base_disabled_;
    diag_draw_stats_ = REXCVAR_GET(nfsmw_nativo_estadisticas_por_dibujo_s) > 0;
    usable_area_ = REXCVAR_GET(nfsmw_nativo_pase_area_util);
    // The small per-draw savings.
    framing_cache_ = REXCVAR_GET(nfsmw_nativo_encuadre_cache) && !framing_cache_disabled_;
    active_memo_usable_height_ = REXCVAR_GET(nfsmw_nativo_alto_util_memo) && !memo_usable_height_disabled_;
    fast_pass_key_ = REXCVAR_GET(nfsmw_nativo_clave_pase_rapida) && !fast_pass_key_disabled_;
    cvars_per_frame_ = REXCVAR_GET(nfsmw_nativo_cvars_por_fotograma);
    frame_alpha_only_ps_ = PsAlphaOnly();
    without_ps_without_frame_color_ = NoPsNoColor();
    without_vegetation_ = REXCVAR_GET(nfsmw_sombras_sin_vegetacion);
    direct_pipelines_ = REXCVAR_GET(nfsmw_nativo_pipelines_directa) && !direct_pipelines_disabled_;
    canonical_key_ = REXCVAR_GET(nfsmw_nativo_clave_canonica) && !canonical_key_disabled_;  // phase 0a
    {
      const bool fresh = REXCVAR_GET(nfsmw_nativo_pipeline_entre_pases);  // phase 0b
      if (fresh != pipeline_between_passes_ || !pipeline_between_tracked_passes_) {
        pipeline_between_passes_ = fresh;
        pipeline_between_tracked_passes_ = true;
        REXLOG_INFO("[nativo] C6 pipeline al empezar pase: {} (fotograma {})",
                    fresh ? "se CONSERVA el enlazado (fase 0b del estado dinamico)" : "se vuelve a enlazar, como antes",
                    frame_);
      }
    }
    // Dynamic state phases 1 and 2, once per command buffer: the whole buffer uses one mode, because a
    // pipeline with a fixed state invalidates the dynamic value of that state.
    {
      uint32_t mode = 0;
      if (eds12_available_ && !eds_disabled_ && REXCVAR_GET(nfsmw_nativo_estado_dinamico)) {
        mode |= kEds12;
      }
      if (eds3_available_ && !eds_disabled_ && REXCVAR_GET(nfsmw_nativo_estado_dinamico3)) {
        mode |= kEds3;
      }
      if (mode != eds_mode_ || !eds_tracked_mode_) {
        eds_tracked_mode_ = true;
        REXLOG_INFO("[nativo] C6 estado dinamico: {} (fotograma {})", EdsModeName(mode), frame_);
      }
      eds_mode_ = mode;
    }
    test_scissor_ = TestScissor();
    without_frame_blur_ = REXCVAR_GET(nfsmw_nativo_sin_desenfoque);  // once per frame
    // The cheap PCF bit also changes the pipeline: once per frame.
    {
      bool fresh = REXCVAR_GET(nfsmw_nativo_pcf_barato);
      const int32_t alternar_pcf = REXCVAR_GET(nfsmw_nativo_pcf_barato_alternar_s);
      if (alternar_pcf > 0) {
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - ps_toggle_start_)
                                  .count();
        fresh = (secs / alternar_pcf) % 2 == 1;
      }
      if (fresh != cheap_pcf_) {
        cheap_pcf_ = fresh;
        REXLOG_INFO("[nativo] C2 muestreo del mapa de sombras: {}",
                    fresh ? "un solo texel (PCF barato)" : "patron 3x3 a medio texel");
      }
    }
    // The filter between mip levels is part of the sampler key, so it is also decided once per frame. Off
    // (the normal case), this is reading one cvar and comparing a bool.
    {
      bool fresh = REXCVAR_GET(nfsmw_nativo_prueba_mip_puntual);
      const int32_t alternar_mip = REXCVAR_GET(nfsmw_nativo_prueba_mip_puntual_alternar_s);
      if (alternar_mip > 0) {
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - ps_toggle_start_)
                                  .count();
        fresh = (secs / alternar_mip) % 2 == 1;
      }
      if (fresh != test_point_mip_) {
        test_point_mip_ = fresh;
        REXLOG_INFO("[nativo] C4 filtro entre niveles de mip: {}",
                    fresh ? "PUNTUAL (prueba: trilineal -> bilineal, la TMU hace la mitad)"
                          : "lineal (el que pide el juego)");
      }
    }
    // nfsmw_nativo_diag_mip_minimo. Part of the sampler key; when it changes the per-texture sampler caches
    // are invalidated (generacion_texturas_, as when an image is retired) so it shows in the next frame.
    {
      const uint32_t fresh = uint32_t(std::clamp(int32_t(REXCVAR_GET(nfsmw_nativo_diag_mip_minimo)), 0, 4));
      if (fresh != diag_mip_min_) {
        diag_mip_min_ = fresh;
        ++textures_generation_;
        REXLOG_INFO("[nativo] C4 diagnostico de mips: las texturas con mips empiezan {} niveles mas abajo{}", fresh,
                    fresh ? " (DIAGNOSTICO: todo se vera borroso)" : " (normal)");
      }
    }
    // The shadow map's own depth bias, once per frame.
    {
      const int32_t constante = std::clamp(int32_t(REXCVAR_GET(nfsmw_nativo_sombras_sesgo_constante)), 0, 100);
      const int32_t pending = std::clamp(int32_t(REXCVAR_GET(nfsmw_nativo_sombras_sesgo_pendiente)), 0, 100);
      if (constante != shadows_constant_bias_ || pending != shadows_slope_bias_ || !shadows_tracked_bias_) {
        shadows_constant_bias_ = constante;
        shadows_slope_bias_ = pending;
        shadows_tracked_bias_ = true;
        REXLOG_INFO("[nativo] C4 desplazamiento de profundidad del mapa de sombras: pendiente {:.1f}, constante {} "
                    "(x1000 unidades de 24 bits){}",
                    double(pending) * 0.1, constante, (constante || pending) ? "" : " (apagado, como antes)");
      }
    }
    // The 1/size bit changes the pipeline, so it is decided once per frame.
    {
      const int32_t alternar = REXCVAR_GET(nfsmw_nativo_inv_tamano_tex_alternar_s);
      bool fresh = REXCVAR_GET(nfsmw_nativo_inv_tamano_tex);
      if (alternar > 0) {
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - ps_toggle_start_)
                                  .count();
        fresh = (secs / alternar) % 2 == 1;
      }
      if (fresh != tex_size_inv_) {
        tex_size_inv_ = fresh;
        REXLOG_INFO("[nativo] C2 tamano de textura: {}",
                    fresh ? "por constante" : "preguntado a la textura");
      }
    }
    /*
     * The early depth test also changes the pipeline (a different module), so it is decided once per
     * frame, like the PCF and the 1/size.
     */
    {
      const int32_t alternar = REXCVAR_GET(nfsmw_nativo_z_temprana_alternar_s);
      bool fresh = REXCVAR_GET(nfsmw_nativo_z_temprana);
      if (alternar > 0) {
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - toggle_z_start_)
                                  .count();
        fresh = (secs / alternar) % 2 == 1;
      }
      if (fresh != z_early_ || !tracked_toggle_z_) {
        z_early_ = fresh;
        tracked_toggle_z_ = true;
        REXLOG_INFO("[nativo] C6 prueba de profundidad: {} (fotograma {})",
                    fresh ? "ANTES de sombrear donde no se escribe Z (Z temprana)"
                          : "despues de sombrear, como antes",
                    frame_);
      }
    }
    skip_invisible_ = REXCVAR_GET(nfsmw_nativo_saltar_invisibles);
    // The deferred sky, also once per frame. If it is switched off mid-frame with one already deferred, the
    // pending one is still emitted through its usual path.
    {
      const bool fresh = REXCVAR_GET(nfsmw_nativo_cielo_aplazado);
      if (fresh != sky_deferred_ || !sky_tracked_) {
        sky_deferred_ = fresh;
        sky_tracked_ = true;
        REXLOG_INFO("[nativo] C6 cielo: {} (fotograma {})",
                    fresh ? "APLAZADO hasta despues de los opacos del pase"
                          : "en su sitio original (el primero, sobre el Z vacio)",
                    frame_);
      }
    }
    EarlyZReport();
    DirectPipelinesReport();
    DetailsReport();
    ZeroBaseReport();
    ControlSet4();  // set 4 by differences in NVK (cvar, alternation and guard)
    Set4Report();
    NvkDrawControl();  // the draw path in NVK (measurement, improvements and guards)
    NvkDrawReport();
    CanonicalKeyReport();  // Dynamic state, phase 0a
    PipelineBetweenPassesReport();  // Dynamic state, phase 0b
    DynamicStateReport();  // dynamic state phases 1 and 2
    BindingsReport();  // texture bind thread, every 10 s
    FingerprintsReport();  // texture hash thread, every 10 s
    ReuseReport();  // measurement only: new textures with a live one's content, every 10 s
    EvictTexturesIfNeeded();
    textures_pool_.PerFrame(frame_);  // grows with headroom, never exactly on demand
    // Test nfsmw_nativo_constantes_ubo_alternar_s. Switched here because the new epoch forces all constants
    // to be allocated again: nothing allocated in one mode is reused in the other.
    if (alternar_ubo_s_ > 0) {
      const auto secs =
          std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - ubo_start_).count();
      const bool ubo = (secs / alternar_ubo_s_) % 2 == 1;
      if (ubo != use_ubo_) {
        use_ubo_ = ubo;
        // With UBOs no push constants are recorded, so when going back to pointers the tracked state is
        // stale.
        std::memset(recorded_push_, 0, sizeof(recorded_push_));
        REXLOG_INFO("[nativo] prueba de constantes: {} (fotograma {})", ubo ? "por UBO" : "por puntero", frame_);
      }
    }
    ++sends_;  // "C6 constantes por UBO" report
    if (use_ubo_) {
      ++ubo_sends_;
    }
    // What the prewarm thread actually compiles also goes into the cache and has to be saved.
    if (const uint32_t compilados = prewarmed_compiled_.load(std::memory_order_relaxed);
        compilados != prewarmed_counted_) {
      pipelines_without_save_ += compilados - prewarmed_counted_;
      prewarmed_counted_ = compilados;
    }
    // The pipeline cache is saved after 64 new pipelines, or after a minute with any new one: on the
    // Switch, exiting does not always reach the destructor.
    if (pipelines_without_save_ &&
        (pipelines_without_save_ >= 64 ||
         std::chrono::steady_clock::now() - saved_cache_ >= std::chrono::seconds(60))) {
      SaveCachePipelines();
    }
  }

  void InvalidateTextures() override { ++textures_generation_; }

  // Only the cache entries that use a view of those images. The views stay valid (they go with their
  // image); what has to be redone is which slot each fetch constant gets.
  void InvalidateImages(VkImage a, VkImage b) override {
    std::array<std::pair<uint32_t, uint32_t>, 64> ranuras{};
    size_t n = 0;
    for (VkImage img : {a, b}) {
      if (img == VK_NULL_HANDLE || (img == b && a == b)) {
        continue;
      }
      const auto it = views_per_image_.find(img);
      if (it == views_per_image_.end()) {
        continue;
      }
      for (uint64_t key : it->second) {
        const auto v = views_.find(key);
        if (v != views_.end() && v->second.img == img && n < ranuras.size()) {
          ranuras[n++] = {v->second.heap, v->second.ranura};
        }
      }
    }
    if (!n) {
      return;
    }
    const auto affected = [&](const CacheSampler& c) {
      for (size_t i = 0; i < n; ++i) {
        if (ranuras[i].first == c.heap && ranuras[i].second == c.ranura) {
          return true;
        }
      }
      return false;
    };
    for (CacheSampler& c : cache_samplers_) {
      if (affected(c)) {
        c.frame = UINT64_MAX;
        c.valid_until = 0;
      }
    }
    for (CacheSampler& c : cache_fetch_) {
      if (affected(c)) {
        c.frame = UINT64_MAX;
        c.valid_until = 0;
      }
    }
  }

  void OcclusionOpen(bool open) override {
    if (!open && occlusion_query_ != UINT32_MAX) {
      // Still inside the pass of the last counted draw: TerminarPase would have closed it already.
      contexto_->FinishOcclusionQuery(occlusion_query_);
      occlusion_query_ = UINT32_MAX;
    }
    open_occlusion_ = open;
  }

  uint64_t Drawn() const override { return drawn_; }

  // nfsmw_nativo_texturas_mb_max: with the texture cache above the limit, evict the textures unused for
  // the longest until it drops to 75 %, at most once every 60 frames.
  // It is safe without waiting for the GPU: a texture not prepared for 120 frames cannot be referenced by
  // any pending submission. The slot caches are valid for at most 32 frames after preparing, and only the
  // previous submission can still be on the GPU (Grabar waits for the slot before reusing it). The heap
  // slots are updated with UPDATE_AFTER_BIND, and the new generation invalidates the caches.
  static constexpr uint64_t kFramesWithoutUseForRelease = 120;
  /*
   * How much GPU memory there is and how much is left.
   *
   * The cache limit was picked by eye twice (384 MB and then 192) because this data was not available.
   * With VK_EXT_memory_budget the driver reports the budget and the usage; without it, at least the heap
   * sizes are visible. It goes with the cache report, every 256 new textures.
   */
  void TrackGpuMemory() {
    const auto* instancia = vk_device_->vulkan_instance();
    if (!instancia) {
      return;
    }
    const auto& ifn = instancia->functions();
    const VkPhysicalDevice phys = vk_device_->physical_device();
    const bool has_budget = vk_device_->extensions().ext_EXT_memory_budget &&
                                 ifn.vkGetPhysicalDeviceMemoryProperties2 != nullptr;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
    budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 properties2{};
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    properties2.pNext = has_budget ? &budget : nullptr;
    if (has_budget) {
      ifn.vkGetPhysicalDeviceMemoryProperties2(phys, &properties2);
    } else {
      ifn.vkGetPhysicalDeviceMemoryProperties(phys, &properties2.memoryProperties);
    }
    const auto& mp = properties2.memoryProperties;
    std::string texto;
    for (uint32_t i = 0; i < mp.memoryHeapCount; ++i) {
      if (!texto.empty()) {
        texto += " | ";
      }
      const bool local = (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
      if (has_budget) {
        texto += fmt::format("monton {}{}: {} MB usados de {} MB presupuestados (tamano {} MB)", i,
                             local ? " (GPU)" : "", budget.heapUsage[i] >> 20,
                             budget.heapBudget[i] >> 20, mp.memoryHeaps[i].size >> 20);
      } else {
        texto += fmt::format("monton {}{}: {} MB", i, local ? " (GPU)" : "",
                             mp.memoryHeaps[i].size >> 20);
      }
    }
    REXLOG_INFO("[nativo] C3 memoria de la GPU: {}{}; la cache de texturas lleva {} MB de {} MB", texto,
                has_budget ? "" : " (sin VK_EXT_memory_budget: solo tamanos)", textures_bytes_ >> 20,
                textures_max_mb_);
    // Without this line there is no way to know whether the pool is working or fragmenting. A vegetation
    // counter once existed and was never printed.
    if (textures_pool_.Active()) {
      REXLOG_INFO("[nativo] C3 {}", textures_pool_.Summary());
    }
  }

  /*
   * Without stutters.
   *
   * The first version waited until the limit was exceeded and evicted 25 % at once. In a race that is a
   * stutter, and a stutter every few minutes is no better than running out of memory every fourteen.
   *
   * Now a few are evicted per frame as soon as the cache nears the limit, and only cold textures: those
   * unused for 120 frames, i.e. two to four seconds. They are not in the working set, so they do not have
   * to be uploaded again right away and there is no thrashing. If there are not enough cold ones, nothing
   * is forced: the cache is allowed to grow, since going a little over is better than uploading and
   * evicting the same thing.
   */
  static constexpr uint32_t kReleasePerFrame = 4;

  void EvictTexturesIfNeeded() {
    const uint64_t limit = uint64_t(std::max(textures_max_mb_, 0)) << 20;
    if (!limit) {
      return;
    }
    // Little by little, from 75 % of the limit. That is what avoids the stutter.
    if (textures_bytes_ > limit / 4 * 3) {
      ReleaseColdFewSome();
    }
    // Safety net: if it still goes over the limit (because almost everything is hot), one batch every 60
    // frames. With the above working, this should almost never trigger.
    if (textures_bytes_ > limit && frame_ >= eviction_attempt_ + 60) {
      eviction_attempt_ = frame_;
      ReleaseTextures(limit / 4 * 3, kFramesWithoutUseForRelease, "por encima del limite");
    }
  }

  // The kSoltarPorFotograma oldest cold ones. Without sorting the whole cache: they are picked on the fly.
  void ReleaseColdFewSome() {
    std::array<std::pair<uint64_t, uint64_t>, kReleasePerFrame> chosen_set{};  // (frame, key)
    uint32_t how_many = 0;
    for (const auto& [key, texture] : textures_) {
      // Nor those with a bind in flight, nor those owned by the hash thread.
      if (texture.img.img == VK_NULL_HANDLE || texture.do_upload || texture.in_flight || texture.job_fingerprint ||
          texture.frame == UINT64_MAX ||
          texture.frame + kFramesWithoutUseForRelease >= frame_) {
        continue;
      }
      if (how_many < kReleasePerFrame) {
        chosen_set[how_many++] = {texture.frame, key};
        continue;
      }
      // Replaces the most recent of the chosen ones, if this one is older.
      uint32_t worst = 0;
      for (uint32_t i = 1; i < how_many; ++i) {
        if (chosen_set[i].first > chosen_set[worst].first) {
          worst = i;
        }
      }
      if (texture.frame < chosen_set[worst].first) {
        chosen_set[worst] = {texture.frame, key};
      }
    }
    if (how_many == 0) {
      return;  // all hot: let it grow rather than thrash
    }
    std::unordered_set<VkImage> images;
    for (uint32_t i = 0; i < how_many; ++i) {
      auto it = textures_.find(chosen_set[i].second);
      if (it == textures_.end()) {
        continue;
      }
      images.insert(it->second.img.img);
      textures_bytes_ -= std::min(textures_bytes_, it->second.bytes);
    }
    ReleaseImages(images);
    released_textures_ += images.size();
    few_released_to_few_ += images.size();
    if (frame_ >= trickle_warning_ + 600) {  // one warning every 600 frames, not one per texture
      trickle_warning_ = frame_;
      REXLOG_INFO("[nativo] C3: cache de texturas cerca del limite: se sueltan frias a goteo ({} en total); "
                  "quedan {} texturas y {} MB de {} MB",
                  few_released_to_few_, textures_.size(), textures_bytes_ >> 20, textures_max_mb_);
    }
  }

  /*
   * The last resort, when the GPU has no more memory to give.
   *
   * Once nvMapCreate started failing even for 64 KB there was no way back: the driver ended up
   * quarantining ranges and the screen went black with the audio still playing. Before it gets to that,
   * half the texture cache is released.
   *
   * Normal eviction only touches what has been unused for 120 frames, because images are destroyed at
   * once and the GPU could still be reading them. Here that margin is not needed: it waits for the GPU to
   * finish everything in flight, and then none is in use. It costs a one-frame stutter; crashing costs
   * the game.
   */
  bool ReleaseTexturesOnMemoryShortage() override {
    if (textures_.empty() || textures_bytes_ == 0 || releasing_per_memory_missing_) {
      return false;  // the guard keeps an allocation inside the cleanup itself from re-entering here
    }
    releasing_per_memory_missing_ = true;
    bool algo = false;
    if (contexto_ && contexto_->WaitGpuIdle()) {
      algo = ReleaseTextures(textures_bytes_ / 2, 0, "SIN MEMORIA EN LA GPU") > 0;
    }
    releasing_per_memory_missing_ = false;
    return algo;
  }

  // Actually retires a group of images. First the views and their slots (pointed at the empty texture),
  // then the images. Used by both the gradual and the batch eviction.
  void ReleaseImages(const std::unordered_set<VkImage>& images) {
    ++textures_generation_;
    for (auto it = views_.begin(); it != views_.end();) {
      if (!images.count(it->second.img)) {
        ++it;
        continue;
      }
      WriteImage(it->second.heap, it->second.ranura, empty_[it->second.heap].view);
      heaps_[it->second.heap].free_items.push_back(it->second.ranura);
      dfn_.vkDestroyImageView(device_, it->second.view, nullptr);
      views_per_image_.erase(it->second.img);  // all views of that image go
      it = views_.erase(it);
    }
    for (auto it = textures_.begin(); it != textures_.end();) {
      if (!images.count(it->second.img.img)) {
        ++it;
        continue;
      }
      RemoveTextureContent(it->second, it->first);  // measurement only: nfsmw_nativo_diag_reutilizar
      DestroyImage(it->second.img);
      it = textures_.erase(it);
    }
  }

  // Evicts unused textures until below `objetivo`. Returns the bytes freed.
  uint64_t ReleaseTextures(uint64_t goal, uint64_t min_age, const char* reason) {
    std::vector<std::pair<uint64_t, uint64_t>> candidate_items;  // (last frame it was prepared, key)
    for (const auto& [key, texture] : textures_) {
      // Nor those with a bind in flight (their image has no memory yet), nor those owned by the hash thread.
      if (texture.img.img != VK_NULL_HANDLE && !texture.do_upload && !texture.in_flight && !texture.job_fingerprint &&
          texture.frame != UINT64_MAX &&
          texture.frame + min_age < frame_) {
        candidate_items.emplace_back(texture.frame, key);
      }
    }
    std::sort(candidate_items.begin(), candidate_items.end());
    const uint64_t before = textures_bytes_;
    std::unordered_set<VkImage> images;
    for (const auto& [is_last, key] : candidate_items) {
      if (textures_bytes_ <= goal) {
        break;
      }
      auto it = textures_.find(key);
      images.insert(it->second.img.img);
      textures_bytes_ -= std::min(textures_bytes_, it->second.bytes);
    }
    if (images.empty()) {
      return 0;
    }
    ReleaseImages(images);
    released_textures_ += images.size();
    REXLOG_INFO("[nativo] C3: cache de texturas {} ({} MB): fuera {} sin usar desde hace mas de {} fotogramas "
                "({} MB); quedan {} texturas y {} MB ({} soltadas en total)",
                reason, before >> 20, images.size(), min_age,
                (before - textures_bytes_) >> 20, textures_.size(), textures_bytes_ >> 20, released_textures_);
    return before - textures_bytes_;
  }

  // ZCULL: clears a depth image by opening a pass with loadOp = CLEAR. Needed for images created without
  // TRANSFER_DST (the only ones the driver gives a ZCULL plane), because vkCmdClearDepthStencilImage
  // requires that usage (VUID-vkCmdClearDepthStencilImage-pRanges-02660). It costs no extra GPU time: it
  // is exactly how NVK implements that command internally (nvk_cmd_clear.c, clear_image opens a pass with
  // loadOp = CLEAR).
  bool ClearDepthInPass(VkCommandBuffer commands, const NativeImage& img,
                               float depth_value, uint32_t stencil) override {
    if (commands == VK_NULL_HANDLE || img.view == VK_NULL_HANDLE || !img.width_px ||
        !img.height_px) {
      return false;
    }
    FinishPass();  // a pass cannot be opened inside another
    uint32_t formats[5] = {0, 0, 0, 0, uint32_t(img.format_id)};
    const VkRenderPass pass = PassOf(formats, kClearLoad);
    if (pass == VK_NULL_HANDLE) {
      return false;
    }
    std::array<VkImageView, 5> views{};
    views[4] = img.view;
    const VkFramebuffer framebuffer = FramebufferDe(pass, views, img.width_px, img.height_px);
    if (framebuffer == VK_NULL_HANDLE) {
      return false;
    }
    VkClearValue clear_info{};
    clear_info.depthStencil = {depth_value, stencil};
    VkRenderPassBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    start.renderPass = pass;
    start.framebuffer = framebuffer;
    start.renderArea.extent = {img.width_px, img.height_px};
    start.clearValueCount = 1;
    start.pClearValues = &clear_info;
    dfn_.vkCmdBeginRenderPass(commands, &start, VK_SUBPASS_CONTENTS_INLINE);
    dfn_.vkCmdEndRenderPass(commands);
    ++depth_clears_in_pass_;
    return true;
  }

  // nfsmw_nativo_borrar_area_util. Clears only the `area` rectangle of a color image with a
  // loadOp = CLEAR pass: what NVK does internally for vkCmdClearColorImage (nvk_cmd_clear.c), over that
  // rectangle. What lies outside is not touched (Vulkan only loads and stores the renderArea).
  bool ClearColorInPass(VkCommandBuffer commands, const NativeImage& img, const VkClearColorValue& color,
                         const VkRect2D& area) override {
    if (commands == VK_NULL_HANDLE || img.view == VK_NULL_HANDLE || !area.extent.width || !area.extent.height ||
        area.offset.x < 0 || area.offset.y < 0 || uint32_t(area.offset.x) + area.extent.width > img.width_px ||
        uint32_t(area.offset.y) + area.extent.height > img.height_px) {
      return false;
    }
    FinishPass();  // a pass cannot be opened inside another
    uint32_t formats[5] = {uint32_t(img.format_id), 0, 0, 0, 0};
    const VkRenderPass pass = PassOf(formats, kClearLoad);
    if (pass == VK_NULL_HANDLE) {
      return false;
    }
    std::array<VkImageView, 5> views{};
    views[0] = img.view;
    const VkFramebuffer framebuffer = FramebufferDe(pass, views, img.width_px, img.height_px);
    if (framebuffer == VK_NULL_HANDLE) {
      return false;
    }
    VkClearValue clear_info{};
    clear_info.color = color;
    VkRenderPassBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    start.renderPass = pass;
    start.framebuffer = framebuffer;
    start.renderArea = area;
    start.clearValueCount = 1;
    start.pClearValues = &clear_info;
    dfn_.vkCmdBeginRenderPass(commands, &start, VK_SUBPASS_CONTENTS_INLINE);
    dfn_.vkCmdEndRenderPass(commands);
    return true;
  }

  // nfsmw_nativo_framebuffers_olvidan_vistas. C2 is about to destroy this view (Destruir, with the GPU
  // idle for that image): the FramebufferDe framebuffers that use it are destroyed and removed from the
  // cache. It walks the created framebuffers (dozens); never during a draw.
  // Self-checking guard: the cache and its view list must stay in step (only FramebufferDe inserts) and
  // no pass may be open (whoever destroys the view has already submitted the work). Otherwise: DIFERENCIA
  // in the log and, for the rest of the session, nothing is destroyed at runtime: the affected
  // framebuffers (or the whole cache, if they are out of step) are retired and destroyed at shutdown.
  // Retiring is never worse than the previous behaviour.
  void ForgetView(VkImageView view) override {
    if (view == VK_NULL_HANDLE || !REXCVAR_GET(nfsmw_nativo_framebuffers_olvidan_vistas)) {
      return;
    }
    const bool a_la_par = views_framebuffers_.size() == framebuffers_.size();
    if (!fb_retire_ && (active_pass_ || !a_la_par)) {
      fb_retire_ = true;
      REXLOG_ERROR("[nativo] C6 framebuffers (build 184): DIFERENCIA al destruir la vista {:016X}: {}. Para el resto "
                   "de la sesion los framebuffers afectados se retiran sin destruirlos (se destruyen al cerrar)",
                   uint64_t(reinterpret_cast<uintptr_t>(view)),
                   active_pass_ ? "hay un pase abierto (quien destruye la vista no ha enviado el trabajo)"
                                : "la cache y su lista de vistas no van a la par (alguien crea framebuffers por otro "
                                  "camino)");
    }
    uint32_t outside = 0;
    if (!a_la_par) {
      // Unknown which ones use the view: drop them all (they are rebuilt on request).
      for (auto& [key, framebuffer] : framebuffers_) {
        fb_retired_.push_back(framebuffer);
      }
      outside = uint32_t(framebuffers_.size());
      framebuffers_.clear();
      views_framebuffers_.clear();
    } else {
      for (auto it = views_framebuffers_.begin(); it != views_framebuffers_.end();) {
        if (std::find(it->second.begin(), it->second.end(), view) == it->second.end()) {
          ++it;
          continue;
        }
        const auto fb = framebuffers_.find(it->first);
        if (fb != framebuffers_.end()) {
          if (fb_retire_) {
            fb_retired_.push_back(fb->second);
          } else {
            dfn_.vkDestroyFramebuffer(device_, fb->second, nullptr);
          }
          framebuffers_.erase(fb);
          ++outside;
        }
        it = views_framebuffers_.erase(it);
      }
    }
    ++fb_forgotten_views_;
    fb_forgotten_ += outside;
    if (fb_forgotten_views_ <= 16 || (fb_forgotten_views_ & 63) == 0) {
      NFSMW_RING_REPORT("[nativo] C6 framebuffers (build 184): vista {:016X} destruida: {} framebuffers {} con "
                           "ella; quedan {} en la cache ({} vistas y {} framebuffers desde el arranque; {} "
                           "retirados)",
                           uint64_t(reinterpret_cast<uintptr_t>(view)), outside,
                           fb_retire_ ? "retirados (sin destruir)" : "destruidos", framebuffers_.size(),
                           fb_forgotten_views_, fb_forgotten_, fb_retired_.size());
    }
  }

  void ForgetImage(VkImage img) override {
    ++textures_generation_;  // the sampler cache could point to a retired view
    const auto index = views_per_image_.find(img);  // without walking vistas_
    if (index == views_per_image_.end()) {
      return;
    }
    for (uint64_t key : index->second) {
      const auto it = views_.find(key);
      if (it == views_.end() || it->second.img != img) {
        continue;
      }
      WriteImage(it->second.heap, it->second.ranura, empty_[it->second.heap].view);
      heaps_[it->second.heap].free_items.push_back(it->second.ranura);
      dfn_.vkDestroyImageView(device_, it->second.view, nullptr);
      views_.erase(it);
    }
    views_per_image_.erase(index);
  }

  DrawStats Stats() const override {
    DrawStats e;
    e.drawn = drawn_;
    e.rejected = rejected_;
    e.pipelines = pipelines_.size();
    e.textures = textures_.size();
    e.texture_uploads = texture_uploads_;
    e.uploaded_megas = megas_bytes_ >> 20;
    e.textures_megas = textures_bytes_ >> 20;
    e.ms_pipelines = ns_pipelines_ / 1000000;
    e.passes = started_passes_;
    e.full_sends = full_sends_;
    e.full_sends_ns = full_sends_ns_;
    e.bytes_vertices = bytes_vertices_;
    e.hits_dedupe = dedupe_.hits();
    e.dedupe_bytes = dedupe_.saved_bytes();
    e.collisions_dedupe = dedupe_.collisions();
    e.bytes_indices = uploaded_indices_bytes_;
    e.samplers = prepared_samplers_;
    e.samplers_cache = samplers_cache_ + samplers_cache_fetch_;
    e.passes_ns = passes_ns_;
    e.ns_vertices = ns_vertices_;
    e.computed_entries = computed_entries_;
    e.entries_ns = entries_ns_;
    e.reused_entries = reused_entries_ + hits_cache_entries_;
    e.passes_per_generation = passes_per_generation_;
    e.passes_per_target = passes_per_target_;
    e.resumed_passes = resumed_passes_;
    e.ns_render_pass = ns_render_pass_;
    e.passes_texels = passes_texels_;
    e.texels_per_category = texels_per_category_;
    e.draws_per_category = draws_per_category_;
    e.useless_ps_draws = useless_ps_draws_;
    e.required_ps_draws = required_ps_draws_;
    e.shadows_active_alpha = shadows_active_alpha_;
    e.shadows_alpha_disabled = shadows_alpha_disabled_;
    e.triangles_per_category = triangles_per_category_;
    e.passes_per_category = passes_per_category_;
    e.sends = sends_;
    e.ubo_sends = ubo_sends_;
    e.peeked_shared = peeked_shared_;
    e.changed_shared = changed_shared_;
    e.frame_repeated_bytes = frame_repeated_bytes_;
    e.previous_equal_bytes = previous_equal_bytes_;
    e.ns_hash_vertices = ns_hash_vertices_;
    e.causes.assign(causes_.begin(), causes_.end());
    e.stages_ns = stages_ns_;
    e.scene_with_discard = scene_with_discard_;
    e.scene_without_discard = scene_without_discard_;
    e.timed_drawn = timed_drawn_;
    e.vegetation_soon = soon_vegetation_draws_;
    std::sort(e.causes.begin(), e.causes.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    return e;
  }

 private:
  bool Reject(uint32_t cause, const char* texto) {
    ++rejected_;
    ++causes_[cause];
    last_cause_ = cause;
    Warn(cause, texto);
    return false;
  }

  void Warn(uint32_t cause, const char* texto) {
    if (warned_.insert(cause).second) {
      REXLOG_WARN("[nativo] C6: {} (causa {})", texto, cause);
    }
  }

  // --- Texture binds on a separate thread (nfsmw_nativo_texturas_enlace_hilo) ----------------------------------
  /*
   * Why. With the memory pool, creating a texture no longer asks the system for memory, but vkBindImageMemory
   * still makes two ioctls: reserving the plane's VA with its pte_kind and mapping the pool chunk into it
   * (nvk_image_plane_bind, nvk_image.c:1696-1710; vkCreateImage reserves no VA except for sparse images,
   * :1337-1350). All our textures are tiled with pte_kind GENERIC_16BX2 (nil/image.rs:439-440 and 876-918),
   * so both are always paid. In a race: 336 VA reservations in 95 ms and 336 mappings in 111 ms, ~0.6 ms of
   * wall time per texture with the ring asleep in the ioctl. Entering a new zone brings 33-45 textures in
   * one frame: 19-27 ms of stalled ring that did not show up in "texturas X ms".
   *
   * How. The ring does the CPU part (vkCreateImage, requirements, pool chunk, descriptor slot) and only
   * queues the vkBindImageMemory. The draw is recorded with its slot and SubirTextura leaves the data in the
   * upload buffer as usual. In AntesDeEnviar (RecogerEnlaces) it waits for whatever is missing, creates the
   * views, writes the descriptors (UPDATE_AFTER_BIND: legal until submission) and records the barrier and
   * the copy in that same upload buffer, which goes in the same vkQueueSubmit, ahead of the work. The GPU
   * receives exactly the same thing in the same submission.
   *
   * The driver. vkBindImageMemory from another thread is safe: the VA and the mapping are under
   * device->va_mutex (horizon/nouveau_horizon_vm.c:173 and 377), the mapping refcount under
   * memory_identity_mutex (nouveau_horizon_memory.c:1065), and everything else belongs to that image. The
   * pool is not touched from the thread: it stays single-threaded (native_texture_pool.h).
   *
   * Threads (see docs/platform-notes.md, Threads). Shared state is under enlaces_mutex_, and every decision
   * to sleep or wake is taken with the lock held; the thread only reads the image, memory and offset of its
   * request and stores the result under the lock. The ring's wait has a timeout and logs every 2 s.
   * Persistent thread (never detached), joined in the destructor before any image is destroyed. Priority
   * 0x2D: a host service, above the guest.
   *
   * Self-checking guard. Observing phase: the first kEnlacesAComprobar wait for their bind right after
   * queuing it (same order as before: only the thread doing the ioctl changes). If all succeed, applying
   * phase. In any phase, a failed bind, a wait of more than 2 s or a deferred copy that can no longer go in
   * its upload buffer: REXLOG_ERROR with the DIFERENCIA and off for the session (the usual path returns;
   * whatever was in flight finishes through the fallback path). And if for 3 reports in a row the ring
   * waits for the thread longer than the thread takes to bind, it does not pay off: it switches off too.
   */
  void DecideBindings() {
    const bool request = REXCVAR_GET(nfsmw_nativo_texturas_enlace_hilo);
    priority_bindings_ = std::clamp<int32_t>(REXCVAR_GET(nfsmw_nativo_texturas_enlace_hilo_prioridad), 0x2C, 0x3B);
    phase_bindings_ = request ? kPeekingBindings : kBindingsDisabled;
    REXLOG_INFO("[nativo] C3: enlace de las texturas nuevas en un hilo aparte (nfsmw_nativo_texturas_enlace_hilo) = {}",
                request ? fmt::format("SI, prioridad {:#x}; fase MIRANDO: las {} primeras esperan su enlace al momento "
                                     "(solo texturas del pool, que esta {})",
                                     priority_bindings_, kBindingsToCheck,
                                     textures_pool_.Active() ? "encendido" : "APAGADO: no se usara")
                       : std::string("no, en el hilo del anillo como siempre"));
  }

  void DisableBindings(const std::string& reason) {
    if (phase_bindings_ == kBindingsDisabled) {
      return;
    }
    phase_bindings_ = kBindingsDisabled;
    REXLOG_ERROR("[nativo] C3: hilo de enlaces de texturas APAGADO para el resto de la sesion: {}. Las texturas nuevas "
                 "vuelven a crearse enteras en el hilo del anillo ({} enlazadas en el hilo, {} fallidas, {} vistas y {} "
                 "copias aplazadas)",
                 reason, thread_bindings_total_, failed_bindings_, deferred_views_, deferred_copies_);
  }

  // Queues the vkBindImageMemory of an image. false = not queued (queue full or no thread): the usual path.
  bool EnqueueBinding(VkImage img, VkDeviceMemory mem, VkDeviceSize offset, uint64_t& ticket) {
    if (!thread_bindings_.joinable()) {
      try {
        thread_bindings_ = std::thread([this] { BindLoop(); });
      } catch (const std::system_error& error) {
        DisableBindings(fmt::format("DIFERENCIA: no se pudo crear el hilo ({})", error.what()));
        return false;
      }
    }
    bool warn = false;
    {
      std::lock_guard<std::mutex> mutex_lock(mutex_bindings_);
      if (requests_bindings_ - collected_bindings_ >= kBindingsQueue) {
        ++full_queue_bindings_;
        return false;
      }
      BindingRequest& req_info = bindings_queue_[requests_bindings_ & (kBindingsQueue - 1)];
      req_info.img = img;
      req_info.mem = mem;
      req_info.offset = offset;
      req_info.result = VK_NOT_READY;
      req_info.ns = 0;
      ticket = requests_bindings_++;
      warn = sleeping_bindings_;  // decided under the lock: if it sleeps, it is inside its wait with the predicate
    }
    if (warn) {
      cv_bindings_.notify_one();
    }
    return true;
  }

  // The thread: the requests' vkBindImageMemory calls, in order. It sleeps on a decision taken under the
  // lock; it never spins. It does not write to the log (the ring records the figures in InformeEnlaces).
  void BindLoop() {
    rex::thread::set_current_thread_name("NFSMW enlaces de texturas");
#if REX_PLATFORM_SWITCH
    const bool priority_ok = RexSwitchSetCurrentThreadPriorityOk(int(priority_bindings_));
#else
    const bool priority_ok = true;
#endif
    std::unique_lock<std::mutex> mutex_lock(mutex_bindings_);
    priority_bindings_ok_ = priority_ok;
    for (;;) {
      if (done_bindings_ == requests_bindings_) {
        if (stop_bindings_) {
          return;  // stop, nothing pending
        }
        if (waiting_bindings_) {
          cv_done_bindings_.notify_one();
        }
        sleeping_bindings_ = true;
        cv_bindings_.wait(mutex_lock, [this] { return stop_bindings_ || done_bindings_ != requests_bindings_; });
        sleeping_bindings_ = false;
        continue;
      }
      // The slot is not reused until the ring collects it (enlaces_recogidos_): it stays ours without the lock.
      BindingRequest& req_info = bindings_queue_[done_bindings_ & (kBindingsQueue - 1)];
      const VkImage img = req_info.img;
      const VkDeviceMemory mem = req_info.mem;
      const VkDeviceSize offset = req_info.offset;
      mutex_lock.unlock();
      const auto before = std::chrono::steady_clock::now();
      const VkResult result = dfn_.vkBindImageMemory(device_, img, mem, offset);
      const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - before)
                                       .count());
      mutex_lock.lock();
      req_info.result = result;
      req_info.ns = ns;
      thread_bindings_ns_ += ns;
      worst_binding_ns_ = std::max(worst_binding_ns_, ns);
      ++done_bindings_;
      if (waiting_bindings_) {
        cv_done_bindings_.notify_one();
      }
    }
  }

  void StopBindings() {
    if (!thread_bindings_.joinable()) {
      return;
    }
    {
      std::lock_guard<std::mutex> mutex_lock(mutex_bindings_);
      stop_bindings_ = true;
    }
    cv_bindings_.notify_one();
    thread_bindings_.join();  // first finishes every queued request
    REXLOG_INFO("[nativo] C3: hilo de enlaces de texturas parado ({} enlazadas en el hilo, {} fallidas, {} vistas y {} "
                "copias aplazadas, {} ranuras perdidas)",
                thread_bindings_total_, failed_bindings_, deferred_views_, deferred_copies_, lost_slots_);
  }

  // The ring waits until the thread has done the requests before `objetivo`. With a timeout and a log line
  // every 2 s.
  void WaitBindings(uint64_t goal) {
    std::unique_lock<std::mutex> mutex_lock(mutex_bindings_);
    if (done_bindings_ >= goal) {
      return;
    }
    const auto before = std::chrono::steady_clock::now();
    waiting_bindings_ = true;
    if (sleeping_bindings_) {
      cv_bindings_.notify_one();  // should not be needed (it has work); in case a wake-up was lost
    }
    int secs = 0;
    while (!cv_done_bindings_.wait_for(mutex_lock, std::chrono::seconds(2),
                                        [this, goal] { return done_bindings_ >= goal; })) {
      secs += 2;
      stuck_bindings_ = true;
      REXLOG_ERROR("[nativo] C3: el hilo del anillo lleva {} s esperando los enlaces de texturas: objetivo {}, hechos "
                   "{}, pedidos {}; el hilo esta {}",
                   secs, goal, done_bindings_, requests_bindings_,
                   sleeping_bindings_ ? "DORMIDO (aviso perdido)" : "despierto (un ioctl que no vuelve)");
    }
    waiting_bindings_ = false;
    mutex_lock.unlock();
    const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - before)
                                     .count());
    bindings_wait_total_ns_ += ns;
    bindings_wait_report_ns_ += ns;
    worst_bindings_wait_ns_ = std::max(worst_bindings_wait_ns_, ns);
    ++bindings_waits_report_;
    nfsmw::waits::g_bindings_waiting_ns.fetch_add(ns, std::memory_order_relaxed);
  }

  /*
   * The CPU part of CrearTextura (image, requirements, pool chunk) here, and its vkBindImageMemory to the
   * thread. false = nothing was touched and the caller continues through CrearTextura: thread off, pool off
   * or full, queue full, or (in the observing phase) a failed bind whose fallback path also failed.
   */
  bool CreateTextureInThread(Texture& texture, VkFormat format_id, uint32_t width_px, uint32_t height_px, uint32_t layer_count,
                          uint32_t background, uint32_t levels) {
    if (phase_bindings_ == kBindingsWithoutDecide) {
      DecideBindings();
    }
    // With the out-of-memory test enabled, everything goes through CrearTextura: that test lives there.
    if (phase_bindings_ == kBindingsDisabled || !textures_pool_.Active() || test_without_memory_every_ > 0) {
      return false;
    }
    const VkImageCreateInfo info = TextureImageInfo(format_id, width_px, height_px, layer_count, background, levels);
    VkImage img = VK_NULL_HANDLE;
    if (dfn_.vkCreateImage(device_, &info, nullptr, &img) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements req{};
    dfn_.vkGetImageMemoryRequirements(device_, img, &req);
    VkDeviceMemory block = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    uint32_t block_id = 0xFFFFFFFFu;
    if (!textures_pool_.Reserve(req, block, offset, block_id)) {
      dfn_.vkDestroyImage(device_, img, nullptr);
      return false;  // no room in the pool: CrearTextura, which can take the dedicated path
    }
    uint64_t ticket = 0;
    if (!EnqueueBinding(img, block, offset, ticket)) {
      dfn_.vkDestroyImage(device_, img, nullptr);  // before the chunk, as the pool requires (not bound yet)
      textures_pool_.Dispose(block_id);
      return false;
    }
    texture.img.img = img;
    texture.img.mem = VK_NULL_HANDLE;  // from the pool: not freed on its own
    texture.img.block_pool = block_id;
    texture.img.width_px = width_px;
    texture.img.height_px = height_px;
    texture.img.format_id = format_id;
    texture.img.prepared = false;
    TextureInFlight flight;
    flight.texture = &texture;
    flight.ticket = ticket;
    flight.img = img;
    flight.format_id = format_id;
    flight.width_px = width_px;
    flight.height_px = height_px;
    flight.layer_count = layer_count;
    flight.background = background;
    flight.levels = levels;
    in_flight_.push_back(flight);
    texture.in_flight = uint32_t(in_flight_.size());
    images_in_flight_.insert(img);
    if (phase_bindings_ == kPeekingBindings) {
      // Observing phase: wait now, before RanuraVista and SubirTextura: everything stays as without the thread.
      CollectBindings(true);
      if (texture.img.img == VK_NULL_HANDLE) {
        return false;  // neither the thread nor the fallback (already logged): CrearTextura, with its emergency path
      }
      if (++checked_bindings_ >= kBindingsToCheck && phase_bindings_ == kPeekingBindings) {
        phase_bindings_ = kApplyingBindings;
        uint64_t thread_ns = 0;
        bool priority_ok = true;
        {
          std::lock_guard<std::mutex> mutex_lock(mutex_bindings_);
          thread_ns = thread_bindings_ns_;
          priority_ok = priority_bindings_ok_;
        }
        REXLOG_INFO("[nativo] C3: hilo de enlaces de texturas: {} enlaces con espera inmediata y ninguno fallido ({:.0f} "
                    "us de media dentro de vkBindImageMemory; el anillo espero {:.0f} us de media por textura). Pasa a "
                    "fase APLICANDO: la espera va antes de cada envio{}",
                    checked_bindings_, double(thread_ns) / 1e3 / double(checked_bindings_),
                    double(bindings_wait_total_ns_) / 1e3 / double(checked_bindings_),
                    priority_ok ? "" : " (el kernel NO acepto la prioridad del hilo)");
      }
    }
    return true;
  }

  // RanuraVista for an image with its bind in flight: the slot now, the view in RecogerEnlaces.
  uint32_t ViewSlotInFlight(uint64_t key, VkImage img, VkFormat format_id, uint32_t swizzle,
                              uint16_t swizzle_host, uint32_t heap) {
    View view;
    view.img = img;
    view.heap = heap;
    view.ranura = ReserveSlot(heap);
    if (!view.ranura) {
      Warn(35, "monton de texturas lleno");
      return 0;
    }
    views_.emplace(key, view);  // vista.vista stays VK_NULL_HANDLE until RecogerEnlaces
    views_per_image_[img].push_back(key);
    ViewInFlight deferred;
    deferred.key = key;
    deferred.img = img;
    deferred.format_id = format_id;
    deferred.swizzle = swizzle;
    deferred.swizzle_host = swizzle_host;
    deferred.heap = heap;
    deferred.ranura = view.ranura;
    views_in_flight_.push_back(deferred);
    ++deferred_views_;
    ++deferred_views_report_;
    return view.ranura;
  }

  // SubirTextura for a texture with its bind in flight: the data is already in the upload buffer; record
  // where.
  bool DeferCopy(Texture& texture, VkDeviceSize offset, VkCommandBuffer upload) {
    const size_t index = size_t(texture.in_flight) - 1;
    if (index >= in_flight_.size() || in_flight_[index].texture != &texture || in_flight_[index].copy_info) {
      DisableBindings(fmt::format("DIFERENCIA: indice de textura en vuelo incoherente ({} de {})", index,
                                in_flight_.size()));
      return false;
    }
    TextureInFlight& flight = in_flight_[index];
    flight.copy_info = true;
    flight.offset_copy = offset;
    flight.epoch_copy = upload_epoch_;
    flight.commands_copy = upload;
    ++deferred_copies_;
    ++deferred_copies_report_;
    return true;
  }

  // The deferred views of an image that had to be recreated: new key (it goes with the image) and its list.
  void MoveViewsInFlight(VkImage old, VkImage incoming) {
    if (old == incoming) {
      return;
    }
    std::vector<uint64_t> keys;
    for (ViewInFlight& deferred : views_in_flight_) {
      if (deferred.img != old) {
        continue;
      }
      const auto it = views_.find(deferred.key);
      if (it == views_.end()) {
        continue;
      }
      View view = it->second;
      views_.erase(it);
      view.img = incoming;
      deferred.img = incoming;
      if (incoming != VK_NULL_HANDLE) {
        deferred.key = ViewKey(incoming, deferred.swizzle, deferred.swizzle_host, deferred.heap);
      }
      if (!views_.emplace(deferred.key, view).second) {
        deferred.key = 0;  // key collision: RecogerEnlaces puts the empty one in its slot
        continue;
      }
      keys.push_back(deferred.key);
    }
    views_per_image_.erase(old);  // an image in flight only has deferred views
    if (incoming != VK_NULL_HANDLE && !keys.empty()) {
      auto& item_list = views_per_image_[incoming];
      item_list.insert(item_list.end(), keys.begin(), keys.end());
    }
  }

  /*
   * Finishes everything the bind thread has. Ring thread only. `con_copias` = false outside AntesDeEnviar
   * (the upload buffer would no longer be the one holding that data): those textures are uploaded again at
   * their next check.
   */
  void CollectBindings(bool with_copies) {
    // No re-entry: if something in here ended up starting new work (UsarRanura), that call does nothing and
    // the outer one sees the upload buffer changed (DIFERENCIA and a full upload at the next check).
    if (in_flight_.empty() || bindings_collecting_) {
      return;
    }
    bindings_collecting_ = true;
    const auto start = std::chrono::steady_clock::now();
    const uint64_t wait_before = bindings_wait_total_ns_;
    const uint64_t goal = in_flight_.back().ticket + 1;
    WaitBindings(goal);
    if (stuck_bindings_) {
      stuck_bindings_ = false;
      DisableBindings("DIFERENCIA: una espera de mas de 2 s al hilo (ver las lineas de arriba)");
    }
    {
      std::lock_guard<std::mutex> mutex_lock(mutex_bindings_);
      for (TextureInFlight& flight : in_flight_) {
        flight.result = bindings_queue_[flight.ticket & (kBindingsQueue - 1)].result;
      }
      collected_bindings_ = goal;  // from here on its slots can be reused
    }
    // 1. Results, in order. If a bind failed, that image is dropped (no command references it yet) and the
    //    texture is created again through CrearTextura without the emergency path: releasing half the cache
    //    submits the work, and we are before that.
    for (TextureInFlight& flight : in_flight_) {
      Texture& texture = *flight.texture;
      images_in_flight_.erase(flight.img);
      texture.in_flight = 0;
      if (flight.result == VK_SUCCESS) {
        ++thread_bindings_total_;
        ++thread_bindings_report_;
        nfsmw::waits::g_thread_bound_textures.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      ++failed_bindings_;
      DisableBindings(fmt::format("DIFERENCIA: vkBindImageMemory devolvio {} en el hilo para una textura de {}x{} ({} "
                                "niveles, {} capas)",
                                int32_t(flight.result), flight.width_px, flight.height_px, flight.levels, flight.layer_count));
      dfn_.vkDestroyImage(device_, flight.img, nullptr);
      textures_pool_.Dispose(texture.img.block_pool);
      texture.img.img = VK_NULL_HANDLE;
      texture.img.block_pool = 0xFFFFFFFFu;
      if (!CreateTexture(texture.img, flight.format_id, flight.width_px, flight.height_px, flight.layer_count, flight.background,
                        flight.levels, false)) {
        // Not even then. The texture is left without an image: its draws in this submission see the empty one
        // (the same as when it cannot be created) and the next time it is used it is created in full, with the
        // emergency path.
        texture.img = NativeImage{};
        textures_bytes_ -= std::min(textures_bytes_, texture.bytes);
        texture.bytes = 0;
      }
      MoveViewsInFlight(flight.img, texture.img.img);
    }
    // 2. Deferred views: created and written to their slot. If that is not possible, the slot keeps the
    //    empty one (what a draw sees when RanuraVista returns 0) and is not reused: this submission
    //    references it (it is counted).
    for (const ViewInFlight& deferred : views_in_flight_) {
      const auto it = deferred.key ? views_.find(deferred.key) : views_.end();
      if (it != views_.end() && it->second.img != VK_NULL_HANDLE &&
          CreateTextureView(it->second.img, deferred.format_id, deferred.swizzle, deferred.swizzle_host,
                            deferred.heap, it->second.view) == VK_SUCCESS) {
        WriteImage(deferred.heap, deferred.ranura, it->second.view);
        continue;
      }
      WriteImage(deferred.heap, deferred.ranura, empty_[deferred.heap].view);
      if (it != views_.end()) {
        views_.erase(it);  // the image has no vistas_por_imagen_ left (MoverVistasEnVuelo) or is rebuilt when used
      }
      ++lost_slots_;
      Warn(34, "no se pudo crear la vista de una textura");
    }
    views_in_flight_.clear();
    // 3. Deferred barriers and copies, in the upload buffer where SubirTextura left their data.
    VkCommandBuffer upload = VK_NULL_HANDLE;
    for (const TextureInFlight& flight : in_flight_) {
      if (!flight.copy_info) {
        continue;
      }
      Texture& texture = *flight.texture;
      if (texture.img.img == VK_NULL_HANDLE) {
        continue;  // no image: created and fully uploaded next time it is used
      }
      if (with_copies && upload == VK_NULL_HANDLE) {
        upload = contexto_->UploadCommands();
      }
      if (!with_copies || upload == VK_NULL_HANDLE || upload != flight.commands_copy ||
          flight.epoch_copy != upload_epoch_) {
        DisableBindings(fmt::format("DIFERENCIA: una copia aplazada ya no puede ir en su bufer de subida (epoca {} "
                                  "frente a {}, {})",
                                  flight.epoch_copy, upload_epoch_,
                                  with_copies ? "otro bufer de subida" : "recogida al cambiar de bufer"));
        texture.img.prepared = false;  // the next check uploads it again in full, with its barrier
        texture.next = 0;
        continue;
      }
      if (!texture.img.prepared) {
        Barrier(upload, texture.img.img, texture.layer_count);
        texture.img.prepared = true;
      }
      RecordTextureCopy(upload, texture, flight.offset_copy);
    }
    in_flight_.clear();
    bindings_collecting_ = false;
    if (!creation_measuring_) {  // inside PrepararTextura its stopwatch already counts it
      const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - start)
                                       .count());
      const uint64_t wait_stats = bindings_wait_total_ns_ - wait_before;
      const uint64_t own = ns > wait_stats ? ns - wait_stats : 0;
      nfsmw::waits::g_textures_create_ns.fetch_add(own, std::memory_order_relaxed);
      create_report_ns_ += own;
    }
  }

  // UsarRanura: a texture with its bind in flight may reach new work (it was prepared before that work's
  // first Grabar), but not with its data already in the previous upload buffer: that should never happen.
  void CheckBindingsOnBufferChange() {
    for (const TextureInFlight& flight : in_flight_) {
      if (flight.copy_info) {
        CollectBindings(false);  // logs the DIFERENCIA, switches off and leaves those textures to be uploaded again
        return;
      }
    }
  }

  // Every 10 s, if there were new textures: what the thread did and how long the ring waited. This is also
  // where it is decided whether it pays off: if for 3 reports in a row the ring waits for the thread longer
  // than the thread takes to bind, the ring would do better doing it itself, and it switches off.
  void BindingsReport() {
    if (phase_bindings_ == kBindingsWithoutDecide) {
      return;
    }
    const auto now_time = std::chrono::steady_clock::now();
    if (now_time - bindings_report_ < std::chrono::seconds(10)) {
      return;
    }
    bindings_report_ = now_time;
    uint64_t thread_ns = 0;
    uint64_t worst_thread = 0;
    uint64_t full_queue = 0;
    bool priority_ok = true;
    {
      std::lock_guard<std::mutex> mutex_lock(mutex_bindings_);
      thread_ns = thread_bindings_ns_ - thread_bindings_previous_ns_;
      thread_bindings_previous_ns_ = thread_bindings_ns_;
      worst_thread = worst_binding_ns_;
      worst_binding_ns_ = 0;
      full_queue = full_queue_bindings_ - full_queue_previous_;
      full_queue_previous_ = full_queue_bindings_;
      priority_ok = priority_bindings_ok_;
    }
    const uint64_t created = created_textures_ - created_report_previous_;
    created_report_previous_ = created_textures_;
    if (created || thread_bindings_report_ || bindings_waits_report_) {
      static constexpr const char* kPhases[] = {"apagada", "MIRANDO", "APLICANDO"};
      NFSMW_RING_REPORT(
          "[nativo] C3 texturas nuevas (build 184), ultimos 10 s: {} creadas, {} con el enlace en el hilo; el anillo "
          "gasto {:.1f} ms creando y espero al hilo {:.1f} ms en {} veces (peor {:.2f} ms); el hilo, {:.1f} ms dentro de "
          "vkBindImageMemory (media {:.0f} us, peor {:.2f} ms); {} vistas y {} copias aplazadas; {} por la ruta de "
          "siempre con la cola llena; fase {}{}",
          created, thread_bindings_report_, double(create_report_ns_) / 1e6, double(bindings_wait_report_ns_) / 1e6,
          bindings_waits_report_, double(worst_bindings_wait_ns_) / 1e6, double(thread_ns) / 1e6,
          thread_bindings_report_ ? double(thread_ns) / 1e3 / double(thread_bindings_report_) : 0.0,
          double(worst_thread) / 1e6, deferred_views_report_, deferred_copies_report_, full_queue,
          kPhases[std::clamp<int32_t>(phase_bindings_, 0, 2)], priority_ok ? "" : " (el kernel NO acepto la prioridad)");
    }
    if (phase_bindings_ == kApplyingBindings && thread_bindings_report_ >= 16) {
      reports_without_compensate_ = bindings_wait_report_ns_ > thread_ns ? reports_without_compensate_ + 1 : 0;
      if (reports_without_compensate_ >= 3) {
        DisableBindings(fmt::format("no compensa: en 3 informes seguidos el anillo espero al hilo mas de lo que el hilo "
                                  "tardo en enlazar (el ultimo, {:.1f} ms de espera por {:.1f} ms de enlaces)",
                                  double(bindings_wait_report_ns_) / 1e6, double(thread_ns) / 1e6));
      }
    }
    thread_bindings_report_ = 0;
    bindings_waits_report_ = 0;
    bindings_wait_report_ns_ = 0;
    worst_bindings_wait_ns_ = 0;
    create_report_ns_ = 0;
    deferred_views_report_ = 0;
    deferred_copies_report_ = 0;
  }

  // --- Hashing of new textures on a separate thread (nfsmw_nativo_texturas_huella_hilo) ------------------------
  /*
   * Why. During zone-change stutters the ring spends 5.3-8.7 ms of the frame in "texturas": the XXH3 of
   * guest memory (2.3-3.1 ms, with some rechecks included) and the untiling and byte swapping of the new
   * ones (3.0-5.6 ms, 1.8-3.1 ms per MB); and outside that figure, the copy to the upload buffer (~0.4 ms
   * per MB). The ring is the bottleneck (the GPU waits 5-11 ms per frame).
   *
   * Exact. The hash and the untiling read guest memory, which the game may reuse as soon as the ring
   * returns the read pointer or writes a fence. So the thread is never given guest addresses: the ring
   * copies, right there (where the inline path computes the hash), the bytes the hash covers (base and
   * mips, the same ranges) into a snapshot, and the thread works on that copy. They are the same bytes read
   * at the same point, and the hash, the untiling and the byte swap are functions of those bytes: the
   * results are identical. Every read of the plan falls inside the copy (PlanearLecturaHuella; the bound was
   * tested against both untiling paths on 145,500 rectangles). The thread writes the result into the space
   * SubirTextura reserves in the upload buffer (same size, same order), and the barrier and the copy are
   * recorded where they always are: the GPU does not read that buffer until vkQueueSubmit, and
   * AntesDeEnviar (RecogerHuellas) waits before that. Same submission, same bytes: no placeholder textures
   * and no frame of delay. The texture's huella_cruda is set when collected, before anything looks at it.
   *
   * The ring never waits for more than one job. When collecting, it takes the ones the thread has not
   * started (from the end of the queue) and does them itself; it only waits for the one the thread is in
   * the middle of. If the thread has no core, the ring ends up doing the usual work at submission, plus the
   * copy, and the "no compensa" guard switches it off.
   *
   * Threads (see docs/platform-notes.md, Threads). All shared state is under huellas_mutex_, and every
   * decision to sleep or wake is taken with the lock held: no "store mine and read yours" with atomics.
   * The thread only reads the snapshot and the plan of its job (written before publishing it) and only
   * writes its buffers, its space in the upload buffer and its job's results; it does not write to the log.
   * Persistent thread (never detached), joined in the destructor before the upload buffer is released.
   * Priority 0x2E: the ring (0x2D) preempts it as soon as it has work (equal priorities do not time-slice)
   * and the guest (0x3B) does not starve it of a core; it also prefers a core other than the ring's, which
   * is busy.
   *
   * Self-checking guard. Observing phase: the first kHuellasAComprobar new textures go through the usual
   * path (that is what gets uploaded) and also through the thread, which returns its raw hash and the XXH3
   * of its data; they are compared when collected. All equal: applying phase, and 1 in
   * kComprobarHuellaUnaDeCada keeps being compared. A DIFERENCIA, a wait of more than 2 s, a job that does
   * not add up, or no payoff for 3 reports in a row: REXLOG_ERROR and off for the session. After a
   * DIFERENCIA, the thread's remaining textures are uploaded again in full. The worst case is the usual
   * path.
   */
  struct FingerprintReadback {  // one LeerNivel call, reading from the snapshot
    uint64_t src = 0;  // desplazamiento en instantaneas_
    uint64_t row_bytes = 0;
    size_t target = 0;  // offset in the texture data
    uint32_t blocks_pitch = 0;
    uint32_t ox = 0;
    uint32_t oy = 0;
    uint32_t bx = 0;
    uint32_t by = 0;
    uint32_t bx_host = 0;
    bool tile = false;
    bool run_check = false;  // the fast untiling guard repeats this level on the usual path
  };
  struct FingerprintJob {
    // Written by the ring before publishing; whoever does the job (the thread or the ring) only reads it.
    Texture* texture = nullptr;  // used by the ring only; in huellas_planeadas_, nullptr = already published or discarded
    uint64_t key = 0;
    uint64_t sequence = 0;
    uint64_t inst_base = 0;  // the copy of the base (all layers) in instantaneas_
    uint64_t bytes_base = 0;
    uint64_t inst_mips = 0;  // the copy of the mips
    uint64_t bytes_mips = 0;
    size_t data_bytes = 0;
    uint8_t* target = nullptr;  // its space in the upload buffer; nullptr for comparison jobs
    uint64_t epoch = 0;          // epoca_subida_ of that buffer
    uint32_t start_read = 0;  // su plan: lecturas_huella_[lectura_inicio, lectura_inicio + lecturas)
    uint32_t reads = 0;
    uint32_t block_bytes = 1;
    uint32_t block_log2 = 0;
    uint32_t order_unit = 0;
    uint32_t orden = 0;
    uint32_t address = 0;  // for the log
    uint32_t width_px = 0;
    uint32_t height_px = 0;
    uint32_t format_id = 0;
    bool fast = false;           // mosaico_rapido_ al planear
    bool check_order = false;  // the fast byte swap guard checks this texture
    bool compare = false;         // observing phase (or 1 in kComprobarHuellaUnaDeCada): only returns its hashes
    bool levels_warning = false;    // collected without differences: the "niveles comprobados ... todos iguales" line
    bool commands_warning = false;
    // Written by whoever does the job; the ring reads it under huellas_mutex_ once it is done.
    int32_t state = 0;
    bool per_ring = false;
    bool distinct_order = false;
    uint32_t distinct_levels = 0;
    std::array<uint32_t, 5> distinct{};  // first differing level: blocks in x and y, pitch and origin (x, y)
    uint64_t raw_fingerprint = 0;
    uint64_t data_fingerprint = 0;  // comparison jobs only
    uint64_t fingerprint_ns = 0;
    uint64_t rest_ns = 0;
  };
  struct FingerprintCompare {  // ring only: what the usual path produced for a comparison texture
    uint64_t raw_data = 0;
    uint64_t payload = 0;
    bool tracked = false;
  };
  static constexpr size_t kReadsPerTexture = 6 * 16;  // capas x niveles de mip
  static constexpr uint32_t kFingerprintReads = 8192;       // reads planned between two collections
  static constexpr uint64_t kFingerprintsQueue = 256;           // potencia de 2
  static constexpr uint32_t kPublishedFingerprintJob = 0xFFFFFFFFu;  // in Textura::huella_trabajo
  static constexpr int32_t kPendingJob = 1;
  static constexpr int32_t kJobInThread = 2;
  static constexpr int32_t kJobInRing = 3;
  static constexpr int32_t kDoneJob = 4;
  static constexpr int32_t kFingerprintsWithoutDecide = -1;
  static constexpr int32_t kFingerprintsDisabled = 0;
  static constexpr int32_t kPeekingFingerprints = 1;
  static constexpr int32_t kApplyingFingerprints = 2;
  static constexpr uint64_t kFingerprintsToCheck = 64;
  static constexpr uint64_t kCheckFingerprintEvery = 128;

  void DecideFingerprints() {
    const bool request = REXCVAR_GET(nfsmw_nativo_texturas_huella_hilo);
    priority_fingerprints_ = std::clamp<int32_t>(REXCVAR_GET(nfsmw_nativo_texturas_huella_hilo_prioridad), 0x2C, 0x3B);
    request_core_fingerprints_ = std::clamp<int32_t>(REXCVAR_GET(nfsmw_nativo_texturas_huella_hilo_nucleo), -2, 2);
    snapshots_bytes_ = uint64_t(std::clamp<int32_t>(REXCVAR_GET(nfsmw_nativo_texturas_huella_hilo_mb), 4, 64))
                          << 20;
    phase_fingerprints_ = request ? kPeekingFingerprints : kFingerprintsDisabled;
    REXLOG_INFO("[nativo] C3: huella y desenmosaicado de las texturas nuevas en un hilo aparte "
                "(nfsmw_nativo_texturas_huella_hilo) = {}",
                request ? fmt::format("SI, prioridad {:#x}, {} MB de instantaneas; fase MIRANDO: las {} primeras van "
                                     "tambien por el camino de siempre y se comparan",
                                     priority_fingerprints_, snapshots_bytes_ >> 20, kFingerprintsToCheck)
                       : std::string("no, en el hilo del anillo como siempre"));
  }

  void DisableFingerprints(const std::string& reason) {
    if (phase_fingerprints_ == kFingerprintsDisabled) {
      return;
    }
    phase_fingerprints_ = kFingerprintsDisabled;
    REXLOG_ERROR("[nativo] C3: hilo de huellas de texturas APAGADO para el resto de la sesion: {}. Las texturas nuevas "
                 "vuelven a prepararse enteras en el hilo del anillo ({} hechas por el hilo, {} por el anillo, {} "
                 "comparadas)",
                 reason, thread_fingerprints_total_, ring_fingerprints_total_, compared_fingerprints_);
  }

  /*
   * One LeerNivel call turned into a read from the snapshot. false = the texture goes through the usual
   * path: LeerNivel would go outside memory (its own check) or would read outside what the hash covers.
   * `rango_*` is the hash region (base or mips) and `en_copia` is where its copy starts in the snapshot.
   * The bound on what is read is the hash's (GetTiledAddressUpperBound2D): the corner of the last
   * 32x32-block tile plus the size of one tile; it was checked against what DesenmosaicarNivel and
   * LeerNivelMosaicoDeSiempre read.
   */
  static bool PlanFingerprintRead(std::array<FingerprintReadback, kReadsPerTexture>& reads, uint32_t& n,
                                   uint64_t address, bool tile, uint32_t blocks_pitch, uint64_t row_bytes,
                                   const TextureFormat& tf, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by,
                                   uint32_t bx_host, size_t target, uint64_t start_range, uint64_t range_bytes,
                                   uint64_t in_copy) {
    if (!bx || !by) {
      return true;  // LeerNivel reads nothing
    }
    const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2 : tf.bytes >= 2 ? 1 : 0;
    const uint64_t loose =
        tile ? uint64_t(std::max<int64_t>(Tile2dOffset(int32_t((ox + bx + 31) & ~31u),
                                                                     int32_t((oy + by + 31) & ~31u), blocks_pitch,
                                                                     log2),
                                             0))
                : row_bytes * (oy + by - 1) + uint64_t(ox + bx) * tf.bytes;
    if (address + loose > kPhysicalMemory) {
      return false;  // LeerNivel would return false: let the usual path report it
    }
    const uint64_t was_read =
        tile ? uint64_t(std::max<int64_t>(Tile2dOffset(int32_t((ox + bx - 1) & ~31u),
                                                                     int32_t((oy + by - 1) & ~31u), blocks_pitch,
                                                                     log2),
                                             0)) +
                      (log2 == 0 ? 0xA00u : log2 == 1 ? 0xC00u : (0x400u << log2))
                : row_bytes * (oy + by - 1) + uint64_t(ox + bx) * tf.bytes;
    if (n >= reads.size() || address < start_range || address + was_read > start_range + range_bytes) {
      return false;
    }
    FingerprintReadback& l = reads[n++];
    l.src = in_copy + (address - start_range);
    l.row_bytes = row_bytes;
    l.target = target;
    l.blocks_pitch = blocks_pitch;
    l.ox = ox;
    l.oy = oy;
    l.bx = bx;
    l.by = by;
    l.bx_host = bx_host;
    l.tile = tile;
    l.run_check = false;
    return true;
  }

  /*
   * The rest of a new texture for the thread: space, snapshot, plan and job. true = applying phase: the
   * thread prepares it and the tail of PrepararTextura is done here (interval, upload and counters); the
   * caller returns. false = the usual path continues: it was not possible, or it is a comparison texture
   * (observing phase, and 1 in kComprobarHuellaUnaDeCada), whose job is already published and is compared
   * when collected.
   */
  bool PlanFingerprint(Texture& texture, uint64_t key, uint32_t n_reads, const uint8_t* base_raw,
                     uint64_t extension, const uint8_t* mips_raw, uint64_t extension_mips, const TextureFormat& tf,
                     uint32_t orden, size_t data_bytes, const std::array<size_t, 16>& offsets,
                     uint32_t address, uint32_t width_px, uint32_t height_px, uint32_t format_id, VkDeviceSize& upload_bytes) {
    texture_comparison_ = nullptr;
    if (phase_fingerprints_ == kFingerprintsWithoutDecide) {
      DecideFingerprints();
    }
    if (phase_fingerprints_ == kFingerprintsDisabled || texture.job_fingerprint != 0 ||
        (phase_fingerprints_ == kPeekingFingerprints &&
         planned_comparisons_fingerprints_ >= kFingerprintsToCheck + fingerprints_without_compare_)) {
      return false;  // while observing, with comparisons already running, the rest only take the usual path
    }
    if (!snapshots_) {
      snapshots_.reset(new (std::nothrow) uint8_t[size_t(snapshots_bytes_)]);
      fingerprint_reads_.reset(new (std::nothrow) FingerprintReadback[kFingerprintReads]);
      if (!snapshots_ || !fingerprint_reads_) {
        snapshots_.reset();
        fingerprint_reads_.reset();
        DisableFingerprints(fmt::format("DIFERENCIA: no hay memoria para {} MB de instantaneas", snapshots_bytes_ >> 20));
        return false;
      }
    }
    const uint64_t inst_base = (used_snapshots_ + 63) & ~uint64_t(63);
    const uint64_t inst_mips = inst_base + ((extension + 63) & ~uint64_t(63));  // the same space the plan assumes
    const uint64_t in_queue = published_fingerprints_ - collected_fingerprints_ + pending_planned_fingerprints_;
    if (in_queue >= kFingerprintsQueue || used_reads_ + n_reads > kFingerprintReads ||
        inst_mips + extension_mips > snapshots_bytes_) {
      ++fingerprints_without_room_report_;
      return false;
    }
    const bool compare =
        phase_fingerprints_ == kPeekingFingerprints || (++applying_new_fingerprints_ % kCheckFingerprintEvery) == 0;
    // The snapshot: the same bytes the usual path's hash covers, at the same point.
    const auto before_copy = std::chrono::steady_clock::now();
    std::memcpy(snapshots_.get() + inst_base, base_raw, size_t(extension));
    if (extension_mips) {
      std::memcpy(snapshots_.get() + inst_mips, mips_raw, size_t(extension_mips));
    }
    const uint64_t copy_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - before_copy)
                                           .count());
    used_snapshots_ = inst_mips + extension_mips;
    snapshots_report_ns_ += copy_ns;
    snapshots_report_bytes_ += extension + extension_mips;
    nfsmw::waits::g_snapshot_fingerprint_ns.fetch_add(copy_ns, std::memory_order_relaxed);
    const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2 : tf.bytes >= 2 ? 1 : 0;
    FingerprintJob t;
    t.texture = &texture;
    t.key = key;
    t.inst_base = inst_base;
    t.bytes_base = extension;
    t.inst_mips = inst_mips;
    t.bytes_mips = extension_mips;
    t.data_bytes = data_bytes;
    t.start_read = used_reads_;
    t.reads = n_reads;
    t.block_bytes = tf.bytes;
    t.block_log2 = log2;
    t.order_unit = tf.order_unit;
    t.orden = orden;
    t.address = address;
    t.width_px = width_px;
    t.height_px = height_px;
    t.format_id = format_id;
    t.fast = fast_tile_ > 0;
    t.compare = compare;
    // The plan, with the decisions of the fast untiling and byte swap guards taken in the same order as the
    // usual path (niveles_vistos_ and ordenes_vistos_). Not for a comparison texture: the usual path also
    // processes it, and that is what keeps those counts.
    for (uint32_t i = 0; i < n_reads; ++i) {
      FingerprintReadback l = plan_reads_[i];
      l.src += inst_base;
      if (!compare && l.tile && t.fast && tf.bytes == (1u << log2)) {
        ++seen_levels_;
        l.run_check = seen_levels_ <= kLevelsToCheck || seen_levels_ % kCheckEvery == 0;
        if (l.run_check && ++checked_levels_ == kLevelsToCheck) {
          t.levels_warning = true;
        }
      }
      fingerprint_reads_[used_reads_ + i] = l;
    }
    used_reads_ += n_reads;
    if (!compare && t.fast && orden != 0 && (tf.order_unit == 2 || tf.order_unit == 4)) {
      ++seen_commands_;
      t.check_order = seen_commands_ <= kLevelsToCheck || seen_commands_ % kCheckEvery == 0;
      if (t.check_order && ++checked_commands_ == kLevelsToCheck) {
        t.commands_warning = true;
      }
    }
    ++planned_fingerprints_report_;
    if (compare) {
      // Published now, with no space in the upload buffer: the thread only returns its two hashes. What gets
      // uploaded is the usual path's result, which continues on return (AnotarComparacionHuella stores its
      // hashes).
      ++planned_comparisons_fingerprints_;
      ++comparison_fingerprints_report_;
      const uint64_t sequence = PublishFingerprintJob(t, nullptr);
      fingerprint_comparisons_[sequence & (kFingerprintsQueue - 1)] = FingerprintCompare{};
      texture_comparison_ = &texture;
      sequence_comparison_ = sequence;
      return false;
    }
    // Applying phase: the tail of PrepararTextura for a new texture, the same as there.
    planned_fingerprints_.push_back(t);
    ++pending_planned_fingerprints_;
    texture.job_fingerprint = uint32_t(planned_fingerprints_.size());
    texture.valid_sample = false;
    texture.consecutive_samples = 0;
    for (uint32_t n = 0; n < texture.levels; ++n) {
      texture.level_offset[n] = uint32_t(offsets[n]);
    }
    texture.interval = 1;
    texture.next = frame_ + 1;
    texture.fingerprint = 0;  // a new texture's data hash decides nothing: 0, as on the usual path
    texture.do_upload = true;
    textures_to_upload_.push_back(&texture);
    nfsmw::waits::g_uploads_textures.fetch_add(1, std::memory_order_relaxed);
    nfsmw::waits::g_uploaded_bytes.fetch_add(data_bytes, std::memory_order_relaxed);
    upload_bytes += (data_bytes + 3) & ~size_t(3);
    return true;
  }

  // Queues a job for the thread (under the lock), waking it if it sleeps. Returns its sequence number. If
  // the thread cannot be created, the job stays queued anyway and the ring does it when collecting (and the
  // feature switches off).
  uint64_t PublishFingerprintJob(const FingerprintJob& job, uint8_t* target) {
    if (!thread_fingerprints_.joinable() && !fingerprints_without_thread_) {
#if REX_PLATFORM_SWITCH
      const int ring = RexSwitchCurrentCore();  // the ring thread asks for itself
#else
      const int ring = -1;
#endif
      core_fingerprints_ = request_core_fingerprints_ != -2 ? request_core_fingerprints_ : ring == 2 ? 1 : 2;
      try {
        thread_fingerprints_ = std::thread([this] { FingerprintsLoop(); });
        REXLOG_INFO("[nativo] C3: hilo de huellas de texturas creado: nucleo preferido {} (el anillo corre en el {}), "
                    "prioridad {:#x}",
                    core_fingerprints_, ring, priority_fingerprints_);
      } catch (const std::system_error& error) {
        fingerprints_without_thread_ = true;
        DisableFingerprints(fmt::format("DIFERENCIA: no se pudo crear el hilo ({})", error.what()));
      }
    }
    bool warn = false;
    uint64_t sequence = 0;
    {
      std::lock_guard<std::mutex> mutex_lock(mutex_fingerprints_);
      sequence = published_fingerprints_;
      FingerprintJob& slot = fingerprints_queue_[sequence & (kFingerprintsQueue - 1)];
      slot = job;
      slot.sequence = sequence;
      slot.target = target;
      slot.epoch = upload_epoch_;
      slot.state = kPendingJob;
      ++published_fingerprints_;
      warn = sleeping_fingerprints_;  // decided under the lock: if it sleeps, it is in its wait with the predicate
    }
    if (warn) {
      cv_fingerprints_.notify_one();
    }
    return sequence;
  }

  // The data bytes of a planned texture (SubirTextura reserves that space). If its index does not match
  // (should not happen), the texture's own, which come from the same per-level computation.
  size_t PlannedFingerprintBytes(const Texture& texture) const {
    const size_t index = size_t(texture.job_fingerprint) - 1;
    return index < planned_fingerprints_.size() && planned_fingerprints_[index].texture == &texture
               ? planned_fingerprints_[index].data_bytes
               : size_t(texture.bytes);
  }

  // SubirTextura for a planned texture: its job, with the space just reserved for it, goes to the thread's
  // queue.
  void PublishFingerprint(Texture& texture, uint8_t* target, size_t bytes) {
    const size_t index = size_t(texture.job_fingerprint) - 1;
    if (index >= planned_fingerprints_.size() || planned_fingerprints_[index].texture != &texture) {
      // Should never happen. Without a job its data would never arrive: zeros in its space, and it is uploaded
      // in full next time.
      std::memset(target, 0, bytes);
      texture.job_fingerprint = 0;
      texture.raw_fingerprint = 0;
      texture.next = 0;
      DisableFingerprints(fmt::format("DIFERENCIA: trabajo planeado incoherente ({} de {})", index,
                                planned_fingerprints_.size()));
      return;
    }
    PublishFingerprintJob(planned_fingerprints_[index], target);
    planned_fingerprints_[index].texture = nullptr;  // published: no longer counts as planned
    --pending_planned_fingerprints_;
    texture.job_fingerprint = kPublishedFingerprintJob;
  }

  // At the start of each draw: the previous draw's planned textures that never reached SubirTextura (the
  // draw was rejected before). As on the usual path, they stay un-uploaded and are prepared again at their
  // next check.
  void DiscardPlannedFingerprints() {
    if (!pending_planned_fingerprints_) {
      return;
    }
    for (FingerprintJob& t : planned_fingerprints_) {
      if (t.texture) {
        t.texture->job_fingerprint = 0;
        t.texture = nullptr;
        ++discarded_fingerprints_;
      }
    }
    pending_planned_fingerprints_ = 0;
  }

  // Observing phase (and 1 in kComprobarHuellaUnaDeCada): the usual path's hashes of the texture whose
  // comparison job was just published. RecogerHuellas compares them.
  void TrackFingerprintCompare(const Texture& texture, const std::vector<uint8_t>& payload) {
    texture_comparison_ = nullptr;
    if (sequence_comparison_ < collected_fingerprints_) {
      return;  // already collected (should not happen): left uncompared and counted
    }
    FingerprintCompare& c = fingerprint_comparisons_[sequence_comparison_ & (kFingerprintsQueue - 1)];
    c.raw_data = texture.raw_fingerprint;
    c.payload = XXH3_64bits(payload.data(), payload.size());
    c.tracked = true;
  }

  /*
   * One job: done by the thread (or by the ring, if it takes it when collecting). The same as
   * PrepararTextura's usual path and in the same order (raw hash, levels through LeerNivel, and byte swap
   * with its guard), but reading from the snapshot. It only reads this job's snapshot and plan, and only
   * writes `datos`, `comprobacion`, its space in the upload buffer and the results in `t`: nothing of the
   * ring's state. `rapido_apagado` belongs to whoever does the job: after a difference in its guard, the
   * rest takes the usual path, as on the ring (which also turns off mosaico_rapido_ when collecting it).
   */
  void ExecuteFingerprint(FingerprintJob& t, std::vector<uint8_t>& payload, std::vector<uint8_t>& check,
                      bool& fast_disabled) {
    const auto start = std::chrono::steady_clock::now();
    const uint8_t* const snap = snapshots_.get();
    uint64_t fingerprint = XXH3_64bits(snap + t.inst_base, size_t(t.bytes_base));
    if (t.bytes_mips) {
      fingerprint = XXH3_64bits_withSeed(snap + t.inst_mips, size_t(t.bytes_mips), fingerprint);
    }
    t.raw_fingerprint = fingerprint;
    const auto after_fingerprint = std::chrono::steady_clock::now();
    payload.assign(t.data_bytes, 0);
    const uint32_t b = t.block_bytes;
    for (uint32_t i = 0; i < t.reads; ++i) {
      const FingerprintReadback& l = fingerprint_reads_[t.start_read + i];
      const uint8_t* const src = snap + l.src;
      uint8_t* const target = payload.data() + l.target;
      if (!l.tile) {
        for (uint32_t y = 0; y < l.by; ++y) {
          std::memcpy(target + size_t(y) * l.bx_host * b,
                      src + uint64_t(l.oy + y) * l.row_bytes + uint64_t(l.ox) * b, size_t(l.bx) * b);
        }
        continue;
      }
      if (t.fast && !fast_disabled && b == (1u << t.block_log2)) {
        switch (t.block_log2) {
          case 0: UntileLevel<0>(src, l.blocks_pitch, l.ox, l.oy, l.bx, l.by, l.bx_host, target); break;
          case 1: UntileLevel<1>(src, l.blocks_pitch, l.ox, l.oy, l.bx, l.by, l.bx_host, target); break;
          case 2: UntileLevel<2>(src, l.blocks_pitch, l.ox, l.oy, l.bx, l.by, l.bx_host, target); break;
          case 3: UntileLevel<3>(src, l.blocks_pitch, l.ox, l.oy, l.bx, l.by, l.bx_host, target); break;
          default: UntileLevel<4>(src, l.blocks_pitch, l.ox, l.oy, l.bx, l.by, l.bx_host, target); break;
        }
        if (l.run_check) {
          // The LeerNivel guard: repeated on the usual path (which is the result kept) and compared.
          check.assign(size_t(l.by) * l.bx_host * b, 0);
          for (uint32_t y = 0; y < l.by; ++y) {
            std::memcpy(check.data() + size_t(y) * l.bx_host * b, target + size_t(y) * l.bx_host * b,
                        size_t(l.bx) * b);
          }
          ReadLevelTileAsAlways(src, l.blocks_pitch, t.block_log2, b, l.ox, l.oy, l.bx, l.by, l.bx_host,
                                    target);
          bool all_equal = true;
          for (uint32_t y = 0; y < l.by && all_equal; ++y) {
            all_equal = std::memcmp(check.data() + size_t(y) * l.bx_host * b,
                                  target + size_t(y) * l.bx_host * b, size_t(l.bx) * b) == 0;
          }
          if (!all_equal) {
            if (!t.distinct_levels++) {
              t.distinct = {l.bx, l.by, l.blocks_pitch, l.ox, l.oy};
            }
            fast_disabled = true;
          }
        }
        continue;
      }
      ReadLevelTileAsAlways(src, l.blocks_pitch, t.block_log2, b, l.ox, l.oy, l.bx, l.by, l.bx_host, target);
    }
    const auto orden = static_cast<xenos::Endian>(t.orden);
    if (t.fast && !fast_disabled && orden != xenos::Endian::kNone && (t.order_unit == 2 || t.order_unit == 4)) {
      if (t.check_order) {
        check = payload;
      }
      ChangeOrderBytes(payload.data(), payload.size(), t.order_unit, t.orden);
      if (t.check_order) {
        AlwaysOrder(check, t.order_unit, orden);
        if (check != payload) {
          t.distinct_order = true;
          payload.swap(check);  // the usual one is kept, as on the ring
          fast_disabled = true;
        }
      }
    } else {
      AlwaysOrder(payload, t.order_unit, orden);
    }
    if (t.compare) {
      t.data_fingerprint = XXH3_64bits(payload.data(), payload.size());
    }
    if (t.target) {
      std::memcpy(t.target, payload.data(), payload.size());
    }
    const auto stop_pos = std::chrono::steady_clock::now();
    t.fingerprint_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(after_fingerprint - start).count());
    t.rest_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(stop_pos - after_fingerprint).count());
  }

  // The thread: the queued jobs, in order, skipping those the ring has taken. It sleeps on a decision taken
  // under the lock; it never spins. It does not write to the log (the ring records the figures when
  // collecting).
  void FingerprintsLoop() {
    rex::thread::set_current_thread_name("NFSMW huellas de texturas");
#if REX_PLATFORM_SWITCH
    const bool priority_ok = RexSwitchSetCurrentThreadPriorityOk(int(priority_fingerprints_));
    const bool core_ok = RexSwitchSetCurrentThreadCore(int(core_fingerprints_));
    const int core = RexSwitchCurrentCore();
#else
    const bool priority_ok = true;
    const bool core_ok = true;
    const int core = -1;
#endif
    std::vector<uint8_t> payload;         // owned by this thread only
    std::vector<uint8_t> check;  // owned by this thread only
    bool fast_disabled = false;        // owned by this thread only: its fast-path guard saw a difference
    std::unique_lock<std::mutex> mutex_lock(mutex_fingerprints_);
    priority_fingerprints_ok_ = priority_ok;
    core_fingerprints_ok_ = core_ok;
    real_core_fingerprints_ = core;
    for (;;) {
      while (taken_fingerprints_ != published_fingerprints_ &&
             fingerprints_queue_[taken_fingerprints_ & (kFingerprintsQueue - 1)].state != kPendingJob) {
        ++taken_fingerprints_;  // taken by the ring
      }
      if (taken_fingerprints_ == published_fingerprints_) {
        if (stop_fingerprints_) {
          return;  // stop, nothing pending
        }
        if (waiting_fingerprints_) {
          cv_done_fingerprints_.notify_one();
        }
        sleeping_fingerprints_ = true;
        cv_fingerprints_.wait(mutex_lock, [this] { return stop_fingerprints_ || taken_fingerprints_ != published_fingerprints_; });
        sleeping_fingerprints_ = false;
        continue;
      }
      // The slot belongs to this thread while it is EnHilo: the ring neither touches nor reuses it until it is
      // collected done.
      FingerprintJob& t = fingerprints_queue_[taken_fingerprints_ & (kFingerprintsQueue - 1)];
      t.state = kJobInThread;
      ++taken_fingerprints_;
      mutex_lock.unlock();
      ExecuteFingerprint(t, payload, check, fast_disabled);
      mutex_lock.lock();
      t.state = kDoneJob;
      ++done_fingerprints_;
      if (waiting_fingerprints_) {
        cv_done_fingerprints_.notify_one();
      }
    }
  }

  /*
   * Waits for and collects all published jobs. Ring thread only: in AntesDeEnviar (before vkQueueSubmit)
   * and, if needed, before rechecking a texture. It takes the ones the thread has not started (from the
   * end) and does them itself; it only waits for the one the thread is in the middle of, with a timeout and
   * a log line every 2 s. Then it sets each texture's raw hash and compares the comparison jobs. If nothing
   * planned is left, the snapshots start over.
   */
  void CollectFingerprints() {
    if (fingerprints_collecting_) {
      return;
    }
    if (published_fingerprints_ == collected_fingerprints_) {
      if (!pending_planned_fingerprints_ && (used_snapshots_ || used_reads_)) {
        used_snapshots_ = 0;  // nothing published uncollected or planned: everything starts over
        used_reads_ = 0;
        planned_fingerprints_.clear();
      }
      return;
    }
    fingerprints_collecting_ = true;
    uint64_t wait_ns = 0;
    uint64_t ring_ns = 0;
    uint64_t ring_done = 0;
    {
      std::unique_lock<std::mutex> mutex_lock(mutex_fingerprints_);
      int secs = 0;
      while (done_fingerprints_ != published_fingerprints_) {
        // 1. One the thread has not started, from the end of the queue (the thread goes from the start).
        FingerprintJob* mio = nullptr;
        for (uint64_t i = published_fingerprints_; i > taken_fingerprints_; --i) {
          FingerprintJob& t = fingerprints_queue_[(i - 1) & (kFingerprintsQueue - 1)];
          if (t.state == kPendingJob) {
            t.state = kJobInRing;
            mio = &t;
            break;
          }
        }
        if (mio) {
          mutex_lock.unlock();
          const auto before = std::chrono::steady_clock::now();
          ExecuteFingerprint(*mio, ring_data_fingerprints_, ring_check_fingerprints_, ring_disabled_fast_fingerprints_);
          ring_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - before)
                                    .count());
          ++ring_done;
          mutex_lock.lock();
          mio->per_ring = true;
          mio->state = kDoneJob;
          ++done_fingerprints_;
          continue;
        }
        // 2. What is left, the thread has half done: wait for it, deciding under the lock.
        const auto before = std::chrono::steady_clock::now();
        waiting_fingerprints_ = true;
        if (sleeping_fingerprints_) {
          cv_fingerprints_.notify_one();  // should not be needed (it has one in progress); in case a wake-up was lost
        }
        const bool ready = cv_done_fingerprints_.wait_for(mutex_lock, std::chrono::seconds(2),
                                                         [this] { return done_fingerprints_ == published_fingerprints_; });
        waiting_fingerprints_ = false;
        const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - before)
                                         .count());
        wait_ns += ns;
        worst_fingerprints_wait_ns_ = std::max(worst_fingerprints_wait_ns_, ns);
        ++fingerprints_waits_report_;
        if (!ready) {
          secs += 2;
          stuck_fingerprints_ = true;
          REXLOG_ERROR("[nativo] C3: el hilo del anillo lleva {} s esperando al hilo de huellas de texturas: "
                       "publicados {}, tomados {}, hechos {}; el hilo esta {}",
                       secs, published_fingerprints_, taken_fingerprints_, done_fingerprints_,
                       sleeping_fingerprints_ ? "DORMIDO (aviso perdido)" : "despierto (un trabajo que no acaba)");
        }
      }
      // The results were written by whoever did each job; the lock makes them visible here.
      gathered_fingerprints_.clear();
      for (uint64_t s = collected_fingerprints_; s < published_fingerprints_; ++s) {
        gathered_fingerprints_.push_back(fingerprints_queue_[s & (kFingerprintsQueue - 1)]);
      }
      collected_fingerprints_ = published_fingerprints_;  // from here on its slots can be reused
    }
    texture_comparison_ = nullptr;
    bool difference = false;
    std::string detail;
    uint64_t thread_obj = 0;
    uint64_t thread_ns = 0;
    for (FingerprintJob& t : gathered_fingerprints_) {
      if (t.per_ring) {
        ++ring_fingerprints_total_;
      } else {
        ++thread_fingerprints_total_;
        if (!t.compare) {  // what the thread actually took off the ring
          ++thread_obj;
          thread_ns += t.fingerprint_ns + t.rest_ns;
          thread_raw_fingerprint_report_ns_ += t.fingerprint_ns;
        }
      }
      // The fast-path guards, with the same lines as the usual path (LeerNivel and the byte swap).
      if (t.distinct_levels) {
        REXLOG_ERROR("[nativo] C3: el desenmosaicado rapido NO coincide ({}x{} bloques de {} bytes, pitch {}, desde "
                     "{},{}). Se APAGA y se usa el de siempre.",
                     t.distinct[0], t.distinct[1], t.block_bytes, t.distinct[2], t.distinct[3], t.distinct[4]);
        fast_tile_ = 0;
      } else if (t.levels_warning) {
        REXLOG_INFO("[nativo] C3: desenmosaicado rapido: {} niveles comprobados contra el de siempre, todos iguales. "
                    "Sigue el rapido.",
                    kLevelsToCheck);
      }
      if (t.distinct_order) {
        REXLOG_ERROR("[nativo] C3: el cambio de orden rapido NO coincide (unidad {}, orden {}, {} bytes). Se APAGAN el "
                     "orden y el desenmosaicado rapidos.",
                     t.order_unit, t.orden, t.data_bytes);
        fast_tile_ = 0;
      } else if (t.commands_warning) {
        REXLOG_INFO("[nativo] C3: cambio de orden rapido: {} texturas comprobadas contra el de siempre, todas iguales.",
                    kLevelsToCheck);
      }
      if (t.compare) {
        const FingerprintCompare& c = fingerprint_comparisons_[t.sequence & (kFingerprintsQueue - 1)];
        if (!c.tracked) {
          ++fingerprints_without_compare_;
          continue;
        }
        ++compared_fingerprints_;
        if (c.raw_data != t.raw_fingerprint || c.payload != t.data_fingerprint) {
          difference = true;
          detail = fmt::format("DIFERENCIA en la textura {:08X} {}x{} formato {}: huella cruda {:016X} en el anillo y "
                                "{:016X} en el hilo; datos {:016X} y {:016X}",
                                t.address, t.width_px, t.height_px, t.format_id, c.raw_data, t.raw_fingerprint, c.payload,
                                t.data_fingerprint);
        }
        continue;
      }
      // A texture done by the thread: its raw hash is that of the same bytes that were uploaded. It must still
      // be in the cache as it was left, and its data must have gone to the current upload buffer (the one about
      // to be submitted).
      const auto it = textures_.find(t.key);
      if (it == textures_.end() || &it->second != t.texture || t.texture->job_fingerprint != kPublishedFingerprintJob) {
        difference = true;
        detail = fmt::format("DIFERENCIA: la textura {:08X} de un trabajo recogido ya no esta como se dejo",
                              t.address);
        t.texture = nullptr;
        continue;
      }
      if (t.epoch != upload_epoch_) {
        difference = true;  // se vuelve a subir entera (huellas_sospechosas_)
        detail = fmt::format("DIFERENCIA: los datos de la textura {:08X} fueron a otro bufer de subida (epoca {} "
                              "frente a {})",
                              t.address, t.epoch, upload_epoch_);
      }
      t.texture->raw_fingerprint = t.raw_fingerprint;
      t.texture->job_fingerprint = 0;
      TrackTextureContent(*t.texture, t.key);  // measurement only: nfsmw_nativo_diag_reutilizar
    }
    if (difference) {
      suspicious_fingerprints_ = true;
      DisableFingerprints(detail);
    }
    if (stuck_fingerprints_) {
      stuck_fingerprints_ = false;
      DisableFingerprints("DIFERENCIA: una espera de mas de 2 s al hilo de huellas (ver las lineas de arriba)");
    }
    if (suspicious_fingerprints_) {
      // The thread's textures planned before the DIFERENCIA may carry the same fault: they are uploaded again
      // in full, on the usual path (already switched off), at their next check.
      for (const FingerprintJob& t : gathered_fingerprints_) {
        if (!t.compare && t.texture) {
          t.texture->img.prepared = false;
          t.texture->next = 0;
          t.texture->job_fingerprint = 0;
          ++reuploaded_fingerprints_;
        }
      }
    } else if (phase_fingerprints_ == kPeekingFingerprints && compared_fingerprints_ >= kFingerprintsToCheck) {
      phase_fingerprints_ = kApplyingFingerprints;
      bool priority_ok = true;
      bool core_ok = true;
      int core = -1;
      {
        std::lock_guard<std::mutex> mutex_lock(mutex_fingerprints_);
        priority_ok = priority_fingerprints_ok_;
        core_ok = core_fingerprints_ok_;
        core = real_core_fingerprints_;
      }
      REXLOG_INFO("[nativo] C3: hilo de huellas de texturas: {} texturas nuevas hechas por los dos caminos, todas "
                  "iguales (huella cruda y datos; {} sin poder comparar). Pasa a fase APLICANDO: las prepara el hilo y "
                  "se sigue comparando 1 de cada {}. El hilo corre en el nucleo {}{}{}",
                  compared_fingerprints_, fingerprints_without_compare_, kCheckFingerprintEvery, core,
                  core_ok ? "" : " (el kernel NO acepto el nucleo preferido)",
                  priority_ok ? "" : " (el kernel NO acepto la prioridad)");
    }
    thread_fingerprints_report_ += thread_obj;
    ring_fingerprints_report_ += ring_done;
    thread_fingerprints_report_ns_ += thread_ns;
    ring_fingerprints_report_ns_ += ring_ns;
    fingerprints_wait_report_ns_ += wait_ns;
    // A few times per frame (on each submission with new textures): the fetch_add calls do not show.
    nfsmw::waits::g_thread_fingerprint_textures.fetch_add(thread_obj, std::memory_order_relaxed);
    nfsmw::waits::g_thread_fingerprint_ns.fetch_add(thread_ns, std::memory_order_relaxed);
    nfsmw::waits::g_ring_fingerprint_textures.fetch_add(ring_done, std::memory_order_relaxed);
    nfsmw::waits::g_ring_fingerprint_ns.fetch_add(ring_ns, std::memory_order_relaxed);
    nfsmw::waits::g_fingerprints_waiting_ns.fetch_add(wait_ns, std::memory_order_relaxed);
    if (!pending_planned_fingerprints_) {
      used_snapshots_ = 0;
      used_reads_ = 0;
      planned_fingerprints_.clear();
    }
    fingerprints_collecting_ = false;
  }

  // UsarRanura: everything published is collected in AntesDeEnviar. If anything reaches here (it should
  // not), its data was going to the buffer just closed without waiting for it: it is collected now (before
  // this one is reset) and those textures are uploaded again.
  void CheckFingerprintsOnBufferChange() {
    if (published_fingerprints_ == collected_fingerprints_) {
      return;
    }
    const uint64_t without_collect = published_fingerprints_ - collected_fingerprints_;
    suspicious_fingerprints_ = true;  // whatever is collected now is uploaded again in full
    CollectFingerprints();
    DisableFingerprints(fmt::format("DIFERENCIA: {} trabajos del hilo de huellas sin recoger al cambiar de bufer de subida",
                              without_collect));
  }

  void StopFingerprints() {
    if (!thread_fingerprints_.joinable()) {
      return;
    }
    {
      std::lock_guard<std::mutex> mutex_lock(mutex_fingerprints_);
      stop_fingerprints_ = true;
    }
    cv_fingerprints_.notify_one();
    thread_fingerprints_.join();  // first finishes the published jobs
    REXLOG_INFO("[nativo] C3: hilo de huellas de texturas parado ({} texturas hechas por el hilo, {} por el anillo, {} "
                "comparadas, {} descartadas, {} vueltas a subir)",
                thread_fingerprints_total_, ring_fingerprints_total_, compared_fingerprints_, discarded_fingerprints_,
                reuploaded_fingerprints_);
  }

  // Every 10 s, if there were new textures: what the thread and the ring did. This is also where it is
  // decided whether it pays off: if for 3 reports in a row (with 16 or more jobs) what the thread took off
  // the ring is less than what the ring spent copying snapshots and waiting for it, the ring would do better
  // alone, and it switches off.
  void FingerprintsReport() {
    if (phase_fingerprints_ == kFingerprintsWithoutDecide) {
      return;
    }
    const auto now_time = std::chrono::steady_clock::now();
    if (now_time - fingerprints_report_ < std::chrono::seconds(10)) {
      return;
    }
    fingerprints_report_ = now_time;
    bool priority_ok = true;
    int core = -1;
    {
      std::lock_guard<std::mutex> mutex_lock(mutex_fingerprints_);
      priority_ok = priority_fingerprints_ok_;
      core = real_core_fingerprints_;
    }
    if (planned_fingerprints_report_ || thread_fingerprints_report_ || ring_fingerprints_report_ || fingerprints_without_room_report_) {
      static constexpr const char* kPhases[] = {"apagada", "MIRANDO", "APLICANDO"};
      NFSMW_RING_REPORT(
          "[nativo] C3 huellas de texturas nuevas (build 185), ultimos 10 s: {} planeadas ({} de comparacion); el hilo "
          "preparo {} en {:.1f} ms (huella {:.1f} ms) y el anillo {} al enviar en {:.1f} ms; el anillo copio {:.1f} MB "
          "de instantaneas en {:.1f} ms y espero al hilo {:.1f} ms en {} veces (peor {:.2f} ms); {} por la ruta de "
          "siempre sin sitio; {} comparadas desde el principio ({} sin poder); hilo en el nucleo {}, prioridad "
          "{:#x}{}; fase {}",
          planned_fingerprints_report_, comparison_fingerprints_report_, thread_fingerprints_report_,
          double(thread_fingerprints_report_ns_) / 1e6, double(thread_raw_fingerprint_report_ns_) / 1e6,
          ring_fingerprints_report_, double(ring_fingerprints_report_ns_) / 1e6,
          double(snapshots_report_bytes_) / 1048576.0, double(snapshots_report_ns_) / 1e6,
          double(fingerprints_wait_report_ns_) / 1e6, fingerprints_waits_report_, double(worst_fingerprints_wait_ns_) / 1e6,
          fingerprints_without_room_report_, compared_fingerprints_, fingerprints_without_compare_, core, priority_fingerprints_,
          priority_ok ? "" : " (el kernel NO acepto la prioridad)", kPhases[std::clamp<int32_t>(phase_fingerprints_, 0, 2)]);
    }
    if (phase_fingerprints_ == kApplyingFingerprints && thread_fingerprints_report_ + ring_fingerprints_report_ >= 16) {
      const bool compensates = thread_fingerprints_report_ns_ > snapshots_report_ns_ + fingerprints_wait_report_ns_;
      reports_without_fingerprints_compensate_ = compensates ? 0 : reports_without_fingerprints_compensate_ + 1;
      if (reports_without_fingerprints_compensate_ >= 3) {
        DisableFingerprints(fmt::format("no compensa: en 3 informes seguidos el hilo le quito al anillo menos de lo que el "
                                  "anillo gasto copiando y esperandolo (el ultimo: hilo {:.1f} ms, copias {:.1f} ms, "
                                  "espera {:.1f} ms)",
                                  double(thread_fingerprints_report_ns_) / 1e6, double(snapshots_report_ns_) / 1e6,
                                  double(fingerprints_wait_report_ns_) / 1e6));
      }
    }
    planned_fingerprints_report_ = 0;
    comparison_fingerprints_report_ = 0;
    thread_fingerprints_report_ = 0;
    ring_fingerprints_report_ = 0;
    thread_fingerprints_report_ns_ = 0;
    thread_raw_fingerprint_report_ns_ = 0;
    ring_fingerprints_report_ns_ = 0;
    snapshots_report_ns_ = 0;
    snapshots_report_bytes_ = 0;
    fingerprints_wait_report_ns_ = 0;
    worst_fingerprints_wait_ns_ = 0;
    fingerprints_waits_report_ = 0;
    fingerprints_without_room_report_ = 0;
  }

  // --- Vertex copies on a separate thread (nfsmw_nativo_subidas_hilo) ------------------------------------------
  // One producer (the ring thread, in Dibujar) and one consumer (copias_hilo_). The producer writes the job
  // into copias_escritas_ and publishes it; the thread copies up to there and publishes copias_hechas_.
  // With the queue full the copy is done on the ring, as before. The destinations are distinct ranges of
  // the upload buffer, so order does not matter. The copies must be waited for before submitting the work
  // (AntesDeEnviar) and before returning the read pointer to the game (EsperarSubidas from
  // nfsmw_nativo_sistema.cpp): after that the game may reuse its vertex buffers.
  //
  // Sleeping and waking are decided under the lock. Two earlier versions hung: the copy thread asleep with
  // 1,123 copies queued and the ring waiting for them. Each side used to decide without the lock whether to
  // wake the other, Dekker-style: the ring published copias_escritas_ and read copias_hechas_, and the
  // thread the other way round. With store(..., memory_order_release) MSVC's STL emits a plain mov, and on
  // x86 (TSO) a store may not yet be visible from another core: both sides missed the wake-up. ARM gives no
  // guarantee either. Now copias_durmiendo_ and copias_esperando_ are only touched under the lock, and
  // nothing is decided without it: the wake-up arrives at the next point that takes the lock (every
  // kCopiasPorAviso copies, when the ring waits, and when the thread runs out of work). There is no atomic
  // hint flag any more; the thread always signals from its no-work path, under the lock, which it goes
  // through as soon as it finishes.
  // Only the queuing thread may wait (single producer). Otherwise it wakes the thread once and waits
  // without helping.
  //
  /*
   * The ring helps instead of waiting (nfsmw_nativo_subidas_ayuda).
   *
   * Why. In a race the ring waited 14.7 ms for the copy thread during one stutter (3,965 draws, 5 waits)
   * and 17.9 ms in the next (3,890 draws, 3 waits; the game spent 21.1 ms without room in the ring). Copying
   * what those frames needed is ~9-11 MB, ~6-7 ms at the rate the profile gives the thread (1.5 GB/s): the
   * rest was the thread ready and without a core. It ran at 0x3B, below the two game threads (0x3A), and
   * Horizon only moves a ready thread to another core when that core runs out of work: with the CPU at
   * 278 % out of 300 (the profile block for that stutter), almost never. And the ring (0x2D) waited for it
   * on a condition variable, which does not lend priority: a textbook priority inversion.
   *
   * How. On every wait with pending copies (by the queuing thread, in the applying phase):
   *   1. the ring takes and copies, in batches of kCopiasPorTrozo, whatever the thread has not taken yet.
   *      If the thread is running on another core they share what is left; if it has no core, the ring
   *      does it all, without waiting for it.
   *   2. whatever is missing can only be the batch the thread is working on. The ring waits by taking
   *      copias_trozo_mutex_, which the thread holds while copying: a libnx mutex waits in the kernel
   *      (svcArbitrateLock) and Horizon lends the owner the waiter's priority, so the thread rises to 0x2D
   *      until it releases it. It is one batch.
   * The thread's own priority is not changed: it takes no core from anyone while the ring is not waiting
   * for it.
   *
   * Counters. copias_escritas_ (ring only), copias_tomadas_ (the thread or the ring, with CAS, in order)
   * and copias_hechas_ (how many are done, whoever did them): hechas <= tomadas <= escritas. Outside
   * EsperarSubidas only the thread takes, so it finishes in order and "hechas = h" still means the first h
   * are done: EncolarCopia's space accounting still holds. Inside, the ring does not queue, and it leaves
   * with everything done.
   *
   * Same data. Each copy is done exactly once by CopiarVertices with the same slot (source, destination,
   * words and byte order), and all of them before returning from EsperarSubidas, as before: before
   * submitting and before returning the read pointer. Only who does it changes, and sometimes it is done
   * earlier.
   *
   * Self-checking guard. Each copy leaves its index + 1 in copias_marcas_. On each wait the ring checks the
   * marks of what was queued since the previous one and that the counters add up; a copy without a mark is
   * done right there, before submitting and before returning the read pointer, and it is a DIFERENCIA
   * (REXLOG_ERROR). Phases: observing (the first kEsperasCopiasMirando waits with pending copies: the
   * thread already takes batches, the ring waits as before and checks), applying (help) and, after a
   * DIFERENCIA with help on, no help (the plain wait). A DIFERENCIA without help, or with the counters out
   * of step, turns the thread off for the session and the copies go back to the ring (the original
   * behaviour). The plain wait no longer waits forever: after 2 s it takes the batch mutex and finishes the
   * job. The batch mutex has no timeout because inside it the thread only takes and copies.
   */
  enum CopiesPhase : int32_t { kCopiesWithoutHelp = 0, kPeekingCopies = 1, kApplyingCopies = 2 };

  void WarnCopiesOtherThread(const char* where) {
    if (!warned_thread_other_copies_) {
      warned_thread_other_copies_ = true;
      REXLOG_ERROR("[nativo] C6: {} desde un hilo que no es el que encola las copias de vertices", where);
    }
  }

  bool EnqueueCopy(const CopyJob& job) {
    if (copies_without_thread_) {
      // Thread switched off by a DIFERENCIA (or it could not be created): the copy goes on the ring.
      ++inf_copies_.in_line;
      inf_copies_.in_line_bytes += uint64_t(job.word_data) * 4;
      return false;
    }
    if (!thread_copies_.joinable()) {
      producer_copies_ = std::this_thread::get_id();
      phase_copies_ = requested_help_copies_ ? kPeekingCopies : kCopiesWithoutHelp;
#if REX_PLATFORM_SWITCH
      const int core_ring = RexSwitchCurrentCore();  // asked by the ring, which creates the thread
#else
      const int core_ring = -1;
#endif
      try {
        thread_copies_ = std::thread([this, core_ring] {
          rex::thread::set_current_thread_name("NFSMW copias de vertices");
#if REX_PLATFORM_SWITCH
          // nfsmw_nativo_subidas_hilo_prioridad (see the cvar).
          const int32_t priority = REXCVAR_GET(nfsmw_nativo_subidas_hilo_prioridad);
          const bool priority_ok = RexSwitchSetCurrentThreadPriorityOk(int(priority));
          // nfsmw_nativo_subidas_hilo_nucleo (see the cvar).
          const int32_t request = REXCVAR_GET(nfsmw_nativo_subidas_hilo_nucleo);
          const int core = request == -2 ? (core_ring == 2 ? 1 : 2) : int(request);
          const bool core_ok = core < 0 || RexSwitchSetCurrentThreadCore(core);
          REXLOG_INFO("[nativo] C6: hilo de copias de vertices a prioridad {:#x}{}; nucleo preferido {}{} (el anillo corre "
                      "en el {}), ahora en el {}",
                      priority, priority_ok ? "" : " (NO aceptada: se queda con la de su creacion)", core,
                      core_ok ? "" : " (NO aceptado)", core_ring, RexSwitchCurrentCore());
#endif
          CopiesLoop();
        });
      } catch (const std::system_error& error) {
        copies_without_thread_ = true;
        REXLOG_ERROR("[nativo] C6: no se pudo crear el hilo de copias de vertices ({}): las copias van en el anillo",
                     error.what());
        return false;
      }
      REXLOG_INFO("[nativo] C6: copias de vertices en un hilo aparte (nfsmw_nativo_subidas_hilo); el anillo las copia "
                  "el mismo al esperarlas (nfsmw_nativo_subidas_ayuda) = {}",
                  requested_help_copies_
                      ? fmt::format("SI, en trozos de {}; fase MIRANDO durante las {} primeras esperas con copias "
                                    "pendientes",
                                    kCopiesPerChunk, kPeekingCopiesWaits)
                      : std::string("no, espera al hilo como antes"));
    } else if (std::this_thread::get_id() != producer_copies_) {
      WarnCopiesOtherThread("EncolarCopia");
    }
    const size_t written_items = written_copies_.load(std::memory_order_relaxed);
    if (written_items - done_copies_.load(std::memory_order_acquire) >= copies_.size()) {
      ++copies_in_line_;
      ++inf_copies_.in_line;
      inf_copies_.in_line_bytes += uint64_t(job.word_data) * 4;
      return false;
    }
    copies_[written_items & (copies_.size() - 1)] = job;
    written_copies_.store(written_items + 1);
    ++inf_copies_.queued;
    inf_copies_.queued_bytes += uint64_t(job.word_data) * 4;
    // Wake-up every kCopiasPorAviso copies: waking the thread as soon as it slept woke it on almost every
    // copy, and every wake-up goes through the kernel (1.4 % of the ring and almost all of the copy thread's
    // CPU). Whatever is left without a wake-up is collected by EsperarSubidas.
    if (((written_items + 1) & (kCopiesPerWarning - 1)) == 0) {
      WakeCopies();
    }
    return true;
  }

  void WakeCopies() {
    bool warn;
    {
      std::lock_guard<std::mutex> mutex_lock(mutex_copies_);
      warn = sleeping_copies_;
    }
    if (warn) {
      cv_copies_.notify_one();
    }
  }

  // Fence measurement only (sistema.cpp, AnotarVallaCopias).
  size_t PendingCopies() const override {
    if (!thread_copies_.joinable()) {
      return 0;
    }
    const size_t written_items = written_copies_.load(std::memory_order_relaxed);
    const size_t done = done_copies_.load(std::memory_order_relaxed);
    return written_items > done ? written_items - done : 0;
  }

  void WaitUploads() override {
    if (!thread_copies_.joinable()) {
      return;
    }
    const bool producer = std::this_thread::get_id() == producer_copies_;
    if (!producer) {
      WarnCopiesOtherThread("EsperarSubidas");
    }
    const size_t goal = written_copies_.load(std::memory_order_relaxed);
    if (done_copies_.load(std::memory_order_acquire) == goal) {
      if (producer) {
        CheckCopies(goal);  // what the thread did since the last wait
      }
      return;
    }
    const auto before = std::chrono::steady_clock::now();
    // What the thread had not taken yet on arrival (while observing, what the ring would have copied).
    inf_copies_.without_take += goal - std::min(goal, taken_copies_.load(std::memory_order_acquire));
    uint64_t helping_ns = 0;
    uint64_t assisted = 0;
    if (producer && phase_copies_ == kApplyingCopies) {
      // 1. What the thread has not taken yet, the ring copies in batches: if the thread is running, they
      //    share it.
      uint64_t bytes = 0;
      while (TakeAndCopy(true, assisted, bytes)) {
      }
      const auto after_help = std::chrono::steady_clock::now();
      helping_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(after_help - before).count());
      assisted_copies_ += assisted;
      assisted_bytes_copies_ += bytes;
      helping_ns_copies_ += helping_ns;
      inf_copies_.assisted += assisted;
      inf_copies_.assisted_bytes += bytes;
      inf_copies_.helping_ns += helping_ns;
      if (assisted) {
        ++inf_copies_.waits_with_help;
      }
      // 2. What is missing is the batch the thread is working on: wait on its mutex, which lends the thread
      //    the ring's priority until it releases it.
      if (done_copies_.load(std::memory_order_acquire) != goal) {
        {
          std::lock_guard<std::mutex> chunk(mutex_chunk_copies_);
          if (done_copies_.load(std::memory_order_acquire) != goal) {
            RepairCopies(goal, "con el cerrojo del trozo el hilo no tiene nada entre manos y aun faltan copias");
          }
        }
        const uint64_t chunk_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now() - after_help)
                                               .count());
        ++inf_copies_.chunk_waits;
        inf_copies_.chunk_ns += chunk_ns;
        inf_copies_.worst_chunk_ns = std::max(inf_copies_.worst_chunk_ns, chunk_ns);
      }
    } else {
      WaitCopiesAsAlways(goal);
    }
    if (producer) {
      CheckCopies(goal);
      if (phase_copies_ == kPeekingCopies && ++peeking_waits_copies_ >= kPeekingCopiesWaits) {
        phase_copies_ = kApplyingCopies;
        REXLOG_INFO("[nativo] C6: copias de vertices: {} esperas con copias pendientes en fase MIRANDO y {} copias "
                    "comprobadas, todas con su marca y los contadores cuadrados. Pasa a fase APLICANDO: el anillo "
                    "copia lo que el hilo no ha tomado y espera el trozo en curso con su cerrojo",
                    peeking_waits_copies_, checked_copies_);
      }
    }
    ++copies_waits_;
    const uint64_t ns_total = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - before)
                                           .count());
    // The waiting time now counts only the wait; what the ring copies is counted separately (as helping).
    const uint64_t wait_ns = ns_total > helping_ns ? ns_total - helping_ns : 0;
    copies_waiting_ns_ += wait_ns;
    ++inf_copies_.waits;
    inf_copies_.wait_ns += wait_ns;
    inf_copies_.worst_wait_ns = std::max(inf_copies_.worst_wait_ns, wait_ns);
    // And to the "[tiron] anillo" line (stall_waits.h). This is only reached when the ring really
    // waits, a few times per frame: the fetch_add calls do not show.
    nfsmw::waits::g_copies_waiting_ns.fetch_add(wait_ns, std::memory_order_relaxed);
    nfsmw::waits::g_copies_waits.fetch_add(1, std::memory_order_relaxed);
    nfsmw::waits::g_copies_helping_ns.fetch_add(helping_ns, std::memory_order_relaxed);
    nfsmw::waits::g_assisted_copies.fetch_add(assisted, std::memory_order_relaxed);
  }

  // The plain wait (observing and no-help phases, or when another thread waits): the ring sleeps until the
  // thread finishes. After 2 s it no longer keeps waiting forever (the earlier hang): it takes the batch
  // mutex, which lends its priority to the thread if it is copying, and if copies are still missing with
  // the thread out of its loop, the ring does them and switches the thread off.
  void WaitCopiesAsAlways(size_t goal) {
    bool to_time = false;
    {
      std::unique_lock<std::mutex> mutex_lock(mutex_copies_);
      if (sleeping_copies_) {
        cv_copies_.notify_one();  // a wake-up EncolarCopia skipped (it only wakes every kCopiasPorAviso copies)
      }
      waiting_copies_ = true;
      to_time = cv_done_copies_.wait_for(mutex_lock, std::chrono::seconds(2), [this, goal] {
        return done_copies_.load(std::memory_order_acquire) == goal;
      });
      waiting_copies_ = false;
    }
    ++inf_copies_.ints_waits;
    if (to_time) {
      return;
    }
    // Should never happen: diagnostic for a hang once seen in the menus.
    REXLOG_ERROR("[nativo] C6: el hilo del anillo lleva 2 s esperando las copias de vertices: objetivo {}, hechas {}, "
                 "tomadas {}, escritas {}, copiadas por el hilo {}",
                 goal, done_copies_.load(std::memory_order_acquire),
                 taken_copies_.load(std::memory_order_acquire), written_copies_.load(std::memory_order_acquire),
                 progress_copies_.load(std::memory_order_relaxed));
    std::lock_guard<std::mutex> chunk(mutex_chunk_copies_);
    if (done_copies_.load(std::memory_order_acquire) != goal) {
      RepairCopies(goal, "2 s esperando al hilo y, con el hilo fuera de su bucle, siguen faltando copias");
    }
  }

  // Takes the next untaken batch (up to kCopiasPorTrozo copies, in order) and copies it. Used by the
  // thread, holding copias_trozo_mutex_, and by the ring when helping. false = nothing was left to take.
  bool TakeAndCopy(bool ring, uint64_t& copies, uint64_t& bytes) {
    const size_t written_items = written_copies_.load(std::memory_order_acquire);
    size_t taken = taken_copies_.load(std::memory_order_acquire);
    size_t n = 0;
    do {
      if (taken >= written_items) {
        return false;
      }
      n = std::min<size_t>(written_items - taken, kCopiesPerChunk);
    } while (!taken_copies_.compare_exchange_weak(taken, taken + n, std::memory_order_acq_rel,
                                                    std::memory_order_acquire));
    const size_t mask = copies_.size() - 1;
    for (size_t i = taken; i < taken + n; ++i) {
      const CopyJob& job = copies_[i & mask];
      CopyVertices(job);
      bytes += uint64_t(job.word_data) * 4;
      marks_copies_[i & mask].store(uint32_t(i + 1), std::memory_order_relaxed);
      if (!ring) {
        progress_copies_.store(i + 1, std::memory_order_relaxed);  // for the EsperarSubidas diagnostic
      }
    }
    copies += n;
    done_copies_.fetch_add(n, std::memory_order_acq_rel);  // publishes the copies and their marks
    return true;
  }

  bool HasCopiesWithoutTake() const {
    return taken_copies_.load(std::memory_order_acquire) != written_copies_.load(std::memory_order_acquire);
  }

  // The guard, on each wait and with everything finished (the ring does not queue while waiting and the
  // thread has nothing half taken): what was queued since the previous one has its mark and the counters
  // add up. Ring only.
  void CheckCopies(size_t goal) {
    if (goal == verified_copies_ || copies_without_thread_) {
      return;
    }
    const size_t mask = copies_.size() - 1;
    const size_t from =
        std::max(verified_copies_, goal > copies_.size() ? goal - copies_.size() : size_t(0));
    size_t without_mark = 0;
    size_t is_first = 0;
    for (size_t i = from; i < goal; ++i) {
      if (marks_copies_[i & mask].load(std::memory_order_relaxed) != uint32_t(i + 1)) {
        if (!without_mark) {
          is_first = i;
        }
        ++without_mark;
      }
    }
    checked_copies_ += goal - from;
    inf_copies_.checked += goal - from;
    const size_t taken = taken_copies_.load(std::memory_order_acquire);
    const size_t done = done_copies_.load(std::memory_order_acquire);
    if (!without_mark && taken == goal && done == goal) {
      verified_copies_ = goal;
      return;
    }
    const std::string reason = without_mark ? fmt::format("{} de {} copias sin su marca (la primera, la {})", without_mark,
                                                       goal - from, is_first)
                                         : std::string("todas con su marca, pero los contadores no cuadran");
    std::lock_guard<std::mutex> chunk(mutex_chunk_copies_);  // the thread, out of its copy loop
    if (phase_copies_ == kApplyingCopies && taken == goal && done == goal) {
      // With the counters right, the suspect is the help path: redo whatever is missing and go back to the
      // plain wait.
      const size_t redone = RedoWithoutMark(goal);
      verified_copies_ = goal;
      ++differences_copies_;
      phase_copies_ = kCopiesWithoutHelp;
      REXLOG_ERROR("[nativo] C6: copias de vertices: DIFERENCIA: {} ({} encoladas, contadores cuadrados; {} rehechas "
                   "en el anillo antes de enviar). Ayuda del anillo APAGADA para el resto de la sesion: espera al hilo "
                   "como antes",
                   reason, goal, redone);
      return;
    }
    RepairCopies(goal, reason);
  }

  // Copies on the ring whatever was queued up to `objetivo` (since the last check, and at most one lap of
  // the queue) that lacks its mark, and marks it. With copias_trozo_mutex_ held. Returns how many.
  size_t RedoWithoutMark(size_t goal) {
    const size_t mask = copies_.size() - 1;
    const size_t from =
        std::max(verified_copies_, goal > copies_.size() ? goal - copies_.size() : size_t(0));
    size_t redone = 0;
    for (size_t i = from; i < goal; ++i) {
      if (marks_copies_[i & mask].load(std::memory_order_relaxed) != uint32_t(i + 1)) {
        CopyVertices(copies_[i & mask]);
        marks_copies_[i & mask].store(uint32_t(i + 1), std::memory_order_relaxed);
        ++redone;
      }
    }
    return redone;
  }

  // Should never happen. With copias_trozo_mutex_ held, i.e. with the thread out of its copy loop and
  // nothing half taken: the ring copies whatever is missing up to `objetivo` (before submitting and before
  // returning the read pointer), the counters are left consistent and the thread is switched off for the
  // session.
  void RepairCopies(size_t goal, const std::string& reason) {
    const size_t taken = taken_copies_.load(std::memory_order_acquire);
    const size_t done = done_copies_.load(std::memory_order_acquire);
    const size_t redone = RedoWithoutMark(goal);
    taken_copies_.store(goal, std::memory_order_release);
    done_copies_.store(goal, std::memory_order_release);
    verified_copies_ = goal;
    ++differences_copies_;
    copies_without_thread_ = true;
    REXLOG_ERROR("[nativo] C6: copias de vertices: DIFERENCIA: {} (encoladas {}, tomadas {}, hechas {}; {} rehechas en "
                 "el anillo antes de enviar). Hilo de copias APAGADO para el resto de la sesion: las copias van en el "
                 "anillo, como antes de la build 90",
                 reason, goal, taken, done, redone);
  }

  const char* CopiesPhaseName() const {
    if (copies_without_thread_) {
      return "SIN HILO (copias en el anillo)";
    }
    return phase_copies_ == kApplyingCopies ? "APLICANDO" : phase_copies_ == kPeekingCopies ? "MIRANDO" : "SIN AYUDA";
  }

  // Every 10 s, if there were copies: who did them and how long the ring really waited. Ring only.
  void CopiesReport() {
    if (!thread_copies_.joinable() && !copies_without_thread_) {
      return;
    }
    const auto now_time = std::chrono::steady_clock::now();
    if (now_time - copies_report_ < std::chrono::seconds(10)) {
      return;
    }
    copies_report_ = now_time;
    const uint64_t n_thread = n_thread_copies_.load(std::memory_order_relaxed);
    const uint64_t thread_bytes = thread_copies_bytes_.load(std::memory_order_relaxed);
    const uint64_t d_n_thread = n_thread - n_thread_copies_previous_;
    const uint64_t d_thread_bytes = thread_bytes - thread_copies_previous_bytes_;
    n_thread_copies_previous_ = n_thread;
    thread_copies_previous_bytes_ = thread_bytes;
    const CopiesReportFigures& c = inf_copies_;
    if (c.queued || c.in_line || c.waits) {
      NFSMW_RING_REPORT(
          "[nativo] C6 copias de vertices (build 185), ultimos 10 s: {} encoladas ({:.1f} MB) y {} en el anillo sin "
          "pasar por la cola ({:.1f} MB); el hilo copio {} ({:.1f} MB) y el anillo ayudo con {} ({:.1f} MB, {:.1f} ms) "
          "en {} de {} esperas con copias pendientes ({} sin tomar al llegar); espero el trozo del hilo {} veces "
          "({:.2f} ms, peor {:.2f} ms) y entero {} veces; espera pura {:.1f} ms (peor {:.2f} ms); {} copias "
          "comprobadas, {} diferencias; fase {}",
          c.queued, double(c.queued_bytes) / 1048576.0, c.in_line, double(c.in_line_bytes) / 1048576.0,
          d_n_thread, double(d_thread_bytes) / 1048576.0, c.assisted, double(c.assisted_bytes) / 1048576.0,
          double(c.helping_ns) / 1e6, c.waits_with_help, c.waits, c.without_take, c.chunk_waits,
          double(c.chunk_ns) / 1e6, double(c.worst_chunk_ns) / 1e6, c.ints_waits, double(c.wait_ns) / 1e6,
          double(c.worst_wait_ns) / 1e6, c.checked, differences_copies_, CopiesPhaseName());
    }
    inf_copies_ = CopiesReportFigures{};
  }

  // The per-fetch-constant sampler table, every 10 s: hits, and misses by cause.
  void CacheFetchReport() {
    const auto now_time = std::chrono::steady_clock::now();
    if (now_time - fetch_report_ < std::chrono::seconds(10)) {
      return;
    }
    fetch_report_ = now_time;
    const std::array<uint64_t, 6> figures_now{samplers_cache_fetch_, collision_failures_fetch_, empty_failures_fetch_,
                                               generation_failures_fetch_, expired_failures_fetch_, samplers_cache_};
    std::array<uint64_t, 6> d{};
    for (size_t i = 0; i < d.size(); ++i) {
      d[i] = figures_now[i] - fetch_report_previous_[i];
    }
    fetch_report_previous_ = figures_now;
    const uint64_t failures = d[1] + d[2] + d[3] + d[4];
    if (d[0] + failures == 0) {
      return;
    }
    NFSMW_RING_REPORT(
        "[nativo] C6 cache de samplers por fetch (build 192, {} casillas), ultimos 10 s: {} aciertos del registro, {} "
        "busquedas en la tabla con {} aciertos y {} fallos ({:.1f} %): {} choques con otra fetch en su casilla, {} en "
        "casilla vacia, {} por generacion y {} caducadas (toca comprobar la textura)",
        cache_fetch_.size(), d[5], d[0] + failures, d[0], failures,
        100.0 * double(failures) / double(std::max<uint64_t>(d[0] + failures, 1)), d[1], d[2], d[3], d[4]);
  }

  void StopCopies() {
    if (!thread_copies_.joinable()) {
      return;
    }
    {
      std::lock_guard<std::mutex> mutex_lock(mutex_copies_);
      stop_copies_ = true;
    }
    cv_copies_.notify_one();
    thread_copies_.join();
    REXLOG_INFO("[nativo] C6: hilo de copias de vertices parado ({} copias en el anillo por cola llena, {} "
                "esperas del anillo, {:.1f} ms esperando; 26/09 (build 185): el anillo copio {} al esperar ({:.1f} MB, "
                "{:.1f} ms), {} comprobadas, {} diferencias, fase {})",
                copies_in_line_, copies_waits_, double(copies_waiting_ns_) / 1e6, assisted_copies_,
                double(assisted_bytes_copies_) / 1048576.0, double(helping_ns_copies_) / 1e6, checked_copies_,
                differences_copies_, CopiesPhaseName());
  }

  void CopiesLoop() {
    for (;;) {
      if (HasCopiesWithoutTake()) {
        // Holding copias_trozo_mutex_ from the first batch until there is nothing left to take, and never
        // waiting on anything inside. If the ring waits for the current batch, it waits on this mutex and the
        // thread runs at the ring's priority until it releases it.
        uint64_t copies = 0;
        uint64_t bytes = 0;
        {
          std::lock_guard<std::mutex> chunk(mutex_chunk_copies_);
          while (TakeAndCopy(false, copies, bytes)) {
          }
        }
        n_thread_copies_.fetch_add(copies, std::memory_order_relaxed);
        thread_copies_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        continue;  // look again: if nothing is left, sleep through the path below
      }
      // Nothing to take (the ring may have taken the last ones). Under the lock: wake the ring if it waits,
      // and sleep until there is more. If the read of copias_escritas_ was stale, the ring signals at its next
      // point that takes the lock.
      std::unique_lock<std::mutex> mutex_lock(mutex_copies_);
      if (waiting_copies_) {
        cv_done_copies_.notify_one();
      }
      sleeping_copies_ = true;
      cv_copies_.wait(mutex_lock, [this] { return stop_copies_ || HasCopiesWithoutTake(); });
      sleeping_copies_ = false;
      /*
       * The thread used to die on its own. It exited with "if (!HayCopiasSinTomar()) return;", on the
       * assumption that it was only woken without work in order to stop. But the ring takes untaken copies
       * when it waits for them (TomarYCopiar, without copias_mutex_): if the thread woke because there were
       * copies and the ring took them before this check, the thread ended for good. It was seen at the
       * Heritage & Omega exit with the ring at 94 %: from then on "el hilo copio 0" in every report, the
       * menu included, and the ring did all the copies (3-4 ms more per frame for the rest of the session).
       * Now it only exits when asked to stop; otherwise it looks again.
       */
      if (stop_copies_ && !HasCopiesWithoutTake()) {
        return;  // stop, nothing pending
      }
    }
  }

  // nfsmw_tratamiento_visual. If the draw is the final composite and the filter is not the original one, uploads its
  // own copy of the constants with the color curve changed, and leaves the cache empty so the next draw uploads the
  // game's. If there is no room in the upload buffer (Dibujar does not count this copy), the frame keeps the original
  // filter.
  // It is out of line and Dibujar always calls it: that way Dibujar gains no branch and its control flow is still the
  // one in the PGO profile. With a branch inside, GCC would drop the whole profile of the ring's hottest function.
  [[gnu::noinline]] void TreatedCompositionCopy(const ShaderEntry* ps, const uint32_t* r,
                                                 uint32_t bytes_ps) noexcept {
    if (!ps || !ps->shader || ps->shader->fingerprint != kCompositionFingerprint) {
      return;
    }
    const int mode = nfsmw::settings::VisualHandling();
    // One line per mode change, on the first draw of the composite: the values the game sets.
    static std::atomic<int> tracked_mode{-1};
    if (tracked_mode.exchange(mode, std::memory_order_relaxed) != mode) {
      const float* k = reinterpret_cast<const float*>(r + kRegConstantesPs);
      REXLOG_INFO("[nativo] filtro de color {}: composicion con {} bytes de constantes. El juego pone Coeffs0 ({:.3f}, "
                  "{:.3f}, {:.3f}, {:.3f}), Coeffs1 ({:.3f}, {:.3f}, {:.3f}, {:.3f}), Desaturation {:.3f} y vineta {:.3f}",
                  mode, bytes_ps, k[24], k[25], k[26], k[27], k[28], k[29], k[30], k[31], k[8], k[4]);
    }
    if (bytes_ps < kHandlingBytes) {
      return;
    }
    VkDeviceSize offset = 0;
    if (mode == 0 || !Reserve(use_ubo_ ? std::max<VkDeviceSize>(bytes_ps, kUboBytesPs) : bytes_ps,
                               use_ubo_ ? ubo_alignment_ : 16, offset)) {
      return;
    }
    std::memcpy(data_upload_ + offset, r + kRegConstantesPs, bytes_ps);
    ApplyVisualTreatment(reinterpret_cast<float*>(data_upload_ + offset), mode);
    constantes_ps_offset_ = offset;
    constantes_ps_generacion_ = UINT64_MAX;
  }

  bool Reserve(VkDeviceSize bytes, VkDeviceSize align_bytes, VkDeviceSize& offset) {
    const VkDeviceSize start = (used_upload_ + align_bytes - 1) & ~(align_bytes - 1);
    if (start + bytes > kUploadSize) {
      offset = 0;
      return false;  // cannot happen: Dibujar checks the space first
    }
    offset = start;
    used_upload_ = start + bytes;
    megas_bytes_ += bytes;
    return true;
  }

  // Like Reservar, but the offset is a multiple of `multiplo` (the stride: a multiple of 4, not always a
  // power of 2). Dibujar has already asked for space including that extra padding (hueco_base_cero).
  bool ReserveMultiple(VkDeviceSize bytes, VkDeviceSize multiple, VkDeviceSize& offset) {
    const VkDeviceSize start = (used_upload_ + multiple - 1) / multiple * multiple;
    if (start + bytes > kUploadSize) {
      offset = 0;
      return false;  // cannot happen: Dibujar checks the space first
    }
    offset = start;
    used_upload_ = start + bytes;
    megas_bytes_ += bytes;
    return true;
  }

  // The pipelines file (cache/nfsmw_nativo_pipelines.bin): the Vulkan cache and the prewarm list.
  static std::filesystem::path PipelinesFilePath() {
    return rex::filesystem::GetExecutableFolder() / kCacheFolder / kPipelinesFile;
  }

  static void ReadIntFile(const std::filesystem::path& file_path, std::vector<uint8_t>& payload) {
    std::ifstream file_name(file_path, std::ios::binary | std::ios::ate);
    const std::streamoff bytes = file_name ? std::streamoff(file_name.tellg()) : 0;
    if (bytes > 0 && bytes < (std::streamoff(256) << 20)) {
      payload.resize(size_t(bytes));
      file_name.seekg(0);
      if (!file_name.read(reinterpret_cast<char*>(payload.data()), std::streamsize(bytes))) {
        payload.clear();
      }
    }
  }

  // Reads the pipelines file and splits it into the cache and the list. If it does not exist yet, the two
  // older files (next to the NRO), which are deleted when the new one is written.
  void ReadPipelinesFile(std::vector<uint8_t>& cache, std::vector<uint8_t>& item_list) {
    std::vector<uint8_t> all;
    ReadIntFile(PipelinesFilePath(), all);
    if (!all.empty()) {
      uint32_t magic = 0;
      uint32_t version = 0;
      uint64_t list_bytes = 0;
      uint64_t bytes_cache = 0;
      if (all.size() >= kPipelinesFileHeader) {
        std::memcpy(&magic, all.data(), 4);
        std::memcpy(&version, all.data() + 4, 4);
        std::memcpy(&list_bytes, all.data() + 8, 8);
        std::memcpy(&bytes_cache, all.data() + 16, 8);
      }
      if (magic != kPipelinesFileMagic || version != kPipelinesFileVersion ||
          list_bytes > all.size() || bytes_cache > all.size() ||
          kPipelinesFileHeader + list_bytes + bytes_cache != all.size()) {
        REXLOG_WARN("[nativo] C6: fichero de pipelines de otra version o danado: se empieza de cero");
        return;
      }
      const auto start = all.begin() + kPipelinesFileHeader;
      item_list.assign(start, start + std::ptrdiff_t(list_bytes));
      cache.assign(start + std::ptrdiff_t(list_bytes), all.end());
      return;
    }
    const std::filesystem::path dir_name = rex::filesystem::GetExecutableFolder();
    ReadIntFile(dir_name / kOldCacheFile, cache);
    ReadIntFile(dir_name / kOldListFile, item_list);
    old_files_ = !cache.empty() || !item_list.empty();
  }

  // On-disk pipeline cache (derived from the game's shaders: not distributed). If the driver rejects it
  // (another driver or corrupt data), it starts from scratch.
  void LoadCachePipelines() {
    const auto& ifn = vk_device_->vulkan_instance()->functions();
    const auto create = reinterpret_cast<CreatePipelineCacheFn>(
        ifn.vkGetDeviceProcAddr(device_, "vkCreatePipelineCache"));
    cache_data_ = reinterpret_cast<PipelineCacheDataFn>(
        ifn.vkGetDeviceProcAddr(device_, "vkGetPipelineCacheData"));
    destroy_cache_ = reinterpret_cast<DestroyPipelineCacheFn>(
        ifn.vkGetDeviceProcAddr(device_, "vkDestroyPipelineCache"));
    if (!create || !cache_data_ || !destroy_cache_) {
      REXLOG_WARN("[nativo] C6: sin cache de pipelines (el driver no da sus funciones)");
      return;
    }
    const std::filesystem::path file_path = PipelinesFilePath();
    std::vector<uint8_t> payload;
    ReadPipelinesFile(payload, read_list_);
    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.initialDataSize = payload.size();
    info.pInitialData = payload.empty() ? nullptr : payload.data();
    if (create(device_, &info, nullptr, &cache_pipelines_) != VK_SUCCESS) {
      info.initialDataSize = 0;
      info.pInitialData = nullptr;
      payload.clear();
      if (create(device_, &info, nullptr, &cache_pipelines_) != VK_SUCCESS) {
        cache_pipelines_ = VK_NULL_HANDLE;
      }
    }
    saved_cache_bytes_ = payload.size();
    saved_cache_ = std::chrono::steady_clock::now();
    REXLOG_INFO("[nativo] C6: cache de pipelines {} ({} KB leidos de {}{})",
                cache_pipelines_ != VK_NULL_HANDLE ? "activa" : "no disponible",
                payload.size() >> 10, file_path.string(), old_files_ ? ", de los dos ficheros de antes" : "");
    // The writer thread always saves both parts; it starts from what is on disk.
    cache_written_ = std::move(payload);
    if (old_files_) {
      // they came from the two older files: moved to the new one on the first save even if nothing changed
      saved_cache_bytes_ = 0;
      pipelines_without_save_ = 1;
      list_without_save_ = 1;
    }
    LoadPipelinesList();  // the prewarm list
  }

  void SaveCachePipelines() {
    saved_cache_ = std::chrono::steady_clock::now();
    // The prewarm list is saved even if the cache does not grow (a new pipeline whose shaders were already
    // in it does not make it grow). The cache, as usual: only if it has new entries.
    std::vector<uint8_t> item_list;
    if (list_without_save_) {
      list_without_save_ = 0;
      item_list = SerializePipelinesList();
    }
    std::vector<uint8_t> payload;
    if (pipelines_without_save_) {
      pipelines_without_save_ = 0;
      size_t bytes = 0;
      // without new entries it is not rewritten (the SD card is slow on the Switch)
      if (cache_pipelines_ != VK_NULL_HANDLE &&
          cache_data_(device_, cache_pipelines_, &bytes, nullptr) == VK_SUCCESS && bytes &&
          bytes != saved_cache_bytes_) {
        payload.resize(bytes);
        const VkResult result = cache_data_(device_, cache_pipelines_, &bytes, payload.data());
        if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || !bytes) {
          payload.clear();
        } else {
          payload.resize(bytes);
          saved_cache_bytes_ = bytes;
        }
      }
    }
    if (payload.empty() && item_list.empty()) {
      return;
    }
    // The file is written on its own thread: on the Switch, writing about 2 MB to the SD card from the ring
    // thread stalled it on every save. If the previous one has not been written yet, this one replaces it.
    std::lock_guard<std::mutex> mutex_lock(mutex_writer_);
    if (!cache_writer_.joinable()) {
      cache_writer_ = std::thread([this] {
        rex::thread::set_current_thread_name("NFSMW cache de pipelines");
        MainCacheWriter();
      });
    }
    if (!payload.empty()) {  // only the list, if the cache did not grow
      data_writer_ = std::move(payload);
    }
    if (!item_list.empty()) {
      list_writer_ = std::move(item_list);
    }
    pending_writer_ = true;
    warning_writer_.notify_one();
  }

  // Pipeline cache writer thread. Sleeping or continuing is decided with the lock held.
  void MainCacheWriter() {
    std::vector<uint8_t> payload;
    std::vector<uint8_t> item_list;  // the prewarm one
    for (;;) {
      {
        std::unique_lock<std::mutex> mutex_lock(mutex_writer_);
        warning_writer_.wait(mutex_lock, [this] { return pending_writer_ || stop_writer_; });
        if (!pending_writer_) {
          return;  // stop, nothing pending
        }
        payload = std::move(data_writer_);
        data_writer_ = {};
        item_list = std::move(list_writer_);
        list_writer_ = {};
        pending_writer_ = false;
      }
      // A single file with both parts. Only one may arrive (the cache did not grow, or the list did not
      // change): the other is the last one written.
      if (!payload.empty()) {
        cache_written_ = std::move(payload);
      }
      if (!item_list.empty()) {
        list_written_ = std::move(item_list);
      }
      WritePipelinesFile();
    }
  }

  // cache/nfsmw_nativo_pipelines.bin with the list and the cache, written to a temporary file that is then
  // renamed. Writer thread only. The first time it is written, the two older files are deleted if present.
  void WritePipelinesFile() {
    const auto before = std::chrono::steady_clock::now();
    const std::filesystem::path file_path = PipelinesFilePath();
    std::error_code error;
    std::filesystem::create_directories(file_path.parent_path(), error);
    std::filesystem::path temporal = file_path;
    temporal += ".tmp";
    {
      const uint32_t magic = kPipelinesFileMagic;
      const uint32_t version = kPipelinesFileVersion;
      const uint64_t list_bytes = list_written_.size();
      const uint64_t bytes_cache = cache_written_.size();
      std::ofstream file_name(temporal, std::ios::binary | std::ios::trunc);
      if (!file_name || !file_name.write(reinterpret_cast<const char*>(&magic), 4) ||
          !file_name.write(reinterpret_cast<const char*>(&version), 4) ||
          !file_name.write(reinterpret_cast<const char*>(&list_bytes), 8) ||
          !file_name.write(reinterpret_cast<const char*>(&bytes_cache), 8) ||
          !file_name.write(reinterpret_cast<const char*>(list_written_.data()), std::streamsize(list_bytes)) ||
          !file_name.write(reinterpret_cast<const char*>(cache_written_.data()), std::streamsize(bytes_cache))) {
        return;
      }
    }
    error.clear();
    std::filesystem::rename(temporal, file_path, error);
    if (error) {
      std::filesystem::remove(file_path, error);
      std::filesystem::rename(temporal, file_path, error);
    }
    if (!error && old_files_) {
      const std::filesystem::path dir_name = rex::filesystem::GetExecutableFolder();
      std::error_code without_importance;
      std::filesystem::remove(dir_name / kOldCacheFile, without_importance);
      std::filesystem::remove(dir_name / kOldListFile, without_importance);
      old_files_ = false;
    }
    if (cache_saved_++ < 8) {
      REXLOG_INFO("[nativo] C6: pipelines guardados ({} KB de cache y {} en la lista del precalentado, en {} ms, desde "
                  "su hilo{})",
                  cache_written_.size() >> 10,
                  list_written_.size() >= kListHeader
                      ? (list_written_.size() - kListHeader) / sizeof(PipelineRecord)
                      : 0,
                  std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - before)
                      .count(),
                  error ? ", no se pudo renombrar" : "");
    }
  }

  void StopCacheWriter() {
    {
      std::lock_guard<std::mutex> mutex_lock(mutex_writer_);
      stop_writer_ = true;
      warning_writer_.notify_one();
    }
    if (cache_writer_.joinable()) {
      cache_writer_.join();
    }
  }

  // One upload buffer per work slot of the render target code: the previous frame's may still be in use
  // on the GPU while the other is filled.
  bool CreateUpload() {
    for (UploadBuffer& s : uploads_) {
      const bool was_created = CreateUploadBuffer();
      s = {upload_, memory_upload_, real_size_upload_, data_upload_, address_upload_};
      upload_ = VK_NULL_HANDLE;
      memory_upload_ = VK_NULL_HANDLE;
      data_upload_ = nullptr;
      if (!was_created) {
        return false;
      }
    }
    const auto& types = vk_device_->memory_types();
    REXLOG_INFO("[nativo] C6: bufer de subida en el tipo de memoria {} ({}, {}; nfsmw_nativo_subida_memoria = {})",
                type_upload_, ((types.host_cached >> type_upload_) & 1) ? "con cache de CPU" : "sin cache de CPU",
                coherent_upload_ ? "coherente, sin publicar" : "se publica antes de enviar",
                REXCVAR_GET(nfsmw_nativo_subida_memoria));
    MeasureUploadMemory();
    CreateSharedSeparately();
    UseSlot(0);
    return true;
  }

  // A small CPU-cached buffer per slot, only for the shared constants. If anything fails, it continues as
  // before (in the upload buffer) and says so in the log.
  void CreateSharedSeparately() {
    separately_shared_ = false;
    if (!REXCVAR_GET(nfsmw_nativo_compartidas_cache)) {
      REXLOG_INFO("[nativo] C6: constantes compartidas en el bufer de subida (nfsmw_nativo_compartidas_cache = false)");
      return;
    }
    const auto& types = vk_device_->memory_types();
    for (UploadBuffer& c : bufs_shared_) {
      VkBufferCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      info.size = kSharedSize;
      info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
      info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      if (dfn_.vkCreateBuffer(device_, &info, nullptr, &c.buf) != VK_SUCCESS) {
        REXLOG_WARN("[nativo] C6: no se pudo crear el bufer de constantes compartidas; van en el de subida");
        return;
      }
      VkMemoryRequirements requirements;
      dfn_.vkGetBufferMemoryRequirements(device_, c.buf, &requirements);
      uint32_t type = 0;
      if (!rex::bit_scan_forward(requirements.memoryTypeBits & types.host_visible & types.host_cached, &type)) {
        REXLOG_WARN("[nativo] C6: no hay memoria visible con cache de CPU; las constantes compartidas van en el de subida");
        return;
      }
      VkMemoryAllocateInfo reservation{};
      reservation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      reservation.allocationSize = requirements.size;
      reservation.memoryTypeIndex = type;
      if (dfn_.vkAllocateMemory(device_, &reservation, nullptr, &c.mem) != VK_SUCCESS ||
          dfn_.vkBindBufferMemory(device_, c.buf, c.mem, 0) != VK_SUCCESS) {
        REXLOG_WARN("[nativo] C6: no se pudo reservar el bufer de constantes compartidas; van en el de subida");
        return;
      }
      void* mapped = nullptr;
      if (dfn_.vkMapMemory(device_, c.mem, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        REXLOG_WARN("[nativo] C6: no se pudo mapear el bufer de constantes compartidas; van en el de subida");
        return;
      }
      c.payload = static_cast<uint8_t*>(mapped);
      c.real_size = requirements.size;
      type_shared_ = type;
    }
    coherent_shared_ = (types.host_coherent >> type_shared_) & 0x1;
    separately_shared_ = true;
    REXLOG_INFO("[nativo] C6: constantes compartidas aparte, en el tipo de memoria {} (con cache de CPU, {}), {} MB "
                "por ranura",
                type_shared_, coherent_shared_ ? "coherente" : "se publica antes de enviar",
                kSharedSize >> 20);
  }

  bool CreateUploadBuffer() {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = kUploadSize;
    // UNIFORM_BUFFER because it is also bound as a dynamic UBO (constants through UBOs, set 4).
    info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (dfn_.vkCreateBuffer(device_, &info, nullptr, &upload_) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements requirements;
    dfn_.vkGetBufferMemoryRequirements(device_, upload_, &requirements);
    // nfsmw_nativo_subida_memoria: with 1 or 2, a host-visible type with or without CPU caching is looked
    // for; if there is none, the SDK's.
    const auto& types = vk_device_->memory_types();
    type_upload_ = UINT32_MAX;
    const int32_t preference = REXCVAR_GET(nfsmw_nativo_subida_memoria);
    if (preference == 1 || preference == 2) {
      const uint32_t visible = requirements.memoryTypeBits & types.host_visible;
      uint32_t type = 0;
      if (rex::bit_scan_forward(visible & (preference == 1 ? types.host_cached : ~types.host_cached), &type)) {
        type_upload_ = type;
      }
    }
    if (type_upload_ == UINT32_MAX) {
      type_upload_ = rex::ui::vulkan::util::ChooseMemoryType(types, requirements.memoryTypeBits,
                                                             rex::ui::vulkan::util::MemoryPurpose::kUpload);
    }
    if (type_upload_ == UINT32_MAX) {
      return false;
    }
    coherent_upload_ = (vk_device_->memory_types().host_coherent >> type_upload_) & 0x1;
    VkMemoryAllocateFlagsInfo flag_bits{};
    flag_bits.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flag_bits.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo reservation{};
    reservation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    reservation.pNext = &flag_bits;
    reservation.allocationSize = requirements.size;
    reservation.memoryTypeIndex = type_upload_;
    if (dfn_.vkAllocateMemory(device_, &reservation, nullptr, &memory_upload_) != VK_SUCCESS) {
      return false;
    }
    real_size_upload_ = requirements.size;
    if (dfn_.vkBindBufferMemory(device_, upload_, memory_upload_, 0) != VK_SUCCESS) {
      return false;
    }
    void* mapped = nullptr;
    if (dfn_.vkMapMemory(device_, memory_upload_, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
      return false;
    }
    data_upload_ = static_cast<uint8_t*>(mapped);
    VkBufferDeviceAddressInfo address{};
    address.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    address.buffer = upload_;
    address_upload_ = buffer_address_(device_, &address);
    return address_upload_ != 0;
  }

  // For the nfsmw_nativo_subida_memoria A/B test: CPU write MB/s into each host-visible memory type and the
  // cost of publishing those 8 MB (vkFlushMappedMemoryRanges; in NVK for Tegra, armDCacheClean). Once.
  void MeasureUploadMemory() {
    if (!REXCVAR_GET(nfsmw_nativo_diag_memoria_subida)) {
      return;
    }
    constexpr VkDeviceSize kBytes = VkDeviceSize(8) << 20;
    std::vector<uint8_t> src(static_cast<size_t>(kBytes));
    for (size_t i = 0; i < src.size(); ++i) {
      src[i] = uint8_t((i * 131) ^ (i >> 11));
    }
    const auto& types = vk_device_->memory_types();
    std::string report;
    for (uint32_t type = 0; type < 32; ++type) {
      if (!((types.host_visible >> type) & 1)) {
        continue;
      }
      VkMemoryAllocateInfo reservation{};
      reservation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      reservation.allocationSize = kBytes;
      reservation.memoryTypeIndex = type;
      VkDeviceMemory mem = VK_NULL_HANDLE;
      if (dfn_.vkAllocateMemory(device_, &reservation, nullptr, &mem) != VK_SUCCESS) {
        continue;
      }
      void* mapped = nullptr;
      if (dfn_.vkMapMemory(device_, mem, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
        using Clock = std::chrono::steady_clock;
        std::memcpy(mapped, src.data(), size_t(kBytes));  // first pass: pages and caches
        const auto before = Clock::now();
        std::memcpy(mapped, src.data(), size_t(kBytes));
        const auto copied = Clock::now();
        const bool coherente = (types.host_coherent >> type) & 1;
        if (!coherente) {
          VkMappedMemoryRange range_info{};
          range_info.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
          range_info.memory = mem;
          range_info.size = VK_WHOLE_SIZE;
          dfn_.vkFlushMappedMemoryRanges(device_, 1, &range_info);
        }
        const auto published = Clock::now();
        const double secs = std::chrono::duration<double>(copied - before).count();
        report += fmt::format(
            "{}tipo {} ({}{}): {:.0f} MB/s{}", report.empty() ? "" : "; ", type,
            ((types.host_cached >> type) & 1) ? "con cache de CPU" : "sin cache de CPU", coherente ? ", coherente" : "",
            secs > 0 ? 8.0 / secs : 0.0,
            coherente ? std::string()
                      : fmt::format(", publicar {:.2f} ms",
                                    std::chrono::duration<double, std::milli>(published - copied).count()));
        dfn_.vkUnmapMemory(device_, mem);
      }
      dfn_.vkFreeMemory(device_, mem, nullptr);
    }
    REXLOG_INFO("[nativo] C6: escritura desde la CPU por tipo de memoria (8 MB): {}",
                report.empty() ? std::string("sin tipos medibles") : report);
  }

  bool CreateDescriptors() {
    static constexpr VkDescriptorType kTypes[4] = {
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER};
    const VkDescriptorBindingFlags binding_flags =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    for (uint32_t i = 0; i < 4; ++i) {
      VkDescriptorSetLayoutBinding bind_index{};
      bind_index.binding = 0;
      bind_index.descriptorType = kTypes[i];
      bind_index.descriptorCount = kHeapCapacity[i];
      bind_index.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
      VkDescriptorSetLayoutBindingFlagsCreateInfo flag_bits{};
      flag_bits.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
      flag_bits.bindingCount = 1;
      flag_bits.pBindingFlags = &binding_flags;
      VkDescriptorSetLayoutCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
      info.pNext = &flag_bits;
      info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
      info.bindingCount = 1;
      info.pBindings = &bind_index;
      if (dfn_.vkCreateDescriptorSetLayout(device_, &info, nullptr, &layouts_[i]) != VK_SUCCESS) {
        return false;
      }
      heaps_[i].cap_bytes = kHeapCapacity[i];
    }
    const VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         kHeapCapacity[0] + kHeapCapacity[1] + kHeapCapacity[2]},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kHeapCapacity[3]}};
    VkDescriptorPoolCreateInfo info_pool{};
    info_pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info_pool.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    info_pool.maxSets = 4;
    info_pool.poolSizeCount = 2;
    info_pool.pPoolSizes = sizes;
    if (dfn_.vkCreateDescriptorPool(device_, &info_pool, nullptr, &pool_) != VK_SUCCESS) {
      return false;
    }
    VkDescriptorSetAllocateInfo reservation{};
    reservation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    reservation.descriptorPool = pool_;
    reservation.descriptorSetCount = 4;
    reservation.pSetLayouts = layouts_.data();
    if (dfn_.vkAllocateDescriptorSets(device_, &reservation, sets_.data()) != VK_SUCCESS) {
      return false;
    }
    // --- Set 4, the constants through dynamic UBOs ------------------------------------------------------
    // Always created: the shaders of the current library use it statically even with the bit off (it is
    // then bound with offsets 0). With an older library it is unnecessary and harmless. No
    // UPDATE_AFTER_BIND: dynamic descriptors do not support it. CrearSubida runs first, so the buffers
    // already exist.
    use_ubo_ = REXCVAR_GET(nfsmw_nativo_constantes_ubo);
    cache_between_frames_ = REXCVAR_GET(nfsmw_nativo_cache_texturas_entre_fotogramas);
    REXLOG_INFO("[nativo] C6: caches de texturas entre fotogramas (nfsmw_nativo_cache_texturas_entre_fotogramas) = {}",
                cache_between_frames_ ? "SI" : "no");
    mipmaps_ = REXCVAR_GET(nfsmw_nativo_mipmaps);
    REXLOG_INFO("[nativo] C3: niveles de mip de las texturas (nfsmw_nativo_mipmaps) = {}", mipmaps_ ? "SI" : "no");
    textures_max_mb_ = REXCVAR_GET(nfsmw_nativo_texturas_mb_max);
    test_without_memory_every_ = REXCVAR_GET(nfsmw_nativo_prueba_sin_memoria_cada);
    if (test_without_memory_every_ > 0) {
      REXLOG_WARN("[nativo] C3: PRUEBA activa: se fingira falta de memoria cada {} reservas de textura",
                  test_without_memory_every_);
    }
    REXLOG_INFO("[nativo] C3: limite de la cache de texturas (nfsmw_nativo_texturas_mb_max) = {} MB{}", textures_max_mb_,
                textures_max_mb_ > 0 ? "" : " (sin limite)");
    diag_mips_ = REXCVAR_GET(nfsmw_nativo_diag_mips);
    if (diag_mips_) {
      REXLOG_INFO("[nativo] C3: diagnostico de mips (nfsmw_nativo_diag_mips) = SI");
    }
    ubo_alignment_ = std::max<VkDeviceSize>(16, vk_device_->properties().minUniformBufferOffsetAlignment);
    alternar_ubo_s_ = REXCVAR_GET(nfsmw_nativo_constantes_ubo_alternar_s);
    REXLOG_INFO("[nativo] C6: constantes por UBO dinamico (nfsmw_nativo_constantes_ubo) = {}; alternar cada {} s; "
                "alineacion {} bytes",
                use_ubo_ ? "SI" : "no", alternar_ubo_s_, ubo_alignment_);
    std::array<VkDescriptorSetLayoutBinding, 3> ubo_bindings{};
    for (uint32_t b = 0; b < 3; ++b) {
      ubo_bindings[b].binding = b;
      ubo_bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      ubo_bindings[b].descriptorCount = 1;
      ubo_bindings[b].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo info_ubo{};
    info_ubo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info_ubo.bindingCount = 3;
    info_ubo.pBindings = ubo_bindings.data();
    if (dfn_.vkCreateDescriptorSetLayout(device_, &info_ubo, nullptr, &layout_ubo_) != VK_SUCCESS) {
      return false;
    }
    const VkDescriptorPoolSize ubo_size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3 * uint32_t(sets_ubo_.size())};
    VkDescriptorPoolCreateInfo info_pool_ubo{};
    info_pool_ubo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info_pool_ubo.maxSets = uint32_t(sets_ubo_.size());
    info_pool_ubo.poolSizeCount = 1;
    info_pool_ubo.pPoolSizes = &ubo_size;
    if (dfn_.vkCreateDescriptorPool(device_, &info_pool_ubo, nullptr, &pool_ubo_) != VK_SUCCESS) {
      return false;
    }
    std::array<VkDescriptorSetLayout, kJobSlots> layouts_ubo;
    layouts_ubo.fill(layout_ubo_);
    VkDescriptorSetAllocateInfo ubo_reserve{};
    ubo_reserve.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ubo_reserve.descriptorPool = pool_ubo_;
    ubo_reserve.descriptorSetCount = uint32_t(sets_ubo_.size());
    ubo_reserve.pSetLayouts = layouts_ubo.data();
    if (dfn_.vkAllocateDescriptorSets(device_, &ubo_reserve, sets_ubo_.data()) != VK_SUCCESS) {
      return false;
    }
    for (size_t ranura = 0; ranura < sets_ubo_.size(); ++ranura) {
      const VkDescriptorBufferInfo blocks[3] = {{uploads_[ranura].buf, 0, kUboBytesVs},
                                                 {uploads_[ranura].buf, 0, kUboBytesPs},
                                                 {separately_shared_ ? bufs_shared_[ranura].buf
                                                                      : uploads_[ranura].buf,
                                                  0, kSharedUboBytes}};
      std::array<VkWriteDescriptorSet, 3> ubo_writes{};
      for (uint32_t b = 0; b < 3; ++b) {
        ubo_writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ubo_writes[b].dstSet = sets_ubo_[ranura];
        ubo_writes[b].dstBinding = b;
        ubo_writes[b].descriptorCount = 1;
        ubo_writes[b].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        ubo_writes[b].pBufferInfo = &blocks[b];
      }
      dfn_.vkUpdateDescriptorSets(device_, 3, ubo_writes.data(), 0, nullptr);
    }
    const VkPushConstantRange range_info{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                    24};
    VkPipelineLayoutCreateInfo info_layout{};
    info_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    const std::array<VkDescriptorSetLayout, 5> layouts_pipeline = {layouts_[0], layouts_[1], layouts_[2],
                                                                    layouts_[3], layout_ubo_};
    info_layout.setLayoutCount = 5;
    info_layout.pSetLayouts = layouts_pipeline.data();
    info_layout.pushConstantRangeCount = 1;
    info_layout.pPushConstantRanges = &range_info;
    // AirborneNX: the rectangle list geometry shader (shaders/rect_list_gs.hlsl). Without it (no
    // geometryShader feature) rectangle lists stay rejected as before.
    VkShaderModuleCreateInfo gs_info{};
    gs_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    gs_info.codeSize = sizeof(kRectListGs);
    gs_info.pCode = kRectListGs;
    if (dfn_.vkCreateShaderModule(device_, &gs_info, nullptr, &rect_list_gs_) != VK_SUCCESS) {
      rect_list_gs_ = VK_NULL_HANDLE;
      REXLOG_WARN("[nativo] C6: no geometry shader for rectangle lists; they are rejected");
    }
    return dfn_.vkCreatePipelineLayout(device_, &info_layout, nullptr, &layout_pipeline_) ==
           VK_SUCCESS;
  }

  // Slot 0 of each heap: a transparent black texture and a basic sampler, as the emulation does for an
  // invalid fetch constant.
  bool CreateEmpty() {
    const std::array<std::pair<VkImageViewType, uint32_t>, 3> types = {
        {{VK_IMAGE_VIEW_TYPE_2D, 1}, {VK_IMAGE_VIEW_TYPE_3D, 1}, {VK_IMAGE_VIEW_TYPE_CUBE, 6}}};
    for (uint32_t i = 0; i < 3; ++i) {
      VkImageCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      info.imageType = i == 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
      info.flags = i == 2 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
      info.format = VK_FORMAT_R8G8B8A8_UNORM;
      info.extent = {1, 1, 1};
      info.mipLevels = 1;
      info.arrayLayers = types[i].second;
      info.samples = VK_SAMPLE_COUNT_1_BIT;
      info.tiling = VK_IMAGE_TILING_OPTIMAL;
      info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      if (!rex::ui::vulkan::util::CreateDedicatedAllocationImage(
              vk_device_, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal,
              empty_[i].img, empty_[i].mem)) {
        return false;
      }
      VkImageViewCreateInfo view{};
      view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      view.image = empty_[i].img;
      view.viewType = types[i].first;
      view.format = VK_FORMAT_R8G8B8A8_UNORM;
      view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, types[i].second};
      if (dfn_.vkCreateImageView(device_, &view, nullptr, &empty_[i].view) != VK_SUCCESS) {
        return false;
      }
      empty_[i].width_px = empty_[i].height_px = 1;
      empty_[i].format_id = VK_FORMAT_R8G8B8A8_UNORM;
      WriteImage(i, 0, empty_[i].view);
    }
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = VK_LOD_CLAMP_NONE;
    if (dfn_.vkCreateSampler(device_, &sampler, nullptr, &empty_sampler_) != VK_SUCCESS) {
      return false;
    }
    WriteSampler(0, empty_sampler_);
    return true;
  }

  // The empty ones are initialized on the first submission: GENERAL and cleared to zero.
  bool PrepareEmpty() {
    if (prepared_empty_) {
      return true;
    }
    const VkCommandBuffer upload = contexto_->UploadCommands();
    if (!upload) {
      return false;
    }
    for (uint32_t i = 0; i < 3; ++i) {
      const uint32_t layer_count = i == 2 ? 6 : 1;
      Barrier(upload, empty_[i].img, layer_count);
      VkClearColorValue zero{};
      if (REXCVAR_GET(nfsmw_nativo_diag_vacias_grises)) {
        zero.float32[0] = zero.float32[1] = zero.float32[2] = 0.5f;
        zero.float32[3] = 1.0f;
      }
      const VkImageSubresourceRange range_info{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layer_count};
      dfn_.vkCmdClearColorImage(upload, empty_[i].img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1,
                                &range_info);
      empty_[i].prepared = true;
    }
    prepared_empty_ = true;
    return true;
  }

  void Barrier(VkCommandBuffer cmd, VkImage img, uint32_t layer_count) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = img;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, layer_count};  // and mips
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dfn_.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                              &barrier);
  }

  void WriteImage(uint32_t heap, uint32_t ranura, VkImageView view) {
    VkDescriptorImageInfo img{};
    img.imageView = view;
    img.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet write_op{};
    write_op.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write_op.dstSet = sets_[heap];
    write_op.dstBinding = 0;
    write_op.dstArrayElement = ranura;
    write_op.descriptorCount = 1;
    write_op.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write_op.pImageInfo = &img;
    dfn_.vkUpdateDescriptorSets(device_, 1, &write_op, 0, nullptr);
  }

  void WriteSampler(uint32_t ranura, VkSampler sampler) {
    VkDescriptorImageInfo img{};
    img.sampler = sampler;
    VkWriteDescriptorSet write_op{};
    write_op.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write_op.dstSet = sets_[3];
    write_op.dstBinding = 0;
    write_op.dstArrayElement = ranura;
    write_op.descriptorCount = 1;
    write_op.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write_op.pImageInfo = &img;
    dfn_.vkUpdateDescriptorSets(device_, 1, &write_op, 0, nullptr);
  }

  uint32_t ReserveSlot(uint32_t heap) {
    auto& m = heaps_[heap];
    if (!m.free_items.empty()) {
      const uint32_t ranura = m.free_items.back();
      m.free_items.pop_back();
      return ranura;
    }
    return m.next < m.cap_bytes ? m.next++ : 0;
  }

  // The draw's vertex input, recomputed only when the patched VS changes.
  const VertexInput* EntryFor(const DrawRequest& p) {
    if (generation_entry_ == p.generacion_vs && vs_entry_ == p.vs) {
      if (valid_entry_) {
        ++reused_entries_;
        return current_entry_;
      }
      // The same input that already failed: no recomputation and no warning.
      ++rejected_;
      ++causes_[cause_entry_];
      return nullptr;
    }
    generation_entry_ = p.generacion_vs;
    vs_entry_ = p.vs;
    valid_entry_ = false;
    // Cache: the same VS with the same patched fetches gives the same input.
    const uint64_t entry_key = EntryKey(p);
    if (entry_key) {
      if (const auto it = cache_entries_.find(entry_key); it != cache_entries_.end()) {
        current_entry_ = &it->second;
        valid_entry_ = true;
        cause_entry_ = 0;
        ++hits_cache_entries_;
        return current_entry_;
      }
    }
    entry_ = VertexInput{};
    const uint64_t rejected_before = rejected_;
    const auto before_entry = std::chrono::steady_clock::now();
    const VertexInput* entry = ComputeEntry(p);
    entries_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - before_entry)
                                 .count());
    ++computed_entries_;
    cause_entry_ = rejected_ != rejected_before ? last_cause_ : 0;
    current_entry_ = &entry_;
    if (entry && entry_key && cache_entries_.size() < 4096) {
      cache_entries_.emplace(entry_key, entry_);
    }
    return entry;
  }

  // Key of the input cache: everything CalcularEntrada reads from the elements and from both microcodes
  // (instruction, usage and index; original and patched words of each fetch), seeded with the VS. 0 = do
  // not cache.
  uint64_t EntryKey(const DrawRequest& p) const {
    const ShaderEntry& vs = *p.vs;
    const auto& patched = p.microcode_vs;
    if (patched.size() != vs.ucode.size() || vs.elements.size() > 32) {
      return 0;
    }
    std::array<uint32_t, 32 * 6> word_data;
    size_t n = 0;
    for (const VertexElement& element : vs.elements) {
      const size_t i = size_t(element.instruction) * 3;
      if (i + 2 >= patched.size()) {
        return 0;
      }
      word_data[n++] = (uint32_t(element.instruction) << 16) | (uint32_t(element.use) << 8) |
                      element.use_index;
      word_data[n++] = vs.ucode[i];
      word_data[n++] = vs.ucode[i + 1];
      word_data[n++] = patched[i];
      word_data[n++] = patched[i + 1];
      word_data[n++] = patched[i + 2];
    }
    const uint64_t key =
        XXH3_64bits_withSeed(word_data.data(), n * sizeof(uint32_t), uint64_t(uintptr_t(p.vs)));
    return key ? key : 1;
  }

  const VertexInput* ComputeEntry(const DrawRequest& p) {
    const ShaderEntry& vs = *p.vs;
    entry_.remaps.fill(kIdentityRemap);
    const auto& patched = p.microcode_vs;
    if (patched.size() != vs.ucode.size()) {
      Reject(20, "el VS del anillo no tiene la longitud del contenedor");
      return nullptr;
    }
    uint32_t used_locations = 0;
    for (const VertexElement& element : vs.elements) {
      const uint32_t reg_index = element.reg_index;  // AirborneNX: masked in the microcode copy
      const uint32_t original = element.swizzle;  // AirborneNX: the microcode copy has it masked
      size_t q = SIZE_MAX;
      for (const VertexElement& other_one : vs.elements) {
        const size_t i = size_t(other_one.instruction) * 3;
        if (((patched[i] >> 12) & 0x3F) != reg_index || (patched[i] & 0x1F) != 0) {
          continue;
        }
        if (q == SIZE_MAX) {
          q = i;
        }
        if (WrittenMask(patched[i + 1] & 0xFFF) == WrittenMask(original)) {
          q = i;  // the one that writes the same components
          break;
        }
      }
      if (q == SIZE_MAX) {
        if (vs_warned_.size() < 16 && vs_warned_.insert(vs.number).second) {
          std::string detail;
          for (const VertexElement& other_one : vs.elements) {
            const size_t i = size_t(other_one.instruction) * 3;
            detail += fmt::format(" {}{}@{}: original r{} op{}, parcheado r{} op{} {:08X} {:08X} {:08X};",
                                   UseName(other_one.use), other_one.use_index, other_one.instruction,
                                   uint32_t(other_one.reg_index), vs.ucode[i] & 0x1F,
                                   (patched[i] >> 12) & 0x3F, patched[i] & 0x1F, patched[i],
                                   patched[i + 1], patched[i + 2]);
          }
          REXLOG_WARN("[nativo] C6 diag: VS n{} sin fetch para {}{} (registro r{}):{}", vs.number,
                      UseName(element.use), element.use_index, reg_index, detail);
        }
        Reject(21, "fetch de vertices no encontrado en el VS parcheado");
        return nullptr;
      }
      const uint32_t d0 = patched[q], d1 = patched[q + 1], d2 = patched[q + 2];
      const uint32_t ranura = ((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 0x3);
      const uint32_t format_id = (d1 >> 16) & 0x3F;
      const uint32_t stride_bytes = (d2 & 0xFF) * 4;
      const int32_t offset = (int32_t(d2 << 1) >> 9) * 4;
      const int32_t loc_index = UseLocation(element.use, element.use_index);
      if (loc_index < 0 || ((used_locations >> loc_index) & 0x1)) {
        Warn(22, "elemento de vertices sin ubicacion libre en el shader: se omite");
        continue;
      }
      const uint32_t code = RemapCode(original, d1 & 0xFFF);
      if (code != kIdentityRemap && swizzle_warnings_ < 24) {
        ++swizzle_warnings_;
        REXLOG_INFO("[nativo] C6: VS n{} {}{} (formato {}): swizzle original {:03X}, parcheado {:03X}, "
                    "remapeo {:03X}",
                    vs.number, UseName(element.use), element.use_index, format_id, original,
                    d1 & 0xFFF, code);
      }
      bool r11g11b10 = false;
      const VkFormat vk = AttributeFormat(format_id, IntEntry(element.use), (d1 >> 12) & 0x1,
                                          (d1 >> 13) & 0x1, false, r11g11b10);
      if (vk == VK_FORMAT_UNDEFINED) {
        Reject(300 + format_id, "formato de vertice todavia no soportado");
        return nullptr;
      }
      if (r11g11b10) {
        entry_.specialization |= 0x1;
      }
      uint32_t bind_index = 0;
      while (bind_index < entry_.bindings.size() && entry_.bindings[bind_index].ranura != ranura) {
        ++bind_index;
      }
      if (bind_index == entry_.bindings.size()) {
        entry_.bindings.push_back({ranura, stride_bytes});
      } else if (!entry_.bindings[bind_index].stride_bytes) {
        entry_.bindings[bind_index].stride_bytes = stride_bytes;
      }
      if (offset < 0) {
        Reject(24, "offset de vertice negativo");
        return nullptr;
      }
      entry_.attributes.push_back({uint32_t(loc_index), bind_index, vk, uint32_t(offset)});
      entry_.remaps[loc_index] = code;
      used_locations |= uint32_t(1) << loc_index;
    }
    for (const VertexBinding& bind_index : entry_.bindings) {
      if (!bind_index.stride_bytes) {
        Reject(25, "stream de vertices sin zancada");
        return nullptr;
      }
    }
    uint64_t fingerprint = XXH3_64bits(entry_.attributes.data(),
                                  entry_.attributes.size() * sizeof(VertexAttribute));
    fingerprint = XXH3_64bits_withSeed(entry_.bindings.data(),
                                  entry_.bindings.size() * sizeof(VertexBinding), fingerprint);
    entry_.fingerprint = fingerprint ^ entry_.specialization;
    valid_entry_ = true;
    return &entry_;
  }

  // Texture of a fetch constant: heap slot and, if it has to be uploaded, it is left in texturas_a_subir_
  // with its data already prepared. Base level of 2D textures and cubemaps.
  void PrepareTexture(const uint32_t* f, uint32_t& ranura, uint32_t& heap,
                       VkDeviceSize& upload_bytes, bool& point_sampling, uint64_t& valid_until,
                       uint32_t& host_width_out, uint32_t& host_height_out, float& scale_out) {
    ranura = 0;
    heap = 0;
    // AirborneNX: exp_adjust of the fetch constant (dword 3, bits 13-18, signed): samples are multiplied by
    // 2^exp_adjust. Resolved textures add their own resolve bias below.
    scale_out = std::ldexp(1.0f, int32_t(f[3] << 13) >> 26);
    host_width_out = 0;  // 0 = unknown (1/size is not written)
    host_height_out = 0;
    point_sampling = false;
    valid_until = frame_;  // by default, this frame only
    if ((f[0] & 0x3) != uint32_t(xenos::FetchConstantType::kTexture)) {
      return;
    }
    const uint32_t dimension = (f[5] >> 9) & 0x3;
    const bool cube = dimension == uint32_t(xenos::DataDimension::kCube);
    const bool volume = dimension == uint32_t(xenos::DataDimension::k3D);
    if (dimension != uint32_t(xenos::DataDimension::k2DOrStacked) && !cube && !volume) {
      Warn(30, "texturas 1D todavia no soportadas: se usa una vacia");
      return;
    }
    heap = cube ? 2 : volume ? 1 : 0;
    const uint32_t layer_count = cube ? 6 : 1;
    // AirborneNX: gamma (3 in every RGB sign) is handled below with sRGB formats; signed and biased are not.
    if (((f[0] >> 2) & 0xFF) && ((f[0] >> 2) & 0x3F) != 0x3F) {
      Warn(31, "texturas con signo o gamma: se leen sin signo");
    }
    const uint32_t swizzle = (f[3] >> 1) & 0xFFF;
    const uint32_t base = (f[1] >> 12) << 12;
    if (!cube && !volume) {
      if (const NativeImage* resolved = contexto_->ResolvedTexture(base & 0x1FFFFFFF)) {
        // AirborneNX: the guest texture's own size (size_2d: 13 + 13 bits).
        const uint32_t guest_width = (f[2] & 0x1FFF) + 1, guest_height = ((f[2] >> 13) & 0x1FFF) + 1;
        if (!IsDepth(resolved->format_id) && !resolved->guest_width &&
            (guest_width != resolved->width_px || guest_height != resolved->height_px)) {
          contexto_->TrackTextureSize(base & 0x1FFFFFFF, guest_width, guest_height);
        }
        // Without this the shadows flicker.
        // With nfsmw_nativo_inv_tamano_tex the shader takes 1/size from the shared constants, and this path
        // used to return without reporting the size: 0 was written and the tfetch offsets (the taps of the
        // shadow map's PCF filter) all landed on the same texel. The filtering disappeared and the edge
        // shimmered as the camera moved. Resolved textures report their size like any other.
        host_width_out = resolved->width_px;
        host_height_out = resolved->height_px;
        scale_out *= resolved->scale;
        // One trace per size, to check in the log that the 1/size constant of resolved textures is no longer
        // 0 (that was the cause of the shadow flicker).
        if (resolved_invsize_warnings_.insert(uint64_t(resolved->width_px) << 32 | resolved->height_px).second) {
          REXLOG_INFO("[nativo] C3: textura resuelta {}x{}: 1/tamano = {:.6f}, {:.6f}",
                      resolved->width_px, resolved->height_px, 1.0f / float(resolved->width_px),
                      1.0f / float(resolved->height_px));
        }
        if (IsDepth(resolved->format_id)) {
          // Depth copy (k_24_8): depth comes out in R and the fetch constant's swizzle distributes it. No
          // filtering: it is read as is.
          ranura = ViewSlot(resolved->img, resolved->format_id, swizzle, kSwizzleRRRR);
          point_sampling = true;
          // This used to be UINT64_MAX, and that is what forced the cross-frame cache off.
          // A resolved texture is a render target the game rewrites every frame. Saying its descriptor slot is
          // valid "until the generation changes" meant that, with the CPU two frames ahead (3 work slots), an
          // old view was reused: that was the smeared race leaderboard that kept
          // nfsmw_nativo_cache_texturas_entre_fotogramas at false for a while. The other paths are already tied
          // to textura.siguiente and relax nothing; these two were the only ones without a horizon.
          valid_until = frame_;
          return;
        }
        // The emulation writes the copy to memory with copy_dest_swap and loads it as a texture: the fetch
        // constant's swizzle (ZYXW for 8888) undoes that swap. Here the copy is image to image and does not
        // swap channels, so the swap goes into the host channels (without this, Mia came out blue).
        ranura = ViewSlot(resolved->img, resolved->format_id, swizzle,
                             resolved->rb_swap ? kSwizzleBGRA : kSwizzleRGBA);
        valid_until = frame_;  // see the long comment on the depth copy
        return;
      }
    }
    const uint32_t format_id = f[1] & 0x3F;
    TextureFormat tf;
    if (!TextureFormatOf(format_id, tf)) {
      Warn(400 + format_id, "formato de textura todavia no soportado: se usa una vacia");
      return;
    }
    // AirborneNX: gamma textures (sign_x/y/z = 3, TextureSign::kGamma) are linearized by the Xenos when
    // sampled (piecewise-linear gamma). The host does it with the sRGB variant of the format, the same
    // approximation the emulation uses by default. They are already separate cache entries: the sign bits
    // are part of the key.
    if (((f[0] >> 2) & 0x3F) == 0x3F) {
      const VkFormat srgb = SrgbFormat(tf.format_id);
      if (srgb != VK_FORMAT_UNDEFINED) {
        tf.format_id = srgb;
      } else {
        Warn(39, "textura gamma en un formato sin variante sRGB: se lee sin linealizar");
      }
    }
    // size_2d: 13 + 13 bits; size_3d: 11 + 11 + 10 bits (xenos.h:1222-1233).
    const uint32_t width_px = volume ? (f[2] & 0x7FF) + 1 : (f[2] & 0x1FFF) + 1;
    const uint32_t height_px = volume ? ((f[2] >> 11) & 0x7FF) + 1 : ((f[2] >> 13) & 0x1FFF) + 1;
    const uint32_t background = volume ? ((f[2] >> 22) & 0x3FF) + 1 : 0;
    if (volume && tf.block > 1) {
      Warn(38, "textura 3D comprimida: se usa una vacia");
      return;
    }
    const uint32_t blocks_x = (width_px + tf.block - 1) / tf.block;
    const uint32_t blocks_y = (height_px + tf.block - 1) / tf.block;
    const uint32_t blocks_pitch = std::max<uint32_t>((((f[0] >> 22) & 0x1FF) << 5) / tf.block, 1);
    // The faces of a cubemap are consecutive: each takes its base level with rows and columns aligned to
    // 32 blocks and the total to 4 KB (GetGuestTextureLayout, pipeline/texture/util.cpp:311-337).
    const uint64_t face_stride =
        (uint64_t((blocks_pitch + 31) & ~uint32_t(31)) * tf.bytes *
             ((blocks_y + 31) & ~uint32_t(31)) +
         4095) &
        ~uint64_t(4095);
    const uint32_t keys[5] = {f[0] & 0xFFC003FC, f[1], f[2], (f[4] >> 2) & 0xFF, f[5] >> 9};
    const uint64_t key = XXH3_64bits(keys, sizeof(keys));

    // Mip levels with the rules of GetSubresourcesFromFetchConstant (pipeline/texture/util.cpp:67-97): no
    // mip address means no mips; the maximum is clamped to the size; if the base is missing or the minimum
    // is above 0, the base is not read. 3D textures still use only the base.
    const bool texture_tile = (f[0] >> 31) & 0x1;
    const uint64_t dir_base = uint64_t(base) & 0x1FFFFFFF;
    const uint64_t dir_mips = uint64_t((f[5] >> 12) & 0x1FFFF) << 12;
    const uint32_t packed_level =
        mipmaps_ && !volume && ((f[5] >> 11) & 0x1) ? PackedLevel(width_px, height_px) : UINT32_MAX;
    uint32_t level_max = 0;
    bool read_base = true;
    if (mipmaps_ && !volume && dir_mips != 0) {
      const uint32_t size_max = Log2Floor(std::max(width_px, height_px));
      uint32_t level_min = std::min((f[4] >> 2) & 0xFu, size_max);
      level_max = std::max(std::min((f[4] >> 6) & 0xFu, size_max), level_min);
      if (level_max != 0) {
        if (dir_base == 0) {
          level_min = std::max(level_min, 1u);
        }
        read_base = level_min == 0;
      }
    }
    // Mip regions from dir_mips (GetGuestTextureLayout): each level stored with rows of 32 blocks computed
    // from the size rounded up to a power of 2 (and aligned to 256 bytes if the texture is linear) and
    // layers aligned to 4 KB; the levels of the packed tail share the region of the first of them.
    struct RegionMip {
      uint64_t byte_offset = 0;
      uint64_t row_bytes = 0;
      uint64_t stride_bytes = 0;
      uint32_t blocks_pitch = 0;
    };
    std::array<RegionMip, 16> regions{};
    uint64_t extension_mips = 0;
    if (level_max != 0) {
      const uint32_t last_level = packed_level == 0 ? 0 : std::min(level_max, packed_level);
      for (uint32_t s = packed_level == 0 ? 0 : 1; s <= last_level; ++s) {
        RegionMip& region = regions[s];
        const uint32_t texels_row = std::max(std::bit_ceil(width_px) >> s, 1u);
        const uint32_t texels_rows = std::max(std::bit_ceil(height_px) >> s, 1u);
        region.blocks_pitch = ((texels_row + tf.block - 1) / tf.block + 31) & ~31u;
        region.row_bytes = uint64_t(region.blocks_pitch) * tf.bytes;
        if (!texture_tile) {
          region.row_bytes = (region.row_bytes + 255) & ~uint64_t(255);
        }
        const uint64_t blocks_rows = uint64_t(((texels_rows + tf.block - 1) / tf.block + 31) & ~31u);
        region.stride_bytes = (region.row_bytes * blocks_rows + 4095) & ~uint64_t(4095);
        region.byte_offset = extension_mips;
        extension_mips += region.stride_bytes * layer_count;
      }
      if (dir_mips + extension_mips > kPhysicalMemory) {
        level_max = 0;  // the mips would go past memory: base only
        extension_mips = 0;
        read_base = true;
      }
    }
    // With the packed tail starting at level 0 (short side of 16 texels or less), the base does not start
    // at its address either (VulkanTextureCache::LoadTextureDataFromResidentMemoryImpl applies the same
    // offset to level 0).
    uint32_t base_ox = 0;
    uint32_t base_oy = 0;
    if (packed_level == 0) {
      PackedOffset(width_px, height_px, tf.block, 0, base_ox, base_oy);
    }

    if (cube && copy_image_) {
      // The game's dynamic cubemap: C2 resolves its faces one by one to those same addresses. They are
      // copied to the cubemap's layers in the upload command buffer, which runs before the work one, so they
      // lag one frame behind.
      std::array<const NativeImage*, 6> faces{};
      bool resolved_list = true;
      for (uint32_t c = 0; c < 6 && resolved_list; ++c) {
        faces[c] = contexto_->ResolvedTexture(
            uint32_t((uint64_t(base) + c * face_stride) & 0x1FFFFFFF));
        resolved_list = faces[c] && faces[c]->format_id == VK_FORMAT_R8G8B8A8_UNORM &&
                    faces[c]->width_px >= width_px && faces[c]->height_px >= height_px;
      }
      if (resolved_list) {
        const uint64_t resolved_key = key ^ 0x9E3779B97F4A7C15ull;
        Texture& texture = textures_[resolved_key];
        if (texture.img.img == VK_NULL_HANDLE) {
          if (!CreateTexture(texture.img, VK_FORMAT_R8G8B8A8_UNORM, width_px, height_px, layer_count)) {
            textures_.erase(resolved_key);
            Warn(32, "no se pudo crear una textura");
            return;
          }
          texture.layer_count = layer_count;
          texture.bytes = uint64_t(width_px) * height_px * 4 * layer_count;
          textures_bytes_ += texture.bytes;
          REXLOG_INFO("[nativo] C3: cubo {:08X} {}x{} con sus caras resueltas por C2 (fetch {:08X} "
                      "{:08X} {:08X} {:08X} {:08X} {:08X})",
                      base, width_px, height_px, f[0], f[1], f[2], f[3], f[4], f[5]);
        }
        if (texture.frame != frame_) {
          const VkCommandBuffer upload = contexto_->UploadCommands();
          if (!upload) {
            return;
          }
          texture.frame = frame_;
          if (!texture.img.prepared) {
            Barrier(upload, texture.img.img, layer_count);
            texture.img.prepared = true;
          }
          for (uint32_t c = 0; c < 6; ++c) {
            VkImageCopy copy_info{};
            copy_info.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy_info.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, c, 1};
            copy_info.extent = {width_px, height_px, 1};
            copy_image_(upload, faces[c]->img, VK_IMAGE_LAYOUT_GENERAL, texture.img.img,
                           VK_IMAGE_LAYOUT_GENERAL, 1, &copy_info);
          }
          // Rear-view mirror diagnostic: how many times the faces are copied to the cubemap per submission.
          ++refreshed_cubes_;
          const auto now_time = std::chrono::steady_clock::now();
          if (now_time - cubes_report_ >= std::chrono::seconds(10)) {
            // It used to be the last periodic line the ring wrote itself; it now goes to the report thread.
            NFSMW_RING_REPORT("[nativo] C3 cubos con caras de C2: {} refrescos en {} trabajos",
                                 refreshed_cubes_ - refreshed_cubes_previous_,
                                 frame_ - cubes_report_frame_);
            cubes_report_ = now_time;
            refreshed_cubes_previous_ = refreshed_cubes_;
            cubes_report_frame_ = frame_;
          }
        }
        ranura = ViewSlot(texture.img.img, VK_FORMAT_R8G8B8A8_UNORM, swizzle,
                             faces[0]->rb_swap ? kSwizzleBGRA : kSwizzleRGBA, heap);
        // The cubemap with the resolved faces must also report its size, or its 1/size stays at 0 and the
        // tfetch offsets are lost (see the branch above).
        host_width_out = texture.img.width_px;
        host_height_out = texture.img.height_px;
        return;
      }
    }

    Texture& texture = textures_[key];
    if (texture.img.img == VK_NULL_HANDLE) {
      // BC in Vulkan: the size is in whole blocks.
      const uint32_t host_width = tf.block > 1 ? (width_px + 3) & ~uint32_t(3) : width_px;
      const uint32_t host_height = tf.block > 1 ? (height_px + 3) & ~uint32_t(3) : height_px;
      // With its mip levels (those that fit in the host size).
      const uint32_t levels = std::min(level_max + 1, Log2Floor(std::max(host_width, host_height)) + 1);
      /*
       * The vkBindImageMemory goes to the bind thread if possible (nfsmw_nativo_texturas_enlace_hilo) and,
       * otherwise, CrearTextura as usual. What the ring spends creating is timed, without its wait for the
       * thread (that is counted separately), for the [tiron] anillo line and the 10 s report.
       */
      const auto before_create = std::chrono::steady_clock::now();
      const uint64_t wait_before_create = bindings_wait_total_ns_;
      creation_measuring_ = true;
      const bool created_one = CreateTextureInThread(texture, tf.format_id, host_width, host_height, layer_count, background, levels) ||
                          CreateTexture(texture.img, tf.format_id, host_width, host_height, layer_count, background, levels);
      creation_measuring_ = false;
      {
        const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - before_create)
                                         .count());
        const uint64_t wait_stats = bindings_wait_total_ns_ - wait_before_create;
        const uint64_t own = ns > wait_stats ? ns - wait_stats : 0;
        nfsmw::waits::g_textures_create_ns.fetch_add(own, std::memory_order_relaxed);
        create_report_ns_ += own;
      }
      if (!created_one) {
        textures_.erase(key);
        Warn(32, "no se pudo crear una textura");
        return;
      }
      texture.layer_count = layer_count;
      texture.background = background;
      texture.levels = levels;
      uploaded_mip_levels_ += levels - 1;
      // Measurement only: the shape without the address, to count, once its first hash is known, whether it
      // repeats the content of a live one (AnotarContenidoTextura). If this image is being recreated, the
      // previous one stops counting. The words are the key's without the base and mip addresses and without
      // bit 11 of f[1], which the sampler sets: format, byte order, tiling, pitch, size, mip limits, dimension
      // and packed tail.
      if (DiagReuse()) {
        RemoveTextureContent(texture, key);
        const uint32_t shape[5] = {keys[0], f[1] & 0x7FF, keys[2], keys[3], (f[5] >> 9) & 0x7};
        texture.content_shape = XXH3_64bits(shape, sizeof(shape));
        texture.address = base;
        texture.content_per_measure = true;
      }
      for (uint32_t n = 0; n < levels; ++n) {
        texture.bytes += uint64_t((std::max(host_width >> n, 1u) + tf.block - 1) / tf.block) *
                         ((std::max(host_height >> n, 1u) + tf.block - 1) / tf.block) * tf.bytes * layer_count *
                         (background ? background : 1);
      }
      textures_bytes_ += texture.bytes;
      // Diagnostic: why the cache grows on the console. A new texture at an address that already had another
      // points to world zones reloaded at the same place; with the same address, format and size but a
      // different key, to key fields that change (mip limits, byte order...).
      {
        ++created_textures_;
        nfsmw::waits::g_created_textures.fetch_add(1, std::memory_order_relaxed);
        const uint64_t shape = XXH3_64bits_withSeed(&base, sizeof(base), (uint64_t(format_id) << 40) ^
                                                                              (uint64_t(width_px) << 20) ^ height_px);
        if (created_per_address_[base]++ > 0) {
          ++created_in_view_address_;
        }
        const auto [it_shape, new_shape] = key_per_shape_.try_emplace(shape, key);
        if (!new_shape && it_shape->second != key) {
          ++key_other_shape_same_created_;
          it_shape->second = key;
          /*
           * In the main menu, ~100 textures per frame once came back with the same address, format and size and
           * a different key, and were created again every frame. This shows which key words change (the first
           * 40 times).
           */
          const auto [it_words, added_items] = words_per_shape_.try_emplace(shape);
          /*
           * Key guard. The same five words cannot produce a different key: if one does, the key was not computed
           * from what its words say (as when XXH3 read claves[4] before it was written; see docs/toolchain.md).
           */
          if (!added_items && std::equal(std::begin(keys), std::end(keys), it_words->second.begin())) {
            const uint64_t times = ++inconsistent_keys_;
            if (times == 1 || times == 10 || times == 100 || times == 1000 || times % 10000 == 0) {
              REXLOG_ERROR("[nativo] C3 clave de textura INCOHERENTE ({} veces): {:08X} {}x{} formato {} con las "
                           "mismas cinco palabras que la vez anterior y otra clave; la clave no sale de sus palabras",
                           times, base, width_px, height_px, format_id);
            }
          }
          if (!added_items && key_other_warnings_ < 40) {
            ++key_other_warnings_;
            const auto& before = it_words->second;
            REXLOG_INFO("[nativo] C3 misma textura con otra clave: {:08X} {}x{} formato {} | antes {:08X} {:08X} "
                        "{:08X} {:02X} {:06X} | ahora {:08X} {:08X} {:08X} {:02X} {:06X} | cambian {:08X} {:08X} "
                        "{:08X} {:02X} {:06X}",
                        base, width_px, height_px, format_id, before[0], before[1], before[2], before[3], before[4], keys[0],
                        keys[1], keys[2], keys[3], keys[4], before[0] ^ keys[0], before[1] ^ keys[1],
                        before[2] ^ keys[2], before[3] ^ keys[3], before[4] ^ keys[4]);
          }
          std::copy(std::begin(keys), std::end(keys), it_words->second.begin());
        } else if (new_shape) {
          auto& word_data = words_per_shape_[shape];
          std::copy(std::begin(keys), std::end(keys), word_data.begin());
        }
        // From 256 to 1024. This line and the GPU memory line after it were the two longest stutters of the
        // "volcado de diagnostico" group: 147.6 ms and 107.5 ms of frame time on their own. With 1024 they
        // still appear once or twice per session, which is enough to see whether the duplicate key counter
        // spikes.
        if (created_textures_ % 1024 == 0) {
          REXLOG_INFO("[nativo] C3 diag cache: {} texturas creadas ({} en la cache, {} MB): {} en una direccion que ya "
                      "tuvo otra textura; {} con la misma direccion, formato y tamano que otra pero distinta clave",
                      created_textures_, textures_.size(), textures_bytes_ >> 20, created_in_view_address_,
                      key_other_shape_same_created_);
          TrackGpuMemory();
        }
      }
      if (textures_.size() <= 48) {
        REXLOG_INFO("[nativo] C3: textura {:08X} {}x{} formato {} {} orden {} pitch {} "
                    "swizzle {:03X} signos {:02X}{}; niveles {} (mips en {:08X}, empaquetados desde {})",
                    base, width_px, height_px, format_id, ((f[0] >> 31) & 0x1) ? "en mosaico" : "lineal",
                    (f[1] >> 6) & 0x3, ((f[0] >> 22) & 0x1FF) << 5, swizzle, (f[0] >> 2) & 0xFF,
                    cube ? " (cubo)" : volume ? " (3D)" : "", levels, dir_mips,
                    packed_level == UINT32_MAX ? std::string("ninguno") : std::to_string(packed_level));
      }
    }
    ranura = ViewSlot(texture.img.img, tf.format_id, swizzle, tf.swizzle_host, heap);
    host_width_out = texture.img.width_px;  // the host's, which is what the shader sees
    host_height_out = texture.img.height_px;
    if (texture.frame == frame_) {
      // Valid until the frame before the next check (if it changed now, only this frame)
      valid_until = std::max<uint64_t>(frame_, texture.next ? texture.next - 1 : 0);
      return;  // already checked this frame
    }
    // Textures that do not change are checked less and less often (down to every 32 frames); those that
    // change (videos) go back to being checked every frame.
    if (texture.img.prepared && frame_ < texture.next) {
      valid_until = texture.next - 1;
      return;
    }
    texture.frame = frame_;
    // If its hash thread job is still uncollected (a buffer-full submission happened in the middle of its
    // draw), its huella_cruda is not valid yet: it is collected before checking, as if the ring had done
    // it.
    if (texture.job_fingerprint == kPublishedFingerprintJob) {
      CollectFingerprints();
    }
    // What it costs from here to the end (check, untile, prepare the upload).
    struct TextureStopwatch {
      std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
      ~TextureStopwatch() {
        nfsmw::waits::g_textures_ns.fetch_add(
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                         .count()),
            std::memory_order_relaxed);
      }
    } texture_stopwatch;
    // 2D and cubemaps: before untiling, XXH3 of the guest bytes. If they have not changed there is nothing
    // to do (untiling and comparing every texture every frame took 62 of the 66 us of each menu draw).
    if (!volume) {
      const uint32_t block_log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2
                                                                : tf.bytes >= 2 ? 1 : 0;
      // Extent of a layer: tiled, GetTiledAddressUpperBound2D (pipeline/texture/util.cpp:461-486); linear,
      // up to the last block. Includes the packed base offset (base_ox, base_oy; 0 for the rest).
      const uint32_t read_blocks_x = blocks_x + base_ox;
      const uint32_t read_blocks_y = blocks_y + base_oy;
      const uint64_t extension_capa =
          ((f[0] >> 31) & 0x1)
              ? uint64_t(std::max<int64_t>(
                    Tile2dOffset(int32_t((read_blocks_x - 1) & ~31u),
                                            int32_t((read_blocks_y - 1) & ~31u), blocks_pitch,
                                            block_log2),
                    0)) +
                    (block_log2 == 0   ? 0xA00u
                     : block_log2 == 1 ? 0xC00u
                                        : (0x400u << block_log2))
              : uint64_t(std::max(blocks_pitch, read_blocks_x)) * tf.bytes * (read_blocks_y - 1) +
                    uint64_t(read_blocks_x) * tf.bytes;
      const uint64_t raw_start = uint64_t(base) & 0x1FFFFFFF;
      const uint64_t extension = (layer_count - 1) * face_stride + extension_capa;
      if (raw_start + extension <= kPhysicalMemory) {
        /*
         * Per-frame check budget.
         * During race stutters both game threads wait for room in the ring (20-50 ms) while the ring neither
         * stops nor waits for the GPU: the ring thread itself is stuck. During the stutters, that thread was in
         * PrepararTextura and in XXH3. Textures that arrive together (one zone) double their interval at the
         * same time (1, 2, 4... 32), so all of them are rechecked in the same frame. Here a stable texture
         * (interval 8 or more) whose check does not fit in the frame's budget is deferred a few frames, at
         * most kAplazamientoMax in a row. New textures, changing ones (videos) and just-uploaded ones are
         * never deferred.
         */
        /*
         * Sampled recheck of stable textures (nfsmw_nativo_huellas_muestreo).
         * A stable texture used to go through XXH3 in full every 32-39 frames only to conclude, almost always,
         * that it had not changed: in the alley that was 0.2-5.3 ms per frame of the ring thread. Now every
         * full check from interval 4 on also computes the hash of a sample of the same memory (HuellaMuestra:
         * first and last 4 KB block and 1 in 8, of the base and the mips) and, if the full hash matches, stores
         * it. Rechecks of stable textures compute only the sample: if it matches, the texture is taken as
         * unchanged; otherwise the usual full path follows. Textures where the sample would read more than half
         * the bytes (those with few blocks) work as before.
         * Self-checking guard: the first kMuestrasAComprobar rechecks of stable textures compute both hashes
         * and count disagreements (same sample, different full hash). After that, 1 in N rechecks of each
         * texture is still full (N = the cvar), so a change the sample misses is caught at most N rechecks
         * later. A single disagreement turns sampling off for the rest of the session, with a warning.
         */
        if (fingerprints_sampling_ < 0) {
          const int32_t cada = REXCVAR_GET(nfsmw_nativo_huellas_muestreo);
          fingerprints_sampling_ = cada >= 2 ? std::min<int32_t>(cada, 64) : 0;
          REXLOG_INFO("[nativo] C3: recomprobacion de texturas estables por muestreo (nfsmw_nativo_huellas_muestreo) "
                      "= {}",
                      fingerprints_sampling_ ? fmt::format("SI, 1 de cada {} completa; las {} primeras con las dos huellas",
                                                      fingerprints_sampling_, kSamplesToCheck)
                                        : std::string("no, siempre la huella completa"));
        }
        const uint64_t complete_bytes = extension + extension_mips;
        const uint64_t bytes_of_sample = SampleBytes(extension) + SampleBytes(extension_mips);
        const bool with_sample = fingerprints_sampling_ > 0 && texture.img.prepared && texture.interval >= 4 &&
                                 bytes_of_sample * 2 <= complete_bytes;
        const bool usable_sample = with_sample && texture.interval >= 8 && texture.valid_sample;
        const bool only_sample = usable_sample && checked_samples_ >= kSamplesToCheck &&
                                  texture.consecutive_samples + 1u < uint32_t(fingerprints_sampling_);
        const uint64_t sample_bytes = with_sample ? bytes_of_sample : 0;
        // What will actually be read, which is what counts against the budget.
        const uint64_t fingerprint_bytes = only_sample ? sample_bytes : complete_bytes + sample_bytes;
        if (fingerprints_frame_ != frame_) {
          fingerprints_frame_ = frame_;
          frame_fingerprint_bytes_ = 0;
        }
        if (fingerprints_budget_ < 0) {
          fingerprints_budget_ = std::max(REXCVAR_GET(nfsmw_nativo_huellas_kb_fotograma), 0);
          REXLOG_INFO("[nativo] C3: presupuesto de comprobaciones de texturas = {} KB por fotograma{}",
                      fingerprints_budget_, fingerprints_budget_ ? "" : " (sin limite)");
        }
        if (fingerprints_budget_ && texture.img.prepared && texture.interval >= 8 &&
            texture.deferrals < kDeferralMax && frame_fingerprint_bytes_ > 0 &&
            frame_fingerprint_bytes_ + fingerprint_bytes > uint64_t(fingerprints_budget_) * 1024) {
          ++texture.deferrals;
          texture.next = frame_ + 1 + (key & 1);
          valid_until = texture.next - 1;
          nfsmw::waits::g_deferred_fingerprints.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        texture.deferrals = 0;
        frame_fingerprint_bytes_ += fingerprint_bytes;
        nfsmw::waits::g_fingerprint_bytes.fetch_add(fingerprint_bytes, std::memory_order_relaxed);
        const auto raw_fingerprint_start = std::chrono::steady_clock::now();
        const uint8_t* const base_raw = mem_->TranslatePhysical(uint32_t(raw_start));
        const uint8_t* const mips_raw = extension_mips ? mem_->TranslatePhysical(uint32_t(dir_mips)) : nullptr;
        // The sample goes before the full hash. If the full one matches, the sample is of that same content,
        // and it is the one stored.
        uint64_t sample = 0;
        if (with_sample) {
          sample = SampleFingerprint(base_raw, extension, 0);
          if (extension_mips) {
            sample = SampleFingerprint(mips_raw, extension_mips, sample);
          }
          sample_bytes_ += sample_bytes;
          SamplingReport(raw_fingerprint_start);
        }
        const bool equal_sample = usable_sample && sample == texture.sample_fingerprint;
        if (only_sample && equal_sample) {
          nfsmw::waits::g_raw_fingerprint_ns.fetch_add(
              uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                            raw_fingerprint_start)
                           .count()),
              std::memory_order_relaxed);
          ++texture.consecutive_samples;
          ++hits_samples_;
          sample_saved_bytes_ += complete_bytes - sample_bytes;
          texture.interval = std::min<uint32_t>(texture.interval * 2, 32);
          texture.next = frame_ + texture.interval + (texture.interval == 32 ? (key >> 7) & 7 : 0);
          valid_until = texture.next - 1;
          return;
        }
        if (only_sample) {
          // The sample changed: now the full hash, which was not in this frame's budget.
          ++distinct_samples_;
          frame_fingerprint_bytes_ += complete_bytes;
          nfsmw::waits::g_fingerprint_bytes.fetch_add(complete_bytes, std::memory_order_relaxed);
        }
        /*
         * New texture -> hash thread (nfsmw_nativo_texturas_huella_hilo; see PlanearHuella).
         * The plan is the same LeerNivel calls the usual path makes further down, with the same arguments, but
         * reading from a copy of the bytes this hash covers. If any read falls outside them, or LeerNivel
         * would go past memory, the texture continues on the usual path.
         */
        if (!texture.img.prepared && phase_fingerprints_ != kFingerprintsDisabled && !diag_mips_ && fast_tile_ >= 0) {
          std::array<size_t, 16> offsets{};
          std::array<size_t, 16> level_layer{};
          size_t bytes_plan = 0;
          for (uint32_t n = 0; n < texture.levels; ++n) {
            const uint32_t bx_n = (std::max(texture.img.width_px >> n, 1u) + tf.block - 1) / tf.block;
            const uint32_t by_n = (std::max(texture.img.height_px >> n, 1u) + tf.block - 1) / tf.block;
            offsets[n] = bytes_plan;
            level_layer[n] = size_t(bx_n) * by_n * tf.bytes;
            bytes_plan += level_layer[n] * layer_count;
          }
          const uint32_t bx_base = (texture.img.width_px + tf.block - 1) / tf.block;
          const uint64_t mips_in_copy = (extension + 63) & ~uint64_t(63);  // where the copy of the mips starts
          uint32_t n_reads = 0;
          bool plan_ok = true;
          for (uint32_t c = 0; plan_ok && c < (read_base ? layer_count : 0u); ++c) {
            plan_ok = PlanFingerprintRead(plan_reads_, n_reads, dir_base + c * face_stride, texture_tile,
                                           blocks_pitch, uint64_t(std::max(blocks_pitch, blocks_x)) * tf.bytes, tf,
                                           base_ox, base_oy, blocks_x, blocks_y, bx_base, level_layer[0] * c,
                                           raw_start, extension, 0);
          }
          for (uint32_t n = 1; plan_ok && n < texture.levels; ++n) {
            const uint32_t s = packed_level == 0 ? 0 : std::min(n, packed_level);
            uint32_t ox = 0;
            uint32_t oy = 0;
            if (n >= packed_level) {
              PackedOffset(width_px, height_px, tf.block, n, ox, oy);
            }
            const uint32_t guest_bx = (std::max(width_px >> n, 1u) + tf.block - 1) / tf.block;
            const uint32_t guest_by = (std::max(height_px >> n, 1u) + tf.block - 1) / tf.block;
            const uint32_t bx_host = (std::max(texture.img.width_px >> n, 1u) + tf.block - 1) / tf.block;
            const uint32_t by_host = (std::max(texture.img.height_px >> n, 1u) + tf.block - 1) / tf.block;
            for (uint32_t c = 0; plan_ok && c < layer_count; ++c) {
              plan_ok = PlanFingerprintRead(plan_reads_, n_reads,
                                             dir_mips + regions[s].byte_offset + c * regions[s].stride_bytes,
                                             texture_tile, regions[s].blocks_pitch, regions[s].row_bytes, tf,
                                             ox, oy, std::min(guest_bx, bx_host), std::min(guest_by, by_host),
                                             bx_host, offsets[n] + level_layer[n] * c, dir_mips, extension_mips,
                                             mips_in_copy);
            }
          }
          if (plan_ok && PlanFingerprint(texture, key, n_reads, base_raw, extension, mips_raw, extension_mips,
                                       tf, (f[1] >> 6) & 0x3, bytes_plan, offsets, base, width_px, height_px,
                                       format_id, upload_bytes)) {
            return;  // applying phase: the thread prepares it; the tail of this function is already done
          }
        }
        uint64_t raw_fingerprint = XXH3_64bits(base_raw, size_t(extension));
        if (extension_mips) {  // and the bytes of all the mips
          raw_fingerprint = XXH3_64bits_withSeed(mips_raw, size_t(extension_mips), raw_fingerprint);
        }
        nfsmw::waits::g_raw_fingerprint_ns.fetch_add(
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                          raw_fingerprint_start)
                         .count()),
            std::memory_order_relaxed);
        const bool equal_complete = texture.img.prepared && raw_fingerprint == texture.raw_fingerprint;
        if (usable_sample && !only_sample) {
          // The guard. This stable recheck computed both hashes.
          ++checked_samples_;
          ++samples_with_two_;
          if (equal_sample && !equal_complete) {
            REXLOG_ERROR("[nativo] C3: la MUESTRA de la textura {:08X} {}x{} formato {} ({} KB de base y {} KB de "
                         "mips) no ha visto un cambio que la huella completa si. Se APAGA el muestreo de huellas "
                         "(tras {} recomprobaciones con las dos).",
                         uint32_t(raw_start), width_px, height_px, format_id, extension >> 10, extension_mips >> 10,
                         checked_samples_);
            fingerprints_sampling_ = 0;
          } else if (checked_samples_ == kSamplesToCheck) {
            REXLOG_INFO("[nativo] C3: muestreo de huellas: {} recomprobaciones con las dos huellas, ningun "
                        "desacuerdo. Sigue el muestreo, con 1 de cada {} completa.",
                        checked_samples_, fingerprints_sampling_);
          }
        }
        if (equal_complete) {
          // The sample of this content, for the stable rechecks.
          if (with_sample && fingerprints_sampling_ > 0) {
            texture.sample_fingerprint = sample;
            texture.valid_sample = true;
          }
          texture.consecutive_samples = 0;
          texture.interval = std::min<uint32_t>(texture.interval * 2, 32);
          // At the maximum interval, from 32 to 39 depending on the texture, so they do not coincide.
          texture.next = frame_ + texture.interval + (texture.interval == 32 ? (key >> 7) & 7 : 0);
          valid_until = texture.next - 1;
          return;
        }
        texture.valid_sample = false;  // the content changed; the sample is no longer valid
        texture.consecutive_samples = 0;
        texture.raw_fingerprint = raw_fingerprint;
        TrackTextureContent(texture, key);  // measurement only: nfsmw_nativo_diag_reutilizar
      }
    }
    // Base level of each layer, untiled and in host byte order.
    const uint32_t host_blocks_x = (texture.img.width_px + tf.block - 1) / tf.block;
    const uint32_t host_blocks_y = (texture.img.height_px + tf.block - 1) / tf.block;
    const size_t bytes_capa = size_t(host_blocks_x) * host_blocks_y * tf.bytes;
    std::vector<uint8_t>& payload = temporal_;
    // All levels back to back, each one layer after layer (SubirTextura copies them one by one).
    std::array<size_t, 16> level_layer_bytes{};
    size_t data_bytes = 0;
    for (uint32_t n = 0; n < texture.levels; ++n) {
      const uint32_t bx_host = (std::max(texture.img.width_px >> n, 1u) + tf.block - 1) / tf.block;
      const uint32_t by_host = (std::max(texture.img.height_px >> n, 1u) + tf.block - 1) / tf.block;
      texture.level_offset[n] = uint32_t(data_bytes);
      level_layer_bytes[n] = size_t(bx_host) * by_host * tf.bytes;
      data_bytes += level_layer_bytes[n] * layer_count * (background ? background : 1);
    }
    payload.assign(data_bytes, 0);
    const bool tile = (f[0] >> 31) & 0x1;
    if (volume && !ReadVolume(f, tf, blocks_x, blocks_y, background, host_blocks_x,
                                host_blocks_y, blocks_pitch, tile, payload)) {
      Warn(33, "textura fuera de la memoria");
      if (!texture.img.prepared) {
        ranura = 0;  // the image remains uninitialized
      }
      return;
    }
    // Base level: not read if not needed (minimum level 1; it stays zeroed and the sampler never goes below
    // 1).
    for (uint32_t c = 0; c < (volume || !read_base ? 0u : layer_count); ++c) {
      const uint64_t address = dir_base + c * face_stride;
      if (!ReadLevel(address, tile, blocks_pitch, uint64_t(std::max(blocks_pitch, blocks_x)) * tf.bytes, tf,
                     base_ox, base_oy, blocks_x, blocks_y, host_blocks_x, payload.data() + bytes_capa * c)) {
        Warn(33, "textura fuera de la memoria");
        if (!texture.img.prepared) {
          ranura = 0;  // the image remains uninitialized
        }
        return;
      }
    }
    // Mip levels. Those in the packed tail share a region and are located with DesplazamientoEmpaquetado;
    // the rest, each in its own.
    for (uint32_t n = 1; n < texture.levels; ++n) {
      const uint32_t s = packed_level == 0 ? 0 : std::min(n, packed_level);
      uint32_t ox = 0;
      uint32_t oy = 0;
      if (n >= packed_level) {
        PackedOffset(width_px, height_px, tf.block, n, ox, oy);
      }
      const uint32_t guest_bx = (std::max(width_px >> n, 1u) + tf.block - 1) / tf.block;
      const uint32_t guest_by = (std::max(height_px >> n, 1u) + tf.block - 1) / tf.block;
      const uint32_t bx_host = (std::max(texture.img.width_px >> n, 1u) + tf.block - 1) / tf.block;
      const uint32_t by_host = (std::max(texture.img.height_px >> n, 1u) + tf.block - 1) / tf.block;
      for (uint32_t c = 0; c < layer_count; ++c) {
        const uint64_t address = dir_mips + regions[s].byte_offset + c * regions[s].stride_bytes;
        if (!ReadLevel(address, tile, regions[s].blocks_pitch, regions[s].row_bytes, tf, ox, oy,
                       std::min(guest_bx, bx_host), std::min(guest_by, by_host), bx_host,
                       payload.data() + texture.level_offset[n] + level_layer_bytes[n] * c)) {
          Warn(39, "nivel de mip fuera de la memoria: se queda a cero");
          break;
        }
      }
    }
    const auto orden = static_cast<xenos::Endian>((f[1] >> 6) & 0x3);
    // In one go (CambiarOrdenBytes), with the same guard as the fast untiling.
    if (fast_tile_ > 0 && orden != xenos::Endian::kNone && (tf.order_unit == 2 || tf.order_unit == 4)) {
      // The first 200, then 1 in 64. Checking every texture, this guard never got to switch itself off (there
      // were only 1,024 textures) and cost 2.1 ms per MB on every new texture.
      ++seen_commands_;
      const bool run_check = seen_commands_ <= kLevelsToCheck || seen_commands_ % kCheckEvery == 0;
      if (run_check) {
        tile_check_ = payload;
      }
      ChangeOrderBytes(payload.data(), payload.size(), tf.order_unit, uint32_t(orden));
      if (run_check) {
        ++checked_commands_;
        AlwaysOrder(tile_check_, tf.order_unit, orden);
        if (tile_check_ != payload) {
          REXLOG_ERROR("[nativo] C3: el cambio de orden rapido NO coincide (unidad {}, orden {}, {} bytes). Se "
                       "APAGAN el orden y el desenmosaicado rapidos.",
                       tf.order_unit, uint32_t(orden), payload.size());
          payload.swap(tile_check_);
          fast_tile_ = 0;
        } else if (checked_commands_ == kLevelsToCheck) {
          REXLOG_INFO("[nativo] C3: cambio de orden rapido: {} texturas comprobadas contra el de siempre, todas "
                      "iguales.",
                      checked_commands_);
        }
      }
    } else {
      AlwaysOrder(payload, tf.order_unit, orden);
    }
    if (diag_mips_ && texture.levels > 1 && read_base && !volume) {
      ReviewMips(base, format_id, width_px, height_px, tf, texture, level_layer_bytes, payload);
    }
    /*
     * For a new (unprepared) texture this hash decides nothing: it is uploaded regardless. 0 is stored, and
     * the first time its memory changes it will be computed and uploaded (at most, one extra upload).
     */
    const auto data_fingerprint_start = std::chrono::steady_clock::now();
    const uint64_t fingerprint = texture.img.prepared ? XXH3_64bits(payload.data(), payload.size()) : 0;
    nfsmw::waits::g_data_fingerprint_ns.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                      data_fingerprint_start)
                     .count()),
        std::memory_order_relaxed);
    if (texture.img.prepared && fingerprint == texture.fingerprint) {
      texture.interval = std::min<uint32_t>(texture.interval * 2, 32);
      texture.next = frame_ + texture.interval;
      valid_until = texture.next - 1;
      return;
    }
    // Observing phase of the hash thread: what the usual path produces, to compare when collecting.
    if (texture_comparison_ == &texture) {
      TrackFingerprintCompare(texture, payload);
    }
    texture.interval = 1;
    texture.next = frame_ + 1;
    texture.fingerprint = fingerprint;
    texture.payload.swap(payload);
    texture.do_upload = true;
    textures_to_upload_.push_back(&texture);
    nfsmw::waits::g_uploads_textures.fetch_add(1, std::memory_order_relaxed);
    nfsmw::waits::g_uploaded_bytes.fetch_add(texture.payload.size(), std::memory_order_relaxed);
    upload_bytes += (texture.payload.size() + 3) & ~size_t(3);
  }

  // --- Measurement only: new textures with the content of another (nfsmw_nativo_diag_reutilizar) ---------------
  /*
   * What it is for. The game reloads the zone packs at new addresses every time it returns to a zone (in
   * the cache diagnostic, of 1,024 textures created, only 6 landed at an address already used). Since the
   * address is part of the key, a returning texture is a new texture: the image is created, bound,
   * untiled and uploaded again, even though the same image, with the same bytes, is still in the cache
   * under the old key and nothing uses it. This measures how often that happens:
   *   - how many new textures have the same content and shape as a live one (the image would be the same);
   *   - how many of those others are cold: not checked for more than kFotogramasSinUsoParaSoltar frames,
   *     which is what eviction uses to mean "unused" (a texture in use is checked at least every 39
   *     frames). Those are the duplicates the cache holds that nothing in use depends on.
   * With MB, to see how much GPU memory and upload traffic is behind them. It changes nothing: no
   * decision of the ring reads what is recorded here.
   *
   * How. vivas_por_contenido_ holds every texture with a valid raw hash under its content key: the XXH3
   * of its shape without the address (the key words without the base and mip addresses, plus the
   * VkFormat, the host size, the levels and the layers) seeded with its raw hash (the guest bytes of base
   * and mips). Two textures with the same content key have the same bytes read with the same layout: the
   * same host data. It is maintained:
   *   - when the image is created (the previous one is removed if the image is recreated);
   *   - when the raw hash is set: the usual path of PrepararTextura, and RecogerHuellas (hash thread);
   *   - when the texture is released: SoltarImagenes, which the gradual, batch and out-of-memory
   *     evictions all go through.
   * A new texture, once its first hash is known, looks at those with the same content key, at most
   * kReutilizarMirarMax: O(1) per texture, never a walk over the live ones. Removing one is O(k), with k
   * the ones with the same content (the repeats; almost always 0 or 1). Candidates are validated when
   * looked at (still under that content key, with a prepared image and no pending hash thread job), so a
   * stale entry cannot count.
   */
  static constexpr uint32_t kReusePeekMax = 8;  // candidates checked per new texture
  static constexpr uint32_t kReuseDetails = 8;  // lines detailing the first matches

  bool DiagReuse() {
    if (diag_reuse_ < 0) {
      diag_reuse_ = REXCVAR_GET(nfsmw_nativo_diag_reutilizar) ? 1 : 0;
      REXLOG_INFO("[nativo] C3: medida de texturas nuevas con el contenido de otra viva "
                  "(nfsmw_nativo_diag_reutilizar) = {}",
                  diag_reuse_ ? "SI (solo cuenta: no cambia nada)" : "no");
    }
    return diag_reuse_ > 0;
  }

  // The content key of a texture with a valid raw hash. Never 0 (0 = not recorded).
  static uint64_t ContentKey(const Texture& texture) {
    const uint64_t shape[4] = {texture.content_shape, (uint64_t(texture.img.format_id) << 32) | texture.levels,
                               (uint64_t(texture.img.width_px) << 32) | texture.img.height_px, texture.layer_count};
    return XXH3_64bits_withSeed(shape, sizeof(shape), texture.raw_fingerprint) | 1;
  }

  // Stops counting a texture (when it is released, when its image is recreated, or before recording it
  // with another hash).
  void RemoveTextureContent(Texture& texture, uint64_t key) {
    if (!texture.content_key) {
      return;
    }
    const auto [from, hasta] = alive_per_content_.equal_range(texture.content_key);
    for (auto it = from; it != hasta; ++it) {
      if (it->second == key) {
        alive_per_content_.erase(it);
        break;
      }
    }
    if (alive_per_content_.find(texture.content_key) == alive_per_content_.end() && distinct_contents_) {
      --distinct_contents_;
    }
    texture.content_key = 0;
  }

  // With the raw hash just set. If the texture is new, counts whether another live one has the same
  // content and shape, and whether that other one is cold. Then records it under its content key. Ring
  // thread only.
  void TrackTextureContent(Texture& texture, uint64_t key) {
    if (!DiagReuse()) {
      return;
    }
    RemoveTextureContent(texture, key);
    const uint64_t content = ContentKey(texture);
    if (texture.content_per_measure) {
      texture.content_per_measure = false;
      ++reuse_new_;
      reuse_new_bytes_ += texture.bytes;
      const Texture* is_equal = nullptr;
      const Texture* cold = nullptr;
      uint32_t peeked = 0;
      const auto [from, hasta] = alive_per_content_.equal_range(content);
      for (auto it = from; it != hasta; ++it) {
        if (peeked++ >= kReusePeekMax) {
          ++reuse_without_peek_;
          break;
        }
        const auto another = textures_.find(it->second);
        if (another == textures_.end() || &another->second == &texture) {
          continue;
        }
        const Texture& t = another->second;
        if (t.content_key != content || t.img.img == VK_NULL_HANDLE || !t.img.prepared ||
            t.job_fingerprint) {
          continue;
        }
        is_equal = &t;
        if (t.frame != UINT64_MAX && t.frame + kFramesWithoutUseForRelease < frame_) {
          cold = &t;
          break;  // one cold one is enough
        }
      }
      if (is_equal) {
        ++reuse_match_;
        reuse_match_bytes_ += texture.bytes;
      }
      if (cold) {
        ++reuse_cold_;
        reuse_cold_bytes_ += texture.bytes;
      }
      if (is_equal && reuse_details_ < kReuseDetails) {
        ++reuse_details_;
        const Texture& t = cold ? *cold : *is_equal;
        NFSMW_RING_REPORT("[nativo] C3 reutilizar por contenido (build 186, solo medida): la textura nueva {:08X} "
                             "({}x{}, VkFormat {}, {} niveles, {} capas, {} KB) tiene el mismo contenido y forma que "
                             "la {:08X}, {}",
                             texture.address, texture.img.width_px, texture.img.height_px,
                             uint32_t(texture.img.format_id), texture.levels, texture.layer_count, texture.bytes >> 10,
                             t.address,
                             cold ? fmt::format("sin comprobarse desde hace {} fotogramas", frame_ - t.frame)
                                  : std::string("que sigue en uso"));
      }
    }
    if (alive_per_content_.find(content) == alive_per_content_.end()) {
      ++distinct_contents_;
    }
    alive_per_content_.emplace(content, key);
    texture.content_key = content;
  }

  // Every 10 s, if there were new textures with a hash. The cache snapshot (recorded, distinct contents)
  // is O(1).
  void ReuseReport() {
    if (diag_reuse_ <= 0) {
      return;
    }
    const auto now_time = std::chrono::steady_clock::now();
    if (now_time - reuse_report_ < std::chrono::seconds(10)) {
      return;
    }
    reuse_report_ = now_time;
    if (reuse_new_) {
      const uint64_t pointed = uint64_t(alive_per_content_.size());
      NFSMW_RING_REPORT(
          "[nativo] C3 reutilizar por contenido (build 186, solo medida), ultimos 10 s: {} texturas nuevas con huella "
          "({:.1f} MB); {} con el mismo contenido y forma que otra viva ({:.1f} MB), {} de ellas con una sin "
          "comprobarse desde hace mas de {} fotogramas ({:.1f} MB); {} con mas de {} iguales sin mirar enteras | en la "
          "cache: {} texturas con huella en {} contenidos distintos ({} repetidas)",
          reuse_new_, double(reuse_new_bytes_) / 1048576.0, reuse_match_,
          double(reuse_match_bytes_) / 1048576.0, reuse_cold_, kFramesWithoutUseForRelease,
          double(reuse_cold_bytes_) / 1048576.0, reuse_without_peek_, kReusePeekMax, pointed,
          distinct_contents_, pointed - std::min(pointed, distinct_contents_));
    }
    reuse_new_ = 0;
    reuse_match_ = 0;
    reuse_cold_ = 0;
    reuse_without_peek_ = 0;
    reuse_new_bytes_ = 0;
    reuse_match_bytes_ = 0;
    reuse_cold_bytes_ = 0;
  }

  // Every 30 s, one line with what hash sampling has done (see PrepararTextura). The ring thread writes
  // it, but it is a single short line every 30 s.
  void SamplingReport(std::chrono::steady_clock::time_point now_time) {
    if (now_time - sampling_report_ < std::chrono::seconds(30)) {
      return;
    }
    if (sampling_report_ != std::chrono::steady_clock::time_point{}) {
      NFSMW_RING_REPORT("[nativo] C3 muestreo de huellas: {} recomprobaciones resueltas con la muestra ({:.1f} MB leidos y "
                  "{:.1f} MB sin leer), {} con la muestra cambiada y {} con las dos huellas desde la linea anterior; "
                  "{} con las dos desde el principio",
                  hits_samples_, double(sample_bytes_) / 1048576.0,
                  double(sample_saved_bytes_) / 1048576.0, distinct_samples_, samples_with_two_,
                  checked_samples_);
    }
    sampling_report_ = now_time;
    hits_samples_ = 0;
    distinct_samples_ = 0;
    samples_with_two_ = 0;
    sample_bytes_ = 0;
    sample_saved_bytes_ = 0;
  }

  // nfsmw_nativo_diag_mips: average color of each level of the first layer against the base's, for
  // DXT1/3/5 (average of the two colors of each block, in RGB565) and 8888. Only levels of 2x2 blocks or
  // more: the small ones are too little data for an average. Counts the textures with any level more than
  // 32/255 away from the base in any channel and logs the first 12 with their averages.
  void ReviewMips(uint32_t base, uint32_t format_id, uint32_t width_px, uint32_t height_px, const TextureFormat& tf,
                   const Texture& texture, const std::array<size_t, 16>& level_layer_bytes,
                   const std::vector<uint8_t>& payload) {
    const bool dxt = tf.format_id == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || tf.format_id == VK_FORMAT_BC2_UNORM_BLOCK ||
                     tf.format_id == VK_FORMAT_BC3_UNORM_BLOCK;
    if (!dxt && tf.format_id != VK_FORMAT_R8G8B8A8_UNORM) {
      return;
    }
    const auto media = [&](uint32_t n, std::array<double, 3>& rgb) {
      const size_t start = texture.level_offset[n];
      const size_t bytes = level_layer_bytes[n];
      if (start + bytes > payload.size() || bytes < 4u * tf.bytes) {
        return false;
      }
      rgb = {0.0, 0.0, 0.0};
      size_t num = 0;
      for (size_t i = 0; i + tf.bytes <= bytes; i += tf.bytes) {
        const uint8_t* p = payload.data() + start + i;
        if (dxt) {
          const uint8_t* color = tf.format_id == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? p : p + 8;
          for (uint32_t k = 0; k < 2; ++k) {
            const uint16_t v = uint16_t(color[2 * k] | (color[2 * k + 1] << 8));
            rgb[0] += ((v >> 11) & 31) * (255.0 / 31.0);
            rgb[1] += ((v >> 5) & 63) * (255.0 / 63.0);
            rgb[2] += (v & 31) * (255.0 / 31.0);
          }
          num += 2;
        } else {
          rgb[0] += p[0];
          rgb[1] += p[1];
          rgb[2] += p[2];
          num += 1;
        }
      }
      for (double& c : rgb) {
        c /= double(num);
      }
      return true;
    };
    std::array<double, 3> base_rgb{};
    if (!media(0, base_rgb)) {
      return;
    }
    ++reviewed_mips_;
    std::string levels = fmt::format("[0]={:.0f}/{:.0f}/{:.0f}", base_rgb[0], base_rgb[1], base_rgb[2]);
    double worst = 0.0;
    for (uint32_t n = 1; n < texture.levels; ++n) {
      std::array<double, 3> rgb{};
      if (!media(n, rgb)) {
        break;
      }
      for (uint32_t c = 0; c < 3; ++c) {
        worst = std::max(worst, std::abs(rgb[c] - base_rgb[c]));
      }
      levels += fmt::format(" [{}]={:.0f}/{:.0f}/{:.0f}", n, rgb[0], rgb[1], rgb[2]);
    }
    if (worst > 32.0) {
      if (++rare_mips_ <= 12) {
        REXLOG_WARN("[nativo] C3 diag mips: textura {:08X} {}x{} formato {} con {} niveles: un nivel se aleja {:.0f} "
                    "de la base; medias RGB {}",
                    base, width_px, height_px, format_id, texture.levels, worst, levels);
      }
    }
    if (reviewed_mips_ % 200 == 0) {
      REXLOG_INFO("[nativo] C3 diag mips: {} texturas revisadas, {} con algun nivel a mas de 32/255 de la base",
                  reviewed_mips_, rare_mips_);
    }
  }

  // The blocks of one level (one layer) of a 2D texture or cubemap, from block (ox, oy) of its guest
  // region (the packed tail or the base of a small texture do not start at 0), to the host destination
  // bx_host blocks wide. Linear: fila_bytes per row; tiled: pitch in blocks (GetTiledOffset2D). false if
  // it goes past memory.
  bool ReadLevel(uint64_t address, bool tile, uint32_t blocks_pitch, uint64_t row_bytes,
                 const TextureFormat& tf, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by, uint32_t bx_host,
                 uint8_t* target) {
    if (!bx || !by) {
      return true;
    }
    const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2 : tf.bytes >= 2 ? 1 : 0;
    const uint64_t extreme =
        tile ? uint64_t(std::max<int64_t>(Tile2dOffset(int32_t((ox + bx + 31) & ~31u),
                                                                     int32_t((oy + by + 31) & ~31u),
                                                                     blocks_pitch, log2),
                                             0))
                : row_bytes * (oy + by - 1) + uint64_t(ox + bx) * tf.bytes;
    if (address + extreme > kPhysicalMemory) {
      return false;
    }
    const uint8_t* src = mem_->TranslatePhysical(uint32_t(address));
    if (!tile) {
      for (uint32_t y = 0; y < by; ++y) {
        std::memcpy(target + size_t(y) * bx_host * tf.bytes,
                    src + uint64_t(oy + y) * row_bytes + uint64_t(ox) * tf.bytes, size_t(bx) * tf.bytes);
      }
      return true;
    }
    // The fast untiling (DesenmosaicarNivel) when the block is 1, 2, 4, 8 or 16 bytes.
    if (fast_tile_ < 0) {
      fast_tile_ = REXCVAR_GET(nfsmw_nativo_mosaico_rapido) ? 1 : 0;
      REXLOG_INFO("[nativo] C3: desenmosaicado rapido (nfsmw_nativo_mosaico_rapido) = {}",
                  fast_tile_ ? "SI, comprobando los primeros niveles contra el de siempre" : "no");
    }
    if (fast_tile_ && tf.bytes == (1u << log2)) {
      switch (log2) {
        case 0: UntileLevel<0>(src, blocks_pitch, ox, oy, bx, by, bx_host, target); break;
        case 1: UntileLevel<1>(src, blocks_pitch, ox, oy, bx, by, bx_host, target); break;
        case 2: UntileLevel<2>(src, blocks_pitch, ox, oy, bx, by, bx_host, target); break;
        case 3: UntileLevel<3>(src, blocks_pitch, ox, oy, bx, by, bx_host, target); break;
        default: UntileLevel<4>(src, blocks_pitch, ox, oy, bx, by, bx_host, target); break;
      }
      /*
       * Guard: the first kNivelesAComprobar levels are repeated on the usual path and compared byte by byte.
       * If one differs, the usual result is kept, a warning is logged and the fast path is off for the rest
       * of the session.
       */
      // The first 200, then 1 in 64 (checking every level, the guard cost 4.2 ms per MB).
      ++seen_levels_;
      if (seen_levels_ <= kLevelsToCheck || seen_levels_ % kCheckEvery == 0) {
        ++checked_levels_;
        tile_check_.assign(size_t(by) * bx_host * tf.bytes, 0);
        for (uint32_t y = 0; y < by; ++y) {
          std::memcpy(tile_check_.data() + size_t(y) * bx_host * tf.bytes,
                      target + size_t(y) * bx_host * tf.bytes, size_t(bx) * tf.bytes);
        }
        ReadLevelTileAsAlways(src, blocks_pitch, log2, tf.bytes, ox, oy, bx, by, bx_host, target);
        bool all_equal = true;
        for (uint32_t y = 0; y < by && all_equal; ++y) {
          all_equal = std::memcmp(tile_check_.data() + size_t(y) * bx_host * tf.bytes,
                                target + size_t(y) * bx_host * tf.bytes, size_t(bx) * tf.bytes) == 0;
        }
        if (!all_equal) {
          REXLOG_ERROR("[nativo] C3: el desenmosaicado rapido NO coincide ({}x{} bloques de {} bytes, pitch {}, "
                       "desde {},{}). Se APAGA y se usa el de siempre.",
                       bx, by, tf.bytes, blocks_pitch, ox, oy);
          fast_tile_ = 0;
        } else if (checked_levels_ == kLevelsToCheck) {
          REXLOG_INFO("[nativo] C3: desenmosaicado rapido: {} niveles comprobados contra el de siempre, todos "
                      "iguales. Sigue el rapido.",
                      checked_levels_);
        }
      }
      return true;
    }
    ReadLevelTileAsAlways(src, blocks_pitch, log2, tf.bytes, ox, oy, bx, by, bx_host, target);
    return true;
  }

  // The original byte swap, word by word.
  static void AlwaysOrder(std::vector<uint8_t>& payload, uint32_t unit, xenos::Endian orden) {
    if (unit == 2 && orden != xenos::Endian::kNone) {
      for (size_t i = 0; i + 1 < payload.size(); i += 2) {
        uint16_t v;
        std::memcpy(&v, payload.data() + i, 2);
        v = xenos::GpuSwap(v, orden);
        std::memcpy(payload.data() + i, &v, 2);
      }
    } else if (unit == 4 && orden != xenos::Endian::kNone) {
      for (size_t i = 0; i + 3 < payload.size(); i += 4) {
        uint32_t v;
        std::memcpy(&v, payload.data() + i, 4);
        v = xenos::GpuSwap(v, orden);
        std::memcpy(payload.data() + i, &v, 4);
      }
    }
  }

  // The original untiling, block by block.
  static void ReadLevelTileAsAlways(const uint8_t* src, uint32_t blocks_pitch, uint32_t log2, uint32_t bytes,
                                        uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by, uint32_t bx_host,
                                        uint8_t* target) {
    for (uint32_t y = 0; y < by; ++y) {
      for (uint32_t x = 0; x < bx; ++x) {
        const int32_t byte_offset =
            Tile2dOffset(int32_t(ox + x), int32_t(oy + y), blocks_pitch, log2);
        std::memcpy(target + (size_t(y) * bx_host + x) * bytes, src + byte_offset, bytes);
      }
    }
  }

  // Base level of a 3D texture, slice by slice (z, then y, then x), in the order vkCmdCopyBufferToImage
  // uploads it. Tiled: GetTiledOffset3D; linear: consecutive slices with rows and columns aligned to 32
  // blocks (GetGuestTextureLayout).
  bool ReadVolume(const uint32_t* f, const TextureFormat& tf, uint32_t blocks_x,
                   uint32_t blocks_y, uint32_t background, uint32_t host_blocks_x,
                   uint32_t host_blocks_y, uint32_t blocks_pitch, bool tile,
                   std::vector<uint8_t>& payload) {
    const uint64_t address = uint64_t((f[1] >> 12) << 12) & 0x1FFFFFFF;
    const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2
                                                             : tf.bytes >= 2 ? 1 : 0;
    const uint64_t row = uint64_t((blocks_pitch + 31) & ~uint32_t(31)) * tf.bytes;
    const uint64_t cut = row * ((blocks_y + 31) & ~uint32_t(31));
    const uint64_t extreme =
        tile ? uint64_t(std::max<int64_t>(
                      Tile3dOffset(int32_t((blocks_x + 31) & ~31u),
                                              int32_t((blocks_y + 31) & ~31u),
                                              int32_t((background + 3) & ~3u), blocks_pitch,
                                              blocks_y, log2),
                      0))
                : cut * background;
    if (address + extreme > kPhysicalMemory) {
      return false;
    }
    const uint8_t* src = mem_->TranslatePhysical(uint32_t(address));
    const size_t cut_bytes = size_t(host_blocks_x) * host_blocks_y * tf.bytes;
    for (uint32_t z = 0; z < background; ++z) {
      for (uint32_t y = 0; y < blocks_y; ++y) {
        for (uint32_t x = 0; x < blocks_x; ++x) {
          const uint64_t byte_offset =
              tile ? uint64_t(Tile3dOffset(int32_t(x), int32_t(y), int32_t(z),
                                                         blocks_pitch, blocks_y, log2))
                      : z * cut + y * row + uint64_t(x) * tf.bytes;
          std::memcpy(payload.data() + cut_bytes * z + (size_t(y) * host_blocks_x + x) * tf.bytes,
                      src + byte_offset, tf.bytes);
        }
      }
    }
    return true;
  }

  // A texture's VkImageCreateInfo, factored out so CrearTextura and CrearTexturaEnHilo create exactly the
  // same image.
  static VkImageCreateInfo TextureImageInfo(VkFormat format_id, uint32_t width_px, uint32_t height_px, uint32_t layer_count,
                                             uint32_t background, uint32_t levels) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = background ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.flags = layer_count == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    info.format = format_id;
    info.extent = {width_px, height_px, background ? background : 1};
    info.mipLevels = std::max(levels, 1u);
    info.arrayLayers = layer_count;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return info;
  }

  // `emergencia` = false only from RecogerEnlaces. Half the cache cannot be released there: that waits
  // for the GPU by submitting the work, and we are inside AntesDeEnviar. Out of memory: false, and the
  // texture is recreated later.
  bool CreateTexture(NativeImage& img, VkFormat format_id, uint32_t width_px, uint32_t height_px,
                    uint32_t layer_count = 1, uint32_t background = 0, uint32_t levels = 1, bool emergency = true) {
    const VkImageCreateInfo info = TextureImageInfo(format_id, width_px, height_px, layer_count, background, levels);
    /*
     * The pool first, and the usual path only if it does not fit.
     *
     * A dedicated allocation per texture costs 1.9 ms of CPU on Horizon, measured inside the ioctls:
     * nvMapCreate 422 us, plus two GPU address reservations (127 us each) and two mappings (626 us each).
     * There are two of each because the dedicated allocation makes its own address and mapping, and then
     * nvk_image_plane_bind makes others for the plane. And the only thing the dedicated allocation buys is
     * compression, which our textures never use (nvk_image.c:843-846 returns false with
     * SAMPLED|TRANSFER_DST). So we paid double and got nothing.
     *
     * Sub-allocating from large slabs removes the allocation's address and mapping: 1.9 -> 0.75 ms. For a
     * burst of 15 textures in one frame, from 28.5 to 11.3 ms. That is what shows when entering a new zone
     * of the map.
     *
     * It does not change a single pixel: same tiling, same format, same pte_kind. Binding at an offset
     * keeps the block-linear layout (nvk_image.c:1700-1710) and is the normal path of any sub-allocator in
     * NVK.
     */
    bool reserved = false;
    if (textures_pool_.Active()) {
      VkImage pool_image = VK_NULL_HANDLE;
      if (dfn_.vkCreateImage(device_, &info, nullptr, &pool_image) == VK_SUCCESS) {
        VkMemoryRequirements req{};
        dfn_.vkGetImageMemoryRequirements(device_, pool_image, &req);
        VkDeviceMemory block = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        uint32_t block_id = 0xFFFFFFFFu;
        if (textures_pool_.Reserve(req, block, offset, block_id) &&
            dfn_.vkBindImageMemory(device_, pool_image, block, offset) == VK_SUCCESS) {
          img.img = pool_image;
          img.mem = VK_NULL_HANDLE;  // from the pool: not freed on its own
          img.block_pool = block_id;
          reserved = true;
        } else {
          if (block_id != 0xFFFFFFFFu) {
            textures_pool_.Dispose(block_id);
          }
          dfn_.vkDestroyImage(device_, pool_image, nullptr);
          textures_pool_.TrackDedicated();
        }
      }
    }
    if (!reserved) {
      img.block_pool = 0xFFFFFFFFu;
      reserved = rex::ui::vulkan::util::CreateDedicatedAllocationImage(
          vk_device_, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, img.img,
          img.mem);
    }
    if (reserved && test_without_memory_every_ > 0 &&
        ++texture_reserves_ % uint64_t(test_without_memory_every_) == 0) {
      // Test: undo the successful allocation and continue as if it had failed.
      DestroyImage(img);
      reserved = false;
      REXLOG_INFO("[nativo] C3: prueba, se finge que falta memoria en la reserva {}", texture_reserves_);
    }
    if (!reserved) {
      // Out of GPU memory. Instead of giving up (which ends in a black screen), half the cache is released
      // with the GPU idle and the allocation is retried once.
      // The retry always takes the dedicated path, on purpose. Getting here means memory is short, and the
      // dedicated path is the one that can ask the system for more; the pool only hands out what it already
      // has. pool_bloque is left invalid so DestruirImagen does not touch the pool.
      img.block_pool = 0xFFFFFFFFu;
      if (!emergency || !ReleaseTexturesOnMemoryShortage() ||
          !rex::ui::vulkan::util::CreateDedicatedAllocationImage(
              vk_device_, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, img.img,
              img.mem)) {
        REXLOG_ERROR("[nativo] C3: sin memoria en la GPU para una textura de {}x{} ({} niveles, {} capas) y "
                     "{}; quedan {} texturas y {} MB",
                     width_px, height_px, std::max(levels, 1u), layer_count,
                     emergency ? "soltar la cache no ha bastado"
                                : "sin soltar la cache (desde RecogerEnlaces; se recrea en su proxima comprobacion)",
                     textures_.size(), textures_bytes_ >> 20);
        return false;
      }
    }
    img.width_px = width_px;
    img.height_px = height_px;
    img.format_id = format_id;
    img.prepared = false;
    return true;
  }

  bool UploadTexture(Texture& texture) {
    const VkCommandBuffer upload = contexto_->UploadCommands();
    if (!upload || !texture.do_upload) {
      return upload != VK_NULL_HANDLE;
    }
    VkDeviceSize offset;
    /*
     * If the hash thread prepares it, its data does not exist yet: its space is reserved (same size, same
     * point) and the thread writes it there; RecogerHuellas waits for it before submitting. The barrier
     * and the copy are recorded below as usual: the GPU does not read the upload buffer until
     * vkQueueSubmit.
     */
    if (texture.job_fingerprint && texture.job_fingerprint != kPublishedFingerprintJob) {
      const size_t bytes = PlannedFingerprintBytes(texture);
      Reserve(bytes, 16, offset);  // BC: offset multiple of the block
      PublishFingerprint(texture, data_upload_ + offset, bytes);
    } else {
      Reserve(texture.payload.size(), 16, offset);  // BC: offset multiple of the block
      std::memcpy(data_upload_ + offset, texture.payload.data(), texture.payload.size());
    }
    /*
     * If its vkBindImageMemory is still on the bind thread, neither the barrier nor the copy can be
     * recorded for the image (it has no memory). The data is already in the upload buffer; the barrier and
     * the copy are recorded in RecogerEnlaces, in this same upload buffer before it is closed
     * (AntesDeEnviar).
     */
    if (texture.in_flight && !DeferCopy(texture, offset, upload)) {
      CollectBindings(true);  // inconsistent index (AplazarCopia already logged it and switched off): collect everything here
    }
    if (!texture.in_flight && texture.img.img != VK_NULL_HANDLE) {
      if (!texture.img.prepared) {
        Barrier(upload, texture.img.img, texture.layer_count);
        texture.img.prepared = true;
      }
      RecordTextureCopy(upload, texture, offset);
    }
    texture.do_upload = false;
    /*
     * The copy of the pixels is no longer needed. They are already in the upload buffer, and to know
     * whether the texture changes its hashes are kept, not the bytes. Keeping the copy made the texture
     * cache take the same space again in CPU RAM (150-680 MB). That once ended in std::bad_alloc in the
     * main menu, with the cache at 682 MB.
     */
    // The larger buffer is kept as temporal_ (so the next new texture does not ask the system for memory
    // again or touch fresh pages); the other is released.
    if (texture.payload.capacity() > temporal_.capacity()) {
      temporal_.swap(texture.payload);
    }
    std::vector<uint8_t>().swap(texture.payload);
    ++texture_uploads_;
    return true;
  }

  // One range per mip level (with all its layers). Factored out for SubirTextura and for the deferred
  // copies of RecogerEnlaces.
  void RecordTextureCopy(VkCommandBuffer upload, const Texture& texture, VkDeviceSize offset) {
    std::array<VkBufferImageCopy, 16> copies{};
    for (uint32_t n = 0; n < texture.levels; ++n) {
      VkBufferImageCopy& copy_info = copies[n];
      copy_info.bufferOffset = offset + texture.level_offset[n];
      copy_info.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, n, 0, texture.layer_count};
      copy_info.imageExtent = {std::max(texture.img.width_px >> n, 1u), std::max(texture.img.height_px >> n, 1u),
                           texture.background ? texture.background : 1};
    }
    dfn_.vkCmdCopyBufferToImage(upload, upload_, texture.img.img, VK_IMAGE_LAYOUT_GENERAL,
                                texture.levels, copies.data());
  }

  // A texture's view, the same for RanuraVista and for the deferred views of RecogerEnlaces.
  VkResult CreateTextureView(VkImage img, VkFormat format_id, uint32_t swizzle, uint16_t swizzle_host, uint32_t heap,
                             VkImageView& view) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = img;
    info.viewType = heap == 2   ? VK_IMAGE_VIEW_TYPE_CUBE
                    : heap == 1 ? VK_IMAGE_VIEW_TYPE_3D
                                  : VK_IMAGE_VIEW_TYPE_2D;
    info.format = format_id;
    info.components = ComponentsMapping(swizzle, swizzle_host);
    info.subresourceRange = {IsDepth(format_id)
                                 ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                                 : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                             0, VK_REMAINING_MIP_LEVELS, 0, heap == 2 ? 6u : 1u};  // with its mips
    return dfn_.vkCreateImageView(device_, &info, nullptr, &view);
  }

  // The vistas_ key (image, heap and both swizzles), factored out for RecogerEnlaces.
  static uint64_t ViewKey(VkImage img, uint32_t swizzle, uint16_t swizzle_host, uint32_t heap) {
    return XXH3_64bits_withSeed(&img, sizeof(img),
                                (uint64_t(heap) << 40) | (uint64_t(swizzle) << 16) | swizzle_host);
  }

  // Heap slot (0 for 2D textures, 2 for cubemaps) for a view of the image with that swizzle.
  uint32_t ViewSlot(VkImage img, VkFormat format_id, uint32_t swizzle, uint16_t swizzle_host,
                       uint32_t heap = 0) {
    const uint64_t key = ViewKey(img, swizzle, swizzle_host, heap);  // The same key
    if (const auto it = views_.find(key); it != views_.end()) {
      return it->second.ranura;
    }
    // An image whose vkBindImageMemory is still on the bind thread. The view carries the image's GPU
    // address, which comes from the bind: the slot is reserved now, and the view is created and written in
    // RecogerEnlaces, before submission (UPDATE_AFTER_BIND descriptors).
    if (!images_in_flight_.empty() && images_in_flight_.count(img)) {
      return ViewSlotInFlight(key, img, format_id, swizzle, swizzle_host, heap);
    }
    View view;
    view.img = img;
    view.heap = heap;
    if (CreateTextureView(img, format_id, swizzle, swizzle_host, heap, view.view) != VK_SUCCESS) {
      Warn(34, "no se pudo crear la vista de una textura");
      return 0;
    }
    view.ranura = ReserveSlot(heap);
    if (!view.ranura) {
      dfn_.vkDestroyImageView(device_, view.view, nullptr);
      Warn(35, "monton de texturas lleno");
      return 0;
    }
    WriteImage(heap, view.ranura, view.view);
    views_.emplace(key, view);
    views_per_image_[img].push_back(key);
    return view.ranura;
  }

  // point: no filtering (depth textures).
  uint32_t RanuraSampler(const uint32_t* f, bool point = false) {
    // Diagnostic: filters the game requests that this renderer does not apply (anisotropy, mip bias and
    // maximum mip level; the bias is not even part of the key). Each new value is logged once.
    {
      const uint32_t aniso = (f[3] >> 25) & 0x7;
      const int32_t bias = int32_t(f[4] << 10) >> 22;  // lod_bias: 10 bits con signo, 5 fraccionarios
      const uint32_t mip_max = (f[4] >> 6) & 0xF;
      const uint32_t bias_index = uint32_t(bias + 512);
      if (!((seen_aniso_ >> aniso) & 1u) || !seen_biases_[bias_index] || !((seen_mip_max_ >> mip_max) & 1u)) {
        seen_aniso_ |= 1u << aniso;
        seen_biases_[bias_index] = true;
        seen_mip_max_ |= 1u << mip_max;
        REXLOG_INFO("[nativo] C4 filtros pedidos por el juego (valor nuevo): anisotropico {}, sesgo de mip {:.3f}, "
                    "nivel maximo de mip {}, filtros mag/min/mip {}/{}/{}{}",
                    aniso, float(bias) / 32.0f, mip_max, (f[3] >> 19) & 0x3, (f[3] >> 21) & 0x3,
                    (f[3] >> 23) & 0x3, point ? " (muestreo puntual)" : "");
      }
    }
    /*
     * Trilinear -> bilinear, for measurement only (scene pass analysis).
     *
     * The game requests mag/min/mip filters 1/1/1, i.e. mipmapMode LINEAR. On Maxwell (GM20B) a trilinear
     * sample takes the texture unit two cycles (two bilinear ones) and a bilinear sample only one: with the
     * filter between levels at NEAREST the TMU's work is halved without touching a single ALU instruction.
     * It is the only lever that separates the two:
     *   - if the scene drops by ~half of the TMU's share, sampling dominates;
     *   - if it does not move, the ALU dominates and the TMU has headroom.
     * A mip level jump is visible on receding surfaces (the PS2 "shimmer"). For measurement only. Off by
     * default: at 0 this is two bool comparisons per new sampler (samplers are cached) and not one more
     * instruction on the draw path.
     *
     * The modified mip field is part of the sampler key, so toggling at runtime creates two different
     * samplers and nothing has to be invalidated: the alternation works.
     */
    uint32_t filters_field = (f[3] >> 19) & 0xFFF;  // mag in 0-1, min in 2-3, mip in 4-5
    if (test_point_mip_ && ((filters_field >> 4) & 0x3) == 1) {
      filters_field &= ~(0x3u << 4);  // LINEAR -> NEAREST between mip levels
    }
    // With nfsmw_nativo_diag_mip_minimo the minimum level rises (without passing the maximum) and goes into
    // the key that way: each value has its own samplers, and going back to 0 reuses the usual ones.
    uint32_t mips_field = (f[4] >> 2) & 0xFF;  // minimum in 0-3, maximum in 4-7
    if (diag_mip_min_ != 0) {
      const uint32_t min_value = mips_field & 0xF;
      const uint32_t max_value = (mips_field >> 4) & 0xF;
      if (max_value > min_value) {
        mips_field = (mips_field & 0xF0) | std::min(max_value, min_value + diag_mip_min_);
      }
    }
    const uint32_t key = ((f[0] >> 10) & 0x1FF) | (filters_field << 9) |
                           (mips_field << 21) | ((f[5] & 0x3) << 29) |
                           (point ? 0x80000000u : 0u);
    if (const auto it = samplers_.find(key); it != samplers_.end()) {
      return it->second.second;
    }
    static constexpr VkSamplerAddressMode kModes[8] = {
        VK_SAMPLER_ADDRESS_MODE_REPEAT,          VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,   VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,   VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE};
    const bool mirror = vk_device_->properties().samplerMirrorClampToEdge;
    const auto mode = [&](uint32_t value) {
      const VkSamplerAddressMode m = kModes[value & 0x7];
      return m == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE && !mirror
                 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                 : m;
    };
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter =
        ((f[3] >> 19) & 0x3) == 1 && !point ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.minFilter =
        ((f[3] >> 21) & 0x3) == 1 && !point ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    const uint32_t mip = (filters_field >> 4) & 0x3;  // already includes the point mip test
    info.mipmapMode =
        mip == 1 && !point ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = mode(f[0] >> 10);
    info.addressModeV = mode(f[0] >> 13);
    info.addressModeW = mode(f[0] >> 16);
    info.minLod = float(mips_field & 0xF);  // with the mip diagnostic if it is enabled
    // With real mips, the fetch constant's maximum level also limits (like the emulation's sampler);
    // single-level textures stay as before.
    info.maxLod = mip == 2 ? info.minLod + 0.25f : std::max(info.minLod, float((f[4] >> 6) & 0xF));
    info.borderColor = (f[5] & 0x3) == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                         : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    // nfsmw_nativo_anisotropico. Only linear samplers with mips (maxLod above minLod); the value is read at
    // start-up, so it does not need to be in the key: every sampler is created with the same one.
    {
      static const uint32_t request_aniso = uint32_t(std::max(0, int32_t(REXCVAR_GET(nfsmw_nativo_anisotropico))));
      const auto& props = vk_device_->properties();
      if (request_aniso > 1 && props.samplerAnisotropy && info.minFilter == VK_FILTER_LINEAR &&
          info.mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR && info.maxLod > info.minLod) {
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = std::max(1.0f, std::min(float(request_aniso), props.maxSamplerAnisotropy));
        if (!tracked_aniso_) {
          tracked_aniso_ = true;
          REXLOG_INFO("[nativo] C4 filtrado anisotropico {} en los samplers lineales con mips (pedido {}, tope del "
                      "dispositivo {}); los de un solo nivel y los puntuales, sin cambios",
                      info.maxAnisotropy, request_aniso, props.maxSamplerAnisotropy);
        }
      }
    }
    VkSampler sampler;
    if (dfn_.vkCreateSampler(device_, &info, nullptr, &sampler) != VK_SUCCESS) {
      Warn(36, "no se pudo crear un sampler");
      return 0;
    }
    const uint32_t ranura = ReserveSlot(3);
    if (!ranura) {
      dfn_.vkDestroySampler(device_, sampler, nullptr);
      Warn(37, "monton de samplers lleno");
      return 0;
    }
    WriteSampler(ranura, sampler);
    samplers_.emplace(key, std::make_pair(sampler, ranura));
    return ranura;
  }

  // Category clipped to 1x1 (0 = none), with its alternating test.
  uint32_t TestScissor() {
    const int32_t category = REXCVAR_GET(nfsmw_nativo_prueba_tijera);
    if (category <= 0) {
      return 0;
    }
    const int32_t alternar = REXCVAR_GET(nfsmw_nativo_prueba_tijera_alternar_s);
    if (alternar <= 0) {
      return uint32_t(category);
    }
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - ps_toggle_start_)
                              .count();
    const bool with_scissor = (secs / alternar) % 2 == 1;
    if (with_scissor != tracked_scissor_toggle_) {
      tracked_scissor_toggle_ = with_scissor;
      REXLOG_INFO("[nativo] C2 tijera de prueba: {}", with_scissor ? "recortando a 1x1" : "normal");
    }
    return with_scissor ? uint32_t(category) : 0;
  }

  /*
   * The ring's verdict for vegetation filtered in the game (nfsmw_d3d_vegetacion_juego). If the draw
   * carries the game's verdict (the kVeg* flags of its record), the ring's own is computed from this
   * draw's registers and PS: the criterion of Dibujar's early discard, minus the earlier exits that have
   * no side effects (primitive type, vertex input, diagnostics) and do not draw either. It goes to the
   * guard (AnotarVegetacionAnillo) and, if Dibujar reaches its early discard, it must have decided the
   * same (AnotarVegetacionModelo otherwise).
   */
  void VegetationVerdict(const DrawRequest& p) {
    vegetation_computed_ = false;
    if (!(p.vegetation_game & kVegHay)) {
      return;
    }
    const uint32_t* r = p.regs;
    VegetationDetail a;
    if (r) {
      a.mode = r[gr::XE_GPU_REG_RB_MODECONTROL];
      a.mask = r[gr::XE_GPU_REG_RB_COLOR_MASK];
      a.control = r[gr::XE_GPU_REG_RB_COLORCONTROL];
    }
    a.vs = p.vs ? int32_t(p.vs->number) : -1;
    a.ps = p.ps ? int32_t(p.ps->number) : -1;
    if (p.ps) {
      a.outputs = p.ps->outputs;
      a.discards = p.ps->discards;
    }
    if (r && p.vs && p.ps && (a.mode & 0x7) == uint32_t(xenos::EdramMode::kColorDepth)) {
      bool color = false;
      for (uint32_t i = 0; i < 4; ++i) {
        color = color || (((a.mask >> (i * 4)) & 0xF) && ((p.ps->outputs >> i) & 0x1));
      }
      const bool alpha_test = ((a.control >> 3) & 0x1) && (a.control & 0x7) != 7;
      a.structure = !color && (alpha_test || p.ps->discards || (p.ps->outputs & 0x10));
    }
    a.settings = (cvars_per_frame_ ? frame_alpha_only_ps_ : PsAlphaOnly()) && without_vegetation_;
    a.oclusion = open_occlusion_;
    vegetation_ring_ = a.structure && a.settings && !a.oclusion;
    vegetation_computed_ = true;
    vegetation_flags_ = p.vegetation_game;
    vegetation_detail_ = a;
    TrackRingVegetation(p.vegetation_game, a);
  }

  // The setting, or its alternating test.
  bool PsAlphaOnly() {
    const int32_t alternar = REXCVAR_GET(nfsmw_nativo_ps_solo_alfa_alternar_s);
    if (alternar <= 0) {
      return REXCVAR_GET(nfsmw_nativo_ps_solo_alfa);
    }
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - ps_toggle_start_)
                              .count();
    const bool only_alpha = (secs / alternar) % 2 == 1;
    if (only_alpha != tracked_alpha_only_toggle_) {
      tracked_alpha_only_toggle_ = only_alpha;
      REXLOG_INFO("[nativo] C2 escrituras de color: {}",
                  only_alpha ? "quitadas en las pasadas sin color" : "todas");
    }
    return only_alpha;
  }

  // The setting, or its alternating test.
  bool NoPsNoColor() {
    const int32_t alternar = REXCVAR_GET(nfsmw_nativo_sin_ps_sin_color_alternar_s);
    if (alternar <= 0) {
      return REXCVAR_GET(nfsmw_nativo_sin_ps_sin_color);
    }
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - ps_toggle_start_)
                              .count();
    const bool without_ps = (secs / alternar) % 2 == 1;
    if (without_ps != tracked_ps_toggle_) {
      tracked_ps_toggle_ = without_ps;
      REXLOG_INFO("[nativo] C2 etapa de fragmentos: {}",
                  without_ps ? "quitada en los dibujos sin color" : "siempre montada");
    }
    return without_ps;
  }

  // 1/size of a slot's host image, in the shared constants. With size 0 (a texture that could not be
  // prepared) 0 is written: the shader does not use it because it does not sample either.
  // AirborneNX: value scale of a slot (see kPalabraEscala).
  static void WriteScale(uint32_t* shared, uint32_t reg_index, float scale) {
    std::memcpy(shared + kScaleWord + reg_index, &scale, sizeof(scale));
  }

  static void WriteInvSize(uint32_t* shared, uint32_t reg_index, uint32_t width_px, uint32_t height_px) {
    const float inv[2] = {width_px ? 1.0f / float(width_px) : 0.0f, height_px ? 1.0f / float(height_px) : 0.0f};
    std::memcpy(shared + kSizeInvWord + reg_index * 2, inv, sizeof(inv));
  }

  bool StartPass(const uint32_t* r, const uint64_t keys[5], uint32_t pitch,
                   uint64_t pass_key) {
    pass_start_ = time_ ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
    if (!PrepareEmpty()) {
      return false;
    }
    std::array<NativeImage*, 5> images{};
    for (uint32_t i = 0; i < 4; ++i) {
      if (keys[i]) {
        images[i] = contexto_->ColorTarget(uint32_t(keys[i] >> 24) & 0xFFF,
                                              uint32_t(keys[i] >> 16) & 0xF, pitch);
        if (!images[i]) {
          return Reject(40, "no hay destino de color");
        }
      }
    }
    if (keys[4]) {
      images[4] = contexto_->DepthTarget(uint32_t(keys[4] >> 24) & 0xFFF,
                                                  uint32_t(keys[4] >> 16) & 0x1, pitch);
      if (!images[4]) {
        return Reject(41, "no hay destino de profundidad");
      }
    }
    (void)r;
    // The render targets are already resolved above; that segment is closed.
    const auto after_targets = time_ ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
    if (time_) {
      stages_ns_[9] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          after_targets - pass_start_).count());
    }
    uint32_t width_px = UINT32_MAX, height_px = UINT32_MAX;
    std::array<VkImageView, 5> views{};
    uint32_t formats[5] = {};
    for (uint32_t i = 0; i < 5; ++i) {
      if (images[i]) {
        width_px = std::min(width_px, images[i]->width_px);
        height_px = std::min(height_px, images[i]->height_px);
        views[i] = images[i]->view;
        formats[i] = uint32_t(images[i]->format_id);
      }
    }
    // Shadow map: a depth-only target with a pitch of 1600 or more (same as omitir_sombras).
    const bool is_shadows = pitch >= 1600 && formats[4] && !formats[0] && !formats[1] &&
                            !formats[2] && !formats[3];
    // nfsmw_nativo_sombra_minimo. If C2 says this is the car pass of the shadow map, each draw is validated
    // for it (and the pass must be depth-only).
    shadow_cars_pass_ = images[4] != nullptr && contexto_->CarShadowPass(images[4], is_shadows);
    // GPU time per pass type (C2 report), by render target width. Shadows are recognized by being
    // depth-only, not by measuring 1600 or more. With the scene at 1920 (nfsmw_1080p_prueba) the width no
    // longer tells them apart: the scene fell into the shadow bucket and vanished from its own category.
    pass_category_ = TargetCategory(pitch, keys);
    contexto_->MarkGpu(pass_category_);
    const bool shadows_without_load =
        is_shadows && REXCVAR_GET(nfsmw_nativo_pase_sombras_sin_load);
    const VkRenderPass pass = PassOf(formats, shadows_without_load ? kIgnoreLoad : kReadLoad);
    if (pass == VK_NULL_HANDLE) {
      return Reject(42, "no se pudo crear el render pass");
    }
    const VkFramebuffer framebuffer = FramebufferDe(pass, views, width_px, height_px);
    if (framebuffer == VK_NULL_HANDLE) {
      return Reject(43, "no se pudo crear el framebuffer");
    }
    if (time_) {
      stages_ns_[10] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - after_targets).count());
    }
    const auto before_open = time_ ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
    const VkCommandBuffer cmd = contexto_->JobCommands();
    if (!cmd) {
      return false;
    }
    VkRenderPassBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    start.renderPass = pass;
    start.framebuffer = framebuffer;
    // Only the rectangle the game uses (see nfsmw_nativo_pase_area_util).
    uint32_t pass_height = height_px;
    if (usable_area_) {
      const auto it = usable_height_.find(pitch);
      if (it != usable_height_.end() && it->second < height_px) {
        pass_height = it->second;
      }
    }
    start.renderArea.extent = {width_px, pass_height};
    // nfsmw_nativo_diag_borrados. What this pass loads and stores of each render target, to compare with
    // what is cleared of it (C2 clears per target). Before opening the pass.
    for (const NativeImage* img : images) {
      if (img) {
        contexto_->TrackPassArea(img, width_px, pass_height);
      }
    }
    // The statistics cover the whole pass, from here to after EndRenderPass.
    pass_stats_ = contexto_->StartStats(pass_category_);
    const auto before_start = std::chrono::steady_clock::now();
    dfn_.vkCmdBeginRenderPass(cmd, &start, VK_SUBPASS_CONTENTS_INLINE);
    ns_render_pass_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - before_start).count());
    if (time_) {
      stages_ns_[11] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - before_open).count());
    }
    active_pass_ = true;
    ++started_passes_;
    passes_texels_ += uint64_t(width_px) * pass_height;  // how much tile is loaded by loadOp = LOAD
    {
      // Same criterion as the per-category GPU time: the render target's pitch decides.
      const uint32_t category = TargetCategory(pitch, keys);
      texels_per_category_[category] += uint64_t(width_px) * pass_height;
      ++passes_per_category_[category];
    }
    commands_pass_ = cmd;
    draws_in_pass_ = 0;  // position within the pass (deferred sky report)
    key_pass_ = pass_key;
    std::memcpy(keys_pass_, keys, sizeof(keys_pass_));  // the bytes that produce pase_clave_
    valid_keys_pass_ = true;
    ++series_pass_;  // the cached framing depends on the pass size and scale
    generation_pass_ = contexto_->CommandsGeneration();
    pass_width_ = width_px;
    pass_height_ = height_px;
    // Only the shadow map is drawn at a size other than the one the guest thinks.
    scale_pass_ = (is_shadows && pitch && width_px && width_px != pitch)
                       ? float(width_px) / float(pitch)
                       : 1.0f;
    // AirborneNX: 4X MSAA surfaces are drawn in 1X terms, twice as large (see Draw).
    if (edram_4x_alias_ &&
        ((r[gr::XE_GPU_REG_RB_SURFACE_INFO] >> 16) & 0x3) == uint32_t(xenos::MsaaSamples::k4X)) {
      scale_pass_ *= 2.0f;
    }
    std::copy(std::begin(formats), std::end(formats), std::begin(formats_pass_));
    rp_pass_ = pass;
    // Phase 0b (nfsmw_nativo_pipeline_entre_pases). Vulkan keeps the bound pipeline across passes of the
    // same buffer: the pass's first draw only binds again if its key differs. A new buffer still starts
    // with nothing bound (Dibujar, grabacion_generacion_).
    if (!pipeline_between_passes_) {
      bound_pipeline_ = VK_NULL_HANDLE;
    } else if (bound_pipeline_ != VK_NULL_HANDLE) {
      ++passes_with_pipeline_;
    }
    recorded_state_ = false;
    if (bound_pipeline_ == VK_NULL_HANDLE) {
      eds_valid_ = false;  // pass that forgets the pipeline (without 0b): set everything again
    }
    return true;
  }

  // ZCULL: three load modes. kCargaBorrar is needed to clear the depth of images created without
  // TRANSFER_DST, the only ones eligible for a ZCULL plane.
  enum : uint32_t { kReadLoad = 0, kIgnoreLoad = 1, kClearLoad = 2 };

  VkRenderPass PassOf(const uint32_t formats[5], uint32_t load_mode = kReadLoad) {
    // The mode is part of the key: two render passes with the same formats but a different loadOp are
    // different.
    const uint64_t key =
        XXH3_64bits_withSeed(formats, sizeof(uint32_t) * 5, load_mode);
    if (const auto it = passes_.find(key); it != passes_.end()) {
      return it->second;
    }
    const VkRenderPass pass = CreatePass(formats, load_mode);  // the usual code, factored out
    if (pass == VK_NULL_HANDLE) {
      return VK_NULL_HANDLE;
    }
    passes_.emplace(key, pass);
    return pass;
  }

  // The plain loop for 16-bit indices (byte-swapped or not), untouched.
  static void ScalarIndices16(const uint8_t* payload, uint32_t num, uint16_t* output, bool rotate, uint32_t& min_value,
                                 uint32_t& max_value) {
    min_value = 0xFFFF;
    max_value = 0;
    if (rotate) {
      for (uint32_t i = 0; i < num; ++i) {
        uint16_t raw;
        std::memcpy(&raw, payload + size_t(i) * 2, 2);
        const uint16_t v = std::byteswap(raw);
        output[i] = v;
        min_value = std::min<uint32_t>(min_value, v);
        max_value = std::max<uint32_t>(max_value, v);
      }
    } else {
      for (uint32_t i = 0; i < num; ++i) {
        uint16_t v;
        std::memcpy(&v, payload + size_t(i) * 2, 2);
        output[i] = v;
        min_value = std::min<uint32_t>(min_value, v);
        max_value = std::max<uint32_t>(max_value, v);
      }
    }
  }

  // The same loop with NEON, 16 indices per iteration (vrev16 swaps the bytes of each 16-bit index, the
  // same as std::byteswap), and the remainder with the plain loop. NEON loads and stores need no
  // alignment on AArch64.
  static void Indices16Neon(const uint8_t* payload, uint32_t num, uint16_t* output, bool rotate, uint32_t& min_value,
                              uint32_t& max_value) {
#if defined(__aarch64__)
    uint32_t i = 0;
    uint32_t mn = 0xFFFF;
    uint32_t mx = 0;
    if (num >= 16) {
      uint16x8_t min_a = vdupq_n_u16(0xFFFF);
      uint16x8_t min_b = min_a;
      uint16x8_t max_a = vdupq_n_u16(0);
      uint16x8_t max_b = max_a;
      // Two loops (swapping is fixed per draw): no decision inside the iteration.
      if (rotate) {
        for (; i + 16 <= num; i += 16) {
          const uint16x8_t a = vreinterpretq_u16_u8(vrev16q_u8(vld1q_u8(payload + size_t(i) * 2)));
          const uint16x8_t b = vreinterpretq_u16_u8(vrev16q_u8(vld1q_u8(payload + size_t(i) * 2 + 16)));
          vst1q_u16(output + i, a);
          vst1q_u16(output + i + 8, b);
          min_a = vminq_u16(min_a, a);
          min_b = vminq_u16(min_b, b);
          max_a = vmaxq_u16(max_a, a);
          max_b = vmaxq_u16(max_b, b);
        }
      } else {
        for (; i + 16 <= num; i += 16) {
          const uint16x8_t a = vreinterpretq_u16_u8(vld1q_u8(payload + size_t(i) * 2));
          const uint16x8_t b = vreinterpretq_u16_u8(vld1q_u8(payload + size_t(i) * 2 + 16));
          vst1q_u16(output + i, a);
          vst1q_u16(output + i + 8, b);
          min_a = vminq_u16(min_a, a);
          min_b = vminq_u16(min_b, b);
          max_a = vmaxq_u16(max_a, a);
          max_b = vmaxq_u16(max_b, b);
        }
      }
      mn = vminvq_u16(vminq_u16(min_a, min_b));
      mx = vmaxvq_u16(vmaxq_u16(max_a, max_b));
    }
    for (; i < num; ++i) {
      uint16_t v;
      std::memcpy(&v, payload + size_t(i) * 2, 2);
      if (rotate) {
        v = std::byteswap(v);
      }
      output[i] = v;
      mn = std::min<uint32_t>(mn, v);
      mx = std::max<uint32_t>(mx, v);
    }
    min_value = mn;
    max_value = mx;
#else
    ScalarIndices16(payload, num, output, rotate, min_value, max_value);
#endif
  }

  // The 16-bit indices of a draw (see nfsmw_nativo_indices_neon). Ring only.
  void Indices16(const uint8_t* payload, uint32_t num, uint16_t* output, bool rotate, uint32_t& min_value,
                   uint32_t& max_value) {
    if (indices_neon_ < 0) {
      indices_neon_ = REXCVAR_GET(nfsmw_nativo_indices_neon) ? 1 : 0;
      REXLOG_INFO("[nativo] C6 indices de 16 bits (build 187): {}",
                  indices_neon_ ? "con NEON; se comprueban los 20.000 primeros dibujos y despues 1 de cada 4.096"
                                : "con el bucle de siempre (nfsmw_nativo_indices_neon = false)");
    }
    if (indices_neon_ == 0) {
      ScalarIndices16(payload, num, output, rotate, min_value, max_value);
      return;
    }
    Indices16Neon(payload, num, output, rotate, min_value, max_value);
    const uint64_t n = ++draws_indices_neon_;
    if (n > 20000 && (n & 4095) != 0) {
      return;
    }
    test_indices_neon_.resize(num);
    uint32_t mn = 0;
    uint32_t mx = 0;
    ScalarIndices16(payload, num, test_indices_neon_.data(), rotate, mn, mx);
    ++checked_indices_neon_;
    if (mn != min_value || mx != max_value ||
        (num && std::memcmp(test_indices_neon_.data(), output, size_t(num) * 2) != 0)) {
      // The plain path stays for this draw and for the rest of the session.
      if (num) {
        std::memcpy(output, test_indices_neon_.data(), size_t(num) * 2);
      }
      min_value = mn;
      max_value = mx;
      indices_neon_ = 0;
      REXLOG_ERROR("[nativo] C6 indices de 16 bits: DIFERENCIA entre NEON y el bucle de siempre ({} indices, {}; minimo "
                   "{} frente a {}, maximo {} frente a {}). Apagado para el resto de la sesion: el bucle de siempre",
                   num, rotate ? "girados" : "sin girar", min_value, mn, max_value, mx);
      return;
    }
    if (checked_indices_neon_ == 20000) {
      REXLOG_INFO("[nativo] C6 indices de 16 bits (build 187): 20.000 dibujos comprobados contra el bucle de siempre, 0 "
                  "diferencias; sigue comprobando 1 de cada 4.096");
    }
  }

  // The PaseDe render pass without its cache, with nothing changed. The pipeline prewarm also uses it
  // from its thread (a compatible one: the same formats). It only reads its arguments.
  VkRenderPass CreatePass(const uint32_t formats[5], uint32_t load_mode) const {
    std::array<VkAttachmentDescription, 5> attachments{};
    std::array<VkAttachmentReference, 4> colores{};
    VkAttachmentReference depth_value{};
    uint32_t n = 0, n_colores = 0;
    for (uint32_t i = 0; i < 5; ++i) {
      if (!formats[i]) {
        continue;
      }
      VkAttachmentDescription& a = attachments[n];
      a.format = VkFormat(formats[i]);
      a.samples = VK_SAMPLE_COUNT_1_BIT;
      // With the test active, the shadow map does not load its previous content: the game clears it before
      // drawing it, so fetching the 1600x1600 tile only to throw it away is wasted work.
      a.loadOp = load_mode == kIgnoreLoad ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                 : load_mode == kClearLoad ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                              : VK_ATTACHMENT_LOAD_OP_LOAD;
      a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      a.stencilLoadOp = i != 4                       ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                        : load_mode == kClearLoad ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                     : VK_ATTACHMENT_LOAD_OP_LOAD;
      a.stencilStoreOp = i == 4 ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
      a.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
      a.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
      if (i < 4) {
        colores[n_colores++] = {n, VK_IMAGE_LAYOUT_GENERAL};
      } else {
        depth_value = {n, VK_IMAGE_LAYOUT_GENERAL};
      }
      ++n;
    }
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = n_colores;
    subpass.pColorAttachments = colores.data();
    subpass.pDepthStencilAttachment = formats[4] ? &depth_value : nullptr;
    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = n;
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    VkRenderPass pass;
    if (dfn_.vkCreateRenderPass(device_, &info, nullptr, &pass) != VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    return pass;
  }

  VkFramebuffer FramebufferDe(VkRenderPass pass, const std::array<VkImageView, 5>& views,
                              uint32_t width_px, uint32_t height_px) {
    struct Key {
      VkRenderPass pass;
      std::array<VkImageView, 5> views;
      uint32_t width_px, height_px;
    } key{pass, views, width_px, height_px};
    const uint64_t fingerprint = XXH3_64bits(&key, sizeof(key));
    if (const auto it = framebuffers_.find(fingerprint); it != framebuffers_.end()) {
      return it->second;
    }
    std::array<VkImageView, 5> attachments{};
    uint32_t n = 0;
    for (VkImageView view : views) {
      if (view != VK_NULL_HANDLE) {
        attachments[n++] = view;
      }
    }
    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = pass;
    info.attachmentCount = n;
    info.pAttachments = attachments.data();
    info.width = width_px;
    info.height = height_px;
    info.layers = 1;
    VkFramebuffer framebuffer;
    if (dfn_.vkCreateFramebuffer(device_, &info, nullptr, &framebuffer) != VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    framebuffers_.emplace(fingerprint, framebuffer);
    views_framebuffers_[fingerprint] = views;  // For OlvidarVista
    return framebuffer;
  }

  // The bright pass variants of nfsmw_resplandor_cielo (1 natural, 2 soft), compiled with the same DXC
  // options as the library.
  VkShaderModule VariantModule(size_t index, const uint32_t* spirv, size_t bytes) {
    if (!created_variants_modules_[index]) {
      created_variants_modules_[index] = true;
      VkShaderModuleCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      info.codeSize = bytes;
      info.pCode = spirv;
      if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &variants_modules_[index]) != VK_SUCCESS) {
        variants_modules_[index] = VK_NULL_HANDLE;
      }
    }
    return variants_modules_[index];
  }

  // nfsmw_resplandor_cielo (F4 menu, Graficos: 0 original, 1 natural, 2 soft), or its test rotation; logs
  // every change.
  int GlowSky() {
    int mode = nfsmw::settings::GlowSky();
    const int32_t alternar = REXCVAR_GET(nfsmw_nativo_resplandor_suave_alternar_s);
    if (alternar > 0) {
      const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now() - shadows_start_)
                                .count();
      mode = int((secs / alternar) % 3);
    }
    mode = std::clamp(mode, 0, 2);
    if (mode != glow_tracked_) {
      glow_tracked_ = mode;
      static constexpr const char* kNames[3] = {"original", "natural", "suave"};
      REXLOG_INFO("[nativo] resplandor del cielo: {} (fotograma {})", kNames[mode], frame_);
    }
    return mode;
  }

  /*
   * The early Z and invisible draws report, every 10 s like the others.
   *
   * What matters is not what is saved but the second figure: "NO se pueden porque escriben profundidad"
   * is, draw by draw, the exact size of what only a depth pre-pass could address, and that is expensive
   * to implement. A small figure means a pre-pass is not worth the risk.
   */
  void EarlyZReport() {
    ++frames_z_;
    const auto now_time = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now_time - last_report_z_).count() < 10) {
      return;
    }
    last_report_z_ = now_time;
    const uint64_t seen_items = frames_z_ - frames_z_previous_;
    const double frames = double(seen_items ? seen_items : 1);
    std::array<uint64_t, kZCounts> d{};
    uint64_t total = 0;
    for (uint32_t i = 0; i < kZCounts; ++i) {
      d[i] = counts_z_[i] - counts_z_previous_[i];
      counts_z_previous_[i] = counts_z_[i];
      total += d[i];
    }
    frames_z_previous_ = frames_z_;
    SkyReport(frames);  // goes before the !total cutoff, which has nothing to do with the sky
    if (!total) {
      return;
    }
    const uint64_t late_items = d[kZSet] + d[kZWriteZ] + d[kZStencil] + d[kZOclusion];
    NFSMW_RING_REPORT(
        "[nativo] C6 Z temprana por fotograma: {:.0f} dibujos la aprovechan, {:.0f} NO se pueden porque "
        "escriben profundidad (esos son el tamano de la pasada previa de C88), {:.0f} por estencil, {:.0f} "
        "dentro de una consulta de oclusion; {:.0f} ya probaban antes. De los que obligaban a sombrear "
        "primero se arregla el {:.0f} %{}",
        double(d[kZSet]) / frames, double(d[kZWriteZ]) / frames,
        double(d[kZStencil]) / frames, double(d[kZOclusion]) / frames,
        double(d[kZEarlyAlready]) / frames,
        late_items ? 100.0 * double(d[kZSet]) / double(late_items) : 0.0,
        z_early_without_module_ ? fmt::format(" ({} shaders sin modulo parcheado)", z_early_without_module_)
                               : std::string());
    if (d[kBlendInvisible] || d[kInvisibleAlfa] || d[kColorOnlyInvisible]) {
      NFSMW_RING_REPORT(
          "[nativo] C6 dibujos invisibles por fotograma: {:.1f} con la mezcla que copia el destino, {:.1f} "
          "con la prueba de alfa en NUNCA; {:.1f} se quedan solo con la profundidad",
          double(d[kBlendInvisible]) / frames, double(d[kInvisibleAlfa]) / frames,
          double(d[kColorOnlyInvisible]) / frames);
    }
  }

  /*
   * The deferred sky report. Without it there is no way to measure whether it works.
   *
   * What to check: "aplazados" must be ~0.9 per frame in a race (the sky is a single draw and is
   * sometimes absent). "perdidos" must be 0: if not, some path closes the pass without going through
   * TerminarPase and the sky is being lost. And "posicion" says how many draws of the pass were recorded
   * between the old place and the new one: exactly the amount of geometry that now covers the sky before
   * it is shaded. If it is small, so is the saving.
   */
  void SkyReport(double frames) {
    const uint64_t deferred_items = sky_deferred_count_ - sky_deferred_previous_;
    const uint64_t seen_items = sky_seen_ - sky_seen_previous_;
    if (!seen_items) {
      return;
    }
    std::array<uint64_t, kSkyReasons> per_reason{};
    uint64_t emitidos = 0;
    for (uint32_t i = 0; i < kSkyReasons; ++i) {
      per_reason[i] = sky_emitted_[i] - sky_emitted_previous_[i];
      sky_emitted_previous_[i] = sky_emitted_[i];
      emitidos += per_reason[i];
    }
    const uint64_t sum_position = sky_sum_position_ - sky_sum_position_previous_;
    const uint64_t candidates = sky_candidates_ - sky_candidates_previous_;
    sky_candidates_previous_ = sky_candidates_;
    sky_seen_previous_ = sky_seen_;
    sky_deferred_previous_ = sky_deferred_count_;
    sky_sum_position_previous_ = sky_sum_position_;
    /*
     * The line that decides whether the criterion works. "con la geometria del domo" is the number that
     * matters: it must be 1.00 per frame. The "detectados" are by fingerprint only and once gave 7.00,
     * which is what broke the image.
     */
    NFSMW_RING_REPORT("[nativo] C6 cielo: {:.2f} detectados por huella y {:.2f} con la geometria del domo "
                "(480 indices y primeros del pase) por fotograma. El segundo tiene que dar 1,00. "
                "GUARDIA: {} ({} fotogramas con domo mirados, {} con exactamente uno, maximo {} en "
                "un fotograma)",
                double(seen_items) / frames, double(candidates) / frames,
                sky_guard_ == kSkyDeferring  ? "SUPERADA, aplazando"
                : sky_guard_ == kSkyDiscarded ? "DESCARTADA, no se aplaza nada"
                                                     : "mirando todavia",
                sky_frames_guard_, sky_guard_with_one_, sky_guard_max_);
    NFSMW_RING_REPORT(
        "[nativo] C6 cielo aplazado ({}): {:.2f} detectados por fotograma, {:.2f} aplazados de verdad "
        "({} no aplazables en total, {} con otro cielo ya pendiente); emitidos {} al llegar un dibujo "
        "con mezcla, {} al llegar un opaco que no escribe Z, {} al cerrarse el pase, {} por otro cielo; "
        "PERDIDOS {}; posicion media {:.0f} dibujos por delante de el (maximo {})",
        sky_deferred_ ? "encendido" : "APAGADO", double(seen_items) / frames,
        double(deferred_items) / frames, sky_no_deferrable_, sky_two_in_pass_,
        per_reason[kSkyPerBlend], per_reason[kSkyPerWithoutZ], per_reason[kSkyPerPassEnd],
        per_reason[kSkyPerOtherSky], sky_lost_,
        emitidos ? double(sum_position) / double(emitidos) : 0.0, sky_position_max_);
  }

  /*
   * Closes the frame for the sky guard. See the block of fields.
   *
   * Runs once per frame and does nothing once the guard has decided, so its cost is one comparison. The
   * decision is taken only once per session and logged.
   */
  void CloseSkyGuardFrame() {
    const uint32_t en_este = sky_guard_in_frame_;
    sky_guard_in_frame_ = 0;
    if (sky_guard_ != kSkyPeeking) {
      return;
    }
    /*
     * A frame without a sky says nothing about the criterion: menus, the logo and loading screens, which
     * come before any race. If they counted, the test would run out in the menu and the guard would switch
     * off for good without ever seeing the dome. Only frames in which the dome appears count.
     */
    if (en_este == 0) {
      return;
    }
    sky_guard_max_ = std::max(sky_guard_max_, en_este);
    if (en_este == 1) {
      ++sky_guard_with_one_;
    }
    if (++sky_frames_guard_ < kSkyTestFrames) {
      return;
    }
    /*
     * The test is over. It only switches on if it came out clean: never more than one, and enough frames
     * with exactly one. In any other case it stays off and the image is identical to the non-deferred one,
     * which is known to work.
     */
    if (sky_guard_max_ == 1 && sky_guard_with_one_ >= kSkyFramesWithOne) {
      sky_guard_ = kSkyDeferring;
      REXLOG_INFO("[nativo] C6 cielo: guardia SUPERADA ({} de {} fotogramas con exactamente un domo, "
                  "nunca dos). Se aplaza el cielo a partir de ahora: son 3,4-4,3 ms de GPU sin "
                  "cambiar un pixel",
                  sky_guard_with_one_, sky_frames_guard_);
    } else {
      sky_guard_ = kSkyDiscarded;
      REXLOG_WARN("[nativo] C6 cielo: guardia NO superada (maximo {} domos en un fotograma, {} de {} "
                  "fotogramas con exactamente uno; hacian falta {}). NO se aplaza nada: la imagen "
                  "queda exactamente como estaba",
                  sky_guard_max_, sky_guard_with_one_, sky_frames_guard_,
                  kSkyFramesWithOne);
    }
  }

  VkShaderModule ModuleOf(const ShaderEntry& entry) {
    if (const auto it = modules_.find(&entry); it != modules_.end()) {
      return it->second;
    }
    const auto& spirv = entry.shader->spirv;
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = spirv.size() * sizeof(uint32_t);
    info.pCode = spirv.data();
    VkShaderModule spv_module = VK_NULL_HANDLE;
    if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &spv_module) != VK_SUCCESS) {
      spv_module = VK_NULL_HANDLE;
    }
    modules_.emplace(&entry, spv_module);
    return spv_module;
  }

  // Pixel shader module without the color writes, one per shader. If it cannot be pruned, the normal one
  // is returned: the image is the same, only the saving is lost.
  VkShaderModule AlphaOnlyModule(const ShaderEntry& entry) {
    if (const auto it = alpha_only_modules_.find(&entry); it != alpha_only_modules_.end()) {
      return it->second != VK_NULL_HANDLE ? it->second : ModuleOf(entry);
    }
    uint32_t removed = 0;
    const std::vector<uint32_t> pruned = PruneColorWrites(entry.shader->spirv, removed);
    VkShaderModule spv_module = VK_NULL_HANDLE;
    if (!pruned.empty()) {
      VkShaderModuleCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      info.codeSize = pruned.size() * sizeof(uint32_t);
      info.pCode = pruned.data();
      if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &spv_module) != VK_SUCCESS) {
        spv_module = VK_NULL_HANDLE;
      }
    }
    REXLOG_INFO("[nativo] C5a: PS n{} sin escrituras de color: {} quitadas, {} palabras de {} ({})",
                entry.number, removed, pruned.size(), entry.shader->spirv.size(),
                spv_module != VK_NULL_HANDLE ? "modulo creado" : "se usa el normal");
    alpha_only_modules_.emplace(&entry, spv_module);
    return spv_module != VK_NULL_HANDLE ? spv_module : ModuleOf(entry);
  }

  /*
   * Pixel shader module with EarlyFragmentTests declared, one per shader. If that is not possible (it
   * already had it, it writes gl_FragDepth...), the normal one is returned: the image is the same and only
   * the saving is lost. It is logged once per shader so the log shows which ones qualified.
   */
  VkShaderModule EarlyModuleZ(const ShaderEntry& entry) {
    if (const auto it = early_modules_z_.find(&entry); it != early_modules_z_.end()) {
      return it->second != VK_NULL_HANDLE ? it->second : ModuleOf(entry);
    }
    const char* reason = "";
    const std::vector<uint32_t> patched = WithEarlyTests(entry.shader->spirv, reason);
    VkShaderModule spv_module = VK_NULL_HANDLE;
    if (!patched.empty()) {
      VkShaderModuleCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      info.codeSize = patched.size() * sizeof(uint32_t);
      info.pCode = patched.data();
      if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &spv_module) != VK_SUCCESS) {
        spv_module = VK_NULL_HANDLE;
      }
    }
    REXLOG_INFO("[nativo] C6 Z temprana: PS n{} {} ({} kills, {} palabras)", entry.number,
                spv_module != VK_NULL_HANDLE
                    ? std::string("prueba la profundidad antes de sombrear")
                    : fmt::format("se queda como estaba: {}", reason),
                entry.kills, entry.shader->spirv.size());
    early_modules_z_.emplace(&entry, spv_module);
    if (spv_module == VK_NULL_HANDLE) {
      ++z_early_without_module_;
    }
    return spv_module != VK_NULL_HANDLE ? spv_module : ModuleOf(entry);
  }

  /*
   * The fixed state of a pipeline, exactly as PipelineDe sets it. It is its usual code, moved here without
   * changing any computation: only its eight declarations become references into EstadoFijoPipeline. Used
   * by PipelineDe and by the guards of the canonical key (phase 0a) and of dynamic state (phases 1 and 2),
   * which thus compare against what the pipeline really carries and not against another copy of the same
   * computations. mezcla.pAttachments points to fijo.mezclas: the structure is not copied.
   */
  struct PipelineFixedState {
    VkPipelineInputAssemblyStateCreateInfo ensamblado{};
    VkPipelineViewportStateCreateInfo view{};
    VkPipelineRasterizationStateCreateInfo rasterizado{};
    VkPipelineMultisampleStateCreateInfo sampling{};
    VkPipelineDepthStencilStateCreateInfo depth_value{};
    std::array<VkPipelineColorBlendAttachmentState, 4> blends{};
    uint32_t n_colores = 0;
    VkPipelineColorBlendStateCreateInfo blend{};
    PipelineFixedState() = default;
    PipelineFixedState(const PipelineFixedState&) = delete;
    PipelineFixedState& operator=(const PipelineFixedState&) = delete;
  };

  // avisar = false from the prewarm thread (Avisar belongs to the ring only).
  void FillFixedState(const PipelineKey& key, PipelineFixedState& fixed, bool warn = true) {
    VkPipelineInputAssemblyStateCreateInfo& ensamblado = fixed.ensamblado;
    ensamblado.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ensamblado.topology = VkPrimitiveTopology(key.topo);
    ensamblado.primitiveRestartEnable = (key.rasterizado & 0x8) ? VK_TRUE : VK_FALSE;

    VkPipelineViewportStateCreateInfo& view = fixed.view;
    view.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    view.viewportCount = 1;
    view.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo& rasterizado = fixed.rasterizado;
    rasterizado.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizado.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizado.cullMode = ((key.rasterizado & 0x1) ? VK_CULL_MODE_FRONT_BIT : 0) |
                           ((key.rasterizado & 0x2) ? VK_CULL_MODE_BACK_BIT : 0);
    // PA_SU_SC_MODE_CNTL.face: 1 = the front face is clockwise (not confirmed on screen).
    rasterizado.frontFace =
        (key.rasterizado & 0x4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizado.lineWidth = 1.0f;
    rasterizado.depthBiasEnable = (key.rasterizado & 0x10) ? VK_TRUE : VK_FALSE;
    rasterizado.depthClampEnable = REXCVAR_GET(nfsmw_nativo_prueba_depth_clamp) ? VK_TRUE : VK_FALSE;

    VkPipelineMultisampleStateCreateInfo& sampling = fixed.sampling;
    sampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    sampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    const uint32_t d = key.depth_value;
    VkPipelineDepthStencilStateCreateInfo& depth_value = fixed.depth_value;
    depth_value.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    if (key.formats[4]) {
      depth_value.depthTestEnable = (d >> 1) & 0x1;
      depth_value.depthWriteEnable = ((d >> 1) & 0x1) && ((d >> 2) & 0x1);
      depth_value.depthCompareOp = VkCompareOp((d >> 4) & 0x7);
      depth_value.stencilTestEnable = d & 0x1;
      depth_value.front.failOp = VkStencilOp((d >> 11) & 0x7);
      depth_value.front.passOp = VkStencilOp((d >> 14) & 0x7);
      depth_value.front.depthFailOp = VkStencilOp((d >> 17) & 0x7);
      depth_value.front.compareOp = VkCompareOp((d >> 8) & 0x7);
      if ((d >> 7) & 0x1) {
        depth_value.back.compareOp = VkCompareOp((d >> 20) & 0x7);
        depth_value.back.failOp = VkStencilOp((d >> 23) & 0x7);
        depth_value.back.passOp = VkStencilOp((d >> 26) & 0x7);
        depth_value.back.depthFailOp = VkStencilOp((d >> 29) & 0x7);
      } else {
        depth_value.back = depth_value.front;
      }
    }

    static constexpr VkBlendFactor kFactors[32] = {
        VK_BLEND_FACTOR_ZERO,
        VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_ZERO,
        VK_BLEND_FACTOR_ZERO,
        VK_BLEND_FACTOR_SRC_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        VK_BLEND_FACTOR_DST_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        VK_BLEND_FACTOR_CONSTANT_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
        VK_BLEND_FACTOR_CONSTANT_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
        VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
    };
    const auto operation = [&](uint32_t op) {
      switch (op) {
        case 1:
          return VK_BLEND_OP_SUBTRACT;
        case 2:
          return VK_BLEND_OP_MIN;
        case 3:
          return VK_BLEND_OP_MAX;
        case 4:
          if (warn) {
            Warn(52, "mezcla con resta inversa: se usa resta");
          }
          return VK_BLEND_OP_SUBTRACT;
        default:
          return VK_BLEND_OP_ADD;
      }
    };
    std::array<VkPipelineColorBlendAttachmentState, 4>& blends = fixed.blends;
    uint32_t& n_colores = fixed.n_colores;
    for (uint32_t i = 0; i < 4; ++i) {
      if (!key.formats[i]) {
        continue;
      }
      const uint32_t m = key.blend[i];
      VkPipelineColorBlendAttachmentState& s = blends[n_colores++];
      s.colorWriteMask = (key.masks >> (i * 4)) & 0xF;
      const uint32_t src_select = m & 0x1F, op = (m >> 5) & 0x7, target = (m >> 8) & 0x1F;
      const uint32_t source_to = (m >> 16) & 0x1F, op_a = (m >> 21) & 0x7,
                     target_to = (m >> 24) & 0x1F;
      /*
       * Blending is only enabled if it is needed on the channels that are written.
       *
       * It used to require all six fields to be "1 x source + 0 x destination, ADD". But if the mask does
       * not write RGB, the three color factors do not matter, and the same goes for alpha. With blendEnable
       * false the ROP does not have to read the destination or go through the blend unit. The result is the
       * same.
       */
      const bool rgb_write = (s.colorWriteMask & 0x7) != 0;
      const bool alpha_write = (s.colorWriteMask & 0x8) != 0;
      const bool direct_color = src_select == 1 && target == 0 && op == 0;
      const bool direct_alpha = source_to == 1 && target_to == 0 && op_a == 0;
      s.blendEnable = (rgb_write && !direct_color) || (alpha_write && !direct_alpha);
      s.srcColorBlendFactor = kFactors[src_select];
      s.dstColorBlendFactor = kFactors[target];
      s.colorBlendOp = operation(op);
      s.srcAlphaBlendFactor = kFactors[source_to];
      s.dstAlphaBlendFactor = kFactors[target_to];
      s.alphaBlendOp = operation(op_a);
    }
    VkPipelineColorBlendStateCreateInfo& blend = fixed.blend;
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = n_colores;
    blend.pAttachments = blends.data();
  }

  // One stencil face, in what Vulkan looks at in the pipeline (masks and reference are dynamic).
  static bool SameStencilFace(const VkStencilOpState& a, const VkStencilOpState& b) {
    return a.failOp == b.failOp && a.passOp == b.passOp && a.depthFailOp == b.depthFailOp && a.compareOp == b.compareOp;
  }

  /*
   * Two fixed states, field by field, in what Vulkan looks at: the blend equation only with blendEnable,
   * each half only if the mask writes its channels, the Z function only with a Z test, stencil operations
   * only with stencil. Returns the first differing field, or nullptr if there is none.
   */
  static const char* EffectiveDifference(const PipelineFixedState& a, const PipelineFixedState& b) {
    if (a.ensamblado.topology != b.ensamblado.topology) {
      return "topology";
    }
    if (a.ensamblado.primitiveRestartEnable != b.ensamblado.primitiveRestartEnable) {
      return "primitiveRestartEnable";
    }
    if (a.view.viewportCount != b.view.viewportCount || a.view.scissorCount != b.view.scissorCount) {
      return "viewportCount o scissorCount";
    }
    const VkPipelineRasterizationStateCreateInfo& ra = a.rasterizado;
    const VkPipelineRasterizationStateCreateInfo& rb = b.rasterizado;
    if (ra.polygonMode != rb.polygonMode || ra.lineWidth != rb.lineWidth ||
        ra.rasterizerDiscardEnable != rb.rasterizerDiscardEnable || ra.depthClampEnable != rb.depthClampEnable) {
      return "polygonMode, lineWidth, rasterizerDiscardEnable o depthClampEnable";
    }
    if (ra.cullMode != rb.cullMode) {
      return "cullMode";
    }
    if (ra.frontFace != rb.frontFace) {
      return "frontFace";
    }
    if (ra.depthBiasEnable != rb.depthBiasEnable) {
      return "depthBiasEnable";
    }
    if (a.sampling.rasterizationSamples != b.sampling.rasterizationSamples) {
      return "rasterizationSamples";
    }
    const VkPipelineDepthStencilStateCreateInfo& pa = a.depth_value;
    const VkPipelineDepthStencilStateCreateInfo& pb = b.depth_value;
    if (pa.depthTestEnable != pb.depthTestEnable) {
      return "depthTestEnable";
    }
    if (pa.depthWriteEnable != pb.depthWriteEnable) {
      return "depthWriteEnable";
    }
    if (pa.depthTestEnable && pa.depthCompareOp != pb.depthCompareOp) {
      return "depthCompareOp";
    }
    if (pa.depthBoundsTestEnable != pb.depthBoundsTestEnable) {
      return "depthBoundsTestEnable";
    }
    if (pa.stencilTestEnable != pb.stencilTestEnable) {
      return "stencilTestEnable";
    }
    if (pa.stencilTestEnable && !SameStencilFace(pa.front, pb.front)) {
      return "front (estencil)";
    }
    if (pa.stencilTestEnable && !SameStencilFace(pa.back, pb.back)) {
      return "back (estencil)";
    }
    if (a.n_colores != b.n_colores || a.blend.attachmentCount != b.blend.attachmentCount ||
        a.blend.logicOpEnable != b.blend.logicOpEnable) {
      return "attachmentCount o logicOpEnable";
    }
    for (uint32_t i = 0; i < a.n_colores && i < 4; ++i) {
      const VkPipelineColorBlendAttachmentState& x = a.blends[i];
      const VkPipelineColorBlendAttachmentState& y = b.blends[i];
      if (x.colorWriteMask != y.colorWriteMask) {
        return "colorWriteMask";
      }
      if (x.blendEnable != y.blendEnable) {
        return "blendEnable";
      }
      if (x.blendEnable && (x.colorWriteMask & 0x7) &&
          (x.srcColorBlendFactor != y.srcColorBlendFactor || x.dstColorBlendFactor != y.dstColorBlendFactor ||
           x.colorBlendOp != y.colorBlendOp)) {
        return "ecuacion de color";
      }
      if (x.blendEnable && (x.colorWriteMask & 0x8) &&
          (x.srcAlphaBlendFactor != y.srcAlphaBlendFactor || x.dstAlphaBlendFactor != y.dstAlphaBlendFactor ||
           x.alphaBlendOp != y.alphaBlendOp)) {
        return "ecuacion de alfa";
      }
    }
    return nullptr;
  }

  /*
   * Phase 0a (nfsmw_nativo_clave_canonica). A draw's key in canonical form. Same criterion as
   * EstadoCanonico (the counter) with two differences: disabled blending becomes 1 x source + 0 x
   * destination, ADD (0x00010001; with 0, RellenarEstadoFijo would enable it with zero factors), and with
   * blending enabled the half of the equation (color or alpha) whose channels the mask does not write also
   * becomes that.
   */
  static void Canonicalize(PipelineKey& c) {
    constexpr uint32_t kDirect = 0x00010001u;  // 1 x source + 0 x destination, ADD, for color and alpha
    uint32_t masks = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      if (!c.formats[i]) {
        c.blend[i] = 0;  // RellenarEstadoFijo skips targets not in the pass (Dibujar leaves them at 0)
        continue;
      }
      const uint32_t mask = (c.masks >> (i * 4)) & 0xF;
      masks |= mask << (i * 4);
      const uint32_t m = c.blend[i];
      const bool direct_color = (m & 0x1F) == 1 && ((m >> 8) & 0x1F) == 0 && ((m >> 5) & 0x7) == 0;
      const bool direct_alpha = ((m >> 16) & 0x1F) == 1 && ((m >> 24) & 0x1F) == 0 && ((m >> 21) & 0x7) == 0;
      const bool rgb_write = (mask & 0x7) != 0;
      const bool alpha_write = (mask & 0x8) != 0;
      if (!((rgb_write && !direct_color) || (alpha_write && !direct_alpha))) {
        c.blend[i] = kDirect;  // without blendEnable Vulkan ignores the equation
        continue;
      }
      uint32_t canonical = m;
      if (!rgb_write) {
        canonical = (canonical & 0xFFFF0000u) | 0x00000001u;  // the color equation reaches no channel
      }
      if (!alpha_write) {
        canonical = (canonical & 0x0000FFFFu) | 0x00010000u;  // neither does the alpha one
      }
      c.blend[i] = canonical;
    }
    c.masks = masks;
    uint32_t d = c.formats[4] ? c.depth_value : 0;  // ignored without a depth target
    d &= ~0x8u;                                      // bit 3 is not read
    if (!((d >> 1) & 0x1)) {
      d &= ~0x74u;  // without a Z test, write and function do not count
    }
    if (!(d & 0x1)) {
      d &= 0x77u;  // sin estencil no cuentan sus funciones ni sus operaciones
    } else if (!((d >> 7) & 0x1)) {
      d &= 0x000FFFF7u;  // without its own back face state, the back copies the front
    }
    c.depth_value = d;
  }

  /*
   * The phase 0a guard. Canonizar only touches blending, masks and depth: the rest of the key must come
   * out the same, and the fixed state of both keys must match in everything Vulkan looks at
   * (DiferenciaEfectiva on RellenarEstadoFijo). On a difference the phase switches off for the session and
   * false is returned.
   */
  bool CheckCanonicalKey(const PipelineKey& raw_data, const PipelineKey& canonical, uint64_t n) {
    ++checked_canonical_;
    const char* field = nullptr;
    if (raw_data.vs != canonical.vs || raw_data.ps != canonical.ps || raw_data.entry != canonical.entry ||
        raw_data.topo != canonical.topo || raw_data.specialization != canonical.specialization ||
        raw_data.rasterizado != canonical.rasterizado || raw_data.padding != canonical.padding ||
        raw_data.fill2 != canonical.fill2 ||
        std::memcmp(raw_data.formats, canonical.formats, sizeof(raw_data.formats)) != 0) {
      field = "shaders, entrada, topologia, especializacion, rasterizado o formatos";
    } else {
      PipelineFixedState a;
      PipelineFixedState b;
      FillFixedState(raw_data, a);
      FillFixedState(canonical, b);
      field = EffectiveDifference(a, b);
    }
    if (field) {
      canonical_key_disabled_ = true;
      canonical_key_ = false;
      valid_canonical_ = false;
      REXLOG_ERROR("[nativo] C6 clave canonica: DIFERENCIA en {} (comprobacion {}: VS {} PS {}, mezcla {:08X} -> "
                   "{:08X}, mascaras {:04X} -> {:04X}, profundidad {:08X} -> {:08X}). Apagada para el resto de la "
                   "sesion: se busca con la clave de siempre",
                   field, n, raw_data.vs, raw_data.ps, raw_data.blend[0], canonical.blend[0], raw_data.masks,
                   canonical.masks, raw_data.depth_value, canonical.depth_value);
      return false;
    }
    if (n == kCanonicalToCheck) {
      NFSMW_RING_REPORT("[nativo] C6 clave canonica: {} claves cambiadas comprobadas campo a campo contra la de "
                           "siempre, 0 diferencias; sigue comprobando 1 de cada 4096",
                           n);
    }
    return true;
  }

  /*
   * The key used to look up a draw's pipeline. The raw one (clave) is still used for everything else: the
   * deferred sky reads its blending (opaco_en_todos) and the counter compares it. Phase 0a: canonical
   * form, remembering the previous draw's (the same raw key gives the same canonical one).
   */
  PipelineKey SearchKey(const PipelineKey& key) {
    PipelineKey c = key;
    if (canonical_key_) {
      if (valid_canonical_ && std::memcmp(&key, &raw_canonical_, sizeof(key)) == 0) {
        c = result_canonical_;
      } else {
        Canonicalize(c);
        if (std::memcmp(&c, &key, sizeof(c)) != 0) {
          const uint64_t n = ++changed_canonical_;
          if ((n <= kCanonicalToCheck || (n & 4095) == 0) && !CheckCanonicalKey(key, c, n)) {
            c = key;  // the guard saw a difference: this draw, and the rest of the session, use the usual one
          }
        }
        raw_canonical_ = key;
        result_canonical_ = c;
        valid_canonical_ = canonical_key_;
      }
    }
    // Phases 1 and 2. What goes through vkCmdSet* leaves the key, and the flag (relleno2) keeps these
    // pipelines apart from the usual ones in the map, in the direct-mapped cache and in PipelineDe's
    // shortcut.
    const uint32_t mode_eds = eds_mode_;
    if (mode_eds & kEds12) {
      c.depth_value = 0;
      c.rasterizado = 0;
      c.topo = TopologyRepresentative(c.topo);
    }
    if (mode_eds & kEds3) {  // phase 2: blending and masks go through vkCmdSet*; targets stay in formatos
      std::fill(std::begin(c.blend), std::end(c.blend), 0u);
      c.masks = 0;
    }
    c.fill2 = mode_eds;
    return c;
  }

  // Every 20 s, phase 0a (nfsmw_nativo_clave_canonica).
  void CanonicalKeyReport() {
    const auto now_time = std::chrono::steady_clock::now();
    if (now_time - canonical_report_ < std::chrono::seconds(20)) {
      return;
    }
    const bool is_first = canonical_report_ == std::chrono::steady_clock::time_point{};
    canonical_report_ = now_time;
    const uint64_t changed = changed_canonical_ - changed_canonical_previous_;
    changed_canonical_previous_ = changed_canonical_;
    const uint64_t pipelines = pipelines_.size();
    const uint64_t added_count = pipelines - std::min<uint64_t>(pipelines, pipelines_canonical_previous_);
    pipelines_canonical_previous_ = pipelines;
    if (is_first || (!changed && !added_count)) {
      return;
    }
    NFSMW_RING_REPORT("[nativo] C6 clave canonica (build 184): {}; {} claves cambiadas en 20 s ({} comprobadas "
                         "desde el principio, {}); {} pipelines en el mapa, {} nuevos en 20 s",
                         canonical_key_disabled_ ? "APAGADA por la guardia"
                         : canonical_key_       ? "encendida"
                                                 : "apagada",
                         changed, checked_canonical_,
                         checked_canonical_ >= kCanonicalToCheck ? "guardia superada" : "comprobando", pipelines,
                         added_count);
  }

  // Every 20 s, phase 0b (nfsmw_nativo_pipeline_entre_pases).
  void PipelineBetweenPassesReport() {
    const auto now_time = std::chrono::steady_clock::now();
    if (now_time - between_passes_report_ < std::chrono::seconds(20)) {
      return;
    }
    const bool is_first = between_passes_report_ == std::chrono::steady_clock::time_point{};
    between_passes_report_ = now_time;
    const uint64_t passes = started_passes_ - between_passes_passes_previous_;
    const uint64_t kept = passes_with_pipeline_ - between_kept_passes_previous_;
    between_passes_passes_previous_ = started_passes_;
    between_kept_passes_previous_ = passes_with_pipeline_;
    if (is_first || !passes) {
      return;
    }
    NFSMW_RING_REPORT("[nativo] C6 pipeline entre pases (build 184): {}; {} pases empezados en 20 s, {} con un "
                         "pipeline enlazado que se conserva (su primer dibujo no vuelve a enlazar si la clave es la "
                         "misma)",
                         pipeline_between_passes_ ? "se conserva" : "se olvida, como antes", passes, kept);
  }

  /*
   * Dynamic state phases 1 and 2. The state that leaves the pipeline, with the Vulkan values the usual
   * pipeline would carry (the criterion of RellenarEstadoFijo, which is PipelineDe's).
   */
  struct EdsState {
    uint32_t face = 0;         // VkCullModeFlags
    uint32_t front_face = 0;       // VkFrontFace
    uint32_t topo = 0;    // VkPrimitiveTopology
    uint32_t restart_flag = 0;     // primitiveRestartEnable
    uint32_t bias = 0;        // depthBiasEnable
    uint32_t test_z = 0;     // depthTestEnable
    uint32_t write_z = 0;    // depthWriteEnable
    uint32_t function_z = 0;    // VkCompareOp
    uint32_t stencil_state = 0;     // stencilTestEnable
    uint32_t ahead[4] = {};  // failOp, passOp, depthFailOp (VkStencilOp) y compareOp (VkCompareOp)
    uint32_t behind[4] = {};   // the same for the back face
    uint32_t n_colores = 0;                    // phase 2: the pass's color targets, compacted as in PipelineDe
    VkBool32 active_blend[4] = {};            // blendEnable
    VkColorBlendEquationEXT equation[4] = {};  // color and alpha factors and operations
    VkColorComponentFlags mask[4] = {};     // colorWriteMask
  };

  // Without EDS3 the dynamic topology must be of the same class as the pipeline's: the lookup key carries
  // one per class. Classes the ring does not draw stay as they are.
  static uint32_t TopologyRepresentative(uint32_t topo) {
    switch (TopologyClass(topo)) {
      case 1:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      case 2:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      default:
        return topo;
    }
  }

  static const char* EdsModeName(uint32_t mode) {
    switch (mode & (kEds12 | kEds3)) {
      case kEds12:
        return "EDS1/EDS2 (fase 1)";
      case kEds3:
        return "EDS3 (fase 2)";
      case kEds12 | kEds3:
        return "EDS1/EDS2 y EDS3 (fases 1 y 2)";
      default:
        return "apagado: todo el estado en el pipeline, como antes";
    }
  }

  // The dynamic state of a raw key in the given mode (kEds12 | kEds3).
  void EdsStateFor(const PipelineKey& c, EdsState& e, uint32_t mode) {
    e = EdsState{};
    if (mode & kEds12) {
      e.face = ((c.rasterizado & 0x1) ? uint32_t(VK_CULL_MODE_FRONT_BIT) : 0u) |
               ((c.rasterizado & 0x2) ? uint32_t(VK_CULL_MODE_BACK_BIT) : 0u);
      e.front_face = (c.rasterizado & 0x4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
      e.restart_flag = (c.rasterizado & 0x8) ? 1u : 0u;
      e.bias = (c.rasterizado & 0x10) ? 1u : 0u;
      e.topo = c.topo;
      if (c.formats[4]) {  // without a depth target everything stays zero, as in RellenarEstadoFijo
        const uint32_t d = c.depth_value;
        e.test_z = (d >> 1) & 0x1;
        e.write_z = ((d >> 1) & 0x1) & ((d >> 2) & 0x1);
        e.function_z = (d >> 4) & 0x7;
        e.stencil_state = d & 0x1;
        e.ahead[0] = (d >> 11) & 0x7;
        e.ahead[1] = (d >> 14) & 0x7;
        e.ahead[2] = (d >> 17) & 0x7;
        e.ahead[3] = (d >> 8) & 0x7;
        if ((d >> 7) & 0x1) {
          e.behind[0] = (d >> 23) & 0x7;
          e.behind[1] = (d >> 26) & 0x7;
          e.behind[2] = (d >> 29) & 0x7;
          e.behind[3] = (d >> 20) & 0x7;
        } else {
          std::memcpy(e.behind, e.ahead, sizeof(e.behind));
        }
      }
    }
    if (mode & kEds3) {  // phase 2: like RellenarEstadoFijo, target by target and compacted
      for (uint32_t i = 0; i < 4; ++i) {
        if (!c.formats[i]) {
          continue;
        }
        const uint32_t a = e.n_colores++;
        const uint32_t m = c.blend[i];
        const uint32_t mask = (c.masks >> (i * 4)) & 0xF;
        const uint32_t src_select = m & 0x1F, op = (m >> 5) & 0x7, target = (m >> 8) & 0x1F;
        const uint32_t source_to = (m >> 16) & 0x1F, op_a = (m >> 21) & 0x7, target_to = (m >> 24) & 0x1F;
        const bool direct_color = src_select == 1 && target == 0 && op == 0;
        const bool direct_alpha = source_to == 1 && target_to == 0 && op_a == 0;
        e.mask[a] = mask;
        e.active_blend[a] =
            (((mask & 0x7) != 0 && !direct_color) || ((mask & 0x8) != 0 && !direct_alpha)) ? VK_TRUE : VK_FALSE;
        VkColorBlendEquationEXT& q = e.equation[a];
        q.srcColorBlendFactor = kBlendFactors[src_select];
        q.dstColorBlendFactor = kBlendFactors[target];
        q.colorBlendOp = BlendOperation(op);
        q.srcAlphaBlendFactor = kBlendFactors[source_to];
        q.dstAlphaBlendFactor = kBlendFactors[target_to];
        q.alphaBlendOp = BlendOperation(op_a);
      }
    }
  }

  /*
   * Records the vkCmdSet* calls for what changed since the last state set in the buffer (eds_grabado_),
   * or for everything with todo = true, and tracks it. What Vulkan ignores in this draw (the Z function
   * without a Z test, stencil operations without stencil, the blend equation without blending) is not
   * re-recorded if only that changed; with todo = true it is recorded anyway, so everything is set at least
   * once in the buffer.
   */
  void EmitDynamicState(VkCommandBuffer cmd, const EdsState& d, bool all, uint32_t mode) {
    EdsState& g = eds_recorded_;
    eds_calls_[kEdsAll] += all ? 1 : 0;
    if (mode & kEds12) {
      if (all || d.face != g.face) {
        set_face_(cmd, VkCullModeFlags(d.face));
        g.face = d.face;
        ++eds_calls_[kEdsFace];
      }
      if (all || d.front_face != g.front_face) {
        set_front_(cmd, VkFrontFace(d.front_face));
        g.front_face = d.front_face;
        ++eds_calls_[kEdsFront];
      }
      if (all || d.topo != g.topo) {
        set_topology_(cmd, VkPrimitiveTopology(d.topo));
        g.topo = d.topo;
        ++eds_calls_[kEdsTopology];
      }
      if (all || d.restart_flag != g.restart_flag) {
        set_reset_(cmd, VkBool32(d.restart_flag));
        g.restart_flag = d.restart_flag;
        ++eds_calls_[kEdsReset];
      }
      if (all || d.bias != g.bias) {
        set_bias_(cmd, VkBool32(d.bias));
        g.bias = d.bias;
        ++eds_calls_[kEdsBias];
      }
      if (all || d.test_z != g.test_z) {
        set_z_test_(cmd, VkBool32(d.test_z));
        g.test_z = d.test_z;
        ++eds_calls_[kEdsTestZ];
      }
      if (all || d.write_z != g.write_z) {
        set_write_z_(cmd, VkBool32(d.write_z));
        g.write_z = d.write_z;
        ++eds_calls_[kEdsWriteZ];
      }
      if (all || (d.test_z && d.function_z != g.function_z)) {
        set_z_func_(cmd, VkCompareOp(d.function_z));
        g.function_z = d.function_z;
        ++eds_calls_[kEdsFunctionZ];
      }
      if (all || d.stencil_state != g.stencil_state) {
        set_stencil_(cmd, VkBool32(d.stencil_state));
        g.stencil_state = d.stencil_state;
        ++eds_calls_[kEdsStencil];
      }
      const bool ahead = all || (d.stencil_state && std::memcmp(d.ahead, g.ahead, sizeof(d.ahead)) != 0);
      const bool behind = all || (d.stencil_state && std::memcmp(d.behind, g.behind, sizeof(d.behind)) != 0);
      if (ahead && behind && std::memcmp(d.ahead, d.behind, sizeof(d.ahead)) == 0) {
        set_ops_stencil_(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, VkStencilOp(d.ahead[0]), VkStencilOp(d.ahead[1]),
                          VkStencilOp(d.ahead[2]), VkCompareOp(d.ahead[3]));
        ++eds_calls_[kEdsOpsStencil];
      } else {
        if (ahead) {
          set_ops_stencil_(cmd, VK_STENCIL_FACE_FRONT_BIT, VkStencilOp(d.ahead[0]), VkStencilOp(d.ahead[1]),
                            VkStencilOp(d.ahead[2]), VkCompareOp(d.ahead[3]));
          ++eds_calls_[kEdsOpsStencil];
        }
        if (behind) {
          set_ops_stencil_(cmd, VK_STENCIL_FACE_BACK_BIT, VkStencilOp(d.behind[0]), VkStencilOp(d.behind[1]),
                            VkStencilOp(d.behind[2]), VkCompareOp(d.behind[3]));
          ++eds_calls_[kEdsOpsStencil];
        }
      }
      if (ahead) {
        std::memcpy(g.ahead, d.ahead, sizeof(g.ahead));
      }
      if (behind) {
        std::memcpy(g.behind, d.behind, sizeof(g.behind));
      }
    }
    if (mode & kEds3) {  // fase 2
      const uint32_t n = d.n_colores;
      if (!n) {
        if (all) {
          g.n_colores = 0;  // nothing set in this buffer: the first draw with color will set it all
        }
      } else {
        const bool other_n = all || n != g.n_colores;
        bool is_on = other_n;
        bool equation = other_n;
        bool mask = other_n;
        for (uint32_t a = 0; a < n && a < 4; ++a) {
          is_on = is_on || d.active_blend[a] != g.active_blend[a];
          mask = mask || d.mask[a] != g.mask[a];
          if (!equation && d.active_blend[a]) {  // without blending the equation does not count; each half, with its channels
            const VkColorBlendEquationEXT& x = d.equation[a];
            const VkColorBlendEquationEXT& y = g.equation[a];
            equation = ((d.mask[a] & 0x7) != 0 &&
                        (x.srcColorBlendFactor != y.srcColorBlendFactor ||
                         x.dstColorBlendFactor != y.dstColorBlendFactor || x.colorBlendOp != y.colorBlendOp)) ||
                       ((d.mask[a] & 0x8) != 0 &&
                        (x.srcAlphaBlendFactor != y.srcAlphaBlendFactor ||
                         x.dstAlphaBlendFactor != y.dstAlphaBlendFactor || x.alphaBlendOp != y.alphaBlendOp));
          }
        }
        if (is_on) {
          set_blend_enabled_(cmd, 0, n, d.active_blend);
          std::memcpy(g.active_blend, d.active_blend, sizeof(g.active_blend));
          ++eds_calls_[kEdsActiveBlend];
        }
        if (equation) {
          set_equation_(cmd, 0, n, d.equation);
          std::memcpy(g.equation, d.equation, sizeof(g.equation));
          ++eds_calls_[kEdsEquation];
        }
        if (mask) {
          set_mask_(cmd, 0, n, d.mask);
          std::memcpy(g.mask, d.mask, sizeof(g.mask));
          ++eds_calls_[kEdsMask];
        }
        g.n_colores = n;
      }
    }
  }

  /*
   * The guard of phases 1 and 2. What the ring believes is set in the buffer (eds_grabado_) must be what
   * this draw's usual pipeline would carry. That state comes from RellenarEstadoFijo with the raw key, the
   * same code PipelineDe uses to create the usual pipelines (not another copy of EstadoEdsDe), and it is
   * compared field by field in what Vulkan looks at. On a difference dynamic state switches off for the
   * session (both phases) and false is returned: the draw has to use its usual pipeline. Limit: it cannot
   * see the GPU, only that the computations and the record of what was recorded agree.
   */
  bool CheckDynamicState(const PipelineKey& key, uint64_t n) {
    ++eds_checked_;
    PipelineFixedState fixed;
    FillFixedState(key, fixed);
    const EdsState& g = eds_recorded_;
    const char* field = nullptr;
    uint32_t pinned = 0;
    uint32_t expected = 0;
    const auto peek = [&](const char* name, uint32_t a, uint32_t b) {
      if (!field && a != b) {
        field = name;
        pinned = a;
        expected = b;
      }
    };
    if (eds_mode_ & kEds12) {
      const VkPipelineRasterizationStateCreateInfo& r = fixed.rasterizado;
      const VkPipelineDepthStencilStateCreateInfo& z = fixed.depth_value;
      peek("cullMode", g.face, r.cullMode);
      peek("frontFace", g.front_face, r.frontFace);
      peek("topology", g.topo, fixed.ensamblado.topology);
      peek("primitiveRestartEnable", g.restart_flag, fixed.ensamblado.primitiveRestartEnable);
      peek("depthBiasEnable", g.bias, r.depthBiasEnable);
      peek("depthTestEnable", g.test_z, z.depthTestEnable);
      peek("depthWriteEnable", g.write_z, z.depthWriteEnable);
      if (z.depthTestEnable) {
        peek("depthCompareOp", g.function_z, z.depthCompareOp);
      }
      peek("stencilTestEnable", g.stencil_state, z.stencilTestEnable);
      if (z.stencilTestEnable) {
        peek("front.failOp", g.ahead[0], z.front.failOp);
        peek("front.passOp", g.ahead[1], z.front.passOp);
        peek("front.depthFailOp", g.ahead[2], z.front.depthFailOp);
        peek("front.compareOp", g.ahead[3], z.front.compareOp);
        peek("back.failOp", g.behind[0], z.back.failOp);
        peek("back.passOp", g.behind[1], z.back.passOp);
        peek("back.depthFailOp", g.behind[2], z.back.depthFailOp);
        peek("back.compareOp", g.behind[3], z.back.compareOp);
      }
    }
    if ((eds_mode_ & kEds3) && fixed.n_colores) {  // phase 2 (without color targets there is nothing to check)
      peek("attachmentCount", g.n_colores, fixed.n_colores);
      for (uint32_t a = 0; a < fixed.n_colores && a < 4; ++a) {
        const VkPipelineColorBlendAttachmentState& s = fixed.blends[a];
        const VkColorBlendEquationEXT& q = g.equation[a];
        peek("colorWriteMask", g.mask[a], s.colorWriteMask);
        peek("blendEnable", g.active_blend[a], s.blendEnable);
        if (s.blendEnable && (s.colorWriteMask & 0x7)) {
          peek("srcColorBlendFactor", q.srcColorBlendFactor, s.srcColorBlendFactor);
          peek("dstColorBlendFactor", q.dstColorBlendFactor, s.dstColorBlendFactor);
          peek("colorBlendOp", q.colorBlendOp, s.colorBlendOp);
        }
        if (s.blendEnable && (s.colorWriteMask & 0x8)) {
          peek("srcAlphaBlendFactor", q.srcAlphaBlendFactor, s.srcAlphaBlendFactor);
          peek("dstAlphaBlendFactor", q.dstAlphaBlendFactor, s.dstAlphaBlendFactor);
          peek("alphaBlendOp", q.alphaBlendOp, s.alphaBlendOp);
        }
      }
    }
    if (field) {
      eds_disabled_ = true;
      eds_mode_ = 0;
      eds_valid_ = false;
      bound_pipeline_ = VK_NULL_HANDLE;
      REXLOG_ERROR("[nativo] C6 estado dinamico: DIFERENCIA en {} (comprobacion {}: fijado {} y el pipeline de siempre "
                   "llevaria {}; VS {} PS {}, topologia {}, profundidad {:08X}, rasterizado {:02X}, mascaras {:04X}). "
                   "Apagado para el resto de la sesion: vuelven los pipelines con todo el estado fijo",
                   field, n, pinned, expected, key.vs, key.ps, key.topo, key.depth_value,
                   key.rasterizado, key.masks);
      return false;
    }
    if (n == kEdsToCheck) {
      NFSMW_RING_REPORT("[nativo] C6 estado dinamico: {} cambios de clave comprobados contra el estado de su "
                           "pipeline de siempre, 0 diferencias; sigue comprobando 1 de cada 4096",
                           n);
    }
    return true;
  }

  /*
   * Phases 1 and 2, before every draw with dynamic state. With the same raw key as the last state set in
   * this buffer there is nothing to do; otherwise what changed is set and the guard checks it for the
   * first 200,000 changes and then 1 in 4,096. Returns false if the guard has switched dynamic state off.
   */
  bool FixDynamicState(VkCommandBuffer cmd, const PipelineKey& key) {
    ++eds_draws_;
    if (eds_valid_ && std::memcmp(&key, &eds_key_, sizeof(key)) == 0) {
      ++eds_repeated_;
      return true;
    }
    EdsState d;
    EdsStateFor(key, d, eds_mode_);
    EmitDynamicState(cmd, d, !eds_valid_, eds_mode_);
    eds_key_ = key;
    eds_valid_ = true;
    const uint64_t n = ++eds_checkable_;
    if (n <= kEdsToCheck || (n & 4095) == 0) {
      return CheckDynamicState(key, n);
    }
    return true;
  }

  /*
   * Dynamic state phases 1 and 2. The EDS1/EDS2 vkCmdSet* functions are core in Vulkan 1.3 and the SDK's
   * table does not load them: they are requested from the driver, like vkCmdCopyImage. With an API below
   * 1.3 or with any of them missing, phase 1 is not used for the whole session and one log line says so.
   */
  void LoadDynamicState() {
    const auto& ifn = vk_device_->vulkan_instance()->functions();
    const auto fetch_proc = [&](const char* name) { return ifn.vkGetDeviceProcAddr(device_, name); };
    set_face_ = reinterpret_cast<PFN_vkCmdSetCullMode>(fetch_proc("vkCmdSetCullMode"));
    set_front_ = reinterpret_cast<PFN_vkCmdSetFrontFace>(fetch_proc("vkCmdSetFrontFace"));
    set_topology_ = reinterpret_cast<PFN_vkCmdSetPrimitiveTopology>(fetch_proc("vkCmdSetPrimitiveTopology"));
    set_reset_ = reinterpret_cast<PFN_vkCmdSetPrimitiveRestartEnable>(fetch_proc("vkCmdSetPrimitiveRestartEnable"));
    set_bias_ = reinterpret_cast<PFN_vkCmdSetDepthBiasEnable>(fetch_proc("vkCmdSetDepthBiasEnable"));
    set_z_test_ = reinterpret_cast<PFN_vkCmdSetDepthTestEnable>(fetch_proc("vkCmdSetDepthTestEnable"));
    set_write_z_ = reinterpret_cast<PFN_vkCmdSetDepthWriteEnable>(fetch_proc("vkCmdSetDepthWriteEnable"));
    set_z_func_ = reinterpret_cast<PFN_vkCmdSetDepthCompareOp>(fetch_proc("vkCmdSetDepthCompareOp"));
    set_stencil_ = reinterpret_cast<PFN_vkCmdSetStencilTestEnable>(fetch_proc("vkCmdSetStencilTestEnable"));
    set_ops_stencil_ = reinterpret_cast<PFN_vkCmdSetStencilOp>(fetch_proc("vkCmdSetStencilOp"));
    const uint32_t api = vk_device_->properties().apiVersion;
    eds12_available_ = api >= VK_MAKE_API_VERSION(0, 1, 3, 0) && set_face_ && set_front_ && set_topology_ &&
                        set_reset_ && set_bias_ && set_z_test_ && set_write_z_ && set_z_func_ &&
                        set_stencil_ && set_ops_stencil_;
    REXLOG_INFO("[nativo] C6 estado dinamico (build 184): EDS1/EDS2 {} (API del dispositivo {}.{}.{})",
                eds12_available_ ? "disponible (nucleo 1.3)" : "NO disponible: la fase 1 no se usa",
                VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api));
#if defined(REX_UI_VULKAN_ESTADO_DINAMICO3)
    // Phase 2. The driver only provides these three if the SDK enabled the extension; the SDK records the
    // three features in its properties (ui_vulkan_estado_dinamico3.patch).
    set_blend_enabled_ = reinterpret_cast<PFN_vkCmdSetColorBlendEnableEXT>(fetch_proc("vkCmdSetColorBlendEnableEXT"));
    set_equation_ = reinterpret_cast<PFN_vkCmdSetColorBlendEquationEXT>(fetch_proc("vkCmdSetColorBlendEquationEXT"));
    set_mask_ = reinterpret_cast<PFN_vkCmdSetColorWriteMaskEXT>(fetch_proc("vkCmdSetColorWriteMaskEXT"));
    const auto& p3 = vk_device_->properties();
    eds3_available_ = vk_device_->extensions().ext_EXT_extended_dynamic_state3 &&
                       p3.extendedDynamicState3ColorBlendEnable && p3.extendedDynamicState3ColorBlendEquation &&
                       p3.extendedDynamicState3ColorWriteMask && set_blend_enabled_ && set_equation_ && set_mask_;
    REXLOG_INFO("[nativo] C6 estado dinamico (build 184): EDS3 (mezcla, ecuacion y mascara de color) {}",
                eds3_available_ ? "disponible"
                                 : "NO disponible (el dispositivo no la da o no esta habilitada): la fase 2 no se usa");
#else
    REXLOG_INFO("[nativo] C6 estado dinamico (build 184): EDS3 NO disponible: este SDK no habilita "
                "VK_EXT_extended_dynamic_state3 (falta ui_vulkan_estado_dinamico3.patch); la fase 2 no se usa");
#endif
  }

  // Every 20 s, dynamic state phases 1 and 2.
  void DynamicStateReport() {
    const auto now_time = std::chrono::steady_clock::now();
    if (now_time - eds_report_ < std::chrono::seconds(20)) {
      return;
    }
    const bool is_first = eds_report_ == std::chrono::steady_clock::time_point{};
    eds_report_ = now_time;
    const std::array<uint64_t, kEdsN> n = eds_calls_;
    eds_calls_.fill(0);
    const uint64_t draws = eds_draws_ - eds_draws_previous_;
    const uint64_t repeated = eds_repeated_ - eds_repeated_previous_;
    const uint64_t frames = frame_ - eds_frames_previous_;
    eds_draws_previous_ = eds_draws_;
    eds_repeated_previous_ = eds_repeated_;
    eds_frames_previous_ = frame_;
    if (is_first || !draws) {
      return;
    }
    uint64_t calls = 0;
    for (size_t k = 0; k < kEdsN; ++k) {
      calls += k == kEdsAll ? 0 : n[k];
    }
    NFSMW_RING_REPORT(
        "[nativo] C6 estado dinamico (build 184): {}; {} dibujos en 20 s ({:.0f} por fotograma; {} repiten la clave "
        "del anterior) y {} vkCmdSet* ({:.3f} por dibujo; {} veces todo: bufer nuevo, cielo o pase que olvida el "
        "pipeline) | cara {}, frente {}, topologia {}, reinicio {}, sesgo {}, prueba Z {}, escritura Z {}, funcion Z "
        "{}, estencil {}, operaciones de estencil {} | mezcla {}, ecuacion {}, mascaras {} | pipelines creados: {} de "
        "siempre, {} EDS1/EDS2, {} EDS3, {} con los dos | guardia: {} comprobados ({})",
        eds_disabled_ ? "APAGADO por la guardia" : EdsModeName(eds_mode_), draws,
        frames ? double(draws) / double(frames) : 0.0, repeated, calls,
        double(calls) / double(draws), n[kEdsAll], n[kEdsFace], n[kEdsFront], n[kEdsTopology],
        n[kEdsReset], n[kEdsBias], n[kEdsTestZ], n[kEdsWriteZ], n[kEdsFunctionZ], n[kEdsStencil],
        n[kEdsOpsStencil], n[kEdsActiveBlend], n[kEdsEquation], n[kEdsMask], pipelines_per_mode_[0],
        pipelines_per_mode_[kEds12], pipelines_per_mode_[kEds3], pipelines_per_mode_[kEds12 | kEds3],
        eds_checked_, eds_checked_ >= kEdsToCheck ? "guardia superada" : "comprobando");
  }

  // Phase 2. The blend operation of a register field, like the lambda in RellenarEstadoFijo.
  VkBlendOp BlendOperation(uint32_t op) {
    switch (op) {
      case 1:
        return VK_BLEND_OP_SUBTRACT;
      case 2:
        return VK_BLEND_OP_MIN;
      case 3:
        return VK_BLEND_OP_MAX;
      case 4:
        Warn(52, "mezcla con resta inversa: se usa resta");
        return VK_BLEND_OP_SUBTRACT;
      default:
        return VK_BLEND_OP_ADD;
    }
  }

  VkPipeline PipelineDe(const PipelineKey& key, const VertexInput& entry,
                        const DrawRequest& p) {
    /*
     * One-entry shortcut.
     *
     * There are 105 pipelines in a whole race and 2,345 draws per frame, so consecutive draws almost always
     * repeat the key. The full lookup is an XXH3 of 80 bytes + an integer division by libstdc++'s prime
     * bucket count + two pointer hops (bucket and node, ~96 B) = 2-3 cache misses, all to end up comparing
     * the same 80 bytes this compares. Here there is just one memcmp on memory that is already hot.
     */
    if (last_valid_key_ && std::memcmp(&last_key_, &key, sizeof(key)) == 0) {
      return last_pipeline_;
    }
    const uint64_t fingerprint = XXH3_64bits(&key, sizeof(key));
    // The direct-mapped cache (nfsmw_nativo_pipelines_directa) before the map.
    PipelineSlot& slot = pipeline_slots_[fingerprint & (kPipelineSlots - 1)];
    if (direct_pipelines_ && slot.pipeline != VK_NULL_HANDLE &&
        std::memcmp(&slot.key, &key, sizeof(key)) == 0) {
      const uint64_t n = ++hits_direct_pipelines_;
      if (!(n <= kPipelinesToCheck || (n & 4095) == 0) ||
          CheckPipelineSlot(fingerprint, key, slot.pipeline, n)) {
        last_key_ = key;
        last_pipeline_ = slot.pipeline;
        last_valid_key_ = true;
        return slot.pipeline;
      }
      // the guard saw a difference: this draw goes through the map, as before
    }
    if (direct_pipelines_) {
      ++failures_direct_pipelines_;
    }
    if (const auto it = pipelines_.find(fingerprint); it != pipelines_.end()) {
      if (std::memcmp(&it->second.first, &key, sizeof(key)) == 0) {
        last_key_ = key;
        last_pipeline_ = it->second.second;
        last_valid_key_ = true;
        if (direct_pipelines_) {  // the slot keeps the map's pair
          slot.key = key;
          slot.pipeline = it->second.second;
        }
        return it->second.second;
      }
      Warn(50, "colision de huellas de pipeline");
      return VK_NULL_HANDLE;
    }
    const VkShaderModule vs = ModuleOf(*p.vs);
    VkShaderModule ps = VK_NULL_HANDLE;
    if (key.ps && (key.specialization & kSpecGlowNatural)) {
      ps = VariantModule(1, kSpirvGlowEnergy, sizeof(kSpirvGlowEnergy));
    } else if (key.ps && (key.specialization & kSpecGlowSmooth)) {
      ps = VariantModule(2, kSpirvGlowSmooth, sizeof(kSpirvGlowSmooth));
    } else if (key.ps && (key.specialization & kSpecOnlyAlpha)) {
      ps = AlphaOnlyModule(*p.ps);
    } else if (key.ps && (key.specialization & kSpecZEarly)) {
      ps = EarlyModuleZ(*p.ps);
    } else if (key.ps) {
      ps = ModuleOf(*p.ps);
    }
    if (vs == VK_NULL_HANDLE || (key.ps && ps == VK_NULL_HANDLE)) {
      Reject(51, "no se pudo crear un modulo de shader");
      return VK_NULL_HANDLE;
    }
    // The VkGraphicsPipelineCreateInfo lives in CrearPipelineVulkan, the same function the prewarm uses from
    // its thread; what surrounds it stays here, as it was.
    uint32_t n_colores = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const auto creation_start = std::chrono::steady_clock::now();
    if (CreateVulkanPipeline(key, entry, vs, ps, rp_pass_, true, pipeline, n_colores) != VK_SUCCESS) {
      Reject(53, "no se pudo crear un pipeline");
      pipeline = VK_NULL_HANDLE;
    } else {
      ++pipelines_without_save_;
    }
    const uint64_t creation_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - creation_start)
                                              .count());
    ns_pipelines_ += creation_ns;
    TrackPipelineCreated(key, entry, p, pipeline, creation_ns);  // List and measurement
    pipelines_.emplace(fingerprint, std::make_pair(key, pipeline));
    ++pipelines_per_mode_[key.fill2 & 3];  // Dynamic state report
    if (direct_pipelines_ && pipeline != VK_NULL_HANDLE) {  // the same pair as the map
      slot.key = key;
      slot.pipeline = pipeline;
    }
    if (pipelines_.size() <= 64) {
      REXLOG_INFO("[nativo] C6: pipeline {} (VS n{} PS n{}, topologia {}, {} colores, "
                  "profundidad {:08X}, mezcla {:08X}, especializacion {})",
                  pipelines_.size(), p.vs->number, key.ps ? int(p.ps->number) : -1,
                  key.topo, n_colores,
                  key.depth_value, key.blend[0], key.specialization);
    }
    return pipeline;
  }

  /*
   * The Vulkan pipeline for a key. It is PipelineDe's usual code, moved here without changes except for
   * the render pass (now an argument) and avisar, so the prewarm can create from its thread exactly the
   * same VkGraphicsPipelineCreateInfo as the ring. It only reads its arguments, layout_pipeline_ and the
   * pipeline cache, which Vulkan synchronizes internally; avisar = false outside the ring.
   */
  VkResult CreateVulkanPipeline(const PipelineKey& key, const VertexInput& entry, VkShaderModule vs,
                               VkShaderModule ps, VkRenderPass pass, bool warn, VkPipeline& pipeline,
                               uint32_t& n_output_colors) {
    const VkSpecializationMapEntry map_table{0, 0, sizeof(uint32_t)};
    const VkSpecializationInfo specialization{1, &map_table, sizeof(uint32_t), &key.specialization};
    VkPipelineShaderStageCreateInfo stages[3]{};
    for (auto& stage_idx : stages) {
      stage_idx.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stage_idx.pName = "main";
      stage_idx.pSpecializationInfo = &specialization;
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;
    // AirborneNX: rectangle lists add the geometry stage (after the others in the array; order is free).
    uint32_t stage_count = key.ps ? 2 : 1;  // mode 5: VS only
    if ((key.specialization & kSpecRectList) && rect_list_gs_ != VK_NULL_HANDLE) {
      stages[stage_count].stage = VK_SHADER_STAGE_GEOMETRY_BIT;
      stages[stage_count].module = rect_list_gs_;
      stages[stage_count].pSpecializationInfo = nullptr;
      ++stage_count;
    }

    std::vector<VkVertexInputBindingDescription> bindings;
    for (uint32_t i = 0; i < entry.bindings.size(); ++i) {
      bindings.push_back({i, entry.bindings[i].stride_bytes, VK_VERTEX_INPUT_RATE_VERTEX});
    }
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (const VertexAttribute& a : entry.attributes) {
      attributes.push_back({a.loc_index, a.bind_index, a.format_id, a.offset});
    }
    VkPipelineVertexInputStateCreateInfo vertices{};
    vertices.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertices.vertexBindingDescriptionCount = uint32_t(bindings.size());
    vertices.pVertexBindingDescriptions = bindings.data();
    vertices.vertexAttributeDescriptionCount = uint32_t(attributes.size());
    vertices.pVertexAttributeDescriptions = attributes.data();

    // The fixed state is filled in by RellenarEstadoFijo: it is the usual code, moved there without
    // changing any computation, so the guards of the canonical key and of dynamic state compare with what
    // really goes into the VkGraphicsPipelineCreateInfo.
    PipelineFixedState fixed;
    FillFixedState(key, fixed, warn);  // notify
    const VkPipelineInputAssemblyStateCreateInfo& ensamblado = fixed.ensamblado;
    const VkPipelineViewportStateCreateInfo& view = fixed.view;
    const VkPipelineRasterizationStateCreateInfo& rasterizado = fixed.rasterizado;
    const VkPipelineMultisampleStateCreateInfo& sampling = fixed.sampling;
    const VkPipelineDepthStencilStateCreateInfo& depth_value = fixed.depth_value;
    const VkPipelineColorBlendStateCreateInfo& blend = fixed.blend;
    const uint32_t n_colores = fixed.n_colores;

    // Dynamic state phases 1 and 2. Their pipelines (flagged in relleno2) also declare as dynamic what is
    // now set with vkCmdSet*; their lookup key already has it zeroed (ClaveDeBusqueda).
    VkDynamicState dynamic_items[24] = {
        VK_DYNAMIC_STATE_VIEWPORT,          VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,   VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_DEPTH_BIAS};
    uint32_t n_dynamic = 7;
    if (key.fill2 & kEds12) {
      for (const VkDynamicState state : kEds12Dynamic) {
        dynamic_items[n_dynamic++] = state;
      }
    }
    if (key.fill2 & kEds3) {
      for (const VkDynamicState state : kEds3Dynamic) {
        dynamic_items[n_dynamic++] = state;
      }
    }
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = n_dynamic;
    dynamic.pDynamicStates = dynamic_items;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = stage_count;
    info.pStages = stages;
    info.pVertexInputState = &vertices;
    info.pInputAssemblyState = &ensamblado;
    info.pViewportState = &view;
    info.pRasterizationState = &rasterizado;
    info.pMultisampleState = &sampling;
    info.pDepthStencilState = &depth_value;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout_pipeline_;
    info.renderPass = pass;  // the pass's (ring) or a compatible one (prewarm)
    info.basePipelineIndex = -1;
    n_output_colors = n_colores;
    pipeline = VK_NULL_HANDLE;
    return dfn_.vkCreateGraphicsPipelines(device_, cache_pipelines_, 1, &info, nullptr, &pipeline);
  }

  // --- Pipeline prewarming (nfsmw_nativo_pipelines_precalentar) ---------------------------
  /*
   * Why. A pipeline that is not in the Vulkan cache is compiled on the fly on the ring: 68-159 ms each on
   * the console. In a first race after the library and the key had changed there were 56 (5.2 s of
   * stutter at the start). It happens the first time after any change to the library, the driver or the
   * key, and on a fresh install.
   *
   * How. The ring records every pipeline it creates (AnotarPipelineCreado) and the list is saved with the
   * cache (GuardarCachePipelines). In the next session, as soon as the library is loaded
   * (IntentarPrecalentar), a lowest-priority thread (BuclePrecalentado) walks the list in creation order
   * (the menu first) and recreates each pipeline with CrearPipelineVulkan, the same function the ring
   * uses, with modules from the same SPIR-V (the same variants and fallbacks as PipelineDe) and a
   * compatible render pass (CrearPase, the same formats); then destroys it. The Vulkan cache keeps the
   * compiled shaders, so when the ring asks for it, it comes from the cache (0-2 ms, like the 8 of the
   * menu). The ring changes nothing of what it draws: it only reads two counters of the thread for the
   * report and the guard.
   *
   * Guard. The thread's pipelines are destroyed unused: they cannot change the image. What can fail is
   * that they are not the ones the ring asks for. The report measures how long the ring takes to create
   * the listed pipelines the thread already prewarmed: if at least half of 8 or more take as long as a
   * compiled one (20 ms or more), it writes DIFERENCIA and stops the thread. Records whose shaders are no
   * longer in the library, or from another dynamic state mode, are skipped.
   */
  enum : uint8_t { kPendingList = 0, kPrewarmedList = 1, kListWithoutShader = 2, kFailedList = 3,
                   kModeOtherList = 4 };
  static constexpr uint64_t kNsCompilado = 5000000;  // more than this: really compiled (a cache hit is 0-2 ms)
  static constexpr uint64_t kSlowNs = 20000000;     // the ring creating one already prewarmed: mismatch

  // At start-up (Inicializar): the previous session's list, before the thread exists. It is not a
  // separate file: it is the part of the pipelines file that CargarCachePipelines read.
  void LoadPipelinesList() {
    const std::filesystem::path file_path = PipelinesFilePath();
    std::vector<uint8_t> payload = std::move(read_list_);
    read_list_ = {};
    if (payload.size() < kListHeader ||
        payload.size() > kListHeader + kMaxListRegs * sizeof(PipelineRecord)) {
      payload.clear();
    }
    const char* reason = nullptr;
    uint32_t hdr[4] = {};
    if (payload.empty()) {
      reason = "no hay lista: se empieza en esta sesion";
    } else {
      std::memcpy(hdr, payload.data(), sizeof(hdr));
      if (hdr[0] != kPipelinesListMagic || hdr[1] != kPipelinesListVersion ||
          hdr[2] != sizeof(PipelineRecord) ||
          payload.size() != kListHeader + size_t(hdr[3]) * sizeof(PipelineRecord)) {
        reason = "lista de otra version o danada: se empieza de cero";
      }
    }
    if (!reason) {
      file_list_.reserve(hdr[3]);
      for (uint32_t i = 0; i < hdr[3]; ++i) {
        PipelineRecord r;
        std::memcpy(&r, payload.data() + kListHeader + size_t(i) * sizeof(PipelineRecord), sizeof(r));
        if (r.n_attributes > PipelineRecord::kMaxAttributes || r.n_bindings > PipelineRecord::kMaxBindings) {
          continue;
        }
        if (list_index_.emplace(XXH3_64bits(&r.key, sizeof(r.key)), file_list_.size()).second) {
          file_list_.push_back(r);
        }
      }
      list_written_ = std::move(payload);  // the writer thread rewrites it unchanged if only the cache changes
    }
    list_state_.assign(file_list_.size(), kPendingList);
    REXLOG_INFO("[nativo] C6 precalentado (build 186): {} pipelines en la lista de {}{}{}", file_list_.size(),
                file_path.string(), reason ? ": " : "", reason ? reason : "");
  }

  // Ring only (GuardarCachePipelines): the file's list without the records the thread found missing their
  // shaders, followed by this session's new ones. Above kMaxRegistrosLista the oldest are dropped.
  std::vector<uint8_t> SerializePipelinesList() const {
    const size_t hasta = prewarmed_until_.load(std::memory_order_acquire);
    std::vector<const PipelineRecord*> regs;
    regs.reserve(file_list_.size() + session_list_.size());
    for (size_t i = 0; i < file_list_.size(); ++i) {
      if (i < hasta && list_state_[i] == kListWithoutShader) {
        continue;
      }
      regs.push_back(&file_list_[i]);
    }
    for (const PipelineRecord& r : session_list_) {
      regs.push_back(&r);
    }
    if (regs.size() > kMaxListRegs) {
      regs.erase(regs.begin(), regs.begin() + std::ptrdiff_t(regs.size() - kMaxListRegs));
    }
    std::vector<uint8_t> payload(kListHeader + regs.size() * sizeof(PipelineRecord));
    const uint32_t hdr[4] = {kPipelinesListMagic, kPipelinesListVersion, uint32_t(sizeof(PipelineRecord)),
                                  uint32_t(regs.size())};
    std::memcpy(payload.data(), hdr, sizeof(hdr));
    for (size_t i = 0; i < regs.size(); ++i) {
      std::memcpy(payload.data() + kListHeader + i * sizeof(PipelineRecord), regs[i], sizeof(PipelineRecord));
    }
    return payload;
  }

  // Ring only (PipelineDe), with every pipeline it creates: the measurement for the report and the guard,
  // and new ones go to the list.
  void TrackPipelineCreated(const PipelineKey& key, const VertexInput& entry, const DrawRequest& p,
                            VkPipeline pipeline, uint64_t ns) {
    const uint64_t fingerprint = XXH3_64bits(&key, sizeof(key));
    if (const auto it = list_index_.find(fingerprint); it != list_index_.end()) {
      const size_t j = it->second;
      if (j < file_list_.size() && j < prewarmed_until_.load(std::memory_order_acquire) &&
          list_state_[j] == kPrewarmedList) {
        ++prewarmed_ring_;
        prewarmed_ring_ns_ += ns;
        if (ns >= kSlowNs) {
          ++slow_prewarmed_ring_;
        }
      } else {
        ++list_ring_;
        list_ring_ns_ += ns;
      }
      return;
    }
    ++new_ring_;
    new_ring_ns_ += ns;
    if (pipeline == VK_NULL_HANDLE || !p.vs || !p.vs->shader || (key.ps && (!p.ps || !p.ps->shader)) ||
        entry.attributes.size() > PipelineRecord::kMaxAttributes ||
        entry.bindings.size() > PipelineRecord::kMaxBindings ||
        file_list_.size() + session_list_.size() >= kMaxListRegs) {
      return;
    }
    PipelineRecord r;
    r.key = key;
    r.vs_fingerprint = p.vs->shader->fingerprint;
    r.ps_fingerprint = key.ps ? p.ps->shader->fingerprint : 0;
    r.n_attributes = uint32_t(entry.attributes.size());
    for (uint32_t k = 0; k < r.n_attributes; ++k) {
      const VertexAttribute& a = entry.attributes[k];
      r.attributes[k] = {a.loc_index, a.bind_index, uint32_t(a.format_id), a.offset};
    }
    r.n_bindings = uint32_t(entry.bindings.size());
    for (uint32_t k = 0; k < r.n_bindings; ++k) {
      r.strides[k] = entry.bindings[k].stride_bytes;
    }
    list_index_.emplace(fingerprint, SIZE_MAX);  // from this session: not recorded again
    session_list_.push_back(r);
    ++list_without_save_;
    ++ring_to_list_;
  }

  // Ring only, on each submission: starts the thread once, as soon as the library is loaded.
  void TryPrewarm() {
    if (prewarmed_decided_) {
      return;
    }
    if (file_list_.empty() || cache_pipelines_ == VK_NULL_HANDLE ||
        !REXCVAR_GET(nfsmw_nativo_pipelines_precalentar)) {
      prewarmed_decided_ = true;
      if (!file_list_.empty()) {
        REXLOG_INFO("[nativo] C6 precalentado (build 186): no se precalienta ({})",
                    cache_pipelines_ == VK_NULL_HANDLE ? "sin cache de pipelines"
                                                       : "nfsmw_nativo_pipelines_precalentar = false");
      }
      return;
    }
    const NativeShaders* library = ActiveLibrary();
    if (!library || !library->loaded() || layout_pipeline_ == VK_NULL_HANDLE) {
      return;  // not yet: check again on the next submission
    }
    prewarmed_decided_ = true;
    prewarmed_library_ = library;
    prewarmed_eds_ = eds_mode_;  // the ring would not request those of another dynamic state mode
    prewarmed_start_ = std::chrono::steady_clock::now();
    try {
      prewarmed_thread_ = std::thread([this] { PrewarmedLoop(); });
      REXLOG_INFO("[nativo] C6 precalentado (build 186): hilo creado para {} pipelines de la lista",
                  file_list_.size());
    } catch (const std::system_error& error) {
      REXLOG_WARN("[nativo] C6 precalentado (build 186): no se pudo crear el hilo ({}); sin precalentar", error.what());
    }
  }

  // The thread. It only reads lista_archivo_, the library, the layout and the cache; it writes
  // estado_lista_[i] before publishing precalentado_hasta_ = i + 1, and its atomic counters. Its modules
  // and render pass are its own and it destroys them when done.
  void PrewarmedLoop() {
    rex::thread::set_current_thread_name("NFSMW precalentado de pipelines");
    int32_t priority = -1;
#if REX_PLATFORM_SWITCH
    // The lowest priority the system accepts, and never above the guest's (0x3B): compiling at the priority
    // threads are born with would take the core from the game and the ring. If none is accepted, nothing is
    // compiled.
    for (const int32_t candidate_item : {0x3F, 0x3E, 0x3D, 0x3C, 0x3B}) {
      if (RexSwitchSetCurrentThreadPriorityOk(int(candidate_item))) {
        priority = candidate_item;
        break;
      }
    }
    if (priority < 0) {
      REXLOG_WARN("[nativo] C6 precalentado (build 186): el sistema no acepta ninguna prioridad de 0x3B a 0x3F: sin "
                  "precalentar");
      prewarmed_finished_.store(true, std::memory_order_release);
      return;
    }
#endif
    prewarmed_priority_.store(priority, std::memory_order_relaxed);
    const NativeShaders& library = *prewarmed_library_;
    std::unordered_map<uint64_t, VkShaderModule> modules;  // (variante << 32) | numero
    std::unordered_map<uint64_t, VkRenderPass> passes;       // by formats
    const auto create = [&](const uint32_t* spirv, size_t bytes) {
      VkShaderModuleCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      info.codeSize = bytes;
      info.pCode = spirv;
      VkShaderModule spv_module = VK_NULL_HANDLE;
      if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &spv_module) != VK_SUCCESS) {
        spv_module = VK_NULL_HANDLE;
      }
      return spv_module;
    };
    // ModuloDe: the library's SPIR-V as is.
    const auto normal = [&](const ShaderEntry& e) {
      auto it = modules.find(e.number);
      if (it == modules.end()) {
        it = modules.emplace(e.number, create(e.shader->spirv.data(), e.shader->spirv.size() * sizeof(uint32_t))).first;
      }
      return it->second;
    };
    // The pixel shader PipelineDe would choose, in the same order: the bright pass variants
    // (ModuloVariante), without color writes (ModuloSoloAlfa) and with early tests (ModuloZTemprana); the
    // latter two with the same fallback: if it cannot be pruned or patched, the normal one.
    const auto pixel = [&](const PipelineKey& key, const ShaderEntry& e) {
      const uint32_t spec = key.specialization;
      const uint32_t variant = (spec & kSpecGlowNatural) ? 1
                                : (spec & kSpecGlowSmooth) ? 2
                                : (spec & kSpecOnlyAlpha)        ? 3
                                : (spec & kSpecZEarly)       ? 4
                                                                : 0;
      if (variant == 0) {
        return normal(e);
      }
      const uint64_t module_key = (uint64_t(variant) << 32) | (variant <= 2 ? 0xFFFFFFFFull : uint64_t(e.number));
      auto it = modules.find(module_key);
      if (it == modules.end()) {
        VkShaderModule spv_module = VK_NULL_HANDLE;
        if (variant == 1) {
          spv_module = create(kSpirvGlowEnergy, sizeof(kSpirvGlowEnergy));
        } else if (variant == 2) {
          spv_module = create(kSpirvGlowSmooth, sizeof(kSpirvGlowSmooth));
        } else if (variant == 3) {
          uint32_t removed = 0;
          const std::vector<uint32_t> pruned = PruneColorWrites(e.shader->spirv, removed);
          if (!pruned.empty()) {
            spv_module = create(pruned.data(), pruned.size() * sizeof(uint32_t));
          }
        } else {
          const char* reason = "";
          const std::vector<uint32_t> patched = WithEarlyTests(e.shader->spirv, reason);
          if (!patched.empty()) {
            spv_module = create(patched.data(), patched.size() * sizeof(uint32_t));
          }
        }
        it = modules.emplace(module_key, spv_module).first;
      }
      if (it->second != VK_NULL_HANDLE || variant <= 2) {
        return it->second;
      }
      return normal(e);
    };
    uint32_t done_items = 0, compilados = 0, without_shader = 0, other_mode = 0, failed_items = 0;
    uint64_t ns_compilados = 0;
    const size_t n = file_list_.size();
    for (size_t i = 0; i < n; ++i) {
      if (prewarmed_stop_.load(std::memory_order_relaxed)) {
        break;
      }
      const PipelineRecord& r = file_list_[i];
      uint8_t state = kFailedList;
      const ShaderEntry* vs = r.key.vs ? library.ByNumber(r.key.vs - 1) : nullptr;
      const ShaderEntry* ps = r.key.ps ? library.ByNumber(r.key.ps - 1) : nullptr;
      if (!vs || !vs->vertices || !vs->shader || vs->shader->fingerprint != r.vs_fingerprint ||
          (r.key.ps && (!ps || ps->vertices || !ps->shader || ps->shader->fingerprint != r.ps_fingerprint))) {
        state = kListWithoutShader;
        ++without_shader;
      } else if (r.key.fill2 != prewarmed_eds_) {
        state = kModeOtherList;
        ++other_mode;
      } else {
        const VkShaderModule vs_module = normal(*vs);
        const VkShaderModule ps_module = r.key.ps ? pixel(r.key, *ps) : VK_NULL_HANDLE;
        const uint64_t pass_key = XXH3_64bits(r.key.formats, sizeof(r.key.formats));
        auto it_pass = passes.find(pass_key);
        if (it_pass == passes.end()) {
          it_pass = passes.emplace(pass_key, CreatePass(r.key.formats, kReadLoad)).first;
        }
        VertexInput entry;
        for (uint32_t k = 0; k < r.n_attributes; ++k) {
          const AttributeRecord& a = r.attributes[k];
          entry.attributes.push_back({a.loc_index, a.bind_index, VkFormat(a.format_id), a.offset});
        }
        for (uint32_t k = 0; k < r.n_bindings; ++k) {
          entry.bindings.push_back({0, r.strides[k]});
        }
        if (vs_module != VK_NULL_HANDLE && (!r.key.ps || ps_module != VK_NULL_HANDLE) &&
            it_pass->second != VK_NULL_HANDLE) {
          VkPipeline pipeline = VK_NULL_HANDLE;
          uint32_t n_colores = 0;
          const auto t0 = std::chrono::steady_clock::now();
          const VkResult result =
              CreateVulkanPipeline(r.key, entry, vs_module, ps_module, it_pass->second, false, pipeline, n_colores);
          const uint64_t ns = uint64_t(
              std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
          if (result == VK_SUCCESS && pipeline != VK_NULL_HANDLE) {
            dfn_.vkDestroyPipeline(device_, pipeline, nullptr);
            state = kPrewarmedList;
            ++done_items;
            if (ns >= kNsCompilado) {
              ++compilados;
              ns_compilados += ns;
            }
          } else {
            ++failed_items;
          }
        } else {
          ++failed_items;
        }
      }
      list_state_[i] = state;
      prewarmed_done_.store(done_items, std::memory_order_relaxed);
      prewarmed_compiled_.store(compilados, std::memory_order_relaxed);
      prewarmed_compiled_ns_.store(ns_compilados, std::memory_order_relaxed);
      prewarmed_without_shader_.store(without_shader, std::memory_order_relaxed);
      prewarmed_other_mode_.store(other_mode, std::memory_order_relaxed);
      prewarmed_failed_.store(failed_items, std::memory_order_relaxed);
      prewarmed_until_.store(i + 1, std::memory_order_release);
    }
    for (const auto& [key, spv_module] : modules) {
      if (spv_module != VK_NULL_HANDLE) {
        dfn_.vkDestroyShaderModule(device_, spv_module, nullptr);
      }
    }
    for (const auto& [key, pass] : passes) {
      if (pass != VK_NULL_HANDLE) {
        dfn_.vkDestroyRenderPass(device_, pass, nullptr);
      }
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - prewarmed_start_).count();
    REXLOG_INFO("[nativo] C6 precalentado (build 186): {} en {:.1f} s, prioridad {:#x}: {} de {} pipelines "
                "precalentados, {} compilados de verdad ({:.0f} ms, {:.1f} ms cada uno; el resto ya estaba en la cache), "
                "{} saltados porque la biblioteca ya no tiene sus shaders, {} de otro modo de estado dinamico y {} "
                "fallidos",
                prewarmed_stop_.load(std::memory_order_relaxed) ? "parado" : "terminado", secs, priority,
                done_items, n, compilados, double(ns_compilados) / 1e6,
                compilados ? double(ns_compilados) / 1e6 / double(compilados) : 0.0, without_shader, other_mode, failed_items);
    prewarmed_finished_.store(true, std::memory_order_release);
  }

  // Ring only, every 10 s if anything changed: the thread's progress, what the ring had to create, and
  // the guard (see above).
  void PrewarmedReport(std::chrono::steady_clock::time_point now_time) {
    if (now_time - prewarmed_report_ < std::chrono::seconds(10)) {
      return;
    }
    prewarmed_report_ = now_time;
    if (!prewarmed_difference_ && prewarmed_ring_ >= 8 &&
        slow_prewarmed_ring_ * 2 >= prewarmed_ring_) {
      prewarmed_difference_ = true;
      prewarmed_stop_.store(true, std::memory_order_relaxed);
      REXLOG_ERROR("[nativo] C6 precalentado (build 186): DIFERENCIA: de {} pipelines que el anillo pidio ya "
                   "precalentados, {} tardaron como uno compilado (20 ms o mas): lo que precalienta el hilo no es lo "
                   "que pide el anillo. Se para el hilo; no cambia nada mas",
                   prewarmed_ring_, slow_prewarmed_ring_);
    }
    const size_t hasta = prewarmed_until_.load(std::memory_order_acquire);
    const bool done_flag = prewarmed_finished_.load(std::memory_order_acquire);
    const uint64_t changes = uint64_t(hasta) + prewarmed_ring_ + list_ring_ + new_ring_ +
                             (done_flag ? 1 : 0) + (prewarmed_decided_ ? 1 : 0);
    if (changes == prewarmed_changes_previous_) {
      return;
    }
    prewarmed_changes_previous_ = changes;
    const auto media = [](uint64_t ns, uint64_t k) { return k ? double(ns) / double(k) / 1e6 : 0.0; };
    const uint32_t compilados = prewarmed_compiled_.load(std::memory_order_relaxed);
    NFSMW_RING_REPORT(
        "[nativo] C6 precalentado de pipelines (build 186): lista de {}; hilo {} (prioridad {:#x}): {} recorridos, {} "
        "precalentados, {} compilados de verdad ({:.0f} ms), {} sin sus shaders, {} de otro modo, {} fallidos | el "
        "anillo creo {} de la lista ya precalentados ({:.1f} ms de media, {} lentos), {} de la lista sin precalentar "
        "({:.1f} ms de media) y {} nuevos ({:.1f} ms de media; {} a la lista)",
        file_list_.size(),
        !prewarmed_thread_.joinable() ? (prewarmed_decided_ ? "sin lanzar" : "esperando a la biblioteca")
        : done_flag                    ? (prewarmed_stop_.load(std::memory_order_relaxed) ? "parado" : "terminado")
                                       : "en marcha",
        uint32_t(prewarmed_priority_.load(std::memory_order_relaxed)), hasta,
        prewarmed_done_.load(std::memory_order_relaxed), compilados,
        double(prewarmed_compiled_ns_.load(std::memory_order_relaxed)) / 1e6,
        prewarmed_without_shader_.load(std::memory_order_relaxed), prewarmed_other_mode_.load(std::memory_order_relaxed),
        prewarmed_failed_.load(std::memory_order_relaxed), prewarmed_ring_,
        media(prewarmed_ring_ns_, prewarmed_ring_), slow_prewarmed_ring_, list_ring_,
        media(list_ring_ns_, list_ring_), new_ring_, media(new_ring_ns_, new_ring_),
        ring_to_list_);
  }

  void StopPrewarmed() {
    prewarmed_stop_.store(true, std::memory_order_relaxed);
    if (prewarmed_thread_.joinable()) {
      prewarmed_thread_.join();  // at most, as long as the pipeline being compiled takes
    }
  }

  void DestroyImage(NativeImage& img) {
    if (img.view != VK_NULL_HANDLE) dfn_.vkDestroyImageView(device_, img.view, nullptr);
    if (img.img != VK_NULL_HANDLE) dfn_.vkDestroyImage(device_, img.img, nullptr);
    // The pool chunk is returned after destroying the image that used it, and in that case `memoria` is
    // NULL on purpose: the memory belongs to a shared slab and is not freed on its own.
    if (img.block_pool != 0xFFFFFFFFu) textures_pool_.Dispose(img.block_pool);
    if (img.mem != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, img.mem, nullptr);
    img = NativeImage{};
  }

  struct Heap {
    uint32_t cap_bytes = 0;
    uint32_t next = 1;
    std::vector<uint32_t> free_items;
  };

  const VulkanDevice* vk_device_;
  const VulkanDevice::Functions& dfn_;
  VkDevice device_;
  rex::memory::Memory* mem_;
  TargetsContext* contexto_;
  BufferAddressFn buffer_address_ = nullptr;
  CopyImageFn copy_image_ = nullptr;  // resolved faces of the dynamic cubemaps

  VkBuffer upload_ = VK_NULL_HANDLE;
  VkDeviceMemory memory_upload_ = VK_NULL_HANDLE;
  uint32_t type_upload_ = UINT32_MAX;
  VkDeviceSize real_size_upload_ = 0;
  uint8_t* data_upload_ = nullptr;
  VkDeviceAddress address_upload_ = 0;
  VkDeviceSize used_upload_ = 0;
  bool coherent_upload_ = false;
  uint64_t upload_epoch_ = 0;
  uint64_t frame_ = 0;
  // Fast untiling and its guard (see LeerNivel).
  static constexpr uint32_t kLevelsToCheck = 200;  // it used to be 2000
  static constexpr uint32_t kCheckEvery = 64;   // after the first ones, by sampling
  uint64_t seen_levels_ = 0;
  uint64_t seen_commands_ = 0;
  int fast_tile_ = -1;
  uint32_t checked_levels_ = 0;
  uint32_t checked_commands_ = 0;
  std::vector<uint8_t> tile_check_;
  // Texture check budget (see PrepararTextura).
  static constexpr uint32_t kDeferralMax = 8;
  int32_t fingerprints_budget_ = -1;
  uint64_t fingerprints_frame_ = UINT64_MAX;
  uint64_t frame_fingerprint_bytes_ = 0;
  // Sampled recheck of stable textures (see PrepararTextura and HuellaMuestra).
  static constexpr uint64_t kSamplesToCheck = 3000;  // the first ones, with both hashes: the guard
  int32_t fingerprints_sampling_ = -1;      // -1 cvar not read; 0 off; N: 1 in N rechecks is full
  uint64_t checked_samples_ = 0;  // stable rechecks with both hashes, since start-up
  uint64_t samples_with_two_ = 0;  // the same, since the last report line
  uint64_t hits_samples_ = 0;     // decided on the sample alone
  uint64_t distinct_samples_ = 0;    // the sample changed and the full path followed
  uint64_t sample_bytes_ = 0;
  uint64_t sample_saved_bytes_ = 0;
  std::chrono::steady_clock::time_point sampling_report_{};
  // Vertex copies on a separate thread (EncolarCopia). Power-of-2 capacity.
  const bool active_copies_ = REXCVAR_GET(nfsmw_nativo_subidas_hilo);
  std::array<CopyJob, 8192> copies_{};
  std::atomic<size_t> written_copies_{0};
  // How many copies are done, whether by the thread or the ring (see EsperarSubidas). Outside
  // EsperarSubidas they finish in order, and then it is also the index of the first one not done.
  std::atomic<size_t> done_copies_{0};
  bool sleeping_copies_ = false;   // under copias_mutex_: the copy thread sleeps or is about to
  bool waiting_copies_ = false;   // under copias_mutex_: the ring waits in EsperarSubidas
  std::mutex mutex_copies_;
  std::condition_variable cv_copies_;
  std::condition_variable cv_done_copies_;
  bool stop_copies_ = false;  // con copias_mutex_
  std::thread thread_copies_;
  uint64_t copies_in_line_ = 0;
  uint64_t copies_waits_ = 0;
  uint64_t copies_waiting_ns_ = 0;
  std::atomic<size_t> progress_copies_{0};  // copies done by the thread, one by one (diagnostic)
  std::thread::id producer_copies_;
  bool warned_thread_other_copies_ = false;
  // The ring's help (nfsmw_nativo_subidas_ayuda) and its guard. See EsperarSubidas.
  const bool requested_help_copies_ = REXCVAR_GET(nfsmw_nativo_subidas_ayuda);
  int32_t phase_copies_ = 0;         // FaseCopias; ring only
  bool copies_without_thread_ = false;    // off after a DIFERENCIA: copies go on the ring; ring only
  std::atomic<size_t> taken_copies_{0};  // how many were taken, in order, by the thread or the ring (with CAS)
  std::mutex mutex_chunk_copies_;          // held by the thread while it copies what it took (lends priority)
  std::array<std::atomic<uint32_t>, 8192> marks_copies_{};  // index + 1 of the last copy done in each slot
  std::atomic<uint64_t> n_thread_copies_{0};  // copies done by the thread, for the report
  std::atomic<uint64_t> thread_copies_bytes_{0};
  size_t verified_copies_ = 0;  // marks checked up to here; ring only
  uint64_t checked_copies_ = 0;
  uint64_t peeking_waits_copies_ = 0;
  uint64_t assisted_copies_ = 0;
  uint64_t assisted_bytes_copies_ = 0;
  uint64_t helping_ns_copies_ = 0;
  uint64_t differences_copies_ = 0;
  struct CopiesReportFigures {  // the last 10 s (InformeCopias); ring only
    uint64_t queued = 0, queued_bytes = 0, in_line = 0, in_line_bytes = 0;
    uint64_t assisted = 0, assisted_bytes = 0, helping_ns = 0, waits_with_help = 0;
    uint64_t waits = 0, without_take = 0, chunk_waits = 0, chunk_ns = 0, worst_chunk_ns = 0;
    uint64_t ints_waits = 0, wait_ns = 0, worst_wait_ns = 0, checked = 0;
  };
  CopiesReportFigures inf_copies_{};
  std::chrono::steady_clock::time_point copies_report_{};
  uint64_t n_thread_copies_previous_ = 0;
  uint64_t thread_copies_previous_bytes_ = 0;
  struct UploadBuffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize real_size = 0;
    uint8_t* payload = nullptr;
    VkDeviceAddress address = 0;
  };
  // One per work slot (there are 3). With fewer than there are slots, two slots would share a buffer and
  // the CPU would write over what the GPU is still reading.
  std::array<UploadBuffer, kJobSlots> uploads_{};  // subida_* are the current slot's
  // Separate shared constants, CPU-cached (nfsmw_nativo_compartidas_cache).
  std::array<UploadBuffer, kJobSlots> bufs_shared_{};
  bool separately_shared_ = false;
  bool coherent_shared_ = true;
  uint32_t type_shared_ = 0;
  uint8_t* data_shared_ = nullptr;
  VkDeviceMemory memory_shared_ = VK_NULL_HANDLE;
  VkDeviceSize real_size_shared_ = 0;
  VkDeviceSize used_shared_ = 0;
  uint64_t published_bytes_shared_ = 0;

  std::array<VkDescriptorSetLayout, 4> layouts_{};
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, 4> sets_{};
  VkPipelineLayout layout_pipeline_ = VK_NULL_HANDLE;
  // Set 4, the constants through dynamic UBOs. One set per upload slot (each one is a VkBuffer).
  bool use_ubo_ = false;
  VkDeviceSize ubo_alignment_ = 256;
  VkDescriptorSetLayout layout_ubo_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ubo_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kJobSlots> sets_ubo_{};  // one per work slot
  uint32_t current_slot_ = 0;
  bool bound_ubo_ = false;
  uint32_t bound_ubo_slot_ = UINT32_MAX;
  std::array<uint32_t, 3> bound_offsets_ubo_{};
  // Set 4 by differences (ControlSet4, InformeSet4).
  std::array<uint64_t, 8> set4_changes_{};  // binds by what changes: bit 0 VS, bit 1 PS, bit 2 shared
  uint64_t set4_first_ = 0;              // binds after a new command buffer, a new slot or the sky
  uint64_t set4_draws_ = 0;
  std::array<uint64_t, 3> vs_reuploaded_{};  // constants uploaded again: by generation, by epoch, because they grow
  std::array<uint64_t, 3> ps_reuploaded_{};
  std::chrono::steady_clock::time_point set4_report_{};
  std::chrono::steady_clock::time_point set4_toggle_start_ = std::chrono::steady_clock::now();
  bool set4_request_ = true;
  bool set4_tracked_request_ = false;
#if REX_PLATFORM_SWITCH
  bool set4_warned_difference_ = false;
  uint64_t set4_nvk_previous_[9] = {};  // NVK counts at the previous report
#endif
  // The draw path in NVK (ControlDibujoNvk and InformeDibujoNvk). Ring only.
  bool nvk_app_preload_ = false;  // hand the pipeline to NVK after PipelineDe
  bool nvk_tracked_draw_ = false;
  bool nvk_requested_improvements_ = true;
  bool nvk_warned_disabled_[5] = {};
  std::chrono::steady_clock::time_point nvk_start_draw_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point nvk_report_{};
  uint64_t nvk_parts_previous_[16][2] = {};
  uint64_t nvk_counts_previous_[13] = {};
  uint64_t nvk_improvements_previous_[5][4] = {};
  int32_t alternar_ubo_s_ = 0;
  uint64_t sends_ = 0;
  uint64_t ubo_sends_ = 0;
  std::chrono::steady_clock::time_point ubo_start_ = std::chrono::steady_clock::now();
  std::array<Heap, 4> heaps_{};
  std::array<NativeImage, 3> empty_{};
  VkSampler empty_sampler_ = VK_NULL_HANDLE;
  bool prepared_empty_ = false;

  std::unordered_map<const ShaderEntry*, VkShaderModule> modules_;
  std::array<VkShaderModule, 3> variants_modules_{};  // 1 natural, 2 soft
  VkShaderModule rect_list_gs_ = VK_NULL_HANDLE;  // AirborneNX: shaders/rect_list_gs.hlsl
  // AirborneNX (airborne_edram_4x_alias): 4X surfaces drawn in 1X terms and rectangles without face culling.
  // Both are what the hardware/emulation do, and they only work together: the XDK clears the scene depth
  // through 640-pitch 4X rectangles, and UE3's depth restore rectangle comes with culling on.
  const bool edram_4x_alias_ = REXCVAR_GET(airborne_edram_4x_alias);
  std::array<bool, 3> created_variants_modules_{};
  int glow_tracked_ = 0;
  std::unordered_map<uint64_t, std::pair<PipelineKey, VkPipeline>> pipelines_;
  // One-entry shortcut for PipelineDe (see the comment there).
  PipelineKey last_key_{};
  VkPipeline last_pipeline_ = VK_NULL_HANDLE;
  bool last_valid_key_ = false;
  // direct cache in front of pipelines_ (nfsmw_nativo_pipelines_directa).
  struct PipelineSlot {
    PipelineKey key{};
    VkPipeline pipeline = VK_NULL_HANDLE;  // VK_NULL_HANDLE = casilla vacia
  };
  static constexpr size_t kPipelineSlots = 256;  // potencia de 2; 256 x 88 bytes = 22 KB contiguos
  std::array<PipelineSlot, kPipelineSlots> pipeline_slots_{};
  bool direct_pipelines_ = true;           // the cvar, once per frame
  bool direct_pipelines_disabled_ = false;  // the guard saw a difference
  uint64_t hits_direct_pipelines_ = 0;
  uint64_t failures_direct_pipelines_ = 0;
  uint64_t checked_direct_pipelines_ = 0;
  uint64_t hits_direct_pipelines_previous_ = 0;
  uint64_t failures_direct_pipelines_previous_ = 0;
  std::chrono::steady_clock::time_point direct_pipelines_report_{};
  static constexpr uint64_t kPipelinesToCheck = 200000;
  // nfsmw_nativo_contar_cambios_pipeline (ContarCambioPipeline and InformeCambiosPipeline).
  enum : uint32_t {
    kBindingsChange = 0,        // vkCmdBindPipeline de Dibujar contados
    kChangeAfterPass,           // of those, the first of a pass (EmpezarPase forgets the bound pipeline)
    kChangeFirst,            // no previous key in the buffer (new buffer or after the deferred sky)
    kIdenticalChange,           // the same key: only after EmpezarPase
    kShadersChange,            // another VS or PS
    kEntryChange,            // same shaders, different vertex input
    kFormatsChange,           // same shaders and input, different formats (another pass)
    kAlphaSpecChange,           // all the above equal; specialization only changes the alpha test (bits 1, 16-18)
    kEarlySpecZChange,      // ... the alpha test and the early Z (bit 20)
    kOtherSpecChange,           // ... other specialization bits
    kChangeWithoutEffect,          // state only, in bits PipelineDe does not read: effectively the same pipeline
    kChangeWithoutEffectAfterPass,  // ... and the first of a pass
    kStateOnlyChange,         // real state-only changes: what dynamic state would avoid
    kStateChangeAfterPass,     // ... and the first of a pass
    kEds12Change,              // ... without blending, masks or another topology class (EDS1/EDS2, core 1.3)
    kEds12ChangeAfterPass,      // ... and the first of a pass
    kTopologyField,           // per field, on the canonical state (one bind may change several)
    kTopologyClassField,
    kBlendField,
    kMasksField,
    kFieldZ,
    kStencilField,
    kFaceField,
    kResetField,
    kBiasField,
    kNChanges
  };
  std::array<uint64_t, kNChanges> pipeline_changes_{};
  PipelineKey bound_key_{};       // the last key bound in this command buffer
  bool valid_bound_key_ = false;   // false: new buffer, after the deferred sky, or not counting
  bool count_pipeline_changes_ = true;  // the cvar, once per frame (InformeCambiosPipeline)
  uint64_t draws_changes_previous_ = 0;
  uint64_t frames_changes_previous_ = 0;
  std::chrono::steady_clock::time_point changes_report_{};
  // Dynamic state, phase 0a (nfsmw_nativo_clave_canonica, ClaveDeBusqueda).
  static constexpr uint64_t kCanonicalToCheck = 200000;
  bool canonical_key_ = true;           // the cvar, once per command buffer (UsarRanura)
  bool canonical_key_disabled_ = false;  // the guard saw a difference
  bool valid_canonical_ = false;         // canonica_cruda_ -> canonica_resultado_ is the previous draw's
  PipelineKey raw_canonical_{};
  PipelineKey result_canonical_{};
  uint64_t changed_canonical_ = 0;      // keys Canonizar changes (not counting consecutive repeats)
  uint64_t checked_canonical_ = 0;
  uint64_t changed_canonical_previous_ = 0;
  uint64_t pipelines_canonical_previous_ = 0;
  std::chrono::steady_clock::time_point canonical_report_{};
  // Dynamic state, phase 0b (nfsmw_nativo_pipeline_entre_pases).
  bool pipeline_between_passes_ = true;  // the cvar, once per command buffer (UsarRanura)
  bool pipeline_between_tracked_passes_ = false;
  uint64_t passes_with_pipeline_ = 0;   // passes started with a bound pipeline that is kept
  uint64_t between_passes_passes_previous_ = 0;
  uint64_t between_kept_passes_previous_ = 0;
  std::chrono::steady_clock::time_point between_passes_report_{};
  // Dynamic state phases 1 and 2 (FijarEstadoDinamico, its guard and its report).
  enum : uint32_t { kEds12 = 1, kEds3 = 2 };  // bits of eds_modo_ and of ClavePipeline::relleno2
  static constexpr VkDynamicState kEds12Dynamic[10] = {
      VK_DYNAMIC_STATE_CULL_MODE,           VK_DYNAMIC_STATE_FRONT_FACE,
      VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,  VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
      VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,   VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
      VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,  VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
      VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP};
  static constexpr uint64_t kEdsToCheck = 200000;
  enum : uint32_t {
    kEdsFace = 0,
    kEdsFront,
    kEdsTopology,
    kEdsReset,
    kEdsBias,
    kEdsTestZ,
    kEdsWriteZ,
    kEdsFunctionZ,
    kEdsStencil,
    kEdsOpsStencil,
    kEdsActiveBlend,  // fase 2
    kEdsEquation,      // fase 2
    kEdsMask,       // fase 2
    kEdsAll,          // times everything is set (not a call)
    kEdsN
  };
  PFN_vkCmdSetCullMode set_face_ = nullptr;  // nucleo de Vulkan 1.3 (CargarEstadoDinamico)
  PFN_vkCmdSetFrontFace set_front_ = nullptr;
  PFN_vkCmdSetPrimitiveTopology set_topology_ = nullptr;
  PFN_vkCmdSetPrimitiveRestartEnable set_reset_ = nullptr;
  PFN_vkCmdSetDepthBiasEnable set_bias_ = nullptr;
  PFN_vkCmdSetDepthTestEnable set_z_test_ = nullptr;
  PFN_vkCmdSetDepthWriteEnable set_write_z_ = nullptr;
  PFN_vkCmdSetDepthCompareOp set_z_func_ = nullptr;
  PFN_vkCmdSetStencilTestEnable set_stencil_ = nullptr;
  PFN_vkCmdSetStencilOp set_ops_stencil_ = nullptr;
  bool eds12_available_ = false;  // API 1.3 and the ten functions
  bool eds_disabled_ = false;       // the guard saw a difference: for the rest of the session, everything in the pipeline
  bool eds_tracked_mode_ = false;
  uint32_t eds_mode_ = 0;          // kEds12 | kEds3 of this command buffer (UsarRanura; the guard sets it to 0)
  bool eds_valid_ = false;        // eds_grabado_ is what is set in this buffer (false: new buffer, sky or pass)
  EdsState eds_recorded_{};
  PipelineKey eds_key_{};      // raw key of the last state set
  uint64_t eds_draws_ = 0;       // draws with dynamic state
  uint64_t eds_repeated_ = 0;     // ... with the same raw key as the previous one: nothing to set or check
  uint64_t eds_checkable_ = 0;  // ... with something to set (what the guard counts)
  uint64_t eds_checked_ = 0;
  uint64_t eds_draws_previous_ = 0;
  uint64_t eds_repeated_previous_ = 0;
  uint64_t eds_frames_previous_ = 0;
  std::array<uint64_t, kEdsN> eds_calls_{};    // vkCmdSet* recorded per state since the last report
  std::array<uint64_t, 4> pipelines_per_mode_{};  // pipelines created per mode (relleno2) since start-up
  std::chrono::steady_clock::time_point eds_report_{};
  // Dynamic state, phase 2 (VK_EXT_extended_dynamic_state3).
  static constexpr VkDynamicState kEds3Dynamic[3] = {VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT,
                                                       VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT,
                                                       VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT};
  // The same mapping as kFactores in RellenarEstadoFijo (fields 2, 3 and 17-31 give ZERO).
  static constexpr VkBlendFactor kBlendFactors[32] = {
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_ONE,
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_SRC_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
      VK_BLEND_FACTOR_SRC_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      VK_BLEND_FACTOR_DST_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
      VK_BLEND_FACTOR_DST_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
      VK_BLEND_FACTOR_CONSTANT_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
      VK_BLEND_FACTOR_CONSTANT_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
      VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
  };
  PFN_vkCmdSetColorBlendEnableEXT set_blend_enabled_ = nullptr;
  PFN_vkCmdSetColorBlendEquationEXT set_equation_ = nullptr;
  PFN_vkCmdSetColorWriteMaskEXT set_mask_ = nullptr;
  bool eds3_available_ = false;  // the SDK enables the extension and its three features; the driver provides the functions
  std::unordered_map<uint64_t, VkRenderPass> passes_;
  std::unordered_map<uint64_t, VkFramebuffer> framebuffers_;
  // nfsmw_nativo_framebuffers_olvidan_vistas. The views of each cached framebuffer (by the same key), the
  // ones retired by the guard, and the counts for the warning.
  std::unordered_map<uint64_t, std::array<VkImageView, 5>> views_framebuffers_;
  std::vector<VkFramebuffer> fb_retired_;
  bool fb_retire_ = false;
  uint64_t fb_forgotten_views_ = 0;
  uint64_t fb_forgotten_ = 0;
  std::unordered_map<uint64_t, Texture> textures_;
  // Measurement only (nfsmw_nativo_diag_reutilizar): textures with a hash, by content key (content key ->
  // texture key; see AnotarContenidoTextura), and the counts for the 10 s report.
  std::unordered_multimap<uint64_t, uint64_t> alive_per_content_;
  uint64_t distinct_contents_ = 0;
  int32_t diag_reuse_ = -1;  // -1 = cvar not read
  uint64_t reuse_new_ = 0;
  uint64_t reuse_match_ = 0;
  uint64_t reuse_cold_ = 0;
  uint64_t reuse_without_peek_ = 0;
  uint64_t reuse_new_bytes_ = 0;
  uint64_t reuse_match_bytes_ = 0;
  uint64_t reuse_cold_bytes_ = 0;
  uint32_t reuse_details_ = 0;
  std::chrono::steady_clock::time_point reuse_report_{};
  std::unordered_map<uint64_t, View> views_;
  // The vistas_ keys of each image. InvalidarImagenes (every resolve and every copy) used to walk all of
  // vistas_: 2.7 % of the ring thread in a race. Maintained together with vistas_ in RanuraVista
  // (insertion) and in QuitarVista (every removal).
  std::unordered_map<VkImage, std::vector<uint64_t>> views_per_image_;
  std::unordered_map<uint32_t, std::pair<VkSampler, uint32_t>> samplers_;
  // Diagnostic: filter values already logged (RanuraSampler).
  uint32_t seen_aniso_ = 0;
  bool tracked_aniso_ = false;  // nfsmw_nativo_anisotropico, one line when the first one is created
  uint32_t diag_mip_min_ = 0;  // nfsmw_nativo_diag_mip_minimo, read once per frame
  // nfsmw_nativo_sombras_sesgo_*, read once per frame.
  int32_t shadows_constant_bias_ = 0;
  int32_t shadows_slope_bias_ = 0;
  bool shadows_tracked_bias_ = false;
  uint32_t seen_mip_max_ = 0;
  std::array<bool, 1024> seen_biases_{};
  std::vector<Texture*> textures_to_upload_;
  std::vector<uint8_t> temporal_;
  std::vector<uint32_t, NoInitAllocator<uint32_t>> indices_;
  std::vector<uint32_t, NoInitAllocator<uint32_t>> converted_;
  std::vector<uint16_t, NoInitAllocator<uint16_t>> indices16_;  // camino rapido: 16 bits sin convertir
  // IndicesDe16 (nfsmw_nativo_indices_neon). Ring only.
  int32_t indices_neon_ = -1;  // -1 cvar not read, 0 plain loop, 1 NEON
  uint64_t draws_indices_neon_ = 0;
  uint64_t checked_indices_neon_ = 0;
  std::vector<uint16_t, NoInitAllocator<uint16_t>> test_indices_neon_;
  // Dynamic state already recorded in the current command buffer and pass.
  bool recorded_state_ = false;
  uint64_t recorded_push_[3] = {};
  VkViewport recorded_viewport_{};
  VkRect2D recorded_scissor_{};
  float recorded_blend_[4] = {};
  float recorded_bias_[2] = {};  // depth bias: constant and slope
  float warned_bias_[2] = {};
  uint32_t bias_warnings_ = 0;
  std::string skip_text_;  // nfsmw_nativo_diag_omitir_ps ya leido
  std::unordered_set<uint32_t> skip_ps_;
  uint32_t recorded_stencil_[2] = {};  // RB_STENCILREFMASK for front and back
  bool valid_recorded_stencil_ = false;
  VkIndexType recorded_indices_type_ = VK_INDEX_TYPE_MAX_ENUM;
  // Shared constants of the previous draw and where they were uploaded.
  uint32_t shared_previous_[kSharedWords] = {};
  uint64_t peeked_shared_ = 0;    // C6 report, to decide how to make the block cheaper
  uint64_t changed_shared_ = 0;
  VkDeviceSize offset_shared_ = 0;
  uint64_t epoch_shared_ = UINT64_MAX;

  VertexInput entry_;
  const VertexInput* current_entry_ = &entry_;  // entrada_ or a cache element
  std::unordered_map<uint64_t, VertexInput> cache_entries_;
  uint64_t hits_cache_entries_ = 0;
  const ShaderEntry* vs_entry_ = nullptr;
  uint64_t generation_entry_ = UINT64_MAX;
  bool valid_entry_ = false;

  uint64_t constantes_vs_generacion_ = UINT64_MAX;
  uint64_t epoch_vs_constants_ = UINT64_MAX;
  VkDeviceSize constantes_vs_offset_ = 0;
  uint64_t constantes_ps_generacion_ = UINT64_MAX;
  uint64_t epoch_ps_constants_ = UINT64_MAX;
  VkDeviceSize constantes_ps_offset_ = 0;
  uint32_t constantes_vs_bytes_ = 0;
  uint32_t constantes_ps_bytes_ = 0;

  bool active_pass_ = false;
  // The game's open occlusion query and the host query counting this pass's span.
  bool open_occlusion_ = false;
  // nfsmw_reflejo_visibilidad (see reflection_on_demand.h). The witness, every ~1 s at 30 FPS.
  static constexpr uint64_t kSentinelEvery = 32;
  bool measure_visibility_ = false;
  bool pending_sentinel_ = false;
  uint64_t next_sentinel_ = 0;
  uint32_t occlusion_query_ = UINT32_MAX;
  uint32_t pass_category_ = kGpuOther;
  bool sent_after_shadows_ = false;        // once per frame
  // Per-frame copies of the diagnostic cvars (see the comment at the Swap).
  std::string diag_skip_text_ps_;
  bool diag_repeated_vertices_ = false;
  bool diag_draw_stats_ = false;
  uint32_t test_scissor_ = 0;
  std::array<VkBuffer, 16> vertices_buffers_{};
  // Height actually used by each render target, deduced from the scissor. It only grows.
  std::unordered_map<uint32_t, uint32_t> usable_height_;
  bool usable_area_ = true;
  // Small per-draw savings. See their cvars.
  bool framing_cache_ = true;             // C1: the cvar, once per frame
  bool framing_cache_disabled_ = false;    // C1: the guard saw a difference
  bool framing_valid_ = false;
  uint64_t generation_framing_ = 0;
  uint64_t framing_series_pass_ = 0;
  uint64_t series_pass_ = 0;                // incremented on every EmpezarPase
  VkViewport framing_viewport_{};
  VkRect2D framing_scissor_{};
  float framing_ndc_[4] = {};
  uint32_t framing_empty_ = 0;
  uint64_t framing_hits_ = 0;         // since start-up (the guard)
  uint64_t framing_checked_ = 0;
  uint64_t framing_hits_report_ = 0;
  uint64_t framing_calculations_report_ = 0;
  static constexpr uint64_t kFramingsToCheck = 200000;
  bool active_memo_usable_height_ = true;      // C2
  bool memo_usable_height_disabled_ = false;
  uint32_t* memo_usable_height_ = nullptr;     // alto_util_ element of the last pitch (the map never erases)
  uint32_t memo_pitch_usable_height_ = 0;
  uint64_t hits_memo_usable_height_ = 0;
  uint64_t hits_memo_usable_height_previous_ = 0;
  static constexpr uint64_t kUsableHeightToCheck = 200000;
  bool fast_pass_key_ = true;          // C3
  bool fast_pass_key_disabled_ = false;
  uint64_t keys_pass_[5] = {};           // the render target bytes that produced pase_clave_
  bool valid_keys_pass_ = false;
  uint64_t fast_pass_keys_ = 0;
  uint64_t fast_pass_keys_previous_ = 0;
  static constexpr uint64_t kPassKeysToCheck = 200000;
  bool cvars_per_frame_ = true;        // C5
  bool frame_alpha_only_ps_ = true;
  bool without_ps_without_frame_color_ = true;
  std::chrono::steady_clock::time_point details_report_{};
  bool without_vegetation_ = false;
  // Shadow map vegetation draws discarded early (see the early discard in Dibujar).
  uint64_t soon_vegetation_draws_ = 0;
  // VeredictoVegetacion (nfsmw_d3d_vegetacion_juego).
  bool vegetation_computed_ = false;  // this draw carries the game's verdict and the ring's has been computed
  bool vegetation_ring_ = false;     // what the early discard has to decide
  uint16_t vegetation_flags_ = 0;
  VegetationDetail vegetation_detail_;
  /*
   * The sky draw, saved in full to replay later (nfsmw_nativo_cielo_aplazado).
   *
   * This holds everything the vkCmd* calls of that draw need, by copy: handles (which do not change
   * within the pass) and values (which depend on no guest register once copied). The only thing kept "by
   * reference" are the offsets within the upload buffer, and that buffer only moves forward until
   * UsarRanura resets it, always after the pass is closed.
   */
  enum : uint32_t {
    kSkyPerBlend = 0,  // a blended draw arrived: that one does read the background color
    kSkyPerWithoutZ,        // an opaque draw without Z write arrived: it could not cover the sky
    kSkyPerPassEnd,   // the pass closes (fallback path)
    kSkyPerOtherSky,   // a second sky in the same pass: the first one is emitted now
    kSkyReasons
  };
  struct DeferredSky {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;   // the upload one: vertices and indices
    bool usa_ubo = false;
    uint64_t push[3] = {};             // only with constants through pointers
    std::array<uint32_t, 3> offsets_ubo{};
    uint32_t ranura_ubo = 0;
    VkViewport viewport{};
    VkRect2D scissor{};
    float blend[4] = {};
    float bias[2] = {};
    bool with_stencil = false;
    uint32_t stencil[2] = {};          // RB_STENCILREFMASK for front and back
    uint32_t n_bindings = 0;
    std::array<VkDeviceSize, 16> offsets_vertices{};
    bool with_indices = false;
    bool indices16 = false;
    uint32_t indices = 0;
    uint32_t first_index = 0;
    uint32_t vmin = 0;
    uint32_t num = 0;               // sin indices
    uint32_t ps_mas_uno = 0;           // for the diagnostic per-draw query
    uint32_t category = 0;
    uint64_t draws_on_defer = 0;   // position in the pass when it was deferred
    uint32_t eds_mode = 0;  // phases 1 and 2 its pipeline was looked up with (eds_modo_)
    EdsState eds{};        // and the state that pipeline lacks
  };
  DeferredSky sky_{};
  bool sky_pending_ = false;
  bool sky_deferred_ = true;  // the cvar, read once per frame
  bool sky_tracked_ = false;
  uint64_t draws_in_pass_ = 0;
  uint64_t sky_seen_ = 0;
  uint64_t sky_seen_previous_ = 0;
  // The ones that pass both marks (480 indices and first of the pass). This is the number to watch: if it
  // is not 1.00 per frame, the criterion still does not isolate the dome and nothing should be deferred.
  // The fingerprint-only "detectados" once gave 7.00.
  uint64_t sky_candidates_ = 0;
  uint64_t sky_candidates_previous_ = 0;
  /*
   * The self-checking guard.
   *
   * Two earlier versions broke the image by enabling this blindly. The check that was missing ("the
   * criterion detects one draw per frame, not seven") no longer needs a diagnostic build: the game itself
   * runs it, before anything is deferred.
   *
   * Phase 0, observing: for the first kCieloFotogramasPrueba race frames nothing is deferred (the image
   *   is exactly the non-deferred one) and the candidates in each frame are counted.
   * Phase 1, deferring: only if those frames never had more than one and at least kCieloFotogramasConUno
   *   had exactly one. Then yes, and that is 3.4-4.3 ms of GPU time.
   * Phase 2, off for good: as soon as two are seen in the same frame, or if the test does not come out
   *   clean. It is not retried for the whole session.
   *
   * So the worst case of this change is that it does nothing. Breaking the image is not among the
   * possible outcomes.
   */
  enum : uint32_t { kSkyPeeking = 0, kSkyDeferring = 1, kSkyDiscarded = 2 };
  uint32_t sky_guard_ = kSkyPeeking;
  uint32_t sky_frames_guard_ = 0;
  uint32_t sky_guard_with_one_ = 0;
  uint32_t sky_guard_in_frame_ = 0;
  uint32_t sky_guard_max_ = 0;
  uint64_t sky_deferred_count_ = 0;
  uint64_t sky_deferred_previous_ = 0;
  uint64_t sky_no_deferrable_ = 0;
  uint64_t sky_two_in_pass_ = 0;
  uint64_t sky_lost_ = 0;
  std::array<uint64_t, kSkyReasons> sky_emitted_{};
  std::array<uint64_t, kSkyReasons> sky_emitted_previous_{};
  uint64_t sky_sum_position_ = 0;
  uint64_t sky_sum_position_previous_ = 0;
  uint64_t sky_position_max_ = 0;
  // The last binding made with vkCmdBindVertexBuffers, so it is not repeated.
  std::array<VkDeviceSize, 16> recorded_offsets_{};
  uint32_t recorded_bindings_ = 0;
  uint64_t saved_bindings_ = 0;
  // nfsmw_nativo_vertices_base_cero (see the cvar) and its guard.
  bool zero_vertices_base_ = true;           // the cvar, read once per frame (UsarRanura)
  bool zero_vertices_base_disabled_ = false;  // the guard saw a difference: the rest of the session, as before
  uint64_t checked_zero_base_ = 0;       // draws on the base-zero path since start-up (the guard)
  uint64_t draws_zero_base_ = 0;           // since the last report: without binding their own offset
  uint64_t zero_base_total_ = 0;             // since the last report: draws recorded with vertices
  uint64_t bindings_several_zero_base_ = 0;
  uint64_t misaligned_zero_base_ = 0;      // one binding, but the dedupe gave a copy off a multiple
  uint64_t recorded_bindings_zero_base_ = 0;  // vkCmdBindVertexBuffers recorded since the last report
  std::chrono::steady_clock::time_point zero_base_report_{};
  static constexpr uint64_t kZeroBaseToCheck = 200000;
  std::array<uint64_t, 19> sub_ns_{};  // C6 subetapas, measurement only
  std::array<uint64_t, 19> sub_n_{};
  uint64_t sub_samples_ = 0;
  std::chrono::steady_clock::time_point sub_next_{};
  uint64_t sends_after_shadows_ = 0;         // for the report
  uint32_t pass_stats_ = UINT32_MAX;
  uint32_t draw_occlusion_warnings_ = 0;  // diagnostic of the query draws
  std::chrono::steady_clock::time_point last_draw_occlusion_warning_{};
  VkCommandBuffer commands_pass_ = VK_NULL_HANDLE;
  uint64_t key_pass_ = 0;
  uint64_t generation_pass_ = UINT64_MAX;
  uint32_t pass_width_ = 0;
  uint32_t pass_height_ = 0;
  float scale_pass_ = 1.0f;  // 1 except in the scaled shadow map
  uint64_t depth_clears_in_pass_ = 0;  // ZCULL
  std::chrono::steady_clock::time_point pass_start_{};  // pass change breakdown
  uint32_t formats_pass_[5] = {};
  VkRenderPass rp_pass_ = VK_NULL_HANDLE;
  uint64_t generation_recording_ = UINT64_MAX;
  VkPipeline bound_pipeline_ = VK_NULL_HANDLE;
  bool bound_sets_ = false;

  uint32_t diagnostics_ = 0;
  std::unordered_set<uint32_t> vs_warned_;
  uint32_t indices_warnings_ = 0;
  uint32_t swizzle_warnings_ = 0;
  std::unordered_set<uint64_t> diagnosed_;
  uint64_t drawn_ = 0;
  uint64_t timed_drawn_ = 0;  // of those, with the stage stopwatch
  uint32_t counter_stopwatch_ = 0;
  bool time_ = false;  // the current draw carries the stage stopwatch
  // Rear-view mirror diagnostic: cubemap refreshes from their resolved faces (report every 10 s).
  uint64_t refreshed_cubes_ = 0;
  uint64_t refreshed_cubes_previous_ = 0;
  uint64_t cubes_report_frame_ = 0;
  std::chrono::steady_clock::time_point cubes_report_{};
  uint64_t rejected_ = 0;
  // 8 stages. Stage 7 is the pass change, split from the pass stage to tell how much of the 3.9 us per
  // draw is the actual change (15 per frame at 162 us) and how much is what is done on every draw.
  std::array<uint64_t, kDrawStages> stages_ns_{};
  std::unordered_map<uint32_t, uint64_t> causes_;
  uint32_t last_cause_ = 0;
  uint32_t cause_entry_ = 0;  // cause of the failed vertex input
  uint64_t texture_uploads_ = 0;
  uint64_t megas_bytes_ = 0;
  uint64_t textures_bytes_ = 0;  // texture images created (C6 report)
  int32_t textures_max_mb_ = 0;   // nfsmw_nativo_texturas_mb_max
  // Large slabs from which textures take chunks, instead of a dedicated allocation per texture (1.9 ms of
  // CPU each on Horizon). Terminar() goes in the destructor after the DestruirImagen loops, never before.
  TexturePool textures_pool_;
  // Texture bind thread (nfsmw_nativo_texturas_enlace_hilo). See the BucleEnlaces block.
  struct BindingRequest {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkResult result = VK_NOT_READY;  // written by the thread, under enlaces_mutex_
    uint64_t ns = 0;                    // wall time inside vkBindImageMemory, on the thread
  };
  struct TextureInFlight {  // ring thread only
    Texture* texture = nullptr;
    uint64_t ticket = 0;
    VkImage img = VK_NULL_HANDLE;  // the queued one (to remove it from imagenes_en_vuelo_ and move its views)
    VkResult result = VK_NOT_READY;
    VkFormat format_id = VK_FORMAT_UNDEFINED;  // the fields below, to recreate it through CrearTextura if the bind fails
    uint32_t width_px = 0;
    uint32_t height_px = 0;
    uint32_t layer_count = 1;
    uint32_t background = 0;
    uint32_t levels = 1;
    bool copy_info = false;  // SubirTextura already left its data in the upload buffer
    VkDeviceSize offset_copy = 0;
    uint64_t epoch_copy = 0;
    VkCommandBuffer commands_copy = VK_NULL_HANDLE;
  };
  struct ViewInFlight {  // ring thread only
    uint64_t key = 0;
    VkImage img = VK_NULL_HANDLE;
    VkFormat format_id = VK_FORMAT_UNDEFINED;
    uint32_t swizzle = 0;
    uint16_t swizzle_host = 0;
    uint32_t heap = 0;
    uint32_t ranura = 0;
  };
  static constexpr uint64_t kBindingsQueue = 256;  // potencia de 2
  static constexpr uint64_t kBindingsToCheck = 64;
  static constexpr int32_t kBindingsWithoutDecide = -1;
  static constexpr int32_t kBindingsDisabled = 0;
  static constexpr int32_t kPeekingBindings = 1;
  static constexpr int32_t kApplyingBindings = 2;
  std::array<BindingRequest, kBindingsQueue> bindings_queue_{};
  std::mutex mutex_bindings_;
  std::condition_variable cv_bindings_;         // there are requests or a stop request (the thread waits on it)
  std::condition_variable cv_done_bindings_;  // the thread finished one (the ring waits on it)
  uint64_t requests_bindings_ = 0;       // con enlaces_mutex_
  uint64_t done_bindings_ = 0;        // con enlaces_mutex_
  uint64_t collected_bindings_ = 0;     // con enlaces_mutex_
  bool sleeping_bindings_ = false;     // con enlaces_mutex_
  bool waiting_bindings_ = false;     // con enlaces_mutex_
  bool stop_bindings_ = false;         // con enlaces_mutex_
  bool priority_bindings_ok_ = true;   // con enlaces_mutex_
  uint64_t thread_bindings_ns_ = 0;       // con enlaces_mutex_
  uint64_t worst_binding_ns_ = 0;        // con enlaces_mutex_
  uint64_t full_queue_bindings_ = 0;    // con enlaces_mutex_
  std::thread thread_bindings_;
  int32_t priority_bindings_ = 0x2D;  // written by the ring before creating the thread
  // Ring thread only:
  int32_t phase_bindings_ = kBindingsWithoutDecide;
  bool stuck_bindings_ = false;
  bool creation_measuring_ = false;
  bool bindings_collecting_ = false;
  std::vector<TextureInFlight> in_flight_;
  std::vector<ViewInFlight> views_in_flight_;
  std::unordered_set<VkImage> images_in_flight_;
  uint64_t checked_bindings_ = 0;
  uint64_t thread_bindings_total_ = 0;
  uint64_t failed_bindings_ = 0;
  uint64_t deferred_views_ = 0;
  uint64_t deferred_copies_ = 0;
  uint64_t lost_slots_ = 0;
  uint64_t bindings_wait_total_ns_ = 0;
  // The 10 s report (InformeEnlaces):
  std::chrono::steady_clock::time_point bindings_report_{};
  uint64_t thread_bindings_report_ = 0;
  uint64_t bindings_waits_report_ = 0;
  uint64_t bindings_wait_report_ns_ = 0;
  uint64_t worst_bindings_wait_ns_ = 0;
  uint64_t create_report_ns_ = 0;
  uint64_t created_report_previous_ = 0;
  uint64_t deferred_views_report_ = 0;
  uint64_t deferred_copies_report_ = 0;
  uint64_t thread_bindings_previous_ns_ = 0;
  uint64_t full_queue_previous_ = 0;
  uint32_t reports_without_compensate_ = 0;
  // Texture hash thread (nfsmw_nativo_texturas_huella_hilo). See the PlanearHuella block.
  std::array<FingerprintJob, kFingerprintsQueue> fingerprints_queue_{};  // a slot belongs to the thread while it is EnHilo
  std::mutex mutex_fingerprints_;
  std::condition_variable cv_fingerprints_;         // there are jobs or a stop request (the thread waits on it)
  std::condition_variable cv_done_fingerprints_;  // the thread finished one (the ring waits on it)
  uint64_t published_fingerprints_ = 0;   // under huellas_mutex_ (only the ring writes it)
  uint64_t taken_fingerprints_ = 0;      // under huellas_mutex_: the thread has looked up to here
  uint64_t done_fingerprints_ = 0;       // con huellas_mutex_
  bool sleeping_fingerprints_ = false;    // con huellas_mutex_
  bool waiting_fingerprints_ = false;    // con huellas_mutex_
  bool stop_fingerprints_ = false;        // con huellas_mutex_
  bool priority_fingerprints_ok_ = true;  // con huellas_mutex_
  bool core_fingerprints_ok_ = true;     // con huellas_mutex_
  int real_core_fingerprints_ = -1;      // under huellas_mutex_: where the thread ran when starting
  std::thread thread_fingerprints_;
  int32_t priority_fingerprints_ = 0x2E;  // written by the ring before creating the thread
  int32_t request_core_fingerprints_ = -2;
  int32_t core_fingerprints_ = -1;       // written by the ring before creating the thread
  // The snapshots and the plans: the ring writes them before publishing each job; the thread only reads
  // them.
  std::unique_ptr<uint8_t[]> snapshots_;
  std::unique_ptr<FingerprintReadback[]> fingerprint_reads_;
  uint64_t snapshots_bytes_ = 0;
  // Ring thread only:
  int32_t phase_fingerprints_ = kFingerprintsWithoutDecide;
  uint64_t collected_fingerprints_ = 0;
  uint64_t used_snapshots_ = 0;
  uint32_t used_reads_ = 0;
  std::array<FingerprintReadback, kReadsPerTexture> plan_reads_{};
  std::vector<FingerprintJob> planned_fingerprints_;
  uint64_t pending_planned_fingerprints_ = 0;
  std::vector<FingerprintJob> gathered_fingerprints_;
  std::array<FingerprintCompare, kFingerprintsQueue> fingerprint_comparisons_{};
  const Texture* texture_comparison_ = nullptr;
  uint64_t sequence_comparison_ = 0;
  std::vector<uint8_t> ring_data_fingerprints_;
  std::vector<uint8_t> ring_check_fingerprints_;
  bool ring_disabled_fast_fingerprints_ = false;
  bool fingerprints_without_thread_ = false;
  bool stuck_fingerprints_ = false;
  bool suspicious_fingerprints_ = false;
  bool fingerprints_collecting_ = false;
  uint64_t planned_comparisons_fingerprints_ = 0;
  uint64_t applying_new_fingerprints_ = 0;
  uint64_t thread_fingerprints_total_ = 0;
  uint64_t ring_fingerprints_total_ = 0;
  uint64_t compared_fingerprints_ = 0;
  uint64_t fingerprints_without_compare_ = 0;
  uint64_t discarded_fingerprints_ = 0;
  uint64_t reuploaded_fingerprints_ = 0;
  // The 10 s report (InformeHuellas):
  std::chrono::steady_clock::time_point fingerprints_report_{};
  uint64_t planned_fingerprints_report_ = 0;
  uint64_t comparison_fingerprints_report_ = 0;
  uint64_t thread_fingerprints_report_ = 0;
  uint64_t ring_fingerprints_report_ = 0;
  uint64_t thread_fingerprints_report_ns_ = 0;
  uint64_t thread_raw_fingerprint_report_ns_ = 0;
  uint64_t ring_fingerprints_report_ns_ = 0;
  uint64_t snapshots_report_ns_ = 0;
  uint64_t snapshots_report_bytes_ = 0;
  uint64_t fingerprints_wait_report_ns_ = 0;
  uint64_t worst_fingerprints_wait_ns_ = 0;
  uint64_t fingerprints_waits_report_ = 0;
  uint64_t fingerprints_without_room_report_ = 0;
  uint32_t reports_without_fingerprints_compensate_ = 0;
  int32_t test_without_memory_every_ = 0;  // nfsmw_nativo_prueba_sin_memoria_cada
  uint64_t texture_reserves_ = 0;
  bool releasing_per_memory_missing_ = false;  // guard against reentry
  uint64_t few_released_to_few_ = 0;
  uint64_t trickle_warning_ = 0;
  uint64_t eviction_attempt_ = 0;
  uint64_t released_textures_ = 0;
  // Diagnostic of why the cache grows: creations per address and last key per shape (address, format and
  // size). Only touched when a texture is created.
  uint64_t created_textures_ = 0;
  uint64_t created_in_view_address_ = 0;
  uint64_t key_other_shape_same_created_ = 0;
  std::unordered_map<uint32_t, uint32_t> created_per_address_;
  std::unordered_map<uint64_t, uint64_t> key_per_shape_;
  std::unordered_map<uint64_t, std::array<uint32_t, 5>> words_per_shape_;
  uint64_t inconsistent_keys_ = 0;  // texture key guard (PrepararTextura)
  uint32_t key_other_warnings_ = 0;
  // "C6 contadores".
  uint64_t started_passes_ = 0;
  uint64_t full_sends_ = 0;
  uint64_t full_sends_ns_ = 0;
  uint64_t bytes_vertices_ = 0;
  uint64_t uploaded_indices_bytes_ = 0;
  uint64_t prepared_samplers_ = 0;
  uint64_t samplers_cache_ = 0;
  uint64_t passes_ns_ = 0;
  uint64_t ns_vertices_ = 0;
  uint64_t computed_entries_ = 0;
  uint64_t entries_ns_ = 0;
  uint64_t reused_entries_ = 0;
  uint64_t passes_per_generation_ = 0;
  uint64_t passes_per_target_ = 0;
  uint64_t resumed_passes_ = 0;
  uint64_t passes_texels_ = 0;
  std::array<uint64_t, kGpuCategories> texels_per_category_{};
  std::array<uint64_t, kGpuCategories> draws_per_category_{};
  std::chrono::steady_clock::time_point ps_toggle_start_ = std::chrono::steady_clock::now();
  bool tracked_ps_toggle_ = false;
  bool tracked_alpha_only_toggle_ = false;
  bool tex_size_inv_ = false;
  bool cheap_pcf_ = false;                    // a single shadow map sample
  bool shadow_cars_pass_ = false;            // nfsmw_nativo_sombra_minimo, car pass
  bool without_frame_blur_ = true;       // nfsmw_nativo_sin_desenfoque, per frame
  bool test_point_mip_ = false;            // Test: trilinear -> bilinear (measurement only)
  // Deduplication of vertex uploads within the frame.
  DedupeVertices dedupe_;
  bool active_dedupe_ = true;
  uint32_t view_sync_dedupe_ = 0;  // last value seen of g_sincronizaciones_anillo
  std::set<uint64_t> resolved_invsize_warnings_;  // one trace per size
  bool tracked_scissor_toggle_ = false;
  std::unordered_map<const ShaderEntry*, VkShaderModule> alpha_only_modules_;
  // The same module with OpExecutionMode EarlyFragmentTests (nfsmw_nativo_z_temprana).
  std::unordered_map<const ShaderEntry*, VkShaderModule> early_modules_z_;
  uint64_t useless_ps_draws_ = 0;     // no color and a PS that does not discard
  uint64_t required_ps_draws_ = 0;  // no color, but the PS is needed
  uint64_t shadows_active_alpha_ = 0;
  uint64_t scene_with_discard_ = 0;
  uint64_t scene_without_discard_ = 0;   // shadow map draws with the alpha test enabled
  /*
   * nfsmw_nativo_z_temprana and nfsmw_nativo_saltar_invisibles. The first four split C6's "lo impiden"
   * in two: those fixed by testing earlier and those that cannot be fixed because they write depth or
   * stencil (the latter are the exact size of a depth pre-pass).
   */
  enum : uint32_t {
    kZSet = 0,       // its depth test is moved earlier
    kZWriteZ,         // not possible: the draw writes depth
    kZStencil,         // not possible: stencil is involved
    kZOclusion,         // left alone: a game occlusion query is open
    kZEarlyAlready,       // no alpha test or kill: the GPU already tested early
    kBlendInvisible,   // blending copies the destination: the draw paints nothing
    kInvisibleAlfa,     // alpha test with the NEVER function
    kColorOnlyInvisible,  // invisible in color but writes Z: only its color is removed
    kZCounts
  };
  std::array<uint64_t, kZCounts> counts_z_{};
  std::array<uint64_t, kZCounts> counts_z_previous_{};
  uint64_t z_early_without_module_ = 0;
  uint64_t frames_z_ = 0;
  uint64_t frames_z_previous_ = 0;
  std::chrono::steady_clock::time_point last_report_z_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point toggle_z_start_ = std::chrono::steady_clock::now();
  bool z_early_ = true;
  bool tracked_toggle_z_ = false;
  bool skip_invisible_ = true;
  uint64_t shadows_alpha_disabled_ = 0;  // ... and without it (the stage would be unnecessary)
  std::array<uint64_t, kGpuCategories> triangles_per_category_{};
  std::array<uint64_t, kGpuCategories> passes_per_category_{};
  uint64_t ns_render_pass_ = 0;
  // Prueba nfsmw_nativo_omitir_sombras_alternar_s.
  std::chrono::steady_clock::time_point shadows_start_ = std::chrono::steady_clock::now();
  bool shadows_skipped_ = false;
  // Diagnostico nfsmw_nativo_diag_vertices_repetidos.
  struct SeenVertices {
    uint64_t frame = UINT64_MAX;
    uint64_t fingerprint = 0;
  };
  std::unordered_map<uint64_t, SeenVertices> seen_vertices_;
  uint64_t frame_repeated_bytes_ = 0;
  uint64_t previous_equal_bytes_ = 0;
  uint64_t ns_hash_vertices_ = 0;
  // Last texture and sampler result of each PS sampler register.
  struct CacheSampler {
    std::array<uint32_t, 6> fetch{};
    uint64_t frame = UINT64_MAX;
    uint64_t generacion = 0;
    uint32_t ranura = 0;
    uint32_t heap = 0;
    uint32_t sampler = 0;
    uint64_t valid_until = 0;  // last frame it is valid without going back to PrepararTextura
    uint32_t width_px = 0;  // host image size, for 1/size
    uint32_t height_px = 0;
    float scale = 1.0f;  // AirborneNX: value scale of the slot (kPalabraEscala)
  };
  bool cache_between_frames_ = true;  // nfsmw_nativo_cache_texturas_entre_fotogramas
  bool mipmaps_ = true;                 // nfsmw_nativo_mipmaps
  bool diag_mips_ = false;              // nfsmw_nativo_diag_mips
  uint64_t reviewed_mips_ = 0;
  uint64_t rare_mips_ = 0;
  uint64_t uploaded_mip_levels_ = 0;    // report: mip levels (without the base) in created textures
  std::array<CacheSampler, 16> cache_samplers_{};
  // Second cache, by the whole fetch constant: a direct-mapped table (the hash picks the slot). It went
  // from 256 to 1024 slots because in a race there are ~340 distinct textures per frame and 256 slots
  // collided (64 KB: fits in L2).
  /*
   * From 1024 to 4096 (288 KB, still fits in the 2 MB L2). At the Heritage & Omega exit there are ~3,000
   * draws per frame and ~205 misses of this table per frame (C6 counters: 97,709 in 20 s), at ~15 us
   * each through PrepararTextura (the texture stage measures 1.2-1.5 us per draw with 3.4 % misses). With
   * N distinct fetches in M slots, the fraction sharing a slot with another is 1 - e^(-N/M): with ~600
   * that is 44 % at 1024 and 14 % at 4096. The hit conditions do not change: there is just more room. The
   * misses-by-cause report (every 10 s) says how many were collisions.
   */
  std::array<CacheSampler, 4096> cache_fetch_{};
  uint64_t samplers_cache_fetch_ = 0;
  uint64_t collision_failures_fetch_ = 0;      // The slot held another fetch constant
  uint64_t empty_failures_fetch_ = 0;       // casilla sin usar todavia
  uint64_t generation_failures_fetch_ = 0;  // same fetch, but generacion_texturas_ changed
  uint64_t expired_failures_fetch_ = 0;    // same fetch, but the texture is due for a check (valido_hasta)
  std::array<uint64_t, 6> fetch_report_previous_{};
  std::chrono::steady_clock::time_point fetch_report_{};
  uint64_t textures_generation_ = 0;  // changes with every C2 copy and every retired image
  // On-disk pipeline cache (CargarCachePipelines and GuardarCachePipelines).
  using CreatePipelineCacheFn = VkResult(VKAPI_PTR*)(VkDevice, const VkPipelineCacheCreateInfo*,
                                                     const VkAllocationCallbacks*,
                                                     VkPipelineCache*);
  using PipelineCacheDataFn = VkResult(VKAPI_PTR*)(VkDevice, VkPipelineCache, size_t*, void*);
  using DestroyPipelineCacheFn = void(VKAPI_PTR*)(VkDevice, VkPipelineCache,
                                                    const VkAllocationCallbacks*);
  PipelineCacheDataFn cache_data_ = nullptr;
  DestroyPipelineCacheFn destroy_cache_ = nullptr;
  VkPipelineCache cache_pipelines_ = VK_NULL_HANDLE;
  uint32_t pipelines_without_save_ = 0;
  uint32_t cache_saved_ = 0;
  size_t saved_cache_bytes_ = 0;  // size of the last file read or saved
  std::chrono::steady_clock::time_point saved_cache_{};
  // Thread that writes the pipeline cache to disk (EscritorCacheMain). It is persistent, because on
  // Horizon detach() closes the game, and wake-ups are decided with the lock held.
  std::thread cache_writer_;
  std::mutex mutex_writer_;
  std::condition_variable warning_writer_;
  std::vector<uint8_t> data_writer_;
  bool pending_writer_ = false;
  bool stop_writer_ = false;
  // The single pipelines file. The last written version of each part (or the one read at start-up): owned
  // by the writer thread once it is running, and by Inicializar before that. lista_leida_ goes from
  // CargarCachePipelines to CargarListaPipelines.
  std::vector<uint8_t> cache_written_;
  std::vector<uint8_t> list_written_;
  std::vector<uint8_t> read_list_;
  bool old_files_ = false;  // the two older files were read: deleted when the new one is written
  uint64_t ns_pipelines_ = 0;  // creando pipelines (informe C6)
  std::unordered_set<uint32_t> warned_;
  // Pipeline prewarm (BuclePrecalentado). lista_archivo_ and estado_lista_ are sized before the thread
  // starts and never change size; the thread writes estado_lista_[i] before publishing
  // precalentado_hasta_ > i. Anything not atomic and not marked otherwise belongs to the ring only.
  std::vector<PipelineRecord> file_list_;         // the list read at start-up (walked by the thread)
  std::vector<uint8_t> list_state_;                   // kLista* of each record of lista_archivo_
  std::unordered_map<uint64_t, size_t> list_index_;   // XXH3 of the key -> index (SIZE_MAX: from this session)
  std::vector<PipelineRecord> session_list_;          // this session's new ones
  uint32_t list_without_save_ = 0;
  std::vector<uint8_t> list_writer_;                 // con escritor_mutex_
  std::thread prewarmed_thread_;
  const NativeShaders* prewarmed_library_ = nullptr;
  uint32_t prewarmed_eds_ = 0;
  bool prewarmed_decided_ = false;
  bool prewarmed_difference_ = false;
  std::chrono::steady_clock::time_point prewarmed_start_{};
  std::chrono::steady_clock::time_point prewarmed_report_{};
  uint64_t prewarmed_changes_previous_ = 0;
  std::atomic<bool> prewarmed_stop_{false};
  std::atomic<bool> prewarmed_finished_{false};
  std::atomic<size_t> prewarmed_until_{0};
  std::atomic<uint32_t> prewarmed_done_{0};
  std::atomic<uint32_t> prewarmed_compiled_{0};
  std::atomic<uint64_t> prewarmed_compiled_ns_{0};
  std::atomic<uint32_t> prewarmed_without_shader_{0};
  std::atomic<uint32_t> prewarmed_other_mode_{0};
  std::atomic<uint32_t> prewarmed_failed_{0};
  std::atomic<int32_t> prewarmed_priority_{-1};
  uint32_t prewarmed_counted_ = 0;  // of precalentado_compilados_, already added to pipelines_sin_guardar_
  uint64_t prewarmed_ring_ = 0, slow_prewarmed_ring_ = 0, prewarmed_ring_ns_ = 0;
  uint64_t list_ring_ = 0, list_ring_ns_ = 0;
  uint64_t new_ring_ = 0, new_ring_ns_ = 0, ring_to_list_ = 0;
};

}  // namespace

std::unique_ptr<VulkanDrawer> VulkanDrawer::Make(const VulkanDevice* dev,
                                                    rex::memory::Memory* mem,
                                                    TargetsContext* contexto) {
  if (!dev || !mem || !contexto) {
    return nullptr;
  }
  auto draws = std::make_unique<VulkanDrawerImpl>(dev, mem, contexto);
  if (!draws->Initialize()) {
    REXLOG_ERROR("[nativo] C6: no se pudieron preparar los dibujos nativos");
    return nullptr;
  }
  REXLOG_INFO("[nativo] C6: dibujos nativos preparados");
  return draws;
}

}  // namespace nfsmw::nativo
