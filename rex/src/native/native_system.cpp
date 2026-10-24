// nfsmw - native renderer, part C1: the app's own graphics system.
//
// What it does
//   Replaces the Xenos emulation plugin when nfsmw_renderizador is
//   "nativo" (OnPreSetup in nfsmw_app.h decides it):
//     - Presentation: the SDK's VulkanProvider + VulkanPresenter, same as
//       the emulation. ReXApp sees presenter() and sets up F3, settings and
//       the console the usual way (rex_app.cpp:386-398).
//     - The game's D3D keeps running unchanged and fills its command ring. A
//       "sink" consumes it right away: it answers what the game expects from
//       the GPU and hands the draws, copies and Swaps to the rest of the
//       native renderer.
//
// What the game expects from the GPU, and why each thing is needed
//   1. MMIO registers at 0x7FC80000. Without a registered range REX_MM_LOAD_U32
//      reads uninitialized garbage (mmio_handler.cpp:105-124). The same fixed
//      values as the emulation are returned (graphics_system.cpp:232-262).
//   2. The ring read pointer: the game spins in sub_82597690 watching that
//      word to know how much room it has left.
//   3. Interrupts: vblank (sub_82597960 counts vblanks) and PM4_INTERRUPT
//      packets, which signal that the GPU reached that point of the ring.
//   4. GPU writes to memory (MEM_WRITE, COND_WRITE, REG_TO_MEM and
//      EVENT_WRITE_*), with the semantics of graphics/command_processor.cpp.
//      Occlusion queries return 1000 samples, like the emulation.
//
// Where the rest is
//   nfsmw_nativo_destinos.cpp does the copies (resolve and clear) and presents
//   the resolved texture of each Swap, nfsmw_nativo_shaders.cpp identifies the
//   shaders that reach the ring, and nfsmw_nativo_dibujos.cpp draws the
//   geometry. See docs/native-renderer.md.

#include "native_system.h"

#include "stall_waits.h"
#include "native_targets.h"
#include "native_hooks.h"
#include "native_shaders.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include <rex/system/xvideo.h>
#include <rex/thread.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/windowed_app_context.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/*
 * xxHash reads with memcpy (XXH_FORCE_MEMORY_ACCESS 0). With the method it picks for GCC (1) it reads through
 * 64- and 32-bit pointers without may_alias, and GCC may move that read ahead of the store of the data being
 * hashed (strict aliasing). With that, the texture key read claves[4] before writing it and the same texture was
 * created several times (see docs/toolchain.md). Same fingerprint values; on AArch64, the same LDR.
 */
#if defined(XXH_IMPLEM_13a8737387)
#error "xxhash.h ya se ha incluido con su implementacion antes de este punto: XXH_FORCE_MEMORY_ACCESS 0 llegaria tarde"
#endif
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

// The summary of the scenery LOD hook (nfsmw_escenario_lod.cpp).
// Declared here instead of creating a header for a single function.
namespace nfsmw::lod_scenario {
std::string Summary();
}  // namespace nfsmw::escenario_lod

#if REX_PLATFORM_SWITCH
// Only for RexSwitchSetCurrentThreadPriority. That header deliberately does not include switch.h.
#include "core/threading_switch.h"  // SDK src/ is on the include path (rex/CMakeLists.txt)
#endif

REXCVAR_DEFINE_STRING(nfsmw_renderizador, "xenos", "NFSMW",
                      "xenos = emulacion de la GPU; nativo = renderizador nativo en desarrollo "
                      "(pieza C1: el juego corre sin emulacion y la pantalla muestra un color de "
                      "prueba)")
    .allowed({"xenos", "nativo"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(nfsmw_nativo_registros_vectoriales, true, "NFSMW",
                    "Renderizador nativo (24/09, build 164): los bloques de registros del anillo se copian de 4 en 4 "
                    "con NEON. Los primeros 200.000 bloques se comprueban contra el camino de siempre y, si uno "
                    "difiere, se apaga solo. false = el camino de siempre")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(nfsmw_nativo_registros_en_bloque, true, "NFSMW",
                    "Renderizador nativo: los bloques de registros sin efectos (casi siempre constantes de VS y PS) "
                    "se escriben de una vez en vez de registro a registro (build 130). false: como antes");

/*
 * The report is written from the ring thread, and that causes real stutters.
 *
 * Measured over 170 race stutters: the periodic dump explains 33 of them (19.4 %) and 1,267 ms of the 5,707
 * lost. It is not a coincidence: the report runs on a timer, not on what happens in the game, and still 13 of
 * the 30 reports of the race land inside a stuttering frame, when chance would give 1.53 %. That is 28 times the
 * background probability. Each block is 31-37 lines and 5.9-7.4 KB, and takes 14-40 ms to write.
 *
 * Interval: 20 s. A longer interval means fewer dumps, but each one costs twice as much and the damage
 * concentrates in clusters of 5-6 stutters in a row; even so, measured side by side, 10 s loses twice the time:
 *         20 s:  33 stutters from this cause, 211 ms of stutter per minute of racing
 *         10 s:  58 stutters,                 428 ms/min
 *     Half a millisecond per frame thrown away, of the same order as what the query pools gained.
 *
 * So 20. The clusters are ugly, but the total rules, and with 20 s there are still ~15 reports per session,
 * plenty to measure.
 *
 * The real fix is not spacing the dump out but keeping its cost off the ring thread, so that no frame pays for
 * the whole block: see the next comment.
 */
REXCVAR_DEFINE_INT32(nfsmw_nativo_informe_s, 20, "NFSMW",
                     "Renderizador nativo: segundos entre volcados del informe de diagnostico. Cada volcado "
                     "lo escribe el hilo que alimenta a la GPU y cuesta tirones: 10 da el doble de detalle y "
                     "el doble de tirones por esta causa")
    .range(5, 120);

/*
 * The dump is written on another thread.
 *
 * What the comment above asked for, with a new measurement: the block takes 29-32 ms on the ring thread. Of the
 * 6 stutters of 60 ms or more in a race, 3 end 40-70 ms after a dump starts and 1 within 0.12 s of the
 * "C6 subetapas" line, when chance would give 0.5; and 3 of the 7 dumps of the race produced a stutter of 60 ms
 * or more. It is not the formatting: the log is synchronous (log_async = false) and the 2-9 ms gaps between
 * lines fall every 2-4 lines, when the FILE is flushed to the SD.
 *
 * Lines are still formatted on the ring (they read its state, which is only coherent on its thread) and
 * enqueued; the "NFSMW informes" thread writes them, sleeping on a condition variable. Both lessons of the
 * failed asynchronous logger (see log_async in sdk/src/core/logging.cpp) are covered: the queue never blocks
 * the producer (full = the line is dropped and counted) and the thread runs at 0x3B, the only time-sliced
 * priority on Horizon. One point of contention remains: if the ring writes a line of its own right while this
 * thread writes another, it waits for that line to finish (the log lock), not for the whole block.
 * false = as before: the ring writes every line itself.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_informes_diferidos, true, "NFSMW",
                    "Renderizador nativo (25/09, build 179): el informe periodico y las lineas [tiron] del hilo del "
                    "anillo se escriben en la SD desde un hilo aparte; el anillo solo las formatea y las encola. "
                    "false = como antes, las escribe el propio anillo (29-32 ms cada 20 s en la 176)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

/*
 * IM_LOAD without memcmp. On every IM_LOAD the ring compared the whole microcode with guest memory (~0.6 us,
 * ~41,000 per second in a race). Now the D3D constructors and the fetch patcher report what they write
 * (microcode_versions.h, with the cross-thread protocol) and the ring only compares if someone has
 * written that microcode since the last check. The self-checking guard is in CargarShaderCacheado.
 */
#include "microcode_versions.h"

REXCVAR_DEFINE_BOOL(nfsmw_nativo_im_load_sin_memcmp, true, "NFSMW",
                    "Renderizador nativo (25/09, build 184): un IM_LOAD cuyo microcodigo no ha escrito nadie desde la "
                    "ultima comprobacion (lo avisan los constructores y el parcheador de los fetch del D3D) se toma de "
                    "la cache sin compararlo entero con memcmp. Empieza comprobando cada carga contra el memcmp y se "
                    "apaga solo al primer desacuerdo. false = memcmp en cada IM_LOAD, como antes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

/*
 * IM_LOAD_IMMEDIATE with an exact cache. sub_825A37D8 (D3D) copies the VS into the ring itself with the
 * fetches patched and, if needed, with the outputs the PS does not read nulled: op 2B, 4,000-8,000 per second
 * in stretches of a race, at 1.5-2.8 us each ("tiempos por paquete"). Now a packet whose microcode is
 * byte-identical to one already seen (memcmp against the stored raw copy) reuses its swapped copy, its
 * identification, its fingerprint and its generation. The self-checking guard is in CargarInmediato.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_im_inmediato_cache, true, "NFSMW",
                    "Renderizador nativo (25/09, build 184): un IM_LOAD_IMMEDIATE (el VS que el D3D copia en el anillo "
                    "con los fetch parcheados) identico byte a byte a uno ya visto reutiliza su copia girada, su "
                    "identificacion, su huella y su generacion, sin girar ni identificar otra vez. Empieza comprobando "
                    "cada acierto contra el camino de siempre y se apaga solo al primer desacuerdo. false = sin cache, "
                    "como antes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Game frames, one per Swap (nfsmw_d3d_trace.cpp). Only for the AnotarJuegoPorDelante measurement.
extern std::atomic<uint64_t> g_nfsmw_game_frames;

namespace nfsmw::native_renderer {
namespace {

// The line queue and its thread. Created with the first line; it lives until exit.
class ReportQueue {
 public:
  ~ReportQueue() {
    {
      std::lock_guard<std::mutex> mutex_lock(mutex_);
      parar_ = true;
    }
    warning_.notify_one();
    if (thread_.joinable()) {
      thread_.join();  // writes whatever is left before exiting
    }
  }

  // false if there is no thread (it could not be created, or shutdown is under way): then the caller writes
  // the line.
  bool Enqueue(std::string&& line) {
    bool warn = false;
    {
      std::lock_guard<std::mutex> mutex_lock(mutex_);
      if (parar_ || without_thread_) {
        return false;
      }
      if (!thread_.joinable()) {
        try {
          thread_ = std::thread([this] { Loop(); });  // persistent: on Horizon detach() closes the game
        } catch (...) {
          without_thread_ = true;  // no thread (Horizon thread limit): everything as before, nothing breaks
          return false;
        }
      }
      if (queue_.size() >= kMaxLines) {
        ++lost_;  // never block the producer: the line is dropped and counted
        return true;
      }
      warn = queue_.empty();  // the thread only sleeps with the queue empty: one wake-up per burst, not per line
      queue_.push_back(std::move(line));
    }
    if (warn) {
      warning_.notify_one();
    }
    return true;
  }

 private:
  static constexpr size_t kMaxLines = 4096;

  void Loop() {
    rex::thread::set_current_thread_name("NFSMW informes");
#if REX_PLATFORM_SWITCH
    const bool priority = RexSwitchSetCurrentThreadPriorityOk(REX_SWITCH_PRIO_GUEST);
#else
    const bool priority = true;
#endif
    REXLOG_INFO("[nativo] informes diferidos (build 179): hilo en marcha{}",
                priority ? "" : " (el kernel NO acepto la prioridad 0x3B)");
    std::deque<std::string> batch;
    for (;;) {
      uint64_t lost = 0;
      {
        std::unique_lock<std::mutex> mutex_lock(mutex_);
        warning_.wait(mutex_lock, [this] { return parar_ || !queue_.empty(); });
        if (queue_.empty()) {
          return;  // parar_ and nothing pending
        }
        batch.swap(queue_);
        lost = lost_;
        lost_ = 0;
      }
      for (const std::string& line : batch) {
        REXLOG_INFO("{}", line);
      }
      batch.clear();
      if (lost) {
        REXLOG_WARN("[nativo] informes diferidos: {} lineas perdidas (la cola de {} estaba llena)", lost,
                    kMaxLines);
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable warning_;
  std::deque<std::string> queue_;  // con mutex_
  std::thread thread_;              // created with mutex_ held
  bool parar_ = false;            // con mutex_
  bool without_thread_ = false;         // with mutex_: the thread could not be created
  uint64_t lost_ = 0;         // con mutex_
};

}  // namespace

void DeferredReport(std::string line) {
  static const bool defer = REXCVAR_GET(nfsmw_nativo_informes_diferidos);
  if (!defer) {
    REXLOG_INFO("{}", line);
    return;
  }
  static ReportQueue queue_obj;
  if (!queue_obj.Enqueue(std::move(line))) {
    REXLOG_INFO("{}", line);  // no report thread: as before (Encolar has not moved it)
  }
}

}  // namespace nfsmw::nativo

/*
 * See BucleAnillo. Lowered so the presentation thread (0x2C) can preempt it; at the same priority the
 * presenter went without a core 70 % of the time.
 */
REXCVAR_DEFINE_INT32(nfsmw_nativo_anillo_prioridad, 0x2D, "NFSMW",
                     "Switch: prioridad de Horizon del hilo del anillo (0x1C-0x3B). 0x2D por "
                     "defecto, un escalon por debajo del presentador (0x2C) y dos por debajo del "
                     "audio (0x2B). Subirlo a 0x2C devuelve el reparto de la compilacion 113, en el "
                     "que solo llegaba a la pantalla la mitad de los fotogramas");
/*
 * Which core the ring starts on.
 *
 * Pinning the Vulkan thread to a separate CPU core was suggested, and the data support looking at it: the ring
 * uses 94.7 % of a core and the game's main thread 88.2 %, and both run with "preferred core -1", which on
 * Horizon is the process default core: the same one for both. The kernel has to keep moving them, and every
 * migration throws away the thread's L1.
 *
 * That does not show in the mean (total CPU does not correlate with FPS: 258 % at 32 FPS and 280 % at 21) but in
 * the variance, which is what is noticeable. And the variance is exactly the problem: 54 % of frames fall in
 * 30-36 ms, straddling the 33.3 vblank, so a few tenths decide whether a frame shows at 33 ms or jumps to 50.
 *
 * It is not an exclusive affinity: it only changes the preferred core and leaves the mask as it was, so the
 * kernel can still move the thread if needed and this cannot cause starvation. That is why it can be on by
 * default.
 *
 * How it is checked: the "C6 anillo" line of the log counts migrations per second and says which core the
 * thread is on. Measured: the migrations are the same with -1 and with 1 (see the next comment).
 */
REXCVAR_DEFINE_INT32(nfsmw_nativo_anillo_nucleo, 1, "NFSMW",
                     "Switch: nucleo preferido del hilo del anillo (-1 = el de por defecto, que es el "
                     "mismo que el del hilo del juego; 0-2 = uno concreto). No es exclusivo: la "
                     "mascara no se toca, asi que el kernel puede moverlo igual");
/*
 * The real pin.
 *
 * Measured: the preferred core does not help. The kernel accepted it ("preferido 1 (aceptado)") and still the
 * thread showed up on cores 0, 1 and 2 with 0.24 migrations per loop, the same figure wherever it is set.
 * Horizon moves it anyway.
 *
 * It also became clear that migrations by themselves are not the problem: 44 per second, at about 12 us to
 * refill the L1 each, is 0.5 ms/s = 0.05 % of CPU. Negligible.
 *
 * What could matter is removing the competition, which is something else and which the migration counter does
 * not see: the ring (94.7 % of a core) and the game's main thread (88.2 %) want 183 % of a core that gives 100,
 * so they preempt each other. With the mask set to a single core the ring stops competing; the other two cores
 * (200 %) have to carry the remaining 183 %, which barely fits.
 *
 * Off by default: fixing a mask can leave a thread unable to run (unlike the preferred core), and measured on
 * the console, pinning the ring to one core made the peaks worse: the minimum dropped to 16.3 FPS against about
 * 21 without pinning (see docs/platform-notes.md, Threads). The A/B is done from the toml without recompiling,
 * and the log says whether migrations drop to zero.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_anillo_nucleo_exclusivo, false, "NFSMW",
                    "Switch: ademas de preferir el nucleo de nfsmw_nativo_anillo_nucleo, PROHIBE los "
                    "demas (mascara exclusiva). Es el pin de verdad. Ojo: si ese nucleo se satura, el "
                    "anillo se queda sin correr; mira las migraciones/s del log, que deben caer a 0");
REXCVAR_DEFINE_INT32(nfsmw_nativo_espera_regmem_us, 1000, "NFSMW",
                     "Renderizador nativo: pausa entre sondeos de WAIT_REG_MEM en microsegundos (en la "
                     "Switch es exacta; en Windows, por debajo de 1000 solo se cede el turno)");
REXCVAR_DEFINE_INT32(nfsmw_nativo_diag_fotograma_s, 0, "NFSMW",
                     "Renderizador nativo: pasados estos segundos, anota en el log cada dibujo y "
                     "cada copia de un fotograma entero (0 = no; solo pruebas)");
REXCVAR_DEFINE_STRING(nfsmw_nativo_diag_vertices_ps, "", "NFSMW",
                      "Renderizador nativo: en el fotograma trazado, para los dibujos con estos PS "
                      "(numeros separados por comas) anota los fetch del VS y los bytes de sus "
                      "primeros vertices y texels (solo pruebas)");
// The game measures with an occlusion query how much of the sun is visible (sub_82225438: GetData, Issue(BEGIN),
// a draw and Issue(END)) and uses it to turn off the flare when trees or terrain cover it. With the faked count of
// 1000 samples the flare was always drawn in full: sky burned to white and blue or purple halos in the trees.
REXCVAR_DEFINE_INT32(nfsmw_nativo_oclusion, 1, "NFSMW",
                     "Renderizador nativo: consultas de oclusion del juego (el destello del sol). 1 = medidas en la GPU, "
                     "como la Xbox 360; 0 = cuenta fingida de 1000 muestras (lo de antes de la build 146: el destello "
                     "no se tapa nunca); 2 = cuenta fingida de 0 muestras (solo pruebas: nunca hay destello)")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(nfsmw_nativo_diag_constantes_ps, "", "NFSMW",
                      "Renderizador nativo (solo pruebas): numeros de PS (separados por comas) de los que se anotan sus "
                      "12 primeras constantes, como mucho cada nfsmw_nativo_diag_constantes_ms");
REXCVAR_DEFINE_INT32(nfsmw_nativo_diag_constantes_ms, 250, "NFSMW",
                     "Renderizador nativo (solo pruebas): intervalo minimo entre anotaciones de constantes por PS")
    .range(0, 60000);
REXCVAR_DEFINE_INT32(nfsmw_nativo_oclusion_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (solo pruebas): con nfsmw_nativo_oclusion = 1 y N > 0, alterna cada N segundos "
                     "entre consultas medidas (tramos pares) y la cuenta fingida de 1000 (tramos impares), y anota cada "
                     "cambio, para comparar el destello del sol en la misma carrera")
    .range(0, 600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_nativo_oclusion_escala, 0, "NFSMW",
                     "Renderizador nativo: muestras por pixel con que se cuentan las consultas de oclusion (0 = las del "
                     "modo de antialiasing que tiene elegido el juego, como en la Xbox 360; 1-16 = fijas, solo pruebas)")
    .range(0, 16)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The 30 FPS guard lives in nfsmw_recorte_sombras.cpp, which owns the switch.
namespace nfsmw::guard30 {
void Latir(double ms);
void Report();
}  // namespace nfsmw::guardia30

namespace nfsmw::render_targets {
uint32_t OriginalSamplesCurrentMode(const uint8_t* base);  // nfsmw_render_targets.cpp
}

namespace nfsmw::native_renderer {
namespace {

namespace xenos = rex::graphics::xenos;
using rex::X_STATUS;  // X_STATUS_SUCCESS and X_STATUS_UNSUCCESSFUL need it too
using Clock = std::chrono::steady_clock;
using OutputContext = rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext;

std::atomic<uint64_t> g_native_swaps{0};  // SwapsNativos(), for the watchdog

/*
 * Adding without a lock in the counters only the ring writes.
 *
 * Stack sampling pointed here: 36 % of the ring thread's time is in BucleAnillo -> Paquete -> PaqueteTipo3. And
 * every packet did a `fetch_add` on an atomic. On Cortex-A57 (ARMv8.0, without the ARMv8.1 atomic instructions)
 * a fetch_add is an ldxr/stxr loop with an exclusive reservation: ~15-20 cycles, and it also invalidates the line
 * on the other cores. At 83,000 packets per frame that is ~1.1 ms per frame spent counting packets.
 *
 * Why removing it is safe: these counters are written by a single thread, the ring thread (Paquete and
 * PaqueteTipo3 are only called from BucleAnillo and from BuferIndirecto, which hangs off it), and the only reader
 * is Informe, which runs on that same thread (BucleAnillo calls it on every loop). The shutdown Informe(true)
 * runs after hilo_anillo_->Wait(), that is, with the thread already dead. Without two writers there is no need
 * for an atomic read-modify-write.
 *
 * They stay std::atomic on purpose (the type does not change, nor do the readers); the only thing that changes
 * is how they are incremented: a relaxed load and store, which on 64-bit ARM64 are a plain ldr and str, without
 * exclusive reservation and without barriers. It is still correct for an outside reader (it never sees half a
 * value) and it stops costing.
 *
 * Do not use this for counters another thread touches: escrituras_wptr_ (written by the game thread through
 * MMIO), vblanks_ and contador_ (the vblank thread). Those keep fetch_add.
 */
template <typename T>
inline void AddRingOnly(std::atomic<T>& counter, T how_much = 1) {
  counter.store(counter.load(std::memory_order_relaxed) + how_much, std::memory_order_relaxed);
}

constexpr uint32_t kBaseMmio = 0x7FC80000;
/*
 * From 16 to 128, because the packet count grew sixfold.
 *
 * 16 was chosen when there were ~14,000 packets per frame. Stack sampling gives the real count: 20.8 million
 * packets per interval, which at 18.1 packets per draw is ~83,000 per frame. At 1 in 16 that is 5,187 clock
 * reads per frame, and each Reloj::now() is a CNTPCT_EL0 read plus the conversion.
 *
 * With 128 there are 648 samples per frame, still plenty for a mean: the relative error of a sample of 648 is
 * 4 %, and these times are read to split percentages, not for exact figures.
 */
constexpr uint64_t kPacketsStopwatchEvery = 128;  // packets per timed one (power of 2)
/*
 * From 8 to 64. At 1 in 8, this timer and above all the stage timer in nfsmw_nativo_dibujos.cpp (about 20 clock
 * reads per timed draw) added up to thousands of clock reads per frame in the alley, with 2,500-4,000 draws.
 * With 64 there are ~1,400 measured draws per second, plenty for a mean every 20 s. The "tiempos" line keeps
 * its scale: the measured time is multiplied by this same factor and divided by all the draws.
 */
constexpr uint64_t kDrawsStopwatchEvery = 64;   // draws per timed one (power of 2)
constexpr uint32_t kMmioMask = 0xFFFF0000;
constexpr uint32_t kMmioSize = 0x0000FFFF;
constexpr uint32_t kNumRegs = 0x5003;  // RegisterFile::kRegisterCount

// Registers with a fixed value or with an effect, as in graphics_system.cpp.
constexpr uint32_t kRegCpRbWptr = 0x01C5;
constexpr uint32_t kRegRbEdramTiming = 0x0F00;
constexpr uint32_t kRegRbBcControl = 0x0F01;
constexpr uint32_t kRegD1GrphPrimarySurface = 0x1844;
// Gamma ramp, from DC_LUT_RW_MODE (0x1921) to DC_LUT_WRITE_EN_MASK (0x1927) (register_table.inc).
// The mask is 0x1927 (an earlier version stopped at 0x1926), and the output applies the ramp.
constexpr uint32_t kRegRampFirst = 0x1921;
constexpr uint32_t kRegIndexRamp = 0x1922;
constexpr uint32_t kRegSequentialRamp = 0x1923;
constexpr uint32_t kRegRamp30 = 0x1925;
constexpr uint32_t kRegMaskRamp = 0x1927;
constexpr uint32_t kRegRampLast = 0x1927;
constexpr uint32_t kRegD1ModeVCounter = 0x194C;
constexpr uint32_t kRegD1ModeVblankVlineStatus = 0x1951;
constexpr uint32_t kRegD1ModeViewportSize = 0x1961;

/*
 * Register generations, so that a draw does not redo work whose inputs did not change.
 *
 * Measured on the console (race): the ring thread is busy 42.5 ms of every 44.2 ms frame, so it is the
 * limit, not the GPU (37.1 ms of real work). Of those 42.5 ms, draw packets (op 22) take 29.5, and inside
 * the translator the most expensive stages are "texturas" (4.1 ms) and half of "pipeline", which is
 * really viewport and scissor.
 *
 * Both are pure functions of registers that almost never change between consecutive draws: the fetch
 * constants (0x4800-0x48BF) and the viewport state (viewport, scissor, clip and window offset). This
 * tracks when they really change (same technique as generacion_constantes_vs_/ps_: compare the value
 * before writing it), and the number travels in PeticionDibujo so the translator can skip the work with
 * a single uint64 comparison.
 *
 * There are 32 fetch constants of 6 words each: the first 16 are used by textures (kRegFetch in
 * nfsmw_nativo_dibujos.cpp) and vertex fetches read theirs from the same block.
 */
constexpr uint32_t kRegFetchFirst = 0x4800;  // SHADER_CONSTANT_FETCH_00_0
constexpr uint32_t kRegFetchLast = 0x48BF;   // SHADER_CONSTANT_FETCH_31_5

// Viewport state registers: PA_SC_WINDOW_OFFSET/SCISSOR_TL/BR, PA_CL_VPORT_[XYZ]SCALE/OFFSET,
// PA_CL_CLIP_CNTL, PA_SU_SC_MODE_CNTL, PA_CL_VTE_CNTL and PA_SU_VTX_CNTL. No VGT_* register is included:
// VGT_DRAW_INITIATOR (0x21FC) changes on every draw and would make the generation useless.
constexpr uint32_t kRegFramingFirst = 0x2080;
constexpr uint32_t kRegFramingLast = 0x2302;
inline bool IsFramingReg(uint32_t index) {
  // One subtraction and one comparison: rejects everything outside the range.
  if (index - kRegFramingFirst > kRegFramingLast - kRegFramingFirst) {
    return false;
  }
  return index <= 0x2082 ||                      // PA_SC_WINDOW_OFFSET .. SCISSOR_BR
         (index >= 0x210F && index <= 0x2114) ||  // PA_CL_VPORT_XSCALE .. ZOFFSET
         (index >= 0x2204 && index <= 0x2206) ||  // PA_CL_CLIP_CNTL, PA_SU_SC_MODE_CNTL, PA_CL_VTE_CNTL
         index == 0x2302;                          // PA_SU_VTX_CNTL
}

/*
 * Phase 2 of the Direct3D-level renderer: the ring-thread half.
 * Applying registers was measured at ~146 ms/s of the ring thread (~4 ms per frame, with the ring at
 * 95 %), almost all of it in EscribirRegistrosEnBloque: 94.5 million words in blocks against 2.2 million
 * one at a time in 10 s. The regular loop (TramoEscalar) byte-swaps each word, compares it and classifies
 * it with several branches. TramoVectorial does the same 4 words at a time with NEON: it splits the span
 * at the boundaries of each class (VS, PS and fetch constants and the small viewport state pieces), copies
 * each piece byte-swapped and only checks whether anything changed in the piece, which is all that was
 * used from the classification.
 */
inline void ScaleSegment(uint32_t index, uint32_t* target, const uint8_t* src, uint32_t n, bool& vs,
                         bool& ps, bool& fetch, bool& framing) {
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t fresh = rex::memory::load_and_swap<uint32_t>(src + size_t(i) * 4);
    if (target[i] != fresh) {
      const uint32_t abs = index + i;
      if (abs >= 0x4000) {
        if (abs < 0x4400) {
          vs = true;
        } else if (abs < 0x4800) {
          ps = true;
        } else if (abs <= kRegFetchLast) {
          fetch = true;
        }
      } else if (IsFramingReg(abs)) {
        framing = true;
      }
      target[i] = fresh;
    }
  }
}

// Copies n big-endian words byte-swapped; true if any of them differed from the previous value.
inline bool CopyWithChange(uint32_t* target, const uint8_t* src, uint32_t n) {
  uint32_t i = 0;
  bool change = false;
#if defined(__aarch64__)
  uint32x4_t difference = vdupq_n_u32(0);
  for (; i + 4 <= n; i += 4) {
    const uint32x4_t fresh = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(src + size_t(i) * 4)));
    difference = vorrq_u32(difference, veorq_u32(fresh, vld1q_u32(target + i)));
    vst1q_u32(target + i, fresh);
  }
  change = vmaxvq_u32(difference) != 0;
#endif
  for (; i < n; ++i) {
    const uint32_t fresh = rex::memory::load_and_swap<uint32_t>(src + size_t(i) * 4);
    change |= target[i] != fresh;
    target[i] = fresh;
  }
  return change;
}

inline void VectorSegment(uint32_t index, uint32_t* target, const uint8_t* src, uint32_t n, bool& vs,
                           bool& ps, bool& fetch, bool& framing) {
  /*
   * Fast path. Almost every span is a block of VS or PS constants (or fetch constants) that lies entirely
   * inside its class: one CopiarConCambio plus marking the class if anything changed is enough, with no
   * search for boundaries (the loop below walks all 12 for every piece). Same if the span touches no class
   * and not the viewport state range: it is only copied. Anything that crosses a boundary still takes the
   * regular loop.
   * Same result as TramoEscalar: tested on the PC with 3.5 million blocks (every start from 0x1F00 to the
   * end with lengths 1 to 40, plus 2 million random ones), 0 differences. VerificarBloque still compares it
   * with the regular path for the first blocks of each run, like the rest of the vector path.
   */
  if (n != 0) {
    const uint32_t is_last = index + n - 1;  // EscribirRegistrosEnBloque already checked it stays inside the table
    if (index >= 0x4000) {
      if (is_last < 0x4400) {
        if (CopyWithChange(target, src, n)) {
          vs = true;
        }
        return;
      }
      if (index >= 0x4400 && is_last < 0x4800) {
        if (CopyWithChange(target, src, n)) {
          ps = true;
        }
        return;
      }
      if (index >= 0x4800 && is_last <= kRegFetchLast) {
        if (CopyWithChange(target, src, n)) {
          fetch = true;
        }
        return;
      }
      if (index > kRegFetchLast) {
        CopyWithChange(target, src, n);  // booleans and loops: no class
        return;
      }
    } else if (is_last < kRegFramingFirst || (index > kRegFramingLast && is_last < 0x4000)) {
      CopyWithChange(target, src, n);  // outside every class and the viewport state range
      return;
    }
  }
  // Where a class starts or ends (see EsRegistroDeEncuadre and TramoEscalar).
  static constexpr uint32_t kCuts[] = {kRegFramingFirst, 0x2083, 0x210F, 0x2115, 0x2204, 0x2207,
                                         0x2302, 0x2303, 0x4000, 0x4400, 0x4800, kRegFetchLast + 1};
  uint32_t i = 0;
  while (i < n) {
    const uint32_t abs = index + i;
    uint32_t stop_pos = n;
    for (const uint32_t cut : kCuts) {
      if (cut > abs && cut - index < stop_pos) {
        stop_pos = cut - index;
      }
    }
    const bool change = CopyWithChange(target + i, src + size_t(i) * 4, stop_pos - i);
    if (change) {
      if (abs >= 0x4000) {
        if (abs < 0x4400) {
          vs = true;
        } else if (abs < 0x4800) {
          ps = true;
        } else if (abs <= kRegFetchLast) {
          fetch = true;
        }
      } else if (IsFramingReg(abs)) {
        framing = true;
      }
    }
    i = stop_pos;
  }
}

constexpr uint32_t kOcclusionSamples = 1000;  // query_occlusion_fake_sample_count
constexpr uint16_t kMaxExtension = 2048 >> 3;  // xenos::kTexture2DCubeMaxWidthHeight >> 3
constexpr int kMaxIndirectDepth = 4;
constexpr auto kRegMemWaitMax = std::chrono::milliseconds(200);
constexpr uint32_t kOutputWidth = 1280;
constexpr uint32_t kOutputHeight = 720;

uint64_t NanosecondsFrom(std::chrono::steady_clock::time_point start) {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count());
}

// Big-endian words of a command buffer. In the ring the position wraps around (the size is a power
// of two); in an indirect buffer it does not.
struct Reader {
  const uint8_t* base = nullptr;
  uint32_t mask = 0;  // words - 1 in the ring; 0 in a linear buffer
  uint32_t pos = 0;
  uint32_t stop_pos = 0;

  uint32_t Pending() const { return mask ? ((stop_pos - pos) & mask) : (stop_pos - pos); }
  uint32_t Peek() const { return rex::memory::load_and_swap<uint32_t>(base + size_t(pos) * 4); }
  uint32_t Read() {
    const uint32_t value = Peek();
    Advance(1);
    return value;
  }
  void Advance(uint32_t word_data) { pos = mask ? ((pos + word_data) & mask) : (pos + word_data); }
};

bool Compares(uint32_t info, uint32_t value, uint32_t reference) {
  switch (info & 0x7) {
    case 0x0:
      return false;
    case 0x1:
      return value < reference;
    case 0x2:
      return value <= reference;
    case 0x3:
      return value == reference;
    case 0x4:
      return value != reference;
    case 0x5:
      return value >= reference;
    case 0x6:
      return value > reference;
    default:
      return true;
  }
}

class NativeGraphicsSystem final : public rex::system::IGraphicsSystem {
 public:
  NativeGraphicsSystem() : regs_(kNumRegs, 0), seen_(kNumRegs) {}
  ~NativeGraphicsSystem() override { Shutdown(); }

  X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override {
    if (presenter_) {
      return X_STATUS_SUCCESS;
    }
    app_context_ = app_context;
    if (!provider_) {
      // Steps C5c-C6: the XenosRecomp SPIR-V needs capabilities that the VkDevice only
      // enables if they are requested before it is created.
      rex::cvar::SetFlagByName("vulkan_native_shader_features", "true");
      provider_ = rex::ui::vulkan::VulkanProvider::Create(true, true);
      if (!provider_) {
        REXLOG_ERROR("[nativo] No se pudo crear el dispositivo Vulkan");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    auto create = [this]() { presenter_ = provider_->CreatePresenter(); };
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous(create);
    } else {
      create();
    }
    if (!presenter_) {
      REXLOG_ERROR("[nativo] No se pudo crear el presentador");
      return X_STATUS_UNSUCCESSFUL;
    }
    REXLOG_INFO("[nativo] Sistema grafico nativo (C1): presentador del SDK, sin emulacion del Xenos");
    return X_STATUS_SUCCESS;
  }

  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* dispatcher,
                         rex::system::KernelState* kernel_state) override {
    dispatcher_ = dispatcher;
    kernel_state_ = kernel_state;
    memory_ = dispatcher->memory();
    if (!registered_mmio_) {
      registered_mmio_ = memory_->AddVirtualMappedRange(kBaseMmio, kMmioMask, kMmioSize, this,
                                                        &ReadMmio, &WriteMmio);
      if (!registered_mmio_) {
        REXLOG_ERROR("[nativo] No se pudo registrar el rango MMIO de la GPU");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    if (active_.exchange(true)) {
      return X_STATUS_SUCCESS;
    }
    // Step C5a: the NFSSPV library sits next to the executable (it is built from the game
    // data, so it is not distributed). Without it the game still runs, without identification.
    if (shaders_.Load(rex::filesystem::GetExecutableFolder() / "airborne_shaders.nfsp")) {
      ActivateHooks(&shaders_);  // step C5b: shader objects and Draw* records
    }
    last_report_ = Clock::now();
    last_time_report_ = last_report_;
    system_start_ = last_report_;
    vblank_thread_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        kernel_state_, 128 * 1024, 0, [this]() { return VblankLoop(); }));
    vblank_thread_->set_name("GPU VSync nativo");
    vblank_thread_->Create();
    ring_thread_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        kernel_state_, 128 * 1024, 0, [this]() { return RingLoop(); }));
    ring_thread_->set_name("GPU anillo nativo");
    ring_thread_->Create();
    // From here on FlushState can write the composite marker (ProcesarMarcador).
    ActivateMarkersConsumer(true);
    return X_STATUS_SUCCESS;
  }

  bool has_presentation() const override { return presenter_ != nullptr; }
  rex::ui::GraphicsProvider* provider() const override { return provider_.get(); }
  rex::ui::Presenter* presenter() const override { return presenter_.get(); }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    data_callback_.store(user_data, std::memory_order_release);
    callback_.store(callback, std::memory_order_release);
    REXLOG_INFO("[nativo] Callback de interrupcion {:08X} ({:08X})", callback, user_data);
  }

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    // As in CommandProcessor::InitializeRingBuffer: (1 << (size_log2 + 3)) bytes.
    words_ring_.store(uint32_t(1) << (size_log2 + 1), std::memory_order_release);
    base_ring_.store(ptr, std::memory_order_release);
    ring_generation_.fetch_add(1, std::memory_order_acq_rel);
    REXLOG_INFO("[nativo] Anillo en {:08X}, {} palabras", ptr, uint32_t(1) << (size_log2 + 1));
  }

  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override {
    (void)block_size_log2;
    returned_read_.store(ptr, std::memory_order_release);
  }

  void Shutdown() override {
    ActivateHooks(nullptr);  // the game hooks stop touching the library
    ActivateMarkersConsumer(false);  // FlushState goes back to the game's own path
    if (active_.exchange(false)) {
      {
        std::lock_guard<std::mutex> mutex_lock(mutex_ring_);
      }
      cv_ring_.notify_all();
      if (ring_thread_) {
        ring_thread_->Wait(0, 0, 0, nullptr);
        ring_thread_.reset();
      }
      if (vblank_thread_) {
        vblank_thread_->Wait(0, 0, 0, nullptr);
        vblank_thread_.reset();
      }
      Report(true);
    }
    // The framebuffers point to presenter images: destroy them before it.
    DestroyVulkan();
    if (presenter_) {
      if (app_context_) {
        app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
      }
      presenter_.reset();
    }
    provider_.reset();
  }

 private:
  // --- Registros MMIO -------------------------------------------------------

  static uint32_t ReadMmio(void* ppc_context, void* contexto, uint32_t address) {
    (void)ppc_context;
    return static_cast<NativeGraphicsSystem*>(contexto)->ReadMmioReg(address);
  }

  static void WriteMmio(void* ppc_context, void* contexto, uint32_t address, uint32_t value) {
    (void)ppc_context;
    static_cast<NativeGraphicsSystem*>(contexto)->WriteMmioReg(address, value);
  }

  uint32_t ReadMmioReg(uint32_t address) {
    const uint32_t r = (address & 0xFFFF) / 4;
    switch (r) {
      case kRegRbEdramTiming:
        return 0x08100748;
      case kRegRbBcControl:
        return 0x0000200E;
      case kRegD1ModeVCounter: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      case kRegD1ModeVblankVlineStatus:
        return 1;  // vblank
      case kRegD1ModeViewportSize: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        const uint32_t width_px = std::min(uint32_t(mode.display_width), uint32_t(0x0FFF));
        const uint32_t height_px = std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
        return (width_px << 16) | height_px;
      }
      default:
        break;
    }
    TrackFirstTime("lectura", r, 0);
    return Reg(r);
  }

  void WriteMmioReg(uint32_t address, uint32_t value) {
    const uint32_t r = (address & 0xFFFF) / 4;
    if (r == kRegCpRbWptr) {
      write_pointer_.store(value, std::memory_order_release);
      wptr_writes_.fetch_add(1, std::memory_order_relaxed);
      // Taking the lock for an instant avoids losing the wakeup if the ring
      // thread is right between checking and going to sleep.
      {
        std::lock_guard<std::mutex> mutex_lock(mutex_ring_);
      }
      cv_ring_.notify_one();
    } else if (r != kRegD1GrphPrimarySurface) {
      TrackFirstTime("escritura", r, value);
    }
    SaveReg(r, value);  // no side effects through MMIO (graphics_system.cpp:265-281)
  }

  // The gamma ramp the game loads, with the same logic as the SDK's CommandProcessor::WriteRegister
  // (DC_LUT_SEQ_COLOR in red, green and blue; DC_LUT_30_COLOR with blue in bits 0-9). Called before the
  // value is stored. The write mask (blue at bit 0) and the index that only advances after DC_LUT_30_COLOR
  // match the SDK; every change bumps version_rampa_ and the output picks it up on the next Swap.
  void TrackGammaRamp(uint32_t index, uint32_t value) {
    ++ramp_writes_[index - kRegRampFirst];
    if (index == kRegIndexRamp) {
      ramp_component_ = 0;
    } else if (index == kRegMaskRamp) {
      ramp_mask_ = value & 0b111;
    } else if (index == kRegSequentialRamp) {
      const uint32_t i = regs_[kRegIndexRamp] & 0xFF;
      if (ramp_mask_ & (UINT32_C(1) << (2 - ramp_component_))) {
        gamma_ramp_[i][ramp_component_] = uint16_t((value >> 6) & 0x3FF);
        ++ramp_version_;
      }
      if (++ramp_component_ >= 3) {
        ramp_component_ = 0;
        regs_[kRegIndexRamp] = (regs_[kRegIndexRamp] & ~UINT32_C(0xFF)) | ((i + 1) & 0xFF);
      }
    } else if (index == kRegRamp30) {
      const uint32_t i = regs_[kRegIndexRamp] & 0xFF;
      if (ramp_mask_ & 0b001) {
        gamma_ramp_[i][2] = uint16_t(value & 0x3FF);
      }
      if (ramp_mask_ & 0b010) {
        gamma_ramp_[i][1] = uint16_t((value >> 10) & 0x3FF);
      }
      if (ramp_mask_ & 0b100) {
        gamma_ramp_[i][0] = uint16_t((value >> 20) & 0x3FF);
      }
      if (ramp_mask_) {
        ++ramp_version_;
      }
      ramp_component_ = 0;
      regs_[kRegIndexRamp] = (regs_[kRegIndexRamp] & ~UINT32_C(0xFF)) | ((i + 1) & 0xFF);
    }
  }

  // EVENT_WRITE_ZPD: the sample counters of an occlusion query.
  // How the game's D3D does it:
  //   - Issue(BEGIN) (sub_8258F810) marks the structure at base + 0 with 0xFFFFFEED and requests the
  //     counters at base + 32.
  //   - Issue(END) requests them at base + 0 (sub_8258EA28).
  //   - GetData (sub_8258F998) waits for base + 0 to lose the marks and returns
  //     ZPass(base + 0) - ZPass(base + 32).
  // Here (nfsmw_nativo_oclusion = 1): at the start, base + 32 is zeroed, and the draws up to the end are
  // counted on the GPU with host queries. At the end, base + 0 gets the last measured count of that
  // structure: one from an earlier frame, because host queries are read when their work finishes. Anything
  // that does not fit keeps the old behavior (a faked count).
  void OcclusionQuery(uint32_t address) {
    using SampleCounts = xenos::xe_gpu_depth_sample_counts;
    constexpr uint32_t kMark = 0xEDFEFFFF;  // 0xFFFFFEED written big-endian by D3D, read as little-endian
    const auto marked = [](const SampleCounts* c) {
      return uint32_t(c->ZPass_A) == kMark || uint32_t(c->ZPass_B) == kMark || uint32_t(c->ZFail_A) == kMark ||
             uint32_t(c->ZFail_B) == kMark;
    };
    auto* counts = memory_->TranslatePhysical<SampleCounts*>(address);
    const bool here_marks = marked(counts);
    const bool marks_before = address >= 32 && marked(memory_->TranslatePhysical<SampleCounts*>(address - 32));
    int32_t mode = REXCVAR_GET(nfsmw_nativo_oclusion);
    // Test cvar nfsmw_nativo_oclusion_alternar_s: odd intervals use the old faked count.
    if (const int32_t alternar_s = REXCVAR_GET(nfsmw_nativo_oclusion_alternar_s); mode == 1 && alternar_s > 0) {
      const auto now_time = Clock::now();
      if (!started_test_occlusion_) {
        started_test_occlusion_ = true;
        start_test_occlusion_ = now_time;
      }
      const auto secs =
          std::chrono::duration_cast<std::chrono::seconds>(now_time - start_test_occlusion_).count();
      const bool faked = (secs / alternar_s) % 2 == 1;
      if (faked != faked_test_occlusion_) {
        faked_test_occlusion_ = faked;
        if (faked && oclusion_base_ != 0 && targets_) {
          uint64_t discarded = 0;
          targets_->FinishOcclusion(oclusion_base_, discarded);
          oclusion_base_ = 0;
        }
        REXLOG_INFO("[nativo] prueba de oclusion: {}", faked ? "fingidas (1000)" : "medidas");
      }
      if (faked) {
        mode = 0;
      }
    }
    const char* que = nullptr;
    uint32_t written = UINT32_MAX;
    uint64_t measures = 0;
    bool measure = false;
    uint32_t scale = 0;
    if (mode == 1 && targets_) {
      if (oclusion_base_ != 0 && address == oclusion_base_) {
        measure = targets_->FinishOcclusion(address, measures);
        oclusion_base_ = 0;
        scale = OcclusionScale();
        written = measure ? uint32_t(std::min<uint64_t>(measures * scale, 0xFFFFFF)) : kOcclusionSamples;
        std::memset(counts, 0, sizeof(*counts));
        counts->ZPass_A = written;
        counts->Total_A = written;
        que = measure ? "final" : "final sin medida";
        if (measure) {
          ++counters_occlusion_[1];
          counters_occlusion_[5] += written;
          counters_occlusion_[6] = std::max<uint64_t>(counters_occlusion_[6], written);
        } else {
          ++counters_occlusion_[2];
        }
        // By render target: the 320 one (the sun) or a scene one (640 or more, scaled by the AA mode).
        const size_t group = scale > 1 ? 1 : 0;
        ++occlusion_per_target_[group];
        if (measure) {
          max_occlusion_per_target_[group] = std::max(max_occlusion_per_target_[group], written);
        }
        if (group == 1 && scene_occlusion_warnings_ < 12) {
          ++scene_occlusion_warnings_;
          const uint32_t info = Reg(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO);
          REXLOG_INFO("[nativo] C2 oclusion en destino de escena {}: estructura {:08X}, pitch {}, MSAA {}, escala {}, "
                      "muestras del host {} ({}), escritas {}",
                      scene_occlusion_warnings_, address, info & 0x3FFF, (info >> 16) & 0x3, scale, measures,
                      measure ? "medidas" : "sin medida", written);
        }
      } else if (address >= 32 && (marks_before || bases_oclusion_.count(address - 32))) {
        const uint32_t base = address - 32;
        if (oclusion_base_ != 0) {
          uint64_t discarded = 0;
          targets_->FinishOcclusion(oclusion_base_, discarded);  // la anterior no llego a su Issue(END)
          ++counters_occlusion_[3];
        }
        if (bases_oclusion_.size() < 64) {
          bases_oclusion_.insert(base);
        }
        targets_->StartOcclusion(base);
        oclusion_base_ = base;
        std::memset(counts, 0, sizeof(*counts));
        que = "principio";
        ++counters_occlusion_[0];
      }
    }
    if (!que) {
      // The previous behavior (also the emulated path's): a faked count at the end.
      std::memset(counts, 0, sizeof(*counts));
      if (here_marks) {
        written = mode == 2 ? 0 : kOcclusionSamples;
        counts->ZPass_A = written;
        counts->Total_A = written;
        ++counters_occlusion_[4];
      }
      que = mode == 1 ? "sin pareja" : "fingida";
    }
    if (occlusion_warnings_ < 8) {  // was 48; these lines are long and the ring thread writes them
      ++occlusion_warnings_;
      // AirborneNX: NFSMW also printed its renderer's AA mode and quality here, read from its globals
      // (0x82A2D1AC, 0x82A2CEE4). Those addresses hold unrelated data in MoH: Airborne (dereferencing
      // one crashed the ring thread), so they are not read.
      const uint32_t info = Reg(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO);
      REXLOG_INFO("[nativo] C2 oclusion {}: ZPD en {:08X} ({}), marcas aqui {} y 32 antes {}; muestras del host {} ({}), "
                  "escala {}, escritas {}; bins {:016X}/{:016X}; RB_SURFACE_INFO {:08X} (pitch {}, MSAA {}), "
                  "RB_DEPTH_INFO {:08X}",
                  occlusion_warnings_, address, que, here_marks, marks_before, measures, measure ? "medidas" : "sin medida",
                  scale, written == UINT32_MAX ? -1 : int64_t(written), bin_select_, bin_mask_, info, info & 0x3FFF,
                  (info >> 16) & 0x3, Reg(rex::graphics::XE_GPU_REG_RB_DEPTH_INFO));
    }
  }

  // Samples per pixel the Xbox 360 would count in the query's render target: the MSAA samples the target
  // requests and, for scene targets (pitch of 640 or more), those of the game's AA mode before it is
  // removed (nfsmw_render_sin_mosaico). The sun flare is queried on a 320 target without MSAA: 1.
  uint32_t OcclusionScale() const {
    const int32_t fixed_val = REXCVAR_GET(nfsmw_nativo_oclusion_escala);
    if (fixed_val > 0) {
      return uint32_t(fixed_val);
    }
    const uint32_t info = Reg(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO);
    const uint32_t target_samples = UINT32_C(1) << std::min<uint32_t>((info >> 16) & 0x3, 2);
    const uint32_t scene_samples =
        (info & 0x3FFF) >= 640 ? nfsmw::render_targets::OriginalSamplesCurrentMode(memory_->virtual_membase()) : 1;
    return std::max(target_samples, scene_samples);
  }

  void TrackFirstTime(const char* que, uint32_t index, uint32_t value) {
    if (index >= kNumRegs || seen_[index].exchange(1, std::memory_order_acq_rel)) {
      return;
    }
    REXLOG_INFO("[nativo] Registro MMIO {:04X}: primera {} ({:08X})", index, que, value);
  }

  // The registers are touched by the game thread (MMIO) and by the ring thread. As in the
  // emulated path, races on integer values are harmless.
  void SaveReg(uint32_t index, uint32_t value) {
    if (index < kNumRegs) {
      /*
       * This entry point is used by MMIO from the game thread and by VIZ_QUERY, and it does not go
       * through EscribirRegistro. Neither touches the fetch constants or the viewport state today (the
       * 360's D3D writes GPU registers through the ring), but if one ever did and the generation were
       * not bumped, the translator would keep the stale viewport or textures. It is called very rarely
       * compared with the ring, so the safeguard costs nothing.
       */
      if (regs_[index] != value) {
        if (index >= kRegFetchFirst && index <= kRegFetchLast) {
          ++generacion_fetch_;
        } else if (IsFramingReg(index)) {
          ++framing_generation_;
        }
      }
      regs_[index] = value;
    }
  }

  // A write from the ring, with the side effects it has when the GPU processes it
  // (CommandProcessor::WriteRegister). SCRATCH_REG values are copied to memory: the
  // D3D writes one and then waits with WAIT_REG_MEM until it shows up at SCRATCH_ADDR
  // (measured on the PC: without this the game stops within the first second).
  // COHER_STATUS_HOST stays marked as pending until the next poll.
  void WriteReg(uint32_t index, uint32_t value) {
    if (index >= kNumRegs) {
      return;
    }
    /*
     * Only when the value really changes.
     *
     * The Xbox 360 D3D dumps the constant block before every draw, and many constants are identical
     * between draws of the same material. Bumping the generation on every write made 60 % of the draws
     * re-upload their 7.7 KB of constants to the buffer (12.54 MB per frame, measured) into memory with
     * no CPU cache (2654 MB/s), on the thread that already uses 96 % of a core.
     *
     * Comparing before writing costs one read of a line that is about to be written anyway, so it is
     * already in cache. And the result is bit-for-bit identical: if the value does not change, the
     * shader reads the same thing.
     */
    if (index >= 0x4000) {
      if (index < 0x4800) {  // VS and PS constants (native draws)
        if (regs_[index] != value) {
          ++(index < 0x4400 ? generacion_constantes_vs_ : generacion_constantes_ps_);
        } else {
          ++constants_without_change_;
        }
        ++written_constants_;
      } else if (index <= kRegFetchLast) {  // fetch constants: textures and vertices
        if (regs_[index] != value) {
          ++generacion_fetch_;
        } else {
          ++fetch_without_change_;
        }
        ++written_fetch_;
      }
    } else if (IsFramingReg(index)) {
      if (regs_[index] != value) {
        ++framing_generation_;
      } else {
        ++framing_without_change_;
      }
      ++framing_written_;
    }
    if (index == rex::graphics::XE_GPU_REG_COHER_STATUS_HOST) {
      value |= UINT32_C(0x80000000);
    }
    if (index >= kRegRampFirst && index <= kRegRampLast) {
      TrackGammaRamp(index, value);
    }
    regs_[index] = value;
    if (index >= rex::graphics::XE_GPU_REG_SCRATCH_REG0 &&
        index <= rex::graphics::XE_GPU_REG_SCRATCH_REG7) {
      const uint32_t n = index - rex::graphics::XE_GPU_REG_SCRATCH_REG0;
      if ((UINT32_C(1) << n) & regs_[rex::graphics::XE_GPU_REG_SCRATCH_UMSK]) {
        const uint32_t address = regs_[rex::graphics::XE_GPU_REG_SCRATCH_ADDR] + n * 4;
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(address), value);
        AddRingOnly(memory_writes_);
      }
    }
  }

  // A block of consecutive registers without side effects, in one go. Returns false (and consumes nothing)
  // if it touches COHER_STATUS_HOST or the SCRATCH_REG registers, if it goes past the table or if it is
  // disabled.
  bool WriteRegsInBlock(uint32_t index, uint32_t num, Reader& payload) {
    if (regs_in_block_ < 0) {
      regs_in_block_ = REXCVAR_GET(nfsmw_nativo_registros_en_bloque) ? 1 : 0;
      REXLOG_INFO("[nativo] registros en bloque (nfsmw_nativo_registros_en_bloque) = {}",
                  regs_in_block_ ? "SI" : "no");
    }
    if (!regs_in_block_ || num < 2 || index >= kNumRegs || num > kNumRegs - index) {
      return false;
    }
    const uint32_t stop_pos = index + num;
    const auto is_due = [index, stop_pos](uint32_t from, uint32_t hasta) { return index < hasta && from < stop_pos; };
    if (is_due(rex::graphics::XE_GPU_REG_COHER_STATUS_HOST, rex::graphics::XE_GPU_REG_COHER_STATUS_HOST + 1) ||
        is_due(rex::graphics::XE_GPU_REG_SCRATCH_REG0, rex::graphics::XE_GPU_REG_SCRATCH_REG7 + 1) ||
        is_due(kRegRampFirst, kRegRampLast + 1)) {
      return false;
    }
    // Same as above, but a block does not know whether it changed until it has been copied, so
    // the generation is bumped afterwards, and only if some word in the range differs.
    const bool vs_peek = is_due(0x4000, 0x4400);
    const bool ps_peek = is_due(0x4400, 0x4800);
    // The same for the fetch constants and the viewport state. The viewport state is four small
    // separate pieces, so it is enough to check whether the block crosses the range that holds them:
    // inside the loop each word is already classified, and a generation bumped too often only makes
    // the translator do extra work, it never draws wrong.
    const bool fetch_peek = is_due(kRegFetchFirst, kRegFetchLast + 1);
    const bool framing_peek = is_due(kRegFramingFirst, kRegFramingLast + 1);
    const bool values_peek = vs_peek || ps_peek || fetch_peek || framing_peek;
    bool vs_change = false;
    bool ps_change = false;
    bool fetch_change = false;
    bool framing_change = false;
    // The vector path, with a self-checking guard (see VerificarBloque).
    if (vector_regs_ < 0) {
      vector_regs_ = REXCVAR_GET(nfsmw_nativo_registros_vectoriales) ? 1 : 0;
      REXLOG_INFO("[nativo] registros vectoriales (nfsmw_nativo_registros_vectoriales) = {}",
                  vector_regs_ ? "SI, comprobando los primeros bloques contra el camino de siempre"
                                         : "no");
    }
    const bool vectorial = vector_regs_ > 0;
    const bool verify = vectorial && verified_blocks_ < kBlocksToVerify;
    Reader verification_data;
    if (verify) {
      verification_data = payload;
      verification_copy_.assign(regs_.begin() + index, regs_.begin() + index + num);
    }
    uint32_t current_index = index;
    uint32_t* target = regs_.data() + index;
    uint32_t remain = num;
    while (remain) {
      // Contiguous span: up to the end of the ring (in a linear buffer, everything left).
      const uint32_t segment = payload.mask ? std::min(remain, payload.mask + 1 - payload.pos) : remain;
      const uint8_t* src = payload.base + size_t(payload.pos) * 4;
      if (values_peek) {
        if (vectorial) {
          VectorSegment(current_index, target, src, segment, vs_change, ps_change, fetch_change,
                         framing_change);
        } else {
          ScaleSegment(current_index, target, src, segment, vs_change, ps_change, fetch_change,
                       framing_change);
        }
        if (vs_peek || ps_peek) {
          written_constants_ += segment;
        }
        if (fetch_peek) {
          written_fetch_ += segment;
        }
        if (framing_peek) {
          framing_written_ += segment;
        }
      } else if (vectorial) {
        CopyWithChange(target, src, segment);
      } else {
        for (uint32_t i = 0; i < segment; ++i) {
          target[i] = rex::memory::load_and_swap<uint32_t>(src + size_t(i) * 4);
        }
      }
      current_index += segment;
      target += segment;
      payload.Advance(segment);
      remain -= segment;
    }
    if (verify) {
      VerifyBlock(index, num, verification_data, values_peek, vs_change, ps_change, fetch_change,
                      framing_change);
    }
    if (vs_change) {
      ++generacion_constantes_vs_;
    }
    if (ps_change) {
      ++generacion_constantes_ps_;
    }
    if (fetch_change) {
      ++generacion_fetch_;
    }
    if (framing_change) {
      ++framing_generation_;
    }
    if ((vs_peek && !vs_change) || (ps_peek && !ps_change)) {
      ++constants_blocks_without_change_;
    }
    if (vs_peek || ps_peek) {
      ++constants_blocks_;
    }
    return true;
  }

  /*
   * The vector path guard. For the first kBloquesAVerificar blocks, the block is replayed through the
   * regular path (TramoEscalar) on a copy of the previous registers, and the results are compared register
   * by register and class by class. If anything differs, the regular path's result is written, the log
   * reports it and the vector path is turned off for the rest of the run.
   */
  void VerifyBlock(uint32_t index, uint32_t num, Reader payload, bool values_peek, bool& vs, bool& ps,
                       bool& fetch, bool& framing) {
    bool ref_vs = false;
    bool ref_ps = false;
    bool ref_fetch = false;
    bool framing_ref = false;
    uint32_t done_val = 0;
    while (done_val < num) {
      const uint32_t remain = num - done_val;
      const uint32_t segment = payload.mask ? std::min(remain, payload.mask + 1 - payload.pos) : remain;
      ScaleSegment(index + done_val, verification_copy_.data() + done_val, payload.base + size_t(payload.pos) * 4, segment,
                   ref_vs, ref_ps, ref_fetch, framing_ref);
      payload.Advance(segment);
      done_val += segment;
    }
    ++verified_blocks_;
    const bool values_equal =
        std::equal(verification_copy_.begin(), verification_copy_.end(), regs_.begin() + index);
    const bool classes_equal =
        !values_peek || (ref_vs == vs && ref_ps == ps && ref_fetch == fetch && framing_ref == framing);
    if (!values_equal || !classes_equal) {
      REXLOG_ERROR("[nativo] registros vectoriales: el bloque {:04X}+{} NO coincide con el camino de siempre "
                   "(valores {}, clases VS {}/{} PS {}/{} fetch {}/{} encuadre {}/{}). Se APAGA el camino vectorial.",
                   index, num, values_equal ? "iguales" : "DISTINTOS", vs, ref_vs, ps, ref_ps, fetch,
                   ref_fetch, framing, framing_ref);
      std::copy(verification_copy_.begin(), verification_copy_.end(), regs_.begin() + index);
      if (values_peek) {
        vs = ref_vs;
        ps = ref_ps;
        fetch = ref_fetch;
        framing = framing_ref;
      }
      vector_regs_ = 0;
      return;
    }
    if (verified_blocks_ == kBlocksToVerify) {
      REXLOG_INFO("[nativo] registros vectoriales: {} bloques comprobados contra el camino de siempre, todos "
                  "iguales. Sigue el camino vectorial.",
                  verified_blocks_);
    }
  }

  /*
   * Phase 2 of the Direct3D-level renderer: the FlushState composite marker.
   * See kMarcadorMagia in native_hooks.h and the end of nfsmw_d3d_registros_nativo.cpp. FlushState
   * writes one NOP with all its register spans instead of ~7 type-0 packets and ~6 padding words per draw.
   *   - Apply mode: the spans are written here with the same effect as a type-0 block (values, generations
   *     and counters of EscribirRegistrosEnBloque).
   *   - Check mode: the type-0 packets of the same dump come right before it and have already been applied.
   *     It only checks that the registers hold what the marker says, and the result goes back to the game
   *     thread.
   * A NOP without the magic belongs to the game itself and is ignored, as before.
   */
  void ProcessMarker(Reader& payload, uint32_t word_data) {
    if (word_data < 2 || (payload.Peek() & 0xFFFFFF00u) != kMagicMarker) {
      return;
    }
    const uint32_t hdr = payload.Read();
    const uint32_t mode = hdr & 0x0Fu;               // registros: 0 sin tramos, 1 aplicar, 2 comprobar
    const uint32_t draw_mode = (hdr >> 4) & 0x0Fu;  // phase 2b: 0 none, kDibujoAplicar, kDibujoComprobar
    const uint32_t sequence = payload.Read();
    uint32_t n = word_data - 2;  // draw record, span headers and values
    // The spans, contiguous in memory: D3D never splits a packet; if it wraps around the ring, it is copied
    // aside.
    const uint8_t* p = payload.base + size_t(payload.pos) * 4;
    if (payload.mask && payload.pos + n > payload.mask + 1) {
      plane_marker_.resize(n);
      for (uint32_t i = 0; i < n; ++i) {
        std::memcpy(&plane_marker_[i], payload.base + size_t((payload.pos + i) & payload.mask) * 4, 4);
      }
      p = reinterpret_cast<const uint8_t*>(plane_marker_.data());
    }
    // Phase 2b: the Draw* record of this FlushState, ahead of the spans. It is stored at the end, once the
    // whole marker has been validated.
    DrawRecord draw;
    if (draw_mode != 0) {
      if ((draw_mode != kApplyDraw && draw_mode != kCheckDraw) || n < kDrawWords ||
          !ReadMarkerDraw(p, draw)) {
        BadMarker(mode, sequence, hdr);
        return;
      }
      p += size_t(kDrawWords) * 4;
      n -= kDrawWords;
    }
    // Validate the whole structure first: a broken marker writes nothing.
    uint32_t segments = 0;
    for (uint32_t i = 0; i < n;) {
      const uint32_t t = rex::memory::load_and_swap<uint32_t>(p + size_t(i) * 4);
      const uint32_t num = t >> 16;
      if (num >= n - i || !DumpSegment(t & 0xFFFFu, num)) {
        BadMarker(mode, sequence, t);
        return;
      }
      i += 1 + num;
      ++segments;
    }
    if (mode > kCheckMarker || (mode == 0 && (n != 0 || draw_mode == 0))) {
      BadMarker(mode, sequence, hdr);
      return;
    }
    // For the DRAW_INDX packets that follow (EmparejarDibujo): until one accepts it or the next Draw* record
    // arrives.
    if (draw_mode != 0) {
      marker_draw_ = draw;
      mode_marker_draw_ = draw_mode;
    }
    if (mode == kCheckMarker) {
      CheckMarker(p, n, sequence);
      return;
    }
    if (mode == 0) {
      return;  // only the draw record
    }
    // The fast path is decided once per marker: the block path and the vector path enabled and already
    // verified.
    const bool fast =
        regs_in_block_ > 0 && vector_regs_ > 0 && verified_blocks_ >= kBlocksToVerify;
    for (uint32_t i = 0; i < n;) {
      const uint32_t t = rex::memory::load_and_swap<uint32_t>(p + size_t(i) * 4);
      const uint32_t index = t & 0xFFFFu;
      const uint32_t num = t >> 16;
      if (fast && num >= 2 && fast_marker_ > 0) {
        ApplyFastSegment(index, num, p + size_t(i + 1) * 4);
      } else {
        // The regular path, same as a type-0 packet with these registers.
        ++slow_segments_marker_;
        Reader lineal;
        lineal.base = p;
        lineal.pos = i + 1;
        lineal.stop_pos = n;
        if (num < 2 || !WriteRegsInBlock(index, num, lineal)) {
          for (uint32_t k = 0; k < num; ++k) {
            WriteReg(index + k, rex::memory::load_and_swap<uint32_t>(p + size_t(i + 1 + k) * 4));
          }
        }
      }
      i += 1 + num;
    }
    ++applied_markers_;
    segments_marker_ += segments;
    words_marker_ += n - segments;
  }

  void BadMarker(uint32_t mode, uint32_t sequence, uint32_t word_value) {
    ++bad_markers_;
    if (marker_warnings_ < 8) {
      ++marker_warnings_;
      REXLOG_ERROR("[nativo] marcador D3D {} MAL FORMADO (modo {}, palabra {:08X}): no se aplica, y el hilo del juego "
                   "apaga el camino del marcador",
                   sequence, mode, word_value);
    }
    TrackMarkerCheck(false, sequence, 0, 0, word_value);
  }

  void CheckMarker(const uint8_t* p, uint32_t n, uint32_t sequence) {
    ++checked_markers_;
    for (uint32_t i = 0; i < n;) {
      const uint32_t t = rex::memory::load_and_swap<uint32_t>(p + size_t(i) * 4);
      const uint32_t index = t & 0xFFFFu;
      const uint32_t num = t >> 16;
      for (uint32_t k = 0; k < num; ++k) {
        const uint32_t value = rex::memory::load_and_swap<uint32_t>(p + size_t(i + 1 + k) * 4);
        if (regs_[index + k] != value) {
          ++distinct_markers_;
          if (marker_warnings_ < 8) {
            ++marker_warnings_;
            REXLOG_ERROR("[nativo] marcador D3D {}: el registro {:04X} vale {:08X} tras sus paquetes y {:08X} en el "
                         "marcador. El hilo del juego apaga el camino del marcador",
                         sequence, index + k, regs_[index + k], value);
          }
          TrackMarkerCheck(false, sequence, index + k, regs_[index + k], value);
          return;
        }
      }
      i += 1 + num;
    }
    TrackMarkerCheck(true, sequence, 0, 0, 0);
  }

  // A span of 2 or more registers from a single dump group (TramoDeVolcado). Same effect as
  // EscribirRegistrosEnBloque with those registers (values, generations and counters), without its checks
  // and without searching for boundaries: in each group, the classes lie in a single known piece.
  void ApplyFastSegment(uint32_t index, uint32_t num, const uint8_t* src) {
    uint32_t* target = regs_.data() + index;
    const bool verify = verified_fast_segments_ < kFastSegmentsToVerify;
    if (verify) {
      marker_copy_.assign(target, target + num);
    }
    bool vs = false;
    bool ps = false;
    bool fetch = false;
    bool framing = false;
    const uint32_t stop_pos = index + num;
    if (index >= 0x4000) {
      // VS, PS, fetch or boolean constants: the whole span belongs to a single class.
      const bool change = CopyWithChange(target, src, num);
      if (index < 0x4400) {
        vs = change;
      } else if (index < 0x4800) {
        ps = change;
      } else if (index <= kRegFetchLast) {
        fetch = change;
      }
    } else {
      // 0x2000-0x23FF: the viewport state of each group is a single piece (0x210F-0x2114,
      // 0x2204-0x2206 and 0x2302).
      uint32_t e0 = stop_pos;
      uint32_t e1 = stop_pos;
      if (index >= 0x2100 && index < 0x2180) {
        e0 = 0x210F;
        e1 = 0x2115;
      } else if (index >= 0x2200 && index < 0x2280) {
        e0 = 0x2204;
        e1 = 0x2207;
      } else if (index >= 0x2300 && index < 0x2380) {
        e0 = 0x2302;
        e1 = 0x2303;
      }
      e0 = std::min(std::max(e0, index), stop_pos);
      e1 = std::min(std::max(e1, e0), stop_pos);
      CopyWithChange(target, src, e0 - index);
      framing = CopyWithChange(target + (e0 - index), src + size_t(e0 - index) * 4, e1 - e0);
      CopyWithChange(target + (e1 - index), src + size_t(e1 - index) * 4, stop_pos - e1);
    }
    if (verify) {
      VerifyMarkerSegment(index, num, src, vs, ps, fetch, framing);
    }
    // The same counters and generations as EscribirRegistrosEnBloque for a block with these registers.
    if (index >= 0x4000 && index < 0x4800) {
      written_constants_ += num;
      ++constants_blocks_;
      if (!(index < 0x4400 ? vs : ps)) {
        ++constants_blocks_without_change_;
      }
    } else if (index >= kRegFetchFirst && index <= kRegFetchLast) {
      written_fetch_ += num;
    } else if (index <= kRegFramingLast && stop_pos > kRegFramingFirst) {
      framing_written_ += num;
    }
    if (vs) {
      ++generacion_constantes_vs_;
    }
    if (ps) {
      ++generacion_constantes_ps_;
    }
    if (fetch) {
      ++generacion_fetch_;
    }
    if (framing) {
      ++framing_generation_;
    }
  }

  // The fast path guard: the first kTramosRapidosAVerificar spans are replayed through TramoEscalar (the
  // regular path, word by word) on a copy of the previous registers, and values and classes are compared.
  // If anything differs, the regular path's result is kept and the fast path is turned off for the rest
  // of the run.
  void VerifyMarkerSegment(uint32_t index, uint32_t num, const uint8_t* src, bool& vs, bool& ps, bool& fetch,
                              bool& framing) {
    bool ref_vs = false;
    bool ref_ps = false;
    bool ref_fetch = false;
    bool framing_ref = false;
    ScaleSegment(index, marker_copy_.data(), src, num, ref_vs, ref_ps, ref_fetch, framing_ref);
    ++verified_fast_segments_;
    const bool all_equal = std::equal(marker_copy_.begin(), marker_copy_.end(), regs_.begin() + index) &&
                         ref_vs == vs && ref_ps == ps && ref_fetch == fetch && framing_ref == framing;
    if (!all_equal) {
      REXLOG_ERROR("[nativo] marcador D3D: el camino rapido del tramo {:04X}+{} NO coincide con el de siempre "
                   "(clases VS {}/{} PS {}/{} fetch {}/{} encuadre {}/{}). Se APAGA el camino rapido.",
                   index, num, vs, ref_vs, ps, ref_ps, fetch, ref_fetch, framing, framing_ref);
      std::copy(marker_copy_.begin(), marker_copy_.end(), regs_.begin() + index);
      vs = ref_vs;
      ps = ref_ps;
      fetch = ref_fetch;
      framing = framing_ref;
      fast_marker_ = 0;
      return;
    }
    if (verified_fast_segments_ == kFastSegmentsToVerify) {
      REXLOG_INFO("[nativo] marcador D3D: {} tramos del camino rapido comprobados contra el de siempre, todos iguales. "
                  "Sigue el camino rapido.",
                  verified_fast_segments_);
    }
  }

  uint32_t Reg(uint32_t index) const { return index < kNumRegs ? regs_[index] : 0; }

  // --- GPU memory (the two low bits of the address are the byte order) ----

  uint32_t ReadMemory(uint32_t address) const {
    uint32_t value;
    std::memcpy(&value, memory_->TranslatePhysical(address & ~uint32_t(0x3)), sizeof(value));
    return xenos::GpuSwap(value, static_cast<xenos::Endian>(address & 0x3));
  }

  void WriteMemory(uint32_t address, uint32_t value) {
    const uint32_t ordered = xenos::GpuSwap(value, static_cast<xenos::Endian>(address & 0x3));
    std::memcpy(memory_->TranslatePhysical(address & ~uint32_t(0x3)), &ordered, sizeof(ordered));
    AddRingOnly(memory_writes_);
  }

  // --- Threads ------------------------------------------------------------------

  void Interrupt(uint32_t src_select, uint32_t cpu) {
    const uint32_t callback = callback_.load(std::memory_order_acquire);
    if (!callback || !dispatcher_) {
      return;
    }
    auto* thread_obj = rex::system::XThread::GetCurrentThread();
    if (!thread_obj) {
      return;
    }
    thread_obj->SetActiveCpu(uint8_t(cpu));
    uint64_t arguments[] = {src_select, data_callback_.load(std::memory_order_acquire)};
    dispatcher_->ExecuteInterrupt(thread_obj->thread_state(), callback, arguments, 2);
    interrupts_.fetch_add(1, std::memory_order_relaxed);  // NOTE: also called from BucleVblank (another thread)
    WarnRingProgress();  // some D3D waits also check the interrupt counter
  }

  int VblankLoop() {
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    const double hz = std::max(1.0, double(float(mode.refresh_rate)));
    const auto interval =
        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / hz));
    auto next = Clock::now() + interval;
    while (active_.load(std::memory_order_acquire)) {
      const auto now_time = Clock::now();
      // After a long pause (debugger, loading) hundreds of vblanks are not replayed.
      if (now_time - next > std::chrono::milliseconds(250)) {
        next = now_time;
      }
      while (now_time >= next) {
        counter_.fetch_add(1, std::memory_order_relaxed);
        vblanks_.fetch_add(1, std::memory_order_relaxed);
        Interrupt(0, 2);
        next += interval;
      }
      rex::thread::Sleep(std::chrono::milliseconds(1));
    }
    return 0;
  }

  int RingLoop() {
    /*
     * The ring thread drops to 0x2D, because of the presenter.
     *
     * Measured: our presentation thread took 54.26 ms per iteration, 53.38 of them in "record"
     * (twenty Vulkan calls that should cost less than a millisecond). It was not work: the thread
     * spent 998 ms of wall-clock time per second but only 300 ms of CPU. The other 700 ms it was
     * ready with no core, preempted by this thread, which runs at 81 % and shared its priority
     * (0x2C). On Horizon, priorities below 0x3B are not time-sliced, so at equal priority the
     * presenter only ran when the ring thread yielded... and the ring thread had just lost exactly
     * what used to make it yield.
     *
     * Result: only 13.37 of the 26.25 frames per second reached the window.
     *
     * This thread is lowered instead of raising the presenter, to stay out of the audio priority
     * (0x2B), which was hard to get right and is settled. The presenter needs ~2 ms about 27 times
     * per second (5 % of a core) and sleeps on a condition variable as soon as it is done, so what
     * it takes from this thread is negligible.
     */
#if REX_PLATFORM_SWITCH
    {
      const int32_t priority = REXCVAR_GET(nfsmw_nativo_anillo_prioridad);
      if (priority >= 0x1C && priority <= 0x3B) {
        RexSwitchSetCurrentThreadPriority(int(priority));
        REXLOG_INFO("[nativo] hilo del anillo a prioridad {:#x} (el presentador va en 0x2C y tiene "
                    "que poder desalojarlo)", uint32_t(priority));
      }
      /*
       * And which core it starts on. See nfsmw_nativo_anillo_nucleo.
       *
       * The core is logged before and after. Without the "before" the line cannot be interpreted: if
       * the process's default core were already 1, asking for core 1 would separate nothing at all and
       * the log would look like a success.
       */
      const int core_before = RexSwitchCurrentCore();
      const int32_t core = REXCVAR_GET(nfsmw_nativo_anillo_nucleo);
      const bool exclusive = core >= 0 && REXCVAR_GET(nfsmw_nativo_anillo_nucleo_exclusivo);
      if (core >= -1 && core <= 2) {
        const bool put_value = exclusive ? RexSwitchPinCurrentThreadToCore(int(core))
                                      : RexSwitchSetCurrentThreadCore(int(core));
        REXLOG_INFO("[nativo] hilo del anillo: nucleo {} {} ({}); estaba en el {} y ahora corre en "
                    "el {}",
                    core, exclusive ? "EXCLUSIVO (mascara reducida)" : "preferido",
                    put_value ? "aceptado" : "RECHAZADO por el kernel", core_before,
                    RexSwitchCurrentCore());
      }
      ring_core_ = RexSwitchCurrentCore();
    }
#endif
    uint32_t was_read = 0;
    uint32_t generacion = 0;
    while (active_.load(std::memory_order_acquire)) {
      {
        std::unique_lock<std::mutex> mutex_lock(mutex_ring_);
        const auto before_without_job = Clock::now();  // for the "[tiron]" line
        const bool with_signal = cv_ring_.wait_for(mutex_lock, std::chrono::milliseconds(4), [&] {
          return !active_.load(std::memory_order_acquire) ||
                 write_pointer_.load(std::memory_order_acquire) != was_read ||
                 ring_generation_.load(std::memory_order_acquire) != generacion;
        });
        nfsmw::waits::Add(nfsmw::waits::kRingWithoutJob, NanosecondsFrom(before_without_job));
        // Wakeup counters (only this thread uses them; Informe runs on it).
        ++ring_turns_;
        if (!with_signal) {
          ++exhausted_ring_waits_;
        }
#if REX_PLATFORM_SWITCH
        /*
         * Counts how many times the kernel has moved this thread to another core. It is one
         * register read per loop iteration (not a real syscall), and it is the only way to know
         * whether the preferred core does anything: if -1 and 1 give the same count, it has no
         * effect here.
         */
        {
          const int now_time = RexSwitchCurrentCore();
          if (now_time != ring_core_) {
            ring_core_ = now_time;
            ++ring_migrations_;
          }
        }
#endif
      }
      if (!active_.load(std::memory_order_acquire)) {
        break;
      }
      Report(false);
      const uint32_t gen = ring_generation_.load(std::memory_order_acquire);
      if (gen != generacion) {
        generacion = gen;
        was_read = 0;  // InitializeRingBuffer leaves the read pointer at zero
      }
      const uint32_t base = base_ring_.load(std::memory_order_acquire);
      const uint32_t word_data = words_ring_.load(std::memory_order_acquire);
      if (!base || !word_data) {
        continue;
      }
      const uint32_t written_val = write_pointer_.load(std::memory_order_acquire) & (word_data - 1);
      if (written_val == was_read) {
        continue;
      }
      ++ring_turns_with_data_;
      Reader reader;
      reader.base = memory_->TranslatePhysical(base);
      reader.mask = word_data - 1;
      reader.pos = was_read;
      reader.stop_pos = written_val;
      const auto ring_start = Clock::now();
      while (reader.Pending()) {
        if (!Packet(reader, 0)) {
          // Partial packet: the rest will arrive with the next CP_RB_WPTR.
          break;
        }
      }
      {
        const uint64_t ring_ns = NanosecondsFrom(ring_start);
        ring_time_ns_ += ring_ns;
        AddRingOnly(nfsmw::waits::g_working_ring_ns, ring_ns);  // without fetch_add
      }
      was_read = reader.pos;
      // Deferred vertex copies must be done before telling the game that the GPU has read up to here:
      // after that it may reuse those buffers (nfsmw_nativo_subidas_hilo).
      if (targets_) {
        targets_->WaitUploads();
      }
      const uint32_t give_back = returned_read_.load(std::memory_order_acquire);
      if (give_back) {
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(give_back), was_read);
      }
      // After returning it: the game's D3D waits sleep until this notification (nfsmw_espera_anillo.cpp).
      WarnRingProgress();
    }
    return 0;
  }

  // --- Paquetes PM4 -----------------------------------------------------------

  // Consumes a whole packet, or leaves the position untouched if it is not complete yet.
  bool Packet(Reader& reader, int depth_value) {
    const uint32_t packet = reader.Peek();
    uint32_t load_op = 0;
    switch (packet >> 30) {
      case 0:
        load_op = packet ? ((packet >> 16) & 0x3FFF) + 1 : 0;
        break;
      case 1:
        load_op = 2;
        break;
      case 2:
        load_op = 0;
        break;
      default:
        load_op = ((packet >> 16) & 0x3FFF) + 1;
        break;
    }
    if (reader.Pending() < load_op + 1) {
      return false;
    }
    reader.Advance(1);
    AddRingOnly(packets_);
    Reader payload = reader;
    reader.Advance(load_op);
    if (!packet) {
      return true;
    }
    // Timer on 1 of every kCronometroPaquetesCada packets, with the time scaled up: there are ~14,000 per
    // frame, and two clock reads per packet cost more than writing the register. WAIT_REG_MEM, long and
    // infrequent, is always timed.
    const bool time = (++packets_stopwatch_ & (kPacketsStopwatchEvery - 1)) == 0;
    const auto start = time ? Clock::now() : Clock::time_point{};
    switch (packet >> 30) {
      case 0: {
        const uint32_t index = packet & 0x7FFF;
        const bool any_reg = (packet >> 15) & 0x1;
        if (any_reg || !WriteRegsInBlock(index, load_op, payload)) {
          for (uint32_t i = 0; i < load_op; ++i) {
            WriteReg(any_reg ? index : index + i, payload.Read());
          }
          released_words_ += load_op;  // Measurement only
        } else {
          block_words_ += load_op;
        }
        if (time) {
          regs_time_ns_ += NanosecondsFrom(start) * kPacketsStopwatchEvery;
        }
        ++regs_count_;
        break;
      }
      case 1: {
        const uint32_t value1 = payload.Read();
        const uint32_t value2 = payload.Read();
        WriteReg(packet & 0x7FF, value1);
        WriteReg((packet >> 11) & 0x7FF, value2);
        if (time) {
          regs_time_ns_ += NanosecondsFrom(start) * kPacketsStopwatchEvery;
        }
        ++regs_count_;
        break;
      }
      case 2:
        break;
      default: {
        const uint32_t opcode = (packet >> 8) & 0x7F;
        const bool wait_stats = opcode == xenos::PM4_WAIT_REG_MEM;
        const auto op_start = wait_stats && !time ? Clock::now() : start;
        PacketType3(payload, packet, load_op, depth_value);
        // An indirect buffer is timed through its packets, so they are not counted twice.
        if (opcode != xenos::PM4_INDIRECT_BUFFER && opcode != xenos::PM4_INDIRECT_BUFFER_PFD) {
          if (wait_stats) {
            opcode_time_ns_[opcode] += NanosecondsFrom(op_start);
          } else if (time) {
            opcode_time_ns_[opcode] += NanosecondsFrom(start) * kPacketsStopwatchEvery;
          }
          ++opcode_count_[opcode];
        }
        break;
      }
    }
    return true;
  }

  void PacketType3(Reader& payload, uint32_t packet, uint32_t word_data, int depth_value) {
    const uint32_t opcode = (packet >> 8) & 0x7F;
    AddRingOnly(opcodes_[opcode]);
    if ((packet & 0x1) && (!(bin_select_ & bin_mask_) || opcode == xenos::PM4_XE_SWAP)) {
      return;  // predicate fails; predicated Swaps are always dropped
    }
    switch (opcode) {
      case xenos::PM4_INTERRUPT: {
        if (word_data < 1) break;
        TrackCopyFence();  // Measurement only
        const uint32_t cpus = payload.Read();
        for (uint32_t cpu = 0; cpu < 6; ++cpu) {
          if (cpus & (1u << cpu)) {
            Interrupt(1, cpu);
          }
        }
        break;
      }
      case xenos::PM4_XE_SWAP: {
        // VdSwap writes "SWAP", the physical address of the frontbuffer and its size.
        if (word_data >= 4) {
          payload.Read();
          swap_frontbuffer_.store(payload.Read(), std::memory_order_relaxed);
          swap_width_.store(payload.Read(), std::memory_order_relaxed);
          swap_height_.store(payload.Read(), std::memory_order_relaxed);
        }
        SwapTrace();
        Present();
        TrackGameAhead();  // Measurement only
        break;
      }
      case xenos::PM4_INDIRECT_BUFFER:
      case xenos::PM4_INDIRECT_BUFFER_PFD: {
        if (word_data < 2) break;
        const uint32_t address = payload.Read() & 0x1FFFFFFF;
        const uint32_t length = payload.Read() & 0xFFFFF;
        if (depth_value < kMaxIndirectDepth) {
          IndirectBuffer(address, length, depth_value + 1);
        }
        break;
      }
      case xenos::PM4_WAIT_REG_MEM: {
        if (word_data < 5) break;
        const uint32_t info = payload.Read();
        const uint32_t poll = payload.Read();
        const uint32_t reference = payload.Read();
        const uint32_t mask = payload.Read();
        const auto regmem_start = Clock::now();  // for the "[tiron]" line
        const auto limit = regmem_start + kRegMemWaitMax;
        uint32_t turns = 0;
        for (;;) {
          if (!(info & 0x10) && poll == rex::graphics::XE_GPU_REG_COHER_STATUS_HOST &&
              (Reg(poll) & UINT32_C(0x80000000))) {
            SaveReg(poll, 0);  // MakeCoherent: there is no shared memory to synchronize
          }
          const uint32_t value = (info & 0x10) ? ReadMemory(poll) : Reg(poll);
          if (Compares(info, value & mask, reference)) {
            break;
          }
          // The real GPU waits; without a GPU some conditions will never be
          // met. The wait is cut short so the ring does not hang.
          if (Clock::now() >= limit || !active_.load(std::memory_order_acquire)) {
            if (exhausted_waits_.fetch_add(1, std::memory_order_relaxed) == 0) {
              REXLOG_WARN("[nativo] WAIT_REG_MEM sin cumplirse ({} {:08X} ref {:08X} mascara {:08X})",
                          (info & 0x10) ? "memoria" : "registro", poll, reference, mask);
            }
            break;
          }
          const int32_t pause_us = REXCVAR_GET(nfsmw_nativo_espera_regmem_us);
          rex::thread::Sleep(std::chrono::microseconds(pause_us > 50 ? pause_us : 50));
          ++turns;
        }
        /*
         * The guest has just waited for the GPU. It is the only point where it may legally rewrite
         * a vertex range it already referenced in this frame, so from here on upload deduplication
         * cannot reuse anything recorded so far. See native_vertex_dedupe.h. Relaxed is
         * enough: the same thread reads it.
         */
        g_ring_syncs.fetch_add(1, std::memory_order_relaxed);
        nfsmw::waits::Add(nfsmw::waits::kRegMemRing, NanosecondsFrom(regmem_start));
        const uint64_t wait_key = (uint64_t(info & 0x10) << 32) | poll;
        if (waits_.size() < 64 || waits_.count(wait_key)) {
          WaitStats& wait_stats = waits_[wait_key];
          ++wait_stats.times;
          wait_stats.turns += turns;
          wait_stats.reference = reference;
          wait_stats.mask = mask;
          wait_stats.info = info;
        }
        break;
      }
      case xenos::PM4_REG_RMW: {
        if (word_data < 3) break;
        const uint32_t info = payload.Read();
        const uint32_t y = payload.Read();
        const uint32_t o = payload.Read();
        uint32_t value = Reg(info & 0x1FFF);
        value &= ((info >> 31) & 0x1) ? Reg(y & 0x1FFF) : y;
        value |= ((info >> 30) & 0x1) ? Reg(o & 0x1FFF) : o;
        WriteReg(info & 0x1FFF, value);
        break;
      }
      case xenos::PM4_REG_TO_MEM: {
        if (word_data < 2) break;
        const uint32_t reg_index = payload.Read();
        const uint32_t address = payload.Read();
        WriteMemory(address, Reg(reg_index));
        break;
      }
      case xenos::PM4_MEM_WRITE: {
        if (word_data < 1) break;
        TrackCopyFence();  // Measurement only
        uint32_t address = payload.Read();
        for (uint32_t i = 1; i < word_data; ++i) {
          WriteMemory(address, payload.Read());
          address += 4;
        }
        break;
      }
      case xenos::PM4_COND_WRITE: {
        if (word_data < 6) break;
        const uint32_t info = payload.Read();
        const uint32_t poll = payload.Read();
        const uint32_t reference = payload.Read();
        const uint32_t mask = payload.Read();
        const uint32_t target = payload.Read();
        const uint32_t datum = payload.Read();
        const uint32_t value = (info & 0x10) ? ReadMemory(poll) : Reg(poll);
        if (Compares(info, value & mask, reference)) {
          if (info & 0x100) {
            WriteMemory(target, datum);
          } else {
            WriteReg(target, datum);
          }
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE:
        if (word_data < 1) break;
        WriteReg(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, payload.Read() & 0x3F);
        break;
      case xenos::PM4_EVENT_WRITE_SHD: {
        if (word_data < 3) break;
        TrackCopyFence();  // Measurement only
        const uint32_t initiator = payload.Read();
        const uint32_t address = payload.Read();
        const uint32_t value = payload.Read();
        WriteReg(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        WriteMemory(address, ((initiator >> 31) & 0x1)
                                       ? counter_.load(std::memory_order_relaxed)
                                       : value);
        break;
      }
      case xenos::PM4_EVENT_WRITE_EXT: {
        if (word_data < 2) break;
        const uint32_t initiator = payload.Read();
        const uint32_t address = payload.Read();
        WriteReg(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        // Screen extent of the previous draw: the maximum is faked.
        const uint16_t extension[] = {0, kMaxExtension, 0, kMaxExtension, 0, 1};
        uint8_t* target = memory_->TranslatePhysical(address & ~uint32_t(0x3));
        for (size_t i = 0; i < std::size(extension); ++i) {
          rex::memory::store_and_swap<uint16_t>(target + i * 2, extension[i]);
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE_ZPD: {
        if (word_data < 1) break;
        WriteReg(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, payload.Read() & 0x3F);
        const uint32_t address = Reg(rex::graphics::XE_GPU_REG_RB_SAMPLE_COUNT_ADDR);
        if (!address) break;
        OcclusionQuery(address);
        break;
      }
      case xenos::PM4_SET_CONSTANT: {
        if (word_data < 1) break;
        const uint32_t index_type = payload.Read();
        uint32_t base = UINT32_MAX;
        switch ((index_type >> 16) & 0xFF) {
          case 0: base = 0x4000; break;  // ALU
          case 1: base = 0x4800; break;  // fetch
          case 2: base = 0x4900; break;  // bool
          case 3: base = 0x4908; break;  // loop
          case 4: base = 0x2000; break;  // registros
          default: break;
        }
        if (base != UINT32_MAX) {
          const uint32_t index = base + (index_type & 0x7FF);
          for (uint32_t i = 1; i < word_data; ++i) {
            WriteReg(index + i - 1, payload.Read());
          }
        }
        break;
      }
      case xenos::PM4_SET_BIN_MASK_LO:
        if (word_data < 1) break;
        bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | payload.Read();
        break;
      case xenos::PM4_SET_BIN_MASK_HI:
        if (word_data < 1) break;
        bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(payload.Read()) << 32);
        break;
      case xenos::PM4_SET_BIN_SELECT_LO:
        if (word_data < 1) break;
        bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | payload.Read();
        break;
      case xenos::PM4_SET_BIN_SELECT_HI:
        if (word_data < 1) break;
        bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(payload.Read()) << 32);
        break;
      case xenos::PM4_SET_BIN_MASK: {
        if (word_data < 2) break;
        const uint64_t height_px = payload.Read();
        const uint64_t low = payload.Read();
        bin_mask_ = (height_px << 32) | low;
        break;
      }
      case xenos::PM4_SET_BIN_SELECT: {
        if (word_data < 2) break;
        const uint64_t height_px = payload.Read();
        const uint64_t low = payload.Read();
        bin_select_ = (height_px << 32) | low;
        break;
      }
      case xenos::PM4_SET_CONSTANT2:
      case xenos::PM4_SET_SHADER_CONSTANTS: {
        // Same format: first value = register index, the rest is written consecutively.
        if (word_data < 1) break;
        const uint32_t index = payload.Read() & 0xFFFF;
        for (uint32_t i = 1; i < word_data; ++i) {
          WriteReg(index + i - 1, payload.Read());
        }
        break;
      }
      case xenos::PM4_LOAD_ALU_CONSTANT: {
        // Registers loaded from guest memory instead of from the ring.
        if (word_data < 3) break;
        const uint32_t address = payload.Read() & 0x3FFFFFFF;
        const uint32_t index_type = payload.Read();
        const uint32_t amount = payload.Read() & 0xFFF;
        uint32_t base = UINT32_MAX;
        switch ((index_type >> 16) & 0xFF) {
          case 0: base = 0x4000; break;  // ALU
          case 1: base = 0x4800; break;  // fetch
          case 2: base = 0x4900; break;  // bool
          case 3: base = 0x4908; break;  // loop
          case 4: base = 0x2000; break;  // registros
          default: break;
        }
        if (base != UINT32_MAX) {
          const uint8_t* src = memory_->TranslatePhysical(address);
          const uint32_t index = base + (index_type & 0x7FF);
          for (uint32_t i = 0; i < amount; ++i) {
            WriteReg(index + i, rex::memory::load_and_swap<uint32_t>(src + size_t(i) * 4));
          }
        }
        break;
      }
      case xenos::PM4_VIZ_QUERY: {
        // Like the emulated path: when the query ends it is reported as visible,
        // in case the game reads the result.
        if (word_data < 1) break;
        const uint32_t datum = payload.Read();
        const uint32_t id = datum & 0x3F;
        if (!(datum & 0x100)) {
          WriteReg(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, xenos::VIZQUERY_START);
        } else {
          WriteReg(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, xenos::VIZQUERY_END);
          const uint32_t reg_index = id < 32 ? uint32_t(rex::graphics::XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0)
                                            : uint32_t(rex::graphics::XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_1);
          const uint32_t bit = UINT32_C(1) << (id < 32 ? id : id - 32);
          SaveReg(reg_index, Reg(reg_index) | bit);
        }
        break;
      }
      case xenos::PM4_DRAW_INDX:
      case xenos::PM4_DRAW_INDX_2: {
        // DRAW_INDX is preceded by the visibility query word.
        const uint32_t skip_over = opcode == xenos::PM4_DRAW_INDX ? 1 : 0;
        if (word_data < skip_over + 1) break;
        if (skip_over) {
          payload.Read();
        }
        const uint32_t initiator = payload.Read();
        WriteReg(rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR, initiator);
        // Indices in a buffer (DMA): the base and size come in the packet and are
        // stored in their registers, like ExecutePacketType3Draw
        // (graphics/command_processor.cpp:1379-1390). Without this, indexed draws
        // read VGT_DMA_SIZE as zero.
        if (((initiator >> 6) & 0x3) == uint32_t(xenos::SourceSelect::kDMA)) {
          if (word_data < skip_over + 3) break;
          WriteReg(rex::graphics::XE_GPU_REG_VGT_DMA_BASE, payload.Read());
          WriteReg(rex::graphics::XE_GPU_REG_VGT_DMA_SIZE, payload.Read());
        }
        AddRingOnly(draws_);
        // No fetch_add, which on the A57 is an ldxr/stxr loop on every draw. Only this thread writes
        // g_dibujos, and the "[tiron]" line reads it from this same thread (Presentar): SumarSoloAnillo is
        // enough.
        AddRingOnly(nfsmw::waits::g_draws);
        // A draw with RB_MODECONTROL in copy mode is a resolve (and/or a
        // clear): the emulated path treats it that way (vulkan/command_processor.cpp:3653).
        if ((Reg(rex::graphics::XE_GPU_REG_RB_MODECONTROL) & 0x7) ==
            uint32_t(xenos::EdramMode::kCopy)) {
          AddRingOnly(copies_);
          last_target_copy_.store(Reg(rex::graphics::XE_GPU_REG_RB_COPY_DEST_BASE),
                                      std::memory_order_relaxed);
          last_control_copy_.store(Reg(rex::graphics::XE_GPU_REG_RB_COPY_CONTROL),
                                      std::memory_order_relaxed);
          last_info_copy_.store(Reg(rex::graphics::XE_GPU_REG_RB_COPY_DEST_INFO),
                                   std::memory_order_relaxed);
          last_pitch_copy_.store(Reg(rex::graphics::XE_GPU_REG_RB_COPY_DEST_PITCH),
                                    std::memory_order_relaxed);
          MakeCopy();
        } else {
          CountDrawShaders();
          MatchDraw();
          TraceDraw();
          // In mode 5 (depth only) the VS is enough: Xenos does not run the PS, and D3D leaves
          // the PS object at 0 in some of the race shadow draws.
          if (draw_vs_ &&
              (draw_ps_ || (Reg(rex::graphics::XE_GPU_REG_RB_MODECONTROL) & 0x7) == 5)) {
            DrawNative();
          }
        }
        break;
      }
      case xenos::PM4_IM_LOAD: {
        // Microcode in GPU memory; the type is in the two low bits.
        if (word_data < 2) break;
        const uint32_t type_address = payload.Read();
        const uint32_t byte_size = payload.Read() & 0xFFFF;
        const uint8_t* src = memory_->TranslatePhysical(type_address & ~uint32_t(0x3));
        if (byte_size && (type_address & 0x3) <= 1) {
          LoadCachedShader(type_address, byte_size, src);
          break;
        }
        microcode_.resize(byte_size);
        for (uint32_t i = 0; i < byte_size; ++i) {
          microcode_[i] = rex::memory::load_and_swap<uint32_t>(src + size_t(i) * 4);
        }
        IdentifyShader(type_address & 0x3);
        break;
      }
      case xenos::PM4_IM_LOAD_IMMEDIATE: {
        // Microcode inside the packet itself.
        if (word_data < 2) break;
        const uint32_t type = payload.Read();
        const uint32_t byte_size = payload.Read() & 0xFFFF;
        if (byte_size > word_data - 2) break;
        LoadImmediate(payload, type, byte_size);  // with an exact cache (nfsmw_nativo_im_inmediato_cache)
        break;
      }
      case xenos::PM4_NOP:
        // The FlushState composite marker (phase 2 of the Direct3D-level renderer).
        // A NOP without its magic belongs to the game itself and does nothing, as before.
        ProcessMarker(payload, word_data);
        break;
      default:
        // Invalidaciones y demas: todavia no dibuja.
        break;
    }
  }

  // Step C2: a draw in copy mode is a resolve and/or a clear.
  bool EnsureTargets() {
    if (!targets_ && !failed_targets_) {
      targets_ = NativeTargets::Make(provider_ ? provider_->vulkan_device() : nullptr, memory_);
      if (!targets_) {
        failed_targets_ = true;
        REXLOG_ERROR("[nativo] No se pudieron crear los destinos nativos (C2)");
      }
    }
    return targets_ != nullptr;
  }

  // Diagnostic nfsmw_nativo_diag_constantes_ps: the first constants of the requested PS, at most once
  // every nfsmw_nativo_diag_constantes_ms per PS (exposure and brightness of the visual treatment).
  void TrackPsConstants() {
    if (!draw_ps_) {
      return;
    }
    const std::string& item_list = REXCVAR_GET(nfsmw_nativo_diag_constantes_ps);
    if (item_list.empty()) {
      return;
    }
    if (item_list != text_list_constants_) {
      text_list_constants_ = item_list;
      list_constants_.clear();
      size_t start = 0;
      while (start <= item_list.size()) {
        const size_t stop_pos = std::min(item_list.find(',', start), item_list.size());
        if (stop_pos > start) {
          list_constants_.insert(uint32_t(std::strtoul(item_list.substr(start, stop_pos - start).c_str(), nullptr, 10)));
        }
        start = stop_pos + 1;
      }
    }
    if (!list_constants_.count(draw_ps_->number)) {
      return;
    }
    const auto now_time = Clock::now();
    auto& is_last = constants_last_[draw_ps_->number];
    if (now_time - is_last < std::chrono::milliseconds(REXCVAR_GET(nfsmw_nativo_diag_constantes_ms))) {
      return;
    }
    is_last = now_time;
    std::string values;
    for (uint32_t k = 0; k < 12; ++k) {
      values += fmt::format(" c{}=(", k);
      for (uint32_t c = 0; c < 4; ++c) {
        float f;
        const uint32_t bits = regs_[0x4400 + k * 4 + c];
        std::memcpy(&f, &bits, sizeof(f));
        values += fmt::format("{}{:.4g}", c ? "," : "", f);
      }
      values += ")";
    }
    REXLOG_INFO("[nativo] constantes PS n{} (Swap {}):{}", draw_ps_->number, swaps_.load(), values);
  }

  // Steps C3-C6: the draw, with its VS and PS identified, goes to Vulkan.
  /*
   * Measurement only. A fence or an interrupt the game can see: how many vertex copies are still
   * pending at that moment (the ring only waits for them before returning the read pointer). Ring
   * thread only.
   */
  /*
   * Measurement only. When the ring finishes a frame (its Swap, already counted in swaps_), how many
   * more Swaps the game has written: 0 or less = the ring is caught up; 1 = the next frame is already
   * entirely in the ring; 2 or more = two or more. The game counts after writing the Swap, so a
   * momentary -1 is normal. The first difference is recorded separately in case the two counters do
   * not start together. Ring thread only.
   */
  void TrackGameAhead() {
    const int64_t difference = int64_t(::g_nfsmw_game_frames.load(std::memory_order_relaxed)) -
                               int64_t(swaps_.load(std::memory_order_relaxed));
    if (!valid_first_ahead_game_) {
      valid_first_ahead_game_ = true;
      ahead_game_first_ = difference;
    }
    ++ahead_game_[difference <= 0 ? 0 : difference == 1 ? 1 : 2];
    ahead_game_max_ = std::max(ahead_game_max_, difference);
  }

  void TrackCopyFence() {
    ++fences_total_;
    if (targets_) {
      const size_t pending_items = targets_->PendingCopies();
      if (pending_items) {
        ++fences_with_copies_;
        copies_fences_max_ = std::max(copies_fences_max_, pending_items);
      }
    }
  }

  void DrawNative() {
    if (!EnsureTargets()) {
      return;
    }
    // AnotarConstantesPs used to read the text cvar on every draw, and REXCVAR_GET of a string
    // is a call with an initialization guard (a load with acquire) that cannot be folded. The
    // flag is refreshed once per Swap: the diagnostic takes at most one frame to turn on.
    if (diag_active_constants_) {
      TrackPsConstants();
    }
    DrawRequest req_info;
    req_info.regs = regs_.data();
    req_info.vs = draw_vs_;
    req_info.ps = draw_ps_;
    req_info.microcode_vs = microcode_vs_;
    req_info.generacion_vs = generacion_vs_;
    req_info.generacion_constantes_vs = generacion_constantes_vs_;
    req_info.generacion_constantes_ps = generacion_constantes_ps_;
    req_info.generacion_fetch = generacion_fetch_;
    req_info.framing_generation = framing_generation_;
    req_info.vegetation_game = vegetation_game_;  // nfsmw_d3d_vegetacion_juego
    // Timer on 1 of every kCronometroDibujosCada draws, with the time scaled up (as in Paquete).
    // The phase comes from a counter that is not reset on each report (dibujos_medidos_ is), offset to
    // the middle of the batch: counting from 1, draws 33, 97, 161... are timed here, and the stage timer
    // of DibujosVulkanImpl::Dibujar runs on 64, 128, 192... Those pay ~20 extra clock reads; previously,
    // after each report, 1 time in 8 they landed exactly on the draws timed here and inflated
    // "dibujos us/dibujo".
    if ((draws_stopwatch_phase_++ & (kDrawsStopwatchEvery - 1)) == kDrawsStopwatchEvery / 2) {
      const auto start = Clock::now();
      targets_->Draw(req_info);
      draws_time_ns_ += NanosecondsFrom(start) * kDrawsStopwatchEvery;
    } else {
      targets_->Draw(req_info);
    }
    ++measured_draws_;
  }

  void MakeCopy() {
    if (!EnsureTargets()) {
      return;
    }
    namespace g = rex::graphics;
    CopyRegisters r;
    r.rb_surface_info = Reg(g::XE_GPU_REG_RB_SURFACE_INFO);
    r.rb_color_info[0] = Reg(g::XE_GPU_REG_RB_COLOR_INFO);
    r.rb_color_info[1] = Reg(g::XE_GPU_REG_RB_COLOR1_INFO);
    r.rb_color_info[2] = Reg(g::XE_GPU_REG_RB_COLOR2_INFO);
    r.rb_color_info[3] = Reg(g::XE_GPU_REG_RB_COLOR3_INFO);
    r.rb_depth_info = Reg(g::XE_GPU_REG_RB_DEPTH_INFO);
    r.rb_copy_control = Reg(g::XE_GPU_REG_RB_COPY_CONTROL);
    r.rb_copy_dest_base = Reg(g::XE_GPU_REG_RB_COPY_DEST_BASE);
    r.rb_copy_dest_pitch = Reg(g::XE_GPU_REG_RB_COPY_DEST_PITCH);
    r.rb_copy_dest_info = Reg(g::XE_GPU_REG_RB_COPY_DEST_INFO);
    r.rb_color_clear = Reg(g::XE_GPU_REG_RB_COLOR_CLEAR);
    r.rb_color_clear_lo = Reg(g::XE_GPU_REG_RB_COLOR_CLEAR_LO);
    r.rb_depth_clear = Reg(g::XE_GPU_REG_RB_DEPTH_CLEAR);
    r.pa_sc_window_offset = Reg(g::XE_GPU_REG_PA_SC_WINDOW_OFFSET);
    r.pa_sc_window_scissor_tl = Reg(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL);
    r.pa_sc_window_scissor_br = Reg(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
    r.pa_su_sc_mode_cntl = Reg(g::XE_GPU_REG_PA_SU_SC_MODE_CNTL);
    r.pa_su_vtx_cntl = Reg(g::XE_GPU_REG_PA_SU_VTX_CNTL);
    r.fetch_vertices[0] = Reg(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0);
    r.fetch_vertices[1] = Reg(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_1);
    TraceCopy(r);
    const auto start = Clock::now();
    targets_->CopyBytes(r);
    copies_time_ns_ += NanosecondsFrom(start);
    ++measures_copies_;
  }

  // Diagnostic (nfsmw_nativo_diag_fotograma_s): every draw and every copy of one whole
  // frame, from the first Swap after that many seconds until the next one.
  void SwapTrace() {
    // Once per Swap, not once per draw (see DibujarNativo).
    diag_active_constants_ = !REXCVAR_GET(nfsmw_nativo_diag_constantes_ps).empty();
    if (tracing_) {
      tracing_ = false;
      done_trace_ = true;
      REXLOG_INFO("[traza] fin del fotograma: {} lineas", traces_);
      return;
    }
    const int32_t secs = REXCVAR_GET(nfsmw_nativo_diag_fotograma_s);
    if (!done_trace_ && secs > 0 &&
        Clock::now() - system_start_ >= std::chrono::seconds(secs)) {
      tracing_ = true;
      traces_ = 0;
      REXLOG_INFO("[traza] fotograma completo tras el Swap {}", swaps_.load());
    }
  }

  // One line per draw: shaders, render targets, state and the textures of the PS samplers
  // (address / format / dimension; ! if the fetch constant is not a texture).
  void TraceDraw() {
    if (!tracing_ || traces_ >= 4000) {
      return;
    }
    ++traces_;
    namespace g = rex::graphics;
    std::string textures;
    if (draw_ps_) {
      for (const SamplerShader& s : draw_ps_->samplers) {
        if (s.reg_index >= 16) {
          continue;
        }
        const uint32_t base = g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + uint32_t(s.reg_index) * 6;
        const uint32_t d0 = Reg(base);
        const uint32_t d1 = Reg(base + 1);
        const uint32_t d3 = Reg(base + 3);
        const uint32_t d5 = Reg(base + 5);
        textures += fmt::format(" t{}={:08X}/f{}/d{}/s{:03X}/e{}/x{}{}", s.reg_index, d1 & 0xFFFFF000,
                                d1 & 0x3F, (d5 >> 9) & 0x3, (d3 >> 1) & 0xFFF, (d1 >> 6) & 0x3,
                                int32_t(d3 << 13) >> 26, (d0 & 0x3) == 2 ? "" : "!");
      }
    }
    if (!draw_vs_ || !draw_ps_) {
      static constexpr const char* kReasons[] = {"registro con shaders desconocidos",
                                                 "sin registro",
                                                 "registro con shaders distintos del IM_LOAD"};
      textures += fmt::format(" | {}: objetos VS {:08X} PS {:08X}; IM_LOAD PS n{} VS n{}; pendientes:",
                              kReasons[matched_reason_ % 3], vs_object_, ps_object_,
                              current_ps_ ? int(current_ps_->number) : -1,
                              current_vs_ ? int(current_vs_->number) : -1);
      for (size_t i = 0; i < pending_.size() && i < 4; ++i) {
        const DrawRecord& pending = pending_[i];
        const ShaderEntry* vs = CachedShaderOfObject(pending.vs);
        const ShaderEntry* ps = CachedShaderOfObject(pending.ps);
        textures += fmt::format(" [funcion {} tipo {} cuenta {} VS n{} PS n{}]",
                                int(pending.func_id), pending.args[0],
                                RegCount(pending), vs ? int(vs->number) : -1,
                                ps ? int(ps->number) : -1);
      }
    }
    const uint32_t initiator = Reg(g::XE_GPU_REG_VGT_DRAW_INITIATOR);
    REXLOG_INFO("[traza] dibujo VS n{} PS n{} tipo {} cuenta {} sup {:08X} rt0 {:08X} rt1 {:08X} "
                "mascara {:08X} mezcla {:08X} colorctl {:08X} prof {:08X} stencil {:08X} "
                "modo {:08X} vte {:08X} clip {:08X} vport {:08X}/{:08X}/{:08X}/{:08X} zinfo {:08X} zvport {:08X}/{:08X} c255 {:08X}/{:08X} pc255 {:08X}{}",
                draw_vs_ ? int(draw_vs_->number) : -1, draw_ps_ ? int(draw_ps_->number) : -1,
                initiator & 0x3F, initiator >> 16, Reg(g::XE_GPU_REG_RB_SURFACE_INFO),
                Reg(g::XE_GPU_REG_RB_COLOR_INFO), Reg(g::XE_GPU_REG_RB_COLOR1_INFO),
                Reg(g::XE_GPU_REG_RB_COLOR_MASK), Reg(g::XE_GPU_REG_RB_BLENDCONTROL0),
                Reg(g::XE_GPU_REG_RB_COLORCONTROL), Reg(g::XE_GPU_REG_RB_DEPTHCONTROL),
                Reg(g::XE_GPU_REG_RB_STENCILREFMASK), Reg(g::XE_GPU_REG_RB_MODECONTROL),
                Reg(g::XE_GPU_REG_PA_CL_VTE_CNTL), Reg(g::XE_GPU_REG_PA_CL_CLIP_CNTL),
                Reg(g::XE_GPU_REG_PA_CL_VPORT_XSCALE), Reg(g::XE_GPU_REG_PA_CL_VPORT_XOFFSET),
                Reg(g::XE_GPU_REG_PA_CL_VPORT_YSCALE), Reg(g::XE_GPU_REG_PA_CL_VPORT_YOFFSET),
                Reg(g::XE_GPU_REG_RB_DEPTH_INFO), Reg(g::XE_GPU_REG_PA_CL_VPORT_ZSCALE),
                Reg(g::XE_GPU_REG_PA_CL_VPORT_ZOFFSET), Reg(g::XE_GPU_REG_SHADER_CONSTANT_000_X + 255 * 4),
                Reg(g::XE_GPU_REG_SHADER_CONSTANT_000_X + 255 * 4 + 1),
                Reg(g::XE_GPU_REG_SHADER_CONSTANT_000_X + (256 + 255) * 4), textures);
    TraceVertices(initiator);
  }

  // Diagnostic (nfsmw_nativo_diag_vertices_ps): the fetch of each VS element, original and
  // patched, the bytes of the draw's first vertices and the center texels of its 8888
  // textures, as they are in guest memory.
  void TraceVertices(uint32_t initiator) {
    namespace g = rex::graphics;
    if (!draw_vs_ || !draw_ps_ || traces_ >= 4000 ||
        microcode_vs_.size() != draw_vs_->ucode.size()) {
      return;
    }
    const std::string item_list = REXCVAR_GET(nfsmw_nativo_diag_vertices_ps);
    bool in_list = false;
    uint32_t value = 0;
    bool hay = false;
    for (char c : item_list + ",") {
      if (c >= '0' && c <= '9') {
        value = value * 10 + uint32_t(c - '0');
        hay = true;
      } else if (hay) {
        in_list = in_list || value == draw_ps_->number;
        value = 0;
        hay = false;
      }
    }
    if (!in_list) {
      return;
    }
    ++traces_;
    std::string detail;
    uint32_t ranura = UINT32_MAX;
    uint32_t stride_bytes = 0;
    for (const VertexElement& element : draw_vs_->elements) {
      const size_t i = size_t(element.instruction) * 3;
      const uint32_t o0 = draw_vs_->ucode[i], o1 = draw_vs_->ucode[i + 1];
      const uint32_t p0 = microcode_vs_[i], p1 = microcode_vs_[i + 1], p2 = microcode_vs_[i + 2];
      detail += fmt::format(" {}{}@{}: original r{} s{:03X} fmt{}; parcheado r{} op{} s{:03X} fmt{} "
                             "f{} z{} o{} ({:08X} {:08X} {:08X});",
                             UseName(element.use), element.use_index, element.instruction,
                             (o0 >> 12) & 0x3F, o1 & 0xFFF, (o1 >> 16) & 0x3F, (p0 >> 12) & 0x3F,
                             p0 & 0x1F, p1 & 0xFFF, (p1 >> 16) & 0x3F,
                             ((p0 >> 20) & 0x1F) * 3 + ((p0 >> 25) & 0x3), p2 & 0xFF,
                             int32_t(p2 << 1) >> 9, p0, p1, p2);
      if (ranura == UINT32_MAX && (p0 & 0x1F) == 0 && !((p1 >> 30) & 0x1)) {
        ranura = ((p0 >> 20) & 0x1F) * 3 + ((p0 >> 25) & 0x3);
        stride_bytes = (p2 & 0xFF) * 4;
      }
    }
    if (ranura < 96 && stride_bytes && stride_bytes <= 64) {
      const uint32_t f0 = Reg(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + ranura * 2);
      const uint32_t f1 = Reg(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + ranura * 2 + 1);
      const uint32_t src_select = (initiator >> 6) & 0x3;
      uint32_t first_item = Reg(g::XE_GPU_REG_VGT_INDX_OFFSET) & 0xFFFFFF;
      if (src_select == uint32_t(xenos::SourceSelect::kDMA)) {
        const bool de_32 = (initiator >> 11) & 0x1;
        const uint32_t base_indices =
            Reg(g::XE_GPU_REG_VGT_DMA_BASE) & (de_32 ? 0x1FFFFFFC : 0x1FFFFFFE);
        const auto indices_order =
            static_cast<xenos::Endian>(Reg(g::XE_GPU_REG_VGT_DMA_SIZE) >> 30);
        if (de_32) {
          uint32_t raw;
          std::memcpy(&raw, memory_->TranslatePhysical(base_indices), 4);
          first_item = (first_item + (xenos::GpuSwap(raw, indices_order) & 0xFFFFFF)) & 0xFFFFFF;
        } else {
          uint16_t raw;
          std::memcpy(&raw, memory_->TranslatePhysical(base_indices), 2);
          first_item = (first_item + xenos::GpuSwap(raw, indices_order)) & 0xFFFFFF;
        }
      }
      const uint64_t address = uint64_t(f0 & 0x1FFFFFFC) + uint64_t(first_item) * stride_bytes;
      std::string bytes;
      if (address + uint64_t(4) * stride_bytes <= 0x20000000) {
        const uint8_t* payload = memory_->TranslatePhysical(uint32_t(address));
        for (uint32_t b = 0; b < 4 * stride_bytes; ++b) {
          bytes += fmt::format("{}{:02X}", b % stride_bytes == 0 ? " | " : (b % 4 == 0 ? " " : ""),
                               payload[b]);
        }
      }
      detail += fmt::format(" fetch f{} {:08X} {:08X} (orden {}) fuente {} primero {} zancada {}:{}",
                             ranura, f0, f1, f1 & 0x3, src_select, first_item, stride_bytes, bytes);
    }
    for (const SamplerShader& s : draw_ps_->samplers) {
      if (s.reg_index >= 16) {
        continue;
      }
      const uint32_t base = g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + uint32_t(s.reg_index) * 6;
      const uint32_t t0 = Reg(base), t1 = Reg(base + 1), t2 = Reg(base + 2);
      if ((t0 & 0x3) != 2 || (t1 & 0x3F) != 6) {
        continue;
      }
      const uint32_t width_px = (t2 & 0x1FFF) + 1, height_px = ((t2 >> 13) & 0x1FFF) + 1;
      const uint32_t pitch = std::max<uint32_t>(((t0 >> 22) & 0x1FF) << 5, 1);
      const bool tile = (t0 >> 31) & 0x1;
      const uint32_t y = height_px / 2;
      std::string texels;
      for (uint32_t k = 0; k < 8; ++k) {
        const uint32_t x = std::min(width_px - 1, (width_px > 8 ? width_px / 2 - 4 : 0) + k);
        const int64_t byte_offset = tile ? DiagTileOffset(int32_t(x), int32_t(y), pitch, 2)
                                               : int64_t(y) * pitch * 4 + int64_t(x) * 4;
        const uint64_t d = uint64_t(t1 & 0x1FFFF000) + uint64_t(byte_offset);
        if (byte_offset < 0 || d + 4 > 0x20000000) {
          break;
        }
        const uint8_t* texel = memory_->TranslatePhysical(uint32_t(d));
        texels += fmt::format(" {:02X}{:02X}{:02X}{:02X}", texel[0], texel[1], texel[2], texel[3]);
      }
      detail += fmt::format("; t{} {}x{} {} pitch {} swizzle {:03X} orden {} signos {:02X} fila {}:{}",
                             s.reg_index, width_px, height_px, tile ? "en mosaico" : "lineal", pitch,
                             (Reg(base + 3) >> 1) & 0xFFF, (t1 >> 6) & 0x3, (t0 >> 2) & 0xFF,
                             y, texels);
      const uint32_t texture_address = t1 & 0x1FFFF000;
      if (dumped_textures_.size() < 8 && dumped_textures_.insert(texture_address).second) {
        std::vector<uint8_t> dump(size_t(width_px) * height_px * 4);
        bool complete = true;
        for (uint32_t yy = 0; yy < height_px && complete; ++yy) {
          for (uint32_t xx = 0; xx < width_px; ++xx) {
            const int64_t dt = tile ? DiagTileOffset(int32_t(xx), int32_t(yy), pitch, 2)
                                       : int64_t(yy) * pitch * 4 + int64_t(xx) * 4;
            const uint64_t texel = uint64_t(texture_address) + uint64_t(dt);
            if (dt < 0 || texel + 4 > 0x20000000) {
              complete = false;
              break;
            }
            std::memcpy(dump.data() + (size_t(yy) * width_px + xx) * 4,
                        memory_->TranslatePhysical(uint32_t(texel)), 4);
          }
        }
        const auto file_path = rex::filesystem::GetExecutableFolder() /
                          fmt::format("diag_textura_{:08X}_{}x{}.bin", texture_address, width_px, height_px);
        std::ofstream file_name(file_path, std::ios::binary | std::ios::trunc);
        file_name.write(reinterpret_cast<const char*>(dump.data()), std::streamsize(dump.size()));
        detail += fmt::format(" (volcada en {}{})", file_path.filename().string(),
                               complete ? "" : ", incompleta");
      }
    }
    detail += "; constantes VS";
    for (uint32_t c = 0; c < 16; ++c) {
      const uint32_t b = 0x4000 + c * 4;
      detail += fmt::format(" c{}=({:.5g} {:.5g} {:.5g} {:.5g})", c, std::bit_cast<float>(Reg(b)),
                             std::bit_cast<float>(Reg(b + 1)), std::bit_cast<float>(Reg(b + 2)),
                             std::bit_cast<float>(Reg(b + 3)));
    }
    detail += "; constantes PS";
    for (uint32_t c = 0; c < 4; ++c) {
      const uint32_t b = 0x4400 + c * 4;
      detail += fmt::format(" c{}=({:.5g} {:.5g} {:.5g} {:.5g})", c, std::bit_cast<float>(Reg(b)),
                             std::bit_cast<float>(Reg(b + 1)), std::bit_cast<float>(Reg(b + 2)),
                             std::bit_cast<float>(Reg(b + 3)));
    }
    REXLOG_INFO("[traza] vertices PS n{} VS n{}:{}", draw_ps_->number, draw_vs_->number,
                detail);
  }

  // GetTiledOffset2D (graphics/pipeline/texture/util.cpp), same as DesplazamientoMosaico2D in
  // nfsmw_nativo_dibujos.cpp; here only for the diagnostic. pitch in blocks.
  static int32_t DiagTileOffset(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bytes) {
    pitch = (pitch + 31) & ~uint32_t(31);
    const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bytes + 7);
    const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bytes;
    const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
    return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
           (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
  }

  void TraceCopy(const CopyRegisters& r) {
    if (!tracing_ || traces_ >= 4000) {
      return;
    }
    ++traces_;
    REXLOG_INFO("[traza] copia control {:08X} destino {:08X} info {:08X} pitch {:08X} sup {:08X} "
                "rt0 {:08X} prof {:08X} borrado {:08X}/{:08X} prof_borrado {:08X}",
                r.rb_copy_control, r.rb_copy_dest_base, r.rb_copy_dest_info, r.rb_copy_dest_pitch,
                r.rb_surface_info, r.rb_color_info[0], r.rb_depth_info, r.rb_color_clear,
                r.rb_color_clear_lo, r.rb_depth_clear);
  }

  // Step C5a: the last VS (type 0) or PS (type 1) uploaded to the ring.
  // IM_LOAD with a cache. The game reloads the same shaders on almost every draw: in a race on the PC,
  // 70,000-97,000 loads per second of about 82 distinct microcodes. Each load byte-swapped the data,
  // computed the XXH3 to identify it and copied the VS, and each new VS then repeated the XXH3 in
  // HuellaVs and in AnotarEntradaVertices. If the address, size and bytes match an earlier load, its
  // swapped copy, identification, fingerprint and generation are reused. The bytes must be compared:
  // D3D patches VS microcode in place, so the address alone is not enough. Same microcode, same
  // generation: whatever is cached per generation (vertex input, fingerprint, coherence) stays valid.
  //
  // IM_LOAD without memcmp (nfsmw_nativo_im_load_sin_memcmp). The memcmp above cost ~0.6 us per IM_LOAD
  // (~41,000 per second in a race). Whoever writes microcode announces it (the D3D constructors and the
  // fetch patcher; the list and the cross-thread protocol are in microcode_versions.h). Each way
  // records the versions of its slot it was checked against guest memory with; if they are unchanged,
  // nobody has written that microcode since, and it is used without reading it.
  // Self-checking guard:
  //   - watching: always the regular memcmp and, if the shortcut had a candidate, it is compared with the
  //     memcmp result. After kImAtajoAMirar agreements, no disagreement and at least one in-place patch
  //     seen (proof that the sub_825A2FB8 hook runs: without it, a patch would not bump any version), it
  //     moves to applying;
  //   - applying: the candidate is used without memcmp, except 1 in every kImAtajoComprobarCada, which is
  //     still compared;
  //   - a disagreement (the candidate no longer matches memory) or a patcher call that is neither of the
  //     two known ones: "DIFERENCIA" in the log and the shortcut turned off for the run. That load and
  //     the following ones use memcmp.
  void LoadCachedShader(uint32_t type_address, uint32_t byte_size, const uint8_t* src) {
    const uint32_t type = type_address & 0x3;
    const size_t bytes = size_t(byte_size) * sizeof(uint32_t);
    auto& paths = cache_loads_[type][size_t((uint64_t(type_address) * 0x9E3779B97F4A7C15ull) >> 57)];
    if (im_phase_shortcut_ == kImShortcutWithoutStart) {
      StartShortcutIm();
    }
    const uint32_t ranura = ucode::RanuraDe(type_address & ~uint32_t(0x3));
    // 1. The shortcut: a way with the same contents, checked with the current versions.
    ShaderLoad* candidate = nullptr;
    if (im_phase_shortcut_ != kImShortcutDisabled) {
      const uint32_t global = ucode::RelaxedGlobal();
      for (ShaderLoad& via : paths) {
        if (!via.validated || via.type_address != type_address || via.byte_size != byte_size) {
          continue;
        }
        if (via.version_global != global) {
          // Someone has written microcode since it was checked: if not in its slot, it is still valid.
          if (via.slot_version != ucode::StartAfterGlobal(ranura)) {
            via.validated = false;
            ++im_i_changed_version_;
            continue;
          }
          if (ucode::g_other_patches.load(std::memory_order_relaxed) != 0) {
            DisableShortcutImPerOther();
            break;
          }
          via.version_global = global;
        }
        candidate = &via;
        break;
      }
    }
    ShaderLoad* load_op = nullptr;
    if (candidate && im_phase_shortcut_ == kImApplyingShortcut &&
        (++im_turn_shortcut_ & (kImCheckShortcutEvery - 1)) != 0) {
      // 2. Without reading the guest microcode.
      load_op = candidate;
      ++cached_loads_;
      ++im_i_without_memcmp_;
    } else {
      // 3. The regular path, with the versions read before and after reading guest memory.
      const bool validate = im_phase_shortcut_ != kImShortcutDisabled;
      ucode::ReadInfo readback;
      if (validate) {
        readback = ucode::BeforeRead(ranura);
      }
      for (ShaderLoad& via : paths) {
        if (via.type_address == type_address && via.byte_size == byte_size &&
            std::memcmp(via.raw.data(), src, bytes) == 0) {
          load_op = &via;
          break;
        }
      }
      if (validate && candidate) {
        if (load_op == candidate) {
          ++im_checked_shortcut_;
          ++im_i_checked_;
          if (im_phase_shortcut_ == kImPeekingShortcut && im_checked_shortcut_ >= kImShortcutToPeek &&
              ucode::g_patches_in_place_.load(std::memory_order_relaxed) != 0) {
            im_phase_shortcut_ = kImApplyingShortcut;
            NFSMW_RING_REPORT("[nativo] C5a IM_LOAD sin memcmp: {} cargas comprobadas contra el memcmp, 0 "
                                 "desacuerdos y {} parches en su sitio vistos: APLICANDO, el memcmp se salta si la "
                                 "version no ha cambiado (1 de cada {} se sigue comprobando)",
                                 im_checked_shortcut_,
                                 ucode::g_patches_in_place_.load(std::memory_order_relaxed),
                                 kImCheckShortcutEvery);
          }
        } else {
          DisableShortcutIm(type_address, byte_size, candidate->raw, candidate->slot_version,
                        candidate->version_global, load_op != nullptr, src, readback);
        }
      }
      if (load_op) {
        ++cached_loads_;
      } else {
        load_op = paths[0].use <= paths[1].use ? &paths[0] : &paths[1];
        load_op->type_address = type_address;
        load_op->byte_size = byte_size;
        load_op->raw.resize(byte_size);
        std::memcpy(load_op->raw.data(), src, bytes);
        load_op->host.resize(byte_size);
        // The fresh copy is byte-swapped without reading game memory again: raw and host hold the same
        // contents, the ones the versions certify (with the guest idle, the same as before).
        const uint8_t* copy_info = reinterpret_cast<const uint8_t*>(load_op->raw.data());
        for (uint32_t i = 0; i < byte_size; ++i) {
          load_op->host[i] = rex::memory::load_and_swap<uint32_t>(copy_info + size_t(i) * 4);
        }
        load_op->entry = shaders_.loaded() ? shaders_.Identify(type == 0, load_op->host) : nullptr;
        load_op->fingerprint = XXH3_64bits(load_op->host.data(), bytes);
        load_op->generacion = ++microcode_generations_;
        load_op->validated = false;
      }
      if (validate && im_phase_shortcut_ != kImShortcutDisabled) {
        ++im_i_con_memcmp_;
        if (ucode::AfterRead(ranura, readback)) {
          load_op->validated = true;
          load_op->slot_version = readback.start;
          load_op->version_global = readback.global;
        } else {
          load_op->validated = false;  // someone wrote to its slot during the read: it will be compared again
          ++im_i_overridden_;
        }
      }
    }
    load_op->use = ++tic_loads_;
    (type == 0 ? current_vs_ : current_ps_) = load_op->entry;
    if (tracing_ && traces_ < 4000) {
      ++traces_;
      REXLOG_INFO("[traza] IM_LOAD {} n{} ({} palabras, huella {:016X})", type == 0 ? "VS" : "PS",
                  load_op->entry ? int(load_op->entry->number) : -1, load_op->host.size(), load_op->fingerprint);
    }
    if (type == 0) {
      microcode_vs_ = load_op->host;  // patched: the vertex input comes from here
      generacion_vs_ = load_op->generacion;
      generation_vs_fingerprint_ = load_op->generacion;
      vs_fingerprint_ = load_op->fingerprint;
    }
  }

  // IM_LOAD without memcmp. The first load reads the cvar.
  void StartShortcutIm() {
    const bool request = REXCVAR_GET(nfsmw_nativo_im_load_sin_memcmp);
    im_phase_shortcut_ = request && ucode::kWatchedCreations ? kImPeekingShortcut : kImShortcutDisabled;
    NFSMW_RING_REPORT("[nativo] C5a IM_LOAD sin memcmp (nfsmw_nativo_im_load_sin_memcmp) = {}",
                         !request ? "no (apagado con el cvar): memcmp en cada IM_LOAD"
                         : !ucode::kWatchedCreations
                             ? "no: con NFSMW_NATIVE_SHADER_LIBRARY los constructores no avisan"
                             : "SI, mirando: cada carga se compara tambien con memcmp hasta tener pruebas");
  }

  const char* ImShortcutPhaseName() const {
    switch (im_phase_shortcut_) {
      case kImPeekingShortcut:
        return "mirando";
      case kImApplyingShortcut:
        return "aplicando";
      case kImShortcutDisabled:
        return "APAGADA";
      default:
        return "sin empezar";
    }
  }

  // The shortcut's candidate does not match guest memory: someone wrote microcode without announcing it.
  // (Its fields are passed separately: CargaShader is declared further down and cannot appear in the
  // signature.)
  void DisableShortcutIm(uint32_t type_address, uint32_t byte_size, const std::vector<uint32_t>& raw,
                     uint32_t slot_version, uint32_t version_global, bool other_via, const uint8_t* src,
                     const ucode::ReadInfo& readback) {
    uint32_t is_first = 0;
    while (is_first < byte_size && std::memcmp(raw.data() + is_first, src + size_t(is_first) * 4, 4) == 0) {
      ++is_first;
    }
    REXLOG_ERROR("[nativo] C5a IM_LOAD sin memcmp: DIFERENCIA. El atajo daba la copia de {} {:08X} ({} palabras) "
                 "comprobada con las versiones ranura {} y global {}, y la memoria del invitado ya es otra (primera "
                 "palabra distinta: {}; el memcmp {}). Versiones de ahora: inicio {} fin {} global {}; fase {} con {} "
                 "cargas comprobadas. Escrituras avisadas: {} parches en su sitio, {} en la copia del anillo, {} "
                 "otros, {} creaciones. Alguien escribe microcodigo sin avisar: atajo APAGADO para el resto de la "
                 "sesion (todas las cargas con memcmp, como antes de la 184)",
                 (type_address & 0x3) == 0 ? "VS" : "PS", type_address & ~uint32_t(0x3), byte_size,
                 slot_version, version_global, is_first,
                 other_via ? "encuentra otra via" : "tiene que volver a copiarlo", readback.start, readback.stop_pos,
                 readback.global, ImShortcutPhaseName(), im_checked_shortcut_,
                 ucode::g_patches_in_place_.load(std::memory_order_relaxed),
                 ucode::g_patches_in_copy.load(std::memory_order_relaxed),
                 ucode::g_other_patches.load(std::memory_order_relaxed),
                 ucode::g_creations.load(std::memory_order_relaxed));
    im_phase_shortcut_ = kImShortcutDisabled;
  }

  // The fetch patcher was called from outside its two known call sites.
  void DisableShortcutImPerOther() {
    REXLOG_ERROR("[nativo] C5a IM_LOAD sin memcmp: DIFERENCIA. El parcheador de los fetch (sub_825A2FB8) se ha "
                 "llamado {} veces desde fuera de sus dos llamadas conocidas (retorno {:08X}, destino {:08X}): puede "
                 "escribir microcodigo de otra forma. Atajo APAGADO para el resto de la sesion (todas las cargas con "
                 "memcmp, como antes de la 184)",
                 ucode::g_other_patches.load(std::memory_order_relaxed),
                 ucode::g_other_return.load(std::memory_order_relaxed),
                 ucode::g_other_target.load(std::memory_order_relaxed));
    im_phase_shortcut_ = kImShortcutDisabled;
  }

  // IM_LOAD_IMMEDIATE (and unusual IM_LOAD loads): no cache.
  void IdentifyShader(uint32_t type) {
    if (type > 1) {
      return;
    }
    const ShaderEntry* entry =
        shaders_.loaded() ? shaders_.Identify(type == 0, microcode_) : nullptr;
    (type == 0 ? current_vs_ : current_ps_) = entry;
    if (tracing_ && traces_ < 4000) {
      ++traces_;
      REXLOG_INFO("[traza] IM_LOAD {} n{} ({} palabras, huella {:016X})", type == 0 ? "VS" : "PS",
                  entry ? int(entry->number) : -1, microcode_.size(),
                  XXH3_64bits(microcode_.data(), microcode_.size() * sizeof(uint32_t)));
    }
    if (type == 0) {
      immediate_vs_ = microcode_;  // patched: the vertex input comes from here
      microcode_vs_ = immediate_vs_;
      generacion_vs_ = ++microcode_generations_;
    }
  }

  // IM_LOAD_IMMEDIATE with an exact cache (nfsmw_nativo_im_inmediato_cache).
  //
  // Who sends them: sub_825A37D8, the D3D copy path. If the GPU has not yet passed the fence of a VS's
  // last IM_LOAD, or outputs the PS does not read must be disabled (sub_825A36A8), it copies the microcode
  // at [VS+40] ([VS+600] bytes) into the ring itself and patches it there (sub_825A2FB8 on the copy).
  // Always type 0 (VS).
  // In a race there are stretches with 4,000-8,000 per second at 1.5-2.8 us each ("tiempos por
  // paquete", op 2B): the regular path byte-swaps word by word, computes the XXH3 in Identificar, copies
  // the VS to vs_inmediato_ and gives it a new generation, so the next draw redoes the fingerprint
  // (another XXH3 in HuellaVs), the fetch coherence and the vertex input key. And the contents repeat:
  // over a whole run there are 82 distinct microcodes between IM_LOAD and immediate loads.
  //   - Set chosen by a cheap signature (size and four words, not swapped). Within the set, the way with
  //     the same signature, the same size and the same bytes (memcmp against its raw copy): equality is
  //     exact; the signature only chooses where to look.
  //   - Hit: the way's swapped copy, identification, fingerprint and generation. Same microcode, same
  //     generation, as in the IM_LOAD cache (CargarShaderCacheado): whatever is cached per generation
  //     stays valid.
  //   - Miss, or a packet that wraps around the ring (not contiguous): the regular path; the miss is
  //     stored.
  // Self-checking guard:
  //   - watching: every load takes the regular path and, if the cache had a way, it is compared with what
  //     the cache would give (swapped copy and identification). After kInmAMirar agreements and no
  //     disagreement it moves to applying;
  //   - applying: hits neither swap nor identify; 1 in every kInmComprobarCada is still compared;
  //   - a disagreement, or a new way whose swapped raw copy is not what the regular path read (the ring
  //     position arithmetic): "DIFERENCIA" in the log and the cache turned off for the run. That load
  //     already uses the regular result.
  struct ImmediateLoad {
    uint64_t signature = 0;  // 0 = via vacia
    uint32_t byte_size = 0;
    std::vector<uint32_t> raw;  // as it is in the ring (big-endian)
    std::vector<uint32_t> host;   // swapped: what gets identified and what the vertex input reads
    const ShaderEntry* entry = nullptr;
    uint64_t fingerprint = 0;      // host XXH3, the same one HuellaVs would compute
    uint64_t generacion = 0;  // from generaciones_microcodigo_, when this content was stored
    uint64_t use = 0;         // the least recently used way is the one replaced
  };

  void LoadImmediate(Reader payload, uint32_t type, uint32_t byte_size) {
    if (imm_phase_ == kImmWithoutStart) {
      StartImmediate();
    }
    const bool lap = payload.mask != 0 && size_t(payload.pos) + byte_size > size_t(payload.mask) + 1;
    if (imm_phase_ == kImmDisabled || type > 1 || byte_size == 0 || lap) {
      if (lap) {
        ++imm_i_return_;
      } else {
        ++imm_i_without_cache_;
      }
      LoadImmediateWithoutCache(payload, type, byte_size);
      return;
    }
    const uint8_t* raw = payload.base + size_t(payload.pos) * 4;
    const size_t bytes = size_t(byte_size) * sizeof(uint32_t);
    const uint64_t signature = ImmediateSignature(raw, byte_size);
    const size_t index = size_t(signature >> (64 - kImmSetBits));
    auto& desc_set = imm_cache_[type][index];
    uint8_t& last_level = imm_last_[type][index];
    ImmediateLoad* via = nullptr;
    for (uint32_t k = 0; k < kImmPaths; ++k) {  // first the way of the set's last hit
      const uint32_t v = (last_level + k) % kImmPaths;
      ImmediateLoad& candidate_item = desc_set[v];
      if (candidate_item.signature == signature && candidate_item.byte_size == byte_size &&
          std::memcmp(candidate_item.raw.data(), raw, bytes) == 0) {
        via = &candidate_item;
        last_level = uint8_t(v);
        break;
      }
    }
    if (via && imm_phase_ == kImmApplying && (++imm_turn_ & (kImmCheckEvery - 1)) != 0) {
      // Hit: no byte swap, no identification, no copy, no new generation.
      via->use = ++inm_tic_;
      UseImmediate(type, *via);
      ++imm_i_hits_;
      ++cached_loads_;
      if (tracing_ && traces_ < 4000) {
        ++traces_;
        REXLOG_INFO("[traza] IM_LOAD {} n{} ({} palabras, huella {:016X}; IM_LOAD_IMMEDIATE de la cache)",
                    type == 0 ? "VS" : "PS", via->entry ? int(via->entry->number) : -1, via->host.size(),
                    via->fingerprint);
      }
      return;
    }
    // The regular path: swaps, identifies and leaves its result in use
    // (vs_actual_ or ps_actual_, vs_inmediato_...).
    LoadImmediateWithoutCache(payload, type, byte_size);
    const ShaderEntry* entry = type == 0 ? current_vs_ : current_ps_;
    if (via) {
      // Check: what the cache would have given against what the regular path just gave.
      uint32_t is_first = 0;
      while (is_first < byte_size && is_first < via->host.size() && via->host[is_first] == microcode_[is_first]) {
        ++is_first;
      }
      if (is_first != byte_size || via->host.size() != byte_size || via->entry != entry) {
        DisableImmediate("un acierto de la cache no da lo mismo que el camino de siempre", type, byte_size, signature, is_first,
                        via->entry, entry);
        return;
      }
      ++imm_checked_;
      ++imm_i_checked_;
      via->use = ++inm_tic_;
      if (imm_phase_ == kImmPeeking && imm_checked_ >= kImmToPeek) {
        imm_phase_ = kImmApplying;
        NFSMW_RING_REPORT("[nativo] C5a IM_LOAD_IMMEDIATE con cache: {} aciertos comprobados contra el camino de "
                             "siempre y 0 desacuerdos: APLICANDO, los aciertos ya no giran ni identifican (1 de "
                             "cada {} se sigue comprobando)",
                             imm_checked_, kImmCheckEvery);
      }
      if (imm_phase_ == kImmApplying) {
        UseImmediate(type, *via);  // the same generation as the unchecked hits
      }
      return;
    }
    // Miss: into the least recently used way of the set.
    ++imm_i_failures_;
    uint32_t room = 0;
    for (uint32_t k = 1; k < kImmPaths; ++k) {
      if (desc_set[k].use < desc_set[room].use) {
        room = k;
      }
    }
    ImmediateLoad& incoming = desc_set[room];
    incoming.signature = signature;
    incoming.byte_size = byte_size;
    incoming.raw.resize(byte_size);
    std::memcpy(incoming.raw.data(), raw, bytes);
    incoming.host.assign(microcode_.begin(), microcode_.end());
    incoming.entry = entry;
    incoming.fingerprint = XXH3_64bits(incoming.host.data(), bytes);
    incoming.generacion = ++microcode_generations_;
    incoming.use = ++inm_tic_;
    last_level = uint8_t(room);
    // The raw copy comes from the packet's position in the ring: once swapped it must equal what the
    // regular path read word by word. Only on misses (a few dozen per run).
    const uint8_t* copy_info = reinterpret_cast<const uint8_t*>(incoming.raw.data());
    for (uint32_t i = 0; i < byte_size; ++i) {
      if (rex::memory::load_and_swap<uint32_t>(copy_info + size_t(i) * 4) != incoming.host[i]) {
        incoming.signature = 0;  // empty way: cannot come out again
        incoming.use = 0;
        DisableImmediate("la copia cruda de una via nueva no es lo que leyo el camino de siempre", type, byte_size, signature,
                        i, entry, entry);
        return;
      }
    }
    if (imm_phase_ == kImmApplying) {
      UseImmediate(type, incoming);
    }
  }

  // The regular IM_LOAD_IMMEDIATE path (formerly inline in PaqueteTipo3).
  void LoadImmediateWithoutCache(Reader payload, uint32_t type, uint32_t byte_size) {
    microcode_.resize(byte_size);
    for (uint32_t i = 0; i < byte_size; ++i) {
      microcode_[i] = payload.Read();
    }
    IdentifyShader(type);
  }

  // The same state IdentificarShader leaves, taken from the way (without swapping, identifying or
  // hashing).
  void UseImmediate(uint32_t type, const ImmediateLoad& via) {
    (type == 0 ? current_vs_ : current_ps_) = via.entry;
    if (type == 0) {
      microcode_vs_ = via.host;  // patched: the vertex input comes from here
      generacion_vs_ = via.generacion;
      generation_vs_fingerprint_ = via.generacion;
      vs_fingerprint_ = via.fingerprint;
    }
  }

  // Cheap signature of an unswapped packet: picks the set and rejects other shaders' ways before the
  // memcmp.
  static uint64_t ImmediateSignature(const uint8_t* raw, uint32_t byte_size) {
    const size_t positions[4] = {0, size_t(byte_size) / 3, size_t(byte_size) * 2 / 3, size_t(byte_size) - 1};
    uint64_t h = uint64_t(byte_size) * 0x9E3779B97F4A7C15ull;
    for (const size_t p : positions) {
      uint32_t word_value;
      std::memcpy(&word_value, raw + p * 4, sizeof(word_value));
      h = (h ^ word_value) * 0xBF58476D1CE4E5B9ull;
      h ^= h >> 29;
    }
    return h | 1;  // never 0: 0 means an empty way
  }

  void StartImmediate() {
    const bool request = REXCVAR_GET(nfsmw_nativo_im_inmediato_cache);
    imm_phase_ = request ? kImmPeeking : kImmDisabled;
    NFSMW_RING_REPORT("[nativo] C5a IM_LOAD_IMMEDIATE con cache (nfsmw_nativo_im_inmediato_cache) = {}",
                         request ? "SI, mirando: cada acierto se compara con el camino de siempre hasta tener pruebas"
                                : "no (apagado con el cvar): cada IM_LOAD_IMMEDIATE se gira e identifica, como antes");
  }

  const char* ImmediatePhaseName() const {
    switch (imm_phase_) {
      case kImmPeeking:
        return "mirando";
      case kImmApplying:
        return "aplicando";
      case kImmDisabled:
        return "APAGADA";
      default:
        return "sin empezar";
    }
  }

  // The cache does not match the regular path: it is turned off for the rest of the run.
  void DisableImmediate(const char* reason, uint32_t type, uint32_t byte_size, uint64_t signature, uint32_t is_first,
                       const ShaderEntry* from_cache, const ShaderEntry* as_always) {
    REXLOG_ERROR("[nativo] C5a IM_LOAD_IMMEDIATE con cache: DIFERENCIA, {}: {} de {} palabras (firma {:016X}), "
                 "primera palabra distinta {}, shader de la cache n{} y del camino de siempre n{}; fase {} con {} "
                 "aciertos comprobados. Cache APAGADA para el resto de la sesion (todo por el camino de siempre, "
                 "como antes de la 184); esta carga ya va con el camino de siempre",
                 reason, type == 0 ? "VS" : "PS", byte_size, signature, is_first,
                 from_cache ? int(from_cache->number) : -1, as_always ? int(as_always->number) : -1,
                 ImmediatePhaseName(), imm_checked_);
    imm_phase_ = kImmDisabled;
  }

  void CountDrawShaders() {
    if (!shaders_.loaded()) {
      return;
    }
    if (!current_vs_) {
      ++draws_without_vs_;
    } else if (!current_ps_) {
      ++draws_without_ps_;
    } else {
      ++identified_draws_;
    }
    const uint64_t par = (uint64_t(current_vs_ ? current_vs_->number + 1 : 0) << 32) |
                         (current_ps_ ? current_ps_->number + 1 : 0);
    // In a race there are 40 distinct pairs and ~2,200 draws per frame, mostly runs of the same
    // material. Remembering the previous pair avoids the set insert (a hash and a possible cache
    // miss) on every draw.
    if (par != last_shaders_pair_) {
      last_shaders_pair_ = par;
      if (pairs_.size() < 4096) {
        pairs_.insert(par);
      }
    }
  }

  // Step C5b: the game thread's Draw* record that corresponds to this ring draw.
  // They arrive in the same order; the primitive type and count are checked, and
  // at most 8 records without a draw are skipped.
  // Phase 2b: if the last marker carries its Draw* record and this draw accepts it (the usual acceptance:
  // type, count and shaders consistent with the IM_LOAD packets), apply mode uses it without the queue or
  // the search. If it is not accepted, it is kept for the next draw (like the head of the queue) and the
  // usual search runs. In check mode the usual search runs and the result of each path is compared
  // (CompararDibujoMarcador).
  void MatchDraw() {
    draw_vs_ = nullptr;
    draw_ps_ = nullptr;
    matched_reason_ = 0;
    vs_object_ = ps_object_ = 0;
    vegetation_game_ = 0;  // without a record there is no game verdict to compare
    if (!shaders_.loaded()) {
      return;
    }
    RefreshObjects();  // a single acquire load per draw (formerly four)
    const uint32_t initiator = Reg(rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR);
    const uint32_t type = initiator & 0x3F;
    const uint32_t num = initiator >> 16;
    const uint32_t marker_mode = mode_marker_draw_;
    bool is_valid_marker = false;
    if (marker_mode != 0) {
      is_valid_marker = marker_draw_.args[0] == type && RegCount(marker_draw_) == num &&
                      ShadersCoherentes(marker_draw_);
      if (!is_valid_marker) {
        ++rejected_marker_regs_;
      } else {
        mode_marker_draw_ = 0;  // accepted: valid for this draw and no other
        if (marker_mode == kApplyDraw) {
          ++draws_with_marker_reg_;
          UseDrawRecord(marker_draw_);
          return;
        }
      }
    }
    DrawRecord reg_index;
    while (pending_.size() < 64 && ExtractDraw(reg_index)) {
      pending_.push_back(reg_index);
    }
    size_t found = SIZE_MAX;
    bool inconsistent = false;
    // The size of a deque is not a field, it is computed by subtracting iterators. It was
    // queried on every loop iteration, and there are ~2,200 draws per frame.
    const size_t candidates = std::min<size_t>(pending_.size(), 8);
    for (size_t i = 0; i < candidates; ++i) {
      if (pending_[i].args[0] != type || RegCount(pending_[i]) != num) {
        continue;
      }
      // Type and count are not enough: in a series of identical draws, a record
      // without a draw in the ring shifts the matching. The record's VS and PS
      // must be those of the last IM_LOAD packets (observed: VS n15 matched with
      // another shader's microcode).
      if (!ShadersCoherentes(pending_[i])) {
        ++inconsistent_candidates_;
        inconsistent = true;
        continue;
      }
      found = i;
      break;
    }
    if (marker_mode == kCheckDraw && is_valid_marker) {
      CompareDrawMarker(found == SIZE_MAX ? nullptr : &pending_[found], type, num);
    }
    if (found == SIZE_MAX) {
      ++draws_without_reg_;
      inconsistent_draws_ += inconsistent;
      matched_reason_ = inconsistent ? 2 : 1;
      if (c5b_warnings_ < 16) {
        ++c5b_warnings_;
        std::string first_item;
        if (!pending_.empty()) {
          const DrawRecord& r = pending_.front();
          first_item = fmt::format("; el primero pendiente es funcion {} tipo {} cuenta {}",
                                int(r.func_id), r.args[0], RegCount(r));
        }
        REXLOG_WARN("[nativo] C5b: dibujo del anillo sin registro de Draw*: tipo {} cuenta {} "
                    "({} pendientes{})",
                    type, num, pending_.size(), first_item);
      }
      UseRingIdentity();
      return;
    }
    skipped_regs_ += found;
    const DrawRecord r = pending_[found];
    pending_.erase(pending_.begin(), pending_.begin() + std::ptrdiff_t(found + 1));
    UseDrawRecord(r);
  }

  // The usual work with the matched record, whether it came from the queue or from the marker (phase 2b:
  // moved out of EmparejarDibujo unchanged).
  void UseDrawRecord(const DrawRecord& r) {
    if (r.shadow) {
      CompareShadow(r.shadow);  // phase 1 of the Direct3D-level renderer
    }
    vs_object_ = r.vs;
    ps_object_ = r.ps;
    ++matched_draws_;
    draw_vs_ = CachedShaderOfObject(r.vs);
    draw_ps_ = CachedShaderOfObject(r.ps);
    // The game's vegetation verdict is only compared when drawing with this record's shaders; not with
    // the ring identity (UsarIdentidadDelAnillo, below) (nfsmw_d3d_vegetacion_juego).
    vegetation_game_ = VegetationIdentity(r.vegetation, draw_vs_ != nullptr && draw_ps_ != nullptr);
    if (draw_vs_ && draw_ps_) {
      // Only when the VS or its microcode changes: it used to be one map write per draw.
      if ((draw_vs_ != vs_mapped_ || generacion_vs_ != generation_mapped_) &&
          vs_per_microcode_.size() < 4096) {
        vs_per_microcode_[VsFingerprint()] = draw_vs_;
        vs_mapped_ = draw_vs_;
        generation_mapped_ = generacion_vs_;
      }
    } else {
      UseRingIdentity();
    }
    draws_with_vs_ += draw_vs_ != nullptr;
    draws_with_ps_ += draw_ps_ != nullptr;
    if (draw_vs_ && (draw_vs_ != vs_tracked_ || generacion_vs_ != generation_tracked_)) {
      vs_tracked_ = draw_vs_;
      generation_tracked_ = generacion_vs_;
      TrackVertexInput(*draw_vs_);
    }
  }

  // Phase 2b: the VS and PS that would be used to draw with this record (null: no record), without
  // changing anything: what UsarRegistroDeDibujo and UsarIdentidadDelAnillo would do.
  std::pair<const ShaderEntry*, const ShaderEntry*> ShadersWithRecord(const DrawRecord* r) {
    const ShaderEntry* vs = r ? CachedShaderOfObject(r->vs) : nullptr;
    const ShaderEntry* ps = r ? CachedShaderOfObject(r->ps) : nullptr;
    if ((vs && ps) || !current_ps_) {
      return {vs, ps};
    }
    const ShaderEntry* identity = current_vs_;
    if (!identity) {
      const auto it = vs_per_microcode_.find(VsFingerprint());
      if (it == vs_per_microcode_.end()) {
        return {vs, ps};
      }
      identity = it->second;
    }
    return {identity, current_ps_};
  }

  /*
   * Phase 2b: the comparison of the watching phase (and of 1 in every 1,024 afterwards). Only when the
   * draw accepts the marker's record: if it does not, the marker path ends in the same search and gives
   * the same result by construction. What decides the draw is compared: the VS and PS it would be drawn
   * with using the record from the search (or the ring identity if none is found) and using the marker's
   * record. A different record with the same shaders is only counted.
   */
  void CompareDrawMarker(const DrawRecord* search, uint32_t type, uint32_t num) {
    const auto with_search = ShadersWithRecord(search);
    const auto with_marker = ShadersWithRecord(&marker_draw_);
    const DrawRecord& m = marker_draw_;
    uint32_t que = 0;  // 0 equal; 1 different shaders; 3 search finds no record and the identity is not valid
    if (with_search != with_marker) {
      que = search ? 1u : 3u;
    } else if (!search || search->func_id != m.func_id || search->vs != m.vs || search->ps != m.ps ||
               search->args[1] != m.args[1] || search->args[2] != m.args[2] || search->args[3] != m.args[3]) {
      ++reg_other_draw_checks_;  // another record (or the identity), same shaders
    }
    ++draw_checks_;
    if (que != 0) {
      ++distinct_draw_checks_;
      if (marker_draw_warnings_ < 8) {
        ++marker_draw_warnings_;
        const auto number = [](const ShaderEntry* e) { return e ? int(e->number) : -1; };
        REXLOG_ERROR("[nativo] registro de dibujo (fase 2b): la busqueda y el marcador no dan lo mismo (caso {}) en "
                     "un dibujo tipo {} cuenta {}: busqueda {} (VS {:08X} PS {:08X}) -> VS n{} PS n{}; marcador "
                     "(VS {:08X} PS {:08X}) -> VS n{} PS n{}. El hilo del juego apaga la fase 2b",
                     que, type, num, search ? "con registro" : "sin registro", search ? search->vs : 0u,
                     search ? search->ps : 0u, number(with_search.first), number(with_search.second), m.vs,
                     m.ps, number(with_marker.first), number(with_marker.second));
      }
    }
    TrackDrawCheck(que == 0, que, search, &m);
  }

  // Phase 2b: the kPalabrasDibujo words of the Draw* record (EscribirDibujo in
  // nfsmw_d3d_registros_nativo.cpp). false if the function is not a Draw* one.
  static bool ReadMarkerDraw(const uint8_t* p, DrawRecord& r) {
    // The low byte is the function and the 16 bits above it the game's vegetation verdict
    // (RegistroDibujo::vegetacion, nfsmw_d3d_vegetacion_juego); the high byte must be 0.
    const uint32_t word_value = rex::memory::load_and_swap<uint32_t>(p);
    const uint32_t func_id = word_value & 0xFFu;
    if (func_id > uint32_t(DrawFunction::kUpIndexed) || (word_value >> 24) != 0) {
      return false;
    }
    r.func_id = DrawFunction(func_id);
    r.vegetation = uint16_t(word_value >> 8);
    r.vs = rex::memory::load_and_swap<uint32_t>(p + 4);
    r.ps = rex::memory::load_and_swap<uint32_t>(p + 8);
    for (uint32_t i = 0; i < 4; ++i) {
      r.args[i] = rex::memory::load_and_swap<uint32_t>(p + 12 + 4 * i);
    }
    r.shadow = (uint64_t(rex::memory::load_and_swap<uint32_t>(p + 28)) << 32) |
               rex::memory::load_and_swap<uint32_t>(p + 32);
    return true;
  }

  /*
   * Phase 1 of the Direct3D-level renderer (docs/native-renderer.md).
   * The snapshot of the device mirror taken in this draw's Draw* call, against the state the ring has
   * read from the packets right when it reaches this draw. Everything that matches is state the renderer
   * can read from the mirror without packets; anything else has to come through another path.
   */
  void CompareShadow(uint64_t sequence) {
    if (!ReadSnapshot(sequence, shadow_snapshot_)) {
      ++shadow_lost_;
      return;
    }
    ++shadow_draws_;
    const MirrorSnapshot& f = shadow_snapshot_;
    for (uint32_t g = 0; g < kMirrorGroups; ++g) {
      const uint64_t m = f.mask[g];
      for (uint32_t i = 0; m && i < 64; ++i) {
        if (!((m >> (63 - i)) & 1)) {
          continue;
        }
        const uint32_t reg = f.reg_base[g] + i;
        ++shadow_compared_state_;
        if (Reg(reg) != f.state[g][i]) {
          ++shadow_distinct_state_;
          ++shadow_per_reg_[reg];
        }
      }
    }
    for (uint32_t i = 0; i < 192; ++i) {
      ++shadow_compared_fetch_;
      if (Reg(0x4800 + i) != f.fetch[i]) {
        ++shadow_distinct_fetch_;
        ++shadow_per_reg_[0x4800 + i];
      }
    }
    for (uint32_t i = 0; i < 2048; ++i) {
      ++shadow_compared_constants_;
      if (Reg(0x4000 + i) != f.constantes[i]) {
        ++shadow_distinct_constants_;
        ++shadow_per_reg_[0x4000 + i];
      }
    }
  }

  // Whether the VS and PS of a Draw* record are the ones the ring has loaded.
  // The PS arrives unpatched: its microcode is compared. For the patched VS, the only
  // possible check is that its fetches write to the original's registers.
  bool ShadersCoherentes(const DrawRecord& reg_index) {
    const ShaderEntry* ps = CachedShaderOfObject(reg_index.ps);
    if (ps && current_ps_ && ps->fingerprint != current_ps_->fingerprint) {
      return false;
    }
    const ShaderEntry* vs = CachedShaderOfObject(reg_index.vs);
    if (!vs) {
      return true;
    }
    if (vs != vs_coherency_ || generacion_vs_ != generation_coherency_) {
      vs_coherency_ = vs;
      generation_coherency_ = generacion_vs_;
      coherency_ = FetchCoherentes(*vs, microcode_vs_);
    }
    return coherency_;
  }

  // No usable Draw* record (in the menu, the final composite has no record of its own
  // and the ring carries VS n111 and PS n19): the PS of the last IM_LOAD, which arrives
  // unpatched; the VS of the IM_LOAD if it was fully identified, and otherwise the one
  // already seen matched with this same patched microcode.
  void UseRingIdentity() {
    if (!current_ps_) {
      return;
    }
    const ShaderEntry* vs = current_vs_;
    if (!vs) {
      const auto it = vs_per_microcode_.find(VsFingerprint());
      if (it == vs_per_microcode_.end()) {
        return;
      }
      vs = it->second;
    }
    draw_vs_ = vs;
    draw_ps_ = current_ps_;
    ++draws_per_im_load_;
  }

  uint64_t VsFingerprint() {
    if (generation_vs_fingerprint_ != generacion_vs_) {
      generation_vs_fingerprint_ = generacion_vs_;
      vs_fingerprint_ = XXH3_64bits(microcode_vs_.data(), microcode_vs_.size() * sizeof(uint32_t));
    }
    return vs_fingerprint_;
  }

  static uint32_t RegCount(const DrawRecord& r) {
    switch (r.func_id) {
      case DrawFunction::kVertices:
        return r.args[2];  // r6: vertex count
      case DrawFunction::kIndexed:
        return r.args[3];  // r7: index count
      case DrawFunction::kVerticesUP:
        return r.args[1];  // r5: vertex count
      case DrawFunction::kUpIndexed:
      default:
        return r.args[3];  // r7: index count (unconfirmed)
    }
  }

  /*
   * The object generation used to be read four times per draw (twice in ShadersCoherentes and
   * twice in EmparejarDibujo), and it is an atomic load with acquire: on the Switch's A57 that
   * is an ldar, which drains the load buffer. It is now read once, on entering the matching;
   * everything after that in the draw (including the trace) already has it.
   */
  void RefreshObjects() {
    const uint64_t generacion = ObjectsGeneration();
    if (generacion != objects_generation_) {
      objects_.clear();
      objects_memo_.fill(ObjectMemo{});
      objects_generation_ = generacion;
    }
  }

  const ShaderEntry* CachedShaderOfObject(uint32_t object) {
    /*
     * A 16-slot direct-mapped cache in front of the map: the draw's VS and PS are looked up several
     * times (candidate coherence and matching), and between consecutive draws they are almost
     * always the same. It saves the hash and the cache miss of the unordered_map, which with
     * ~2,200 draws per frame were ~9,000 lookups. RefrescarObjetos has already validated the
     * generation: when it changes, both caches are cleared together.
     */
    ObjectMemo& memo = objects_memo_[size_t((object * UINT32_C(2654435761)) >> 28)];
    if (memo.object == object) {
      return memo.entry;
    }
    const ShaderEntry* entry;
    if (const auto it = objects_.find(object); it != objects_.end()) {
      entry = it->second;
    } else {
      entry = ShaderOfObject(object);
      objects_.emplace(object, entry);
    }
    memo.object = object;
    memo.entry = entry;
    return entry;
  }

  // Vertex input of the patched VS. Each element of the container fetches into
  // a temporary register; the ring microcode is searched, among the fetch
  // instructions, for the one that writes to that register (D3D reorders them),
  // and it yields the fetch constant, format, stride and offset. Logged once per
  // variant, for validation.
  void TrackVertexInput(const ShaderEntry& vs) {
    if (microcode_vs_.size() != vs.ucode.size()) {
      ++distinct_length_variants_;
      return;
    }
    if (views_variants_.size() >= 256) {
      return;
    }
    const uint64_t key = (uint64_t(vs.number) << 48) ^ VsFingerprint();  // the same XXH3, already computed
    if (!views_variants_.insert(key).second) {
      return;
    }
    std::string detail;
    for (const VertexElement& element : vs.elements) {
      const uint32_t reg_index = element.reg_index;  // AirborneNX: masked in the microcode copy
      size_t p = SIZE_MAX;
      for (const VertexElement& other_one : vs.elements) {
        const size_t q = size_t(other_one.instruction) * 3;
        if (((microcode_vs_[q] >> 12) & 0x3F) == reg_index && (microcode_vs_[q] & 0x1F) == 0) {
          p = q;
          break;
        }
      }
      if (p == SIZE_MAX) {
        detail += fmt::format(" {}{}:?", UseName(element.use), element.use_index);
        ++elements_without_fetch_;
        continue;
      }
      const uint32_t d0 = microcode_vs_[p], d1 = microcode_vs_[p + 1], d2 = microcode_vs_[p + 2];
      // exp_adjust (signed bits 24-29) and sign mode (bit 14): the emulated path multiplies the
      // value by 2^exp_adjust (spirv_translator_fetch.cpp:445-449); this path does not.
      const int32_t exponent = int32_t(d1 << 2) >> 26;
      detail += fmt::format(" {}{}:f{}/fmt{}/z{}/o{}/s{:03X}>{:03X}{}{}{}{}", UseName(element.use),
                             element.use_index, ((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 0x3),
                             (d1 >> 16) & 0x3F, d2 & 0xFF, int32_t(d2 << 1) >> 9,
                             uint32_t(element.swizzle), d1 & 0xFFF, ((d1 >> 30) & 0x1) ? "/mini" : "",
                             ((d1 >> 12) & 0x1) ? "/signo" : "",
                             exponent ? fmt::format("/exp{}", exponent) : std::string(),
                             ((d1 >> 14) & 0x1) ? "/rf1" : "");
    }
    if (views_variants_.size() <= 24) {  // was 200; the ring thread writes it
      REXLOG_INFO("[nativo] C5b: VS n{} (variante {:016X}): entrada de vertices{}", vs.number,
                  key, detail);
    }
  }

  // Fetch constant 0, which VdSwap writes right before the Swap packet.
  SwapTexture SwapTextureOf() const {
    SwapTexture texture;
    for (uint32_t i = 0; i < 6; ++i) {
      texture.dword[i] = Reg(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + i);
    }
    return texture;
  }

  void IndirectBuffer(uint32_t address, uint32_t word_data, int depth_value) {
    AddRingOnly(indirect_);
    Reader reader;
    reader.base = memory_->TranslatePhysical(address);
    reader.pos = 0;
    reader.stop_pos = word_data;
    while (reader.Pending()) {
      if (!Packet(reader, depth_value)) {
        break;
      }
    }
  }

  // --- Presentacion de prueba -------------------------------------------------

  void Present() {
    const uint64_t swap = swaps_.fetch_add(1, std::memory_order_relaxed) + 1;
    g_native_swaps.fetch_add(1, std::memory_order_relaxed);
    counter_.fetch_add(1, std::memory_order_relaxed);
    if (!presenter_) {
      return;
    }
    // C2: the game image, if this Swap has a resolved texture.
    if (targets_) {
      // The gamma ramp, if the game has changed it (same thread as AnotarRampaGamma).
      if (sent_ramp_version_ != ramp_version_) {
        sent_ramp_version_ = ramp_version_;
        targets_->GammaRamp(gamma_ramp_);
      }
      const auto start = Clock::now();
      const bool was_presented = targets_->Present(presenter_.get(), SwapTextureOf(),
                                                   swap_width_.load(std::memory_order_relaxed),
                                                   swap_height_.load(std::memory_order_relaxed));
      present_time_ns_ += NanosecondsFrom(start);
      ++measures_presents_;
      if (was_presented) {
        return;
      }
    }
    presenter_->RefreshGuestOutput(
        kOutputWidth, kOutputHeight, kOutputWidth, kOutputHeight,
        [this, swap](rex::ui::Presenter::GuestOutputRefreshContext& contexto) {
          return ClearOutput(static_cast<OutputContext&>(contexto), swap);
        });
  }

  bool ClearOutput(OutputContext& contexto, uint64_t swap) {
    const rex::ui::vulkan::VulkanDevice* dev = provider_->vulkan_device();
    const auto& dfn = dev->functions();
    const VkDevice device = dev->device();

    if (render_pass_ == VK_NULL_HANDLE) {
      VkAttachmentDescription attached{};
      attached.format = rex::ui::vulkan::VulkanPresenter::kGuestOutputFormat;
      attached.samples = VK_SAMPLE_COUNT_1_BIT;
      attached.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      attached.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attached.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attached.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attached.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      attached.finalLayout = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout;
      VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      VkSubpassDescription subpass{};
      subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      subpass.colorAttachmentCount = 1;
      subpass.pColorAttachments = &reference;
      VkRenderPassCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
      info.attachmentCount = 1;
      info.pAttachments = &attached;
      info.subpassCount = 1;
      info.pSubpasses = &subpass;
      if (dfn.vkCreateRenderPass(device, &info, nullptr, &render_pass_) != VK_SUCCESS) {
        render_pass_ = VK_NULL_HANDLE;
        return false;
      }
    }

    if (pool_ == VK_NULL_HANDLE) {
      VkCommandPoolCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      info.queueFamilyIndex = dev->queue_family_graphics_compute();
      if (dfn.vkCreateCommandPool(device, &info, nullptr, &pool_) != VK_SUCCESS) {
        pool_ = VK_NULL_HANDLE;
        return false;
      }
      VkCommandBufferAllocateInfo reservation{};
      reservation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      reservation.commandPool = pool_;
      reservation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      reservation.commandBufferCount = 1;
      if (dfn.vkAllocateCommandBuffers(device, &reservation, &commands_) != VK_SUCCESS) {
        commands_ = VK_NULL_HANDLE;
      }
      VkFenceCreateInfo info_fence{};
      info_fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      if (dfn.vkCreateFence(device, &info_fence, nullptr, &fence_) != VK_SUCCESS) {
        fence_ = VK_NULL_HANDLE;
      }
    }
    if (commands_ == VK_NULL_HANDLE || fence_ == VK_NULL_HANDLE) {
      return false;
    }

    // A single command buffer: wait for the previous one before reusing it.
    if (pending_fence_) {
      dfn.vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX);
      dfn.vkResetFences(device, 1, &fence_);
      pending_fence_ = false;
    }
    dfn.vkResetCommandPool(device, pool_, 0);

    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    for (const Framebuffer& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE && f.version == contexto.image_version()) {
        framebuffer = f.framebuffer;
      }
    }
    if (framebuffer == VK_NULL_HANDLE) {
      Framebuffer& f = framebuffers_[next_framebuffer_];
      next_framebuffer_ = (next_framebuffer_ + 1) % framebuffers_.size();
      if (f.framebuffer != VK_NULL_HANDLE) {
        dfn.vkDestroyFramebuffer(device, f.framebuffer, nullptr);
        f.framebuffer = VK_NULL_HANDLE;
      }
      VkImageView view = contexto.image_view();
      VkFramebufferCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      info.renderPass = render_pass_;
      info.attachmentCount = 1;
      info.pAttachments = &view;
      info.width = kOutputWidth;
      info.height = kOutputHeight;
      info.layers = 1;
      if (dfn.vkCreateFramebuffer(device, &info, nullptr, &f.framebuffer) != VK_SUCCESS) {
        f.framebuffer = VK_NULL_HANDLE;
        return false;
      }
      f.version = contexto.image_version();
      framebuffer = f.framebuffer;
    }

    VkCommandBufferBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    start.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dfn.vkBeginCommandBuffer(commands_, &start) != VK_SUCCESS) {
      return false;
    }
    // Test color: green goes up and down with each Swap, so a capture
    // tells a running game from a stalled one.
    const float phase = float(swap % 240) / 239.0f;
    VkClearValue color{};
    color.color.float32[0] = 0.05f;
    color.color.float32[1] = 0.10f + 0.40f * phase;
    color.color.float32[2] = 0.35f;
    color.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = render_pass_;
    pass.framebuffer = framebuffer;
    pass.renderArea.extent.width = kOutputWidth;
    pass.renderArea.extent.height = kOutputHeight;
    pass.clearValueCount = 1;
    pass.pClearValues = &color;
    dfn.vkCmdBeginRenderPass(commands_, &pass, VK_SUBPASS_CONTENTS_INLINE);
    dfn.vkCmdEndRenderPass(commands_);
    if (dfn.vkEndCommandBuffer(commands_) != VK_SUCCESS) {
      return false;
    }
    VkSubmitInfo send{};
    send.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    send.commandBufferCount = 1;
    send.pCommandBuffers = &commands_;
    {
      const auto queue_obj = dev->AcquireQueue(dev->queue_family_graphics_compute(), 0);
      if (dfn.vkQueueSubmit(queue_obj.queue(), 1, &send, fence_) != VK_SUCCESS) {
        return false;
      }
    }
    pending_fence_ = true;
    contexto.SetIs8bpc(true);
    return true;
  }

  void DestroyVulkan() {
    targets_.reset();  // uses the device: before everything else
    if (!provider_ || !provider_->vulkan_device()) {
      return;
    }
    const rex::ui::vulkan::VulkanDevice* dev = provider_->vulkan_device();
    const auto& dfn = dev->functions();
    const VkDevice device = dev->device();
    if (fence_ != VK_NULL_HANDLE) {
      if (pending_fence_) {
        dfn.vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX);
        pending_fence_ = false;
      }
      dfn.vkDestroyFence(device, fence_, nullptr);
      fence_ = VK_NULL_HANDLE;
    }
    for (Framebuffer& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE) {
        dfn.vkDestroyFramebuffer(device, f.framebuffer, nullptr);
        f = Framebuffer{};
      }
    }
    if (pool_ != VK_NULL_HANDLE) {
      dfn.vkDestroyCommandPool(device, pool_, nullptr);
      pool_ = VK_NULL_HANDLE;
      commands_ = VK_NULL_HANDLE;
    }
    if (render_pass_ != VK_NULL_HANDLE) {
      dfn.vkDestroyRenderPass(device, render_pass_, nullptr);
      render_pass_ = VK_NULL_HANDLE;
    }
  }

  // Report lines go to the report thread (NFSMW_INFORME_ANILLO, see InformeDiferido), except the
  // shutdown ones (forzar), which are written right here so they are not lost.
#define NFSMW_REPORT_ROUTED(defer, ...) do { if (defer) { NFSMW_RING_REPORT(__VA_ARGS__); } else { REXLOG_INFO(__VA_ARGS__); } } while (0)
  void Report(bool force) {
    const auto now_time = Clock::now();
    if (!force &&
        now_time - last_report_ < std::chrono::seconds(REXCVAR_GET(nfsmw_nativo_informe_s))) {
      return;
    }
    const bool defer_report = !force;
    last_report_ = now_time;
    NFSMW_REPORT_ROUTED(defer_report,
        "[nativo] swaps={} vblanks={} paquetes={} interrupciones={} indirectos={} "
        "escrituras_memoria={} esperas_agotadas={} dibujos={} copias={}",
        swaps_.load(), vblanks_.load(), packets_.load(), interrupts_.load(),
        indirect_.load(), memory_writes_.load(), exhausted_waits_.load(), draws_.load(),
        copies_.load());
    // Ring thread cost since the previous report. "Anillo" includes the
    // WAIT_REG_MEM waits and the wait for the GPU when presenting.
    const double secs = std::chrono::duration<double>(now_time - last_time_report_).count();
    last_time_report_ = now_time;
    if (secs > 0.0) {
      NFSMW_REPORT_ROUTED(defer_report,
          "[nativo] tiempos: anillo {:.1f} ms/s; dibujos {:.1f} us/dibujo ({}); copias {:.1f} us/copia; "
          "presentar {:.2f} ms/Swap con la espera a la GPU",
          double(ring_time_ns_) / 1e6 / secs,
          measured_draws_ ? double(draws_time_ns_) / 1e3 / double(measured_draws_) : 0.0,
          measured_draws_,
          measures_copies_ ? double(copies_time_ns_) / 1e3 / double(measures_copies_) : 0.0,
          measures_presents_
              ? double(present_time_ns_) / 1e6 / double(measures_presents_)
              : 0.0);
      // Ring thread wakeups: how many CP_RB_WPTR writes the game makes and how many iterations this
      // thread runs to serve them.
      {
        const uint64_t writes = wptr_writes_.load(std::memory_order_relaxed);
        const uint64_t d_writes = writes - wptr_writes_previous_;
        const uint64_t d_turns = ring_turns_ - ring_turns_previous_;
        const uint64_t d_with_data = ring_turns_with_data_ - ring_turns_with_data_previous_;
        const uint64_t d_exhausted = exhausted_ring_waits_ - exhausted_ring_waits_previous_;
        /*
         * Core migrations go on this same line on purpose, next to the timed-out waits: both
         * measure the same thing (the ring not working when it should) and are best read
         * together. If nfsmw_nativo_anillo_nucleo at -1 and at 1 gives the same migrations/s,
         * the preferred core has no effect here.
         */
        const uint64_t d_migrations = ring_migrations_ - ring_migrations_previous_;
        NFSMW_REPORT_ROUTED(defer_report, "[nativo] despertares del anillo: {:.0f} escrituras de CP_RB_WPTR/s, {:.0f} vueltas/s, {:.0f} "
                    "con datos/s ({:.2f} escrituras por vuelta con datos), {:.0f} esperas de 4 ms agotadas/s; "
                    "nucleo {} con {:.0f} migraciones/s ({:.2f} por vuelta)",
                    double(d_writes) / secs, double(d_turns) / secs, double(d_with_data) / secs,
                    d_with_data ? double(d_writes) / double(d_with_data) : 0.0, double(d_exhausted) / secs,
                    ring_core_, double(d_migrations) / secs,
                    d_turns ? double(d_migrations) / double(d_turns) : 0.0);
        ring_migrations_previous_ = ring_migrations_;
        wptr_writes_previous_ = writes;
        ring_turns_previous_ = ring_turns_;
        ring_turns_with_data_previous_ = ring_turns_with_data_;
        exhausted_ring_waits_previous_ = exhausted_ring_waits_;
      }
      // Breakdown per packet, largest first (top 8).
      std::vector<std::tuple<uint64_t, int, uint64_t>> parts;
      if (regs_time_ns_) {
        parts.emplace_back(regs_time_ns_, -1, regs_count_);
      }
      for (int op = 0; op < int(opcode_time_ns_.size()); ++op) {
        if (opcode_time_ns_[op]) {
          parts.emplace_back(opcode_time_ns_[op], op, opcode_count_[op]);
        }
      }
      std::sort(parts.begin(), parts.end(), std::greater<>());
      std::string distribution;
      for (size_t i = 0; i < parts.size() && i < 8; ++i) {
        const auto& [ns, op, n] = parts[i];
        distribution += op < 0 ? fmt::format(" registros {:.1f} ms/s ({})", double(ns) / 1e6 / secs, n)
                          : fmt::format(" op {:02X} {:.1f} ms/s ({})", op,
                                        double(ns) / 1e6 / secs, n);
      }
      NFSMW_REPORT_ROUTED(defer_report, "[nativo] tiempos por paquete:{}", distribution);
      // How many register words take the block path and how many go one at a time.
      NFSMW_REPORT_ROUTED(defer_report, "[nativo] registros por camino: {} palabras en bloque, {} una a una ({:.1f} palabras por paquete de "
                  "tipo 0/1)",
                  block_words_, released_words_,
                  regs_count_ ? double(block_words_ + released_words_) / double(regs_count_) : 0.0);
      block_words_ = released_words_ = 0;
      // Phase 2 of the Direct3D-level renderer (ProcesarMarcador). Its time shows up as "op 10".
      if (applied_markers_ || checked_markers_ || bad_markers_) {
        NFSMW_REPORT_ROUTED(defer_report, "[nativo] marcadores D3D (fase 2): {} aplicados ({:.1f} tramos y {:.1f} palabras cada uno; {} "
                    "tramos por el camino de siempre), {} comprobados contra sus paquetes ({} distintos), {} mal "
                    "formados; camino rapido {} ({} tramos comprobados)",
                    applied_markers_,
                    applied_markers_ ? double(segments_marker_) / double(applied_markers_) : 0.0,
                    applied_markers_ ? double(words_marker_) / double(applied_markers_) : 0.0,
                    slow_segments_marker_, checked_markers_, distinct_markers_, bad_markers_,
                    fast_marker_ > 0 ? "encendido" : "APAGADO", verified_fast_segments_);
        applied_markers_ = checked_markers_ = distinct_markers_ = bad_markers_ = 0;
        segments_marker_ = slow_segments_marker_ = words_marker_ = 0;
      }
      // Phase 2b: the draw record in the marker (EmparejarDibujo).
      if (draws_with_marker_reg_ || rejected_marker_regs_ || draw_checks_) {
        NFSMW_REPORT_ROUTED(defer_report, "[nativo] registro de dibujo en el marcador (fase 2b): {} dibujos sin busqueda, {} registros "
                    "del marcador no aceptados (tipo, cuenta o shaders: se busco como siempre), {} comprobados "
                    "contra la busqueda ({} distintos, {} con otro registro y los mismos shaders)",
                    draws_with_marker_reg_, rejected_marker_regs_, draw_checks_,
                    distinct_draw_checks_, reg_other_draw_checks_);
        draws_with_marker_reg_ = rejected_marker_regs_ = 0;
        draw_checks_ = distinct_draw_checks_ = reg_other_draw_checks_ = 0;
      }
      // Phase 1 of the Direct3D-level renderer.
      if (shadow_draws_ || shadow_lost_) {
        std::vector<std::pair<uint64_t, uint32_t>> worst_list;
        for (const auto& [reg, n] : shadow_per_reg_) {
          worst_list.emplace_back(n, reg);
        }
        std::sort(worst_list.begin(), worst_list.end(), std::greater<>());
        std::string item_list;
        for (size_t i = 0; i < worst_list.size() && i < 12; ++i) {
          item_list += fmt::format(" 0x{:04X}x{}", worst_list[i].second, worst_list[i].first);
        }
        size_t seen_groups = 0;
        for (uint32_t g = 0; g < kMirrorGroups; ++g) {
          seen_groups += shadow_snapshot_.mask[g] != 0;
        }
        NFSMW_REPORT_ROUTED(defer_report, "[nativo] sombra D3D: {} dibujos comparados ({} fotos perdidas; {} de {} grupos del espejo "
                    "aprendidos) | estado {} distintos de {} | fetch {} de {} | constantes {} de {} | registros "
                    "con diferencias: {}{}",
                    shadow_draws_, shadow_lost_, seen_groups, kMirrorGroups, shadow_distinct_state_,
                    shadow_compared_state_, shadow_distinct_fetch_, shadow_compared_fetch_,
                    shadow_distinct_constants_, shadow_compared_constants_, worst_list.size(),
                    item_list.empty() ? "" : " (los que mas:" + item_list + ")");
        shadow_draws_ = shadow_lost_ = 0;
        shadow_compared_state_ = shadow_distinct_state_ = 0;
        shadow_compared_fetch_ = shadow_distinct_fetch_ = 0;
        shadow_compared_constants_ = shadow_distinct_constants_ = 0;
        shadow_per_reg_.clear();
      }
      // What WAIT_REG_MEM waits for: the 4 with the most 1 ms iterations.
      std::vector<std::pair<uint64_t, WaitStats>> waits(waits_.begin(), waits_.end());
      std::sort(waits.begin(), waits.end(), [](const auto& a, const auto& b) {
        return a.second.turns > b.second.turns;
      });
      std::string waits_detail;
      for (size_t i = 0; i < waits.size() && i < 4; ++i) {
        const auto& [key, e] = waits[i];
        waits_detail += fmt::format(
            " {} {:08X} (funcion {} ref {:08X} mascara {:08X}): {} esperas, {} vueltas;",
            (key >> 32) ? "memoria" : "registro", uint32_t(key), e.info & 0x7, e.reference,
            e.mask, e.times, e.turns);
      }
      if (!waits_detail.empty()) {
        NFSMW_REPORT_ROUTED(defer_report, "[nativo] WAIT_REG_MEM:{}", waits_detail);
      }
    }
    ring_time_ns_ = draws_time_ns_ = copies_time_ns_ = present_time_ns_ = 0;
    measured_draws_ = measures_copies_ = measures_presents_ = 0;
    regs_time_ns_ = regs_count_ = 0;
    opcode_time_ns_.fill(0);
    opcode_count_.fill(0);
    waits_.clear();
    const uint32_t frontbuffer = swap_frontbuffer_.load();
    const uint32_t target = last_target_copy_.load();
    NFSMW_REPORT_ROUTED(defer_report,
        "[nativo] ultimo swap: frontbuffer={:08X} {}x{}; ultima copia: destino={:08X} "
        "control={:08X} info={:08X} pitch={:08X}; coinciden={}",
        frontbuffer, swap_width_.load(), swap_height_.load(), target, last_control_copy_.load(),
        last_info_copy_.load(), last_pitch_copy_.load(),
        (frontbuffer & 0x1FFFFFFF) == (target & 0x1FFFFFFF));
    if (targets_) {
      uint64_t copies = 0, clears = 0, presented = 0, rechazos = 0;
      targets_->Stats(copies, clears, presented, rechazos);
      uint64_t gpu_ns = 0, gpu_jobs = 0;
      targets_->GpuTime(gpu_ns, gpu_jobs);
      const uint64_t interval_presented = presented - gpu_previous_presented_;
      NFSMW_REPORT_ROUTED(defer_report, "[nativo] C2: copias={} (de profundidad {}) borrados={} (de profundidad {}) "
                  "presentados={} rechazos={}; GPU {:.2f} ms por Swap ({} trabajos medidos)",
                  copies, targets_->DepthCopies(), clears,
                  targets_->DepthClears(), presented, rechazos,
                  interval_presented
                      ? double(gpu_ns - gpu_previous_ns_) / 1e6 / double(interval_presented)
                      : 0.0,
                  gpu_jobs - gpu_jobs_previous_);
      // Occlusion queries since the previous report.
      {
        const auto d = [&](size_t i) { return counters_occlusion_[i] - counters_occlusion_previous_[i]; };
        uint64_t oc[5] = {};
        targets_->OcclusionStats(oc);
        if (d(0) || d(1) || d(2) || d(4)) {
          NFSMW_REPORT_ROUTED(defer_report, "[nativo] C2 oclusion: modo {}; consultas del juego empezadas {}, terminadas con medida {} y "
                      "sin medida todavia {}, sin su final {}, fingidas fuera de pareja {}; escritas medidas: media "
                      "{:.1f}, maxima historica {}; terminadas por destino: pequeno (escala 1) {} (maxima {}), de "
                      "escena (escala del modo de AA) {} (maxima {}); del host: tramos {} (sin sitio {}), consultas "
                      "publicadas {}, muestras {} (maxima historica por consulta {})",
                      REXCVAR_GET(nfsmw_nativo_oclusion), d(0), d(1), d(2), d(3), d(4),
                      d(1) ? double(d(5)) / double(d(1)) : 0.0, counters_occlusion_[6], occlusion_per_target_[0],
                      max_occlusion_per_target_[0], occlusion_per_target_[1], max_occlusion_per_target_[1],
                      oc[0] - host_occlusion_previous_[0], oc[1] - host_occlusion_previous_[1],
                      oc[2] - host_occlusion_previous_[2], oc[3] - host_occlusion_previous_[3], oc[4]);
        }
        occlusion_per_target_ = {};
        counters_occlusion_previous_ = counters_occlusion_;
        std::copy(std::begin(oc), std::end(oc), host_occlusion_previous_);
      }
      {
        uint64_t waits = 0, waits_ns = 0;
        targets_->GpuWaits(waits, waits_ns);
        const double waits_ms = double(waits_ns - gpu_waits_previous_ns_) / 1e6;
        NFSMW_REPORT_ROUTED(defer_report, "[nativo] C2: esperas del anillo a la GPU: {} ({:.1f} ms; {:.2f} ms por Swap)",
                    waits - gpu_waits_previous_, waits_ms,
                    interval_presented ? waits_ms / double(interval_presented) : 0.0);
        gpu_waits_previous_ = waits;
        gpu_waits_previous_ns_ = waits_ns;
        // And the real duration of the jobs, to check it against the sum of the timestamps.
        uint64_t jobs = 0, jobs_ns = 0;
        targets_->GpuJobsDuration(jobs, jobs_ns);
        const uint64_t d_jobs = jobs - jobs_gpu_previous_;
        const double jobs_ms = double(jobs_ns - gpu_job_previous_ns_) / 1e6;
        NFSMW_REPORT_ROUTED(defer_report, "[nativo] C2: trabajos de GPU por reloj de pared: {} ({:.1f} ms; {:.2f} ms cada uno; "
                    "{:.2f} ms por Swap)",
                    d_jobs, jobs_ms, d_jobs ? jobs_ms / double(d_jobs) : 0.0,
                    interval_presented ? jobs_ms / double(interval_presented) : 0.0);
        jobs_gpu_previous_ = jobs;
        gpu_job_previous_ns_ = jobs_ns;
      }
      {
        uint64_t cost[6] = {};
        targets_->RecordCost(cost);
        uint64_t dc[6] = {};
        for (size_t i = 0; i < 6; ++i) {
          dc[i] = cost[i] - record_cost_previous_[i];
          record_cost_previous_[i] = cost[i];
        }
        NFSMW_REPORT_ROUTED(defer_report, "[nativo] C2: bufers de comandos empezados {} ({:.1f} ms en Grabar, {:.1f} ms "
                    "reiniciando pools); lecturas de vuelta {} ({:.2f} M texels, {:.1f} ms "
                    "escribiendolas)",
                    dc[0], double(dc[1]) / 1e6, double(dc[2]) / 1e6, dc[3], double(dc[4]) / 1e6,
                    double(dc[5]) / 1e6);
      }
      std::array<uint64_t, kGpuCategories> categories{};
      targets_->GpuTimePerCategory(categories);
      if (interval_presented) {
        const auto ms = [&](uint32_t c) {
          return double(categories[c] - gpu_categories_previous_[c]) / 1e6 /
                 double(interval_presented);
        };
        NFSMW_REPORT_ROUTED(defer_report, "[nativo] C2: GPU por Swap: sombras {:.2f} ms, escena {:.2f}, reflejo {:.2f}, "
                    "320 (cubo y desenfoque) {:.2f}, menores {:.2f}, copias {:.2f}, borrados {:.2f}, "
                    "resto {:.2f}, escena sin profundidad {:.2f}, hueco entre trabajos {:.2f}",
                    ms(kGpuShadows), ms(kGpuScene), ms(kGpuReflection), ms(kGpu320),
                    ms(kGpuLesser), ms(kGpuCopies), ms(kGpuClears), ms(kGpuOther),
                    ms(kGpuSceneWithoutDepth), ms(kGpuSlotBetweenJobs));
      }
      gpu_categories_previous_ = categories;
      // The size of the remaining copies.
      if (interval_presented) {
        std::array<uint64_t, 4> t_copies{}, t_pixels{};
        targets_->CopiesBySize(t_copies, t_pixels);
        static constexpr const char* kBuckets[] = {"<=64x64", "<=320x320", "<=1024x1024", "mayores"};
        std::string line;
        for (uint32_t c = 0; c < 4; ++c) {
          const uint64_t n = t_copies[c] - bucket_copies_previous_[c];
          const uint64_t px = t_pixels[c] - bucket_pixels_previous_[c];
          if (n) {
            line += fmt::format(" {} {:.1f} copias y {:.2f} Mpixeles", kBuckets[c],
                                 double(n) / double(interval_presented),
                                 double(px) / 1e6 / double(interval_presented));
          }
        }
        if (!line.empty()) {
          NFSMW_REPORT_ROUTED(defer_report, "[nativo] C2 copias por fotograma y tamano:{}", line);
        }
        bucket_copies_previous_ = t_copies;
        bucket_pixels_previous_ = t_pixels;
      }
      // Actual fragments and vertices of each pass, to separate per-pixel cost from geometry cost.
      // The counts do not depend on the machine: what is measured on the PC holds for the console.
      {
        std::array<uint64_t, kGpuCategories> frag{}, vert{}, prim{};
        targets_->PipelineStats(frag, vert, prim);
        static constexpr const char* kStatTypes[] = {"resto",  "sombras", "escena",  "reflejo",
                                                      "cubo",   "menores", "copias",  "borrados",
                                                      "escena sin profundidad", "hueco"};
        std::string line;
        for (uint32_t c = 0; interval_presented && c < kGpuCategories; ++c) {
          const uint64_t f = frag[c] - category_fragments_previous_[c];
          const uint64_t v = vert[c] - category_vertices_previous_[c];
          const uint64_t p = prim[c] - category_primitives_previous_[c];
          if (f || v) {
            line += fmt::format(" {} {:.2f} M fragmentos, {:.0f} k vertices, {:.0f} k primitivas;",
                                 kStatTypes[c], double(f) / 1e6 / double(interval_presented),
                                 double(v) / 1e3 / double(interval_presented),
                                 double(p) / 1e3 / double(interval_presented));
          }
        }
        if (!line.empty()) {
          NFSMW_REPORT_ROUTED(defer_report, "[nativo] C2 por fotograma y tipo de pasada:{}", line);
        }
        category_fragments_previous_ = frag;
        category_vertices_previous_ = vert;
        category_primitives_previous_ = prim;
      }
      // Which pixel shaders fill the screen, from the diagnostic frames
      // (nfsmw_nativo_estadisticas_por_dibujo_s). Accumulated from the start, not per interval.
      {
        std::vector<uint64_t> frag_ps, dib_ps;
        uint64_t frames = 0;
        targets_->StatsPerShader(frag_ps, dib_ps, frames);
        if (frames > diagnostic_frames_previous_) {
          diagnostic_frames_previous_ = frames;
          static constexpr const char* kPsTypes[] = {"resto",  "sombras", "escena",  "reflejo",
                                                     "cubo",   "menores", "copias",  "borrados",
                                                     "escena sin profundidad", "hueco"};
          const size_t per_category = frag_ps.size() / kGpuCategories;
          for (uint32_t c = 0; c < kGpuCategories; ++c) {
            uint64_t total = 0;
            std::vector<uint32_t> orden;
            for (size_t i = 0; i < per_category; ++i) {
              const size_t e = size_t(c) * per_category + i;
              total += frag_ps[e];
              if (frag_ps[e]) {
                orden.push_back(uint32_t(e));
              }
            }
            uint64_t draws = 0;
            for (size_t i = 0; i < per_category; ++i) {
              draws += dib_ps[size_t(c) * per_category + i];
            }
            if (!total && !draws) {
              continue;  // that category was not measured
            }
            std::sort(orden.begin(), orden.end(),
                      [&](uint32_t a, uint32_t b) { return frag_ps[a] > frag_ps[b]; });
            std::string line;
            for (size_t i = 0; i < orden.size() && i < 8; ++i) {
              const uint32_t e = orden[i];
              const uint32_t ps = uint32_t(e % per_category);
              line += fmt::format(" PS {} {:.2f} M ({:.0f} %, {:.1f} dibujos)",
                                   ps ? fmt::format("n{}", ps - 1) : std::string("sin"),
                                   double(frag_ps[e]) / 1e6 / double(frames),
                                   100.0 * double(frag_ps[e]) / double(total),
                                   double(dib_ps[e]) / double(frames));
            }
            NFSMW_REPORT_ROUTED(defer_report, "[nativo] C2 fragmentos de {} por pixel shader ({} fotogramas, {:.2f} M y {:.0f} "
                        "dibujos por fotograma):{}",
                        kPsTypes[c], frames, double(total) / 1e6 / double(frames),
                        double(draws) / double(frames), line);
          }
        }
      }
      // Real scale of the GPU timestamps. Between two reports, the end timestamp of the last timed job
      // must advance as much as the clock (with a lag of one or two frames at each end). On the console
      // NVK declares 1 ns per unit and the ratio comes out at 1.628 in every interval measured, menus
      // included: the GPU ms in the reports must be multiplied by the accumulated scale.
      {
        const uint64_t mark = targets_->GpuEndMarkNs();
        const Clock::time_point now_time = Clock::now();
        if (mark != 0 && mark != gpu_mark_previous_ns_) {
          if (gpu_mark_previous_ns_ != 0 && mark > gpu_mark_previous_ns_) {
            const double real_ns = double(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now_time - mark_clock_previous_).count());
            const double progress_ns = double(mark - gpu_mark_previous_ns_);
            real_scale_ns_ += real_ns;
            marks_scale_ns_ += progress_ns;
            const double scale = real_scale_ns_ / marks_scale_ns_;
            NFSMW_REPORT_ROUTED(defer_report, "[nativo] C2: escala de las marcas de GPU {:.3f} en este intervalo ({:.3f} acumulada; tiempo "
                        "real entre informes / avance de las marcas); GPU real por Swap {:.2f} ms",
                        real_ns / progress_ns, scale,
                        interval_presented
                            ? double(gpu_ns - gpu_previous_ns_) / 1e6 / double(interval_presented) * scale
                            : 0.0);
          }
          gpu_mark_previous_ns_ = mark;
          mark_clock_previous_ = now_time;
        }
      }
      // Mode of the intermediate timestamps of the jobs timed in the interval.
      {
        const uint64_t precise_items = targets_->PreciseMarksJobs();
        NFSMW_REPORT_ROUTED(defer_report, "[nativo] C2: marcas precisas en {} de {} trabajos medidos", precise_items - precise_jobs_previous_,
                    gpu_jobs - gpu_jobs_previous_);
        precise_jobs_previous_ = precise_items;
      }
      // Intervals between Swaps and GPU overlaps.
      {
        std::array<uint64_t, kSwapBuckets> buckets{};
        uint64_t overlaps = 0;
        double worst_ms = 0.0;
        targets_->IntervalsBetweenSwaps(buckets, overlaps, worst_ms);
        uint64_t total = 0;
        for (uint32_t i = 0; i < kSwapBuckets; ++i) {
          total += buckets[i] - swap_buckets_previous_[i];
        }
        if (total) {
          const auto c = [&](uint32_t i) { return buckets[i] - swap_buckets_previous_[i]; };
          NFSMW_REPORT_ROUTED(defer_report, "[nativo] C2: intervalos entre Swaps: <15 ms {} | 15-18 {} | 18-25 {} | 25-30 {} | 30-36 {} | "
                      "36-50 {} | 50-60 {} | 60-75 {} | 75-100 {} | 100-150 {} | >=150 {}; "
                      "peor fotograma {:.1f} ms; trabajos solapados en la GPU {}",
                      c(0), c(1), c(2), c(3), c(4), c(5), c(6), c(7), c(8), c(9), c(10), worst_ms,
                      overlaps - gpu_overlaps_previous_);
        }
        swap_buckets_previous_ = buckets;
        nfsmw::guard30::Report();  // which step the 30 FPS guard was on
        {
          // How much of the game's constant dump is redundant.
          static uint64_t esc_prev = 0, without_prev = 0, blo_prev = 0, blosin_prev = 0;
          const uint64_t esc = written_constants_ - esc_prev;
          const uint64_t without_c = constants_without_change_ - without_prev;
          const uint64_t blo = constants_blocks_ - blo_prev;
          const uint64_t blosin = constants_blocks_without_change_ - blosin_prev;
          esc_prev = written_constants_; without_prev = constants_without_change_;
          blo_prev = constants_blocks_; blosin_prev = constants_blocks_without_change_;
          if (esc > 0 || blo > 0) {
            NFSMW_REPORT_ROUTED(defer_report, "[nativo] C6 constantes del juego: {} escrituras sueltas ({} sin cambio, {:.1f} %) | "
                        "{} bloques ({} sin cambio, {:.1f} %)",
                        esc, without_c, esc ? 100.0 * double(without_c) / double(esc) : 0.0,
                        blo, blosin, blo ? 100.0 * double(blosin) / double(blo) : 0.0);
          }
        }
        {
          /*
           * How often the fetch constants and the viewport state really change. This
           * measure tells how much the translator can skip: if changes per frame are far
           * fewer than draws per frame, the "texturas" stage and the viewport/scissor
           * are almost entirely repeated work.
           */
          static uint64_t fe_prev = 0, fs_prev = 0, fg_prev = 0;
          static uint64_t ee_prev = 0, es_prev = 0, eg_prev = 0, dib_prev = 0;
          const uint64_t fe = written_fetch_ - fe_prev, fs = fetch_without_change_ - fs_prev;
          const uint64_t fg = generacion_fetch_ - fg_prev;
          const uint64_t ee = framing_written_ - ee_prev, es = framing_without_change_ - es_prev;
          const uint64_t eg = framing_generation_ - eg_prev;
          // dibujos_medidos_ was already reset above in this same report: the right count is
          // the ring's accumulated counter.
          const uint64_t draw_total = draws_.load(std::memory_order_relaxed);
          const uint64_t dib = draw_total - dib_prev;
          fe_prev = written_fetch_; fs_prev = fetch_without_change_; fg_prev = generacion_fetch_;
          ee_prev = framing_written_; es_prev = framing_without_change_; eg_prev = framing_generation_;
          dib_prev = draw_total;
          if (fe > 0 || ee > 0) {
            NFSMW_REPORT_ROUTED(defer_report, "[nativo] C6 generaciones: fetch {} escrituras ({} sin cambio), {} cambios "
                        "({:.2f} por dibujo) | encuadre {} escrituras ({} sin cambio), {} cambios "
                        "({:.2f} por dibujo); dibujos {}",
                        fe, fs, fg, dib ? double(fg) / double(dib) : 0.0,
                        ee, es, eg, dib ? double(eg) / double(dib) : 0.0, dib);
          }
        }
        gpu_overlaps_previous_ = overlaps;
      }
      // Breakdown of the time spent in Presentar.
      {
        uint64_t c[12] = {};
        targets_->PresentCost(c);
        const auto d = [&](int i) { return c[i] - present_cost_previous_[i]; };
        const auto media = [&](int ns, int times) { return d(times) ? double(d(ns)) / 1e6 / double(d(times)) : 0.0; };
        if (d(0) || d(2) || d(5) || d(8)) {
          NFSMW_REPORT_ROUTED(defer_report, "[nativo] C2: presentar (ms por vez): espera a la salida anterior {:.2f} ({}); envio del trabajo: "
                      "candado de la cola {:.2f}, vkQueueSubmit {:.2f} ({}); envio de la salida: candado {:.2f}, "
                      "vkQueueSubmit {:.2f} ({}); RefreshGuestOutput: antes de la llamada {:.2f}, dentro {:.2f}, "
                      "despues {:.2f} ({})",
                      media(1, 0), d(0), media(3, 2), media(4, 2), d(2), media(6, 5), media(7, 5), d(5),
                      media(9, 8), media(10, 8), media(11, 8), d(8));
        }
        std::copy(c, c + 12, present_cost_previous_);
      }
      gpu_previous_ns_ = gpu_ns;
      gpu_jobs_previous_ = gpu_jobs;
      gpu_previous_presented_ = presented;
      const DrawStats d = targets_->GetDrawStats();
      std::string causes;
      for (const auto& [cause, n] : d.causes) {
        causes += fmt::format(" {}={}", cause, n);
      }
      // Per-stage cost of the draws recorded since the previous report, measured on the timed ones.
      const uint64_t recorded_items = d.drawn - drawn_previous_;
      const uint64_t timed = d.timed_drawn - timed_previous_;
      if (recorded_items && timed) {
        static constexpr const char* kStages[] = {"estado",  "indices",  "texturas",
                                                  "pase",    "subidas",  "pipeline",
                                                  "grabar",  "(de la etapa pase, el cambio de pase)",
                                                  "[del cambio de pase, cerrar el anterior]",
                                                  "[destinos]", "[render pass y framebuffer]",
                                                  "[abrir el pase]"};
        std::string stages;
        for (size_t i = 0; i < d.stages_ns.size(); ++i) {
          stages += fmt::format(" {} {:.1f}", kStages[i],
                                double(d.stages_ns[i] - stages_previous_[i]) / 1e3 / double(timed));
        }
        /*
         * The divisor is the number of recorded draws, not the number of incoming draws.
         * `dibujados_cronometrados_` is incremented in nfsmw_nativo_dibujos.cpp:2440, right after
         * vkCmdDraw/vkCmdDrawIndexed: it only counts recorded draws, never rejected ones.
         * Arithmetic check on a log (1 in 8 timed): sample 107,457 and recorded 859,614, and
         * 859,614/8 = 107,452 (exact). With the incoming draws it would have been 147,852.
         *
         * In practice: to turn these us/draw into ms per frame, multiply by the recorded draws
         * (~1,395/frame), not by the incoming ones (~1,957/frame). Getting it wrong inflates every
         * stage by 40 %. Also, these samples pay for 12 clock reads, so they come out ~3 us above
         * "dibujos us/dibujo".
         */
        // The ratio comes from the counts themselves (it used to be a hard-coded 8; the stage timer
        // now runs on 1 of every 64 draws).
        NFSMW_REPORT_ROUTED(defer_report, "[nativo] C6 etapas (us por dibujo GRABADO, muestra de {} = 1 de cada {:.0f} grabados; "
                    "grabados {}):{}",
                    timed, double(recorded_items) / double(timed), recorded_items, stages);
      }
      // The scenery LOD hook. If "forzadas" does not resemble "que el juego dibuja", the hook is
      // not reaching the objects and the popping will remain (this has happened before).
      NFSMW_REPORT_ROUTED(defer_report, "[nativo] C6 {}", nfsmw::lod_scenario::Summary());
      // The early vegetation rejection has its own counter. Without this line there is no way to
      // check whether the shortcut works (the number would have to be deduced by subtracting
      // recorded draws from incoming ones).
      {
        const uint64_t soon = d.vegetation_soon - vegetation_soon_previous_;
        vegetation_soon_previous_ = d.vegetation_soon;
        if (interval_presented) {
          NFSMW_REPORT_ROUTED(defer_report, "[nativo] C6 vegetacion de sombras tirada pronto: {:.0f} por fotograma ({} en el "
                      "intervalo); se ahorran indices, texturas, subida y pase de cada uno",
                      double(soon) / double(interval_presented), soon);
        }
      }
      {
        // How much of the scene forces shading before the depth test.
        const uint64_t con = d.scene_with_discard, without = d.scene_without_discard;
        const uint64_t dcon = con - discard_with_previous_, dsin = without - discard_without_previous_;
        discard_with_previous_ = con;
        discard_without_previous_ = without;
        if (dcon + dsin) {
          NFSMW_REPORT_ROUTED(defer_report, "[nativo] C6 descarte temprano en la escena: {} dibujos lo permiten, {} lo impiden "
                      "(prueba de alfa, kill o profundidad) = {:.0f} %",
                      dsin, dcon, 100.0 * double(dcon) / double(dcon + dsin));
        }
      }
      drawn_previous_ = d.drawn;
      timed_previous_ = d.timed_drawn;
      stages_previous_ = d.stages_ns;
      {
        const std::array<uint64_t, 20> counters = {d.passes, d.full_sends,
                                                      d.full_sends_ns, d.bytes_vertices,
                                                      d.bytes_indices, d.samplers,
                                                      d.samplers_cache, d.passes_ns,
                                                      d.ns_vertices, d.computed_entries,
                                                      d.entries_ns, d.reused_entries,
                                                      d.passes_per_generation, d.passes_per_target,
                                                      d.resumed_passes, d.ns_render_pass,
                                                      d.frame_repeated_bytes,
                                                      d.previous_equal_bytes, d.ns_hash_vertices,
                                                      d.passes_texels};
        // Deduplication of vertex uploads within the frame.
        const std::array<uint64_t, 3> dedupe = {d.hits_dedupe, d.dedupe_bytes,
                                                d.collisions_dedupe};
        const auto ddelta = [&](size_t i) { return dedupe[i] - dedupe_previous_[i]; };
        const auto delta = [&](size_t i) { return counters[i] - counters_previous_[i]; };
        NFSMW_REPORT_ROUTED(defer_report, "[nativo] C6 contadores: pases {} envios por subida llena {} ({:.1f} ms) "
                    "vertices {:.1f} MB indices {:.1f} MB samplers {} (cache {}); "
                    "{:.1f} ms en cambios de pase ({:.1f} us cada uno), {:.1f} ms copiando vertices",
                    delta(0), delta(1), double(delta(2)) / 1e6, double(delta(3)) / 1048576.0,
                    double(delta(4)) / 1048576.0, delta(5), delta(6), double(delta(7)) / 1e6,
                    delta(0) ? double(delta(7)) / 1e3 / double(delta(0)) : 0.0,
                    timed ? double(delta(8)) / 1e6 * double(recorded_items) / double(timed) : 0.0);
        if (interval_presented) {
          NFSMW_REPORT_ROUTED(defer_report, "[nativo] C6 vertices sin repetir: {} enlaces reaprovechados, {:.2f} MB que "
                      "NO se han copiado ({:.2f} MB por fotograma, {:.0f} % de los subidos){}",
                      ddelta(0), double(ddelta(1)) / 1048576.0,
                      double(ddelta(1)) / 1048576.0 / double(interval_presented),
                      delta(3) + ddelta(1)
                          ? 100.0 * double(ddelta(1)) / double(delta(3) + ddelta(1))
                          : 0.0,
                      ddelta(2) ? fmt::format("; *** {} COLISIONES: la tabla se queda corta ***",
                                              ddelta(2))
                                : "");
        }
        dedupe_previous_ = dedupe;
        NFSMW_REPORT_ROUTED(defer_report, "[nativo] C6 contadores: entradas de vertices calculadas {} ({:.1f} ms, "
                    "{:.1f} us cada una), reutilizadas {}",
                    delta(9), double(delta(10)) / 1e6,
                    delta(9) ? double(delta(10)) / 1e3 / double(delta(9)) : 0.0, delta(11));
        NFSMW_REPORT_ROUTED(defer_report, "[nativo] C6 pases: por bufer de comandos nuevo {}, por destinos {}, "
                    "reanudados tras copia o borrado {}; {:.1f} ms en vkCmdBegin/EndRenderPass; "
                    "area abierta {:.1f} Mtexels ({:.2f} Mtexels por fotograma)",
                    delta(12), delta(13), delta(14), double(delta(15)) / 1e6,
                    double(delta(19)) / 1e6,
                    interval_presented ? double(delta(19)) / 1e6 / double(interval_presented)
                                          : 0.0);
        if (interval_presented) {
          static constexpr const char* kTypes[] = {"resto",  "sombras", "escena",  "reflejo",
                                                   "cubo",   "menores", "copias",  "borrados",
                                                   "escena sin profundidad", "hueco"};
          std::string area;
          for (uint32_t c = 0; c < kGpuCategories; ++c) {
            const uint64_t t = d.texels_per_category[c] - category_texels_previous_[c];
            const uint64_t n = d.passes_per_category[c] - category_passes_previous_[c];
            if (!t && !n) {
              continue;
            }
            const uint64_t dib = d.draws_per_category[c] - category_draws_previous_[c];
            const uint64_t tri = d.triangles_per_category[c] - category_triangles_previous_[c];
            area += fmt::format(" {} {:.2f} Mtexels en {:.1f} pases ({:.0f} dibujos, {:.0f} k triangulos)",
                                kTypes[c], double(t) / 1e6 / double(interval_presented),
                                double(n) / double(interval_presented),
                                double(dib) / double(interval_presented),
                                double(tri) / 1e3 / double(interval_presented));
          }
          NFSMW_REPORT_ROUTED(defer_report, "[nativo] C6 area abierta por fotograma y tipo de destino:{}", area);
        }
        if (interval_presented) {
          // Draws without color, with and without a required pixel shader.
          NFSMW_REPORT_ROUTED(defer_report, "[nativo] C6 dibujos sin color por fotograma: {:.0f} con el pixel shader de sobra, "
                      "{:.0f} que lo necesitan (prueba de alfa, kill o profundidad); en el mapa de sombras "
                      "{:.0f} con la prueba de alfa puesta y {:.0f} sin ella",
                      double(d.useless_ps_draws - useless_ps_draws_previous_) /
                          double(interval_presented),
                      double(d.required_ps_draws - required_ps_draws_previous_) /
                          double(interval_presented),
                      double(d.shadows_active_alpha - shadows_active_alpha_previous_) /
                          double(interval_presented),
                      double(d.shadows_alpha_disabled - shadows_alpha_disabled_previous_) /
                          double(interval_presented));
        }
        shadows_active_alpha_previous_ = d.shadows_active_alpha;
        shadows_alpha_disabled_previous_ = d.shadows_alpha_disabled;
        useless_ps_draws_previous_ = d.useless_ps_draws;
        required_ps_draws_previous_ = d.required_ps_draws;
        category_texels_previous_ = d.texels_per_category;
        category_passes_previous_ = d.passes_per_category;
        category_draws_previous_ = d.draws_per_category;
        category_triangles_previous_ = d.triangles_per_category;
        // Which constants mode each interval used (test cvar nfsmw_nativo_constantes_ubo_alternar_s).
        NFSMW_REPORT_ROUTED(defer_report, "[nativo] C6 constantes por UBO: {} de {} envios", d.ubo_sends - ubo_sends_previous_,
                    d.sends - sends_previous_);
        {
          const uint64_t peeked = d.peeked_shared - peeked_shared_previous_;
          const uint64_t changed = d.changed_shared - changed_shared_previous_;
          peeked_shared_previous_ = d.peeked_shared;
          changed_shared_previous_ = d.changed_shared;
          if (peeked > 0) {
            NFSMW_REPORT_ROUTED(defer_report, "[nativo] C6 constantes compartidas: {} de {} dibujos cambian los 488 bytes "
                        "({:.1f} %); los otros solo pagan el memcmp",
                        changed, peeked, 100.0 * double(changed) / double(peeked));
          }
        }
        ubo_sends_previous_ = d.ubo_sends;
        sends_previous_ = d.sends;
        // Diagnostic: whether the game has touched the gamma ramp since the previous report.
        {
          uint64_t total = 0;
          for (uint64_t n : ramp_writes_) {
            total += n;
          }
          if (total != ramp_writes_previous_) {
            ramp_writes_previous_ = total;
            uint32_t distinct_list = 0;
            for (uint32_t i = 0; i < 256; ++i) {
              const uint16_t identity = uint16_t(i * 0x3FF / 0xFF);
              if (gamma_ramp_[i][0] != identity || gamma_ramp_[i][1] != identity ||
                  gamma_ramp_[i][2] != identity) {
                ++distinct_list;
              }
            }
            const auto sample = [&](uint32_t i) {
              return fmt::format("[{}]={}/{}/{}", i, gamma_ramp_[i][0], gamma_ramp_[i][1], gamma_ramp_[i][2]);
            };
            NFSMW_REPORT_ROUTED(defer_report, "[nativo] C2 rampa de gamma: escrituras modo {} indice {} secuencial {} PWL {} 30 bits {} "
                        "1926 {} mascara {} (vale {}); entradas de la tabla de 256 distintas de la identidad {}; "
                        "{} {} {} {} {} (identidad: i*1023/255); version {}",
                        ramp_writes_[0], ramp_writes_[1], ramp_writes_[2], ramp_writes_[3],
                        ramp_writes_[4], ramp_writes_[5], ramp_writes_[6], ramp_mask_, distinct_list,
                        sample(0), sample(64), sample(128), sample(192), sample(255), ramp_version_);
            // The whole table, to compare the image with the Xbox 360 one outside the game.
            bool equal_channels = true;
            for (const auto& entry : gamma_ramp_) {
              equal_channels = equal_channels && entry[0] == entry[1] && entry[0] == entry[2];
            }
            static constexpr const char* kChannelNames[3] = {"roja", "verde", "azul"};
            for (uint32_t c = 0; c < (equal_channels ? 1u : 3u); ++c) {
              std::string item_list;
              item_list.reserve(256 * 5);
              for (uint32_t i = 0; i < 256; ++i) {
                item_list += fmt::format("{}{}", i ? " " : "", gamma_ramp_[i][c]);
              }
              NFSMW_REPORT_ROUTED(defer_report, "[nativo] C2 rampa de gamma, tabla {}: {}",
                          equal_channels ? "de los tres canales" : kChannelNames[c], item_list);
            }
          }
        }
        if (delta(16) || delta(17)) {
          NFSMW_REPORT_ROUTED(defer_report, "[nativo] C6 vertices repetidos: {:.1f} MB copiados; {:.1f} MB repetidos en el "
                      "mismo fotograma y {:.1f} MB iguales a un fotograma anterior; {:.1f} ms en hashes",
                      double(delta(3)) / 1048576.0, double(delta(16)) / 1048576.0,
                      double(delta(17)) / 1048576.0, double(delta(18)) / 1e6);
        }
        counters_previous_ = counters;
      }
      NFSMW_REPORT_ROUTED(defer_report, "[nativo] C6: dibujados={} rechazados={} pipelines={} ({} ms creando) "
                  "texturas={} ({} MB) subidas de textura={} MB subidos={}; rechazos por "
                  "causa:{}",
                  d.drawn, d.rejected, d.pipelines, d.ms_pipelines, d.textures,
                  d.textures_megas, d.texture_uploads, d.uploaded_megas, causes);
    }
    if (shaders_.loaded()) {
      const ShaderStats e = shaders_.Stats();
      NFSMW_REPORT_ROUTED(defer_report, "[nativo] C5a: cargas={} distintos={} identificados={} sin_identificar={} "
                  "ambiguos={} exportes_anulados={}; dibujos con VS y PS={} sin VS={} sin PS={}; pares VS/PS={}",
                  e.loads + cached_loads_, e.distinct_items, e.identified, e.without_identify, e.ambiguos,
                  e.overridden_exports,
                  identified_draws_, draws_without_vs_, draws_without_ps_, pairs_.size());
      // IM_LOAD without memcmp (CargarShaderCacheado).
      {
        const uint64_t without_memcmp = im_i_without_memcmp_;
        const uint64_t con_memcmp = im_i_con_memcmp_;
        NFSMW_REPORT_ROUTED(defer_report,
                            "[nativo] C5a IM_LOAD sin memcmp (build 184): fase {}; en el intervalo {} cargas sin leer "
                            "el microcodigo ({:.1f} %) y {} con memcmp ({} comparadas con el atajo e iguales, {} por "
                            "una escritura en su ranura, {} sin apuntar por una escritura a medias); comprobadas "
                            "para aplicar {} de {}{} | escrituras avisadas desde el arranque: {} parches en su sitio, "
                            "{} en la copia del anillo, {} otros, {} creaciones",
                            ImShortcutPhaseName(), without_memcmp,
                            without_memcmp + con_memcmp
                                ? 100.0 * double(without_memcmp) / double(without_memcmp + con_memcmp)
                                : 0.0,
                            con_memcmp, im_i_checked_, im_i_changed_version_, im_i_overridden_,
                            std::min(im_checked_shortcut_, kImShortcutToPeek), kImShortcutToPeek,
                            im_phase_shortcut_ == kImPeekingShortcut && im_checked_shortcut_ >= kImShortcutToPeek
                                ? " (sin ningun parche en su sitio visto: el gancho de sub_825A2FB8 no corre)"
                                : "",
                            ucode::g_patches_in_place_.load(std::memory_order_relaxed),
                            ucode::g_patches_in_copy.load(std::memory_order_relaxed),
                            ucode::g_other_patches.load(std::memory_order_relaxed),
                            ucode::g_creations.load(std::memory_order_relaxed));
        im_i_without_memcmp_ = im_i_con_memcmp_ = im_i_checked_ = im_i_changed_version_ = im_i_overridden_ = 0;
      }
      // IM_LOAD_IMMEDIATE with an exact cache (CargarInmediato).
      if (imm_phase_ != kImmWithoutStart) {
        const uint64_t total =
            imm_i_hits_ + imm_i_checked_ + imm_i_failures_ + imm_i_return_ + imm_i_without_cache_;
        uint32_t occupied_items = 0;
        for (const auto& per_type : imm_cache_) {
          for (const auto& desc_set : per_type) {
            for (const ImmediateLoad& via : desc_set) {
              occupied_items += via.signature != 0 ? 1 : 0;
            }
          }
        }
        NFSMW_REPORT_ROUTED(defer_report,
                            "[nativo] C5a IM_LOAD_IMMEDIATE con cache (build 184): fase {}; en el intervalo {} "
                            "cargas: {} aciertos sin girar ni identificar ({:.1f} %), {} aciertos comprobados contra "
                            "el camino de siempre, {} fallos (vias nuevas), {} por dar la vuelta al anillo y {} sin "
                            "cache; vias ocupadas {} de {}; aciertos comprobados desde el arranque {}",
                            ImmediatePhaseName(), total, imm_i_hits_,
                            total ? 100.0 * double(imm_i_hits_) / double(total) : 0.0, imm_i_checked_,
                            imm_i_failures_, imm_i_return_, imm_i_without_cache_, occupied_items,
                            2 * kImmPaths * (uint32_t(1) << kImmSetBits), imm_checked_);
        imm_i_hits_ = imm_i_checked_ = imm_i_failures_ = imm_i_return_ = imm_i_without_cache_ = 0;
      }
      // Measurement only: fences with pending vertex copies (AnotarVallaCopias).
      NFSMW_REPORT_ROUTED(defer_report,
                          "[nativo] C6 vallas con copias de vertices pendientes (build 185, solo medida): {} de {} "
                          "vallas e interrupciones en el intervalo (como mucho {} copias pendientes)",
                          fences_with_copies_, fences_total_, copies_fences_max_);
      fences_with_copies_ = fences_total_ = 0;
      copies_fences_max_ = 0;
      // Measurement only (AnotarJuegoPorDelante).
      NFSMW_REPORT_ROUTED(defer_report,
                          "[nativo] C6 juego por delante del anillo (build 186, solo medida): al terminar el "
                          "anillo un fotograma, el juego llevaba 0 Swaps mas en {}, 1 en {} y 2 o mas en {} "
                          "(como mucho {}; diferencia del primer fotograma {})",
                          ahead_game_[0], ahead_game_[1], ahead_game_[2], ahead_game_max_,
                          ahead_game_first_);
      ahead_game_[0] = ahead_game_[1] = ahead_game_[2] = 0;
      ahead_game_max_ = 0;
      const HookStats g = GetHookStats();
      NFSMW_REPORT_ROUTED(defer_report, "[nativo] C5b: shaders creados VS={} (conocidos {}) PS={} (conocidos {}); "
                  "dibujos emparejados={} sin registro={} registros saltados={} perdidos={}; "
                  "con VS={} con PS={}; registros con shaders distintos de los IM_LOAD={} "
                  "(dibujos que se quedan sin registro por eso={}); dibujos con la identidad "
                  "del anillo={}; variantes VS={} "
                  "(longitud distinta {}, elementos sin fetch {})",
                  g.vs_created, g.vs_known, g.ps_created, g.ps_known,
                  matched_draws_, draws_without_reg_, skipped_regs_, g.lost_items,
                  draws_with_vs_, draws_with_ps_, inconsistent_candidates_,
                  inconsistent_draws_, draws_per_im_load_, views_variants_.size(),
                  distinct_length_variants_, elements_without_fetch_);
    }
    if (!tracked_histogram_ && swaps_.load() >= 60) {
      tracked_histogram_ = true;
      std::string lines;
      for (uint32_t op = 0; op < opcodes_.size(); ++op) {
        const uint64_t n = opcodes_[op].load();
        if (n) {
          lines += fmt::format(" {:02X}={}", op, n);
        }
      }
      NFSMW_REPORT_ROUTED(defer_report, "[nativo] paquetes tipo 3 por opcode (hex=cuenta):{}", lines);
    }
  }

#undef NFSMW_REPORT_ROUTED

  struct Framebuffer {
    uint64_t version = UINT64_MAX;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
  };

  rex::ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<rex::ui::vulkan::VulkanProvider> provider_;
  std::unique_ptr<rex::ui::Presenter> presenter_;

  rex::memory::Memory* memory_ = nullptr;
  rex::runtime::FunctionDispatcher* dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  bool registered_mmio_ = false;

  std::vector<uint32_t> regs_;
  std::vector<std::atomic<uint8_t>> seen_;

  std::atomic<uint32_t> callback_{0};
  std::atomic<uint32_t> data_callback_{0};
  std::atomic<uint32_t> base_ring_{0};
  std::atomic<uint32_t> words_ring_{0};
  std::atomic<uint32_t> ring_generation_{0};
  std::atomic<uint32_t> returned_read_{0};
  std::atomic<uint32_t> write_pointer_{0};
  // Ring wakeups: the game thread counts the writes; the ring thread counts the rest.
  std::atomic<uint64_t> wptr_writes_{0};
  uint64_t wptr_writes_previous_ = 0;
  // Which core the ring thread runs on and how many times it has been moved to another one.
  int ring_core_ = -1;
  uint64_t ring_migrations_ = 0;
  uint64_t ring_migrations_previous_ = 0;
  uint64_t ring_turns_ = 0;
  uint64_t ring_turns_previous_ = 0;
  uint64_t ring_turns_with_data_ = 0;
  uint64_t ring_turns_with_data_previous_ = 0;
  uint64_t exhausted_ring_waits_ = 0;
  uint64_t exhausted_ring_waits_previous_ = 0;
  std::atomic<uint32_t> counter_{0};
  uint64_t bin_mask_ = 0xFFFFFFFFull;
  uint64_t bin_select_ = 0xFFFFFFFFull;

  std::atomic<bool> active_{false};
  std::mutex mutex_ring_;
  std::condition_variable cv_ring_;
  rex::system::object_ref<rex::system::XHostThread> vblank_thread_;
  rex::system::object_ref<rex::system::XHostThread> ring_thread_;

  std::atomic<uint64_t> swaps_{0};
  std::atomic<uint64_t> vblanks_{0};
  std::atomic<uint64_t> packets_{0};
  std::atomic<uint64_t> interrupts_{0};
  uint64_t fences_total_ = 0;       // AnotarVallaCopias, ring thread only
  uint64_t fences_with_copies_ = 0;
  size_t copies_fences_max_ = 0;
  uint64_t ahead_game_[3] = {};  // AnotarJuegoPorDelante, ring thread only
  int64_t ahead_game_max_ = 0;
  int64_t ahead_game_first_ = 0;
  bool valid_first_ahead_game_ = false;
  std::atomic<uint64_t> indirect_{0};
  std::atomic<uint64_t> memory_writes_{0};
  std::atomic<uint64_t> exhausted_waits_{0};
  std::atomic<uint64_t> draws_{0};
  std::atomic<uint64_t> copies_{0};
  std::atomic<uint32_t> last_target_copy_{0};
  std::atomic<uint32_t> last_control_copy_{0};
  std::atomic<uint32_t> last_info_copy_{0};
  std::atomic<uint32_t> last_pitch_copy_{0};
  std::atomic<uint32_t> swap_frontbuffer_{0};
  std::atomic<uint32_t> swap_width_{0};
  std::atomic<uint32_t> swap_height_{0};
  std::array<std::atomic<uint64_t>, 128> opcodes_{};
  bool tracked_histogram_ = false;  // only the ring thread touches it
  // Step C2: render targets, copies and presentation (ring thread only).
  std::unique_ptr<NativeTargets> targets_;
  bool failed_targets_ = false;
  // Step C5a: identified shaders (ring thread only).
  NativeShaders shaders_;
  const ShaderEntry* current_vs_ = nullptr;
  const ShaderEntry* current_ps_ = nullptr;
  std::vector<uint32_t> microcode_;
  uint64_t identified_draws_ = 0;
  uint64_t draws_without_vs_ = 0;
  uint64_t draws_without_ps_ = 0;
  std::unordered_set<uint64_t> pairs_;
  // Step C5b: matching with the game's Draw* calls (ring thread only).
  std::deque<DrawRecord> pending_;
  std::unordered_map<uint32_t, const ShaderEntry*> objects_;
  uint64_t objects_generation_ = UINT64_MAX;
  // direct cache in front of objetos_ (ShaderDeObjetoCacheado).
  struct ObjectMemo {
    uint32_t object = UINT32_MAX;  // UINT32_MAX = empty slot: not a guest object
    const ShaderEntry* entry = nullptr;
  };
  std::array<ObjectMemo, 16> objects_memo_{};
  uint64_t last_shaders_pair_ = UINT64_MAX;  // ContarDibujoShaders: the previous draw's pair
  std::span<const uint32_t> microcode_vs_;  // from the IM_LOAD cache or from vs_inmediato_
  const ShaderEntry* draw_vs_ = nullptr;
  const ShaderEntry* draw_ps_ = nullptr;
  uint64_t matched_draws_ = 0;
  uint64_t draws_without_reg_ = 0;
  uint64_t skipped_regs_ = 0;
  uint64_t draws_with_vs_ = 0;
  uint64_t draws_with_ps_ = 0;
  uint64_t inconsistent_candidates_ = 0;
  uint64_t inconsistent_draws_ = 0;
  const ShaderEntry* vs_coherency_ = nullptr;
  uint64_t generation_coherency_ = UINT64_MAX;
  bool coherency_ = false;
  const ShaderEntry* vs_tracked_ = nullptr;
  uint64_t generation_tracked_ = UINT64_MAX;
  uint32_t c5b_warnings_ = 0;
  uint64_t distinct_length_variants_ = 0;
  uint64_t elements_without_fetch_ = 0;
  std::unordered_set<uint64_t> views_variants_;
  // Steps C3-C6: generations, so unchanged data is not uploaded again.
  uint64_t generacion_vs_ = 0;  // one per distinct VS microcode (CargarShaderCacheado)
  // Cache de IM_LOAD (CargarShaderCacheado): 2 tipos x 128 conjuntos x 2 vias.
  struct ShaderLoad {
    uint32_t type_address = 0;
    uint32_t byte_size = 0;
    std::vector<uint32_t> raw;  // as it is in game memory
    std::vector<uint32_t> host;   // swapped: what gets identified and what the vertex input reads
    const ShaderEntry* entry = nullptr;
    uint64_t fingerprint = 0;      // XXH3 de host
    uint64_t generacion = 0;  // from generaciones_microcodigo_, when this content was loaded
    uint64_t use = 0;         // the least recently used way is the one replaced
    // The versions of its slot this content was checked against guest memory with
    // (microcodigo::AntesDeLeer and DespuesDeLeer). validada = false: it must be compared with memcmp again.
    bool validated = false;
    uint32_t slot_version = 0;
    uint32_t version_global = 0;
  };
  std::array<std::array<std::array<ShaderLoad, 2>, 128>, 2> cache_loads_{};
  uint64_t tic_loads_ = 0;
  uint64_t cached_loads_ = 0;
  uint64_t microcode_generations_ = 0;
  // IM_LOAD without memcmp (CargarShaderCacheado). Ring thread only.
  static constexpr int kImShortcutWithoutStart = -1;
  static constexpr int kImPeekingShortcut = 0;
  static constexpr int kImApplyingShortcut = 1;
  static constexpr int kImShortcutDisabled = 2;
  static constexpr uint64_t kImShortcutToPeek = 200000;
  static constexpr uint64_t kImCheckShortcutEvery = 4096;  // potencia de 2
  int im_phase_shortcut_ = kImShortcutWithoutStart;
  uint64_t im_turn_shortcut_ = 0;
  uint64_t im_checked_shortcut_ = 0;  // agreements with the memcmp since startup
  // Since the last report.
  uint64_t im_i_without_memcmp_ = 0;
  uint64_t im_i_con_memcmp_ = 0;
  uint64_t im_i_checked_ = 0;
  uint64_t im_i_changed_version_ = 0;
  uint64_t im_i_overridden_ = 0;
  std::vector<uint32_t> immediate_vs_;  // VS from IM_LOAD_IMMEDIATE, uncached
  // Exact IM_LOAD_IMMEDIATE cache (CargarInmediato): 2 types x 32 sets x 8 ways. Ring thread only.
  static constexpr uint32_t kImmSetBits = 5;
  static constexpr uint32_t kImmPaths = 8;
  static constexpr int kImmWithoutStart = -1;
  static constexpr int kImmPeeking = 0;
  static constexpr int kImmApplying = 1;
  static constexpr int kImmDisabled = 2;
  static constexpr uint64_t kImmToPeek = 20000;
  static constexpr uint64_t kImmCheckEvery = 1024;  // potencia de 2
  std::array<std::array<std::array<ImmediateLoad, kImmPaths>, (size_t(1) << kImmSetBits)>, 2> imm_cache_{};
  std::array<std::array<uint8_t, (size_t(1) << kImmSetBits)>, 2> imm_last_{};  // way of the last hit
  int imm_phase_ = kImmWithoutStart;
  uint64_t imm_turn_ = 0;
  uint64_t inm_tic_ = 0;
  uint64_t imm_checked_ = 0;  // hits checked and equal since startup
  // Since the last report.
  uint64_t imm_i_hits_ = 0;
  uint64_t imm_i_checked_ = 0;
  uint64_t imm_i_failures_ = 0;
  uint64_t imm_i_return_ = 0;
  uint64_t imm_i_without_cache_ = 0;
  const ShaderEntry* vs_mapped_ = nullptr;
  uint64_t generation_mapped_ = UINT64_MAX;
  // How many constant writes do not change the value. Tells whether comparing before bumping
  // the generation pays off, and by how much.
  uint64_t written_constants_ = 0;
  uint64_t constants_without_change_ = 0;
  uint64_t constants_blocks_ = 0;
  uint64_t constants_blocks_without_change_ = 0;
  uint64_t generacion_constantes_vs_ = 0;
  int regs_in_block_ = -1;  // -1 = cvar not read yet
  uint64_t generacion_constantes_ps_ = 0;
  // Generations of the fetch constants and the viewport state (viewport, scissor, clip). They
  // travel in PeticionDibujo so the translator does not redo per draw what has not changed.
  uint64_t generacion_fetch_ = 0;
  uint64_t framing_generation_ = 0;
  uint64_t written_fetch_ = 0;
  uint64_t fetch_without_change_ = 0;
  uint64_t framing_written_ = 0;
  uint64_t framing_without_change_ = 0;
  // Ring thread times since the last report (that thread only).
  uint64_t ring_time_ns_ = 0;
  uint64_t draws_time_ns_ = 0;
  uint64_t copies_time_ns_ = 0;
  uint64_t present_time_ns_ = 0;
  uint64_t measured_draws_ = 0;
  uint64_t draws_stopwatch_phase_ = 0;  // phase of the DibujarNativo timer, never reset
  uint64_t measures_copies_ = 0;
  uint64_t measures_presents_ = 0;
  // Ring breakdown per packet: registers (types 0 and 1) and each type-3 opcode.
  uint64_t regs_time_ns_ = 0;
  uint64_t regs_count_ = 0;
  uint64_t block_words_ = 0;   // Measurement only
  // Vector path of EscribirRegistrosEnBloque and its guard.
  static constexpr uint64_t kBlocksToVerify = 200000;
  int vector_regs_ = -1;
  uint64_t verified_blocks_ = 0;
  std::vector<uint32_t> verification_copy_;
  // Phase 1 of the Direct3D-level renderer (CompararSombra).
  MirrorSnapshot shadow_snapshot_;
  uint64_t shadow_draws_ = 0;
  uint64_t shadow_lost_ = 0;
  uint64_t shadow_compared_state_ = 0;
  uint64_t shadow_distinct_state_ = 0;
  uint64_t shadow_compared_fetch_ = 0;
  uint64_t shadow_distinct_fetch_ = 0;
  uint64_t shadow_compared_constants_ = 0;
  uint64_t shadow_distinct_constants_ = 0;
  std::unordered_map<uint32_t, uint64_t> shadow_per_reg_;
  // Phase 2 of the Direct3D-level renderer (ProcesarMarcador). Ring thread only.
  static constexpr uint64_t kFastSegmentsToVerify = 200000;
  int fast_marker_ = 1;  // 0: the fast path was turned off by a mismatch (VerificarTramoMarcador)
  uint64_t verified_fast_segments_ = 0;
  std::vector<uint32_t> marker_copy_;
  std::vector<uint32_t> plane_marker_;  // a marker that wrapped around the ring, made contiguous (should not happen)
  uint64_t applied_markers_ = 0;
  uint64_t checked_markers_ = 0;
  uint64_t distinct_markers_ = 0;
  uint64_t bad_markers_ = 0;
  uint64_t segments_marker_ = 0;
  uint64_t slow_segments_marker_ = 0;  // through the regular path (EscribirRegistrosEnBloque or one at a time)
  uint64_t words_marker_ = 0;
  uint32_t marker_warnings_ = 0;
  // Phase 2b: the Draw* record carried by the last marker, for the DRAW_INDX that follows. It stays
  // until a draw accepts it or the next Draw* record arrives, like the head of the queue.
  DrawRecord marker_draw_;
  uint32_t mode_marker_draw_ = 0;  // 0 none, kDibujoAplicar or kDibujoComprobar
  uint16_t vegetation_game_ = 0;  // kVeg* flags of the record used to draw (0: none)
  uint64_t draws_with_marker_reg_ = 0;
  uint64_t rejected_marker_regs_ = 0;
  uint64_t draw_checks_ = 0;
  uint64_t distinct_draw_checks_ = 0;
  uint64_t reg_other_draw_checks_ = 0;
  uint32_t marker_draw_warnings_ = 0;
  uint64_t released_words_ = 0;
  uint64_t packets_stopwatch_ = 0;
  std::array<uint64_t, 128> opcode_time_ns_{};
  std::array<uint64_t, 128> opcode_count_{};
  // WAIT_REG_MEM per polled register or address since the last report.
  struct WaitStats {
    uint64_t times = 0;
    uint64_t turns = 0;  // failed polls, each with a 1 ms wait
    uint32_t reference = 0;
    uint32_t mask = 0;
    uint32_t info = 0;
  };
  std::unordered_map<uint64_t, WaitStats> waits_;
  // Draw stages at the previous report (for the deltas).
  uint64_t drawn_previous_ = 0;
  uint64_t timed_previous_ = 0;
  uint64_t vegetation_soon_previous_ = 0;
  std::array<uint64_t, kDrawStages> stages_previous_{};
  uint64_t discard_with_previous_ = 0;
  uint64_t discard_without_previous_ = 0;
  std::array<uint64_t, 20> counters_previous_{};
  std::array<uint64_t, 3> dedupe_previous_{};  // "C6 contadores"
  std::array<uint64_t, kGpuCategories> category_texels_previous_{};  // "C6 area abierta"
  std::array<uint64_t, kGpuCategories> category_passes_previous_{};
  uint64_t useless_ps_draws_previous_ = 0;
  uint64_t shadows_active_alpha_previous_ = 0;
  uint64_t shadows_alpha_disabled_previous_ = 0;
  uint64_t required_ps_draws_previous_ = 0;
  std::array<uint64_t, kGpuCategories> category_draws_previous_{};
  std::array<uint64_t, kGpuCategories> category_triangles_previous_{};
  uint64_t ubo_sends_previous_ = 0;  // "C6 constantes por UBO"
  uint64_t peeked_shared_previous_ = 0;  // "C6 constantes compartidas"
  uint64_t changed_shared_previous_ = 0;
  std::array<uint64_t, kSwapBuckets> swap_buckets_previous_{};  // "C2: intervalos entre Swaps"
  uint64_t gpu_overlaps_previous_ = 0;
  uint64_t present_cost_previous_[12] = {};  // "C2: presentar"
  uint64_t sends_previous_ = 0;
  // Gamma ramp loaded by the game (AnotarRampaGamma), identity at startup. The output applies it.
  std::array<std::array<uint16_t, 3>, 256> gamma_ramp_ = [] {
    std::array<std::array<uint16_t, 3>, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      t[i].fill(uint16_t(i * 0x3FF / 0xFF));
    }
    return t;
  }();
  uint32_t ramp_component_ = 0;
  uint32_t ramp_mask_ = 0b111;  // default DC_LUT_WRITE_EN_MASK (register_table.inc)
  uint64_t ramp_version_ = 0;
  uint64_t sent_ramp_version_ = 0;  // the identity is already in the output
  std::array<uint64_t, kRegRampLast - kRegRampFirst + 1> ramp_writes_{};
  uint64_t ramp_writes_previous_ = 0;
  // Occlusion queries (ConsultaOclusion), ring thread only.
  uint32_t oclusion_base_ = 0;                    // structure of the open query (0 = none)
  std::unordered_set<uint32_t> bases_oclusion_;   // structures seen in an Issue(BEGIN)
  uint32_t occlusion_warnings_ = 0;                  // details of the first ones in the log
  // Finished queries per render target kind (0: under 640 or without MSAA on the Xbox 360; 1: scene,
  // scaled) in the interval, and the largest count written for each since the start.
  std::array<uint64_t, 2> occlusion_per_target_{};
  std::array<uint32_t, 2> max_occlusion_per_target_{};
  uint32_t scene_occlusion_warnings_ = 0;
  // Diagnostic nfsmw_nativo_diag_constantes_ps.
  std::string text_list_constants_;
  std::unordered_set<uint32_t> list_constants_;
  std::unordered_map<uint32_t, Clock::time_point> constants_last_;
  // Prueba nfsmw_nativo_oclusion_alternar_s.
  bool started_test_occlusion_ = false;
  bool faked_test_occlusion_ = false;
  Clock::time_point start_test_occlusion_{};
  // Started, finished with a measurement, finished without a measurement yet, started without their
  // end, written with the faked count outside a pair, sum of the written measurements and the
  // largest one written.
  std::array<uint64_t, 7> counters_occlusion_{};
  std::array<uint64_t, 7> counters_occlusion_previous_{};
  uint64_t host_occlusion_previous_[5] = {};  // DestinosNativos::EstadisticasOclusion at the previous report
  // GPU time at the previous report (C2).
  uint64_t gpu_previous_ns_ = 0;
  uint64_t gpu_waits_previous_ = 0;     // C2 report: ring waits for the GPU
  uint64_t gpu_waits_previous_ns_ = 0;
  uint64_t jobs_gpu_previous_ = 0;
  uint64_t gpu_job_previous_ns_ = 0;
  uint64_t record_cost_previous_[6] = {};  // C2 report: Grabar and readbacks
  uint64_t gpu_jobs_previous_ = 0;
  uint64_t gpu_previous_presented_ = 0;
  std::array<uint64_t, kGpuCategories> gpu_categories_previous_{};
  std::array<uint64_t, kGpuCategories> category_fragments_previous_{};
  std::array<uint64_t, kGpuCategories> category_vertices_previous_{};
  std::array<uint64_t, kGpuCategories> category_primitives_previous_{};
  uint64_t diagnostic_frames_previous_ = 0;
  std::array<uint64_t, 4> bucket_copies_previous_{};
  std::array<uint64_t, 4> bucket_pixels_previous_{};
  // Real scale of the GPU timestamps (C2 report).
  uint64_t gpu_mark_previous_ns_ = 0;
  uint64_t precise_jobs_previous_ = 0;
  Clock::time_point mark_clock_previous_{};
  double real_scale_ns_ = 0.0;
  double marks_scale_ns_ = 0.0;
  Clock::time_point last_time_report_{};
  Clock::time_point last_report_{};
  // Trace of one frame (nfsmw_nativo_diag_fotograma_s).
  Clock::time_point system_start_{};
  bool tracing_ = false;
  bool done_trace_ = false;
  // nfsmw_nativo_diag_constantes_ps is not empty. Refreshed on every Swap.
  bool diag_active_constants_ = false;
  std::unordered_set<uint32_t> dumped_textures_;  // nfsmw_nativo_diag_vertices_ps
  uint32_t traces_ = 0;
  uint32_t matched_reason_ = 0;  // 0 emparejado, 1 sin registro, 2 registro incoherente
  uint32_t vs_object_ = 0;           // objects of the paired record
  uint32_t ps_object_ = 0;
  // Ring identity for draws without a usable record (UsarIdentidadDelAnillo).
  std::unordered_map<uint64_t, const ShaderEntry*> vs_per_microcode_;
  uint64_t vs_fingerprint_ = 0;
  uint64_t generation_vs_fingerprint_ = UINT64_MAX;
  uint64_t draws_per_im_load_ = 0;

  // Vulkan objects for the test image: only the ring thread touches them.
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer commands_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  bool pending_fence_ = false;
  std::array<Framebuffer, rex::ui::vulkan::VulkanPresenter::kMaxActiveGuestOutputImageVersions>
      framebuffers_{};
  size_t next_framebuffer_ = 0;
};

}  // namespace

uint64_t NativeSwaps() {
  return g_native_swaps.load(std::memory_order_relaxed);
}

bool Active() {
  return REXCVAR_GET(nfsmw_renderizador) == "nativo";
}

std::unique_ptr<rex::system::IGraphicsSystem> CreateGraphicSystem() {
  return std::make_unique<NativeGraphicsSystem>();
}

}  // namespace nfsmw::nativo
