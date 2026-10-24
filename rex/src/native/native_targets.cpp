// nfsmw - native renderer, part C2 (see native_targets.h).
//
// WHAT IT COVERS
//   - k_8_8_8_8 and k_8_8_8_8_GAMMA color render targets without MSAA, as
//     R8G8B8A8 images of pitch x max(720, pitch) (capped at 2048).
//   - Copy of a rectangle of the render target to the resolved texture at
//     RB_COPY_DEST_BASE (destination format k_8_8_8_8), with the rectangle and
//     the base computed like GetResolveInfo (graphics/util/draw.cpp:765-1010).
//   - Render targets with MSAA: used with 1 sample (MSAA hangs NVK on the Switch).
//   - Color clear with RB_COLOR_CLEAR, converted like the emulation does
//     (vulkan/render_target_cache.cpp:5350-5355).
//   - Presentation: the resolved texture requested by fetch constant 0 of the
//     Swap is drawn into the presenter output with the SDK's own shaders. By
//     default, with the gamma ramp the game loads, like the Xbox 360 display
//     (shaders/output_gamma_ramp.frag).
//
// DRAWS (parts C3-C6, nfsmw_nativo_dibujos.cpp)
//   This class lends them the render targets (color and depth), the frame's
//   command buffer and an upload one that is submitted right before it.
//
// WHAT IT DOES NOT COVER
//   Copies from depth, 16- and 32-bit formats, 3D textures as destination,
//   clearing only the rectangle (the whole image is cleared) and the normal
//   draws (parts C3-C6, in nfsmw_nativo_dibujos.cpp).
//
// Everything is used only by the native system's PM4 ring thread: no locks.

#include "native_targets.h"
#include "stall_waits.h"
#include "native_shaders.h"  // Samplers of the PS (nfsmw_nativo_diag_lectores_s)
#include "reflection_on_demand.h"  // road reflection only when it is read

#include "graphics_settings.h"

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/util.h>

#include <algorithm>
#include <array>
#include <chrono>

#if REX_PLATFORM_SWITCH
// Counter 0 of the console profiler (game FPS), the same one IssueSwap of the emulated path uses.
extern "C" void RexSwitchPerfCount(unsigned id);
// Interval of a long frame for stack sampling ("durante los tirones" section).
extern "C" void RexSwitchPerfTiron(uint64_t start, uint64_t stop_pos);
#endif
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

REXCVAR_DEFINE_BOOL(airborne_barreras_copias, true, "AirborneNX",
                    "Native renderer: a memory barrier before and after every image copy and blit (NFSMW "
                    "relied on NVK ordering; desktop drivers overlap them with the passes around)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_nativo_resolver_sin_copia_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (prueba, build 154): con N > 0 alterna copiar e intercambiar cada N "
                     "segundos, para comparar capturas del mismo sitio con el juego en pausa");
REXCVAR_DEFINE_INT32(nfsmw_nativo_sombras_escala, 100, "NFSMW",
                     "Renderizador nativo (19/09, build 201): dibuja los dos mapas de sombras de 1600x1600 "
                     "del juego a este porcentaje y los sube de tamano al resolverlos. 100 = como la Xbox 360 "
                     "(1600, que son 2000 de sus 2048 baldosas de EDRAM); 64 = como la version de PC de este "
                     "mismo juego (1024). El valor se toma al crear el primer mapa y no cambia en marcha")
    .range(50, 100);
REXCVAR_DEFINE_BOOL(nfsmw_nativo_resolver_sin_copia, true, "NFSMW",
                    "Renderizador nativo (17/09, build 154; por defecto desde la 161, medido en consola: -2,36 ms "
                    "de los 7,41 de copias): al resolver un destino entero a una textura del mismo "
                    "tamano y formato, intercambia las imagenes en vez de copiar los pixeles. La imagen no cambia; si "
                    "el juego vuelve a dibujar en ese destino sin borrarlo antes, se restaura la copia");
/*
 * Swap also when the command does not clear the render target.
 *
 * The game resolves the shadow map twice per frame, once per cascade. The first command clears the
 * render target and therefore swaps (free); the second does not clear, and therefore copied the whole
 * 1600x1600: 2.56 Mpixels, 2.4 ms of real GPU time measured, 44 % of all copies in the frame.
 *
 * Requiring the command to clear was a precaution, not a necessity: if the game draws into the render
 * target again without clearing it, RestaurarContenido already brings the image back from the resolved
 * texture. So the worst case is paying the same copy later (net zero), not a regression. And the game
 * starts the next frame by clearing the shadow map, so there should not be a single restore.
 *
 * How to check it in the log: "C2 resoluciones sin copia" must go from ~0.93 to ~1.9 per frame and
 * "restauraciones" must stay at 0. If the restores go up, this does not help and should be turned off:
 * we would be paying for the copy anyway, just somewhere else.
 */
/*
 * Swap the color too, not only the depth.
 *
 * Image swapping (resolving without copying) works for depth and is worth 2.36 ms measured. For color it
 * never could, not because of the mechanism but because of a silly size detail: the color render target is
 * created with `height = max(720, pitch)`, so with pitch 1280 it is 1280x1280, while the resolved texture is
 * 1280x720. Since IntercambiarConResuelta requires the same size, it always said no.
 *
 * The idea: the game never draws below row 720 in a color render target (the viewport and the scissor are
 * 1280x720 in every draw measured), so the pitch-1280 color render target was created 720 high and both
 * sizes matched.
 *
 * That is 4 copies of 1280x720 per frame = 3.69 Mpixels, 78 % of all copy traffic.
 *
 * Result (tested): it does not work, for two separate reasons.
 *   1. Without requiring a clear: 16,807 color swaps and 17,350 restores, almost one for one. The game
 *      draws on top again without clearing, so the copy is paid anyway, later and with two operations
 *      instead of one.
 *   2. Requiring a clear and creating the render target 720 high: the scene breaks (the screen fills with
 *      a yellow smear). The color render target cannot be shrunk to 720.
 * It stays off. The code is kept because the mechanism is correct; what fails is that this game reuses the
 * color render target without clearing it.
 *
 * The safety net is mandatory and did not exist for color: DestinoColor did not call RestaurarContenido
 * (DestinoProfundidad did). Without it, the first frame in which the game draws on top without clearing
 * would see the previous frame's content. It is added here.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_intercambiar_color, false, "NFSMW",
                    "Renderizador nativo (20/09): al resolver el destino de color entero a una textura del "
                    "mismo tamano, intercambiar las imagenes en vez de copiar 1280x720 pixeles. Son 4 copias "
                    "por fotograma, el 78 % del trafico de copias. La imagen no cambia; si el juego dibuja "
                    "encima sin borrar, se restaura");

REXCVAR_DEFINE_BOOL(nfsmw_nativo_intercambiar_sin_borrado, true, "NFSMW",
                    "Renderizador nativo (20/09): intercambiar la imagen tambien cuando la orden de resolve no "
                    "borra el destino. Se lleva por delante la copia de 1600x1600 del segundo mapa de sombras "
                    "(2,4 ms reales). Si el juego dibuja encima sin borrar, se restaura: peor caso, lo mismo "
                    "que ahora. Vigila 'restauraciones' en el log: tiene que quedarse en 0");
/*
 * The flickering shadows of the main menu. This was the cause.
 *
 * A resolve without a clear that is done by swapping leaves the content in the texture and the render target
 * with that texture's old image (contenido_invalido). It was only brought back when drawing on top
 * (DestinoProfundidad) and never before another resolve, which read the old image as it was.
 *
 * In a race it does not happen: between the two resolves of the shadow map the cars are drawn and that
 * brings it back. In the menu it does, every frame. The garage draws the car into the 1600x1600 map and
 * resolves twice in a row, without drawing in between: without a clear to 07CEA000 (read by the car body) and
 * with a clear to 086AE000 (read by the scenery). On the console, in the menu: 398 swaps without a clear + 398
 * with a clear and 0 restores in 10 s. With images A, B and C:
 *     draw into A -> resolve to 07CEA000: the texture keeps A (correct), the render target gets B (the old one)
 *                 -> resolve to 086AE000: the texture keeps B: the previous frame's map
 * The scenery always sampled the map from one frame earlier with the current frame's light matrix. While the
 * shadow framing does not change, it is not noticeable. As soon as it jumps (the menu camera comes in
 * rotating) the old map lands shifted and the wall to the right of the car goes dark for one frame: a
 * console capture shows it every ~4 game frames between seconds 8 and 9.
 *
 * Now, if the render target about to be resolved has its content in another texture, it is first brought
 * back by copying (without lending or the minimum trick: this is a resolve, not the car pass), and the
 * resolve continues as usual. It is the exact path: on the Xbox 360 both resolves read the same EDRAM. It
 * costs one 1600x1600 copy per frame in the menu (~1.5 ms of GPU); in a race nothing is touched because the
 * render target already arrives valid. Report line: "C2 restauraciones por fotograma", the "para
 * resolver" figure. false = previous behavior.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_resolver_contenido_valido, true, "NFSMW",
                    "Renderizador nativo (26/09, build 193): antes de resolver un destino de profundidad cuyo contenido "
                    "se fue en un intercambio, traerlo de vuelta. Arregla el parpadeo de las sombras del menu (el "
                    "escenario muestreaba el mapa del fotograma anterior). false = como la 192");
/*
 * The verdict on intercambiar_sin_borrado, with numbers measured on the console.
 *
 * Console log, three consecutive 10 s intervals in a race:
 *     swaps without clear       4679 -> 4885 -> 5145 -> 5369   (+206, +260, +224)
 *     restores                  3584 -> 3790 -> 4050 -> 4274   (+206, +260, +224)
 * One restore per swap, exactly, in all three intervals. So the pixel saving is exactly zero: the
 * 1600x1600 copy is not avoided, it is paid later and with two operations (swap + restore) instead of one.
 *
 * And it is the missing piece to make the "copias" breakdown of the C2 report add up:
 *     recorded inventory  2.67 Mpixels x 0.78 ms/Mpixel = 2.08 ms
 *     restores            0.83/frame x 2.56 Mpixels x 0.78 = 1.66 ms  <-- not recorded
 *     scaling blit 1280x720 -> 1024x576 (internal resolution)         = 0.49 ms
 *                                                            total    4.23 ms
 *     measured: 2.69 raw x 1.627 = 4.38 ms real.
 * Restores were 38 % of the frame's copies and did not show up in any bucket. They are recorded from here
 * on (AnotarCopia in RestaurarContenido), so that the inventory adds up by itself.
 *
 * The setting is not turned off: turning it off moves the copy back to the resolve and costs the same. What
 * has to go is the restore, which is what the two settings below are for.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_restaurar_area_util, true, "NFSMW",
                    "Renderizador nativo (20/09 noche): al traer de vuelta el contenido de un destino "
                    "intercambiado, copiar solo las filas que el juego resuelve de verdad en vez de la imagen "
                    "entera. Los destinos se crean con alto = max(720, pitch) (la escena es 1280x1280 para "
                    "dibujar 1280x720), y lo de debajo del area util no se dibuja ni se lee nunca");
/*
 * Restore without copying a single pixel.
 *
 * The render target lost its content in a swap: it is in the resolved texture R. Normally R -> target is
 * copied (10.24 MB for the shadow map, 1.66 ms real per frame). But the content does not have to be
 * duplicated, it only has to be in the render target: swapping the two images again is enough. Cost: zero
 * bytes, only handles.
 *
 * What it costs: R keeps the render target's old image until the game resolves to that address again. For
 * the shadow map that happens in the same frame and before anybody reads it:
 *     [cascade 1 pass]  <- restored here, R is lent out
 *     resolve cascade 1 -> another address
 *     [cascade 2 pass]
 *     resolve cascade 2 -> R          <- R gets good content back
 *     reflection, cubemap, SCENE      <- the scene is what samples R
 * So in steady state it is safe. In transitions (menu, loading, pause) it may not be.
 *
 * That is why it is off and watched: 'prestadas leidas' in the C2 report counts the times someone
 * requested a resolved texture while it was lent out. If the log says 0, this is worth 1.66 ms real and
 * gets turned on. If it says anything else, the image could show the previous frame's shadows and it stays
 * off.
 */
/*
 * It stayed on, but the watchdog reading flipped. Read this.
 *
 * The mix-up: it was turned on citing "prestadas leidas 0, limpio" from an earlier run. That zero proved
 * nothing: the cvar was false in the toml, not a single image was lent, and the watchdog counted zeros
 * because it had nothing to count. The first session in which this path actually ran says the opposite:
 *     prestadas leidas 8226   -> *** EL PRESTAMO NO ES SEGURO ***
 * One read per frame, exactly, during the five minutes of racing. The premise written above ("in steady
 * state it is safe, the risk is the transitions") is backwards: the transitions gave 9 isolated reads and
 * the steady state gives one every time.
 *
 * And even so it stayed on, because what it buys is measured and was the best variance improvement so far,
 * and variance is what the player notices (same load, without vs. with):
 *     copies         4.29 -> 1.93 ms real        frame       38.63 -> 36.11 ms
 *     deviation      8.19 -> 7.06 ms             > 50 ms     6.14 % -> 3.66 %
 *     frame median   43.0 -> 33.3 ms
 * Turning it off brings the median back to 43 ms, for a risk that in six minutes of play did not produce a
 * single visible fault. The "C2 prestadas leidas, por direccion" line reports what is read, with address
 * and size: if it is the 1600x1600 shadow map, it has to be fixed (by returning the render target, which is
 * where the good content is); anything else may be harmless.
 */
/*
 * Off. This is what left the car without a shadow (bridges, trees).
 *
 * The race shadow pass resolves the same render target twice (sub_82443B18):
 *   1. after the world, without a clear  -> texture[1] ([0x82A15374], 07CEA000)   sub_824427F8
 *   2. after the cars, with a clear      -> texture[0] ([0x82A15370], 086AE000)   sub_82442908
 * The car body (effect type 7, sub_824511E8) samples 1 (the map without cars, so as not to shadow itself)
 * and the world samples 0. Between the two resolves the game draws the cars on top without clearing, so
 * here the render target was restored by swapping: texture[1] got its old image back and stayed lent out
 * until the next frame's resolve. The scene reads it before that: in a console run, the increase of
 * "prestadas leidas" is exactly the number of reads of 07CEA000 in each race report (369/369, 385/385,
 * 336/336...). And since it always gets the same image back, the car does not read the previous frame's
 * map: it reads one frozen since the start of the race. The ground darkens under the bridge and the car
 * does not.
 *
 * It costs the 1600x1600 copy this used to save (2.56 Mpixels, ~2.00 ms real per frame in a race): that
 * saving was fake. With false, "prestadas leidas" and "por intercambio" must stay at 0.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_restaurar_por_intercambio, false, "NFSMW",
                    "Renderizador nativo (20/09 noche; APAGADO el 25/09): traer de vuelta el contenido de "
                    "un destino intercambiado volviendo a intercambiar las imagenes en vez de copiarlas. NO "
                    "ENCENDER: deja prestada la textura[1] del mapa de sombras, la que muestrea el coche, y "
                    "el coche deja de recibir sombras. Vigila 'prestadas leidas' en el log: tiene que ser 0");
/*
 * Depth that nobody samples is not copied (nfsmw_nativo_profundidad_perezosa).
 *
 * Every race frame the game resolves the scene depth to a 1024x576 texture (091F0000 in the logs: 0.59
 * Mpixels and one copy per frame, "C2 caras resueltas") and the only reader is the final composition
 * p_000139 (PS n19): it is its HEIGHTMAP, which only feeds the radial blur factor. With
 * nfsmw_nativo_sin_desenfoque (true by default) that sampling is dead (the specialization constant cuts
 * the blend and the compiler removes it), but the copy was still being paid: ~0.35 ms real per frame
 * (0.60 ms per Mpixel, which is what the 1600x1600 copy cost when it came back: copies went from 1.93 to
 * 3.47 ms).
 *
 * Now that copy is deferred, and only if in the last kPerezosaFotogramas frames that address was requested
 * by the composition without blur and no draw really sampled it. It is kept pending (source, destination,
 * rectangle):
 *   - if a draw really samples it (another shader, p_000140 with depth of field, or the composition with the
 *     blur on), it is copied right before that draw. The source has not changed: every write to it first
 *     goes through AntesDeEscribirProfundidad (pass, clear, restore, swap and destruction);
 *   - if the source is about to be written in another frame and the composition already requested it,
 *     nobody really read it: it is dropped without copying (the normal case in a race: the next frame's
 *     scene clear);
 *   - if the source is about to be written in the same frame, or without the composition having requested
 *     it, it is copied (exact);
 *   - if another resolve arrives at the same address while the copy is still deferred, it is copied first
 *     (exact).
 * GUARD: if a draw really samples an address whose copy was dropped (a late read: a previous frame's depth
 * after the next frame has started drawing), DIFERENCIA in the log and it turns off for the session.
 * Report line: "C2 profundidad perezosa". false = always copy, as before.
 */
// The first version saved nothing (the copy was recorded if the source was rewritten in the same frame, which
// is always the case); with the current AntesDeEscribirProfundidad rule it is dropped. On again.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_profundidad_perezosa, true, "NFSMW",
                    "Renderizador nativo (25/09, build 184): la profundidad que el juego resuelve para la composicion "
                    "final solo se copia si un dibujo la muestrea de verdad (sin el desenfoque, ninguno). La imagen no "
                    "cambia; ~0,35 ms de GPU por fotograma. Se comprueba sola. false = copiar siempre, como antes");
/*
 * The front buffer is drawn from its render target, without copying it (nfsmw_nativo_frontal_perezoso).
 *
 * At the end of each frame the game resolves the 1024x576 output (the pitch-1040 render target) to one of
 * its two front buffers (09430000 and 09670000) and the Swap draws it with PintarSalida. That is 0.59
 * Mpixels of copying per frame (~0.35 ms real, at 0.60 ms per Mpixel) and across the measured sessions no
 * draw has ever sampled a front buffer: 0 reads in every session ("C2 caras resueltas"). Only the Swap
 * reads them.
 *
 * If the address is a front buffer (a Swap drew it in the last kFrontalFotogramas and no draw sampled it)
 * and the copy is 1 to 1, of the whole texture and from the corner of the render target, it is deferred:
 *   - in the Swap it is drawn from the image that holds the content with the exact variant of the output
 *     (texelFetch of the pixel, output of the front buffer's size and without FXAA, which is the usual
 *     one): texel (x, y) of that image is the same byte the copy would have left at (x, y) of the texture.
 *     With FXAA, without the ramp or with another output, the copy is recorded before submitting the work
 *     and the texture is drawn, as always;
 *   - if the game clears the whole render target (which it does when the next frame starts), the render
 *     target gets a spare image of the same size and the one with the content is kept for the front
 *     buffer: the clear does not need what was there, so nothing is copied. There are at most
 *     kFrontalImagenesMax spares (~4 MB each);
 *   - if the render target is written some other way first (a pass, a restore, a swap), if a draw samples
 *     the front buffer or if another resolve arrives that does not cover it entirely, the copy is recorded
 *     first (exact);
 *   - if another resolve arrives that covers it entirely, the deferred copy is unnecessary and dropped.
 * The content is never lost: the image is the same by construction. GUARD: if something requests a front
 * buffer whose copy can no longer be made, DIFERENCIA in the log and it turns off for the session. Report
 * line: "C2 frontal perezoso". false = always copy, as before.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_frontal_perezoso, true, "NFSMW",
                    "Renderizador nativo (25/09, build 184): el Swap pinta el frontal desde su destino de render (o una "
                    "imagen retenida) en vez de copiarlo antes a la textura: 0,59 Mpixeles menos por fotograma. Misma "
                    "imagen; con FXAA se copia como siempre. Se comprueba sola. false = copiar siempre, como antes");
/*
 * The shadow map without the 1600x1600 copy (nfsmw_nativo_sombra_minimo).
 *
 * The race shadow pass (sub_82443B18) draws the world into the 1600x1600 render target, resolves it without
 * a clear to texture[1] (07CEA000: the map without cars, sampled by the car body), draws the cars on top and
 * resolves it with a clear to texture[0] (086AE000: the map with cars, sampled by the world). Since we
 * resolve by swapping images, after the first resolve the render target no longer holds the world and it
 * has to be brought back before the cars: that is the 1600x1600 copy (2.56 Mpixels, ~1.5 ms real per frame,
 * the most expensive copy in the frame).
 *
 * With this setting the render target is cleared to 1.0 instead of copied and the cars are drawn alone.
 * texture[0] ends up with the cars only, and the draws that sample it through their SHADOWMAP_SAMPLER
 * (tfetch2DSombraMin in the library) get min(texture[0], texture[1]): the 3D index word of that register
 * carries texture[1] and the pipeline carries the SPEC_CONSTANT_SOMBRA_MINIMO bit (23). This is exact, not
 * an approximation: with Z writes and a LESS or LEQUAL test, what a draw leaves on a buffer is the minimum
 * of what was there and of its fragments, and min(world, min(1, cars)) = min(world, cars) because no texel
 * exceeds 1.0. The map is point-sampled and both views carry the same swizzle: the result is the same value
 * the copy would have left. The depth bias is the same on both paths (same pipelines). With the cheap PCF
 * there is one extra read per fragment that samples the world shadow; with the 9-sample PCF it would be nine
 * and it does not pay off: without nfsmw_nativo_pcf_barato it copies as always.
 *
 * It costs a clear of 2.56 Mpixels (~0.19 ms at 0.075 ms per Mpixel) and that read; it saves the copy
 * (~1.54 ms).
 *
 * GUARD, self-checking:
 *   - WATCHING: it copies as always and watches the whole cycle (resolve without a clear by swapping, car pass
 *     on the same render target, resolve with a clear by swapping), that every draw of the car pass is exact
 *     (pass without color, without stencil, without an occlusion query and, if it writes Z, a NEVER, LESS or
 *     LEQUAL test) and that every read of texture[0] comes from a shader with tfetch2DSombraMin in that
 *     register and outside the window between the two resolves. Those reads already use the pipeline with
 *     the bit (minimum with itself: the same texel), so switching to applying does not create pipelines in
 *     the middle of a race. After kSombraMinimoCiclos clean cycles in a row it moves to
 *   - APPLYING: clears instead of copying. If anything watched fails, DIFERENCIA in the log (REXLOG_ERROR) and
 *   - OFF for the session: it copies again. A library without tfetch2DSombraMin never leaves watching.
 * Report line: "C2 sombra por minimo". false = always copy, as before.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_sombra_minimo, true, "NFSMW",
                    "Renderizador nativo (25/09, build 184): el mapa de sombras del mundo ya no se restaura copiando "
                    "1600x1600; los coches se dibujan sobre el destino borrado y el mundo muestrea el minimo de las dos "
                    "texturas. Misma imagen. Necesita la biblioteca con tfetch2DSombraMin. Se comprueba sola. false = "
                    "copiar como antes");
REXCVAR_DEFINE_INT32(nfsmw_nativo_sombra_minimo_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (25/09, build 184, prueba): con N > 0 y la guardia ya aplicando, alterna cada N "
                     "segundos el minimo (tramos pares) y la copia de siempre (impares), para medir la ganancia neta en la "
                     "consola con 'C2: GPU por Swap'. 0 = sin alternar");
/*
 * Repeated clears.
 *
 * Clears are 0.41 raw = 0.67 ms real per frame (9 per frame: 4 color and 5 depth).
 * vkCmdClearColorImage and vkCmdClearDepthStencilImage clear the whole image, and the game requests some
 * clears on a render target that is already cleared to that same value and has not been drawn to since.
 * That clear does not change a single bit.
 *
 * The condition is deliberately conservative: it is skipped only if (a) the last clear of that render
 * target used the same value, (b) the global draw counter has not gone up since then (meaning nothing has
 * been drawn anywhere) and (c) the render target has not changed image through a swap nor received a
 * restore. With that it is impossible to skip a clear that is needed.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_saltar_borrados_repetidos, true, "NFSMW",
                    "Renderizador nativo (20/09 noche): saltarse un borrado cuando el destino ya esta borrado "
                    "con ese mismo valor y no se ha dibujado nada desde entonces. No cambia ni un pixel; "
                    "'borrados saltados' en el informe C2 dice cuantos se ahorran");
/*
 * How much of each clear is used (nfsmw_nativo_diag_borrados).
 *
 * Clears are ~0.70 ms real per frame in a race (8 color and 9 depth) and they all clear the whole image:
 * 1280x1280 for a 1280x720 scene, 1040x1040 for a 1024x576 output, 320x720 for a 256x256 cubemap face. To
 * clear less, it must first be known, per render target, which part is really used until the next clear:
 * what the passes load and store (their renderArea, the useful area), the rectangles that are resolved,
 * what is restored and what is swapped (whole). Measurement only: it does not change the image. One line
 * every 20 s: "C2 borrados por destino". false = not measured.
 */
// false by default. Measured: the clip to the useful area that needs it only saved 0.09 ms of GPU, and the
// tracking runs on the PM4 ring thread, which is now the bottleneck.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_diag_borrados, false, "NFSMW",
                    "Renderizador nativo (25/09, build 184, diagnostico): por destino, area borrada frente a la que se "
                    "usa hasta el borrado siguiente (pases, resolves, restauraciones e intercambios). Una linea cada "
                    "20 s ('C2 borrados por destino'). No cambia la imagen");
/*
 * Color clears only over the area in use (nfsmw_nativo_borrar_area_util).
 *
 * vkCmdClearColorImage clears the whole image: 1280x1280 for a 1280x720 scene, 1040x1040 for the 1024x576
 * output, 320x720 for a 256x256 cubemap face. With the measurement of nfsmw_nativo_diag_borrados (the
 * largest height used by passes, resolves, restores and swaps of that render target, plus one row of margin,
 * rounded up to the next multiple of 64), once a render target has kAreaUtilCiclos measured clears only that
 * top band is cleared, with a loadOp = CLEAR pass (the same thing NVK does internally, over fewer pixels).
 * The bottom band is recorded with its color and cleared before any use that reaches it: the result is the
 * same as clearing it all. Draws do not touch it: NVK clips each pass to its renderArea (SET_SURFACE_CLIP)
 * and the pass that reaches the band completes it before it opens. If a render target needs that 3 times,
 * it stops being clipped (its useful area is not stable). Color only: depth without TRANSFER_DST is cleared
 * per pass and that pass clears the whole ZCULL region (NVK), so it is not clipped. GUARD: if a band cannot
 * be completed, DIFERENCIA in the log and it turns off for the session. false = clear the whole image, as
 * before.
 */
// false by default. Measured: 0.09 ms of GPU per frame (1.18 Mpixels not cleared): not worth it with the
// PM4 ring as the bottleneck.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_borrar_area_util, false, "NFSMW",
                    "Renderizador nativo (25/09, build 184): los borrados de color borran solo las filas que se usan de "
                    "verdad; el resto se borra antes de que algo lo use. Misma imagen. Necesita "
                    "nfsmw_nativo_diag_borrados. false = la imagen entera, como antes");
// Defined in nfsmw_nativo_dibujos.cpp; here it is only read so as not to open two queries of the same type
// at once.
REXCVAR_DECLARE(int32_t, nfsmw_nativo_estadisticas_por_dibujo_s);
// nfsmw_nativo_sombra_minimo only pays off with the single-sample PCF (defined in nfsmw_nativo_dibujos.cpp).
REXCVAR_DECLARE(bool, nfsmw_nativo_pcf_barato);
REXCVAR_DEFINE_BOOL(nfsmw_nativo_estadisticas_pipeline, false, "NFSMW",
                    "Renderizador nativo (17/09, build 156): cuenta fragmentos sombreados, invocaciones de "
                    "vertice y primitivas recortadas por tipo de pasada (informe C2). La imagen no cambia, "
                    "pero las consultas cuestan tiempo de GPU: solo para medir");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_diag_borrado, false, "NFSMW",
                    "Renderizador nativo: borrar cada destino de render con un color propio en "
                    "vez del color del juego (solo pruebas: comprueba copia, borrado y "
                    "presentacion)");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_diag_resueltas, false, "NFSMW",
                    "Renderizador nativo: presentar en mosaico las texturas resueltas de cada "
                    "fotograma, en el orden de sus copias (solo pruebas)");
/*
 * Who reads each resolved texture (nfsmw_nativo_diag_lectores_s, measurement only).
 *
 * "C2 caras resueltas" counts copies and reads per address, but a read is a call to TexturaResuelta and
 * only happens when the sampler caches of DibujosVulkan miss (per frame and per fetch constant): it does not
 * say which shader reads nor after which copy. With 098B0000 (the 1024x576 scene) that leaves the question
 * open: it is written twice per frame (the blit of the 1280x720 scene and, after the composition, the
 * composited scene 1 to 1) and it has two reads per frame. If both come from the first write, nobody reads
 * the second one before the next frame covers it entirely, and it is unnecessary (0.59 Mpixels, ~0.35 ms
 * real per frame).
 *
 * Every N seconds two whole frames are examined: every logical write to a resolved texture (the color or
 * depth resolve, even if the real copy is deferred or is a swap), every draw that samples it according to
 * the fetch constants of its pixel shader, bypassing the caches, and the Swap that draws the front buffer.
 * At the end, one line per address: for each write, who reads it until the next one (PS nN, sampler, draws
 * and the render target being drawn into) and SOBRA if nobody reads it and the next one covers it entirely.
 * What is read before the first write of the window goes to the last one before it ("[antes]"). The
 * per-address lines are only printed if their pattern changes (the first three windows, all of them); the
 * summary line always is.
 * A write that comes out as SOBRA stays watched in every frame (menus, pause, rain, cutscenes...),
 * separating the reads before its source is written again (a deferred copy covers them: it would be recorded
 * right before that draw; this is the case of the raindrops on the screen with 098B0000) from the ones after
 * (with a single one, dropping the copy when the source is written would not be exact). The first of each
 * kind is reported; the counts, in every summary. It is the evidence needed before deferring or dropping a
 * copy.
 * Cost: outside the window, one if per draw and per copy (and, with something watched, comparing each
 * sampler's address with 1-4 addresses); in the two frames of the window, one lookup in the resolved map
 * per sampler (~0.2-0.3 ms per frame). 0 = off.
 */
// 0 by default. Measured: its watching only served the lazy composite copy, which saves nothing.
REXCVAR_DEFINE_INT32(nfsmw_nativo_diag_lectores_s, 0, "NFSMW",
                     "Renderizador nativo (25/09, build 184): cada estos segundos se miran dos fotogramas enteros y se "
                     "escribe, por textura resuelta, que pixel shaders leen cada escritura antes de la siguiente "
                     "(lineas C2 lectores); lo que sale como SOBRA se vigila despues en todos los fotogramas. Solo "
                     "mide. 0 = apagado")
    .range(0, 3600);
/*
 * The composited scene is only copied if someone reads it (nfsmw_nativo_compuesta_perezosa).
 * The scene texture (098B0000 at 1024x576) is written twice per frame: the blit of the scene and, after the
 * composition, the composited scene 1 to 1 from the output (sub_82442478: VT, resolve and drops). The
 * second one is only read by the raindrops on the screen (sub_82448168, PS n145), which come right after and
 * before the HUD. It is deferred: it is recorded right before the first draw that samples it without its
 * source having been written again (exact), dropped if another write covers it entirely (exact) and dropped
 * when its source (the HUD) is written if nobody has read it: 0.59 Mpixels and ~0.35 ms real of GPU per
 * frame without drops. It only applies to the (address, source) pair that the watching of
 * nfsmw_nativo_diag_lectores_s has seen kCompuestaAMirar times with no read after the source was written,
 * and it turns itself off if a draw samples the texture after its copy was dropped (DIFERENCIA in the log).
 * Without the diagnostic (nfsmw_nativo_diag_lectores_s = 0) it never applies. false = always copy, as
 * before.
 */
// false by default. Measured: 19,895 of 21,041 copies were recorded anyway because a draw reads it almost
// every frame: 0 ms saved.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_compuesta_perezosa, false, "NFSMW",
                    "Renderizador nativo (25/09, build 184): la copia 1 a 1 de la escena compuesta a su textura "
                    "(098B0000, antes del HUD) se aplaza: se graba si un dibujo la muestrea antes de volver a escribir "
                    "su origen (las gotas de lluvia) y se tira si no. Se activa tras la vigilancia de "
                    "nfsmw_nativo_diag_lectores_s y se apaga sola al primer desacuerdo. false = se copia siempre");
REXCVAR_DEFINE_INT32(nfsmw_nativo_leer_resueltas_texels, 4096, "NFSMW",
                     "Renderizador nativo: las texturas resueltas de hasta estos texels se copian "
                     "tambien a la memoria del invitado, que el juego lee para su exposicion (0 = "
                     "ninguna; 4096 = 64x64, las que usa la exposicion; 57600 = 320x180)");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_invalidar_texturas_cada_copia, false, "NFSMW",
                    "Renderizador nativo: tirar las caches de texturas en cada copia (lo de antes de la build 127). "
                    "Desde la 127 solo se tiran cuando una textura resuelta se crea, se rehace, se prepara o cambia "
                    "su orden de canales");
/*
 * On by default. It was written earlier and left off without being measured.
 *
 * The baseline without overclock shows the ring waiting 17.24 ms per frame for the previous presentation
 * to finish: almost exactly one 60 Hz vsync. The frame's work is ~38 ms, i.e. 2.3 vsyncs, and it should land
 * on 3 (50 ms, 20 FPS); with that wait on top it goes to almost 4 (62 ms, 16 FPS).
 *
 * And it fits the SDK, which already has a mailbox of three output images made for this:
 *     static constexpr uint32_t kGuestOutputMailboxSize = 3;
 *     // mailbox for presenting ... without long interlocking between guest output refreshing and painting
 * The SDK hands out a different image on each refresh, so painting the next one while the previous one is
 * presented is the intended use, not a shortcut. It does not change the image.
 *
 * To compare without a new NRO: set it to false in the toml and repeat the race.
 */
/*
 * How many work slots are really used (there is room for 3).
 *
 * With 2 the CPU can only be one frame ahead of the GPU, and on the console that meant 4,826 ms out of
 * every 10 seconds stopped inside Grabar() waiting for the GPU to finish the previous work. With 3 it can
 * be two ahead. Each slot costs a 64 MB upload buffer.
 *
 * It is left selectable to compare without a new NRO: with 2, exactly the earlier behavior.
 */
/*
 * Four. Breaking a console run down by regime showed that the bottleneck changes with the stretch of track,
 * and that the third slot falls short exactly where the GPU is the limit:
 *
 *   few draws (straight):    GPU 31.37 ms | gap 2.05 | fence wait inside Grabar() 4.9-9.8 ms
 *   many draws (alley):      GPU 30.30 ms | gap 5.50 | fence wait ~0
 *
 * That is: on the straight the CPU arrives first and waits for the GPU to release the slot; in the alley it
 * is the other way round. With four slots the CPU can be three frames ahead and stops stalling in the first
 * case. It fits easily: the log says "monton 0 (GPU): 482 MB usados de 1382 MB presupuestados", and each
 * slot costs a 64 MB upload buffer.
 */
/*
 * It stays at three. The fourth one breaks the image, and the reason is known.
 *
 * A build with only this change (sky deferral off) showed the same artifacts seen before: flickering and
 * odd colors. So the fourth slot shared the blame, it was not innocent.
 *
 * The cause: the work slots and the output slots are two different things and only one is configurable.
 *
 *     work slots   (nfsmw_nativo_ranuras_trabajo):  3 -> 4
 *     output slots (kRanurasSalida, :486):          3   FIXED
 *
 * `kRanurasSalida` governs the quad that draws the game image on screen: its pools, its descriptors, its
 * fences (`fences_salida_`) and `salidas_pendientes_`, and the rotation at :3305 is `% kRanurasSalida`.
 * Letting the CPU get three frames ahead in the work reuses an output slot whose image is still being
 * displayed. Hence the artifacts.
 *
 * To raise it, kRanurasSalida must be raised at the same time and the four places above reviewed.
 * Changing only this number is not enough.
 *
 * -------- what was believed before, kept so that it is not repeated --------
 * Four, and this time on its own.
 *
 * It first went in together with the sky deferral and the lazy sealing. That build broke the image and all
 * three were reverted without knowing which one it was; then the sky counter showed the culprit was the
 * sky ("7,00 detectados por fotograma" when 0.9 was expected). So this one comes back on its own, which
 * is how it should have been added from the start.
 *
 * What it buys, measured per regime:
 *   few draws (straight):    GPU 31.37 ms | gap 2.05 | waiting for the fence in Grabar() 4.9-9.8 ms
 *   many draws (alley):      GPU 30.30 ms | gap 5.50 | waiting for the fence ~0
 *
 * On the straight the CPU arrives first and stalls waiting for the GPU to release the slot. With four it is
 * three frames ahead. It costs 64 MB of upload buffer, out of the ~900 MB free in the heap.
 */
REXCVAR_DEFINE_INT32(nfsmw_nativo_ranuras_trabajo, 3, "NFSMW",
                     "Renderizador nativo: ranuras de trabajo (2 a 4). Con mas, la CPU va mas fotogramas por "
                     "delante de la GPU y se para menos; cada una cuesta 64 MB. 2 = como la compilacion 80")
    .range(2, 4)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// ZCULL (hierarchical depth culling), switchable.
//
// It is enabled by removing TRANSFER_DST from the game's depth buffers, which is what makes them eligible
// for a ZCULL plane in the driver. It measured -2.12 ms in the scene, but right after that the car body was
// observed darkening at times as if it were in shadow, and the scene and the reflection cubemap are exactly
// what ends up under ZCULL (the shadow map does not: it has TRANSFER_DST). It stayed off until a race ruled
// that out.
/*
 * On by default. It had been turned off on suspicion of darkening the car body, and that suspicion was
 * ruled out: the change of tone was the environment cubemap with caras_max=1 refreshing one face every six
 * frames, so the car's lighting lagged and then jumped. ZCULL is worth -2.12 ms measured and was already
 * enabled by hand in the test toml; the default value was misleading.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_zcull, true, "NFSMW",
                    "Descarte jerarquico de profundidad (ZCULL): quita TRANSFER_DST a las "
                    "profundidades del juego para que el driver les de plano de ZCULL");

REXCVAR_DEFINE_BOOL(nfsmw_nativo_salida_sin_espera, true, "NFSMW",
                    "Renderizador nativo: pinta la salida de cada Swap rotando 3 ranuras en vez de una, para no "
                    "esperar a que la GPU termine la salida anterior (en la consola el anillo esperaba ahi 17 ms "
                    "por Swap, casi un vsync). No cambia la imagen; false vuelve al comportamiento de antes");
// In NVK, TOP_OF_PIPE is PIPELINE_LOCATION_NONE: the timestamp is released when the GPU reads the command,
// not when the previous work finishes, and the per-category breakdown is approximate (a copy gets charged
// with the draw of the previous pass). BOTTOM_OF_PIPE is PIPELINE_LOCATION_ALL: it is released when all
// previous work finishes.
/*
 * On by default. It was only enabled through the toml, and the toml overrides the default: if the toml
 * ever ships without that line, the improvement silently disappears. Three cvars like this were found, one
 * of them worth 2 ms and dead for several builds.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_marcas_precisas, true, "NFSMW",
                    "Renderizador nativo (medida): marcas de tiempo de GPU entre categorias con BOTTOM_OF_PIPE (se "
                    "sueltan al terminar lo anterior) en vez de TOP_OF_PIPE (al leer el comando): reparto por "
                    "categorias exacto. No cambia la imagen");
REXCVAR_DEFINE_INT32(nfsmw_nativo_marcas_precisas_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (medida): con N > 0 alterna marcas normales (tramos pares) y precisas "
                     "(tramos impares) cada N segundos y anota cada cambio, para comparar en la misma ejecucion");
REXCVAR_DEFINE_INT32(nfsmw_nativo_lecturas_cada, 1, "NFSMW",
                     "Renderizador nativo: de cada destino se lee y escribe en la memoria del invitado "
                     "una de cada N copias pequenas (1 = todas); el juego las usa para su exposicion");
// The Xbox 360 passes the image through the gamma ramp the game loads, and NFSMW does not load the identity
// (measured: [64] = 273 and [128] = 539 in 10 bits, instead of 256 and 513).
REXCVAR_DEFINE_BOOL(nfsmw_nativo_rampa_gamma, true, "NFSMW",
                    "Renderizador nativo: aplica en la salida la rampa de gamma que carga el juego, como la pantalla "
                    "de la Xbox 360 (sin ella, los medios tonos y las sombras salen mas oscuros). false: la imagen tal "
                    "cual, como antes de la build 137")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The 30 FPS guard (nfsmw_guardia_30) lives in nfsmw_recorte_sombras.cpp, which owns the lever it pulls.
namespace nfsmw::guard30 {
void Latir(double ms);
void Report();
}  // namespace nfsmw::guardia30

namespace nfsmw::native_renderer {
namespace shaders {
// The same SPIR-V the SDK presenter uses to draw the game image
// (vulkan_presenter.cpp:128-141).
#include "vulkan_spirv/guest_output_bilinear_ps.h"
#include "vulkan_spirv/guest_output_triangle_strip_rect_vs.h"
// The same sampling as guest_output_bilinear_ps with the game's gamma ramp
// (shaders/output_gamma_ramp.frag).
#include "shaders/output_gamma_ramp_ps.h"
}  // namespace shaders

namespace {

namespace xenos = rex::graphics::xenos;
using rex::ui::vulkan::VulkanDevice;
using rex::ui::vulkan::VulkanPresenter;

constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;

// AirborneNX: MoH: Airborne (UE3, HDR) also renders to 7e3, 2_10_10_10 and 16-bit targets. Host format of
// each Xenos color render target format; VK_FORMAT_UNDEFINED = not supported. 7e3 and the 16-bit formats
// keep the shader's raw values in half floats (the emulation's host path does the same for 7e3); 16_16 and
// 16_16_16_16 lose their -32..32 clamp. The resolved texture takes the render target's host format, so a
// resolve stays a plain image copy and the draws sample the host image.
VkFormat TargetHostFormat(uint32_t format_id) {
  switch (xenos::ColorRenderTargetFormat(format_id)) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    default:
      return VK_FORMAT_UNDEFINED;
  }
}

// RB_COLOR_CLEAR (+ RB_COLOR_CLEAR_LO << 32) in the render target's format, decoded like the emulation
// (vulkan/render_target_cache.cpp, resolve clear).
VkClearColorValue ClearColor(uint32_t format_id, uint64_t value) {
  using F = xenos::ColorRenderTargetFormat;
  VkClearColorValue color{};
  switch (F(format_id)) {
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_10_10_10_10:
      for (uint32_t j = 0; j < 3; ++j) {
        color.float32[j] = float((value >> (j * 10)) & 0x3FF) * (1.0f / 1023.0f);
      }
      color.float32[3] = float((value >> 30) & 0x3) * (1.0f / 3.0f);
      break;
    case F::k_2_10_10_10_FLOAT:
    case F::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      for (uint32_t j = 0; j < 3; ++j) {
        color.float32[j] = xenos::Float7e3To32(uint32_t(value >> (j * 10)) & 0x3FF);
      }
      color.float32[3] = float((value >> 30) & 0x3) * (1.0f / 3.0f);
      break;
    case F::k_16_16:
    case F::k_16_16_FLOAT:
    case F::k_16_16_16_16:
    case F::k_16_16_16_16_FLOAT: {
      const bool es_float = F(format_id) == F::k_16_16_FLOAT || F(format_id) == F::k_16_16_16_16_FLOAT;
      for (uint32_t j = 0; j < 4; ++j) {
        const uint16_t componente = uint16_t((value >> (j * 16)) & 0xFFFF);
        color.float32[j] = es_float ? rex::xenos_half_to_float(componente)
                                    : std::max(float(int16_t(componente)) * (1.0f / 32767.0f), -1.0f);
      }
      break;
    }
    case F::k_32_FLOAT:
    case F::k_32_32_FLOAT:
      for (uint32_t j = 0; j < 2; ++j) {
        const uint32_t bits = uint32_t(value >> (j * 32));
        std::memcpy(&color.float32[j], &bits, sizeof(bits));
      }
      break;
    default:
      for (uint32_t j = 0; j < 4; ++j) {
        color.float32[j] = float((value >> (j * 8)) & 0xFF) * (1.0f / 255.0f);
      }
      break;
  }
  return color;
}

// log2 of the bytes per texel of a resolve destination (GetResolveInfo aligns the base to 32 texels).
uint32_t CopyBytesLog2(uint32_t target_format) {
  using F = xenos::ColorFormat;
  switch (F(target_format)) {
    // The *_AS_16_16_16_16 formats are 32 bpp (only the sampling precision is 16 bits).
    case F::k_16_16_16_16:
    case F::k_16_16_16_16_FLOAT:
    case F::k_32_32_FLOAT:
      return 3;
    case F::k_32_32_32_32_FLOAT:
      return 4;
    case F::k_5_6_5:
    case F::k_1_5_5_5:
    case F::k_4_4_4_4:
    case F::k_16:
    case F::k_16_FLOAT:
      return 1;
    case F::k_8:
      return 0;
    default:
      return 2;
  }
}
constexpr uint32_t kTargetMaxHeight = 2048;
constexpr VkImageSubresourceRange kColorRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
constexpr VkImageSubresourceRange kDepthRange = {
    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
using CopyImageFn = void(VKAPI_PTR*)(VkCommandBuffer, VkImage, VkImageLayout, VkImage,
                                        VkImageLayout, uint32_t, const VkImageCopy*);
using ClearDepthFn = void(VKAPI_PTR*)(VkCommandBuffer, VkImage, VkImageLayout,
                                             const VkClearDepthStencilValue*, uint32_t,
                                             const VkImageSubresourceRange*);
using FnBlit = void(VKAPI_PTR*)(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout,
                                uint32_t, const VkImageBlit*, VkFilter);

// AirborneNX: NFSMW records its copies without barriers (on NVK a barrier drains the pipeline and the
// ordering it relied on happened to hold). On desktop drivers a copy then overlaps the passes around it:
// the particles read the resolved scene depth half copied (a diagonal edge and 32x32 blocks). With
// airborne_barreras_copias every copy and blit waits for the writes before it and is visible to what
// follows. One device per process, so the wrapped functions live in statics.
struct CopiesWithBarriers {
  static inline PFN_vkCmdPipelineBarrier barrier = nullptr;
  static inline CopyImageFn copy_bytes = nullptr;
  static inline FnBlit blit = nullptr;
  static constexpr VkPipelineStageFlags kStages =
      VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
  static void Before(VkCommandBuffer cb) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier(cb, kStages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
  }
  static void After(VkCommandBuffer cb) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, kStages, 0, 1, &b, 0, nullptr, 0, nullptr);
  }
  static void VKAPI_PTR CopyBytes(VkCommandBuffer cb, VkImage o, VkImageLayout lo, VkImage d, VkImageLayout ld,
                               uint32_t n, const VkImageCopy* r) {
    Before(cb);
    copy_bytes(cb, o, lo, d, ld, n, r);
    After(cb);
  }
  static void VKAPI_PTR Blit(VkCommandBuffer cb, VkImage o, VkImageLayout lo, VkImage d, VkImageLayout ld,
                             uint32_t n, const VkImageBlit* r, VkFilter f) {
    Before(cb);
    blit(cb, o, lo, d, ld, n, r, f);
    After(cb);
  }
};

// Direct3D 11 16.8 fixed point with rounding, like ui::FloatToD3D11Fixed16p8.
int32_t Fixed16p8(float value) {
  if (!(std::abs(value) >= 1.0f / 512.0f)) {
    return 0;
  }
  const double scaled = std::clamp(double(value) * 256.0, -2147483392.0, 2147483392.0);
  return int32_t(std::lround(scaled));
}

// Address of a texel in a 32x32-tiled texture (pipeline/texture/util.cpp:424-436).
int32_t Tile2dOffset(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bytes + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bytes;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// With 4-byte texels, DesplazamientoMosaico2D(x, y, pitch, 2) is
// (((x >> 5) + (y >> 5) * (pitch aligned to 32 >> 5)) << 12) + table[(y & 31) * 32 + (x & 31)]:
// the part inside the 32x32 tile does not depend on the pitch (EscribirLecturas).
const std::array<uint16_t, 1024>& Tile2dTexel4Table() {
  static const std::array<uint16_t, 1024> table = [] {
    std::array<uint16_t, 1024> t{};
    for (int32_t y = 0; y < 32; ++y) {
      for (int32_t x = 0; x < 32; ++x) {
        t[size_t(y) * 32 + size_t(x)] = uint16_t(Tile2dOffset(x, y, 32, 2));
      }
    }
    return t;
  }();
  return table;
}

int32_t SignExtend15(uint32_t value) {
  return int32_t(value << 17) >> 17;
}

// Fixed color per render target with nfsmw_nativo_diag_borrado: the
// capture shows which render target reached the screen.
VkClearColorValue DiagnosticColor(uint32_t base, uint32_t format_id, uint32_t pitch) {
  uint32_t h = base * 2654435761u ^ pitch * 2246822519u ^ format_id * 3266489917u;
  h ^= h >> 15;
  h *= 2246822519u;
  h ^= h >> 13;
  VkClearColorValue color{};
  for (uint32_t j = 0; j < 3; ++j) {
    color.float32[j] = float((h >> (j * 8)) & 0xFF) * (1.0f / 255.0f);
  }
  color.float32[3] = 1.0f;
  return color;
}

using Image = NativeImage;  // preparada = already in GENERAL and cleared

struct Resolved {
  Image img;
  uint32_t guest_format = 0;
  bool rb_swap = false;
  // Times the draws have requested this address since the last report. It lives here and not in the report
  // map because TexturaResuelta is on the hot path (one call per texture and draw, ~8,000 per frame) and the
  // lookup of the resolved texture is done anyway.
  uint64_t reads = 0;
};

// C2 report of readbacks per render target (base and size) and cadence of nfsmw_nativo_lecturas_cada.
struct ReadbackTarget {
  uint32_t base = 0;
  uint32_t width_px = 0;
  uint32_t height_px = 0;
  uint64_t copies = 0;    // small copies seen in the interval
  uint64_t skipped = 0;  // of those, the ones not read because of nfsmw_nativo_lecturas_cada
};

// Rear-view mirror diagnostic: copies to a square resolved texture (AnotarCopia).
// And of all the others too, with their size and how many times each one is read. That answers whether a
// copy is needed: a resolved texture that is copied every frame and never requested is wasted bandwidth.
struct TargetCopy {
  uint32_t width_px = 0;
  uint32_t height_px = 0;
  uint64_t copies = 0;
  uint64_t with_draws = 0;  // copies with some draw since the previous copy
  uint64_t draws = 0;
  uint64_t pixels = 0;
};

// What is known about a render target between game commands. It serves two purposes:
//  - skipping a clear that changes nothing (nfsmw_nativo_saltar_borrados_repetidos);
//  - restoring only the rows the game really uses (nfsmw_nativo_restaurar_area_util).
// It lives in a separate map and not in ImagenNativa because that structure belongs to another file.
struct TargetState {
  bool clean_clear = false;    // the content is exactly the last clear, with nothing on top
  uint64_t clear_value = 0;     // color empaquetado, o profundidad+stencil
  uint64_t draws_on_clear = 0; // global draw counter at that moment
  uint32_t used_height = 0;        // the largest y1 the game has resolved from this render target
};

// Readback of a small resolved texture (nfsmw_nativo_leer_resueltas_texels):
// host-visible buffer it is copied to and what is needed to write it to the guest.
struct ReadInfo {
  VkBuffer buf = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  uint8_t* payload = nullptr;
  VkDeviceSize bytes = 0;
  bool coherente = true;
};
struct PendingReadback {
  ReadInfo* readback;
  uint32_t base;          // RB_COPY_DEST_BASE
  int32_t x0;             // texel of the texture where the rectangle starts
  int32_t y0;
  uint32_t width_px;
  uint32_t height_px;
  uint32_t pitch;         // RB_COPY_DEST_PITCH
  uint32_t target_height;
  uint32_t info;          // RB_COPY_DEST_INFO
};

// Output slots with nfsmw_nativo_salida_sin_espera (without it, only slot 0 is used).
constexpr uint32_t kOutputSlots = 3;
// GPU timestamps per slot (the last one is kept for the final timestamp).
constexpr uint32_t kMarksPerSlot = 128;
constexpr uint8_t kGpuEnd = 0xFF;
// Host occlusion queries per work unit (draw spans of the game's queries).
constexpr uint32_t kOclusionesPorRanura = 32;
// nfsmw_reflejo_visibilidad. Own occlusion queries per work unit: the draws that sample the reflection and
// the witness (the final composition). The ones that do not fit are assumed visible.
constexpr uint32_t kVisibilityPerSlot = 16;
constexpr uint8_t kWaterVisibility = 0;
constexpr uint8_t kSentinelVisibility = 1;
// Passes measured per work unit. In a race there are ~16 per frame including the resumed ones.
constexpr uint32_t kStatsPerSlot = 64;
constexpr uint32_t kStatCounters = 3;  // vertices, primitivas recortadas y fragmentos
// Draws measured in a diagnostic frame (the scene has ~1200).
constexpr uint32_t kDrawStatsPerSlot = 2048;
constexpr uint32_t kShaderLabels = 512;
// Buckets of the copy breakdown by size (pixels of the copy).
constexpr uint32_t kCopyBuckets = 4;
constexpr uint32_t kBucketPixels[kCopyBuckets] = {64 * 64, 320 * 320, 1024 * 1024, 0xFFFFFFFFu};

// One of the work slots: while a frame is being recorded, the previous ones can still be
// on the GPU with the other one.
struct JobSlot {
  VkCommandPool job_pool = VK_NULL_HANDLE;
  VkCommandPool upload_pool = VK_NULL_HANDLE;
  VkCommandBuffer job = VK_NULL_HANDLE;
  VkCommandBuffer upload = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool pending = false;
  uint64_t orden = 0;                        // numero de envio
  // Wall-clock time of the work, from vkQueueSubmit to the signaled fence. The GPU timestamps give
  // 30.5 ms per frame while the hardware's own counter says 99.7 % load on a 55 ms frame: either there is
  // work we do not mark or the timestamp scale is wrong. This bounds the truth from above (the CPU sees
  // the fence a little late) and the timestamps from below.
  std::chrono::steady_clock::time_point sent{};
  std::vector<PendingReadback> reads;  // readbacks submitted with the work
  std::vector<uint8_t> categories;  // GPU category of each timestamp written
  bool precise_marks = false;      // intermediate marks with BOTTOM_OF_PIPE
  // Occlusion queries recorded in the work, in order from the slot's first one, with the number of the game
  // query they add to.
  std::vector<std::pair<uint32_t, uint64_t>> oclusiones;
  // Statistics queries recorded, with the category of each one's pass.
  std::vector<std::pair<uint32_t, uint8_t>> stats;
  // Per-draw queries, with the pixel shader number of each one.
  std::vector<std::pair<uint32_t, uint16_t>> draw_stats;
  // Reflection visibility queries, with their type (kVisibilidadAgua or kVisibilidadTestigo).
  std::vector<std::pair<uint32_t, uint8_t>> visibility;
};

// A game occlusion query (from its Issue(BEGIN) to its Issue(END)) while its spans are being counted on the
// GPU.
struct GameOcclusionQuery {
  uint32_t base = 0;              // D3D counter structure
  uint64_t sample_count = 0;          // sum of the spans read
  uint32_t pending_segments = 0;  // recorded and not yet read
  bool finished = false;         // ya llego su Issue(END)
  bool failed = false;           // some span without room or not read: not published
};

class VulkanTargets final : public NativeTargets, public TargetsContext {
 public:
  VulkanTargets(const VulkanDevice* dev, rex::memory::Memory* mem)
      : vk_device_(dev),
        dfn_(dev->functions()),
        device_(dev->device()),
        mem_(mem),
        family_(dev->queue_family_graphics_compute()) {}

  ~VulkanTargets() override {
    WaitGpu();
    draws_.reset();  // their framebuffers and views point to these images
    for (auto& [key, img] : depths_) {
      Destroy(img);
    }
    for (auto& [key, img] : targets_) {
      Destroy(img);
    }
    for (auto& [key, resolved] : resolved_) {
      Destroy(resolved.img);
    }
    Destroy(tile_);
    for (FrontImage& spare : images_fronts_) {  // nfsmw_nativo_frontal_perezoso
      Destroy(spare.img);
    }
    pending_reads_.clear();
    for (JobSlot& ranura : ranuras_) {
      ranura.reads.clear();
    }
    for (auto& [key, readback] : reads_) {
      DestroyRead(readback);
    }
    for (auto& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE) {
        dfn_.vkDestroyFramebuffer(device_, f.framebuffer, nullptr);
      }
    }
    if (pipeline_ != VK_NULL_HANDLE) dfn_.vkDestroyPipeline(device_, pipeline_, nullptr);
    for (VkPipeline p : ramp_pipelines_) {
      if (p != VK_NULL_HANDLE) dfn_.vkDestroyPipeline(device_, p, nullptr);
    }
    if (fs_ramp_ != VK_NULL_HANDLE) dfn_.vkDestroyShaderModule(device_, fs_ramp_, nullptr);
    for (OutputRamp& ramp : output_ramps_) {
      if (ramp.buf != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, ramp.buf, nullptr);
      if (ramp.mem != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, ramp.mem, nullptr);
    }
    if (layout_pipeline_ != VK_NULL_HANDLE)
      dfn_.vkDestroyPipelineLayout(device_, layout_pipeline_, nullptr);
    if (descriptors_pool_ != VK_NULL_HANDLE)
      dfn_.vkDestroyDescriptorPool(device_, descriptors_pool_, nullptr);
    if (descriptors_layout_ != VK_NULL_HANDLE)
      dfn_.vkDestroyDescriptorSetLayout(device_, descriptors_layout_, nullptr);
    if (sampler_ != VK_NULL_HANDLE) dfn_.vkDestroySampler(device_, sampler_, nullptr);
    if (vs_ != VK_NULL_HANDLE) dfn_.vkDestroyShaderModule(device_, vs_, nullptr);
    if (fs_ != VK_NULL_HANDLE) dfn_.vkDestroyShaderModule(device_, fs_, nullptr);
    if (output_render_pass_ != VK_NULL_HANDLE)
      dfn_.vkDestroyRenderPass(device_, output_render_pass_, nullptr);
    for (JobSlot& ranura : ranuras_) {
      if (ranura.fence != VK_NULL_HANDLE) dfn_.vkDestroyFence(device_, ranura.fence, nullptr);
      if (ranura.job_pool != VK_NULL_HANDLE)
        dfn_.vkDestroyCommandPool(device_, ranura.job_pool, nullptr);
      if (ranura.upload_pool != VK_NULL_HANDLE)
        dfn_.vkDestroyCommandPool(device_, ranura.upload_pool, nullptr);
    }
    if (queries_ != VK_NULL_HANDLE) dfn_.vkDestroyQueryPool(device_, queries_, nullptr);
    if (oclusiones_ != VK_NULL_HANDLE) dfn_.vkDestroyQueryPool(device_, oclusiones_, nullptr);
    if (visibility_ != VK_NULL_HANDLE) dfn_.vkDestroyQueryPool(device_, visibility_, nullptr);
    for (uint32_t i = 0; i < kOutputSlots; ++i) {
      if (output_fences_[i] != VK_NULL_HANDLE) dfn_.vkDestroyFence(device_, output_fences_[i], nullptr);
      if (output_pools_[i] != VK_NULL_HANDLE) dfn_.vkDestroyCommandPool(device_, output_pools_[i], nullptr);
    }
  }

  bool Initialize() {
    // vkCmdCopyImage is not in the SDK's function table: it is requested from the driver.
    copy_image_ = reinterpret_cast<CopyImageFn>(
        vk_device_->vulkan_instance()->functions().vkGetDeviceProcAddr(device_,
                                                                          "vkCmdCopyImage"));
    if (!copy_image_) {
      REXLOG_ERROR("[nativo] C2: el driver no da vkCmdCopyImage");
      return false;
    }
    clear_depth_ = reinterpret_cast<ClearDepthFn>(
        vk_device_->vulkan_instance()->functions().vkGetDeviceProcAddr(
            device_, "vkCmdClearDepthStencilImage"));
    blit_ = reinterpret_cast<FnBlit>(
        vk_device_->vulkan_instance()->functions().vkGetDeviceProcAddr(device_,
                                                                          "vkCmdBlitImage"));
    if (REXCVAR_GET(airborne_barreras_copias)) {
      CopiesWithBarriers::barrier = dfn_.vkCmdPipelineBarrier;
      CopiesWithBarriers::copy_bytes = copy_image_;
      copy_image_ = &CopiesWithBarriers::CopyBytes;
      if (blit_) {
        CopiesWithBarriers::blit = blit_;
        blit_ = &CopiesWithBarriers::Blit;
      }
      REXLOG_INFO("[nativo] C2: copias con barreras (airborne_barreras_copias)");
    }
    // Attachment, copy and clear source and destination, and sampling of the resolved ones (shadows).
    const VkFormatFeatureFlags kDepthUses =
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    // To draw the shadow map smaller it has to be scaled up when resolving it, and that is a vkCmdBlitImage on
    // depth. If the driver does not provide it, nothing is scaled.
    const VkFormatFeatureFlags kScaledUses =
        VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    VkFormatFeatureFlags chosen_uses = 0;
    for (const VkFormat candidate : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}) {
      VkFormatProperties props{};
      vk_device_->vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(
          vk_device_->physical_device(), candidate, &props);
      const VkFormatFeatureFlags uses = props.optimalTilingFeatures & kDepthUses;
      if (!(uses & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        continue;
      }
      if (depth_format_ == VK_FORMAT_UNDEFINED || uses == kDepthUses) {
        depth_format_ = candidate;
        chosen_uses = uses;
        scalable_depth_ =
            (props.optimalTilingFeatures & kScaledUses) == kScaledUses;
      }
      if (uses == kDepthUses) {
        break;
      }
    }
    if (depth_format_ != VK_FORMAT_UNDEFINED) {
      const VkFormatFeatureFlags missing = kDepthUses & ~chosen_uses;
      REXLOG_INFO("[nativo] C2: formato de profundidad {}{}{}{}",
                  depth_format_ == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8",
                  (missing & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? ", sin muestreo" : "",
                  (missing & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) ? ", sin origen de copias" : "",
                  (missing & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) ? ", sin destino de copias" : "");
      if (!scalable_depth_) {
        REXLOG_INFO("[nativo] C2: el driver no escala profundidad: el mapa de sombras se queda a su tamano");
      }
    }
    // One pool per command buffer: the SDK table does not include
    // vkResetCommandBuffer either, so the whole pool is reset.
    VkCommandPoolCreateInfo info_pool{};
    info_pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info_pool.queueFamilyIndex = family_;
    VkCommandBufferAllocateInfo reservation{};
    reservation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    reservation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    reservation.commandBufferCount = 1;
    VkFenceCreateInfo info_fence{};
    info_fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    for (JobSlot& ranura : ranuras_) {
      if (dfn_.vkCreateCommandPool(device_, &info_pool, nullptr, &ranura.job_pool) !=
              VK_SUCCESS ||
          dfn_.vkCreateCommandPool(device_, &info_pool, nullptr, &ranura.upload_pool) !=
              VK_SUCCESS ||
          dfn_.vkCreateFence(device_, &info_fence, nullptr, &ranura.fence) != VK_SUCCESS) {
        return false;
      }
      reservation.commandPool = ranura.job_pool;
      if (dfn_.vkAllocateCommandBuffers(device_, &reservation, &ranura.job) != VK_SUCCESS) {
        return false;
      }
      reservation.commandPool = ranura.upload_pool;
      if (dfn_.vkAllocateCommandBuffers(device_, &reservation, &ranura.upload) != VK_SUCCESS) {
        return false;
      }
    }
    for (uint32_t i = 0; i < kOutputSlots; ++i) {
      if (dfn_.vkCreateCommandPool(device_, &info_pool, nullptr, &output_pools_[i]) != VK_SUCCESS) {
        return false;
      }
      reservation.commandPool = output_pools_[i];
      if (dfn_.vkAllocateCommandBuffers(device_, &reservation, &output_commands_[i]) != VK_SUCCESS) {
        return false;
      }
      if (dfn_.vkCreateFence(device_, &info_fence, nullptr, &output_fences_[i]) != VK_SUCCESS) {
        return false;
      }
    }
    used_slots_ = uint32_t(std::clamp(REXCVAR_GET(nfsmw_nativo_ranuras_trabajo), 2,
                                          int32_t(ranuras_.size())));
    REXLOG_INFO("[nativo] C2: ranuras de trabajo (nfsmw_nativo_ranuras_trabajo) = {} de {}", used_slots_,
                ranuras_.size());
    output_without_wait_ = REXCVAR_GET(nfsmw_nativo_salida_sin_espera);
    invalidate_every_copy_ = REXCVAR_GET(nfsmw_nativo_invalidar_texturas_cada_copia);
    REXLOG_INFO("[nativo] C2: caches de texturas tiradas en cada copia (nfsmw_nativo_invalidar_texturas_cada_copia) = {}",
                invalidate_every_copy_ ? "SI" : "no");
    REXLOG_INFO("[nativo] C2: salida sin esperar a la anterior (nfsmw_nativo_salida_sin_espera) = {}",
                output_without_wait_ ? "SI" : "no");
    precise_marks_ = REXCVAR_GET(nfsmw_nativo_marcas_precisas);
    toggle_marks_s_ = REXCVAR_GET(nfsmw_nativo_marcas_precisas_alternar_s);
    marks_start_ = std::chrono::steady_clock::now();
    REXLOG_INFO("[nativo] C2: marcas de GPU precisas (nfsmw_nativo_marcas_precisas) = {}; alternar cada {} s",
                precise_marks_ ? "SI" : "no", toggle_marks_s_);
    // GPU time per work unit: two timestamps per slot. Without timestamp bits on the queue, it is not measured.
    {
      const auto& ifn = vk_device_->vulkan_instance()->functions();
      uint32_t families = 0;
      ifn.vkGetPhysicalDeviceQueueFamilyProperties(vk_device_->physical_device(), &families,
                                                   nullptr);
      std::vector<VkQueueFamilyProperties> queues(families);
      ifn.vkGetPhysicalDeviceQueueFamilyProperties(vk_device_->physical_device(), &families,
                                                   queues.data());
      VkPhysicalDeviceProperties phys{};
      ifn.vkGetPhysicalDeviceProperties(vk_device_->physical_device(), &phys);
      write_mark_ = reinterpret_cast<WriteMarkFn>(
          ifn.vkGetDeviceProcAddr(device_, "vkCmdWriteTimestamp"));
      read_queries_ = reinterpret_cast<ReadQueriesFn>(
          ifn.vkGetDeviceProcAddr(device_, "vkGetQueryPoolResults"));
      if (family_ < families && queues[family_].timestampValidBits && write_mark_ &&
          read_queries_ && phys.limits.timestampPeriod > 0.0f) {
        VkQueryPoolCreateInfo queries_info{};
        queries_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        queries_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queries_info.queryCount = uint32_t(ranuras_.size() * kMarksPerSlot);
        if (dfn_.vkCreateQueryPool(device_, &queries_info, nullptr, &queries_) != VK_SUCCESS) {
          queries_ = VK_NULL_HANDLE;
        }
        mark_period_ns_ = phys.limits.timestampPeriod;
      }
      REXLOG_INFO("[nativo] C2: tiempo de GPU por Swap {}",
                  queries_ != VK_NULL_HANDLE ? "medido con marcas de tiempo"
                                               : "no disponible (la cola no tiene marcas)");
      // The game's occlusion queries (the sun flare), counted on the GPU.
      VkQueryPoolCreateInfo info_oclusiones{};
      info_oclusiones.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
      info_oclusiones.queryType = VK_QUERY_TYPE_OCCLUSION;
      info_oclusiones.queryCount = uint32_t(ranuras_.size() * kOclusionesPorRanura);
      if (!read_queries_ ||
          dfn_.vkCreateQueryPool(device_, &info_oclusiones, nullptr, &oclusiones_) != VK_SUCCESS) {
        oclusiones_ = VK_NULL_HANDLE;
      }
      precise_occlusion_ = vk_device_->properties().occlusionQueryPrecise;
      REXLOG_INFO("[nativo] C2: consultas de oclusion del host {} ({})",
                  oclusiones_ != VK_NULL_HANDLE ? "disponibles" : "no disponibles: cuenta fingida",
                  precise_occlusion_ ? "precisas" : "sin precision: cuentan si hubo alguna muestra");
      // nfsmw_reflejo_visibilidad, in a separate pool so as not to touch the count of the game's queries.
      if (nfsmw::demand_reflection::MeasureVisibility() && read_queries_) {
        VkQueryPoolCreateInfo visibility_info{};
        visibility_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        visibility_info.queryType = VK_QUERY_TYPE_OCCLUSION;
        visibility_info.queryCount = uint32_t(ranuras_.size() * kVisibilityPerSlot);
        if (dfn_.vkCreateQueryPool(device_, &visibility_info, nullptr, &visibility_) != VK_SUCCESS) {
          visibility_ = VK_NULL_HANDLE;
        }
        REXLOG_INFO("[nativo] C2: visibilidad del reflejo (build 192): {}",
                    visibility_ != VK_NULL_HANDLE ? "consultas disponibles"
                                                   : "SIN consultas: el reflejo se decide por lecturas (como la 191)");
      }
      // Pipeline statistics per pass. The order of the counters is the order of the bits,
      // not the order of this list: vertices, clipped primitives and fragments.
      if (vk_device_->properties().pipelineStatisticsQuery && read_queries_) {
        VkQueryPoolCreateInfo stats_info{};
        stats_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        stats_info.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        stats_info.queryCount = uint32_t(ranuras_.size() * kStatsPerSlot);
        stats_info.pipelineStatistics =
            VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
            VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
            VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        if (dfn_.vkCreateQueryPool(device_, &stats_info, nullptr, &stats_) != VK_SUCCESS) {
          stats_ = VK_NULL_HANDLE;
        }
      }
      if (stats_ != VK_NULL_HANDLE) {
        VkQueryPoolCreateInfo draw_info{};
        draw_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        draw_info.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        draw_info.queryCount = uint32_t(ranuras_.size() * kDrawStatsPerSlot);
        draw_info.pipelineStatistics =
            VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
            VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
            VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        if (dfn_.vkCreateQueryPool(device_, &draw_info, nullptr, &draw_stats_) != VK_SUCCESS) {
          draw_stats_ = VK_NULL_HANDLE;
        }
        fragments_per_shader_.assign(size_t(kShaderLabels) * kGpuCategories, 0);
        draws_per_shader_.assign(size_t(kShaderLabels) * kGpuCategories, 0);
      }
      REXLOG_INFO("[nativo] C2: estadisticas de tuberia por pasada {}",
                  stats_ != VK_NULL_HANDLE ? "disponibles (nfsmw_nativo_estadisticas_pipeline)"
                                                  : "no disponibles");
    }

    VkSamplerCreateInfo info_sampler{};
    info_sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info_sampler.magFilter = VK_FILTER_LINEAR;
    info_sampler.minFilter = VK_FILTER_LINEAR;
    info_sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info_sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info_sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info_sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (dfn_.vkCreateSampler(device_, &info_sampler, nullptr, &sampler_) != VK_SUCCESS) {
      return false;
    }

    // Same layout as the presenter: image at 0 and sampler at 1, and the gamma ramp at 2 (the pipeline without
    // the ramp does not use it).
    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].pImmutableSamplers = &sampler_;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo info_layout{};
    info_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info_layout.bindingCount = 3;
    info_layout.pBindings = bindings;
    if (dfn_.vkCreateDescriptorSetLayout(device_, &info_layout, nullptr, &descriptors_layout_) !=
        VK_SUCCESS) {
      return false;
    }
    VkPushConstantRange ranges[2]{};
    ranges[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    ranges[0].offset = 0;
    ranges[0].size = 16;  // GuestOutputPaintRectangleConstants
    ranges[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    ranges[1].offset = 16;
    ranges[1].size = 16;  // Presenter::BilinearConstants
    VkPipelineLayoutCreateInfo info_pipeline_layout{};
    info_pipeline_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info_pipeline_layout.setLayoutCount = 1;
    info_pipeline_layout.pSetLayouts = &descriptors_layout_;
    info_pipeline_layout.pushConstantRangeCount = 2;
    info_pipeline_layout.pPushConstantRanges = ranges;
    if (dfn_.vkCreatePipelineLayout(device_, &info_pipeline_layout, nullptr, &layout_pipeline_) !=
        VK_SUCCESS) {
      return false;
    }
    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kOutputSlots},
                                       {VK_DESCRIPTOR_TYPE_SAMPLER, kOutputSlots},
                                       {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kOutputSlots}};
    VkDescriptorPoolCreateInfo info_pool_desc{};
    info_pool_desc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info_pool_desc.maxSets = kOutputSlots;
    info_pool_desc.poolSizeCount = 3;
    info_pool_desc.pPoolSizes = sizes;
    if (dfn_.vkCreateDescriptorPool(device_, &info_pool_desc, nullptr, &descriptors_pool_) !=
        VK_SUCCESS) {
      return false;
    }
    VkDescriptorSetAllocateInfo desc_reserve{};
    desc_reserve.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    desc_reserve.descriptorPool = descriptors_pool_;
    desc_reserve.descriptorSetCount = 1;
    desc_reserve.pSetLayouts = &descriptors_layout_;
    for (uint32_t i = 0; i < kOutputSlots; ++i) {
      if (dfn_.vkAllocateDescriptorSets(device_, &desc_reserve, &output_descriptors_[i]) != VK_SUCCESS) {
        return false;
      }
    }
    // One ramp buffer per output slot, so it is only changed in a slot the GPU is no longer using. Without
    // them, the output has no ramp (as before).
    gamma_ramp_ = REXCVAR_GET(nfsmw_nativo_rampa_gamma);
    for (uint32_t i = 0; i < kOutputSlots && gamma_ramp_; ++i) {
      OutputRamp& ramp = output_ramps_[i];
      uint32_t type = 0;
      void* mapped = nullptr;
      if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
              vk_device_, sizeof(ramp_values_), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
              rex::ui::vulkan::util::MemoryPurpose::kUpload, ramp.buf, ramp.mem, &type) ||
          dfn_.vkMapMemory(device_, ramp.mem, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        REXLOG_WARN("[nativo] C2: no se pudo crear el bufer de la rampa de gamma: la salida va sin ella");
        gamma_ramp_ = false;
        break;
      }
      ramp.payload = static_cast<uint8_t*>(mapped);
      ramp.type = type;
      VkDescriptorBufferInfo buffer_info{ramp.buf, 0, sizeof(ramp_values_)};
      VkWriteDescriptorSet write_op{};
      write_op.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write_op.dstSet = output_descriptors_[i];
      write_op.dstBinding = 2;
      write_op.descriptorCount = 1;
      write_op.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      write_op.pBufferInfo = &buffer_info;
      dfn_.vkUpdateDescriptorSets(device_, 1, &write_op, 0, nullptr);
    }
    if (!gamma_ramp_) {
      // Without a ramp, binding 2 stays unwritten: the pipeline without the ramp does not read it.
      for (OutputRamp& ramp : output_ramps_) {
        if (ramp.buf != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, ramp.buf, nullptr);
        if (ramp.mem != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, ramp.mem, nullptr);
        ramp = OutputRamp{};
      }
    }

    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = sizeof(shaders::guest_output_triangle_strip_rect_vs);
    module_info.pCode = shaders::guest_output_triangle_strip_rect_vs;
    if (dfn_.vkCreateShaderModule(device_, &module_info, nullptr, &vs_) != VK_SUCCESS) {
      return false;
    }
    module_info.codeSize = sizeof(shaders::guest_output_bilinear_ps);
    module_info.pCode = shaders::guest_output_bilinear_ps;
    if (dfn_.vkCreateShaderModule(device_, &module_info, nullptr, &fs_) != VK_SUCCESS) {
      return false;
    }
    if (gamma_ramp_) {
      module_info.codeSize = sizeof(shaders::nfsmw_gamma_ps_ramp_output);
      module_info.pCode = shaders::nfsmw_gamma_ps_ramp_output;
      if (dfn_.vkCreateShaderModule(device_, &module_info, nullptr, &fs_ramp_) != VK_SUCCESS) {
        return false;
      }
    }

    VkAttachmentDescription attached{};
    attached.format = VulkanPresenter::kGuestOutputFormat;
    attached.samples = VK_SAMPLE_COUNT_1_BIT;
    attached.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attached.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attached.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attached.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attached.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attached.finalLayout = VulkanPresenter::kGuestOutputInternalLayout;
    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkRenderPassCreateInfo info_rp{};
    info_rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info_rp.attachmentCount = 1;
    info_rp.pAttachments = &attached;
    info_rp.subpassCount = 1;
    info_rp.pSubpasses = &subpass;
    if (dfn_.vkCreateRenderPass(device_, &info_rp, nullptr, &output_render_pass_) != VK_SUCCESS) {
      return false;
    }

    pipeline_ = CreateOutputPipeline(fs_, nullptr);
    if (pipeline_ == VK_NULL_HANDLE) {
      return false;
    }
    // The ramp variants without extras (bilinear and exact texel) are created now; the post-processing ones
    // with per-pixel work and FXAA, the first time they are requested (PipelineRampa).
    if (gamma_ramp_ && (RampPipeline(0) == VK_NULL_HANDLE || RampPipeline(1) == VK_NULL_HANDLE)) {
      return false;
    }
    REXLOG_INFO("[nativo] C2: rampa de gamma del juego en la salida (nfsmw_nativo_rampa_gamma) = {}",
                gamma_ramp_ ? "SI" : "no");
    // Parts C3-C6: without the required capabilities only copies and presentation remain.
    draws_ = VulkanDrawer::Make(vk_device_, mem_, this);
    return true;
  }

  // Pipeline of the output pass with the fragment shader fs (and its specialization constants, if any).
  VkPipeline CreateOutputPipeline(VkShaderModule fs, const VkSpecializationInfo* special) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    stages[1].pSpecializationInfo = special;
    VkPipelineVertexInputStateCreateInfo entry{};
    entry.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ensamblado{};
    ensamblado.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ensamblado.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    view.viewportCount = 1;
    view.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterizado{};
    rasterizado.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizado.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizado.cullMode = VK_CULL_MODE_NONE;
    rasterizado.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizado.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo sampling{};
    sampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    sampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState attached_blend{};
    attached_blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &attached_blend;
    const VkDynamicState dynamic_items[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_items;
    VkGraphicsPipelineCreateInfo info_pipeline{};
    info_pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info_pipeline.stageCount = 2;
    info_pipeline.pStages = stages;
    info_pipeline.pVertexInputState = &entry;
    info_pipeline.pInputAssemblyState = &ensamblado;
    info_pipeline.pViewportState = &view;
    info_pipeline.pRasterizationState = &rasterizado;
    info_pipeline.pMultisampleState = &sampling;
    info_pipeline.pColorBlendState = &blend;
    info_pipeline.pDynamicState = &dynamic;
    info_pipeline.layout = layout_pipeline_;
    info_pipeline.renderPass = output_render_pass_;
    info_pipeline.basePipelineIndex = -1;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (dfn_.vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info_pipeline, nullptr, &pipeline) !=
        VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    return pipeline;
  }

  // Variant of the output pass with the ramp, by index: 4 * fxaa + 2 * grading + exact (specialization
  // constants 2, 1 and 0 of the shader). Created the first time it is requested; VK_NULL_HANDLE if it fails
  // (and it is logged).
  VkPipeline RampPipeline(uint32_t index) {
    VkPipeline& pipeline = ramp_pipelines_[index];
    if (pipeline == VK_NULL_HANDLE && !failed_ramp_pipelines_[index]) {
      const VkSpecializationMapEntry entries[3] = {{0, 0, sizeof(VkBool32)},
                                                    {1, sizeof(VkBool32), sizeof(VkBool32)},
                                                    {2, 2 * sizeof(VkBool32), sizeof(VkBool32)}};
      const VkBool32 values[3] = {(index & 1) ? VK_TRUE : VK_FALSE, (index & 2) ? VK_TRUE : VK_FALSE,
                                   (index & 4) ? VK_TRUE : VK_FALSE};
      const VkSpecializationInfo special{3, entries, sizeof(values), values};
      const auto before = std::chrono::steady_clock::now();
      pipeline = CreateOutputPipeline(fs_ramp_, &special);
      if (pipeline == VK_NULL_HANDLE) {
        failed_ramp_pipelines_[index] = true;
        REXLOG_ERROR("[nativo] C2: no se pudo crear la variante {} de la pasada de salida", index);
      } else {
        REXLOG_INFO("[nativo] C2: variante {} de la pasada de salida (exacta {}, graduacion {}, FXAA {}) creada en {:.1f} "
                    "ms",
                    index, (index & 1) ? "si" : "no", (index & 2) ? "si" : "no", (index & 4) ? "si" : "no",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count());
      }
    }
    return pipeline;
  }

  bool CopyBytes(const CopyRegisters& reg) override {
    // Draws since the previous copy: those of the render target being resolved now (rear-view mirror
    // diagnostic).
    const uint64_t drawn = draws_ ? draws_->Drawn() : 0;
    const uint64_t draws_before = drawn - copy_last_drawn_;
    copy_last_drawn_ = drawn;
    if (draws_) {
      draws_->FinishPass();  // copying and clearing are not allowed inside a pass
      // No longer on every copy; see ObtenerResuelta and Preparar.
      if (invalidate_every_copy_) {
        draws_->InvalidateTextures();
      }
    }
    if (Record()) {
      MarkGpu(kGpuCopies);  // GPU time of copies and clears (C2 report)
    }
    const uint32_t control = reg.rb_copy_control;
    const uint32_t src = control & 0x7;
    const bool clear_color = (control >> 8) & 0x1;
    const uint32_t command = (control >> 20) & 0x3;
    const bool copy_bytes = command == uint32_t(xenos::CopyCommand::kRaw) ||
                        command == uint32_t(xenos::CopyCommand::kConvert);
    const uint32_t pitch = reg.rb_surface_info & 0x3FFF;
    const uint32_t msaa = (reg.rb_surface_info >> 16) & 0x3;
    if (msaa != uint32_t(xenos::MsaaSamples::k1X) && warned_.insert(1).second) {
      REXLOG_INFO("[nativo] C2: destino con MSAA: se usa con 1 muestra");
    }
    if (src >= xenos::kMaxColorRenderTargets) {
      // From depth: the copy goes to a resolved texture with the host depth
      // format, which the draws sample as k_24_8. With a depth source no color
      // is cleared (IsClearingColor, graphics/util/draw.h:534-538).
      const bool will_clear = ((control >> 9) & 0x1) && clear_depth_;
      if (copy_bytes) {
        CopyDepth(reg, pitch, will_clear);
      }
      if (will_clear) {
        ClearDepth(reg, pitch);
      }
      return true;
    }
    const uint32_t info_color = reg.rb_color_info[src];
    const uint32_t color_format = (info_color >> 16) & 0xF;
    if (TargetHostFormat(color_format) == VK_FORMAT_UNDEFINED) {
      return Reject(100 + color_format, "formato de destino de render todavia no soportado");
    }
    int32_t x0, y0, x1, y1;
    if (!Rect(reg, pitch, x0, y0, x1, y1)) {
      return false;
    }

    // First the objects (creating or recreating can submit work), then recording.
    Image* render_target = GetTarget(info_color & 0xFFF, color_format, pitch);
    if (!render_target) {
      return false;
    }
    // The game has just said which area of this render target matters to it. It is the data RestaurarContenido
    // uses to stop copying the bottom rows that nobody draws or reads (the scene render target is created
    // 1280x1280 to draw 1280x720).
    TrackUsableArea(*render_target, y1);
    TrackClearUse(*render_target, uint32_t(std::max(x1, 0)), uint32_t(std::max(y1, 0)));
    uint32_t resolved_base = 0;
    Resolved* resolved = nullptr;
    uint32_t dx = 0, dy = 0;
    if (copy_bytes) {
      const uint32_t target_info = reg.rb_copy_dest_info;
      const uint32_t target_format = (target_info >> 7) & 0x3F;
      // AirborneNX: any destination format; the resolved texture keeps the render target's host format.
      if ((target_info >> 3) & 0x1) {
        Reject(3, "copia a textura 3D o array: todavia no");
      } else {
        const uint32_t target_pitch = reg.rb_copy_dest_pitch & 0x3FFF;
        const uint32_t target_height = (reg.rb_copy_dest_pitch >> 16) & 0x3FFF;
        // Base in multiples of 32 texels (GetResolveInfo), with the destination's bytes per texel.
        const uint32_t base_x = uint32_t(x0) & ~uint32_t(31);
        const uint32_t base_y = uint32_t(y0) & ~uint32_t(31);
        const uint32_t base =
            reg.rb_copy_dest_base + uint32_t(Tile2dOffset(int32_t(base_x), int32_t(base_y), target_pitch,
                                                                     CopyBytesLog2(target_format)));
        dx = uint32_t(x0) - base_x;
        dy = uint32_t(y0) - base_y;
        // The red/blue swap of the copy only exists for 8- and 10-bit unorm channels.
        const bool rb_swap = ((target_info >> 24) & 0x1) &&
                                    (render_target->format_id == VK_FORMAT_R8G8B8A8_UNORM ||
                                     render_target->format_id == VK_FORMAT_A2B10G10R10_UNORM_PACK32);
        // AirborneNX: predicated tiling (MoH:A draws its 1280x720 scene in 512- and 208-row tiles) resolves
        // each tile into the same texture, the later ones with RB_COPY_DEST_BASE at the tile's first row.
        // Such a copy goes into the texture that contains that address, rows further down.
        uint32_t containing_base = 0, rows = 0;
        if (FindContainingResolved(base & 0x1FFFFFFF, target_pitch, target_height, CopyBytesLog2(target_format),
                                      render_target->format_id, containing_base, rows)) {
          resolved = &resolved_.find(containing_base)->second;
          dy += rows;
          resolved_base = containing_base;
          if (warned_.insert(0x7000000u | (base & 0xFFFFFF)).second) {
            REXLOG_INFO("[nativo] C2: copia de baldosa a {:08X}: dentro de la textura resuelta {:08X}, {} filas abajo",
                        base & 0x1FFFFFFF, containing_base, rows);
          }
        } else {
          resolved = GetResolved(base & 0x1FFFFFFF, target_pitch, target_height, target_format, rb_swap,
                                     render_target->format_id);
          resolved_base = base & 0x1FFFFFFF;
        }
        if (resolved) {
          // AirborneNX: RB_COPY_DEST_INFO.copy_dest_exp_bias (bits 16-21, signed): the guest texture holds
          // 2^bias times the render target. The copy here is 1:1 and the samples are scaled instead.
          resolved->img.scale = std::ldexp(1.0f, int32_t(target_info << 10) >> 26);
          TrackCopy(base & 0x1FFFFFFF, target_pitch, target_height, draws_before);
          if (composite_has_ || composite_expired_ != 0) {  // nfsmw_nativo_compuesta_perezosa
            CompositeBeforeTextureWrite(base & 0x1FFFFFFF, x0 == 0 && y0 == 0 &&
                                                                   uint32_t(x1 - x0) >= resolved->img.width_px &&
                                                                   uint32_t(y1 - y0) >= resolved->img.height_px);
          }
          if (diag_window_ || !diag_watched_.empty()) {  // nfsmw_nativo_diag_lectores_s
            DiagWrite(base & 0x1FFFFFFF, info_color & 0xFFF, pitch, x0, y0, uint32_t(x1 - x0), uint32_t(y1 - y0),
                          resolved->img.width_px, resolved->img.height_px, draws_before, false);
          }
        }
        if (resolved && REXCVAR_GET(nfsmw_nativo_diag_resueltas) &&
            frame_resolved_.size() < 64 &&
            std::find(frame_resolved_.begin(), frame_resolved_.end(),
                      base & 0x1FFFFFFF) == frame_resolved_.end()) {
          frame_resolved_.push_back(base & 0x1FFFFFFF);
        }
      }
    }

    if (!Record()) {
      return false;
    }
    Prepare(*render_target);
    if (resolved) {
      Prepare(resolved->img);
      // What the game asks for and what fits in the render target.
      const uint32_t request_width =
          std::min(uint32_t(x1 - x0), render_target->width_px - uint32_t(x0));
      const uint32_t request_height = std::min(uint32_t(y1 - y0), render_target->height_px - uint32_t(y0));
      const uint32_t fits_width = resolved->img.width_px > dx ? resolved->img.width_px - dx : 0;
      const uint32_t fits_height = resolved->img.height_px > dy ? resolved->img.height_px - dy : 0;
      const uint32_t width_px = std::min(request_width, fits_width);
      const uint32_t height_px = std::min(request_height, fits_height);
      // With the scene at a higher resolution than the render target (nfsmw_resolucion_interna = 1920x1080
      // and a 1280x720 front buffer), copying 1 to 1 takes only a piece: the image comes out cropped, with the
      // car and the HUD out of frame. When it does not fit, it is shrunk with a linear blit, which is exactly
      // the scaling wanted. If it fits, it is copied as always, bit for bit.
      // It only shrinks when the scene really has to be reduced (by a factor of 1.25 or more) and both images
      // are color: vkCmdBlitImage with a linear filter on a depth target is not valid and brings the process
      // down. Mismatches of a few pixels (320x184 -> 320x180) are still cropped as always.
      const bool shrink = blit_ != nullptr && fits_width && fits_height &&
                           render_target->format_id == kColorFormat &&
                           resolved->img.format_id == kColorFormat &&
                           (request_width * 4 >= fits_width * 5 || request_height * 4 >= fits_height * 5);
      if (shrink && uint32_t(x0) < render_target->width_px && uint32_t(y0) < render_target->height_px) {
        // nfsmw_nativo_frontal_perezoso. If this texture had a deferred copy, it is dropped if this resolve
        // covers it entirely, and recorded first otherwise.
        ResolvePreviousFront(resolved_base, dx == 0 && dy == 0 && fits_width == resolved->img.width_px &&
                                                   fits_height == resolved->img.height_px);
        VkImageBlit reduction{};
        reduction.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        reduction.srcOffsets[0] = {x0, y0, 0};
        reduction.srcOffsets[1] = {x0 + int32_t(request_width), y0 + int32_t(request_height), 1};
        reduction.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        reduction.dstOffsets[0] = {int32_t(dx), int32_t(dy), 0};
        reduction.dstOffsets[1] = {int32_t(dx + fits_width), int32_t(dy + fits_height), 1};
        blit_(job_commands_, render_target->img, VK_IMAGE_LAYOUT_GENERAL,
              resolved->img.img, VK_IMAGE_LAYOUT_GENERAL, 1, &reduction, VK_FILTER_LINEAR);
        ++copies_;
        ++reductions_;
        if (reductions_ <= 4) {
          REXLOG_INFO("[resolucion] la escena de {}x{} se encoge a {}x{} al resolverla (superescalado)",
                      request_width, request_height, fits_width, fits_height);
        }
        TrackCopy(fits_width, fits_height);
        // A linear-filter blit reads the large rectangle and writes the small one, so it costs more than a copy
        // of the output size. The pixels read are recorded, since they are what dominates.
        WrittenResolved(resolved_base, uint64_t(request_width) * request_height);
        ReadResolved(reg, *resolved, x0, y0, dx, dy, fits_width, fits_height);
      } else if (width_px && height_px && uint32_t(x0) < render_target->width_px &&
                 uint32_t(y0) < render_target->height_px && dx < resolved->img.width_px &&
                 dy < resolved->img.height_px) {
        /*
         * The same as is done with depth. If the whole render target is resolved to a texture of the same size,
         * the two images are swapped and not a single pixel is copied. That is 4 copies of 1280x720 per frame,
         * 78 % of the traffic.
         */
        const bool color_int = x0 == 0 && y0 == 0 && dx == 0 && dy == 0 &&
                                  width_px == render_target->width_px && height_px == render_target->height_px;
        /*
         * For color the clear is a requirement, unlike for depth. Tested without it: 16,807 color swaps and
         * 17,350 restores, almost one for one; the game draws on top again without clearing and the copy is
         * paid anyway, only later and with two operations instead of one.
         */
        if (color_int && clear_color && ResolveWithoutCopy() &&
            REXCVAR_GET(nfsmw_nativo_intercambiar_color)) {
          // With x0 = y0 = 0 the tiling offset is 0, so the address is the base as is.
          if (SwapWithResolved(*render_target, *resolved, reg.rb_copy_dest_base & 0x1FFFFFFF)) {
            ++color_swaps_;
            ReadResolved(reg, *resolved, x0, y0, dx, dy, width_px, height_px);
            return true;
          }
        }
        VkImageCopy copy_info{};
        copy_info.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy_info.srcOffset = {x0, y0, 0};
        copy_info.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy_info.dstOffset = {int32_t(dx), int32_t(dy), 0};
        copy_info.extent = {width_px, height_px, 1};
        // nfsmw_nativo_frontal_perezoso. The previous copy to this texture, if still deferred, is dropped (this
        // one covers it entirely) or recorded first; and if this texture is a front buffer that only the Swap
        // reads, it is deferred.
        ResolvePreviousFront(resolved_base,
                                dx == 0 && dy == 0 && width_px == resolved->img.width_px && height_px == resolved->img.height_px);
        if (!DeferFrontCopy(resolved_base, *render_target, *resolved, copy_info) &&
            !DeferComposite(resolved_base, *render_target, *resolved, copy_info,
                              (info_color & 0xFFF) | (pitch << 12))) {
          copy_image_(job_commands_, render_target->img, VK_IMAGE_LAYOUT_GENERAL,
                         resolved->img.img, VK_IMAGE_LAYOUT_GENERAL, 1, &copy_info);
          ++copies_;
          TrackCopy(width_px, height_px);
          WrittenResolved(resolved_base, uint64_t(width_px) * height_px);
          ReadResolved(reg, *resolved, x0, y0, dx, dy, width_px, height_px);
        }
      }
    }
    if (clear_color) {
      const uint64_t raw_value =
          uint64_t(reg.rb_color_clear) | (uint64_t(reg.rb_color_clear_lo) << 32);
      /*
       * Two things that were missing here.
       *  1. The clear leaves the whole render target with a known color, so if its content had gone away
       *     in a swap it no longer has to be brought back. Depth already did this (BorrarProfundidad);
       *     color did not, and that is why, when nfsmw_nativo_intercambiar_color was turned on, there were
       *     17,350 restores against 16,807 swaps: almost all of them restored only to clear right after.
       *  2. If the render target is already cleared to that same color and nothing has been drawn since
       *     then, the vkCmdClearColorImage does not change a single bit and is skipped.
       */
      if (composite_has_ && composite_.src == ((info_color & 0xFFF) | (pitch << 12))) {
        CompositeBeforeSourceWrite();  // nfsmw_nativo_compuesta_perezosa (its source is cleared)
      }
      if (!diag_watched_.empty()) {  // nfsmw_nativo_diag_lectores_s (a source is cleared)
        DiagSourceWritten((info_color & 0xFFF) | (pitch << 12));
      }
      render_target->invalid_content = false;
      const uint64_t state_value =
          REXCVAR_GET(nfsmw_nativo_diag_borrado) ? ~uint64_t(0) : raw_value;
      if (RedundantClear(*render_target, state_value)) {
        ++skipped_clears_;
      } else {
        // nfsmw_nativo_frontal_perezoso. If this render target is the source of a deferred front buffer, the
        // clear goes to a spare image and the one with the content is kept for the front buffer (no copy).
        RotateFrontBeforeClear(*render_target);
        MarkGpu(kGpuClears);  // clears, separate from copies (C2 report)
        VkClearColorValue color{};
        if (REXCVAR_GET(nfsmw_nativo_diag_borrado)) {
          color = DiagnosticColor(info_color & 0xFFF, color_format, pitch);
        } else {
          color = ClearColor(color_format, raw_value);
        }
        // nfsmw_nativo_borrar_area_util. Only the rows in use; the bottom band, if needed.
        if (!ClearColorUsableArea(*render_target, color)) {
          dfn_.vkCmdClearColorImage(job_commands_, render_target->img,
                                    VK_IMAGE_LAYOUT_GENERAL, &color, 1, &kColorRange);
          RemoveBand(*render_target);
        }
        ++clears_;
        TrackDiagClear(*render_target, info_color & 0xFFF, color_format, pitch, false, false);
      }
    }
    if (((control >> 9) & 0x1) && clear_depth_) {
      ClearDepth(reg, pitch);
    }
    return true;
  }

  void ClearDepth(const CopyRegisters& reg, uint32_t pitch) {
    const uint32_t info = reg.rb_depth_info;
    Image* depth_value = GetDepth(info & 0xFFF, (info >> 16) & 0x1, pitch);
    if (!depth_value || !Record()) {
      return;
    }
    Prepare(*depth_value);
    BeforeDepthWrite(*depth_value);  // nfsmw_nativo_profundidad_perezosa
    ShadowMinBeforeClear(*depth_value);   // nfsmw_nativo_sombra_minimo
    depth_value->invalid_content = false;  // The clear gives it contents
    const VkClearDepthStencilValue value{float(reg.rb_depth_clear >> 8) / 16777215.0f,
                                         reg.rb_depth_clear & 0xFF};
    // If it is already cleared with this same value and nobody has drawn anything since then, the clear does
    // not change a single bit. Mind the path below: the per-pass clear also resets the ZCULL plane, so that
    // one is never skipped (only the vkCmdClearDepthStencilImage one).
    if (depth_value->supports_copy_target &&
        RedundantClear(*depth_value, uint64_t(reg.rb_depth_clear))) {
      ++depth_skipped_clears_;
      return;
    }
    MarkGpu(kGpuClears);  // clears, separate from copies (C2 report)
    ++depth_clears_;
    // nfsmw_nativo_diag_borrados. Without TRANSFER_DST the clear is done by opening a pass (ZCULL).
    TrackDiagClear(*depth_value, info & 0xFFF, (info >> 16) & 0x1, pitch, true,
                      !depth_value->supports_copy_target);
    // ZCULL: see Preparar. Without TRANSFER_DST the clear has to be done by opening a pass.
    if (!depth_value->supports_copy_target && draws_) {
      // This bool cannot be dropped. If the clear pass cannot be opened, the depth keeps the previous frame's
      // content and the ZCULL hi-Z is not reset either (only a loadOp = CLEAR resets it), so the whole frame
      // culls against stale data. Without TRANSFER_DST there is no alternative path, so at least it is
      // counted and reported.
      if (!draws_->ClearDepthInPass(job_commands_, *depth_value, value.depth,
                                             value.stencil)) {
        if (++clears_in_failed_pass_ <= 8) {
          REXLOG_WARN("[nativo] C2: NO se pudo borrar la profundidad {}x{} abriendo un pase; se queda "
                      "con el contenido anterior (fallo {})",
                      depth_value->width_px, depth_value->height_px, clears_in_failed_pass_);
        }
      }
      return;
    }
    clear_depth_(job_commands_, depth_value->img, VK_IMAGE_LAYOUT_GENERAL, &value, 1,
                        &kDepthRange);
  }

  // Depth copy: the rectangle of the depth render target to a resolved texture
  // of the same host format, at the base GetResolveInfo computes
  // (graphics/util/draw.cpp:945-1010; 4 bytes per texel, like k_24_8).
  // borrara: this same game command clears the render target right after resolving it. Only then is it worth
  // swapping the images: if the game kept drawing on top, the content would have to be brought back and the
  // copy would be paid anyway (measured: one restore per frame on the shadow map).
  void CopyDepth(const CopyRegisters& reg, uint32_t pitch, bool will_clear) {
    const uint32_t target_info = reg.rb_copy_dest_info;
    if ((target_info >> 3) & 0x1) {
      Reject(3, "copia a textura 3D o array: todavia no");
      return;
    }
    int32_t x0, y0, x1, y1;
    if (!copy_image_ || !Rect(reg, pitch, x0, y0, x1, y1)) {
      return;
    }
    const uint32_t info = reg.rb_depth_info;
    ++depth_copies_;  // the game asks to resolve the depth to a texture
    Image* depth_value = GetDepth(info & 0xFFF, (info >> 16) & 0x1, pitch);
    if (!depth_value) {
      return;
    }
    const uint32_t target_pitch = reg.rb_copy_dest_pitch & 0x3FFF;
    const uint32_t target_height = (reg.rb_copy_dest_pitch >> 16) & 0x3FFF;
    const uint32_t base_x = uint32_t(x0) & ~uint32_t(31);
    const uint32_t base_y = uint32_t(y0) & ~uint32_t(31);
    const uint32_t base =
        reg.rb_copy_dest_base +
        uint32_t(Tile2dOffset(int32_t(base_x), int32_t(base_y), target_pitch, 2));
    const uint32_t dx = uint32_t(x0) - base_x;
    uint32_t dy = uint32_t(y0) - base_y;
    const uint32_t texture_format = ((info >> 16) & 0x1) ? 23 : 22;  // k_24_8_FLOAT : k_24_8
    // With the scaled shadow map, the resolved texture stays at that same size.
    // The scene picks it by address, without looking at the size, and samples it with normalized coordinates
    // and point sampling, so 1024 read with the UVs of 1600 gives the same texel as 1024 upscaled to 1600
    // with NEAREST: the image does not change. What is saved is the upscaling blit (1.78 ms on the console)
    // and the larger half of the copy (10.24 MB -> 4.2 MB, twice per frame).
    uint32_t resolved_width = target_pitch, resolved_height = target_height;
    if (depth_value->guest_width && depth_value->guest_width != depth_value->width_px) {
      resolved_width = uint32_t(uint64_t(target_pitch) * depth_value->width_px / depth_value->guest_width);
      resolved_height = uint32_t(uint64_t(target_height) * depth_value->height_px / depth_value->guest_height);
    }
    // If the previous copy to this address is still deferred, it is recorded first (exact; it does not happen
    // in a race: the scene clear resolves it earlier). See nfsmw_nativo_profundidad_perezosa.
    if (!pending_.empty() && pending_.count(base & 0x1FFFFFFF)) {
      RecordPendingCopy(base & 0x1FFFFFFF);
      ++lazy_write_copied_;
    }
    // AirborneNX: a later tile of predicated tiling goes into the texture that contains its address (see
    // the color copy).
    Resolved* resolved = nullptr;
    uint32_t containing_base = 0, rows = 0;
    if (resolved_width == target_pitch &&
        FindContainingResolved(base & 0x1FFFFFFF, target_pitch, target_height, 2, depth_format_,
                                  containing_base, rows)) {
      resolved = &resolved_.find(containing_base)->second;
      dy += rows;
    } else {
      resolved = GetResolved(base & 0x1FFFFFFF, resolved_width, resolved_height, texture_format, false,
                                 depth_format_);
    }
    if (!resolved || !Record()) {
      return;
    }
    // Depth is also included in the per-render-target inventory. Previously only the square cubemap faces were
    // recorded, so the shadow map (the most expensive copy in the frame) did not show up.
    TrackCopy(base & 0x1FFFFFFF, resolved_width, resolved_height, 0);
    if (diag_window_ || !diag_watched_.empty()) {  // nfsmw_nativo_diag_lectores_s
      DiagWrite(base & 0x1FFFFFFF, info & 0xFFF, pitch, x0, y0, uint32_t(x1 - x0), uint32_t(y1 - y0),
                    resolved->img.width_px, resolved->img.height_px, 0, true);
    }
    Prepare(*depth_value);
    Prepare(resolved->img);
    TrackUsableArea(*depth_value, y1);  // the area the game really resolves
    TrackClearUse(*depth_value, uint32_t(std::max(x1, 0)), uint32_t(std::max(y1, 0)));
    // With the scaled shadow map, the rectangle the guest sends is in 1600-pixel units even though the image
    // is smaller. Everything below reasons in guest pixels.
    const uint32_t guest_width = depth_value->guest_width ? depth_value->guest_width : depth_value->width_px;
    const uint32_t guest_height = depth_value->guest_height ? depth_value->guest_height : depth_value->height_px;
    const bool scaled = guest_width != depth_value->width_px || guest_height != depth_value->height_px;
    const uint32_t width_px = std::min({uint32_t(x1 - x0), guest_width - uint32_t(x0),
                                     resolved->img.width_px - dx});
    const uint32_t height_px = std::min({uint32_t(y1 - y0), guest_height - uint32_t(y0),
                                    resolved->img.height_px - dy});
    if (!width_px || !height_px || uint32_t(x0) >= guest_width ||
        uint32_t(y0) >= guest_height || dx >= resolved->img.width_px ||
        dy >= resolved->img.height_px) {
      return;
    }
    // nfsmw_nativo_resolver_contenido_valido. If the content of this render target went away in an earlier
    // swap and nobody brought it back, it is in another texture: it is brought back before reading it (the
    // menu flicker).
    if (depth_value->invalid_content) {
      if (REXCVAR_GET(nfsmw_nativo_resolver_contenido_valido)) {
        RestoreContent(*depth_value, true);
      } else {
        ++resolve_old_content_;  // previous behavior: the old image is resolved
      }
    }
    if (scaled) {
      // Source and destination have the same reduced size, so it is a normal 1 to 1 copy
      // with the rectangle converted to image pixels. No blit, no scaling.
      const auto aX = [&](int32_t v) {
        return int32_t(int64_t(v) * depth_value->width_px / guest_width);
      };
      const auto aY = [&](int32_t v) {
        return int32_t(int64_t(v) * depth_value->height_px / guest_height);
      };
      const uint32_t img_width = std::min(
          {uint32_t(std::max(1, aX(x0 + int32_t(width_px)) - aX(x0))),
           depth_value->width_px - uint32_t(aX(x0)), resolved->img.width_px - uint32_t(aX(int32_t(dx)))});
      const uint32_t img_height = std::min(
          {uint32_t(std::max(1, aY(y0 + int32_t(height_px)) - aY(y0))),
           depth_value->height_px - uint32_t(aY(y0)), resolved->img.height_px - uint32_t(aY(int32_t(dy)))});
      if (!img_width || !img_height) {
        return;
      }
      VkImageCopy copy_info{};
      copy_info.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
      copy_info.srcOffset = {aX(x0), aY(y0), 0};
      copy_info.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
      copy_info.dstOffset = {aX(int32_t(dx)), aY(int32_t(dy)), 0};
      copy_info.extent = {img_width, img_height, 1};
      copy_image_(job_commands_, depth_value->img, VK_IMAGE_LAYOUT_GENERAL,
                     resolved->img.img, VK_IMAGE_LAYOUT_GENERAL, 1, &copy_info);
      ++copies_;
      ++shadows_uploads_;
      TrackCopy(img_width, img_height);
      WrittenResolved(base & 0x1FFFFFFF, uint64_t(img_width) * img_height);
      return;
    }
    // If the whole render target is resolved to a texture of the same size, the images are swapped and
    // nothing is copied (the shadow map is 1600x1600: 10 MB per copy).
    if (ResolveWithoutCopy()) {  // and if it is not swapped, why not
      const bool integer_value = x0 == 0 && y0 == 0 && dx == 0 && dy == 0 &&
                          width_px == depth_value->width_px && height_px == depth_value->height_px;
      // The clear is no longer a requirement (see nfsmw_nativo_intercambiar_sin_borrado).
      const bool can = integer_value && (will_clear || REXCVAR_GET(nfsmw_nativo_intercambiar_sin_borrado));
      if (can) {
        if (SwapWithResolved(*depth_value, *resolved, base & 0x1FFFFFFF)) {
          if (!will_clear) {
            ++swaps_without_clear_;
          }
          ShadowMinAfterSwap(*depth_value, base & 0x1FFFFFFF, will_clear);
          return;
        }
        ++without_swap_[2];  // the swap itself could not be done
      } else {
        /*
         * The label was backwards ever since nfsmw_nativo_intercambiar_sin_borrado existed. With that setting
         * on, `puede` = whole, so everything that lands here is "not resolved whole" and the log counted it as
         * "the command does not clear the render target". It is now split by the real cause: if the resolve
         * is whole, the cause is the clear; otherwise, it is the size.
         */
        ++without_swap_[integer_value ? 0 : 1];  // 0: the command does not clear the render target; 1: not the whole render target
        if (warnings_without_swap_ < 8) {
          ++warnings_without_swap_;
          REXLOG_INFO("[nativo] C2 sin intercambio: {}x{} de {}x{} en ({},{})->({},{}), borra {} (base {:03X})",
                      width_px, height_px, depth_value->width_px, depth_value->height_px, x0, y0, dx, dy, will_clear,
                      base & 0xFFF);
        }
      }
    }
    VkImageCopy copy_info{};
    copy_info.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    copy_info.srcOffset = {x0, y0, 0};
    copy_info.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    copy_info.dstOffset = {int32_t(dx), int32_t(dy), 0};
    copy_info.extent = {width_px, height_px, 1};
    // If this address is only requested by the composition without blur, the copy is deferred
    // (nfsmw_nativo_profundidad_perezosa): it is done as soon as a draw really samples it; if nobody does,
    // it is not.
    ShadowMinCopyFrom(*depth_value);  // nfsmw_nativo_sombra_minimo
    if (DeferDepthCopy(base & 0x1FFFFFFF, *depth_value, *resolved, copy_info)) {
      return;
    }
    copy_image_(job_commands_, depth_value->img, VK_IMAGE_LAYOUT_GENERAL,
                   resolved->img.img, VK_IMAGE_LAYOUT_GENERAL, 1, &copy_info);
    ++copies_;
    TrackCopy(width_px, height_px);
    WrittenResolved(base & 0x1FFFFFFF, uint64_t(width_px) * height_px);
  }

  void GammaRamp(const std::array<std::array<uint16_t, 3>, 256>& ramp) override {
    game_ramp_ = ramp;
    RecalculateRamp();
  }

  // The output table = the game's ramp followed by the post-processing, with the formula and order of
  // GoldenEye-Recomp (ge_grade.cs.hlsl): temperature, brightness, contrast, tint, saturation, vibrance and
  // gamma. Everything that is single-channel is computed here for the 256 entries, so without saturation,
  // vibrance, vignette or scanlines there is no per-pixel work (and when off, the table is the ramp as
  // is). With them (graduacion_), the table stops before the gamma and without clamping, and the shader
  // does the rest.
  void RecalculateRamp() {
    const nfsmw::settings::Postprocess& p = postprocess_;
    grading_ = p.active && (p.saturation != 1.0f || p.rumble != 0.0f || p.vignette > 0.0f || p.lines > 0.0f);
    const float tint[3] = {p.r_tint, p.g_tint, p.b_tint};
    const float gamma_inverse = p.gamma > 0.0f ? 1.0f / p.gamma : 1.0f;
    for (uint32_t i = 0; i < 256; ++i) {
      for (uint32_t c = 0; c < 3; ++c) {
        float x = float(game_ramp_[i][c] & 0x3FF) / 1023.0f;
        if (p.active) {
          x += c == 0 ? p.temperatura * 0.10f : c == 2 ? -p.temperatura * 0.10f : 0.0f;
          x += p.brightness;
          x = (x - 0.5f) * p.contrast + 0.5f;
          x = x + (x * tint[c] - x) * p.tint;
          if (!grading_) {
            x = std::clamp(std::pow(std::max(x, 0.0001f), gamma_inverse), 0.0f, 1.0f);
          }
        }
        ramp_values_[i * 4 + c] = x;
      }
      ramp_values_[i * 4 + 3] = 1.0f;
    }
    const size_t extra = 256 * 4;
    ramp_values_[extra] = p.saturation;
    ramp_values_[extra + 1] = p.rumble;
    ramp_values_[extra + 2] = gamma_inverse;
    ramp_values_[extra + 3] = 0.0f;
    ramp_values_[extra + 4] = p.vignette;
    ramp_values_[extra + 5] = p.lines;
    ramp_values_[extra + 6] = 0.0f;
    ramp_values_[extra + 7] = 0.0f;
    ++ramp_version_;
  }

  bool Present(rex::ui::Presenter* presenter_ptr, const SwapTexture& swap, uint32_t width_px,
                 uint32_t height_px) override {
    nfsmw::demand_reflection::TrackSwap();  // nfsmw_reflejo_bajo_demanda
    // The per-draw diagnostic window opens here, on the PM4 ring thread, which is the one that records the
    // draws. Inside the paint call it would be another thread and the window would catch an arbitrary piece
    // of the frame (33 of 500 shadow draws were measured).
    OpenDiagnosticWindow();
#if REX_PLATFORM_SWITCH
    RexSwitchPerfCount(0);  // the profiler's "juego N fps", which read 0.0 with the native renderer
#endif
    // Histogram of intervals between Swaps (C2 report): whether they come in vsync steps or spread out.
    {
      const auto now_time = std::chrono::steady_clock::now();
      if (last_swap_ != std::chrono::steady_clock::time_point{}) {
        const double ms = std::chrono::duration<double, std::milli>(now_time - last_swap_).count();
#if REX_PLATFORM_SWITCH
        // Frames over 45 ms go to stack sampling. The window starts one normal frame (33 ms) earlier, because
        // the game thread prepares the frame ahead of the ring. Ticks at 19.2 MHz.
        if (ms > 45.0) {
          uint64_t stop_pos;
          asm volatile("mrs %0, cntpct_el0" : "=r"(stop_pos));  // libnx's armGetSystemTick
          const uint64_t largo = uint64_t((ms + 33.3) * 19200.0);
          RexSwitchPerfTiron(stop_pos > largo ? stop_pos - largo : 0, stop_pos);
        }
#endif
        const uint32_t bucket = ms < 15.0    ? 0
                                : ms < 18.0  ? 1
                                : ms < 25.0  ? 2
                                : ms < 30.0  ? 3
                                : ms < 36.0  ? 4
                                : ms < 50.0  ? 5
                                : ms < 60.0  ? 6
                                : ms < 75.0  ? 7
                                : ms < 100.0 ? 8
                                : ms < 150.0 ? 9
                                             : 10;
        ++swap_buckets_[bucket];
        // The worst frame of the interval. The average is not felt; the peak is.
        if (ms > worst_swap_ms_) {
          worst_swap_ms_ = ms;
        }
        nfsmw::guard30::Latir(ms);  // the 30 FPS guard decides with this
        /*
         * What happens in a frame that runs long.
         *
         * Measured on the console: 7.3 % of frames exceed 50 ms and there are peaks of 2.1 seconds. Pipelines
         * are stable (116, 11 ms) and the texture cache no longer evicts, so the cause is something else.
         *
         * When a frame runs long, what that frame did (not the running total) is dumped. If the culprit is
         * loading textures, or creating pipelines, or a big copy, or simply that the game sent three times as
         * many draws, it shows here. It only writes when there is a stutter, so it costs nothing in the normal
         * case.
         */
        /*
         * The previous dump was not enough, and this is why.
         *
         * The stutters are 60-70 ms frames (median 67, against an average of 39.34) with an exactly normal
         * render load: 14 copies, 4 clears, 2 swaps, 1 restore, the same as any other frame. And they are not
         * quantized to the vblank (34 of them fall between 60.0 and 62.5 ms, which is not a multiple of
         * 16.67). So there are ~26 extra ms that are not in the drawing and the old dump did not see them.
         *
         * So now the time breakdown is dumped, not only the work count:
         *   - GPU work of that frame, with the timestamps we already measure. If it goes up to 60 ms it is
         *     scene load; if it stays at 33 the time went to the CPU or to waiting.
         *   - how long the thread took to record and how long to present.
         *   - new textures and pipelines, the usual suspects of an isolated peak.
         * With that, the log says where the 26 ms come from without guessing.
         */
        /*
         * The cap of 200 was blinding us for half a race.
         *
         * The 200 warnings ran out at t=135 s of a 234 s session, and the following intervals had 96 frames
         * over 50 ms without a single line. So exactly the most played stretch was a black hole. With 1000 a
         * whole session fits, and since it only writes when there is a stutter it costs nothing in the normal
         * case.
         */
        if (ms > 60.0 && hitch_warnings_ < 1000) {
          ++hitch_warnings_;
          /* gpu_ns_ are raw GPU timestamps: multiply by 1.627 to get real milliseconds (see docs/measuring.md). */
          const double gpu_ms = double(gpu_ns_ - hitch_gpu_ns_) / 1e6 * 1.627;
          // The three [tiron] lines go to the report thread (NFSMW_INFORME_ANILLO).
          NFSMW_RING_REPORT(
              "[tiron] fotograma de {:.1f} ms (GPU {:.1f} reales, grabar {:.1f}): {} copias, "
              "{} borrados, {} intercambios, {} restauraciones, {} esperas a la GPU",
              ms, gpu_ms, double(record_ns_ - hitch_record_ns_) / 1e6, copies_ - hitch_copies_,
              clears_ - hitch_clears_, swap_count_ - hitch_resolves_,
              restores_ - hitch_restore_, gpu_waits_ - hitch_waits_);
          /*
           * Who waits for whom in this frame, in ms (see stall_waits.h).
           * The game's and the ring's waits overlap: they do not add up to the frame, they say where it went.
           */
          namespace e = nfsmw::waits;
          const auto delta = [&](e::Kind t) {
            return double(e::g_ns[t].load(std::memory_order_relaxed) - hitch_waits_ns_[t]) / 1e6;
          };
          const auto times = [&](e::Kind t) {
            return e::g_times[t].load(std::memory_order_relaxed) - hitch_times_waits_[t];
          };
          NFSMW_RING_REPORT(
              "[tiron] esperas (ms): juego: relevo del ejecutor {:.1f} ({}), relevo del preparador {:.1f} ({}), "
              "sitio en el anillo {:.1f} ({}), dentro de sub_826E8EE8 {:.1f} ({}; vtabla {:08X}, llamante {:08X}) | "
              "anillo: sin trabajo {:.1f} ({}), WAIT_REG_MEM {:.1f} ({}), valla de la GPU {:.1f}, salida {:.1f} | "
              "texturas comprobadas {:.1f} MB, aplazadas {}",
              delta(e::kExecutorHandoff), times(e::kExecutorHandoff), delta(e::kPreparerHandoff),
              times(e::kPreparerHandoff), delta(e::kRingRoom), times(e::kRingRoom),
              delta(e::kMediumGame), times(e::kMediumGame), e::g_medium_vtable.load(std::memory_order_relaxed),
              e::g_medium_caller.load(std::memory_order_relaxed), delta(e::kRingWithoutJob),
              times(e::kRingWithoutJob), delta(e::kRegMemRing), times(e::kRegMemRing),
              double(gpu_waits_ns_ - hitch_gpu_waits_ns_) / 1e6,
              double(output_wait_ns_ - hitch_output_wait_ns_) / 1e6,
              double(e::g_fingerprint_bytes.load(std::memory_order_relaxed) - hitch_fingerprint_bytes_) / 1048576.0,
              e::g_deferred_fingerprints.load(std::memory_order_relaxed) - hitch_deferred_fingerprints_);
          // What the ring spent this frame on.
          const auto dif = [](const std::atomic<uint64_t>& a, uint64_t before) {
            return a.load(std::memory_order_relaxed) - before;
          };
          // And how long the ring waited for the vertex copy thread (EsperarSubidas). The wait before each
          // submit is inside "trabajando" and the one before returning the read pointer is outside: it is read
          // separately, not added.
          // And what the ring spent creating textures (image, memory and view; "texturas" starts afterwards),
          // how many the binding thread bound and how long the ring waited for it
          // (nfsmw_nativo_texturas_enlace_hilo).
          NFSMW_RING_REPORT("[tiron] anillo: {} dibujos, trabajando {:.1f} ms, texturas {:.1f} ms ({} subidas, {:.1f} MB; "
                      "{} creadas; huellas {:.1f} + {:.1f} ms); esperando al hilo de copias de vertices {:.1f} ms "
                      "({} veces), ayudandolo {:.1f} ms ({} copias); "
                      "crear texturas {:.1f} ms en el anillo, {} enlazadas en el hilo, esperandolo {:.1f} ms; "
                      "huellas en el hilo: {} texturas en {:.1f} ms del hilo, copias en el anillo {:.1f} ms, {} "
                      "hechas por el anillo en {:.1f} ms, esperandolo {:.1f} ms",
                      dif(e::g_draws, hitch_draws_), double(dif(e::g_working_ring_ns, hitch_ring_ns_)) / 1e6,
                      double(dif(e::g_textures_ns, hitch_textures_ns_)) / 1e6,
                      dif(e::g_uploads_textures, hitch_uploads_textures_),
                      double(dif(e::g_uploaded_bytes, hitch_uploaded_bytes_)) / 1048576.0,
                      dif(e::g_created_textures, hitch_created_textures_),
                      double(dif(e::g_raw_fingerprint_ns, hitch_raw_fingerprint_ns_)) / 1e6,
                      double(dif(e::g_data_fingerprint_ns, hitch_data_fingerprint_ns_)) / 1e6,
                      double(dif(e::g_copies_waiting_ns, hitch_copies_waiting_ns_)) / 1e6,
                      dif(e::g_copies_waits, hitch_copies_waits_),
                      double(dif(e::g_copies_helping_ns, hitch_copies_helping_ns_)) / 1e6,
                      dif(e::g_assisted_copies, hitch_assisted_copies_),
                      double(dif(e::g_textures_create_ns, hitch_textures_create_ns_)) / 1e6,
                      dif(e::g_thread_bound_textures, hitch_thread_bound_textures_),
                      double(dif(e::g_bindings_waiting_ns, hitch_bindings_waiting_ns_)) / 1e6,
                      // the texture fingerprint thread (nfsmw_nativo_texturas_huella_hilo)
                      dif(e::g_thread_fingerprint_textures, hitch_thread_fingerprint_textures_),
                      double(dif(e::g_thread_fingerprint_ns, hitch_thread_fingerprint_ns_)) / 1e6,
                      double(dif(e::g_snapshot_fingerprint_ns, hitch_snapshot_fingerprint_ns_)) / 1e6,
                      dif(e::g_ring_fingerprint_textures, hitch_ring_fingerprint_textures_),
                      double(dif(e::g_ring_fingerprint_ns, hitch_ring_fingerprint_ns_)) / 1e6,
                      double(dif(e::g_fingerprints_waiting_ns, hitch_fingerprints_waiting_ns_)) / 1e6);
          /*
           * The game's side. Four of the seven race stutters in one run come from the preparer (the executor
           * waiting for the handoff, or waiting for commands inside the list, with the ring idle), and the wait
           * without commands did not show up in any line. "Fuera de la lista" is the preparer's simulation plus
           * its handoff wait (the one in the line above): the difference is what the game itself takes.
           */
          NFSMW_RING_REPORT("[tiron] juego: el ejecutor sin ordenes {:.1f} ms ({}) | el preparador: llenando la "
                               "lista {:.1f} ms ({}), de ello TreeCull {:.1f} ms ({}); fuera de la lista {:.1f} ms ({})",
                               delta(e::kExecutorWithoutCommands), times(e::kExecutorWithoutCommands),
                               delta(e::kListPreparer), times(e::kListPreparer),
                               delta(e::kScenarioPreparer), times(e::kScenarioPreparer),
                               delta(e::kOutsidePreparer), times(e::kOutsidePreparer));
        }
        for (uint32_t t = 0; t < nfsmw::waits::kNumTypes; ++t) {
          hitch_waits_ns_[t] = nfsmw::waits::g_ns[t].load(std::memory_order_relaxed);
          hitch_times_waits_[t] = nfsmw::waits::g_times[t].load(std::memory_order_relaxed);
        }
        hitch_gpu_waits_ns_ = gpu_waits_ns_;
        hitch_draws_ = nfsmw::waits::g_draws.load(std::memory_order_relaxed);
        hitch_ring_ns_ = nfsmw::waits::g_working_ring_ns.load(std::memory_order_relaxed);
        hitch_textures_ns_ = nfsmw::waits::g_textures_ns.load(std::memory_order_relaxed);
        hitch_uploads_textures_ = nfsmw::waits::g_uploads_textures.load(std::memory_order_relaxed);
        hitch_uploaded_bytes_ = nfsmw::waits::g_uploaded_bytes.load(std::memory_order_relaxed);
        hitch_created_textures_ = nfsmw::waits::g_created_textures.load(std::memory_order_relaxed);
        hitch_raw_fingerprint_ns_ = nfsmw::waits::g_raw_fingerprint_ns.load(std::memory_order_relaxed);
        hitch_data_fingerprint_ns_ = nfsmw::waits::g_data_fingerprint_ns.load(std::memory_order_relaxed);
        hitch_fingerprint_bytes_ = nfsmw::waits::g_fingerprint_bytes.load(std::memory_order_relaxed);
        hitch_deferred_fingerprints_ = nfsmw::waits::g_deferred_fingerprints.load(std::memory_order_relaxed);
        hitch_copies_waiting_ns_ = nfsmw::waits::g_copies_waiting_ns.load(std::memory_order_relaxed);  // 170
        hitch_copies_waits_ = nfsmw::waits::g_copies_waits.load(std::memory_order_relaxed);
        hitch_copies_helping_ns_ = nfsmw::waits::g_copies_helping_ns.load(std::memory_order_relaxed);  // 185
        hitch_assisted_copies_ = nfsmw::waits::g_assisted_copies.load(std::memory_order_relaxed);
        // Creating textures and the binding thread (nfsmw_nativo_texturas_enlace_hilo).
        hitch_textures_create_ns_ = nfsmw::waits::g_textures_create_ns.load(std::memory_order_relaxed);
        hitch_thread_bound_textures_ = nfsmw::waits::g_thread_bound_textures.load(std::memory_order_relaxed);
        hitch_bindings_waiting_ns_ = nfsmw::waits::g_bindings_waiting_ns.load(std::memory_order_relaxed);
        // The texture fingerprint thread (nfsmw_nativo_texturas_huella_hilo).
        hitch_thread_fingerprint_textures_ = nfsmw::waits::g_thread_fingerprint_textures.load(std::memory_order_relaxed);
        hitch_thread_fingerprint_ns_ = nfsmw::waits::g_thread_fingerprint_ns.load(std::memory_order_relaxed);
        hitch_snapshot_fingerprint_ns_ = nfsmw::waits::g_snapshot_fingerprint_ns.load(std::memory_order_relaxed);
        hitch_ring_fingerprint_textures_ = nfsmw::waits::g_ring_fingerprint_textures.load(std::memory_order_relaxed);
        hitch_ring_fingerprint_ns_ = nfsmw::waits::g_ring_fingerprint_ns.load(std::memory_order_relaxed);
        hitch_fingerprints_waiting_ns_ = nfsmw::waits::g_fingerprints_waiting_ns.load(std::memory_order_relaxed);
        hitch_output_wait_ns_ = output_wait_ns_;
        hitch_gpu_ns_ = gpu_ns_;
        hitch_record_ns_ = record_ns_;
        hitch_copies_ = copies_;
        hitch_restore_ = restores_;
        hitch_waits_ = gpu_waits_;
        hitch_clears_ = clears_;
        hitch_resolves_ = swap_count_;
      }
      last_swap_ = now_time;
    }
    const uint32_t base = (swap.dword[1] & 0xFFFFF000) & 0x1FFFFFFF;
    DiagReadersOnPresent(base);  // nfsmw_nativo_diag_lectores_s
    // nfsmw_nativo_frontal_perezoso. Before submitting the work: either it is drawn from the image that
    // holds the content (no copy) or the deferred copy is recorded now, ahead of the output.
    Image* const front_source = FrontOnPresent(base, width_px, height_px);
    const bool tile =
        presenter_ptr && REXCVAR_GET(nfsmw_nativo_diag_resueltas) && ComposeTile(base);
    // Without waiting for the GPU: the output goes after the work on the same queue.
    if (!presenter_ptr || !SendJob(false)) {
      return false;
    }
    if (tile) {
      bool tile_painted = false;
      presenter_ptr->RefreshGuestOutput(
          1280, 720, 1280, 720, [&](rex::ui::Presenter::GuestOutputRefreshContext& base_contexto) {
            auto& contexto =
                static_cast<VulkanPresenter::VulkanGuestOutputRefreshContext&>(base_contexto);
            tile_painted = PaintOutput(contexto, tile_, 1280, 720);
            return tile_painted;
          });
      if (tile_painted) {
        ++presented_;
      }
      return tile_painted;
    }
    auto it = resolved_.find(base);
    if (it == resolved_.end() || !it->second.img.prepared) {
      Reject(4, "Swap sin textura resuelta: se pinta el color de prueba");
      return false;
    }
    Resolved& resolved = it->second;
    const uint32_t w = std::min(width_px ? width_px : resolved.img.width_px, resolved.img.width_px);
    const uint32_t h = std::min(height_px ? height_px : resolved.img.height_px, resolved.img.height_px);
    bool painted = false;
    // How much of RefreshGuestOutput belongs to the SDK (before and after the callback) and how much to
    // PintarSalida.
    const auto before_refresh = std::chrono::steady_clock::now();
    auto call_entry = before_refresh;
    auto call_output = before_refresh;
    bool call = false;
    presenter_ptr->RefreshGuestOutput(
        w, h, 1280, 720, [&](rex::ui::Presenter::GuestOutputRefreshContext& base_contexto) {
          call_entry = std::chrono::steady_clock::now();
          call = true;
          auto& contexto =
              static_cast<VulkanPresenter::VulkanGuestOutputRefreshContext&>(base_contexto);
          painted = PaintOutput(contexto, front_source ? *front_source : resolved.img, w, h,
                                 front_source != nullptr);  // nfsmw_nativo_frontal_perezoso
          call_output = std::chrono::steady_clock::now();
          return painted;
        });
    if (call) {
      before_call_ns_ += Ns(before_refresh, call_entry);
      in_call_ns_ += Ns(call_entry, call_output);
      after_call_ns_ += Ns(call_output, std::chrono::steady_clock::now());
      ++refreshes_;
    }
    if (painted) {
      ++presented_;
    }
    return painted;
  }

  // nfsmw_nativo_compuesta_perezosa (see the cvar's comment). PM4 ring thread only.
  // It applies to the (address, source) pair that the diagnostic's watching has seen for kCompuestaAMirar
  // writes without any read after the source was written again. Until then, it copies as always (WATCHING).
  bool CompositeApplicable(uint32_t address, uint32_t src) {
    if (composite_disabled_ || !REXCVAR_GET(nfsmw_nativo_compuesta_perezosa)) {
      return false;
    }
    for (const DiagWatched& v : diag_watched_) {
      if (v.address != address || v.src != src) {
        continue;
      }
      if (v.late != 0 || v.writes < kCompositeToPeek) {
        return false;
      }
      if (composite_applying_ != address) {
        composite_applying_ = address;
        NFSMW_RING_REPORT("[nativo] C2 compuesta perezosa: {:08X} desde {:03X}/{}: {} escrituras vigiladas, {} "
                             "leidas antes de volver a escribir su origen y 0 despues: APLICANDO (la copia se aplaza; "
                             "se graba antes del primer dibujo que la lea y se tira al escribir su origen)",
                             address, src & 0xFFF, src >> 12, v.writes, v.before);
      }
      return true;
    }
    return false;
  }

  // From Copiar's 1 to 1 path: true if the copy is deferred (not recorded now).
  bool DeferComposite(uint32_t address, Image& target, const Resolved& resolved, const VkImageCopy& copy_info,
                        uint32_t src) {
    if (composite_has_ || REXCVAR_GET(nfsmw_nativo_diag_resueltas) || REXCVAR_GET(nfsmw_nativo_intercambiar_color)) {
      return false;
    }
    if (copy_info.srcOffset.x != 0 || copy_info.srcOffset.y != 0 || copy_info.dstOffset.x != 0 || copy_info.dstOffset.y != 0 ||
        copy_info.extent.width != resolved.img.width_px || copy_info.extent.height != resolved.img.height_px ||
        target.format_id != kColorFormat || resolved.img.format_id != kColorFormat) {
      return false;
    }
    // The small ones are read back for the guest (LeerResuelta): never those.
    const int32_t read_texels = REXCVAR_GET(nfsmw_nativo_leer_resueltas_texels);
    if (uint64_t(copy_info.extent.width) * copy_info.extent.height <= uint64_t(std::max<int32_t>(read_texels, 0))) {
      return false;
    }
    if (!CompositeApplicable(address, src)) {
      return false;
    }
    composite_.address = address;
    composite_.src = src;
    composite_.target = &target;
    composite_.vk_source = target.img;
    composite_.vk_texture = resolved.img.img;
    composite_.copy_info = copy_info;
    composite_has_ = true;
    WrittenResolved(address);  // new contents (deferred)
    ++composite_deferred_;
    return true;
  }

  // Records the deferred copy now (outside a pass). Exact: its source has not been touched since it was
  // deferred.
  void RecordComposite() {
    if (!composite_has_) {
      return;
    }
    composite_has_ = false;
    const CompositePending p = composite_;
    const auto r = resolved_.find(p.address);
    if (r == resolved_.end() || r->second.img.img != p.vk_texture || !p.target ||
        p.target->img != p.vk_source || !copy_image_ || !Record()) {
      composite_expired_ = p.address;
      CompositeDifference(p.address, "la copia aplazada ya no se puede grabar (cambio la imagen del origen o de la "
                                       "textura)");
      return;
    }
    if (draws_) {
      draws_->FinishPass();  // it can arrive from a draw: the copy goes outside the pass
    }
    MarkGpu(kGpuCopies);
    copy_image_(job_commands_, p.vk_source, VK_IMAGE_LAYOUT_GENERAL, p.vk_texture, VK_IMAGE_LAYOUT_GENERAL, 1,
                   &p.copy_info);
    ++copies_;
    TrackCopy(p.copy_info.extent.width, p.copy_info.extent.height);
    WrittenResolved(p.address, uint64_t(p.copy_info.extent.width) * p.copy_info.extent.height);
  }

  // Its source is about to be written and nobody has read it: it is dropped (or recorded, if the guard
  // turned it off). The texture is left without it until the next whole write; if someone samples it
  // before that, the guard fires.
  void CompositeBeforeSourceWrite() {
    if (!composite_has_) {
      return;
    }
    if (composite_disabled_) {
      RecordComposite();
      ++composite_other_recorded_;
      return;
    }
    composite_has_ = false;
    composite_expired_ = composite_.address;
    composite_saved_pixels_ += uint64_t(composite_.copy_info.extent.width) * composite_.copy_info.extent.height;
    ++composite_source_dropped_;
  }

  // Before each draw with a deferred copy or a stale texture: if the draw samples it, the deferred copy is
  // recorded first (exact) and the stale one trips the guard; if it draws into its source, the deferred
  // copy is dropped.
  void CompositeBeforeDraw(const DrawRequest& p) {
    if (!p.ps || !p.regs) {
      return;  // without a PS, no color is sampled or written
    }
    const uint32_t* r = p.regs;
    for (const SamplerShader& s : p.ps->samplers) {
      if (s.reg_index >= 16) {
        continue;
      }
      const uint32_t* f = r + kDiagRegFetch + uint32_t(s.reg_index) * 6;
      if ((f[0] & 0x3) != uint32_t(xenos::FetchConstantType::kTexture) ||
          ((f[5] >> 9) & 0x3) != uint32_t(xenos::DataDimension::k2DOrStacked)) {
        continue;
      }
      const uint32_t base = ((f[1] >> 12) << 12) & 0x1FFFFFFF;
      if (composite_has_ && base == composite_.address) {
        RecordComposite();
        ++composite_read_recorded_;
      } else if (composite_expired_ != 0 && base == composite_expired_) {
        ++composite_late_reads_;
        CompositeDifference(base, "un dibujo la muestrea despues de tirar su copia (ve la escena de antes de "
                                  "componer)");
        composite_expired_ = 0;
      }
    }
    if (composite_has_ && DrawWritesTo(r, composite_.src)) {
      CompositeBeforeSourceWrite();
    }
  }

  // Before each resolve to a texture (from Copiar, before copying anything): another write to the deferred
  // address. If it covers it entirely, the deferred copy is unnecessary (nobody has read it: exact);
  // otherwise it is recorded first.
  void CompositeBeforeTextureWrite(uint32_t address, bool whole_value) {
    if (composite_has_ && composite_.address == address) {
      if (whole_value) {
        composite_has_ = false;
        composite_saved_pixels_ += uint64_t(composite_.copy_info.extent.width) * composite_.copy_info.extent.height;
        ++composite_replaced_;
      } else {
        RecordComposite();
        ++composite_other_recorded_;
      }
    }
    if (whole_value && composite_expired_ == address) {
      composite_expired_ = 0;  // whole new content: it no longer misses the dropped copy
    }
  }

  // An image that is destroyed (Destruir) cannot stay in the deferred copy.
  void CompositeOnDestroy(const Image& img) {
    if (!composite_has_ || img.img == VK_NULL_HANDLE) {
      return;
    }
    if (img.img == composite_.vk_source) {
      composite_has_ = false;  // without its source there is no copy to make: the texture becomes stale
      composite_expired_ = composite_.address;
    } else if (img.img == composite_.vk_texture) {
      composite_has_ = false;  // the texture is rebuilt: its content is lost just as before
    }
  }

  void CompositeDifference(uint32_t address, const char* reason) {
    ++composite_differences_;
    if (composite_disabled_) {
      return;
    }
    composite_disabled_ = true;
    REXLOG_ERROR("[nativo] C2 compuesta perezosa: DIFERENCIA en {:08X}: {}. Apagada para el resto de la sesion: se "
                 "copia siempre, como antes de la 184",
                 address, reason);
  }

  // With the diagnostic's summary (every nfsmw_nativo_diag_lectores_s seconds).
  void CompositeReport() {
    if (!composite_deferred_ && !composite_differences_) {
      return;
    }
    const uint64_t frames = presented_ - composite_presented_previous_;
    const double mp = double(composite_saved_pixels_ - composite_pixels_previous_) / 1e6;
    const double per_frame = frames ? mp / double(frames) : 0.0;
    NFSMW_RING_REPORT("[nativo] C2 compuesta perezosa (build 184): {} copias aplazadas desde el arranque; {} "
                         "grabadas antes de un dibujo que la lee (exacto) y {} antes de otra escritura; {} tiradas al "
                         "escribir su origen y {} al taparla otra entera: {:.2f} Mpixeles por fotograma sin copiar "
                         "(~{:.2f} ms reales); lecturas tardias {}{}",
                         composite_deferred_, composite_read_recorded_, composite_other_recorded_,
                         composite_source_dropped_, composite_replaced_, per_frame, per_frame * 0.60,
                         composite_late_reads_,
                         composite_disabled_ ? " *** APAGADA POR LA GUARDIA ***" : " (0 = la imagen es la misma)");
    composite_pixels_previous_ = composite_saved_pixels_;
    composite_presented_previous_ = presented_;
  }

  // nfsmw_nativo_diag_lectores_s (see the cvar's comment). PM4 ring thread only.
  // On each Swap, with the front buffer it draws: opens the window (two whole frames, Swap to Swap) or closes
  // it.
  void DiagReadersOnPresent(uint32_t front_buf) {
    if (diag_window_) {
      if (resolved_.count(front_buf)) {
        DiagRead(front_buf, kSwapReader, 0, 0);
      }
      if (++diag_frame_ >= 2) {
        DiagReadersReport();
        diag_window_ = false;
        diag_writes_.clear();
      }
      return;
    }
    const int32_t cada = REXCVAR_GET(nfsmw_nativo_diag_lectores_s);
    if (cada <= 0) {
      diag_watched_.clear();  // turned off at run time: nothing is watched either
      return;
    }
    const auto now_time = std::chrono::steady_clock::now();
    if (now_time - diag_readers_last_ < std::chrono::seconds(cada)) {
      return;
    }
    diag_readers_last_ = now_time;
    diag_window_ = true;
    diag_frame_ = 0;
    diag_writes_.clear();
  }

  // A logical write to the resolved texture at `direccion`: the resolve the game requests (color or depth).
  void DiagWrite(uint32_t address, uint32_t src, uint32_t pitch, int32_t x0, int32_t y0,
                     uint32_t request_width, uint32_t request_height, uint32_t width_px, uint32_t height_px, uint64_t draws,
                     bool depth_value) {
    const uint32_t de = (src & 0xFFF) | (pitch << 12);
    for (DiagWatched& v : diag_watched_) {
      if (v.address == address) {
        v.pending = de == v.src;  // from here on, whoever reads it needs the watched copy
        v.written_source = false;
        v.writes += v.pending ? 1 : 0;
      }
    }
    if (!diag_window_) {
      return;
    }
    std::vector<DiagWriteRecord>& item_list = diag_writes_[address];
    if (item_list.size() >= kDiagMaxWrites) {
      ++diag_lost_writes_;
      return;
    }
    DiagWriteRecord e;
    e.frame = int32_t(diag_frame_);
    e.orden = 1;
    for (const DiagWriteRecord& another : item_list) {
      e.orden += another.frame == e.frame ? 1 : 0;
    }
    e.src = de;
    e.request_width = request_width;
    e.request_height = request_height;
    e.width_px = width_px;
    e.height_px = height_px;
    e.draws = draws;
    e.depth_value = depth_value;
    e.whole_value = x0 == 0 && y0 == 0 && request_width >= width_px && request_height >= height_px;
    item_list.push_back(std::move(e));
  }

  // A read of `direccion` in the window: it goes to its last write (or to the one before the window).
  void DiagRead(uint32_t address, uint32_t ps, uint32_t reg_index, uint32_t target) {
    std::vector<DiagWriteRecord>& item_list = diag_writes_[address];
    if (item_list.empty()) {
      item_list.emplace_back();  // what it already held before the window (frame -1)
    }
    DiagWriteRecord& e = item_list.back();
    for (DiagReader& l : e.readers) {
      if (l.ps == ps && l.reg_index == reg_index && l.target == target) {
        ++l.times;
        return;
      }
    }
    if (e.readers.size() >= kDiagMaxReaders) {
      ++e.others;
      return;
    }
    e.readers.push_back(DiagReader{ps, reg_index, target, 1});
  }

  // The resolved textures the PS of this draw samples, read from its fetch constants the way DibujosVulkan
  // reads them (sampler registers 0-15; 2D by their address and cubemaps by their 6 faces), bypassing the
  // caches.
  void DiagDrawReads(const DrawRequest& p) {
    if (!p.ps || !p.regs) {
      return;
    }
    const uint32_t* r = p.regs;
    const uint32_t target = (r[kDiagRegColorInfo] & 0xFFF) | ((r[kDiagRegSurfaceInfo] & 0x3FFF) << 12);
    for (const SamplerShader& s : p.ps->samplers) {
      if (s.reg_index >= 16) {
        continue;
      }
      const uint32_t* f = r + kDiagRegFetch + uint32_t(s.reg_index) * 6;
      if ((f[0] & 0x3) != uint32_t(xenos::FetchConstantType::kTexture)) {
        continue;
      }
      const uint32_t base = ((f[1] >> 12) << 12) & 0x1FFFFFFF;
      const uint32_t dimension = (f[5] >> 9) & 0x3;
      if (dimension == uint32_t(xenos::DataDimension::k2DOrStacked)) {
        if (resolved_.count(base)) {
          DiagRead(base, p.ps->number, s.reg_index, target);
          WatchRead(base, p.ps->number);
        }
      } else if (dimension == uint32_t(xenos::DataDimension::kCube)) {
        // The faces of a resolved cubemap are consecutive, as DibujosVulkan looks for them (8888: 4 bytes per
        // texel, rows and columns aligned to 32 and each face to 4 KB).
        const uint32_t height_px = ((f[2] >> 13) & 0x1FFF) + 1;
        const uint32_t pitch = std::max<uint32_t>(((f[0] >> 22) & 0x1FF) << 5, 1);
        const uint64_t stride_bytes =
            (uint64_t((pitch + 31) & ~uint32_t(31)) * 4 * ((height_px + 31) & ~uint32_t(31)) + 4095) & ~uint64_t(4095);
        for (uint32_t c = 0; c < 6; ++c) {
          const uint32_t face = uint32_t((uint64_t(base) + c * stride_bytes) & 0x1FFFFFFF);
          if (resolved_.count(face)) {
            DiagRead(face, p.ps->number, s.reg_index, target);
          }
        }
      }
    }
    DiagSourceWrittenPerDraw(r);  // after its reads: the draw reads its textures before writing
  }

  // Outside the window, with something watched: only the 2D address of each sampler is compared with the
  // watched ones.
  void DiagWatch(const DrawRequest& p) {
    if (!p.ps || !p.regs) {
      return;
    }
    const uint32_t* r = p.regs;
    for (const SamplerShader& s : p.ps->samplers) {
      if (s.reg_index >= 16) {
        continue;
      }
      const uint32_t* f = r + kDiagRegFetch + uint32_t(s.reg_index) * 6;
      if ((f[0] & 0x3) == uint32_t(xenos::FetchConstantType::kTexture) &&
          ((f[5] >> 9) & 0x3) == uint32_t(xenos::DataDimension::k2DOrStacked)) {
        WatchRead(((f[1] >> 12) << 12) & 0x1FFFFFFF, p.ps->number);
      }
    }
    DiagSourceWrittenPerDraw(r);
  }

  // A read of a watched address after its watched write. Before the source is written again, a deferred
  // copy would cover it (it would be recorded right before that draw); afterwards, dropping the copy when
  // the source is written would not be exact.
  void WatchRead(uint32_t address, uint32_t ps) {
    for (DiagWatched& v : diag_watched_) {
      if (v.address != address || !v.pending) {
        continue;
      }
      if (!v.written_source) {
        if (v.before++ == 0) {
          v.ps_before = ps;
          NFSMW_RING_REPORT("[nativo] C2 lectores (build 184): {:08X}, escrita desde {:03X}/{}, la lee el PS n{} "
                               "ANTES de volver a escribir su origen (tras {} escrituras vigiladas): una copia "
                               "aplazada la cubre",
                               address, v.src & 0xFFF, v.src >> 12, ps, v.writes);
        }
      } else if (v.late++ == 0) {
        v.late_ps = ps;
        NFSMW_RING_REPORT("[nativo] C2 lectores (build 184): {:08X}, escrita desde {:03X}/{}, la lee el PS n{} "
                             "DESPUES de escribir su origen (tras {} escrituras vigiladas): tirar esa copia al "
                             "escribir el origen NO seria exacto",
                             address, v.src & 0xFFF, v.src >> 12, ps, v.writes);
      }
    }
  }

  // A draw (or a clear) on render target `destino` (EDRAM base | pitch << 12): if it is the source of a
  // pending watched write, its content is no longer in the source.
  void DiagSourceWritten(uint32_t target) {
    for (DiagWatched& v : diag_watched_) {
      if (v.pending && !v.written_source && v.src == target) {
        v.written_source = true;
      }
    }
  }

  // Whether a draw writes to `destino`: its four color render targets with their write mask. Over-reporting
  // is harmless (it is treated as written earlier); under-reporting is not.
  static bool DrawWritesTo(const uint32_t* r, uint32_t target) {
    const uint32_t mask = r[kDiagRegColorMask];
    const uint32_t pitch = (r[kDiagRegSurfaceInfo] & 0x3FFF) << 12;
    for (uint32_t i = 0; i < 4; ++i) {
      if (((mask >> (4 * i)) & 0xF) != 0 && ((r[kDiagRegsColorInfo[i]] & 0xFFF) | pitch) == target) {
        return true;
      }
    }
    return false;
  }

  void DiagSourceWrittenPerDraw(const uint32_t* r) {
    for (DiagWatched& v : diag_watched_) {
      if (v.pending && !v.written_source && DrawWritesTo(r, v.src)) {
        v.written_source = true;
      }
    }
  }

  // When the window closes: one line per address (if its pattern has changed) and the summary line.
  void DiagReadersReport() {
    ++diag_windows_;
    std::vector<uint32_t> addresses;
    addresses.reserve(diag_writes_.size());
    for (const auto& [address, item_list] : diag_writes_) {
      addresses.push_back(address);
    }
    std::sort(addresses.begin(), addresses.end());
    const uint64_t texels_cpu = uint64_t(std::max<int32_t>(REXCVAR_GET(nfsmw_nativo_leer_resueltas_texels), 0));
    std::string left_over;
    uint32_t writes = 0, added_items = 0, all_equal = 0;
    for (const uint32_t address : addresses) {
      const std::vector<DiagWriteRecord>& item_list = diag_writes_[address];
      const auto r = resolved_.find(address);
      const uint32_t width_px = r != resolved_.end() ? r->second.img.width_px : 0;
      const uint32_t height_px = r != resolved_.end() ? r->second.img.height_px : 0;
      // The small ones are also copied to guest memory and read by the CPU (the exposure): they are never
      // unnecessary.
      const bool cpu = width_px && uint64_t(width_px) * height_px <= texels_cpu;
      std::string line;
      uint64_t signature = address;
      for (size_t i = 0; i < item_list.size(); ++i) {
        const DiagWriteRecord& e = item_list[i];
        if (e.frame < 0) {
          line += " [antes]";
        } else {
          ++writes;
          const bool blit = e.request_width * 4 >= e.width_px * 5 || e.request_height * 4 >= e.height_px * 5;
          line += fmt::format(" [{}.{}] {} {}x{} desde {:03X}/{}{} ({} dibujos antes)", e.frame, e.orden,
                               e.depth_value ? "profundidad" : (blit ? "blit" : "1:1"), e.request_width,
                               e.request_height, e.src & 0xFFF, e.src >> 12, e.whole_value ? "" : ", parcial",
                               e.draws);
        }
        signature = (signature ^ (uint64_t(uint32_t(e.frame + 1)) << 48 | uint64_t(e.orden) << 40 | e.src)) *
                0x100000001B3ull;
        if (e.readers.empty()) {
          const bool next = i + 1 < item_list.size();
          if (cpu) {
            line += " -> ningun dibujo (la lee la CPU)";
          } else if (e.frame >= 0 && next && item_list[i + 1].whole_value) {
            line += " -> NADIE antes de la siguiente, que la tapa entera: SOBRA";
            left_over += fmt::format(" {:08X} [{}.{}] desde {:03X}/{}", address, e.frame, e.orden,
                                  e.src & 0xFFF, e.src >> 12);
            signature ^= 0x5A5Au;
            AddWatched(address, e.src);
          } else {
            line += next ? " -> nadie antes de la siguiente (que no la tapa entera)"
                               : " -> nadie hasta el final de la ventana";
          }
        } else {
          line += " ->";
          for (const DiagReader& l : e.readers) {
            if (l.ps == kSwapReader) {
              line += fmt::format(" Swap x{}", l.times);
            } else {
              line += fmt::format(" PS n{} s{} x{} en {:03X}/{}", l.ps, l.reg_index, l.times, l.target & 0xFFF,
                                   l.target >> 12);
            }
            signature = (signature ^ (uint64_t(l.ps) << 32 | uint64_t(l.reg_index) << 26 | (l.target & 0x3FFFFFF))) *
                    0x100000001B3ull;
          }
          if (e.others) {
            line += fmt::format(" (y {} lectores mas)", e.others);
          }
        }
        line += ";";
      }
      uint64_t& prev = diag_signatures_[address];
      if (prev == signature && diag_windows_ > 3) {
        ++all_equal;  // the same pattern as the last time it was written: not repeated
        continue;
      }
      prev = signature;
      ++added_items;
      NFSMW_RING_REPORT("[nativo] C2 lectores (build 184, ventana {}): {:08X} {}x{}:{}", diag_windows_, address,
                           width_px, height_px, line);
    }
    std::string watched;
    for (const DiagWatched& v : diag_watched_) {
      watched += fmt::format(" {:08X} desde {:03X}/{}: {} escrituras; {} lecturas antes de volver a escribir el "
                               "origen{} y {} despues{};",
                               v.address, v.src & 0xFFF, v.src >> 12, v.writes, v.before,
                               v.before ? fmt::format(" (la primera, PS n{})", v.ps_before) : std::string(),
                               v.late,
                               v.late ? fmt::format(" (la primera, PS n{}): tirarla NO seria exacto", v.late_ps)
                                         : std::string(v.writes ? " (tirarla al escribir el origen seria exacto)"
                                                                    : ""));
    }
    NFSMW_RING_REPORT("[nativo] C2 lectores de las resueltas (build 184, ventana {}, 2 fotogramas cada {} s): {} "
                         "direcciones y {} escrituras; {} lineas escritas y {} iguales a la ultima escrita (no se "
                         "repiten){}; escrituras que nadie lee antes de otra que las tapa entera:{} | vigiladas en "
                         "todos los fotogramas:{}",
                         diag_windows_, REXCVAR_GET(nfsmw_nativo_diag_lectores_s), addresses.size(), writes,
                         added_items, all_equal,
                         diag_lost_writes_
                             ? fmt::format(" ({} escrituras sin apuntar por el tope de {} por direccion)",
                                           diag_lost_writes_, kDiagMaxWrites)
                             : std::string(),
                         left_over.empty() ? std::string(" ninguna") : left_over,
                         watched.empty() ? std::string(" ninguna") : watched);
    CompositeReport();  // nfsmw_nativo_compuesta_perezosa
  }

  // A write that is unnecessary in a window becomes watched in every frame (at most kDiagMaxVigiladas).
  void AddWatched(uint32_t address, uint32_t src) {
    for (const DiagWatched& v : diag_watched_) {
      if (v.address == address && v.src == src) {
        return;
      }
    }
    if (diag_watched_.size() < kDiagMaxWatched) {
      DiagWatched v;
      v.address = address;
      v.src = src;
      diag_watched_.push_back(v);
    }
  }

  // Per-draw diagnostic window, one whole frame every N seconds. It is decided
  // in the Swap because the upload buffer rotates several times per frame.
  void OpenDiagnosticWindow() {
    const int32_t cada = REXCVAR_GET(nfsmw_nativo_estadisticas_por_dibujo_s);
    if (cada <= 0) {
      diagnostic_window_ = false;
      return;
    }
    const auto now_time = std::chrono::steady_clock::now();
    if (diagnostic_window_) {
      diagnostic_window_ = false;  // that frame was already measured
      return;
    }
    if (std::chrono::duration_cast<std::chrono::seconds>(now_time - last_window_).count() >= cada) {
      last_window_ = now_time;
      diagnostic_window_ = true;
      read_window_ = false;
    }
  }

  void Stats(uint64_t& copies, uint64_t& clears, uint64_t& presented,
                    uint64_t& rechazos) const override {
    copies = copies_;
    clears = clears_;
    presented = presented_;
    rechazos = rechazos_;
  }

  // Depth copies (the only ones that force the depth tile to be stored).
  uint64_t DepthCopies() const override { return depth_copies_; }
  uint64_t DepthClears() const override { return depth_clears_; }

  void GpuTime(uint64_t& nanosegundos, uint64_t& jobs) const override {
    nanosegundos = gpu_ns_;
    jobs = gpu_jobs_;
  }

  uint64_t GpuEndMarkNs() const override { return uint64_t(double(last_gpu_mark_) * mark_period_ns_); }
  uint64_t PreciseMarksJobs() const override { return gpu_precise_jobs_; }

  void GpuWaits(uint64_t& times, uint64_t& nanosegundos) const override {
    times = gpu_waits_;
    nanosegundos = gpu_waits_ns_;
  }

  void GpuJobsDuration(uint64_t& times, uint64_t& nanosegundos) const override {
    times = jobs_gpu_;
    nanosegundos = gpu_job_ns_;
  }

  void RecordCost(uint64_t cost[6]) const override {
    cost[0] = recordings_;
    cost[1] = record_ns_;
    cost[2] = pools_restart_ns_;
    cost[3] = written_reads_;
    cost[4] = written_texels_;
    cost[5] = reads_write_ns_;
  }

  // C2 report: waits of the ring for the GPU in CompletarUna.
  uint64_t gpu_waits_ = 0;
  uint64_t gpu_waits_ns_ = 0;
  uint64_t jobs_gpu_ = 0;     // work units timed with the wall clock
  uint64_t gpu_job_ns_ = 0;   // and how long they took from submit to fence
  // C2 report: cost of Grabar and of the readbacks.
  uint64_t recordings_ = 0;
  uint64_t record_ns_ = 0;
  uint64_t pools_restart_ns_ = 0;
  uint64_t written_reads_ = 0;
  uint64_t written_texels_ = 0;
  uint64_t reads_write_ns_ = 0;

  void GpuTimePerCategory(
      std::array<uint64_t, kGpuCategories>& nanosegundos) const override {
    nanosegundos = gpu_categories_ns_;
  }

  void PipelineStats(std::array<uint64_t, kGpuCategories>& fragments,
                            std::array<uint64_t, kGpuCategories>& vertices,
                            std::array<uint64_t, kGpuCategories>& primitives) const override {
    fragments = category_fragments_;
    vertices = category_vertices_;
    primitives = category_primitives_;
  }

  void CopiesBySize(std::array<uint64_t, 4>& copies, std::array<uint64_t, 4>& pixels) const override {
    copies = bucket_copies_;
    pixels = bucket_pixels_;
  }

  void StatsPerShader(std::vector<uint64_t>& fragments, std::vector<uint64_t>& draws,
                             uint64_t& frames) const override {
    fragments = fragments_per_shader_;
    draws = draws_per_shader_;
    frames = diagnostic_frames_;
  }

  void PresentCost(uint64_t cost[12]) const override {
    const uint64_t values[12] = {output_waits_,   output_wait_ns_,  job_sends_,  job_lock_ns_,
                                  job_submit_ns_, output_sends_,     output_lock_ns_, output_submit_ns_,
                                  refreshes_,        before_call_ns_, in_call_ns_,     after_call_ns_};
    std::copy(std::begin(values), std::end(values), cost);
  }

  void IntervalsBetweenSwaps(std::array<uint64_t, kSwapBuckets>& buckets, uint64_t& overlaps,
                            double& worst_ms) const override {
    buckets = swap_buckets_;
    worst_ms = worst_swap_ms_;
    worst_swap_ms_ = 0.0;  // reset on every report: what matters is the worst of this interval
    overlaps = gpu_overlaps_;
  }

  bool Draw(const DrawRequest& req_info) override {
    if (composite_has_ || composite_expired_ != 0) {
      CompositeBeforeDraw(req_info);  // nfsmw_nativo_compuesta_perezosa
    }
    if (diag_window_) {
      DiagDrawReads(req_info);  // nfsmw_nativo_diag_lectores_s
    } else if (!diag_watched_.empty()) {
      DiagWatch(req_info);
    }
    return draws_ && draws_->Draw(req_info);
  }

  DrawStats GetDrawStats() const override {
    return draws_ ? draws_->Stats() : DrawStats{};
  }

  void WaitUploads() override {
    if (draws_) {
      draws_->WaitUploads();
    }
  }

  size_t PendingCopies() const override {  // fence measurement only
    return draws_ ? draws_->PendingCopies() : 0;
  }

  // --- ContextoDestinos (piezas C3-C6) ---------------------------------------

  VkCommandBuffer JobCommands() override {
    return Record() ? job_commands_ : VK_NULL_HANDLE;
  }

  VkCommandBuffer UploadCommands() override {
    if (!Record()) {
      return VK_NULL_HANDLE;
    }
    if (!upload_recording_) {
      VkCommandBufferBeginInfo start{};
      start.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      start.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      if (dfn_.vkBeginCommandBuffer(upload_commands_, &start) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
      }
      upload_recording_ = true;
    }
    return upload_commands_;
  }

  uint64_t CommandsGeneration() const override { return commands_generation_; }

  NativeImage* ColorTarget(uint32_t base, uint32_t format_id, uint32_t pitch) override {
    Image* img = GetTarget(base, format_id, pitch);
    if (!img || !Record()) {
      return nullptr;
    }
    Prepare(*img);
    BeforeColorWrite(*img);  // nfsmw_nativo_frontal_perezoso
    // Mandatory since color is also swapped. Here a pass starts that draws on top; if the content went away
    // in a swap and nobody has cleared it, it has to be brought back. Without this, the first frame that
    // draws without clearing shows the previous frame.
    RestoreContent(*img);
    return img;
  }

  NativeImage* DepthTarget(uint32_t base, uint32_t format_id, uint32_t pitch) override {
    Image* img = GetDepth(base, format_id, pitch);
    if (!img || !Record()) {
      return nullptr;
    }
    Prepare(*img);
    BeforeDepthWrite(*img);  // nfsmw_nativo_profundidad_perezosa
    // A pass that draws on top starts here, so if the content went away in a swap and nobody has cleared it
    // since then, it has to be brought back.
    RestoreContent(*img);
    return img;
  }

  void TrackTextureSize(uint32_t address, uint32_t width_px, uint32_t height_px) override {
    auto& byte_size = texture_sizes_[address];
    if (byte_size.first != width_px || byte_size.second != height_px) {
      byte_size = {width_px, height_px};
      if (warned_.insert(0x6000000u | (address & 0xFFFFFF)).second) {
        REXLOG_INFO("[nativo] C2: la textura en {:08X} se muestrea como {}x{}: sus resueltas tendran ese tamano",
                    address, width_px, height_px);
      }
    }
  }

  const NativeImage* ResolvedTexture(uint32_t address) override {
    const auto it = resolved_.find(address);
    // Watchdog of nfsmw_nativo_restaurar_por_intercambio. This is the only door through which draws reach a
    // resolved texture, so if someone requests one that is lent out (its content was given back to the render
    // target and it has not been resolved again) the lending is not safe and the previous frame's image would
    // be sampled. It must stay at 0 in the log.
    if (!borrowed_.empty() && borrowed_.count(address)) {
      ++read_borrowed_;
    }
    if (it == resolved_.end()) {
      return nullptr;
    }
    // And how many times each address is requested, for the per-render-target copy report. A resolved
    // texture that is copied every frame and never requested is a copy that is not needed. It is a ++ on the
    // entry that has already been looked up: it costs nothing even though this is called thousands of times
    // per frame.
    ++it->second.reads;
    if (address == nfsmw::demand_reflection::kAddress) {
      nfsmw::demand_reflection::TrackRead();  // nfsmw_reflejo_bajo_demanda
    }
    // nfsmw_nativo_profundidad_perezosa. Who requests each depth texture; if its copy is deferred and this is
    // a real sampling, it is recorded now, before this draw.
    if (it->second.img.format_id == depth_format_) {
      TrackDepthRead(address);
    } else if (!presented_fronts_.empty() && presented_fronts_.count(address)) {
      TrackFrontRead(address);  // nfsmw_nativo_frontal_perezoso
    }
    return it->second.img.prepared ? &it->second.img : nullptr;
  }

  // nfsmw_nativo_profundidad_perezosa. DibujosVulkan sets it around the textures of the final composition
  // when its depth sampling is dead (no blur).
  void DeadDepthReads(bool dead) override { dead_depth_reads_ = dead; }

  // nfsmw_nativo_sombra_minimo, what the draws ask (see the cvar's comment).
  // When a pass is opened on this depth: if it is the car pass of the current cycle, each draw is validated.
  bool CarShadowPass(const NativeImage* depth_value, bool only_depth) override {
    if (!depth_value || depth_value != sm_cycle_target_ || sm_state_ != kSmCars) {
      return false;
    }
    if (!only_depth) {
      ShadowMinCycleFailure("el pase de los coches tiene destino de color", 0, true);
    }
    return true;
  }

  // A draw of the car pass. `exacto`: it leaves in the depth the minimum of what was there and of its
  // fragments.
  void CarShadowDraw(bool exact_val, uint32_t control_z) override {
    ++sm_cars_draws_;
    if (!exact_val) {
      ++sm_exact_no_draws_;
      ShadowMinCycleFailure("un dibujo de los coches no deja el minimo (el dato es su control de Z)", control_z, true);
    }
  }

  // The texture with cars that the draws must look at: whenever it holds the cars only, and otherwise while
  // watching or applying. 0 = none.
  uint32_t CarShadowAddress() const override {
    return (sm_virtual_ || (sm_enabled_ && sm_phase_ != kSmDisabled)) ? sm_cars_ : 0;
  }

  // A draw samples the texture with cars. `capaz`: its pixel shader has tfetch2DSombraMin in that register
  // (`ps` is its number, for the log). Returns the partner to take the minimum with, or nullptr to sample
  // it as is.
  const NativeImage* CarShadowCompanion(bool capable, uint32_t ps) override {
    if (sm_virtual_) {
      // It holds the cars only: without the minimum, the world would miss its shadows.
      const auto it = resolved_.find(sm_world_);
      const bool world = it != resolved_.end() && it->second.img.prepared;
      if (!sm_valid_virtual_ || !capable || !world) {
        ++sm_unable_reads_;
        ShadowMinDifference(!sm_valid_virtual_ ? "se muestrea la textura de los coches solos y la del mundo ya es de "
                                                     "otro ciclo (el dato es el pixel shader)"
                               : !capable ? "un pixel shader sin tfetch2DSombraMin muestrea la textura de los coches solos "
                                          "(el dato es su numero)"
                                        : "no esta la textura del mundo (el dato es el pixel shader)",
                               ps);
        return nullptr;
      }
      ++sm_reads_min_;
      return &it->second.img;
    }
    // It is the usual one (world and cars): who reads it and when is watched, so the minimum can be applied
    // without surprises.
    if (sm_state_ != kSmFree) {
      ++sm_reads_to_untimed_;
      ShadowMinUnfit("la textura de los coches se muestrea entre las dos resoluciones (el dato es el pixel shader)",
                         ps);
    }
    if (!capable) {
      ++sm_unable_reads_;
      ShadowMinUnfit("un pixel shader sin tfetch2DSombraMin muestrea la textura de los coches: biblioteca sin el "
                         "minimo? (el dato es su numero)",
                         ps);
      return nullptr;
    }
    const auto it = resolved_.find(sm_cars_);
    if (!sm_enabled_ || sm_phase_ == kSmDisabled || it == resolved_.end() || !it->second.img.prepared) {
      ++sm_normal_reads_;
      return nullptr;
    }
    // Minimum with itself (the same texel): the pipeline with the bit already exists when applying starts.
    ++sm_reads_if_same_;
    return &it->second.img;
  }

  // nfsmw_nativo_diag_borrados. What is known about the clears of a render target.
  struct ClearUse {
    uint32_t base = 0;             // EDRAM base, guest format and pitch (for the report)
    uint32_t format_id = 0;
    uint32_t pitch = 0;
    bool depth_value = false;
    bool in_pass = false;          // cleared by opening a pass (depth without TRANSFER_DST: ZCULL)
    bool open_state = false;          // there is a clear whose use is being measured
    uint32_t used_width = 0;      // since the last clear: the largest x1 and y1 used
    uint32_t height_used = 0;
    uint32_t max_width = 0;        // the same since startup (only grows)
    uint32_t max_height = 0;
    uint64_t cycles = 0;           // clears with their use already measured, since startup
    uint64_t clears = 0;         // in the report interval
    uint64_t clears_pixels = 0;
    uint64_t used_pixels = 0;   // of the cycles closed in the interval
    // nfsmw_nativo_borrar_area_util. The bottom band the last clear left uncleared.
    bool band = false;
    uint32_t band_from = 0;
    VkImage image_band = VK_NULL_HANDLE;
    VkClearColorValue color_band{};
    uint32_t completed_bands = 0;
    bool usable_area_disabled = false;  // its useful area is not stable: cleared entirely
  };

  // What is drawn in a pass on that render target (its renderArea). Called by DibujosVulkan in EmpezarPase.
  void TrackPassArea(const NativeImage* img, uint32_t width_px, uint32_t height_px) override {
    if (img) {
      TrackClearUse(*img, width_px, height_px);
    }
  }

  // A whole clear of that image. Closes the previous cycle (what has been used since the previous clear) and
  // opens another.
  void TrackDiagClear(const Image& img, uint32_t base, uint32_t format_id, uint32_t pitch, bool depth_value,
                         bool in_pass) {
    if (!diag_clears_) {
      return;
    }
    ClearUse& u = clears_use_[&img];
    if (u.open_state) {
      u.used_pixels += uint64_t(u.used_width) * u.height_used;
      ++u.cycles;
    }
    u.base = base;
    u.format_id = format_id;
    u.pitch = pitch;
    u.depth_value = depth_value;
    u.in_pass = in_pass;
    ++u.clears;
    u.clears_pixels += uint64_t(img.width_px) * img.height_px;
    u.used_width = 0;
    u.height_used = 0;
    u.open_state = true;
  }

  // The rectangle from 0 to width and from 0 to height of that image is used (a pass, a resolve, a restore
  // or a swap). It only counts on render targets that have been cleared at least once.
  void TrackClearUse(const Image& img, uint32_t width_px, uint32_t height_px) {
    // nfsmw_nativo_borrar_area_util. While any band remains uncleared it is checked even if the diagnostic
    // was turned off at run time: a pending band is always completed before it is used.
    if (clears_use_.empty() || (!diag_clears_ && !pending_bands_)) {
      return;
    }
    const auto it = clears_use_.find(&img);
    if (it == clears_use_.end()) {
      return;
    }
    ClearUse& u = it->second;
    width_px = std::min(width_px, img.width_px);
    height_px = std::min(height_px, img.height_px);
    // nfsmw_nativo_borrar_area_util. If this use reaches the band the last clear left uncleared (or its
    // first row, which a linear blit can read), it is cleared now, before the use: the result is the same
    // as if it had been cleared entirely.
    if (u.band && (u.image_band != img.img || height_px >= u.band_from)) {
      CompleteBand(img, u);
    }
    if (!diag_clears_) {
      return;  // it was only checked because of the band
    }
    u.used_width = std::max(u.used_width, width_px);
    u.height_used = std::max(u.height_used, height_px);
    u.max_width = std::max(u.max_width, width_px);
    u.max_height = std::max(u.max_height, height_px);
  }

  // nfsmw_nativo_borrar_area_util. Clears only the rows that render target uses, if they are already known.
  // Returns false if it has to be cleared entirely, as always.
  bool ClearColorUsableArea(Image& target, const VkClearColorValue& color) {
    if (!clear_usable_area_ || !diag_clears_ || !draws_ || target.format_id != kColorFormat) {
      return false;
    }
    const auto it = clears_use_.find(&target);
    if (it == clears_use_.end()) {
      return false;
    }
    ClearUse& u = it->second;
    if (u.usable_area_disabled || u.cycles < kCyclesUsableArea || u.max_height == 0) {
      return false;
    }
    // Up to the multiple of 64 that leaves at least one row of margin below what is used: a linear blit (the
    // resolve that shrinks the scene) can read the row below its rectangle, and that row must be cleared.
    const uint32_t height_px = std::min(target.height_px, (u.max_height + 64u) & ~63u);
    if (height_px >= target.height_px) {
      return false;
    }
    if (!draws_->ClearColorInPass(job_commands_, target, color, VkRect2D{{0, 0}, {target.width_px, height_px}})) {
      return false;
    }
    if (!u.band) {
      ++pending_bands_;
    }
    u.band = true;
    u.band_from = height_px;
    u.image_band = target.img;
    u.color_band = color;
    ++clears_usable_area_;
    pixels_usable_area_ += uint64_t(target.width_px) * (target.height_px - height_px);
    return true;
  }

  // The render target has been cleared entirely: no band is left.
  void RemoveBand(const Image& target) {
    if (clears_use_.empty()) {
      return;
    }
    const auto it = clears_use_.find(&target);
    if (it != clears_use_.end() && it->second.band) {
      it->second.band = false;
      if (pending_bands_) {
        --pending_bands_;
      }
    }
  }

  // Clears the band that was left uncleared, before a use that reaches it (outside a pass: called by Copiar,
  // RestaurarContenido, IntercambiarConResuelta and EmpezarPase before opening its own).
  void CompleteBand(const Image& img, ClearUse& u) {
    u.band = false;
    if (pending_bands_) {
      --pending_bands_;
    }
    ++u.completed_bands;
    ++completed_usable_area_;
    if (u.completed_bands >= 3) {
      u.usable_area_disabled = true;  // its useful area is not stable: from now on it is cleared entirely
    }
    const bool same_image = u.image_band == img.img;
    const uint32_t band_height = img.height_px > u.band_from ? img.height_px - u.band_from : 0;
    if (same_image && (!band_height || (draws_ && Record() &&
                                         draws_->ClearColorInPass(
                                             job_commands_, img, u.color_band,
                                             VkRect2D{{0, int32_t(u.band_from)}, {img.width_px, band_height}})))) {
      return;
    }
    // The image changed without going through a clear (should not happen) or the band could not be cleared.
    ++failures_usable_area_;
    if (!global_usable_area_disabled_) {
      global_usable_area_disabled_ = true;
      clear_usable_area_ = false;
      REXLOG_ERROR("[nativo] C2 borrar area util: DIFERENCIA, no se pudo completar la banda de {}x{} desde la fila {} "
                   "({}). Apagado para el resto de la sesion: se borra la imagen entera",
                   img.width_px, img.height_px, u.band_from, same_image ? "fallo el pase" : "la imagen cambio");
    }
  }

  // Every 20 s, per render target, what was cleared against what was used (C2 borrados por destino).
  // Clears cost ~0.075 ms real per Mpixel (measured, see "C2 borrados saltados").
  void ClearsReport() {
    diag_clears_ = REXCVAR_GET(nfsmw_nativo_diag_borrados);
    clear_usable_area_ = REXCVAR_GET(nfsmw_nativo_borrar_area_util) && !global_usable_area_disabled_;
    const auto now_time = std::chrono::steady_clock::now();
    if (clears_report_ == std::chrono::steady_clock::time_point{}) {
      clears_report_ = now_time;
      clears_report_presented_ = presented_;
      return;
    }
    if (now_time - clears_report_ < std::chrono::seconds(20)) {
      return;
    }
    clears_report_ = now_time;
    const uint64_t frames = presented_ - clears_report_presented_;
    clears_report_presented_ = presented_;
    if (diag_clears_ && frames > 0 && !clears_use_.empty()) {
      struct Row {
        const ClearUse* use;
        uint32_t width_px;
        uint32_t height_px;
        double clear_info;  // Mpixels per frame
        double used;
      };
      std::vector<Row> rows;
      double clear_total = 0.0, used_total = 0.0;
      for (const auto& [img, u] : clears_use_) {
        if (!u.clears) {
          continue;
        }
        const double clear_info = double(u.clears_pixels) / 1e6 / double(frames);
        // Each clear closes the previous one's cycle: in steady state, as many closed cycles as clears.
        const double used = std::min(clear_info, double(u.used_pixels) / 1e6 / double(frames));
        rows.push_back({&u, img->width_px, img->height_px, clear_info, used});
        clear_total += clear_info;
        used_total += used;
      }
      std::sort(rows.begin(), rows.end(),
                [](const Row& a, const Row& b) { return a.clear_info - a.used > b.clear_info - b.used; });
      std::string item_list;
      for (const Row& row : rows) {
        const ClearUse& u = *row.use;
        item_list += fmt::format(" | {:03X}/{} {} {}x{}{}: {:.2f} por fotograma, {:.2f} Mpixeles borrados, {:.2f} usados "
                             "(hasta {}x{})",
                             u.base, u.pitch, u.depth_value ? "prof" : "color", row.width_px, row.height_px,
                             u.in_pass ? " (por pase, ZCULL)" : "", double(u.clears) / double(frames),
                             row.clear_info, row.used, u.max_width, u.max_height);
      }
      NFSMW_RING_REPORT("[nativo] C2 borrados por destino (build 184, {} fotogramas): {:.2f} Mpixeles borrados y {:.2f} "
                           "usados por fotograma (sobran {:.2f}: ~{:.2f} ms reales){}",
                           frames, clear_total, used_total, clear_total - used_total,
                           (clear_total - used_total) * 0.075, item_list);
    }
    if (clears_usable_area_ || failures_usable_area_) {  // nfsmw_nativo_borrar_area_util
      const double mp =
          double(pixels_usable_area_ - pixels_usable_area_previous_) / 1e6 / double(std::max<uint64_t>(frames, 1));
      NFSMW_RING_REPORT("[nativo] C2 borrar area util (build 184): {} borrados de color recortados desde el arranque; "
                           "{:.2f} Mpixeles por fotograma sin borrar (~{:.2f} ms reales); {} bandas completadas antes de "
                           "un uso; {} fallos{}",
                           clears_usable_area_, mp, mp * 0.075, completed_usable_area_, failures_usable_area_,
                           global_usable_area_disabled_ ? " *** APAGADO POR LA GUARDIA ***" : " (0 = la imagen es la misma)");
      pixels_usable_area_previous_ = pixels_usable_area_;
    }
    for (auto& [img, u] : clears_use_) {
      u.clears = 0;
      u.clears_pixels = 0;
      u.used_pixels = 0;
    }
  }

  // A request for a resolved depth texture (see nfsmw_nativo_profundidad_perezosa).
  void TrackDepthRead(uint32_t address) {
    if (dead_depth_reads_) {
      last_dead_read_[address] = presented_;
      if (!pending_.empty()) {
        const auto p = pending_.find(address);
        if (p != pending_.end()) {
          p->second.dead_read = true;
        }
      }
      return;
    }
    last_alive_read_[address] = presented_;
    if (!pending_.empty() && pending_.count(address)) {
      RecordPendingCopy(address);
      ++lazy_read_copied_;
    }
    if (!expired_.empty() && expired_.erase(address)) {
      ++lazy_late_reads_;
      if (!lazy_disabled_) {
        lazy_disabled_ = true;
        REXLOG_ERROR("[nativo] C2 profundidad perezosa: DIFERENCIA, un dibujo muestrea {:08X} despues de tirar su "
                     "copia (lectura tardia: lee una profundidad vieja). Apagada para el resto de la sesion: se "
                     "copia siempre",
                     address);
      }
    }
  }

  // Records the deferred copy of that address, outside a pass, and removes it from the list.
  void RecordPendingCopy(uint32_t address) {
    const auto p = pending_.find(address);
    if (p == pending_.end()) {
      return;
    }
    const PendingCopy pending = p->second;
    pending_.erase(p);
    const auto r = resolved_.find(address);
    if (r == resolved_.end() || r->second.img.img != pending.vk_target || !pending.src ||
        pending.src->img != pending.vk_source || !copy_image_ || !Record()) {
      expired_.insert(address);  // should not happen: every image change goes through the hooks first
      return;
    }
    if (draws_) {
      draws_->FinishPass();  // it can arrive from a draw (TexturaResuelta): the copy goes outside the pass
    }
    MarkGpu(kGpuCopies);
    copy_image_(job_commands_, pending.vk_source, VK_IMAGE_LAYOUT_GENERAL, pending.vk_target,
                   VK_IMAGE_LAYOUT_GENERAL, 1, &pending.copy_info);
    ++copies_;
    TrackCopy(pending.copy_info.extent.width, pending.copy_info.extent.height);
    WrittenResolved(address, uint64_t(pending.copy_info.extent.width) * pending.copy_info.extent.height);
  }

  // Before any write to a depth render target. The deferred copies that come from it are recorded (same
  // frame, or the composition has not requested them yet) or dropped (nobody has sampled them).
  void BeforeDepthWrite(const Image& img) {
    if (pending_.empty() || img.img == VK_NULL_HANDLE) {
      return;
    }
    for (auto it = pending_.begin(); it != pending_.end();) {
      if (it->second.vk_source != img.img) {
        ++it;
        continue;
      }
      const uint32_t address = it->first;
      // Dropped even within the same frame if the composite without blur already requested it (leida_muerta).
      // Keeping same-frame copies saved nothing: the game always rewrites the source within the same frame,
      // and the reader watch saw 0 reads after the rewrite. The guard catches a late read.
      if (!it->second.dead_read) {
        ++it;  // GrabarCopiaPendiente erases that entry: the iterator is already on the next one
        RecordPendingCopy(address);
        ++lazy_write_copied_;
      } else {
        lazy_saved_pixels_ += uint64_t(it->second.copy_info.extent.width) * it->second.copy_info.extent.height;
        it = pending_.erase(it);
        expired_.insert(address);
        ++lazy_dropped_;
      }
    }
  }

  // Defers the copy if, lately, the only reader of that address is the composite without blur.
  bool DeferDepthCopy(uint32_t address, const Image& src, const Resolved& resolved,
                               const VkImageCopy& copy_info) {
    if (lazy_disabled_ || !REXCVAR_GET(nfsmw_nativo_profundidad_perezosa)) {
      return false;
    }
    const auto dead_one = last_dead_read_.find(address);
    if (dead_one == last_dead_read_.end() || presented_ - dead_one->second > kLazyFrames) {
      return false;  // the composite without blur has not requested it lately
    }
    const auto viva = last_alive_read_.find(address);
    if (viva != last_alive_read_.end() && presented_ - viva->second <= kLazyFrames) {
      return false;  // something really samples it: copy as usual
    }
    PendingCopy& pending = pending_[address];
    pending.src = &src;
    pending.vk_source = src.img;
    pending.vk_target = resolved.img.img;
    pending.copy_info = copy_info;
    pending.frame = presented_;
    pending.dead_read = false;
    WrittenResolved(address);  // new contents (deferred): no longer lent or stale
    ++lazy_deferred_;
    return true;
  }

  // nfsmw_nativo_frontal_perezoso. Deferred copy to a front buffer, and the render target's spare images.
  struct PendingFront {
    Image* target = nullptr;           // source render target (destinos_ never erases: the pointer stays valid)
    VkImage vk_source = VK_NULL_HANDLE;  // image holding the content: the target's own or a retained one
    int32_t retained = -1;               // index in frontales_imagenes_ if it is no longer in the target
    VkImage vk_texture = VK_NULL_HANDLE;
    VkImageCopy copy_info{};
  };
  struct FrontImage {
    Image img;              // same size, format and usage as the target: swapped with it
    uint32_t retained_per = 0;  // address of the front buffer that retains it; 0 = free
  };

  // Is this address a front buffer read only by the Swap? A Swap presented it recently and no draw has
  // sampled it in that time.
  bool IsFrontOnlyOfSwap(uint32_t address) const {
    const auto was_presented = presented_fronts_.find(address);
    if (was_presented == presented_fronts_.end() || presented_ - was_presented->second > kFrontFrames) {
      return false;
    }
    const auto was_read = read_fronts_.find(address);
    return was_read == read_fronts_.end() || presented_ - was_read->second > kFrontFrames;
  }

  // The retained image of a deferred copy becomes free again.
  void FreeRetained(const PendingFront& pending) {
    if (pending.retained >= 0 && size_t(pending.retained) < images_fronts_.size()) {
      images_fronts_[size_t(pending.retained)].retained_per = 0;
    }
  }

  // The image holding the deferred copy's content, if it is still the same one; otherwise nullptr.
  Image* FrontSource(const PendingFront& pending) {
    Image* src = nullptr;
    if (pending.retained >= 0) {
      if (size_t(pending.retained) < images_fronts_.size()) {
        src = &images_fronts_[size_t(pending.retained)].img;
      }
    } else {
      src = pending.target;
    }
    return src && src->img == pending.vk_source && src->prepared ? src : nullptr;
  }

  // Records the deferred copy to that front buffer (outside a pass) and removes it from the list.
  void RecordFrontCopy(uint32_t address) {
    const auto p = pending_fronts_.find(address);
    if (p == pending_fronts_.end()) {
      return;
    }
    const PendingFront pending = p->second;
    pending_fronts_.erase(p);
    const auto r = resolved_.find(address);
    Image* const src = FrontSource(pending);
    if (r == resolved_.end() || r->second.img.img != pending.vk_texture || !src || !copy_image_ ||
        !Record()) {
      FreeRetained(pending);
      expired_fronts_.insert(address);  // should not happen: every image change goes through the hooks first
      return;
    }
    if (draws_) {
      draws_->FinishPass();  // may come from a draw (TexturaResuelta): the copy goes outside the pass
    }
    MarkGpu(kGpuCopies);
    copy_image_(job_commands_, pending.vk_source, VK_IMAGE_LAYOUT_GENERAL, pending.vk_texture,
                   VK_IMAGE_LAYOUT_GENERAL, 1, &pending.copy_info);
    ++copies_;
    TrackCopy(pending.copy_info.extent.width, pending.copy_info.extent.height);
    WrittenResolved(address, uint64_t(pending.copy_info.extent.width) * pending.copy_info.extent.height);
    expired_fronts_.erase(address);
    FreeRetained(pending);
  }

  // Another resolve (or an image swap) reaches that texture. If it covers the whole texture, the deferred
  // copy is unnecessary and is dropped: nobody will see its content. Otherwise it is recorded first.
  void ResolvePreviousFront(uint32_t address, bool whole_value) {
    if (whole_value && !expired_fronts_.empty()) {
      expired_fronts_.erase(address);  // fully covered: it is no longer missing any copy
    }
    if (pending_fronts_.empty()) {
      return;
    }
    const auto p = pending_fronts_.find(address);
    if (p == pending_fronts_.end()) {
      return;
    }
    if (!whole_value) {
      RecordFrontCopy(address);
      ++front_write_copied_;
      return;
    }
    front_saved_pixels_ += uint64_t(p->second.copy_info.extent.width) * p->second.copy_info.extent.height;
    FreeRetained(p->second);
    pending_fronts_.erase(p);
    ++front_replaced_;
  }

  // Defers the copy if it is 1:1, covers the whole texture from the target's corner, and that address is
  // read only by the Swap.
  bool DeferFrontCopy(uint32_t address, Image& target, const Resolved& resolved, const VkImageCopy& copy_info) {
    if (front_disabled_ || !REXCVAR_GET(nfsmw_nativo_frontal_perezoso) || REXCVAR_GET(nfsmw_nativo_diag_resueltas)) {
      return false;
    }
    if (copy_info.srcOffset.x != 0 || copy_info.srcOffset.y != 0 || copy_info.dstOffset.x != 0 || copy_info.dstOffset.y != 0 ||
        copy_info.extent.width != resolved.img.width_px || copy_info.extent.height != resolved.img.height_px ||
        target.format_id != kColorFormat || resolved.img.format_id != kColorFormat) {
      return false;
    }
    // Small ones are read back for the guest (LeerResuelta): those are never deferred.
    const int32_t read_texels = REXCVAR_GET(nfsmw_nativo_leer_resueltas_texels);
    if (uint64_t(copy_info.extent.width) * copy_info.extent.height <= uint64_t(std::max<int32_t>(read_texels, 0))) {
      return false;
    }
    if (!IsFrontOnlyOfSwap(address)) {
      return false;
    }
    PendingFront& pending = pending_fronts_[address];
    pending.target = &target;
    pending.vk_source = target.img;
    pending.retained = -1;
    pending.vk_texture = resolved.img.img;
    pending.copy_info = copy_info;
    WrittenResolved(address);  // new contents (deferred)
    expired_fronts_.erase(address);
    ++front_deferred_;
    return true;
  }

  // Before writing to a color target in any way other than a full clear (a pass, a restore, an image
  // swap): deferred copies whose source is its image are recorded now (exact).
  void BeforeColorWrite(const Image& target) {
    if (pending_fronts_.empty() || target.img == VK_NULL_HANDLE) {
      return;
    }
    for (auto it = pending_fronts_.begin(); it != pending_fronts_.end();) {
      if (it->second.retained >= 0 || it->second.vk_source != target.img) {
        ++it;
        continue;
      }
      const uint32_t address = it->first;
      ++it;  // GrabarCopiaFrontal erases that entry: the iterator is already on the next one
      RecordFrontCopy(address);
      ++front_write_copied_;
    }
  }

  // Before a full clear of a color target. If its image is the source of a deferred front buffer, the
  // target takes a free spare image of the same size and the one holding the content is retained for the
  // front buffer: the clear does not need the old content, so nothing is copied. Without a spare (or with
  // two front buffers from the same image), the copy is recorded first, as usual.
  void RotateFrontBeforeClear(Image& target) {
    if (pending_fronts_.empty() || target.img == VK_NULL_HANDLE) {
      return;
    }
    uint32_t base = 0;
    uint32_t how_many = 0;
    for (const auto& [address, pending] : pending_fronts_) {
      if (pending.retained < 0 && pending.vk_source == target.img) {
        base = address;
        ++how_many;
      }
    }
    if (how_many == 0) {
      return;
    }
    int32_t free_item = -1;
    if (how_many == 1) {
      for (size_t i = 0; i < images_fronts_.size(); ++i) {
        const Image& candidate_item = images_fronts_[i].img;
        if (images_fronts_[i].retained_per == 0 && candidate_item.img != VK_NULL_HANDLE &&
            candidate_item.width_px == target.width_px && candidate_item.height_px == target.height_px &&
            candidate_item.format_id == target.format_id) {
          free_item = int32_t(i);
          break;
        }
      }
      if (free_item < 0 && images_fronts_.size() < kFrontImagesMax) {
        FrontImage incoming;
        // Same usage flags as ObtenerDestino: the image becomes the render target.
        if (Make(incoming.img, target.width_px, target.height_px,
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                  target.format_id)) {
          Prepare(incoming.img);
          images_fronts_.push_back(incoming);
          free_item = int32_t(images_fronts_.size()) - 1;
          REXLOG_INFO("[nativo] C2 frontal perezoso: imagen de repuesto {} de {}x{} para el destino del frontal",
                      images_fronts_.size(), target.width_px, target.height_px);
        }
      }
    }
    if (free_item < 0) {
      ++front_without_spare_;
      BeforeColorWrite(target);  // the copy is recorded before the clear, as usual
      return;
    }
    Image& spare = images_fronts_[size_t(free_item)].img;
    std::swap(target.img, spare.img);
    std::swap(target.mem, spare.mem);
    std::swap(target.view, spare.view);
    std::swap(target.prepared, spare.prepared);
    images_fronts_[size_t(free_item)].retained_per = base;
    pending_fronts_[base].retained = free_item;  // origen_vk is still the content image, now retained
    if (draws_) {
      draws_->InvalidateImages(target.img, spare.img);
    }
    ++front_rotations_;
  }

  // A draw samples a front buffer. If its copy is deferred, it is recorded now (before the draw).
  void TrackFrontRead(uint32_t address) {
    read_fronts_[address] = presented_;
    if (!pending_fronts_.empty() && pending_fronts_.count(address)) {
      RecordFrontCopy(address);
      ++front_read_copied_;
    }
    if (!expired_fronts_.empty() && expired_fronts_.erase(address)) {
      ++front_late_reads_;
      if (!front_disabled_) {
        front_disabled_ = true;
        REXLOG_ERROR("[nativo] C2 frontal perezoso: DIFERENCIA, un dibujo muestrea el frontal {:08X} sin su ultima "
                     "copia. Apagado para el resto de la sesion: se copia siempre",
                     address);
      }
    }
  }

  // The Swap of that front buffer, before the submission. Returns the image that can be presented without
  // a copy (the target's or the retained one, with the front buffer in its corner) or nullptr: in that
  // case any deferred copy is recorded here (in the same submission, ahead of the output) and the output
  // reads the texture as usual.
  Image* FrontOnPresent(uint32_t address, uint32_t width_px, uint32_t height_px) {
    presented_fronts_[address] = presented_;
    if (!expired_fronts_.empty() && expired_fronts_.erase(address)) {
      ++front_late_reads_;
      if (!front_disabled_) {
        front_disabled_ = true;
        REXLOG_ERROR("[nativo] C2 frontal perezoso: DIFERENCIA, el Swap pinta el frontal {:08X} sin su ultima copia. "
                     "Apagado para el resto de la sesion: se copia siempre",
                     address);
      }
    }
    if (pending_fronts_.empty()) {
      return nullptr;
    }
    if (REXCVAR_GET(nfsmw_nativo_diag_resueltas)) {
      // The diagnostic grid shows every resolved texture: if it is enabled at runtime, no copy stays deferred.
      while (!pending_fronts_.empty()) {
        RecordFrontCopy(pending_fronts_.begin()->first);  // removes it from the list, whether it succeeds or not
        ++front_swap_copied_;
      }
      return nullptr;
    }
    const auto p = pending_fronts_.find(address);
    if (p == pending_fronts_.end()) {
      return nullptr;
    }
    const auto r = resolved_.find(address);
    Image* const src = FrontSource(p->second);
    bool can = src && r != resolved_.end() && r->second.img.img == p->second.vk_texture &&
                 r->second.img.prepared && gamma_ramp_ && !nfsmw::settings::AntialiasingFxaa() &&
                 !front_disabled_;
    if (can) {
      // Same computation as Presentar: the output has the texture's size (exact variant) and the copy
      // covers it.
      const Image& texture = r->second.img;
      const uint32_t w = std::min(width_px ? width_px : texture.width_px, texture.width_px);
      const uint32_t h = std::min(height_px ? height_px : texture.height_px, texture.height_px);
      can = w == texture.width_px && h == texture.height_px && p->second.copy_info.extent.width == w &&
              p->second.copy_info.extent.height == h;
    }
    if (!can) {
      RecordFrontCopy(address);
      ++front_swap_copied_;
      return nullptr;
    }
    if (p->second.retained >= 0) {
      ++front_retained_painted_;
    } else {
      ++front_target_painted_;
    }
    return src;
  }

  // Upload buffer full: submit what has been recorded and continue in the other slot, with its empty
  // upload buffer (Grabar only waits if that slot's last submission is still on the GPU).
  bool SendAndWait() override { return SendJob(false) && Record(); }

  // The function above submits and continues; this one waits. Same order as every other place that must
  // destroy something the GPU might still be reading (see the resolved texture that changes size).
  bool WaitGpuIdle() override {
    WaitGpu();
    return Record();
  }

  void MarkGpu(uint32_t category) override {
    if (queries_ == VK_NULL_HANDLE || !recording_) {
      return;
    }
    JobSlot& ranura = ranuras_[ranura_];
    if (ranura.categories.size() + 1 >= kMarksPerSlot ||
        (!ranura.categories.empty() && ranura.categories.back() == category)) {
      return;  // no room (the last one is for the end mark) or same category continues
    }
    write_mark_(job_commands_,
                    ranura.precise_marks ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    queries_, ranura_ * kMarksPerSlot + uint32_t(ranura.categories.size()));
    ranura.categories.push_back(uint8_t(category));
  }

  // The game's occlusion queries (see native_targets.h).
  void StartOcclusion(uint32_t base) override {
    if (open_occlusion_ != 0) {
      uint64_t discarded = 0;
      FinishOcclusion(0, discarded);
    }
    if (!draws_ || oclusiones_ == VK_NULL_HANDLE) {
      return;
    }
    if (game_occlusions_.size() >= 256) {
      game_occlusions_.clear();  // spans that were never read (should not happen): leftovers are ignored
    }
    open_occlusion_ = next_occlusion_++;
    game_occlusions_[open_occlusion_].base = base;
    draws_->OcclusionOpen(true);
  }

  bool FinishOcclusion(uint32_t base, uint64_t& sample_count) override {
    if (open_occlusion_ != 0) {
      if (draws_) {
        draws_->OcclusionOpen(false);
      }
      const auto it = game_occlusions_.find(open_occlusion_);
      open_occlusion_ = 0;
      if (it != game_occlusions_.end()) {
        it->second.finished = true;
        if (it->second.pending_segments == 0) {
          PublishOcclusion(it);  // no draws counted: 0 samples
        }
      }
    }
    const auto measure = oclusion_por_base_.find(base);
    if (measure == oclusion_por_base_.end()) {
      return false;
    }
    sample_count = measure->second;
    return true;
  }

  void OcclusionStats(uint64_t values[5]) const override {
    std::copy(std::begin(occlusion_stats_), std::end(occlusion_stats_), values);
  }

  uint32_t StartOcclusionQuery() override {
    if (oclusiones_ == VK_NULL_HANDLE || open_occlusion_ == 0 || !recording_) {
      return UINT32_MAX;
    }
    const auto it = game_occlusions_.find(open_occlusion_);
    if (it == game_occlusions_.end()) {
      return UINT32_MAX;
    }
    JobSlot& ranura = ranuras_[ranura_];
    if (ranura.oclusiones.size() >= kOclusionesPorRanura) {
      ++occlusion_stats_[1];
      it->second.failed = true;
      return UINT32_MAX;
    }
    const uint32_t index = ranura_ * kOclusionesPorRanura + uint32_t(ranura.oclusiones.size());
    dfn_.vkCmdBeginQuery(job_commands_, oclusiones_, index,
                         precise_occlusion_ ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    ranura.oclusiones.emplace_back(index, open_occlusion_);
    ++it->second.pending_segments;
    ++occlusion_stats_[0];
    return index;
  }

  void FinishOcclusionQuery(uint32_t index) override {
    if (oclusiones_ != VK_NULL_HANDLE && index != UINT32_MAX && recording_) {
      dfn_.vkCmdEndQuery(job_commands_, oclusiones_, index);
    }
  }

  // nfsmw_reflejo_visibilidad. Our own query around a single draw (the one that samples the reflection,
  // or the witness), inside the open pass. UINT32_MAX if there is no pool, nothing is being recorded or
  // there is no room left.
  uint32_t StartVisibilityQuery(bool sentinel) override {
    if (visibility_ == VK_NULL_HANDLE || !recording_) {
      return UINT32_MAX;
    }
    JobSlot& ranura = ranuras_[ranura_];
    if (ranura.visibility.size() >= kVisibilityPerSlot) {
      ++visibility_without_room_;
      return UINT32_MAX;
    }
    const uint32_t index = ranura_ * kVisibilityPerSlot + uint32_t(ranura.visibility.size());
    dfn_.vkCmdBeginQuery(job_commands_, visibility_, index, 0);
    ranura.visibility.emplace_back(index, sentinel ? kSentinelVisibility : kWaterVisibility);
    return index;
  }

  void FinishVisibilityQuery(uint32_t index) override {
    if (visibility_ != VK_NULL_HANDLE && index != UINT32_MAX && recording_) {
      dfn_.vkCmdEndQuery(job_commands_, visibility_, index);
    }
  }

  // One query per pass, from before vkCmdBeginRenderPass to after EndRenderPass.
  uint32_t StartStats(uint32_t category) override {
    // With the per-draw diagnostic enabled this one is not opened: two queries of the same type cannot be
    // active at once, and the per-pass query would enclose the per-draw ones.
    if (stats_ == VK_NULL_HANDLE || !recording_ ||
        !REXCVAR_GET(nfsmw_nativo_estadisticas_pipeline) ||
        REXCVAR_GET(nfsmw_nativo_estadisticas_por_dibujo_s) > 0) {
      return UINT32_MAX;
    }
    JobSlot& ranura = ranuras_[ranura_];
    if (ranura.stats.size() >= kStatsPerSlot) {
      ++stats_without_room_;
      if (stats_without_room_ % 200 == 1) {
        REXLOG_WARN("[nativo] C2: sin sitio para estadisticas de pasada ({} veces): el trabajo lleva mas de {} "
                    "pases", stats_without_room_, kStatsPerSlot);
      }
      return UINT32_MAX;
    }
    const uint32_t index =
        ranura_ * kStatsPerSlot + uint32_t(ranura.stats.size());
    dfn_.vkCmdBeginQuery(job_commands_, stats_, index, 0);
    ranura.stats.emplace_back(index, uint8_t(category));
    return index;
  }

  void FinishStats(uint32_t index) override {
    if (stats_ != VK_NULL_HANDLE && index != UINT32_MAX && recording_) {
      dfn_.vkCmdEndQuery(job_commands_, stats_, index);
    }
  }

  // One query per draw, tagged with its pixel shader.
  uint32_t StartDrawStats(uint32_t label, uint32_t category) override {
    if (draw_stats_ == VK_NULL_HANDLE || !recording_ || !diagnostic_window_) {
      return UINT32_MAX;
    }
    JobSlot& ranura = ranuras_[ranura_];
    if (ranura.draw_stats.size() >= kDrawStatsPerSlot) {
      ++draw_stats_without_room_;
      if (draw_stats_without_room_ % 2000 == 1) {
        REXLOG_WARN("[nativo] C2: sin sitio para estadisticas por dibujo ({} veces)",
                    draw_stats_without_room_);
      }
      return UINT32_MAX;
    }
    const uint32_t index = ranura_ * kDrawStatsPerSlot +
                            uint32_t(ranura.draw_stats.size());
    dfn_.vkCmdBeginQuery(job_commands_, draw_stats_, index, 0);
    ranura.draw_stats.emplace_back(
        index, uint16_t((category % kGpuCategories) * kShaderLabels + label % kShaderLabels));
    return index;
  }

  void FinishDrawStats(uint32_t index) override {
    if (draw_stats_ != VK_NULL_HANDLE && index != UINT32_MAX && recording_) {
      dfn_.vkCmdEndQuery(job_commands_, draw_stats_, index);
    }
  }

 private:
  bool Reject(uint32_t cause, const char* texto) {
    ++rechazos_;
    if (warned_.insert(cause).second) {
      REXLOG_WARN("[nativo] C2: {} (causa {})", texto, cause);
    }
    return false;
  }

  // Rectangle covered by the copy, as in GetResolveInfo (draw.cpp:787-889).
  bool Rect(const CopyRegisters& reg, uint32_t pitch, int32_t& x0, int32_t& y0,
                  int32_t& x1, int32_t& y1) {
    const uint32_t type = reg.fetch_vertices[0] & 0x3;
    const uint32_t address = reg.fetch_vertices[0] >> 2;
    const auto orden = static_cast<xenos::Endian>(reg.fetch_vertices[1] & 0x3);
    const uint32_t byte_size = (reg.fetch_vertices[1] >> 2) & 0xFFFFFF;
    if (type != uint32_t(xenos::FetchConstantType::kVertex) || byte_size != 3 * 2) {
      return Reject(5, "vertices de la copia en un formato no soportado");
    }
    const uint8_t* vertices = mem_->TranslatePhysical(address * 4);
    const float medio_pixel =
        (reg.pa_su_vtx_cntl & 0x1) == uint32_t(xenos::PixelCenter::kD3DZero) ? 0.5f : 0.0f;
    int32_t fixed[6];
    for (int i = 0; i < 6; ++i) {
      float value;
      std::memcpy(&value, vertices + i * 4, sizeof(value));
      fixed[i] = Fixed16p8(xenos::GpuSwap(value, orden) + medio_pixel);
    }
    x0 = (std::min({fixed[0], fixed[2], fixed[4]}) + 127) >> 8;
    y0 = (std::min({fixed[1], fixed[3], fixed[5]}) + 127) >> 8;
    x1 = (std::max({fixed[0], fixed[2], fixed[4]}) + 127) >> 8;
    y1 = (std::max({fixed[1], fixed[3], fixed[5]}) + 127) >> 8;

    const int32_t offset_x = SignExtend15(reg.pa_sc_window_offset & 0x7FFF);
    const int32_t offset_y = SignExtend15((reg.pa_sc_window_offset >> 16) & 0x7FFF);
    if ((reg.pa_su_sc_mode_cntl >> 16) & 0x1) {  // vtx_window_offset_enable
      x0 += offset_x;
      y0 += offset_y;
      x1 += offset_x;
      y1 += offset_y;
    }
    // Window scissor (GetScissor without clamping to the pitch).
    int32_t left = int32_t(reg.pa_sc_window_scissor_tl & 0x3FFF);
    int32_t up = int32_t((reg.pa_sc_window_scissor_tl >> 16) & 0x3FFF);
    int32_t right = int32_t(reg.pa_sc_window_scissor_br & 0x3FFF);
    int32_t down = int32_t((reg.pa_sc_window_scissor_br >> 16) & 0x3FFF);
    if (!((reg.pa_sc_window_scissor_tl >> 31) & 0x1)) {  // window_offset_disable
      left += offset_x;
      up += offset_y;
      right += offset_x;
      down += offset_y;
    }
    left = std::max(left, 0);
    up = std::max(up, 0);
    right = std::max(right, left);
    down = std::max(down, up);
    x0 = std::clamp(x0, left, right);
    y0 = std::clamp(y0, up, down);
    x1 = std::clamp(x1, left, right);
    y1 = std::clamp(y1, up, down);
    // D3D9 alinea a 8 (kResolveAlignmentPixels).
    x0 &= ~int32_t(7);
    y0 &= ~int32_t(7);
    x1 = (x1 + 7) & ~int32_t(7);
    y1 = (y1 + 7) & ~int32_t(7);
    const int32_t aligned_pitch = int32_t(pitch & ~uint32_t(7));
    x0 = std::min(x0, aligned_pitch);
    x1 = std::min(x1, aligned_pitch);
    if (x0 >= x1 || y0 >= y1) {
      return Reject(6, "rectangulo de copia vacio");
    }
    return true;
  }

  Image* GetTarget(uint32_t base, uint32_t format_id, uint32_t pitch) {
    // AirborneNX: the *_AS_* formats have the same EDRAM layout as their base format (only blending
    // precision differs), so both name the same EDRAM and the same image. UE3 draws the scene into 7e3 at
    // 0x2D0 and then adds the lights onto it as 2_10_10_10_FLOAT_AS_16_16_16_16 at the same base.
    if (format_id == uint32_t(xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16)) {
      format_id = uint32_t(xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
    } else if (format_id == uint32_t(xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10)) {
      format_id = uint32_t(xenos::ColorRenderTargetFormat::k_2_10_10_10);
    }
    if (!pitch) {
      Reject(7, "destino de render con pitch 0");
      return nullptr;
    }
    const uint64_t key = (uint64_t(base) << 20) | (uint64_t(format_id) << 16) | pitch;
    auto it = targets_.find(key);
    if (it != targets_.end()) {
      return &it->second;
    }
    /*
     * The height comes from the pitch, so the scene's color target measures 1280x1280 to draw 1280x720
     * and can never be swapped with its resolved texture. With a pitch of 1280 or less the game never
     * draws below 720 (measured viewport and scissor), so 720 rows would be enough and the sizes would
     * match. Above 1280 (the internal 1080p mode) the larger height is needed.
     */
    /*
     * Creating the color target 720 rows high (instead of deriving the height from the pitch), so that it
     * matches the resolved texture and the images can be swapped instead of copied, was tried: it breaks
     * the scene (the screen fills with a yellow smear). Reverted.
     */
    const uint32_t height_px = std::min(kTargetMaxHeight, std::max<uint32_t>(720, (pitch + 15) & ~15u));
    const VkFormat host_format = TargetHostFormat(format_id);
    if (host_format == VK_FORMAT_UNDEFINED) {
      Reject(100 + format_id, "formato de destino de render todavia no soportado");
      return nullptr;
    }
    Image img;
    if (!Make(img, pitch, height_px,
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
               host_format)) {
      Reject(8, "no se pudo crear un destino de render");
      return nullptr;
    }
    const VkClearColorValue diag = DiagnosticColor(base, format_id, pitch);
    REXLOG_INFO("[nativo] C2: destino de render base {:03X}, formato {}, {}x{} (color de "
                "diagnostico {:02X}{:02X}{:02X})",
                base, format_id, pitch, height_px, uint32_t(std::lround(diag.float32[0] * 255.0f)),
                uint32_t(std::lround(diag.float32[1] * 255.0f)),
                uint32_t(std::lround(diag.float32[2] * 255.0f)));
    return &targets_.emplace(key, img).first->second;
  }

  Image* GetDepth(uint32_t base, uint32_t format_id, uint32_t pitch) {
    if (!pitch || depth_format_ == VK_FORMAT_UNDEFINED) {
      Reject(11, "sin destino de profundidad (pitch 0 o formato no disponible)");
      return nullptr;
    }
    const uint64_t key = (uint64_t(base) << 20) | (uint64_t(format_id) << 16) | pitch;
    auto it = depths_.find(key);
    if (it != depths_.end()) {
      return &it->second;
    }
    const uint32_t height_px = std::min(kTargetMaxHeight, std::max<uint32_t>(720, (pitch + 15) & ~15u));
    // Smaller shadow map.
    //
    // The game requests two 1600x1600 maps per frame, and 1600 is not a quality choice: it is 2000 of
    // the 2048 80x16 tiles of the Xbox 360 EDRAM. The PC version of this same game uses 1024x1024. Here
    // the map is drawn at nfsmw_nativo_sombras_escala percent and upscaled when resolved, so the texture
    // the scene samples is the usual one and only the detail goes down.
    //
    // It is recognized by its 1600x1600 size, which no other render target uses. The scale is read only
    // once (when the first map is created) so that changing it at runtime does not leave old images unused.
    uint32_t image_width = pitch, image_height = height_px;
    uint32_t guest_width = 0, guest_height = 0;
    if (pitch == kShadowsSide && height_px == kShadowsSide) {
      if (shadows_scale_ == 0) {
        shadows_scale_ = uint32_t(std::clamp(REXCVAR_GET(nfsmw_nativo_sombras_escala), 50, 100));
        if (!blit_ || !scalable_depth_) {
          shadows_scale_ = 100;
        }
      }
      if (shadows_scale_ < 100) {
        image_width = std::max<uint32_t>(64, ((pitch * shadows_scale_ / 100) + 15) & ~15u);
        image_height = std::max<uint32_t>(64, ((height_px * shadows_scale_ / 100) + 15) & ~15u);
        guest_width = pitch;
        guest_height = height_px;
        REXLOG_INFO("[nativo] C2: mapa de sombras a {} %: se dibuja y se resuelve a {}x{} (el juego pide {}x{})",
                    shadows_scale_, image_width, image_height, pitch, height_px);
      }
    }
    // The usage flag that decides whether there is hierarchical depth culling.
    //
    // The driver only assigns a ZCULL plane if the usage flags fit in
    //   DEPTH_STENCIL_ATTACHMENT | TRANSFER_SRC | SAMPLED | INPUT_ATTACHMENT
    // (nvk_image.c). TRANSFER_DST disqualifies the image, and we only request it for
    // vkCmdClearDepthStencilImage, which the specification requires with that usage.
    //
    // TRANSFER_DST is dropped from the scene depth, which is 50 % of the frame, and kept for the shadow
    // map. Reason: the scene resolves 1280x720 out of a 1280x1280 image, so it is never "whole" and
    // never swaps with its resolved texture (console logs: thousands of "sin intercambio ... porque la
    // orden no borra el destino", 0 restores). The shadow map does swap, ~0.83 times per frame, and that
    // swap is worth 2.36 ms measured. So the scene gains ZCULL without losing a single swap.
    //
    // In exchange, that image can no longer be cleared with vkCmdClearDepthStencilImage: it is cleared
    // by opening a pass with loadOp = CLEAR (DibujosVulkan::BorrarProfundidadEnPase).
    // With nfsmw_nativo_zcull off, every image gets TRANSFER_DST: none receives a ZCULL plane and clears
    // go back to vkCmdClearDepthStencilImage, the path used before ZCULL.
    //
    // =========================================================================================
    // Do not remove TRANSFER_DST from the shadow map. Examined in depth and rejected.
    //
    // 1. It still needs it. The reasoning was that the map is no longer copied but swapped, so the flag
    //    would be unnecessary. Wrong: each swap is paid for with a restore, and a restore is exactly a
    //    copy into this image (RestaurarContenido -> copiar_imagen_ with destino.imagen as dst, which
    //    requires TRANSFER_DST). Steady race, three consecutive 10 s intervals:
    //        swaps without clear  4679 -> 4885 -> 5145 -> 5369   (+206, +260, +224)
    //        restores             3584 -> 3790 -> 4050 -> 4274   (+206, +260, +224)
    //    Exactly one restore per swap. The 1600x1600 copy did not go away: it is paid later, in
    //    DestinoProfundidad. Without TRANSFER_DST that is an illegal write.
    //    Also, IntercambiarConResuelta requires admite_destino_de_copia, so removing the flag would also
    //    disable the 2 swaps per frame that do pay off.
    //
    // 2. Even if it were possible, it would not help. Fit over 17 race intervals with a constant open
    //    area (5.12 Mtexels): raw ms = 0.0342 x thousands of triangles + 0.023, r2 = 0.982. The
    //    intercept is 0.037 real ms: with both maps open and zero triangles the pass costs nothing.
    //    The whole pass is geometry, and ZCULL discards fragments. The same fit against draws gives
    //    r2 = 0.404. ZCULL ceiling here: 0.037 ms; with the most generous bound (the scale test: -44 %
    //    of area gave -9 % of time), 0.1-0.25 ms. Against the 2.36 ms of the swap that would be lost:
    //    a net loss of 10 to 1.
    //
    // 3. ZCULL for shadows only, without touching the scene depth: in NVK (nvk_cmd_draw.c,
    //    `use_zcull`) a pass opened with loadOp = LOAD_OP_CLEAR enables ZCULL even when the image has
    //    no plane. It is ephemeral, without LOAD_ZCULL/STORE_ZCULL between passes, which a shadow map
    //    drawn whole every time does not need. The shadow pass opens with LOAD, or with DONT_CARE under
    //    nfsmw_nativo_pase_sombras_sin_load, so it gets no ephemeral ZCULL either. Given point 2, that
    //    does not pay off while the pass is pure geometry.
    // =========================================================================================
    const bool zcull = REXCVAR_GET(nfsmw_nativo_zcull);
    const bool is_shadows_map = (pitch == kShadowsSide && height_px == kShadowsSide);
    const bool con_transfer_dst = is_shadows_map || !zcull;
    VkImageUsageFlags depth_use = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                        VK_IMAGE_USAGE_SAMPLED_BIT;
    if (con_transfer_dst) {
      depth_use |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    Image img;
    // Source of the depth copies (CopiarProfundidad): TRANSFER_SRC is required.
    // SAMPLED: with nfsmw_nativo_resolver_sin_copia this image can end up being the resolved texture.
    if (!Make(img, image_width, image_height, depth_use, depth_format_)) {
      Reject(12, "no se pudo crear un destino de profundidad");
      return nullptr;
    }
    img.guest_width = guest_width;
    img.guest_height = guest_height;
    img.supports_copy_target = con_transfer_dst;
    REXLOG_INFO("[nativo] C2: destino de profundidad base {:03X}, formato {}, {}x{}", base,
                format_id, pitch, height_px);
    return &depths_.emplace(key, img).first->second;
  }

  // AirborneNX: the resolved texture whose memory holds `direccion` past its first row, with the same
  // width and host format, where a copy of `alto` rows starting there fits. In a tiled 2D texture rows go in
  // 32-row macro tiles of pitch (aligned to 32) texels, so the offset of row y (a multiple of 32) is
  // y * pitch * bytes per texel. Returns the texture's base and that row.
  bool FindContainingResolved(uint32_t address, uint32_t width_px, uint32_t height_px, uint32_t log2_bytes,
                                 VkFormat host_format, uint32_t& base_out, uint32_t& row_out) const {
    const uint64_t bytes_per_row = uint64_t((width_px + 31) & ~31u) << log2_bytes;
    if (!bytes_per_row) {
      return false;
    }
    for (const auto& [base, r] : resolved_) {
      if (address <= base || r.img.width_px != width_px || r.img.format_id != host_format) {
        continue;
      }
      const uint64_t byte_offset = uint64_t(address) - base;
      if (byte_offset % (bytes_per_row * 32)) {
        continue;
      }
      const uint64_t row = byte_offset / bytes_per_row;
      if (row >= r.img.height_px || row + height_px > uint64_t(r.img.height_px) + 31) {
        continue;
      }
      base_out = base;
      row_out = uint32_t(row);
      return true;
    }
    return false;
  }

  Resolved* GetResolved(uint32_t base, uint32_t width_px, uint32_t height_px, uint32_t format_id,
                            bool rb_swap, VkFormat host_format = kColorFormat) {
    // AirborneNX: the size the guest samples it with, when it fits in the destination (AnotarTamanoTextura).
    if (const auto t = texture_sizes_.find(base);
        t != texture_sizes_.end() && t->second.first <= width_px && t->second.second <= height_px) {
      width_px = t->second.first;
      height_px = t->second.second;
    }
    if (!width_px || !height_px) {
      Reject(9, "copia a una textura de tamano 0");
      return nullptr;
    }
    auto it = resolved_.find(base);
    if (it != resolved_.end()) {
      if (it->second.img.width_px == width_px && it->second.img.height_px == height_px &&
          it->second.img.format_id == host_format) {
        if (draws_ && (it->second.guest_format != format_id || it->second.rb_swap != rb_swap)) {
          draws_->InvalidateTextures();  // changes the swizzle it is sampled with
        }
        it->second.guest_format = format_id;
        it->second.rb_swap = rb_swap;
        it->second.img.rb_swap = rb_swap;
        return &it->second;
      }
      // Another size or format at the same address: the old image may still be in use.
      SendJob(true);
      WaitGpu();
      if (draws_) {
        draws_->ForgetImage(it->second.img.img);
      }
      Destroy(it->second.img);
      resolved_.erase(it);
      // The image at that address no longer exists, so no render target owes it anything.
      if (!borrowed_.empty()) {
        borrowed_.erase(base);
      }
      ShadowMinResolvedWritten(base);  // the new image does not have its content
    }
    Resolved resolved;
    // The attachment usage (color or depth) is needed for nfsmw_nativo_resolver_sin_copia: this image can
    // end up being the render target it is swapped with.
    const VkImageUsageFlags target_use = IsDepthFormat(host_format)
                                              ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                              : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (!Make(resolved.img, width_px, height_px,
               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | target_use,
               host_format)) {
      Reject(10, "no se pudo crear una textura resuelta");
      return nullptr;
    }
    resolved.guest_format = format_id;
    resolved.rb_swap = rb_swap;
    resolved.img.rb_swap = rb_swap;
    REXLOG_INFO("[nativo] C2: textura resuelta en {:08X}, {}x{}, formato {}", base, width_px, height_px,
                format_id);
    if (draws_) {
      draws_->InvalidateTextures();  // that address is now sampled from the resolved texture
    }
    return &resolved_.emplace(base, resolved).first->second;
  }

  // One more copy in its size bucket, to tell whether the copy milliseconds come from pixels or from a
  // fixed cost per copy.
  void TrackCopy(uint32_t width_px, uint32_t height_px) {
    const uint64_t pixels = uint64_t(width_px) * height_px;
    for (uint32_t c = 0; c < kCopyBuckets; ++c) {
      if (pixels <= kBucketPixels[c]) {
        ++bucket_copies_[c];
        bucket_pixels_[c] += pixels;
        return;
      }
    }
  }

  // The setting, or its alternating test.
  bool ResolveWithoutCopy() {
    const int32_t alternar = REXCVAR_GET(nfsmw_nativo_resolver_sin_copia_alternar_s);
    if (alternar <= 0) {
      return REXCVAR_GET(nfsmw_nativo_resolver_sin_copia);
    }
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - toggle_start_)
                              .count();
    const bool without_copy = (secs / alternar) % 2 == 1;
    if (without_copy != tracked_toggle_) {
      tracked_toggle_ = without_copy;
      REXLOG_INFO("[nativo] C2 resolucion: {}", without_copy ? "sin copia (intercambio)" : "copiando");
    }
    return without_copy;
  }

  static bool IsDepthFormat(VkFormat format_id) {
    return format_id == VK_FORMAT_D24_UNORM_S8_UINT || format_id == VK_FORMAT_D32_SFLOAT_S8_UINT ||
           format_id == VK_FORMAT_D16_UNORM || format_id == VK_FORMAT_D32_SFLOAT ||
           format_id == VK_FORMAT_X8_D24_UNORM_PACK32;
  }

  // Resolve without copying. The whole render target becomes the resolved texture and that texture's
  // old image stays as the render target. What the game sees is the same; what is saved is moving the
  // pixels (the two 1600x1600 shadow map copies are 41 MB per frame). false if it is not possible.
  bool SwapWithResolved(Image& target, Resolved& resolved, uint32_t base) {
    if (target.width_px != resolved.img.width_px || target.height_px != resolved.img.height_px ||
        target.format_id != resolved.img.format_id) {
      return false;
    }
    // ZCULL safety net. If the render target was created without TRANSFER_DST (so it can have a ZCULL
    // plane), swapping it would put it in the resolved texture's place, which does receive copies: an
    // illegal write. And the other way round, the resolved texture (with TRANSFER_DST and no plane) would
    // become the depth target and ZCULL would switch off on alternate frames. Fall back to the usual copy
    // path instead.
    if (!target.supports_copy_target) {
      return false;
    }
    // nfsmw_nativo_diag_borrados. The whole image becomes a sampled texture, so all of it counts as used
    // (noted before the images change places).
    TrackClearUse(target, target.width_px, target.height_px);
    // Careful: OlvidarImagen must not be called here. It frees the descriptor slot and reuses it in the
    // same frame, so draws already recorded with it end up reading another image (flicker). Bumping the
    // generation is enough: later draws resolve their view again, and existing views stay valid because
    // they go with their image.
    // nfsmw_nativo_profundidad_perezosa. The render target changes image: first, whatever it has deferred
    // as a source is resolved; and the texture receives the whole target, so its deferred copy is no
    // longer needed.
    if (!pending_.empty()) {
      BeforeDepthWrite(target);
      if (pending_.erase(base)) {
        ++lazy_replaced_;
      }
    }
    // nfsmw_nativo_frontal_perezoso. Same for front buffers: deferred copies whose source is this target
    // are recorded first, and the texture receives the whole target (its deferred copy is unnecessary).
    if (!pending_fronts_.empty()) {
      BeforeColorWrite(target);
      ResolvePreviousFront(base, true);
    }
    std::swap(target.img, resolved.img.img);
    std::swap(target.mem, resolved.img.mem);
    std::swap(target.view, resolved.img.view);
    std::swap(target.prepared, resolved.img.prepared);
    resolved.img.rb_swap = resolved.rb_swap;  // belongs to the content, not the image
    resolved.img.invalid_content = false;
    target.invalid_content = true;  // its content is now in the resolved texture
    target.base_resolved = base;
    ++swap_count_;
    WrittenResolved(base);   // if it was lent, it has valid content again
    ForgetClear(target);  // and the target keeps another image: its clear no longer holds
    if (draws_) {
      // Only what points to these two images: invalidating the whole cache here costs more than the copy
      // it saves (measured on PC: +0.18 ms of scene per frame).
      draws_->InvalidateImages(target.img, resolved.img.img);
    }
    return true;
  }

  // If the render target lost its content in an image swap and the game is about to draw on top, the
  // content is brought back from the resolved texture.
  //
  // In a race this is not rare: it happens once for every swap without a clear (measured: +206/+260/+224
  // swaps and +206/+260/+224 restores in three consecutive intervals). That is 1.66 real ms per frame
  // that no bucket of the copy inventory accounted for. Here they are counted, trimmed to the useful
  // area and, if requested, done without copying a single pixel.
  //
  // para_resolver = requested by a resolve (nfsmw_nativo_resolver_contenido_valido), not by a pass that
  // will draw on top. Then it is only copied: no lending (it would leave the source texture without
  // content just when the car body is about to read it) and no nfsmw_nativo_sombra_minimo clear (that
  // is for the car pass).
  void RestoreContent(Image& target, bool para_resolver = false) {
    if (!target.invalid_content) {
      return;
    }
    target.invalid_content = false;
    // nfsmw_nativo_profundidad_perezosa. The resolved texture is about to be read (if its copy was
    // deferred, it is recorded first) and the render target written (first, whatever it has deferred as
    // a source).
    if (!pending_.empty()) {
      if (pending_.count(target.base_resolved)) {
        RecordPendingCopy(target.base_resolved);
        ++lazy_read_copied_;
      }
      BeforeDepthWrite(target);
    }
    // nfsmw_nativo_frontal_perezoso. Same for front buffers: if the texture about to be read has a
    // deferred copy, it is recorded first; and so are deferred copies whose source is this render target.
    if (!pending_fronts_.empty()) {
      if (pending_fronts_.count(target.base_resolved)) {
        RecordFrontCopy(target.base_resolved);
        ++front_read_copied_;
      }
      BeforeColorWrite(target);
    }
    auto it = resolved_.find(target.base_resolved);
    if (it == resolved_.end() || it->second.img.width_px != target.width_px ||
        it->second.img.height_px != target.height_px || it->second.img.format_id != target.format_id ||
        !copy_image_ || !Record()) {
      if (para_resolver) {
        ++resolve_without_source_;  // unknown where to bring it from; resolve whatever is there
      }
      return;
    }
    if (para_resolver) {
      // nfsmw_nativo_resolver_contenido_valido. Only the copy, with its barriers.
      CopyBackForResolve(target, it->second.img);
      return;
    }
    // The content does not need duplicating, only to be in the render target: swapping the two images
    // back puts it where it belongs without moving a byte. The resolved texture stays lent until the next
    // resolve to that address; 'prestadas leidas' checks that nobody looks at it in the meantime.
    // Both must already be in GENERAL: the swap moves the image, not its layout, and preparing after the
    // swap would put the barrier on the wrong image.
    /*
     * Why the restore does not swap.
     *
     * Restores copy 2.56 Mpixels per frame = 2.00 real ms, and 17 log reports in a row show a saving of
     * 0.00. This path exists precisely to avoid the copy, and the counter says "0 por intercambio": it
     * never activates. It has three conditions, so each one is counted instead of guessing which one
     * fails. Three uint64 increments on a path that already copies 10 MB: zero cost.
     */
    if (REXCVAR_GET(nfsmw_nativo_restaurar_por_intercambio)) {
      if (!target.supports_copy_target) {
        ++no_swap_without_transfer_dst_;
      } else if (!target.prepared) {
        ++no_target_swap_without_prepare_;
      } else if (!it->second.img.prepared) {
        ++no_resolved_swap_without_prepare_;
      } else if (target.width_px != it->second.img.width_px ||
                 target.height_px != it->second.img.height_px) {
        // Not a condition of the if below, but it does matter: the swap moves the whole image, and if the
        // sizes do not match it would leave the render target with the wrong dimensions. If this count is
        // high, the lever cannot work as written.
        ++no_distinct_sizes_swap_;
      }
    }
    if (REXCVAR_GET(nfsmw_nativo_restaurar_por_intercambio) && target.supports_copy_target &&
        target.prepared && it->second.img.prepared) {
      const uint32_t base = target.base_resolved;
      std::swap(target.img, it->second.img.img);
      std::swap(target.mem, it->second.img.mem);
      std::swap(target.view, it->second.img.view);
      std::swap(target.prepared, it->second.img.prepared);
      it->second.img.invalid_content = false;
      if (draws_) {
        draws_->InvalidateImages(target.img, it->second.img.img);
      }
      borrowed_.insert(base);
      ++loans_;
      ++restores_;
      saved_restore_pixels_ += uint64_t(target.width_px) * target.height_px;
      ForgetClear(target);
      return;
    }
    // nfsmw_nativo_sombra_minimo. The car pass of the shadow map: while it is being applied, the render
    // target is cleared to 1.0 instead of copying the world back (the world draws take the minimum).
    if (ShadowMinInsteadOfRestore(target)) {
      return;
    }
    Prepare(it->second.img);
    Prepare(target);
    const VkImageAspectFlags aspect =
        IsDepthFormat(target.format_id) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    // Render targets are created with height = max(720, pitch): the scene measures 1280x1280 to draw
    // 1280x720 and the blur targets 320x720 to draw 320x180. What lies below the useful area is never
    // drawn or resolved, so copying it wastes bandwidth. The useful area is the largest y1 the game has
    // asked to resolve from this target; until there is data, the whole target is copied.
    uint32_t height_px = target.height_px;
    if (REXCVAR_GET(nfsmw_nativo_restaurar_area_util)) {
      const auto e = target_state_.find(&target);
      if (e != target_state_.end() && e->second.used_height &&
          e->second.used_height < target.height_px) {
        height_px = e->second.used_height;
        ++clipped_restores_;
        saved_restore_pixels_ += uint64_t(target.width_px) * (target.height_px - height_px);
      }
    }
    VkImageCopy copy_info{};
    copy_info.srcSubresource = {aspect, 0, 0, 1};
    copy_info.dstSubresource = {aspect, 0, 0, 1};
    copy_info.extent = {target.width_px, height_px, 1};
    TrackClearUse(target, target.width_px, height_px);  // nfsmw_nativo_diag_borrados
    MarkGpu(kGpuCopies);
    copy_image_(job_commands_, it->second.img.img, VK_IMAGE_LAYOUT_GENERAL,
                   target.img, VK_IMAGE_LAYOUT_GENERAL, 1, &copy_info);
    ++restores_;
    restored_pixels_ += uint64_t(target.width_px) * height_px;
    TrackCopy(target.width_px, height_px);  // so the copy inventory adds up to the measured ms
    ForgetClear(target);
  }

  /*
   * nfsmw_nativo_resolver_contenido_valido. The render target gets its content back from the resolved
   * texture where the image swap left it, just before a resolve reads it.
   *
   * The copy goes between two barriers, and here they are needed: the source is the image that was just
   * drawn as the depth target (the previous resolve moved it into the texture by swapping), and what is
   * copied will be sampled as soon as the current resolve swaps it. NVK only waits for the GPU and
   * flushes the texture cache at a barrier (in a race the restore still goes without them, as always:
   * this does not touch it). Each one costs a pipeline drain, twice per frame in the menu. The whole
   * target is copied: that is what the EDRAM held and what the resolve reads.
   */
  void CopyBackForResolve(Image& target, Image& src) {
    Prepare(src);
    Prepare(target);
    const VkImageAspectFlags aspect =
        IsDepthFormat(target.format_id) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    constexpr VkPipelineStageFlags kWrites =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dfn_.vkCmdPipelineBarrier(job_commands_, kWrites, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0,
                              nullptr, 0, nullptr);
    VkImageCopy copy_info{};
    copy_info.srcSubresource = {aspect, 0, 0, 1};
    copy_info.dstSubresource = {aspect, 0, 0, 1};
    copy_info.extent = {target.width_px, target.height_px, 1};
    TrackClearUse(target, target.width_px, target.height_px);  // nfsmw_nativo_diag_borrados, like the restore
    MarkGpu(kGpuCopies);
    copy_image_(job_commands_, src.img, VK_IMAGE_LAYOUT_GENERAL, target.img, VK_IMAGE_LAYOUT_GENERAL,
                   1, &copy_info);
    // And the copied data made visible to what follows: the current resolve (copy or swap), the draws that
    // sample the texture that receives it, and the pass that draws to the render target again.
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dfn_.vkCmdPipelineBarrier(job_commands_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                              0, 1, &barrier, 0, nullptr, 0, nullptr);
    ++restores_for_resolve_;
    restored_pixels_ += uint64_t(target.width_px) * target.height_px;
    TrackCopy(target.width_px, target.height_px);
    ForgetClear(target);
  }

  // --- nfsmw_nativo_sombra_minimo (see the cvar comment) --------------------------------------------------

  // A failure while applying may change the image of this very frame: DIFERENCIA in the log and it
  // switches off for the session. Textures that already hold only the cars keep being served with the
  // minimum until they are written again.
  void ShadowMinDifference(const char* reason, uint32_t datum) {
    ++sm_differences_;
    if (sm_phase_ != kSmDisabled) {
      sm_phase_ = kSmDisabled;
      REXLOG_ERROR("[nativo] C2 sombra por minimo: DIFERENCIA ({}; dato {:08X}). Se apaga para la sesion: el mapa de "
                   "sombras se vuelve a copiar como antes",
                   reason, datum);
    }
  }

  // Something the minimum would not reproduce, seen before any image changed: not applied this session.
  void ShadowMinUnfit(const char* reason, uint32_t datum) {
    if (sm_phase_ != kSmDisabled) {
      sm_phase_ = kSmDisabled;
      REXLOG_WARN("[nativo] C2 sombra por minimo: no se aplica en esta sesion ({}; dato {:08X}); el mapa de sombras se "
                  "sigue copiando como siempre",
                  reason, datum);
    }
  }

  // A failure within the current cycle. If the render target was already cleared (applying) it is a
  // DIFERENCIA; otherwise either the minimum does not work for this game (permanent) or the cycle simply
  // does not count (transient: menus, loading).
  void ShadowMinCycleFailure(const char* reason, uint32_t datum, bool permanent) {
    sm_clean_cycle_ = false;
    if (sm_clear_cycle_) {
      ShadowMinDifference(reason, datum);
    } else if (permanent) {
      ShadowMinUnfit(reason, datum);
    }
  }

  // The setting for the cycle that starts: the cvar, its alternating test and the cheap PCF.
  bool ShadowMinEnabled() {
    if (!REXCVAR_GET(nfsmw_nativo_sombra_minimo) || !REXCVAR_GET(nfsmw_nativo_pcf_barato)) {
      return false;
    }
    const int32_t alternar = REXCVAR_GET(nfsmw_nativo_sombra_minimo_alternar_s);
    if (alternar <= 0 || sm_phase_ != kSmApplying) {
      return true;
    }
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                                           sm_toggle_start_)
                              .count();
    const bool with_min = (secs / alternar) % 2 == 0;
    if (with_min != sm_tracked_segment_) {
      sm_tracked_segment_ = with_min;
      REXLOG_INFO("[nativo] C2 sombra por minimo (alternancia): {}", with_min ? "con el minimo" : "copiando");
    }
    return with_min;
  }

  // After resolving a whole depth target by swapping images (CopiarProfundidad). Without a clear it
  // starts a cycle (the texture receives the world); with a clear, after the car pass, it ends it (the
  // texture receives the cars).
  void ShadowMinAfterSwap(const Image& target, uint32_t base, bool will_clear) {
    if (target.width_px != kShadowsSide || target.height_px != kShadowsSide) {
      return;
    }
    if (!will_clear) {
      if (sm_state_ == kSmCars && sm_clear_cycle_) {
        ShadowMinDifference("el destino con los coches solos se resuelve sin borrar", base);
      }
      if (sm_state_ != kSmFree) {
        sm_consecutive_clean_ = 0;  // the previous cycle did not close: count from scratch
      }
      sm_state_ = kSmAfterWorld;
      sm_cycle_target_ = &target;
      sm_cycle_world_ = base;
      sm_clean_cycle_ = true;
      sm_clear_cycle_ = false;
      sm_rewritten_world_ = false;
      sm_enabled_ = ShadowMinEnabled();
      return;
    }
    if (sm_state_ != kSmCars || &target != sm_cycle_target_) {
      if (sm_state_ != kSmFree) {
        sm_consecutive_clean_ = 0;
      }
      sm_state_ = kSmFree;
      return;
    }
    sm_state_ = kSmFree;
    ++sm_cycles_;
    if (sm_clear_cycle_) {
      // The texture has just received only the cars: from here on it is sampled paired with the world.
      if (base != sm_cars_) {
        ShadowMinDifference("la resolucion de los coches va a otra textura", base);
        sm_cars_ = base;  // it holds only the cars: serve that one
      }
      sm_virtual_ = true;
      sm_valid_virtual_ = !sm_rewritten_world_;
      if (sm_rewritten_world_) {
        ShadowMinDifference("la textura del mundo se volvio a escribir dentro del ciclo", sm_world_);
      }
      ++sm_applied_cycles_;
      return;
    }
    // Observing (or applying with the setting off in this cycle): the texture is the usual one, with the
    // world and the cars.
    if (base != sm_cars_ || sm_cycle_world_ != sm_world_ || sm_cycle_target_ != sm_target_) {
      if (sm_phase_ == kSmApplying) {
        // A cycle different from the learned one (split screen, another mode): it cannot be served.
        // Stop applying.
        ShadowMinUnfit("aparece un ciclo de mapa de sombras distinto del aprendido (el dato es su textura)", base);
        return;
      }
      sm_cars_ = base;  // learned (or changed): count again from zero
      sm_world_ = sm_cycle_world_;
      sm_target_ = sm_cycle_target_;
      sm_consecutive_clean_ = 0;
    }
    sm_consecutive_clean_ = (sm_clean_cycle_ && !sm_rewritten_world_) ? sm_consecutive_clean_ + 1 : 0;
    if (sm_phase_ == kSmPeeking && sm_enabled_ && sm_consecutive_clean_ >= kShadowMinCycles) {
      sm_phase_ = kSmApplying;
      REXLOG_INFO("[nativo] C2 sombra por minimo: APLICANDO tras {} ciclos limpios seguidos (destino {}x{}, mundo en "
                  "{:08X}, coches en {:08X}): el pase de los coches se dibuja sobre el destino borrado y no se copia",
                  sm_consecutive_clean_, target.width_px, target.height_px, sm_world_, sm_cars_);
    }
  }

  // From RestaurarContenido: the render target lost its content in a swap and is about to be drawn on.
  // If this is the car pass of the current cycle it is noted, and while applying, the target is cleared
  // to 1.0 instead of bringing the world back (true: no copy needed).
  bool ShadowMinInsteadOfRestore(Image& target) {
    if (&target != sm_cycle_target_ || sm_state_ != kSmAfterWorld) {
      if (&target == sm_cycle_target_ && sm_state_ == kSmCars) {
        ShadowMinCycleFailure("el destino de los coches se restaura otra vez", target.base_resolved, false);
      }
      return false;
    }
    if (target.base_resolved != sm_cycle_world_) {
      ShadowMinCycleFailure("el destino se restaura desde otra textura", target.base_resolved, false);
      return false;
    }
    sm_state_ = kSmCars;
    // Only with the learned cycle: the same render target and the same world texture as in the observing
    // phase.
    if (sm_phase_ != kSmApplying || !sm_enabled_ || &target != sm_target_ || sm_cycle_world_ != sm_world_ ||
        !sm_cars_ || !clear_depth_ || !target.supports_copy_target) {
      return false;
    }
    Prepare(target);
    MarkGpu(kGpuClears);
    const VkClearDepthStencilValue value{1.0f, 0};
    clear_depth_(job_commands_, target.img, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &kDepthRange);
    TrackClearUse(target, target.width_px, target.height_px);  // nfsmw_nativo_diag_borrados: like the restore
    ForgetClear(target);  // not a game clear: the game's next clear cannot be skipped
    sm_clear_cycle_ = true;
    sm_saved_pixels_ += uint64_t(target.width_px) * target.height_px;
    return true;
  }

  // A copy (not a swap) from a depth target: if it is the current cycle's target, something reads its
  // content mid-cycle. With the target already cleared, what gets copied is only the cars.
  void ShadowMinCopyFrom(const Image& src) {
    if (&src == sm_cycle_target_ && sm_state_ != kSmFree) {
      ShadowMinCycleFailure("se copia del destino del mapa de sombras a mitad del ciclo", 0, false);
    }
  }

  // The game clears the current cycle's target without the second resolve: its content is lost the same
  // way on both paths, but in this cycle the texture with the cars is not renewed while the world one
  // is. The cycle does not count.
  void ShadowMinBeforeClear(const Image& target) {
    if (&target == sm_cycle_target_ && sm_state_ != kSmFree) {
      sm_consecutive_clean_ = 0;
      sm_state_ = kSmFree;
    }
  }

  // A resolved texture receives new content (ResueltaEscrita) or is recreated (ObtenerResuelta).
  void ShadowMinResolvedWritten(uint32_t base) {
    if (sm_virtual_) {
      if (base == sm_cars_) {
        sm_virtual_ = false;  // new content: no longer only the cars
      } else if (base == sm_world_) {
        sm_valid_virtual_ = false;  // its pair changes: reading it now would not give its cycle's minimum
      }
    }
    if (sm_state_ != kSmFree && base == sm_cycle_world_) {
      sm_rewritten_world_ = true;
    }
  }

  // C2 report line (every 10 s, with the other copy lines). The ms use 0.60 per Mpixel copied and 0.075
  // per Mpixel cleared.
  void ShadowMinReport() {
    if (!sm_cycles_ && !sm_differences_ && !sm_unable_reads_ && !sm_reads_to_untimed_) {
      return;
    }
    const double frames = double(presented_ - copies_report_presented_);
    const double porFot = frames > 0.0 ? 1.0 / frames : 0.0;
    const double mp = double(sm_saved_pixels_ - sm_saved_pixels_previous_) / 1e6 * porFot;
    static constexpr const char* kPhases[3] = {"mirando", "APLICANDO", "apagada"};
    NFSMW_RING_REPORT(
        "[nativo] C2 sombra por minimo (build 184): fase {}; {} ciclos desde el arranque ({} limpios seguidos, {} "
        "aplicados); {:.2f} Mpixeles por fotograma borrados en vez de copiados (~{:.2f} ms reales de copia menos y ~{:.2f} "
        "de borrado mas); dibujos de los coches {} ({} no exactos); lecturas de {:08X} desde el arranque: {} con el mundo "
        "de pareja, {} consigo misma, {} sin minimo, {} entre las dos resoluciones, {} sin tfetch2DSombraMin; "
        "DIFERENCIAS {}{}",
        kPhases[sm_phase_ < 3 ? sm_phase_ : 2], sm_cycles_, sm_consecutive_clean_, sm_applied_cycles_, mp, mp * 0.60,
        mp * 0.075, sm_cars_draws_, sm_exact_no_draws_, sm_cars_, sm_reads_min_, sm_reads_if_same_,
        sm_normal_reads_, sm_reads_to_untimed_, sm_unable_reads_, sm_differences_,
        sm_differences_ ? " *** LA IMAGEN PUEDE HABER CAMBIADO: ver el ERROR del log ***" : " (0 = la imagen es la misma)");
    sm_saved_pixels_previous_ = sm_saved_pixels_;
  }

  // The resolved texture has just received new content, so if it was lent it no longer is. An empty set
  // (the normal case) touches nothing. `pixeles` = the pixels actually copied (0 if the resolve swapped
  // images), so that the per-target inventory shows where the traffic goes.
  void WrittenResolved(uint32_t base, uint64_t pixels = 0) {
    ShadowMinResolvedWritten(base);  // nfsmw_nativo_sombra_minimo
    if (!borrowed_.empty()) {
      borrowed_.erase(base);
    }
    if (!expired_.empty()) {
      expired_.erase(base);  // New contents: no dropped copy is missing any more
    }
    if (pixels) {
      const auto c = copies_per_target_.find(base);
      if (c != copies_per_target_.end()) {
        c->second.pixels += pixels;
      }
    }
  }

  // The render target's content is no longer that of its last clear (something was copied over it or
  // it changed image). The game's next clear cannot be skipped.
  //
  // It also answers the other question about clears: if the target's content changes without anything
  // drawn since it was cleared, that clear was wiped out by a swap or a restore and served no purpose.
  // It is the equivalent of a loadOp = CLEAR that gets thrown away: these are DONT_CARE candidates.
  void ForgetClear(const Image& target) {
    const auto e = target_state_.find(&target);
    if (e == target_state_.end()) {
      return;
    }
    if (e->second.clean_clear &&
        e->second.draws_on_clear == (draws_ ? draws_->Drawn() : 0)) {
      ++useless_clears_;
      useless_clears_pixels_ += uint64_t(target.width_px) * target.height_px;
    }
    e->second.clean_clear = false;
  }

  // The largest rectangle the game resolves from this render target: the area it actually uses.
  void TrackUsableArea(const Image& target, int32_t y1) {
    if (y1 > 0) {
      auto& e = target_state_[&target];
      e.used_height = std::max(e.used_height, std::min(uint32_t(y1), target.height_px));
    }
  }

  // A clear that does not change a single bit. It is skipped if the render target is already cleared
  // to that same value and nothing has been drawn, anywhere, since then. Returns true if it must be
  // skipped.
  bool RedundantClear(const Image& target, uint64_t value) {
    auto& e = target_state_[&target];
    const uint64_t drawn = draws_ ? draws_->Drawn() : 0;
    if (REXCVAR_GET(nfsmw_nativo_saltar_borrados_repetidos) && e.clean_clear &&
        e.clear_value == value && e.draws_on_clear == drawn) {
      skipped_clears_pixels_ += uint64_t(target.width_px) * target.height_px;
      return true;
    }
    e.clean_clear = true;
    e.clear_value = value;
    e.draws_on_clear = drawn;
    return false;
  }

  bool Make(Image& img, uint32_t width_px, uint32_t height_px, VkImageUsageFlags use,
             VkFormat format_id = kColorFormat) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format_id;
    info.extent = {width_px, height_px, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = use;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!rex::ui::vulkan::util::CreateDedicatedAllocationImage(
            vk_device_, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, img.img,
            img.mem)) {
      // The GPU can run out of memory here too. Render targets are few and large, so one that does not fit
      // shows up at once: the texture cache is asked to release memory and the allocation is retried once.
      if (!draws_ || !draws_->ReleaseTexturesOnMemoryShortage() ||
          !rex::ui::vulkan::util::CreateDedicatedAllocationImage(
              vk_device_, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, img.img,
              img.mem)) {
        REXLOG_ERROR("[nativo] C2: sin memoria en la GPU para un destino de {}x{} (formato {}) y soltar la cache "
                     "no ha bastado",
                     width_px, height_px, uint32_t(format_id));
        return false;
      }
    }
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = img.img;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format_id;
    view_info.subresourceRange = IsDepthFormat(format_id) ? kDepthRange : kColorRange;
    if (dfn_.vkCreateImageView(device_, &view_info, nullptr, &img.view) != VK_SUCCESS) {
      Destroy(img);
      return false;
    }
    img.width_px = width_px;
    img.height_px = height_px;
    img.format_id = format_id;
    img.prepared = false;
    return true;
  }

  void Destroy(Image& img) {
    CompositeOnDestroy(img);  // nfsmw_nativo_compuesta_perezosa
    // nfsmw_nativo_frontal_perezoso. A deferred copy cannot keep a destroyed image. If the texture is
    // what gets destroyed (ObtenerResuelta recreates it with another size or format), its content is lost
    // just as before: the copy is simply dropped. If it is the source (only at shutdown: render targets
    // are not destroyed, and whatever leaves a target through a swap is recorded first), the texture lacks
    // that copy: it becomes stale and, if something requests it before another full resolve, the guard
    // trips.
    if (!pending_fronts_.empty() && img.img != VK_NULL_HANDLE) {
      for (auto it = pending_fronts_.begin(); it != pending_fronts_.end();) {
        if (it->second.vk_texture == img.img) {
          FreeRetained(it->second);
          it = pending_fronts_.erase(it);
        } else if (it->second.vk_source == img.img) {
          expired_fronts_.insert(it->first);
          FreeRetained(it->second);
          it = pending_fronts_.erase(it);
        } else {
          ++it;
        }
      }
    }
    // nfsmw_nativo_profundidad_perezosa. Without the image there is no copy to do: its texture becomes
    // stale (if something samples it before the next resolve, the guard trips).
    if (!pending_.empty() && img.img != VK_NULL_HANDLE) {
      for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second.vk_source == img.img || it->second.vk_target == img.img) {
          expired_.insert(it->first);
          it = pending_.erase(it);
        } else {
          ++it;
        }
      }
    }
    // The DibujosVulkan framebuffers that use this view are destroyed before it: Vulkan may give the same
    // handle to a new view and FramebufferDe would return a stale one (see
    // nfsmw_nativo_framebuffers_olvidan_vistas). At shutdown, dibujos_ is already gone and so are its
    // framebuffers.
    if (draws_ && img.view != VK_NULL_HANDLE) {
      draws_->ForgetView(img.view);
    }
    if (img.view != VK_NULL_HANDLE) dfn_.vkDestroyImageView(device_, img.view, nullptr);
    if (img.img != VK_NULL_HANDLE) dfn_.vkDestroyImage(device_, img.img, nullptr);
    if (img.mem != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, img.mem, nullptr);
    img = Image{};
  }

  // First time: to GENERAL (valid for copy, clear and sampling) and cleared to zero.
  void Prepare(Image& img) {
    if (img.prepared) {
      return;
    }
    // In the upload command buffer, which runs before the work one (the rear-view mirror cubemap copies
    // its resolved faces there and, on its first frame, read them unprepared).
    const VkCommandBuffer commands = UploadCommands();
    if (commands == VK_NULL_HANDLE) {
      return;
    }
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    const bool depth_value = img.format_id == VK_FORMAT_D24_UNORM_S8_UINT ||
                             img.format_id == VK_FORMAT_D32_SFLOAT_S8_UINT;
    barrier.image = img.img;
    barrier.subresourceRange = depth_value ? kDepthRange : kColorRange;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    dfn_.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                              &barrier);
    if (depth_value) {
      // ZCULL: images created without TRANSFER_DST (the ones eligible for a ZCULL plane) cannot be
      // cleared with vkCmdClearDepthStencilImage. They are cleared by opening a pass with
      // loadOp = CLEAR. The barrier above (UNDEFINED -> GENERAL) stays as it is: it is what makes the
      // driver zero the ZCULL plane, and without it the hardware kills the context.
      if (!img.supports_copy_target && draws_) {
        if (!draws_->ClearDepthInPass(commands, img, 1.0f, 0) &&
            ++clears_in_failed_pass_ <= 8) {
          REXLOG_WARN("[nativo] C2: NO se pudo preparar la profundidad {}x{} abriendo un pase de "
                      "borrado (fallo {})",
                      img.width_px, img.height_px, clears_in_failed_pass_);
        }
      } else if (clear_depth_) {
        const VkClearDepthStencilValue far_clear{1.0f, 0};
        clear_depth_(commands, img.img, VK_IMAGE_LAYOUT_GENERAL, &far_clear, 1,
                            &kDepthRange);
      }
    } else {
      const VkClearColorValue zero{};
      dfn_.vkCmdClearColorImage(commands, img.img, VK_IMAGE_LAYOUT_GENERAL, &zero,
                                1, &kColorRange);
    }
    img.prepared = true;
    if (draws_) {
      draws_->InvalidateTextures();  // TexturaResuelta only returns prepared ones
    }
  }

  bool Record() {
    if (recording_) {
      return true;
    }
    const auto before_record = std::chrono::steady_clock::now();
    const bool recording = StartRecording();
    record_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - before_record).count());
    ++recordings_;
    return recording;
  }

  bool StartRecording() {
    // Slots rotate: while one frame is recorded, the previous ones can still be on the GPU. It only waits
    // if this slot's last submission has not finished yet, so the more slots there are, the further ahead
    // the CPU can get before it has to stop.
    ranura_ = (ranura_ + 1) % used_slots_;
    JobSlot& ranura = ranuras_[ranura_];
    Complete(ranura);
    const auto before_pools = std::chrono::steady_clock::now();
    dfn_.vkResetCommandPool(device_, ranura.job_pool, 0);
    dfn_.vkResetCommandPool(device_, ranura.upload_pool, 0);
    pools_restart_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - before_pools).count());
    job_commands_ = ranura.job;
    upload_commands_ = ranura.upload;
    upload_recording_ = false;
    VkCommandBufferBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    start.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dfn_.vkBeginCommandBuffer(job_commands_, &start) != VK_SUCCESS) {
      return false;
    }
    ranura.categories.clear();
    if (queries_ != VK_NULL_HANDLE) {
      dfn_.vkCmdResetQueryPool(job_commands_, queries_, ranura_ * kMarksPerSlot,
                               kMarksPerSlot);
    }
    if (oclusiones_ != VK_NULL_HANDLE) {
      DiscardOcclusions(ranura);  // Completar already read the last submission's; these were never submitted
      dfn_.vkCmdResetQueryPool(job_commands_, oclusiones_, ranura_ * kOclusionesPorRanura,
                               kOclusionesPorRanura);
    }
    // nfsmw_reflejo_visibilidad. Any left over belong to work that was never submitted: without a
    // measurement, they count as visible. The reset always happens when there is a pool, even if the
    // guard trips mid-frame.
    if (visibility_ != VK_NULL_HANDLE) {
      if (!ranura.visibility.empty()) {
        nfsmw::demand_reflection::TrackVisible(false);
        ranura.visibility.clear();
      }
      dfn_.vkCmdResetQueryPool(job_commands_, visibility_, ranura_ * kVisibilityPerSlot,
                               kVisibilityPerSlot);
    }
    /*
     * Do not reset pools that this frame will not use.
     *
     * This used to happen always, with any toml: 64 statistics queries + 2048 per-draw statistics
     * queries = 2,144 resets per frame. And in our NVK on Tegra the bulk path is excluded for the layout
     * of these pools (nvk_query_pool.c:351: everything that is not a timestamp falls into
     * ALIGNED_INTERLEAVED), so each reset is a 5-dword SET_REPORT_SEMAPHORE with
     * RELEASE_AFTER_ALL_PRECEEDING_WRITES_COMPLETE and PIPELINE_LOCATION_ALL: 2,144 chained writes that
     * wait on each other, 42.9 KB of command stream, at the start of every frame.
     *
     * And they are almost never used: the per-pass statistics are effectively disabled (see the `> 0`
     * check in EmpezarEstadisticas: with estadisticas_por_dibujo_s set, the per-pass query is never
     * opened), and the per-draw ones are only used one frame every 20 seconds.
     *
     * The reset comes before the first MarcarGpu, so its cost does not show up in any category of the
     * breakdown: it is swallowed by the "hueco entre trabajos" (gap between submissions). Estimated at
     * 0.3-1.1 real ms per frame; the measured upper bound (the minimum gap over a whole session) is
     * 2.81 ms.
     *
     * With both off, 32 resets remain instead of 2,144: -98.5 %. The CPU vectors are cleared anyway,
     * which costs nothing.
     */
    ranura.stats.clear();  // those never submitted are not read
    ranura.draw_stats.clear();
    if (stats_ != VK_NULL_HANDLE && REXCVAR_GET(nfsmw_nativo_estadisticas_pipeline) &&
        REXCVAR_GET(nfsmw_nativo_estadisticas_por_dibujo_s) <= 0) {
      dfn_.vkCmdResetQueryPool(job_commands_, stats_,
                               ranura_ * kStatsPerSlot, kStatsPerSlot);
    }
    if (draw_stats_ != VK_NULL_HANDLE && diagnostic_window_) {
      dfn_.vkCmdResetQueryPool(job_commands_, draw_stats_,
                               ranura_ * kDrawStatsPerSlot,
                               kDrawStatsPerSlot);
    }
    // The timestamp mode is decided per whole submission.
    if (toggle_marks_s_ > 0) {
      const int64_t secs = std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::steady_clock::now() - marks_start_)
                                   .count();
      const bool precise = (secs / toggle_marks_s_) % 2 == 1;
      if (precise != precise_marks_) {
        precise_marks_ = precise;
        REXLOG_INFO("[nativo] prueba de marcas: {} (trabajo {})", precise ? "precisas" : "normales",
                    commands_generation_);
      }
    }
    ranura.precise_marks = precise_marks_;
    recording_ = true;
    MarkGpu(kGpuOther);
    ++commands_generation_;
    if (draws_) {
      draws_->UseSlot(ranura_);
    }
    return true;
  }

  // Waits for that slot's submission. Earlier submissions that have already finished are collected
  // first, so read-backs are written in frame order.
  /*
   * This was what prevented CPU and GPU from overlapping.
   *
   * It used to wait as well for every earlier slot still pending, so each frame drained the whole GPU
   * before recording continued. That is why the report always said "trabajos solapados en la GPU 0",
   * why adding a third slot changed nothing (it waited for all of them, however many there were) and
   * why the wait moved elsewhere instead of disappearing.
   *
   * Now it only blocks on the slot about to be reused, the only one whose resources are needed. Earlier
   * ones are collected only if they have already finished (polling without waiting), so their
   * timestamps are read in order when possible; one still running is collected when its turn comes.
   */
  void Complete(JobSlot& ranura) {
    for (JobSlot& another : ranuras_) {
      if (&another != &ranura && another.pending && another.orden < ranura.orden &&
          dfn_.vkGetFenceStatus(device_, another.fence) == VK_SUCCESS) {
        CompleteOne(another);
      }
    }
    CompleteOne(ranura);
  }

  void CompleteOne(JobSlot& ranura) {
    if (!ranura.pending) {
      return;
    }
    const auto before_wait = std::chrono::steady_clock::now();
    dfn_.vkWaitForFences(device_, 1, &ranura.fence, VK_TRUE, UINT64_MAX);
    gpu_waits_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - before_wait)
                                    .count());
    ++gpu_waits_;
    if (ranura.sent.time_since_epoch().count()) {
      gpu_job_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - ranura.sent)
                                      .count());
      ++jobs_gpu_;
    }
    dfn_.vkResetFences(device_, 1, &ranura.fence);
    ranura.pending = false;
    const uint32_t marked_items = uint32_t(ranura.categories.size());
    if (queries_ != VK_NULL_HANDLE && marked_items >= 2) {
      std::array<uint64_t, kMarksPerSlot> marks{};
      const uint32_t index = uint32_t(&ranura - ranuras_.data());
      if (read_queries_(device_, queries_, index * kMarksPerSlot, marked_items,
                          sizeof(uint64_t) * marked_items, marks.data(), sizeof(uint64_t),
                          VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
          marks[marked_items - 1] >= marks[0]) {
        for (uint32_t i = 0; i + 1 < marked_items; ++i) {
          if (marks[i + 1] >= marks[i] && ranura.categories[i] < kGpuCategories) {
            gpu_categories_ns_[ranura.categories[i]] +=
                uint64_t(double(marks[i + 1] - marks[i]) * mark_period_ns_);
          }
        }
        // GPU gap since the end of the previous submission (C2 report). Submissions are read in the order
        // they were sent; if one could not be read, marca_fin_previa_ is 0 and that gap is not counted.
        if (end_mark_previous_ != 0) {
          if (marks[0] >= end_mark_previous_) {
            gpu_categories_ns_[kGpuSlotBetweenJobs] +=
                uint64_t(double(marks[0] - end_mark_previous_) * mark_period_ns_);
          } else {
            ++gpu_overlaps_;
          }
        }
        end_mark_previous_ = marks[marked_items - 1];
        last_gpu_mark_ = marks[marked_items - 1];
        gpu_ns_ += uint64_t(double(marks[marked_items - 1] - marks[0]) * mark_period_ns_);
        ++gpu_jobs_;
        if (ranura.precise_marks) {
          ++gpu_precise_jobs_;
        }
      } else {
        end_mark_previous_ = 0;  // without this submission's timestamps, the next gap is not counted
      }
    } else {
      end_mark_previous_ = 0;
    }
    ranura.categories.clear();
    ReadOcclusions(ranura);
    ReadVisibility(ranura);
    ReadStats(ranura);
    ReadDrawStats(ranura);
    WriteReads(ranura.reads);
  }

  // Per-draw statistics of a finished submission, added to its pixel shader.
  void ReadDrawStats(JobSlot& ranura) {
    if (ranura.draw_stats.empty()) {
      return;
    }
    const uint32_t n = uint32_t(ranura.draw_stats.size());
    std::vector<uint64_t> values(size_t(n) * kStatCounters, 0);
    if (draw_stats_ != VK_NULL_HANDLE && read_queries_ &&
        read_queries_(device_, draw_stats_, ranura.draw_stats.front().first, n,
                        sizeof(uint64_t) * values.size(), values.data(),
                        sizeof(uint64_t) * kStatCounters,
                        VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      for (uint32_t i = 0; i < n; ++i) {
        const uint16_t label = ranura.draw_stats[i].second;
        fragments_per_shader_[label] += values[i * kStatCounters + 2];
        ++draws_per_shader_[label];
      }
      // A diagnostic frame may span several submissions: the first one of each window counts.
      if (!read_window_) {
        read_window_ = true;
        ++diagnostic_frames_;
      }
    }
    ranura.draw_stats.clear();
  }

  // Pass statistics of a finished submission, added to their category.
  void ReadStats(JobSlot& ranura) {
    if (ranura.stats.empty()) {
      return;
    }
    const uint32_t n = uint32_t(ranura.stats.size());
    std::array<uint64_t, kStatsPerSlot * kStatCounters> values{};
    if (stats_ != VK_NULL_HANDLE && read_queries_ &&
        read_queries_(device_, stats_, ranura.stats.front().first, n,
                        sizeof(uint64_t) * kStatCounters * n, values.data(),
                        sizeof(uint64_t) * kStatCounters,
                        VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      for (uint32_t i = 0; i < n; ++i) {
        const uint8_t c = ranura.stats[i].second;
        if (c < kGpuCategories) {
          category_vertices_[c] += values[i * kStatCounters + 0];
          category_primitives_[c] += values[i * kStatCounters + 1];
          category_fragments_[c] += values[i * kStatCounters + 2];
        }
      }
    }
    ranura.stats.clear();
  }

  // Occlusion queries of a finished submission. They add to their game query, which is published once
  // it has its Issue(END) and all its spans have been read.
  void ReadOcclusions(JobSlot& ranura) {
    if (ranura.oclusiones.empty()) {
      return;
    }
    const uint32_t n = uint32_t(ranura.oclusiones.size());
    std::array<uint64_t, kOclusionesPorRanura> counts{};
    const bool read_count = oclusiones_ != VK_NULL_HANDLE && read_queries_ &&
                        read_queries_(device_, oclusiones_, ranura.oclusiones.front().first, n,
                                        sizeof(uint64_t) * n, counts.data(), sizeof(uint64_t),
                                        VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
    for (uint32_t i = 0; i < n; ++i) {
      auto it = game_occlusions_.find(ranura.oclusiones[i].second);
      if (it == game_occlusions_.end()) {
        continue;
      }
      if (read_count) {
        it->second.sample_count += counts[i];
      } else {
        it->second.failed = true;
      }
      if (it->second.pending_segments) {
        --it->second.pending_segments;
      }
      if (it->second.finished && it->second.pending_segments == 0) {
        PublishOcclusion(it);
      }
    }
    ranura.oclusiones.clear();
  }

  // nfsmw_reflejo_visibilidad. Our own queries of a finished submission: for each water draw, whether
  // it left any sample; the witness must always leave some. If they cannot be read, the water counts as
  // visible and the witness counts neither for nor against.
  void ReadVisibility(JobSlot& ranura) {
    if (ranura.visibility.empty()) {
      return;
    }
    const uint32_t n = uint32_t(ranura.visibility.size());
    std::array<uint64_t, kVisibilityPerSlot> counts{};
    const bool read_count = visibility_ != VK_NULL_HANDLE && read_queries_ &&
                        read_queries_(device_, visibility_, ranura.visibility.front().first, n,
                                        sizeof(uint64_t) * n, counts.data(), sizeof(uint64_t),
                                        VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
    for (uint32_t i = 0; i < n; ++i) {
      if (ranura.visibility[i].second == kSentinelVisibility) {
        if (read_count) {
          nfsmw::demand_reflection::TrackSentinel(counts[i] != 0);
        }
      } else if (!read_count) {
        nfsmw::demand_reflection::TrackVisible(false);
      } else if (counts[i] != 0) {
        nfsmw::demand_reflection::TrackVisible(true);
      } else {
        nfsmw::demand_reflection::TrackHidden();
      }
    }
    ranura.visibility.clear();
  }

  // Spans recorded in a submission that was never sent: their game query is not published.
  void DiscardOcclusions(JobSlot& ranura) {
    for (const auto& [index, id] : ranura.oclusiones) {
      auto it = game_occlusions_.find(id);
      if (it != game_occlusions_.end()) {
        it->second.failed = true;
        if (it->second.pending_segments) {
          --it->second.pending_segments;
        }
        if (it->second.finished && it->second.pending_segments == 0) {
          PublishOcclusion(it);
        }
      }
    }
    ranura.oclusiones.clear();
  }

  void PublishOcclusion(std::unordered_map<uint64_t, GameOcclusionQuery>::iterator it) {
    if (!it->second.failed) {
      if (oclusion_por_base_.size() >= 64 && !oclusion_por_base_.count(it->second.base)) {
        oclusion_por_base_.clear();
      }
      oclusion_por_base_[it->second.base] = it->second.sample_count;
      ++occlusion_stats_[2];
      occlusion_stats_[3] += it->second.sample_count;
      occlusion_stats_[4] = std::max(occlusion_stats_[4], it->second.sample_count);
    }
    game_occlusions_.erase(it);
  }

  bool SendJob(bool wait_until) {
    if (recording_) {
      if (draws_) {
        draws_->BeforeSend();  // closes the pass and publishes the upload buffer
      }
      recording_ = false;
      std::array<VkCommandBuffer, 2> buffers{};
      uint32_t n = 0;
      if (upload_recording_) {
        upload_recording_ = false;
        if (dfn_.vkEndCommandBuffer(upload_commands_) != VK_SUCCESS) {
          return false;
        }
        buffers[n++] = upload_commands_;
      }
      if (queries_ != VK_NULL_HANDLE) {
        // Final mark: closes the last span (MarcarGpu leaves room for it).
        JobSlot& marked = ranuras_[ranura_];
        write_mark_(job_commands_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_,
                        ranura_ * kMarksPerSlot + uint32_t(marked.categories.size()));
        marked.categories.push_back(kGpuEnd);
      }
      if (dfn_.vkEndCommandBuffer(job_commands_) != VK_SUCCESS) {
        return false;
      }
      buffers[n++] = job_commands_;
      VkSubmitInfo send{};
      send.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      send.commandBufferCount = n;
      send.pCommandBuffers = buffers.data();
      {
        const auto before_lock = std::chrono::steady_clock::now();  // "C2: presentar" report
        const auto queue_obj = vk_device_->AcquireQueue(family_, 0);
        const auto before_send = std::chrono::steady_clock::now();
        if (dfn_.vkQueueSubmit(queue_obj.queue(), 1, &send, ranuras_[ranura_].fence) != VK_SUCCESS) {
          return false;
        }
        job_lock_ns_ += Ns(before_lock, before_send);
        job_submit_ns_ += Ns(before_send, std::chrono::steady_clock::now());
        ++job_sends_;
      }
      JobSlot& ranura = ranuras_[ranura_];
      ranura.pending = true;
      ranura.orden = ++sends_;
      ranura.sent = std::chrono::steady_clock::now();
      // Read-backs recorded in this submission are written when it finishes.
      ranura.reads.insert(ranura.reads.end(), pending_reads_.begin(),
                             pending_reads_.end());
      pending_reads_.clear();
    }
    if (wait_until) {
      Complete(ranuras_[ranura_]);
    }
    return true;
  }

  void WaitGpu() {
    if (recording_) {
      SendJob(false);
    }
    for (JobSlot& ranura : ranuras_) {
      Complete(ranura);
    }
    WaitOutputs();
  }

  // All pending outputs, before destroying or reusing what they use.
  void WaitOutputs() {
    for (uint32_t i = 0; i < kOutputSlots; ++i) {
      if (pending_outputs_[i]) {
        dfn_.vkWaitForFences(device_, 1, &output_fences_[i], VK_TRUE, UINT64_MAX);
        dfn_.vkResetFences(device_, 1, &output_fences_[i]);
        pending_outputs_[i] = false;
      }
    }
  }

  // Diagnostic (nfsmw_nativo_diag_resueltas): the frame's resolved textures in a 4x4 grid over
  // 1280x720, in the order of their first copy. Cells without a texture stay dark purple.
  bool ComposeTile(uint32_t base_swap) {
    std::vector<uint32_t> bases;
    bases.swap(frame_resolved_);
    if (bases.empty() || !blit_) {
      return false;
    }
    if (tile_.img == VK_NULL_HANDLE &&
        !Make(tile_, 1280, 720, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
      blit_ = nullptr;
      return false;
    }
    if (draws_) {
      draws_->FinishPass();
    }
    if (!Record()) {
      return false;
    }
    Prepare(tile_);
    const VkClearColorValue background{{0.12f, 0.0f, 0.12f, 1.0f}};
    dfn_.vkCmdClearColorImage(job_commands_, tile_.img, VK_IMAGE_LAYOUT_GENERAL, &background,
                              1, &kColorRange);
    std::string item_list;
    for (size_t i = 0; i < bases.size() && i < 16; ++i) {
      const auto it = resolved_.find(bases[i]);
      if (it == resolved_.end() || !it->second.img.prepared ||
          it->second.img.format_id != kColorFormat) {
        continue;  // depth ones do not support blits to color
      }
      const Image& img = it->second.img;
      const int32_t x = int32_t(i % 4) * 320;
      const int32_t y = int32_t(i / 4) * 180;
      VkImageBlit copy_info{};
      copy_info.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy_info.srcOffsets[1] = {int32_t(img.width_px), int32_t(img.height_px), 1};
      copy_info.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy_info.dstOffsets[0] = {x + 2, y + 2, 0};
      copy_info.dstOffsets[1] = {x + 318, y + 178, 1};
      blit_(job_commands_, img.img, VK_IMAGE_LAYOUT_GENERAL, tile_.img,
            VK_IMAGE_LAYOUT_GENERAL, 1, &copy_info, VK_FILTER_LINEAR);
      item_list += fmt::format(" {}{}:{:08X} {}x{}", i, bases[i] == base_swap ? "*" : "", bases[i],
                           img.width_px, img.height_px);
    }
    const auto now_time = std::chrono::steady_clock::now();
    if (now_time - last_tile_warning_ >= std::chrono::seconds(10)) {
      last_tile_warning_ = now_time;
      REXLOG_INFO("[nativo] diag resueltas: {} en el fotograma (* = la del Swap):{}", bases.size(),
                  item_list);
    }
    return true;
  }

  // Rear-view mirror diagnostic: for each resolved texture, how many copies there are and how many had
  // draws since the previous copy (the car's cubemap faces are 256x256). A face resolved without draws
  // copies again what the render target already had.
  // It is no longer limited to square textures up to 512. The copy inventory has to be complete
  // (address, size, Mpixels and reads) because it is the only thing that tells whether a copy is
  // needed. There are ~20 addresses in a race, which fit easily in one line.
  void TrackCopy(uint32_t base, uint32_t width_px, uint32_t height_px, uint64_t draws) {
    if (base == nfsmw::demand_reflection::kAddress) {
      nfsmw::demand_reflection::TrackCopy();  // nfsmw_reflejo_bajo_demanda
    }
    TargetCopy& d = copies_per_target_[base];
    d.width_px = width_px;
    d.height_px = height_px;
    ++d.copies;
    d.draws += draws;
    d.with_draws += draws != 0 ? 1 : 0;
    const auto now_time = std::chrono::steady_clock::now();
    if (now_time - report_copies_ < std::chrono::seconds(10)) {
      return;
    }
    report_copies_ = now_time;
    std::vector<std::pair<uint32_t, TargetCopy>> orden(copies_per_target_.begin(),
                                                         copies_per_target_.end());
    std::sort(orden.begin(), orden.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string item_list;
    uint32_t written_items = 0;
    for (const auto& [address, c] : orden) {
      if (++written_items > 48) {  // defensive cap: ~20 in a race, but a load can touch many more
        item_list += fmt::format(" (y {} direcciones mas)", orden.size() - 48);
        break;
      }
      const auto r = resolved_.find(address);
      const uint64_t reads = r != resolved_.end() ? r->second.reads : 0;
      if (r != resolved_.end()) {
        r->second.reads = 0;
      }
      item_list += fmt::format(" {:08X} {}x{}: {} copias, {} con dibujos ({:.1f} dibujos por copia), "
                           "{:.2f} Mpixeles, {} lecturas{};",
                           address, c.width_px, c.height_px, c.copies, c.with_draws,
                           c.copies ? double(c.draws) / double(c.copies) : 0.0,
                           double(c.pixels) / 1e6, reads,
                           c.copies && !reads ? " *** NADIE LA LEE ***" : "");
    }
    copies_per_target_.clear();
    NFSMW_RING_REPORT("[nativo] C2 caras resueltas desde el informe anterior:{}", item_list);
    if (swap_count_ || restores_) {
      // Restores are the verdict. If they go up, the swap without a clear saves nothing: the same copy is
      // paid, just later.
      NFSMW_RING_REPORT("[nativo] C2 resoluciones sin copia: {} intercambios ({} sin borrado, {} de COLOR) y {} "
                  "restauraciones desde el arranque -> {}",
                  swap_count_, swaps_without_clear_, color_swaps_, restores_,
                  restores_ == 0 ? "ni una restauracion: el ahorro es limpio"
                                       : "*** hay restauraciones: el ahorro NO es limpio ***");
      // nfsmw_nativo_resolver_contenido_valido. In the menu, one per frame with shadows; in a race, 0. The
      // "sin origen" count is resolves of a render target whose content is no longer anywhere (must be
      // 0); the other count is the resolves that, with the setting off, would read the stale image.
      if (restores_for_resolve_ || resolve_without_source_ || resolve_old_content_) {
        NFSMW_RING_REPORT("[nativo] C2 resolver con contenido valido (build 193): {} veces se trajo de vuelta el "
                             "contenido antes de resolver (antes el resolve leia la imagen del fotograma anterior: el "
                             "parpadeo del menu); {} sin origen; {} leidas como la 192 (ajuste apagado)",
                             restores_for_resolve_, resolve_without_source_, resolve_old_content_);
      }
      // And what they cost per frame, which is the only thing that decides. 0.78 real ms per Mpixel
      // copied and 0.075 per Mpixel cleared, measured here.
      const double frames = double(presented_ - copies_report_presented_);
      const double porFot = frames > 0.0 ? 1.0 / frames : 0.0;
      const double mp_rest = double(restored_pixels_ - restored_pixels_previous_) / 1e6;
      const double savings_mp =
          double(saved_restore_pixels_ - saved_restore_pixels_previous_) / 1e6;
      NFSMW_RING_REPORT("[nativo] C2 restauraciones por fotograma: {:.2f} Mpixeles copiados ({:.2f} ms "
                  "reales) y {:.2f} Mpixeles ahorrados ({:.2f} ms); desde el arranque: {} recortadas "
                  "al area util y {} por intercambio (prestadas leidas {}{})",
                  mp_rest * porFot, mp_rest * porFot * 0.78, savings_mp * porFot,
                  savings_mp * porFot * 0.78, clipped_restores_, loans_,
                  read_borrowed_,
                  read_borrowed_ ? " *** EL PRESTAMO NO ES SEGURO ***" : ", limpio");
      // And why the restore does not swap, which is what has to be known to fix it.
      if (no_swap_without_transfer_dst_ || no_target_swap_without_prepare_ ||
          no_resolved_swap_without_prepare_ || no_distinct_sizes_swap_) {
        NFSMW_RING_REPORT(
            "[nativo] C2 restaurar por intercambio, veces que NO se pudo: {} sin TRANSFER_DST, "
            "{} con el destino sin preparar, {} con la resuelta sin preparar; y de las que si "
            "cumplian, {} tenian tamanos distintos (esas romperian la imagen)",
            no_swap_without_transfer_dst_, no_target_swap_without_prepare_,
            no_resolved_swap_without_prepare_, no_distinct_sizes_swap_);
      }
      restored_pixels_previous_ = restored_pixels_;
      saved_restore_pixels_previous_ = saved_restore_pixels_;
    }
    if (skipped_clears_ || depth_skipped_clears_) {
      // Clears that did not change a single bit (nfsmw_nativo_saltar_borrados_repetidos). A clear costs
      // 0.075 real ms per Mpixel: ten times less than a copy, but it adds up.
      const double frames = double(presented_ - copies_report_presented_);
      const double porFot = frames > 0.0 ? 1.0 / frames : 0.0;
      const double mp = double(skipped_clears_pixels_ - skipped_clears_pixels_previous_) / 1e6;
      NFSMW_RING_REPORT("[nativo] C2 borrados saltados: {} de color y {} de profundidad desde el arranque; "
                  "{:.2f} Mpixeles por fotograma sin borrar ({:.2f} ms reales)",
                  skipped_clears_, depth_skipped_clears_, mp * porFot,
                  mp * porFot * 0.075);
      skipped_clears_pixels_previous_ = skipped_clears_pixels_;
    }
    if (useless_clears_) {
      // Clears wiped out by a swap or a restore before anything was drawn. These are the real candidates
      // for removal (the equivalent of a loadOp = DONT_CARE).
      const double frames = double(presented_ - copies_report_presented_);
      const double porFot = frames > 0.0 ? 1.0 / frames : 0.0;
      const double mp = double(useless_clears_pixels_ - useless_clears_pixels_previous_) / 1e6;
      NFSMW_RING_REPORT("[nativo] C2 borrados que no sirvieron para nada (el destino cambio de contenido sin "
                  "que se dibujara nada): {} desde el arranque; {:.2f} Mpixeles por fotograma "
                  "({:.2f} ms reales)",
                  useless_clears_, mp * porFot, mp * porFot * 0.075);
      useless_clears_pixels_previous_ = useless_clears_pixels_;
    }
    if (lazy_deferred_ || lazy_late_reads_) {  // nfsmw_nativo_profundidad_perezosa
      // 0.60 real ms per Mpixel copied: the 1600x1600 shadow map copy raised copies from 1.93 to 3.47 ms.
      const double frames = double(presented_ - copies_report_presented_);
      const double porFot = frames > 0.0 ? 1.0 / frames : 0.0;
      const double mp = double(lazy_saved_pixels_ - lazy_saved_pixels_previous_) / 1e6;
      NFSMW_RING_REPORT("[nativo] C2 profundidad perezosa (build 184): {} resoluciones de profundidad aplazadas "
                           "desde el arranque; {} grabadas al muestrearlas, {} antes de volver a escribir su origen o "
                           "de otro resolve, {} sustituidas por un intercambio y {} tiradas sin copiar (nadie las "
                           "muestreo): {:.2f} Mpixeles por fotograma sin copiar (~{:.2f} ms reales); lecturas "
                           "tardias {}{}",
                           lazy_deferred_, lazy_read_copied_, lazy_write_copied_,
                           lazy_replaced_, lazy_dropped_, mp * porFot, mp * porFot * 0.60,
                           lazy_late_reads_,
                           lazy_disabled_ ? " *** APAGADA POR LA GUARDIA ***" : " (0 = la imagen es la misma)");
      lazy_saved_pixels_previous_ = lazy_saved_pixels_;
    }
    if (front_deferred_ || front_late_reads_) {  // nfsmw_nativo_frontal_perezoso
      const double frames = double(presented_ - copies_report_presented_);
      const double porFot = frames > 0.0 ? 1.0 / frames : 0.0;
      const double mp = double(front_saved_pixels_ - front_saved_pixels_previous_) / 1e6;
      NFSMW_RING_REPORT("[nativo] C2 frontal perezoso (build 184): {} copias a frontales aplazadas desde el arranque; "
                           "Swaps pintados desde el destino {} y desde una imagen retenida {}; {} borrados sobre una "
                           "imagen de repuesto ({} sin repuesto); grabadas: {} al muestrearlas, {} antes de volver a "
                           "escribir el destino u otro resolve, {} en el Swap (FXAA, sin rampa u otra salida); {} tiradas "
                           "sin copiar (otro resolve las tapa enteras): {:.2f} Mpixeles por fotograma sin copiar (~{:.2f} "
                           "ms reales); {} imagenes de repuesto; lecturas tardias {}{}",
                           front_deferred_, front_target_painted_, front_retained_painted_,
                           front_rotations_, front_without_spare_, front_read_copied_,
                           front_write_copied_, front_swap_copied_, front_replaced_, mp * porFot,
                           mp * porFot * 0.60, images_fronts_.size(), front_late_reads_,
                           front_disabled_ ? " *** APAGADO POR LA GUARDIA ***" : " (0 = la imagen es la misma)");
      front_saved_pixels_previous_ = front_saved_pixels_;
    }
    ShadowMinReport();  // nfsmw_nativo_sombra_minimo
    ClearsReport();  // nfsmw_nativo_diag_borrados, every 20 s
    copies_report_presented_ = presented_;
    if (without_swap_[0] || without_swap_[1] || without_swap_[2]) {
      NFSMW_RING_REPORT("[nativo] C2 copias de profundidad sin intercambiar desde el informe anterior: {} porque la "
                  "orden no borra el destino, {} porque no se resuelve entero, {} porque no se pudo",
                  without_swap_[0], without_swap_[1], without_swap_[2]);
      without_swap_ = {};
    }
  }

  // Read-back of a small resolved texture: the copied rectangle goes to a host-visible buffer in the
  // same submission; EscribirLecturas moves it into guest memory.
  void ReadResolved(const CopyRegisters& reg, const Resolved& resolved, int32_t x0, int32_t y0,
                    uint32_t dx, uint32_t dy, uint32_t width_px, uint32_t height_px) {
    const int32_t max_value = REXCVAR_GET(nfsmw_nativo_leer_resueltas_texels);
    if (max_value <= 0 || uint64_t(width_px) * height_px > uint64_t(max_value) ||
        resolved.img.format_id != kColorFormat) {
      return;
    }
    const uint64_t target_key =
        (uint64_t(reg.rb_copy_dest_base) << 28) ^ (uint64_t(width_px) << 14) ^ uint64_t(height_px);
    ReadbackTarget& target = reads_per_target_[target_key];
    target.base = reg.rb_copy_dest_base;
    target.width_px = width_px;
    target.height_px = height_px;
    const uint64_t cada = uint64_t(std::max(REXCVAR_GET(nfsmw_nativo_lecturas_cada), 1));
    if (target.copies++ % cada != 0) {
      ++target.skipped;  // exposure changes slowly: the guest keeps the previous one
      return;
    }
    const VkDeviceSize bytes = VkDeviceSize(width_px) * height_px * 4;
    const uint64_t key = (uint64_t(reg.rb_copy_dest_base) << 32) ^ (uint64_t(uint32_t(x0)) << 16) ^
                           uint64_t(uint32_t(y0));
    // One buffer per slot: the previous submission may still be copying into the other slot's.
    ReadInfo* pointer = &reads_[key ^ (uint64_t(ranura_) << 63)];
    if (pointer->bytes < bytes && pointer->buf != VK_NULL_HANDLE) {
      // Larger: the old buffer may have recorded or submitted copies.
      SendJob(true);
      WaitGpu();
      DestroyRead(*pointer);
      if (!Record()) {
        return;
      }
      pointer = &reads_[key ^ (uint64_t(ranura_) << 63)];
      if (pointer->bytes < bytes && pointer->buf != VK_NULL_HANDLE) {
        DestroyRead(*pointer);  // the GPU has nothing pending any more
      }
    }
    ReadInfo& readback = *pointer;
    if (readback.bytes < bytes) {
      uint32_t type = 0;
      if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
              vk_device_, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              rex::ui::vulkan::util::MemoryPurpose::kReadback, readback.buf, readback.mem, &type)) {
        readback = ReadInfo{};
        Reject(16, "no se pudo crear un bufer de lectura de una textura resuelta");
        return;
      }
      void* mapped = nullptr;
      if (dfn_.vkMapMemory(device_, readback.mem, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        DestroyRead(readback);
        Reject(16, "no se pudo crear un bufer de lectura de una textura resuelta");
        return;
      }
      readback.payload = static_cast<uint8_t*>(mapped);
      readback.bytes = bytes;
      readback.coherente = (vk_device_->memory_types().host_coherent >> type) & 0x1;
    }
    VkBufferImageCopy copy_info{};
    copy_info.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy_info.imageOffset = {int32_t(dx), int32_t(dy), 0};
    copy_info.imageExtent = {width_px, height_px, 1};
    dfn_.vkCmdCopyImageToBuffer(job_commands_, resolved.img.img, VK_IMAGE_LAYOUT_GENERAL,
                                readback.buf, 1, &copy_info);
    pending_reads_.push_back({&readback, reg.rb_copy_dest_base, x0, y0, width_px, height_px,
                                    reg.rb_copy_dest_pitch & 0x3FFF,
                                    (reg.rb_copy_dest_pitch >> 16) & 0x3FFF, reg.rb_copy_dest_info});
    if (done_reads_++ == 0) {
      REXLOG_INFO("[nativo] C2: lectura de vuelta de texturas resueltas de hasta {} texels (la "
                  "primera: {:08X}, {}x{})",
                  max_value, reg.rb_copy_dest_base, width_px, height_px);
    }
  }

  // The submission finished: the submitted read-backs go to guest memory. Loaded with its fetch
  // constant (GpuSwap per word and R8G8B8A8), each texel must give the same channels as the resolved
  // texture on the GPU: (B, G, R, A) with copy_dest_swap and (R, G, B, A) without it.
  void WriteReads(std::vector<PendingReadback>& reads) {
    const auto before_reads = std::chrono::steady_clock::now();
    if (before_reads - reads_report_ >= std::chrono::seconds(10)) {
      std::string item_list;
      for (auto it = reads_per_target_.begin(); it != reads_per_target_.end();) {
        ReadbackTarget& d = it->second;
        if (!d.copies) {
          it = reads_per_target_.erase(it);  // no ha vuelto a aparecer
          continue;
        }
        item_list += fmt::format(" {:08X} {}x{} {}/{};", d.base, d.width_px, d.height_px, d.copies - d.skipped,
                             d.copies);
        d.copies = 0;
        d.skipped = 0;
        ++it;
      }
      if (!item_list.empty()) {
        NFSMW_RING_REPORT("[nativo] C2 lecturas de vuelta por destino (hechas/copias):{}", item_list);
      }
      reads_report_ = before_reads;
    }
    for (const PendingReadback& p : reads) {
      const ReadInfo& readback = *p.readback;
      if (!readback.payload) {
        continue;
      }
      ++written_reads_;
      written_texels_ += uint64_t(p.width_px) * p.height_px;
      if (!readback.coherente) {
        VkMappedMemoryRange range_info{};
        range_info.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range_info.memory = readback.mem;
        range_info.size = VK_WHOLE_SIZE;
        dfn_.vkInvalidateMappedMemoryRanges(device_, 1, &range_info);
      }
      const bool swap_state = (p.info >> 24) & 0x1;
      const uint32_t copy_order = p.info & 0x7;  // Endian128: 0-3 as xenos::Endian
      const auto orden =
          copy_order <= 3 ? static_cast<xenos::Endian>(copy_order) : xenos::Endian::k8in32;
      // Each texel is a permutation of its 4 bytes (R/B swap plus GpuSwap): it is computed once with
      // marked bytes. Addresses come from the tile and the tiling table, and the guest's physical memory
      // is contiguous.
      const uint32_t permutation =
          xenos::GpuSwap(swap_state ? uint32_t(0x03000102) : uint32_t(0x03020100), orden);
      const uint8_t b0 = uint8_t(permutation), b1 = uint8_t(permutation >> 8),
                    b2 = uint8_t(permutation >> 16), b3 = uint8_t(permutation >> 24);
      uint8_t* const phys_mem = mem_->TranslatePhysical(0);
      const uint64_t base = uint64_t(p.base & 0x1FFFFFFF);
      const auto& table = Tile2dTexel4Table();
      const uint64_t tiles_per_row = ((p.pitch + 31) & ~uint32_t(31)) >> 5;
      for (uint32_t j = 0; j < p.height_px; ++j) {
        const uint32_t ty = uint32_t(p.y0) + j;
        if (p.target_height && ty >= p.target_height) {
          break;
        }
        const uint64_t row = base + ((uint64_t(ty >> 5) * tiles_per_row) << 12);
        const uint16_t* const local = table.data() + size_t(ty & 31) * 32;
        const uint8_t* s = readback.payload + size_t(j) * p.width_px * 4;
        for (uint32_t i = 0; i < p.width_px; ++i, s += 4) {
          const uint32_t tx = uint32_t(p.x0) + i;
          if (tx >= p.pitch) {
            break;
          }
          const uint64_t address = row + (uint64_t(tx >> 5) << 12) + local[tx & 31];
          if (address + 4 > 0x20000000) {
            continue;
          }
          const uint8_t t0 = s[b0], t1 = s[b1], t2 = s[b2], t3 = s[b3];
          uint8_t* const d = phys_mem + address;
          d[0] = t0;
          d[1] = t1;
          d[2] = t2;
          d[3] = t3;
        }
      }
    }
    reads.clear();
    reads_write_ns_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - before_reads).count());
  }

  void DestroyRead(ReadInfo& readback) {
    if (readback.mem != VK_NULL_HANDLE) {
      if (readback.payload) {
        dfn_.vkUnmapMemory(device_, readback.mem);
      }
      dfn_.vkFreeMemory(device_, readback.mem, nullptr);
    }
    if (readback.buf != VK_NULL_HANDLE) {
      dfn_.vkDestroyBuffer(device_, readback.buf, nullptr);
    }
    readback = ReadInfo{};
  }

  // desde_destino = the source is a render target's image (or a retained one) with the front buffer in its
  // corner and larger than the output (nfsmw_nativo_frontal_perezoso). Only requested with the exact variant
  // and without FXAA.
  bool PaintOutput(VulkanPresenter::VulkanGuestOutputRefreshContext& contexto, Image& src,
                    uint32_t width_px, uint32_t height_px, bool from_target = false) {
    // With nfsmw_nativo_salida_sin_espera, the next of 3 slots, and it only waits if that one is still pending.
    // Without it, always slot 0: it waits for the previous output.
    const uint32_t s = output_without_wait_ ? (current_output_ + 1) % kOutputSlots : 0;
    if (pending_outputs_[s]) {
      const auto before_wait = std::chrono::steady_clock::now();  // "C2: presentar" report
      dfn_.vkWaitForFences(device_, 1, &output_fences_[s], VK_TRUE, UINT64_MAX);
      output_wait_ns_ += Ns(before_wait, std::chrono::steady_clock::now());
      ++output_waits_;
      dfn_.vkResetFences(device_, 1, &output_fences_[s]);
      pending_outputs_[s] = false;
    }
    current_output_ = s;
    // The menu post-processing settings, if they changed (an atomic counter; the cvars are only read then).
    if (gamma_ramp_) {
      const uint64_t postprocess_version = nfsmw::settings::PostprocessVersion();
      if (postprocess_version != postprocess_version_) {
        postprocess_version_ = postprocess_version;
        postprocess_ = nfsmw::settings::ReadPostprocess();
        RecalculateRamp();
        const auto& p = postprocess_;
        REXLOG_INFO("[nativo] C2: posproceso {}: brillo {:.2f}, contraste {:.2f}, saturacion {:.2f}, vibracion {:.2f}, "
                    "temperatura {:.2f}, gamma {:.2f}, tinte {:.2f}/{:.2f}/{:.2f} al {:.2f}, vineta {:.2f}, lineas "
                    "{:.2f}; {}",
                    p.active ? "encendido" : "apagado", p.brightness, p.contrast, p.saturation, p.rumble,
                    p.temperatura, p.gamma, p.r_tint, p.g_tint, p.b_tint, p.tint, p.vignette, p.lines,
                    grading_ ? "con trabajo por pixel (saturacion, vibracion, vineta o lineas)"
                                : "todo en la tabla, sin trabajo por pixel");
      }
    }
    // The gamma ramp, if it changed since this slot's last output (the GPU is no longer reading it).
    if (gamma_ramp_ && output_ramps_[s].version != ramp_version_) {
      OutputRamp& ramp = output_ramps_[s];
      std::memcpy(ramp.payload, ramp_values_.data(), sizeof(ramp_values_));
      rex::ui::vulkan::util::FlushMappedMemoryRange(vk_device_, ramp.mem, ramp.type);
      ramp.version = ramp_version_;
    }
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    for (const auto& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE && f.version == contexto.image_version()) {
        framebuffer = f.framebuffer;
      }
    }
    if (framebuffer == VK_NULL_HANDLE) {
      auto& f = framebuffers_[next_framebuffer_];
      next_framebuffer_ = (next_framebuffer_ + 1) % framebuffers_.size();
      if (f.framebuffer != VK_NULL_HANDLE) {
        WaitOutputs();  // a pending output could still use this framebuffer
        dfn_.vkDestroyFramebuffer(device_, f.framebuffer, nullptr);
        f.framebuffer = VK_NULL_HANDLE;
      }
      VkImageView view = contexto.image_view();
      VkFramebufferCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      info.renderPass = output_render_pass_;
      info.attachmentCount = 1;
      info.pAttachments = &view;
      info.width = width_px;
      info.height = height_px;
      info.layers = 1;
      if (dfn_.vkCreateFramebuffer(device_, &info, nullptr, &f.framebuffer) != VK_SUCCESS) {
        f.framebuffer = VK_NULL_HANDLE;
        return false;
      }
      f.version = contexto.image_version();
      framebuffer = f.framebuffer;
    }

    VkDescriptorImageInfo image_info{};
    image_info.imageView = src.view;
    image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet write_op{};
    write_op.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write_op.dstSet = output_descriptors_[s];
    write_op.dstBinding = 0;
    write_op.descriptorCount = 1;
    write_op.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write_op.pImageInfo = &image_info;
    dfn_.vkUpdateDescriptorSets(device_, 1, &write_op, 0, nullptr);

    dfn_.vkResetCommandPool(device_, output_pools_[s], 0);
    VkCommandBufferBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    start.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dfn_.vkBeginCommandBuffer(output_commands_[s], &start) != VK_SUCCESS) {
      return false;
    }
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = output_render_pass_;
    pass.framebuffer = framebuffer;
    pass.renderArea.extent = {width_px, height_px};
    if (from_target) {
      // That image was just written as a render target (or copy destination) in the frame's work, which goes in
      // another submission: make the output read it fully written.
      VkMemoryBarrier barrier{};
      barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      dfn_.vkCmdPipelineBarrier(output_commands_[s],
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
    dfn_.vkCmdBeginRenderPass(output_commands_[s], &pass, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0.0f, 0.0f, float(width_px), float(height_px), 0.0f, 1.0f};
    dfn_.vkCmdSetViewport(output_commands_[s], 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, {width_px, height_px}};
    dfn_.vkCmdSetScissor(output_commands_[s], 0, 1, &scissor);
    VkPipeline pipeline = pipeline_;
    if (gamma_ramp_) {
      // With an output the size of the source (the normal case), the exact texel, unfiltered.
      // From a render target, the output has the front buffer's size, not the image's, and has no FXAA
      // (FrontalAlPresentar only requests it that way): texelFetch of the pixel, the same texel the texture
      // would hold.
      const bool exact = from_target || (width_px == src.width_px && height_px == src.height_px);
      const bool fxaa = !from_target && nfsmw::settings::AntialiasingFxaa();
      if (fxaa != tracked_fxaa_) {
        tracked_fxaa_ = fxaa;
        REXLOG_INFO("[nativo] C2: antialiasing en la salida: {}", fxaa ? "FXAA" : "ninguno");
      }
      pipeline = RampPipeline((fxaa ? 4 : 0) + (grading_ ? 2 : 0) + (exact ? 1 : 0));
      if (pipeline == VK_NULL_HANDLE) {
        pipeline = ramp_pipelines_[exact ? 1 : 0];  // requested variant missing: the usual one
      }
      if (!exact && !bilinear_ramp_warned_) {
        bilinear_ramp_warned_ = true;
        REXLOG_INFO("[nativo] C2: salida de {}x{} desde un origen de {}x{}: rampa con muestreo bilineal", width_px,
                    height_px, src.width_px, src.height_px);
      }
    }
    dfn_.vkCmdBindPipeline(output_commands_[s], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    dfn_.vkCmdBindDescriptorSets(output_commands_[s], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 layout_pipeline_, 0, 1, &output_descriptors_[s], 0, nullptr);
    const float rect[4] = {-1.0f, -1.0f, 2.0f, 2.0f};  // x, y, width, height in NDC
    dfn_.vkCmdPushConstants(output_commands_[s], layout_pipeline_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                            sizeof(rect), rect);
    struct {
      int32_t byte_offset[2];
      float inverse[2];
    } bilinear = {{0, 0}, {1.0f / float(width_px), 1.0f / float(height_px)}};
    dfn_.vkCmdPushConstants(output_commands_[s], layout_pipeline_, VK_SHADER_STAGE_FRAGMENT_BIT, 16,
                            sizeof(bilinear), &bilinear);
    dfn_.vkCmdDraw(output_commands_[s], 4, 1, 0, 0);
    dfn_.vkCmdEndRenderPass(output_commands_[s]);
    if (from_target) {
      // And nothing submitted later (the next frame draws and clears that image again) writes it before the
      // output has read it.
      dfn_.vkCmdPipelineBarrier(output_commands_[s], VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                nullptr, 0, nullptr, 0, nullptr);
    }
    if (dfn_.vkEndCommandBuffer(output_commands_[s]) != VK_SUCCESS) {
      return false;
    }
    VkSubmitInfo send{};
    send.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    send.commandBufferCount = 1;
    send.pCommandBuffers = &output_commands_[s];
    {
      const auto before_lock = std::chrono::steady_clock::now();  // "C2: presentar" report
      const auto queue_obj = vk_device_->AcquireQueue(family_, 0);
      const auto before_send = std::chrono::steady_clock::now();
      if (dfn_.vkQueueSubmit(queue_obj.queue(), 1, &send, output_fences_[s]) != VK_SUCCESS) {
        return false;
      }
      output_lock_ns_ += Ns(before_lock, before_send);
      output_submit_ns_ += Ns(before_send, std::chrono::steady_clock::now());
      ++output_sends_;
    }
    pending_outputs_[s] = true;
    contexto.SetIs8bpc(true);
    return true;
  }

  struct Framebuffer {
    uint64_t version = UINT64_MAX;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
  };

  const VulkanDevice* vk_device_;
  const VulkanDevice::Functions& dfn_;
  VkDevice device_;
  rex::memory::Memory* mem_;
  uint32_t family_;

  std::array<VkCommandPool, kOutputSlots> output_pools_{};  // output slots
  CopyImageFn copy_image_ = nullptr;
  uint32_t reductions_ = 0;  // resolves that had to shrink
  // Those of the slot being recorded (Grabar switches them).
  VkCommandBuffer job_commands_ = VK_NULL_HANDLE;
  VkCommandBuffer upload_commands_ = VK_NULL_HANDLE;
  // Three slots by default, up to four. With two, the CPU is only one frame ahead and waits for the GPU inside
  // Grabar() half the time (4,826 ms out of every 10 s on the console). How many are actually used is set by
  // nfsmw_nativo_ranuras_trabajo, so configurations can be compared without another NRO.
  // The fourth: measured by regime, with few draws (open road, GPU-bound) the fence wait inside Grabar() is
  // 4.9-9.8 ms, while with many (alleys, CPU-bound) it is ~0. So the third slot falls short exactly where the
  // GPU is the bottleneck. It fits easily: the log says "monton 0 (GPU): 482 MB usados de 1382 MB
  // presupuestados".
  std::array<JobSlot, 4> ranuras_{};
  uint32_t used_slots_ = 3;  // set by the cvar (nfsmw_nativo_ranuras_trabajo)
  uint32_t ranura_ = 1;  // Grabar starts with slot 0
  uint64_t sends_ = 0;
  std::array<VkCommandBuffer, kOutputSlots> output_commands_{};
  std::array<VkFence, kOutputSlots> output_fences_{};
  bool recording_ = false;
  std::array<bool, kOutputSlots> pending_outputs_{};
  uint32_t current_output_ = 0;
  bool output_without_wait_ = false;
  bool precise_marks_ = false;
  int32_t toggle_marks_s_ = 0;
  std::chrono::steady_clock::time_point marks_start_{};
  bool invalidate_every_copy_ = false;
  bool upload_recording_ = false;
  uint64_t commands_generation_ = 0;
  ClearDepthFn clear_depth_ = nullptr;
  // GPU time of the submissions (timestamps, if the queue supports them).
  using WriteMarkFn = void(VKAPI_PTR*)(VkCommandBuffer, VkPipelineStageFlags, VkQueryPool,
                                          uint32_t);
  using ReadQueriesFn = VkResult(VKAPI_PTR*)(VkDevice, VkQueryPool, uint32_t, uint32_t, size_t,
                                              void*, VkDeviceSize, VkQueryResultFlags);
  WriteMarkFn write_mark_ = nullptr;
  ReadQueriesFn read_queries_ = nullptr;
  VkQueryPool queries_ = VK_NULL_HANDLE;
  // The game's occlusion queries. The open one (0 = none), those waiting for their spans by number, the last
  // complete count of each D3D structure and, accumulated, spans, spans without room, published queries,
  // published samples and the maximum of a single query.
  VkQueryPool oclusiones_ = VK_NULL_HANDLE;
  // Pipeline statistics per pass category.
  VkQueryPool stats_ = VK_NULL_HANDLE;
  std::array<uint64_t, kGpuCategories> category_fragments_{};
  std::array<uint64_t, kGpuCategories> category_vertices_{};
  std::array<uint64_t, kGpuCategories> category_primitives_{};
  uint64_t stats_without_room_ = 0;
  VkQueryPool draw_stats_ = VK_NULL_HANDLE;
  std::vector<uint64_t> fragments_per_shader_;
  std::vector<uint64_t> draws_per_shader_;
  uint64_t diagnostic_frames_ = 0;
  uint64_t draw_stats_without_room_ = 0;
  std::array<uint64_t, 3> without_swap_{};          // no clear, not whole, not possible
  uint32_t warnings_without_swap_ = 0;
  std::array<uint64_t, kCopyBuckets> bucket_copies_{};
  std::array<uint64_t, kCopyBuckets> bucket_pixels_{};
  bool diagnostic_window_ = false;
  bool read_window_ = false;
  std::chrono::steady_clock::time_point last_window_ = std::chrono::steady_clock::now();
  bool precise_occlusion_ = false;
  VkQueryPool visibility_ = VK_NULL_HANDLE;  // nfsmw_reflejo_visibilidad
  uint64_t visibility_without_room_ = 0;
  uint64_t open_occlusion_ = 0;
  uint64_t next_occlusion_ = 1;
  std::unordered_map<uint64_t, GameOcclusionQuery> game_occlusions_;
  std::unordered_map<uint32_t, uint64_t> oclusion_por_base_;
  uint64_t occlusion_stats_[5] = {};
  double mark_period_ns_ = 0.0;
  uint64_t gpu_ns_ = 0;
  uint64_t gpu_jobs_ = 0;
  uint64_t gpu_precise_jobs_ = 0;  // measured with precise intermediate marks
  std::array<uint64_t, kGpuCategories> gpu_categories_ns_{};
  uint64_t end_mark_previous_ = 0;  // end timestamp of the last submission read (gap between submissions)
  uint64_t last_gpu_mark_ = 0;  // same, but kept when a read fails (timestamp scale)
  // Breakdown of Presentar's time (CostePresentar).
  uint64_t output_waits_ = 0, output_wait_ns_ = 0;
  uint64_t job_sends_ = 0, job_lock_ns_ = 0, job_submit_ns_ = 0;
  uint64_t output_sends_ = 0, output_lock_ns_ = 0, output_submit_ns_ = 0;
  uint64_t refreshes_ = 0, before_call_ns_ = 0, in_call_ns_ = 0, after_call_ns_ = 0;
  static uint64_t Ns(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point hasta) {
    return hasta > from ? uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(hasta - from).count()) : 0;
  }
  uint64_t gpu_overlaps_ = 0;
  std::array<uint64_t, kSwapBuckets> swap_buckets_{};
  mutable double worst_swap_ms_ = 0.0;  // worst frame since the previous report
  std::chrono::steady_clock::time_point last_swap_{};
  // Resolved texture grid diagnostic (nfsmw_nativo_diag_resueltas).
  FnBlit blit_ = nullptr;
  // The game's shadow map, and the scale it is being drawn at (0 = not decided yet).
  static constexpr uint32_t kShadowsSide = 1600;
  bool scalable_depth_ = false;
  uint32_t shadows_scale_ = 0;
  uint64_t shadows_uploads_ = 0;
  std::vector<uint32_t> frame_resolved_;
  Image tile_;
  std::chrono::steady_clock::time_point last_tile_warning_{};
  // Read-back of small resolved textures (nfsmw_nativo_leer_resueltas_texels).
  std::unordered_map<uint64_t, ReadInfo> reads_;
  std::unordered_map<uint64_t, ReadbackTarget> reads_per_target_;  // informe C2 y cadencia
  std::chrono::steady_clock::time_point reads_report_{};
  std::vector<PendingReadback> pending_reads_;  // recorded in the current submission
  uint64_t done_reads_ = 0;
  // Rear-view mirror diagnostic (AnotarCopia).
  uint64_t copy_last_drawn_ = 0;
  std::unordered_map<uint32_t, TargetCopy> copies_per_target_;
  std::chrono::steady_clock::time_point report_copies_{};
  VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
  // Parts C3-C6: draws with the library shaders (nullptr without the required capabilities).
  std::unique_ptr<VulkanDrawer> draws_;

  std::unordered_map<uint64_t, Image> targets_;
  std::unordered_map<uint32_t, Resolved> resolved_;
  // AirborneNX: size each resolved texture is sampled with (AnotarTamanoTextura).
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> texture_sizes_;
  // nfsmw_nativo_diag_lectores_s (DiagLectoresAlPresentar). Ring thread only.
  struct DiagReader {
    uint32_t ps = 0;        // PS number in the library (kLectorSwap: the Swap presenting the front buffer)
    uint32_t reg_index = 0;  // sampler (fetch constant)
    uint32_t target = 0;   // EDRAM base of color 0 | pitch << 12: where it draws
    uint32_t times = 0;     // draws
  };
  struct DiagWriteRecord {
    int32_t frame = -1;  // 0 or 1 within the window; -1: the last one before the window
    uint32_t orden = 0;      // n-th write to that address in its frame
    uint32_t src = 0;     // EDRAM base | pitch << 12 of the target being resolved
    uint32_t request_width = 0;
    uint32_t request_height = 0;
    uint32_t width_px = 0;  // the texture's
    uint32_t height_px = 0;
    uint64_t draws = 0;    // draws since the previous copy (any)
    bool depth_value = false;
    bool whole_value = false;  // covers it fully: from (0,0) and at least its size
    uint32_t others = 0;   // readers that do not fit in the list
    std::vector<DiagReader> readers;
  };
  // A write classified as SOBRA in a window, watched in every frame since then.
  struct DiagWatched {
    uint32_t address = 0;
    uint32_t src = 0;          // EDRAM base | pitch << 12 of the watched writes
    bool pending = false;       // the texture's last write comes from that source
    bool written_source = false;  // and the source was drawn to or cleared since then
    uint64_t writes = 0;      // from that source since watching began
    uint64_t before = 0;           // reads before the source is rewritten (a deferred copy covers them)
    uint64_t late = 0;         // reads after the source is written (dropping the copy there would fail)
    uint32_t ps_before = 0;        // PS of the first one of each kind
    uint32_t late_ps = 0;
  };
  static constexpr uint32_t kSwapReader = UINT32_MAX;
  static constexpr size_t kDiagMaxWrites = 32;
  static constexpr size_t kDiagMaxReaders = 12;
  static constexpr size_t kDiagMaxWatched = 4;
  static constexpr uint32_t kDiagRegSurfaceInfo = 0x2000;  // RB_SURFACE_INFO
  static constexpr uint32_t kDiagRegColorInfo = 0x2001;    // RB_COLOR_INFO
  static constexpr uint32_t kDiagRegFetch = 0x4800;        // SHADER_CONSTANT_FETCH_00_0
  static constexpr uint32_t kDiagRegColorMask = 0x2104;    // RB_COLOR_MASK: 4 bits per color target
  static constexpr uint32_t kDiagRegsColorInfo[4] = {0x2001, 0x2003, 0x2004, 0x2005};  // RB_COLOR_INFO, RB_COLORn_INFO
  bool diag_window_ = false;
  uint32_t diag_frame_ = 0;
  uint64_t diag_windows_ = 0;
  uint64_t diag_lost_writes_ = 0;
  std::chrono::steady_clock::time_point diag_readers_last_ = std::chrono::steady_clock::now();
  std::unordered_map<uint32_t, std::vector<DiagWriteRecord>> diag_writes_;
  std::unordered_map<uint32_t, uint64_t> diag_signatures_;  // pattern of the last line written, per address
  std::vector<DiagWatched> diag_watched_;
  // nfsmw_nativo_compuesta_perezosa (AplazarCompuesta). A single deferred copy at a time.
  struct CompositePending {
    uint32_t address = 0;
    uint32_t src = 0;                 // EDRAM base | pitch << 12 of the source target
    Image* target = nullptr;           // that target (destinos_ never erases: the pointer stays valid)
    VkImage vk_source = VK_NULL_HANDLE;  // its image when deferred: if it changes, it can no longer be recorded
    VkImage vk_texture = VK_NULL_HANDLE;
    VkImageCopy copy_info{};
  };
  static constexpr uint64_t kCompositeToPeek = 900;  // watched writes without late reads (~30 s of racing)
  CompositePending composite_;
  bool composite_has_ = false;
  bool composite_disabled_ = false;
  uint32_t composite_expired_ = 0;   // address whose last copy was dropped; 0 = none
  uint32_t composite_applying_ = 0;  // address of the last APLICANDO notice
  uint64_t composite_deferred_ = 0;
  uint64_t composite_read_recorded_ = 0;
  uint64_t composite_other_recorded_ = 0;
  uint64_t composite_source_dropped_ = 0;
  uint64_t composite_replaced_ = 0;
  uint64_t composite_late_reads_ = 0;
  uint64_t composite_differences_ = 0;
  uint64_t composite_saved_pixels_ = 0;
  uint64_t composite_pixels_previous_ = 0;
  uint64_t composite_presented_previous_ = 0;
  std::unordered_map<uint64_t, Image> depths_;

  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptors_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptors_pool_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kOutputSlots> output_descriptors_{};
  VkShaderModule vs_ = VK_NULL_HANDLE;
  VkShaderModule fs_ = VK_NULL_HANDLE;
  VkRenderPass output_render_pass_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  // Output with the game's gamma ramp (nfsmw_nativo_rampa_gamma).
  struct OutputRamp {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    uint8_t* payload = nullptr;
    uint32_t type = 0;
    uint64_t version = UINT64_MAX;  // the rampa_valores_ version the buffer holds
  };
  bool gamma_ramp_ = false;
  std::array<OutputRamp, kOutputSlots> output_ramps_{};
  VkShaderModule fs_ramp_ = VK_NULL_HANDLE;
  // Index 4 * fxaa + 2 * graduacion + exacta: 0 bilinear (like guest_output_bilinear_ps), 1 exact texel
  // (output the same size as the source); +2 with the saturation, vibrance, vignette or scanline
  // post-processing; +4 with FXAA. Those other than 0 and 1 are created on request (PipelineRampa).
  std::array<VkPipeline, 8> ramp_pipelines_{};
  std::array<bool, 8> failed_ramp_pipelines_{};
  bool bilinear_ramp_warned_ = false;
  // The game's ramp in 10 bits (red, green, blue). Until the game loads its own, the identity the SDK starts
  // with (i * 1023 / 255).
  std::array<std::array<uint16_t, 3>, 256> game_ramp_ = [] {
    std::array<std::array<uint16_t, 3>, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      t[i].fill(uint16_t(i * 0x3FF / 0xFF));
    }
    return t;
  }();
  // The output UBO in std140: one vec4 per entry (red, green and blue in 0-1, with the single-channel
  // post-processing applied), followed by the mix (saturation, vibrance, 1 / gamma) and effects (vignette,
  // scanlines). Filled by RecalcularRampa.
  static constexpr size_t kRampFloats = 256 * 4 + 8;
  std::array<float, kRampFloats> ramp_values_{};
  uint64_t ramp_version_ = 0;
  // Post-processing in effect and the settings version it was computed from.
  nfsmw::settings::Postprocess postprocess_{};
  uint64_t postprocess_version_ = 0;
  bool grading_ = false;  // shader variant with saturation, vibrance, vignette or scanlines
  bool tracked_fxaa_ = false;  // nfsmw_antialiasing at the last output (to log changes)
  std::array<Framebuffer, VulkanPresenter::kMaxActiveGuestOutputImageVersions> framebuffers_{};
  size_t next_framebuffer_ = 0;

  std::chrono::steady_clock::time_point toggle_start_ = std::chrono::steady_clock::now();
  bool tracked_toggle_ = false;
  uint64_t swap_count_ = 0;    // resolves without a copy
  uint64_t restores_ = 0;
  uint64_t restores_for_resolve_ = 0;  // nfsmw_nativo_resolver_contenido_valido
  uint64_t resolve_without_source_ = 0;           // the content was in no texture
  uint64_t resolve_old_content_ = 0;      // with the setting off, resolves of the stale image
  uint64_t swaps_without_clear_ = 0;  // the ones that used to copy 1600x1600
  uint64_t color_swaps_ = 0;  // color targets resolved without a copy
  // Copies and clears removed, and what they cost.
  std::unordered_map<const Image*, TargetState> target_state_;
  uint64_t skipped_clears_ = 0;             // de color
  uint64_t depth_skipped_clears_ = 0;
  uint64_t skipped_clears_pixels_ = 0;
  uint64_t useless_clears_ = 0;             // clears wiped out by a swap with no draw in between
  uint64_t useless_clears_pixels_ = 0;
  uint64_t clipped_restores_ = 0;     // restores that did not copy the whole image
  uint64_t restored_pixels_ = 0;           // the ones actually copied (so the report adds up)
  uint64_t saved_restore_pixels_ = 0;   // the ones not copied, due to useful area or swap
  uint64_t loans_ = 0;                     // restauraciones hechas volviendo a intercambiar
  uint64_t read_borrowed_ = 0;              // times a lent resolved texture was requested (must be 0)
  std::unordered_set<uint32_t> borrowed_;     // bases whose content is lent to the render target
  // nfsmw_nativo_sombra_minimo. State of the shadow map cycle and phase of the guard.
  enum : uint32_t { kSmFree = 0, kSmAfterWorld = 1, kSmCars = 2 };
  enum : uint32_t { kSmPeeking = 0, kSmApplying = 1, kSmDisabled = 2 };
  static constexpr uint64_t kShadowMinCycles = 300;  // consecutive clean cycles before applying (~10 s of racing)
  uint32_t sm_phase_ = kSmPeeking;
  uint32_t sm_state_ = kSmFree;
  bool sm_enabled_ = false;                  // cvar, alternation and cheap PCF, read at the start of each cycle
  bool sm_tracked_segment_ = true;               // nfsmw_nativo_sombra_minimo_alternar_s: last interval written to the log
  std::chrono::steady_clock::time_point sm_toggle_start_ = std::chrono::steady_clock::now();
  const Image* sm_cycle_target_ = nullptr;   // the current cycle: 1600x1600 target...
  uint32_t sm_cycle_world_ = 0;                // ...and the texture that received the world (no clear)
  bool sm_clean_cycle_ = false;               // nothing watched has failed in this cycle
  bool sm_clear_cycle_ = false;              // this cycle's car pass draws on the cleared target
  bool sm_rewritten_world_ = false;            // the world texture was rewritten within the cycle
  const Image* sm_target_ = nullptr;         // learned while observing: the target...
  uint32_t sm_world_ = 0;                      // ...the texture without cars (textura[1], 07CEA000)...
  uint32_t sm_cars_ = 0;                     // ...and the texture with cars (textura[0], 086AE000)
  bool sm_virtual_ = false;                    // sm_coches_ holds only the cars: sampled with the minimum
  bool sm_valid_virtual_ = false;             // and sm_mundo_ still holds the world of that same cycle
  uint64_t sm_consecutive_clean_ = 0;
  uint64_t sm_cycles_ = 0;
  uint64_t sm_applied_cycles_ = 0;
  uint64_t sm_saved_pixels_ = 0;
  uint64_t sm_saved_pixels_previous_ = 0;
  uint64_t sm_cars_draws_ = 0;
  uint64_t sm_exact_no_draws_ = 0;
  uint64_t sm_reads_min_ = 0;            // of the cars-only texture, paired with the world
  uint64_t sm_reads_if_same_ = 0;          // of the usual texture, paired with itself (observing)
  uint64_t sm_normal_reads_ = 0;          // of the usual texture, without the minimum
  uint64_t sm_reads_to_untimed_ = 0;       // between the two resolves of a cycle
  uint64_t sm_unable_reads_ = 0;         // from a shader without tfetch2DSombraMin on that register
  uint64_t sm_differences_ = 0;
  // nfsmw_nativo_diag_borrados (see UsoBorrado).
  std::unordered_map<const Image*, ClearUse> clears_use_;  // destinos_ and profundidades_ never erase: the key stays valid
  bool diag_clears_ = REXCVAR_GET(nfsmw_nativo_diag_borrados);
  std::chrono::steady_clock::time_point clears_report_{};
  uint64_t clears_report_presented_ = 0;
  // nfsmw_nativo_borrar_area_util.
  bool clear_usable_area_ = REXCVAR_GET(nfsmw_nativo_borrar_area_util);
  bool global_usable_area_disabled_ = false;
  uint32_t pending_bands_ = 0;  // targets with an uncleared band: AnotarUsoBorrado checks them even when not measuring
  uint64_t clears_usable_area_ = 0;
  uint64_t completed_usable_area_ = 0;
  uint64_t failures_usable_area_ = 0;
  uint64_t pixels_usable_area_ = 0;
  uint64_t pixels_usable_area_previous_ = 0;
  static constexpr uint64_t kCyclesUsableArea = 120;  // measured clears of a target before trimming its own
  // nfsmw_nativo_profundidad_perezosa. Deferred depth copies, by texture address.
  struct PendingCopy {
    const Image* src = nullptr;  // depth target (profundidades_ never erases: the pointer stays valid)
    VkImage vk_source = VK_NULL_HANDLE;
    VkImage vk_target = VK_NULL_HANDLE;
    VkImageCopy copy_info{};
    uint64_t frame = 0;     // presentados_ al aplazarla
    bool dead_read = false;  // the composite without blur already requested it
  };
  std::unordered_map<uint32_t, PendingCopy> pending_;
  std::unordered_map<uint32_t, uint64_t> last_alive_read_;    // per address, in presentados_
  std::unordered_map<uint32_t, uint64_t> last_dead_read_;
  std::unordered_set<uint32_t> expired_;     // dropped copies: their texture lacks the last resolve
  bool dead_depth_reads_ = false;  // lo pone DibujosVulkan (LecturasDeProfundidadMuertas)
  bool lazy_disabled_ = false;              // the guard saw a late read
  uint64_t lazy_deferred_ = 0;
  uint64_t lazy_read_copied_ = 0;
  uint64_t lazy_write_copied_ = 0;
  uint64_t lazy_replaced_ = 0;
  uint64_t lazy_dropped_ = 0;
  uint64_t lazy_late_reads_ = 0;
  uint64_t lazy_saved_pixels_ = 0;
  uint64_t lazy_saved_pixels_previous_ = 0;
  static constexpr uint64_t kLazyFrames = 30;  // ~1 s: window for "requested" and "nobody samples it"
  // nfsmw_nativo_frontal_perezoso (see FrontalPendiente).
  std::unordered_map<uint32_t, PendingFront> pending_fronts_;  // per texture address
  std::vector<FrontImage> images_fronts_;                         // repuestos y retenidas
  std::unordered_map<uint32_t, uint64_t> presented_fronts_;  // address -> presentados_ at its last Swap
  std::unordered_map<uint32_t, uint64_t> read_fronts_;       // address -> last sampling by a draw
  std::unordered_set<uint32_t> expired_fronts_;              // missing a copy that can no longer be made
  bool front_disabled_ = false;
  uint64_t front_deferred_ = 0;
  uint64_t front_target_painted_ = 0;
  uint64_t front_retained_painted_ = 0;
  uint64_t front_rotations_ = 0;
  uint64_t front_without_spare_ = 0;
  uint64_t front_read_copied_ = 0;
  uint64_t front_write_copied_ = 0;
  uint64_t front_swap_copied_ = 0;
  uint64_t front_replaced_ = 0;
  uint64_t front_late_reads_ = 0;
  uint64_t front_saved_pixels_ = 0;
  uint64_t front_saved_pixels_previous_ = 0;
  static constexpr uint64_t kFrontFrames = 8;    // window for "the Swap presents it" and "nobody samples it"
  static constexpr size_t kFrontImagesMax = 3;     // spares of the front buffer's target (~4 MB each at 1040)
  // The values above as they were at the previous report: the new lines are per frame.
  uint64_t restored_pixels_previous_ = 0;
  uint64_t saved_restore_pixels_previous_ = 0;
  uint64_t skipped_clears_pixels_previous_ = 0;
  uint64_t useless_clears_pixels_previous_ = 0;
  uint64_t copies_report_presented_ = 0;
  // State of the previous frame, for the stutter dump.
  uint64_t hitch_copies_ = 0, hitch_clears_ = 0, hitch_resolves_ = 0;
  uint64_t hitch_restore_ = 0, hitch_waits_ = 0;
  // Snapshot of the waits at the previous Swap, for the "[tiron] esperas" line.
  uint64_t hitch_waits_ns_[nfsmw::waits::kNumTypes] = {};
  uint64_t hitch_times_waits_[nfsmw::waits::kNumTypes] = {};
  uint64_t hitch_gpu_waits_ns_ = 0, hitch_output_wait_ns_ = 0;
  uint64_t hitch_fingerprint_bytes_ = 0, hitch_deferred_fingerprints_ = 0;
  uint64_t hitch_draws_ = 0, hitch_ring_ns_ = 0, hitch_textures_ns_ = 0;
  uint64_t hitch_uploads_textures_ = 0, hitch_uploaded_bytes_ = 0, hitch_created_textures_ = 0;
  uint64_t hitch_raw_fingerprint_ns_ = 0, hitch_data_fingerprint_ns_ = 0;
  uint64_t hitch_copies_waiting_ns_ = 0, hitch_copies_waits_ = 0;
  uint64_t hitch_copies_helping_ns_ = 0, hitch_assisted_copies_ = 0;
  uint64_t hitch_textures_create_ns_ = 0, hitch_thread_bound_textures_ = 0;
  uint64_t hitch_bindings_waiting_ns_ = 0;
  uint64_t hitch_thread_fingerprint_textures_ = 0, hitch_thread_fingerprint_ns_ = 0, hitch_snapshot_fingerprint_ns_ = 0;
  uint64_t hitch_ring_fingerprint_textures_ = 0, hitch_ring_fingerprint_ns_ = 0, hitch_fingerprints_waiting_ns_ = 0;
  // Why the restore does not swap. See the block in RestaurarContenido.
  uint64_t no_swap_without_transfer_dst_ = 0;
  uint64_t no_target_swap_without_prepare_ = 0;
  uint64_t no_resolved_swap_without_prepare_ = 0;
  uint64_t no_distinct_sizes_swap_ = 0;
  uint64_t hitch_gpu_ns_ = 0;      // GPU timestamps when the previous frame closed
  uint64_t hitch_record_ns_ = 0;   // ns recording when the previous frame closed
  uint32_t hitch_warnings_ = 0;
  uint64_t copies_ = 0;
  uint64_t depth_copies_ = 0;  // those that force keeping the depth tile
  uint64_t depth_clears_ = 0;  // depth clears (borrados_ only counts color ones)
  uint64_t clears_in_failed_pass_ = 0;  // clears through a pass that could not be opened
  uint64_t clears_ = 0;
  uint64_t presented_ = 0;
  uint64_t rechazos_ = 0;
  std::unordered_set<uint32_t> warned_;
};

}  // namespace

bool SupportedColorTargetFormat(uint32_t format_id) {
  return TargetHostFormat(format_id) != VK_FORMAT_UNDEFINED;
}

std::unique_ptr<NativeTargets> NativeTargets::Make(const VulkanDevice* dev,
                                                        rex::memory::Memory* mem) {
  if (!dev || !mem) {
    return nullptr;
  }
  auto targets = std::make_unique<VulkanTargets>(dev, mem);
  if (!targets->Initialize()) {
    REXLOG_ERROR("[nativo] C2: no se pudo preparar la presentacion de destinos");
    return nullptr;
  }
  return targets;
}

}  // namespace nfsmw::nativo
