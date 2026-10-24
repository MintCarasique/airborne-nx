// nfsmw - native renderer, step C5b (see native_hooks.h).

#include "native_hooks.h"

#include "native_shaders.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "deferred_report.h"  // deferred reports

#include <atomic>
#include <chrono>
#include <mutex>
#include <span>
#include <unordered_map>
#include <array>  // nfsmw_d3d_vegetacion_juego
#include <string>

REXCVAR_DEFINE_BOOL(nfsmw_nativo_sombra_d3d, false, "NFSMW",
                    "Renderizador nativo (24/09, fase 1 del renderizador a nivel de Direct3D): en 1 de cada 64 "
                    "dibujos fotografia el espejo de registros del dispositivo y el anillo lo compara con lo que "
                    "lee de los paquetes (linea «sombra D3D»). No cambia lo que se dibuja")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Phase 2b of the Direct3D-level renderer: see g_dibujo_en_curso and AnotarDibujo below.
REXCVAR_DEFINE_BOOL(nfsmw_d3d_marcador_registro, true, "NFSMW",
                    "Renderizador nativo (25/09, fase 2b del renderizador a nivel de Direct3D): el marcador del "
                    "FlushState de cada Draw* lleva su registro (VS, PS, argumentos) y el anillo lo usa sin la cola "
                    "ni la busqueda de EmparejarDibujo. Empieza comprobando contra la busqueda y se apaga solo al "
                    "primer desacuerdo. false = como antes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Shadow map vegetation filtered on the game thread (see DecidirVegetacion).
REXCVAR_DEFINE_BOOL(nfsmw_d3d_vegetacion_juego, true, "NFSMW",
                    "Renderizador nativo (25/09, build 184): los DrawVertices y DrawIndexedVertices que el anillo "
                    "tiraria como vegetacion del mapa de sombras (sin color, con prueba de alfa o descarte) se saltan "
                    "enteros en el hilo del juego: sin FlushState, DRAW_INDX ni registro. Empieza mirando (el anillo "
                    "comprueba el veredicto de cada dibujo) y se apaga solo al primer desacuerdo. false = como antes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// The settings of the ring's vegetation discard (nfsmw_nativo_dibujos.cpp): the game side checks the
// same ones.
REXCVAR_DECLARE(bool, nfsmw_nativo_ps_solo_alfa);
REXCVAR_DECLARE(int32_t, nfsmw_nativo_ps_solo_alfa_alternar_s);
REXCVAR_DECLARE(bool, nfsmw_sombras_sin_vegetacion);

namespace nfsmw::native_renderer {
namespace {

// Where SetVertexShader (sub_8259C2A8) and SetPixelShader (sub_8259BDC0) store the bound shader inside
// the device object (from the recompiled code).
constexpr uint32_t kVsDevice = 0x4FE8;
constexpr uint32_t kPsDevice = 0x3290;

// One producer (the thread using D3D) and one consumer (the ring thread).
constexpr uint32_t kQueueSize = uint32_t(1) << 16;

std::atomic<NativeShaders*> g_shaders{nullptr};

std::mutex g_mutex;
std::unordered_map<uint32_t, const ShaderEntry*> g_objects;
std::atomic<uint64_t> g_generacion{0};
std::atomic<uint64_t> g_vs_created{0};
std::atomic<uint64_t> g_vs_known{0};
std::atomic<uint64_t> g_ps_created{0};
std::atomic<uint64_t> g_ps_known{0};
std::atomic<uint32_t> g_warnings{0};

DrawRecord g_queue[kQueueSize];
std::atomic<uint32_t> g_write{0};
std::atomic<uint32_t> g_read{0};
std::atomic<uint64_t> g_lost{0};

uint32_t ReadBe(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// Phase 1 of the Direct3D-level renderer (see InstantaneaEspejo in the header).
constexpr uint32_t kSnapshotEvery = 64;            // 1 in 64 draws
constexpr uint32_t kSnapshots = 256;              // snapshots in flight between the game thread and the ring thread
constexpr uint32_t kFetchDevice = 0x480;
constexpr uint32_t kVsConstantsDevice = 0x780;
constexpr uint32_t kPsConstantsDevice = 0x1780;

std::atomic<uint64_t> g_group_mask[kMirrorGroups];
std::atomic<uint32_t> g_group_offset[kMirrorGroups];  // 0 = aun no se ha visto

struct Snapshot {
  std::atomic<uint64_t> sequence{0};  // 0 while being written
  MirrorSnapshot payload;
};
Snapshot g_snapshots[kSnapshots];
uint64_t g_next_snapshot = 1;  // game thread only
uint32_t g_snapshots_counter = 0;

int GroupOf(uint32_t base_reg) {
  if (base_reg >= 0x2000 && base_reg < 0x2400 && (base_reg & 0x7F) == 0) {
    return int((base_reg - 0x2000) >> 7);
  }
  return base_reg == 0x4900 ? 8 : -1;
}

/*
 * Phase 2b: the draw record inside the marker.
 * Each Draw* stores its record before calling the original, and its FlushState runs inside the
 * original. With 2b the record does not go straight to the queue: it stays in g_dibujo_en_curso, that
 * FlushState takes it (TomarDibujoEnCurso) and puts it in its marker, and the DRAW_INDX that follows in
 * the ring uses it without a lookup if it accepts it. If the marker cannot carry it (no room, phase 2 off)
 * or nobody takes it, it goes to the queue as usual (EntregarDibujo, TerminarDibujo).
 * Self-checking guard: in the observing phase the record goes both ways (queue and marker in check
 * mode); the draw uses the usual lookup and the ring compares the shaders from the lookup with those from
 * the marker's record (CompararDibujoMarcador). After kComprobacionesDibujo matches, marker only; even so
 * 1 in kComprobarDibujoCada is still checked. At the first difference it is off for the whole session
 * ("[d3d_marcador] registro de dibujo: DIFERENCIA" in the log) and records go back to the queue.
 */
constexpr uint64_t kDrawChecks = 20000;
constexpr uint64_t kCheckDrawEvery = 1024;  // potencia de 2
DrawRecord g_draw_in_running;  // only the thread using D3D (the Draw* calls and their FlushState)
bool g_pending_draw = false;
uint64_t g_draw_turn = 0;
std::atomic<bool> g_draw_disabled{false};
std::atomic<bool> g_applying_draw{false};
std::atomic<uint64_t> g_equal_draw{0};  // written only by the ring thread
std::atomic<bool> g_distinct_draw{false};
// The first disagreement, written by the ring thread before g_dibujo_distinto (release).
uint32_t g_draw_that = 0;
DrawRecord g_search_draw;
DrawRecord g_marker_draw;
bool g_search_has_draw = false;
bool g_marker_has_draw = false;
// Report every 10 s (only the thread using D3D).
uint64_t g_i_draws = 0;
uint64_t g_i_in_marker = 0;
uint64_t g_i_checking = 0;
uint64_t g_i_per_queue = 0;
uint64_t g_i_without_flushstate = 0;
int64_t g_i_next_ms = 0;

bool DrawInMarker() {
  static const bool active = REXCVAR_GET(nfsmw_d3d_marcador_registro);
  return active && !g_draw_disabled.load(std::memory_order_relaxed);
}

// The usual path: to the ring's queue (if it is full, the record is dropped and counted, as before).
void EnqueueDraw(const DrawRecord& reg_index) {
  const uint32_t write_op = g_write.load(std::memory_order_relaxed);
  const uint32_t next = (write_op + 1) & (kQueueSize - 1);
  if (next == g_read.load(std::memory_order_acquire)) {
    g_lost.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_queue[write_op] = reg_index;
  g_write.store(next, std::memory_order_release);
}

void DisableDraw() {
  if (g_draw_disabled.load(std::memory_order_relaxed)) {
    return;
  }
  g_draw_disabled.store(true, std::memory_order_relaxed);
  static constexpr const char* kQue[] = {
      "?", "los registros dan shaders distintos", "?",
      "la busqueda no encuentra registro y la identidad del anillo da otros shaders"};
  const DrawRecord& b = g_search_draw;
  const DrawRecord& m = g_marker_draw;
  REXLOG_ERROR("[d3d_marcador] registro de dibujo: DIFERENCIA con la busqueda del anillo ({}): busqueda {} (funcion "
               "{} tipo {} VS {:08X} PS {:08X}), marcador {} (funcion {} tipo {} VS {:08X} PS {:08X}). Fase 2b APAGADA "
               "para el resto de la sesion: los registros vuelven a la cola",
               kQue[g_draw_that < 4 ? g_draw_that : 0], g_search_has_draw ? "con registro" : "sin registro",
               int(b.func_id), b.args[0], b.vs, b.ps, g_marker_has_draw ? "con registro" : "sin registro",
               int(m.func_id), m.args[0], m.vs, m.ps);
}

void DrawReport() {
  const int64_t now_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
  if (now_time < g_i_next_ms) {
    return;
  }
  const bool first_item = g_i_next_ms == 0;
  g_i_next_ms = now_time + 10000;
  if (!first_item) {
    NFSMW_DEFERRED_REPORT("[d3d_marcador] registro de dibujo (fase 2b), ultimos 10 s: {} en el marcador ({} comprobando), {} "
                "por la cola, {} sin FlushState | fase {} | comprobados en el anillo {} de {}",
                g_i_in_marker, g_i_checking, g_i_per_queue, g_i_without_flushstate,
                g_draw_disabled.load(std::memory_order_relaxed)     ? "APAGADA"
                : g_applying_draw.load(std::memory_order_relaxed) ? "aplicando"
                                                                     : "mirando",
                g_equal_draw.load(std::memory_order_relaxed), kDrawChecks);
  }
  g_i_in_marker = g_i_checking = g_i_per_queue = g_i_without_flushstate = 0;
}

}  // namespace


namespace detail_hooks {

const ShaderEntry* IdentifyCreation(const uint8_t* base, uint32_t address, bool vertices) {
  NativeShaders* shaders = g_shaders.load(std::memory_order_acquire);
  if (!shaders || !address || address > UINT32_MAX - 24) {
    return nullptr;
  }
  const uint8_t* p = base + address;
  // 2005 (NFSMW) or 2008 (MoH: Airborne) container; the low bit tells vertex from pixel.
  const uint32_t signature = ReadBe(p);
  if ((signature & 0xFFFFFFFEu) != 0x102A0E00u && (signature & 0xFFFFFFFEu) != 0x102A1100u) {
    return nullptr;
  }
  if (bool(signature & 1) != vertices) {
    return nullptr;
  }
  const uint64_t total = uint64_t(ReadBe(p + 4)) + ReadBe(p + 8);
  if (total < 24 || total > 65536 || uint64_t(address) + total > (uint64_t(1) << 32)) {
    return nullptr;
  }
  return shaders->IdentifyContainer(std::span<const uint8_t>(p, size_t(total)));
}

void RememberCreation(uint32_t object, const ShaderEntry* entry, bool vertices) {
  if (!g_shaders.load(std::memory_order_acquire)) {
    return;
  }
  (vertices ? g_vs_created : g_ps_created).fetch_add(1, std::memory_order_relaxed);
  if (entry) {
    (vertices ? g_vs_known : g_ps_known).fetch_add(1, std::memory_order_relaxed);
  } else if (g_warnings.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_WARN("[nativo] C5b: {} shader creado en {:08X} que no esta en la biblioteca",
                vertices ? "vertex" : "pixel", object);
  }
  if (!object) {
    return;
  }
  std::lock_guard<std::mutex> mutex_lock(g_mutex);
  // The address of a freed object gets reused: an unknown shader clears any earlier association with it.
  if (entry) {
    g_objects.insert_or_assign(object, entry);
  } else {
    g_objects.erase(object);
  }
  g_generacion.fetch_add(1, std::memory_order_acq_rel);
}

}  // namespace ganchos_detalle

// see native_hooks.h.
const NativeShaders* ActiveLibrary() {
  return g_shaders.load(std::memory_order_acquire);
}

void ActivateHooks(NativeShaders* shaders) {
  g_shaders.store(shaders, std::memory_order_release);
  if (!shaders) {
    std::lock_guard<std::mutex> mutex_lock(g_mutex);
    g_objects.clear();
    g_generacion.fetch_add(1, std::memory_order_acq_rel);
  }
}

void TrackDraw(DrawFunction func_id, const uint8_t* base, uint32_t dev, uint32_t r4,
                  uint32_t r5, uint32_t r6, uint32_t r7, uint16_t vegetation) {
  if (!dev || !g_shaders.load(std::memory_order_relaxed)) {
    return;
  }
  DrawRecord reg_index;
  reg_index.vs = ReadBe(base + dev + kVsDevice);
  reg_index.ps = ReadBe(base + dev + kPsDevice);
  reg_index.args[0] = r4;
  reg_index.args[1] = r5;
  reg_index.args[2] = r6;
  reg_index.args[3] = r7;
  reg_index.func_id = func_id;
  reg_index.shadow = 0;
  reg_index.vegetation = vegetation;  // nfsmw_d3d_vegetacion_juego
  static const bool shadow = REXCVAR_GET(nfsmw_nativo_sombra_d3d);
  if (shadow && ++g_snapshots_counter % kSnapshotEvery == 0) {
    const uint64_t sequence = g_next_snapshot++;
    Snapshot& snapshot = g_snapshots[sequence % kSnapshots];
    snapshot.sequence.store(0, std::memory_order_release);  // being written
    MirrorSnapshot& d = snapshot.payload;
    const uint8_t* disp = base + dev;
    for (uint32_t g = 0; g < kMirrorGroups; ++g) {
      const uint32_t desp = g_group_offset[g].load(std::memory_order_relaxed);
      d.mask[g] = desp ? g_group_mask[g].load(std::memory_order_relaxed) : 0;
      d.reg_base[g] = g < 8 ? 0x2000 + g * 0x80 : 0x4900;
      if (!desp) {
        continue;
      }
      for (uint32_t i = 0; i < 64; ++i) {
        if ((d.mask[g] >> (63 - i)) & 1) {
          d.state[g][i] = ReadBe(disp + desp + i * 4);
        }
      }
    }
    for (uint32_t i = 0; i < 192; ++i) {
      d.fetch[i] = ReadBe(disp + kFetchDevice + i * 4);
    }
    for (uint32_t i = 0; i < 1024; ++i) {
      d.constantes[i] = ReadBe(disp + kVsConstantsDevice + i * 4);
      d.constantes[1024 + i] = ReadBe(disp + kPsConstantsDevice + i * 4);
    }
    snapshot.sequence.store(sequence, std::memory_order_release);
    reg_index.shadow = sequence;
  }
  // Phase 2b: with the record in the marker, this Draw*'s FlushState decides (TomarDibujoEnCurso);
  // otherwise, to the queue as usual.
  if (DrawInMarker()) {
    if (g_pending_draw) {  // should not happen: the previous Draw* reached neither its FlushState nor TerminarDibujo
      EnqueueDraw(g_draw_in_running);
      ++g_i_without_flushstate;
    }
    g_draw_in_running = reg_index;
    g_pending_draw = true;
    if ((++g_i_draws & 4095) == 0) {
      DrawReport();
    }
    return;
  }
  EnqueueDraw(reg_index);
}

bool TakeCurrentDraw(DrawRecord& reg_index) {
  if (!g_pending_draw) {
    return false;
  }
  g_pending_draw = false;
  reg_index = g_draw_in_running;
  return true;
}

uint32_t DecideDrawMode() {
  if (!DrawInMarker()) {
    return 0;
  }
  // Relaxed: an acquire load per draw would be an ldar on the A57. The acquire only if the ring has seen
  // something.
  if (g_distinct_draw.load(std::memory_order_relaxed)) {
    std::atomic_thread_fence(std::memory_order_acquire);
    DisableDraw();
    return 0;
  }
  if (!g_applying_draw.load(std::memory_order_relaxed)) {
    const uint64_t all_equal = g_equal_draw.load(std::memory_order_relaxed);
    if (all_equal < kDrawChecks) {
      return kCheckDraw;
    }
    g_applying_draw.store(true, std::memory_order_relaxed);
    REXLOG_INFO("[d3d_marcador] registro de dibujo: {} dibujos comprobados contra la busqueda del anillo, 0 "
                "desacuerdos: el marcador lleva ya el registro y el anillo no busca (1 de cada {} se sigue "
                "comprobando)",
                all_equal, kCheckDrawEvery);
  }
  return (++g_draw_turn & (kCheckDrawEvery - 1)) == 0 ? kCheckDraw : kApplyDraw;
}

void DeliverDraw(const DrawRecord& reg_index, uint32_t mode, bool in_marker) {
  if (!in_marker) {
    ++g_i_per_queue;
    EnqueueDraw(reg_index);
    return;
  }
  ++g_i_in_marker;
  if (mode == kCheckDraw) {  // the ring's lookup has to be able to find it to compare
    ++g_i_checking;
    EnqueueDraw(reg_index);
  }
}

void FinishDraw() {
  if (!g_pending_draw) {
    return;
  }
  g_pending_draw = false;
  ++g_i_without_flushstate;
  EnqueueDraw(g_draw_in_running);
}

void TrackDrawCheck(bool is_equal, uint32_t que, const DrawRecord* search,
                              const DrawRecord* marker) {
  if (is_equal) {  // written only by the ring thread
    g_equal_draw.store(g_equal_draw.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    return;
  }
  if (g_distinct_draw.load(std::memory_order_relaxed)) {
    return;
  }
  g_draw_that = que;
  g_search_has_draw = search != nullptr;
  if (search) {
    g_search_draw = *search;
  }
  g_marker_has_draw = marker != nullptr;
  if (marker) {
    g_marker_draw = *marker;
  }
  g_distinct_draw.store(true, std::memory_order_release);
}

void LearnMirrorGroup(uint32_t base_reg, uint64_t mask, uint32_t byte_offset) {
  const int g = GroupOf(base_reg);
  if (g < 0 || !byte_offset) {
    return;
  }
  // The dump copies from origen + 4 * bit position: the mirror of register base_registro + i is at
  // desplazamiento + 4 * i. Always the same per group; the first one seen is stored.
  uint32_t expected = 0;
  g_group_offset[g].compare_exchange_strong(expected, byte_offset, std::memory_order_relaxed);
  const uint64_t before = g_group_mask[g].load(std::memory_order_relaxed);
  if ((before | mask) != before) {
    g_group_mask[g].store(before | mask, std::memory_order_relaxed);
  }
}

bool ReadSnapshot(uint64_t sequence, MirrorSnapshot& output) {
  const Snapshot& snapshot = g_snapshots[sequence % kSnapshots];
  if (snapshot.sequence.load(std::memory_order_acquire) != sequence) {
    return false;
  }
  output = snapshot.payload;
  std::atomic_thread_fence(std::memory_order_acquire);
  return snapshot.sequence.load(std::memory_order_relaxed) == sequence;  // not rewritten while being copied
}

bool ExtractDraw(DrawRecord& reg_index) {
  const uint32_t readback = g_read.load(std::memory_order_relaxed);
  if (readback == g_write.load(std::memory_order_acquire)) {
    return false;
  }
  reg_index = g_queue[readback];
  g_read.store((readback + 1) & (kQueueSize - 1), std::memory_order_release);
  return true;
}

const ShaderEntry* ShaderOfObject(uint32_t object) {
  std::lock_guard<std::mutex> mutex_lock(g_mutex);
  const auto it = g_objects.find(object);
  return it != g_objects.end() ? it->second : nullptr;
}

uint64_t ObjectsGeneration() {
  return g_generacion.load(std::memory_order_acquire);
}

/*
 * Shadow map vegetation, filtered on the game thread (nfsmw_d3d_vegetacion_juego).
 *
 * The ring drops, right on entry to Dibujar, the colorless draws with an alpha test, a kill or PS-written
 * depth: the shadow map vegetation, ~480 per frame in a race (267-760). But each one still costs ~2.2-2.6 us
 * of ring time outside Dibujar (its FlushState marker, the DRAW_INDX, the pairing and its share of IM_LOAD)
 * plus its whole Draw* on the game thread (FlushState with streams, shaders and IM_LOAD, and the EDRAM mode
 * changes 5 and 4 with their packets). Here the same decision is taken in the D3D Draw*, before the
 * original, using the device's register mirror, and if it is vegetation the original is not called: the
 * Draw* never exists for the ring.
 *
 * Why the whole Draw* can be removed (from reading the PowerPC code)
 *   - DrawVertices (82593A10) and DrawIndexedVertices (82593C50), before FlushState, only touch
 *     VGT_INDX_OFFSET in the mirror (dev+0x2D14, with its dirty bit 1<<61 in dev+0x28): the same is done
 *     here. After FlushState they only write their DRAW_INDX (normal path: bit 0x04 of dev+0x28C0 clear and a
 *     single-segment count).
 *   - What the skipped FlushState would have dumped stays marked dirty and the next one dumps it (or the
 *     partial dump 825A42E0 of Resolve and ClearF) with the mirror's values, which are the same: the ring's
 *     state at the next draw is the usual one. The same goes for the shaders: their dirty bits stay set and
 *     the next FlushState patches and loads them for whatever shader pair is bound then.
 *   - What gets written to the ring without a dump first: the occlusion query (8258F810 and 8258EA28)
 *     writes RB_MODECONTROL, RB_COLOR_INFO and RB_SAMPLE_COUNT_ADDR directly and leaves them dirty to be
 *     restored. In the ring the only lazy register read without a dump before it is RB_SURFACE_INFO
 *     (EscalaOclusion). That is why nothing is skipped with group 0x2000 dirty (render targets pending a
 *     dump) or with an occlusion query open.
 *   - Predicated tiling and ZPass: inside a D3D block (dev+0x28C0 & 0x3F: 0x10 tiling, 0x04 extension path
 *     with SET_BIN_MASK and EVENT_WRITE_EXT, 0x01, 0x02 and 0x20 ZPass, 0x08 the one from 825AB9A0) nothing
 *     is skipped: there the packets are recorded to be replayed per tile.
 *
 * The criterion: Dibujar's up to its early discard, with the state the ring will see for that draw
 *   - PS and VS bound (dev+0x3290 and dev+0x4FE8) and present in the library (ShaderDeObjeto: the identity
 *     the ring takes from the record).
 *   - Effective EDRAM mode 4. It is the mirror's (RB_MODECONTROL, dev+0x2D94) except for one change
 *     FlushState makes before dumping it: with a PS bound and its dirty bit 0x100 in dev+0x30, the shader
 *     loader (825A3AF0) calls 825A3A58, which turns mode 5 into 4. The switch to 5 (825A3968) only happens
 *     without a PS, and the streams (825A2D80) only set bit 0x80. No other function in the FlushState tree
 *     writes RB_MODECONTROL, RB_COLOR_MASK, RB_COLORCONTROL or the PS.
 *   - No color: no target i with RB_COLOR_MASK (dev+0x2D1C) nonzero and the PS writing it.
 *   - Alpha test (RB_COLORCONTROL, dev+0x2D7C: enabled and a function other than ALWAYS), the PS's own
 *     kill, or depth written by the PS.
 *   - And to skip it: nfsmw_nativo_ps_solo_alfa without alternation, nfsmw_sombras_sin_vegetacion, no
 *     occlusion query open, outside blocks, no render targets pending a dump, and a single segment
 *     (count 1..65535).
 *
 * Self-checking guard, in two phases
 *   1. Observing: nothing is skipped. Each Draw* carries its verdict in the record and the ring computes
 *      its own in Dibujar, with its own registers and PS, and counts game yes/no against ring yes/no.
 *      After kComprobaciones checked draws the game would skip, with no disagreement, it moves to applying.
 *   2. Applying: they are skipped. 1 in kMuestraCada is still sent, flagged kVegMuestra, for the ring to
 *      check, and the ones not skipped still carry their negative verdict, which the ring checks on all of
 *      them.
 *   The filter is switched off for the rest of the session, with DIFERENCIA in the log, on: a differing
 *   verdict in either direction, an occlusion query open in the ring that the game does not see, a draw the
 *   game sees as vegetation that the ring does not draw with its record's shaders, or a ring verdict that
 *   differs from Dibujar's early discard.
 *
 * Memory ordering
 *   The ring writes the disagreement details and then g_distinto with release. The game reads g_distinto
 *   relaxed on every Draw* and, if it is true, issues an acquire fence before reading the details: the fence
 *   synchronizes with the release and the details are seen in full (the same pairing as phase 2b in
 *   DecidirModoDibujo). Nobody waits or sleeps on this flag: the game only polls it, so no wake-up can be
 *   lost and nothing can hang (the earlier hangs were a thread asleep waiting for a wake-up). The worst case
 *   is seeing it a few microseconds late. The ring's counters have a single writer (the ring) and the game
 *   reads them relaxed: they only count. The verdict travels from the game to the ring inside the draw
 *   record, over the same already synchronized path as phase 2b (marker) or the queue (release and acquire
 *   on its indices).
 */
namespace {
namespace vegetation {

constexpr uint64_t kChecks = 200000;  // observing phase: draws the game would skip, checked in the ring
constexpr uint64_t kSampleEvery = 4096;       // applying phase: 1 in kMuestraCada is sent anyway (power of 2)
// The device's register mirror (FlushState groups: 0x2100 from +0x2D0C, 0x2200 from +0x2D74).
constexpr uint32_t kEdramMode = 0x2D94;     // RB_MODECONTROL (0x2208)
constexpr uint32_t kColorMask = 0x2D1C;  // RB_COLOR_MASK (0x2104)
constexpr uint32_t kControlColor = 0x2D7C;  // RB_COLORCONTROL (0x2202)
constexpr uint32_t kBaseIndex = 0x2D14;    // VGT_INDX_OFFSET (0x2102)
// Its dirty masks (64-bit big-endian): +0x20 fetch, booleans and group 0x2000 (bits 0x3FFFC000 of the low
// word); +0x28 groups 0x2100 (bit 63 = 0x2100), 0x2180, 0x2200 and 0x2280; +0x30 streams (0x400), shaders
// (0x1E0; 0x100 is the PS) and groups 0x2300 and 0x2380.
constexpr uint32_t kDirty20 = 0x20;
constexpr uint32_t kDirty28 = 0x28;
constexpr uint32_t kDirty30 = 0x30;
constexpr uint32_t kBlocks = 0x28C0;      // D3D block byte (BeginTiling 825992F0, ZPass 825999D8...)
constexpr uint8_t kMaskBlocks = 0x3F;  // 0x40 and 0x80 are set at device creation: not blocks
constexpr uint32_t kOcclusionType = 9;      // D3DQUERYTYPE_OCCLUSION, en consulta+4 (8258F810)
constexpr uint32_t kMaxQueries = 16;

enum Que : uint32_t { kThatNothing, kThatGameIf, kThatRingIf, kQueOclusion, kThatWithoutIdentity, kThatModel, kQues };

// ---- Game thread (the one using D3D: the Draw* calls and the queries' Issue) ----
int8_t g_active = -1;  // nfsmw_d3d_vegetacion_juego, read the first time (without a local static guard)
bool g_applying = false;
bool g_disabled = false;
bool g_settings = false;  // refreshed every 1024 Draw* calls checked
uint64_t g_turn = 0;
uint64_t g_peeked_total = 0;
struct Memo {
  uint32_t object = 0;
  const ShaderEntry* entry = nullptr;
};
std::array<Memo, 32> g_memo{};
uint64_t g_memo_generacion = UINT64_MAX;
// Report every 10 s.
int64_t g_i_next_ms = 0;
uint64_t g_i_peeked = 0;
uint64_t g_i_si = 0;
uint64_t g_i_skipped = 0;
uint64_t g_i_samples = 0;
uint64_t g_i_block = 0;
uint64_t g_i_targets = 0;
uint64_t g_i_oclusion = 0;
uint64_t g_i_settings = 0;
uint64_t g_i_count = 0;
std::array<uint32_t, 64> g_i_blocks{};  // the block ones, by value of dev+0x28C0 & 0x3F

// ---- Open D3D occlusion queries (Issue BEGIN without its END), per query object ----
// The D3D thread touches them; the lock only prevents a race should they ever be called from another
// thread.
std::mutex g_mutex_queries;
std::array<uint32_t, kMaxQueries> g_queries{};
uint32_t g_n_queries = 0;
std::atomic<uint32_t> g_open_queries{0};  // UINT32_MAX: count lost, nothing is skipped any more

// ---- Shared with the ring thread ----
// Counters: only the ring writes them, without atomic read-modify-write (the A57 has no LSE). The game
// reads them relaxed for the report and to decide when to move to applying.
enum CounterId : uint32_t {
  kSiSi,
  kNoNo,
  kSiNo,
  kNoSi,
  kNoIfBlock,
  kNoIfTargets,
  kNoSiOclusion,
  kNoIfSettings,
  kNoIfCount,
  kDistinctSettings,
  kWithoutCompare,
  kAgreements,
  kOkSamples,
  kCounters
};
// Own cache line: the ring writes them on every draw and they must not share a line with anything the
// game thread reads on every Draw* (g_distinto), or every write would steal it from the other core.
alignas(64) std::atomic<uint64_t> g_counters[kCounters];
alignas(64) std::array<uint64_t, kCounters> g_previous{};  // own cache line; the previous report's values (game thread only)
// The first disagreement: the ring writes the details and then g_distinto with release (see Memory
// ordering). Own cache line (the game reads it on every Draw*; see g_contadores).
alignas(64) std::atomic<bool> g_distinct{false};
uint32_t g_que = kThatNothing;
uint16_t g_flags = 0;
VegetationDetail g_detail;

uint64_t ReadBe64(const uint8_t* p) {
  return (uint64_t(ReadBe(p)) << 32) | ReadBe(p + 4);
}

void WriteBe(uint8_t* p, uint32_t value) {
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

const char* SiNo(bool value) {
  return value ? "si" : "no";
}

inline void Bump(CounterId c) {  // ring thread only
  g_counters[c].store(g_counters[c].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

// Ring thread only: the details before the flag (release); the game reads them after its acquire fence.
void Mismatch(Que que, uint16_t flag_bits, const VegetationDetail& ring) {
  if (g_distinct.load(std::memory_order_relaxed)) {
    return;
  }
  g_que = que;
  g_flags = flag_bits;
  g_detail = ring;
  g_distinct.store(true, std::memory_order_release);
}

// The object of a bound shader, with the same identity the ring takes from the record (ShaderDeObjeto).
// The generation is read relaxed: a shader created on another thread only reaches a Draw* on this thread
// through some synchronization in the game, and then this read already sees the new generation
// (write-read coherence).
const ShaderEntry* Entry(uint32_t object) {
  const uint64_t generacion = g_generacion.load(std::memory_order_relaxed);
  if (generacion != g_memo_generacion) {
    g_memo.fill(Memo{});
    g_memo_generacion = generacion;
  }
  Memo& memo = g_memo[size_t((object * UINT32_C(2654435761)) >> 27)];
  if (memo.object != object) {
    memo.object = object;
    memo.entry = ShaderOfObject(object);
  }
  return memo.entry;
}

// Dibujar's criterion up to its early discard (nfsmw_nativo_dibujos.cpp), with the state the ring will
// see.
bool Structure(const uint8_t* d, uint32_t& reason) {
  const uint32_t ps_object = ReadBe(d + kPsDevice);
  const uint32_t vs_object = ReadBe(d + kVsDevice);
  if (!ps_object || !vs_object) {
    reason = kReasonVegWithoutShaders;
    return false;
  }
  // The EDRAM mode the ring will see: the mirror's, with the switch from 5 to 4 FlushState makes before
  // dumping it if a PS is bound and its dirty bit is set (825A3AF0 and 825A3A58). Without a PS it already
  // returned above.
  uint32_t mode = ReadBe(d + kEdramMode) & 0x7;
  if (mode == 5 && (ReadBe64(d + kDirty30) & 0x100)) {
    mode = 4;
  }
  if (mode != 4) {
    reason = kModeReasonVeg;
    return false;
  }
  const ShaderEntry* ps = Entry(ps_object);
  const ShaderEntry* vs = Entry(vs_object);
  if (!ps || !vs) {
    reason = kUnknownReasonVeg;
    return false;
  }
  const uint32_t mask = ReadBe(d + kColorMask);
  for (uint32_t i = 0; i < 4; ++i) {
    if (((mask >> (i * 4)) & 0xF) && ((ps->outputs >> i) & 0x1)) {
      reason = kColorReasonVeg;
      return false;
    }
  }
  const uint32_t control = ReadBe(d + kControlColor);
  const bool alpha_test = ((control >> 3) & 0x1) && (control & 0x7) != 7;
  if (!alpha_test && !ps->discards && !(ps->outputs & 0x10)) {
    reason = kReasonVegWithoutDiscard;
    return false;
  }
  reason = 0;
  return true;
}

void RefreshVegetationSettings() {
  g_settings = REXCVAR_GET(nfsmw_nativo_ps_solo_alfa) && REXCVAR_GET(nfsmw_nativo_ps_solo_alfa_alternar_s) <= 0 &&
              REXCVAR_GET(nfsmw_sombras_sin_vegetacion);
}

// Game thread only, after the acquire fence.
void DisableVegetation() {
  if (g_disabled) {
    return;
  }
  g_disabled = true;
  static constexpr const char* kQueTexto[kQues] = {
      "?",
      "el juego ve vegetacion y el anillo no",
      "el anillo ve vegetacion y el juego no",
      "consulta de oclusion abierta en el anillo y no en el juego",
      "el juego ve vegetacion y el anillo no dibuja con los shaders de su registro",
      "el veredicto del anillo no es el descarte temprano de Dibujar"};
  const VegetationDetail& a = g_detail;
  const uint16_t b = g_flags;
  REXLOG_ERROR("[vegetacion] DIFERENCIA con el anillo ({}): juego {} (motivo {}, ajustes {}, oclusion {}, bloque {}, "
               "destinos {}, saltaria {}, muestra {}; fase {}); anillo: estructura {}, ajustes {}, oclusion {}, "
               "descarte temprano en Dibujar {}, RB_MODECONTROL {:08X}, RB_COLOR_MASK {:08X}, RB_COLORCONTROL {:08X}, "
               "PS n{} (salidas {:X}, descarta {}), VS n{}. Filtro de la vegetacion en el juego APAGADO para el resto "
               "de la sesion: todos los Draw* vuelven al anillo",
               kQueTexto[g_que < kQues ? g_que : 0], SiNo((b & kVegSi) != 0), (b >> kReasonVeg) & 0xF,
               SiNo((b & kSettingsVeg) != 0), SiNo((b & kVegOclusion) != 0), SiNo((b & kBlockVeg) != 0),
               SiNo((b & kTargetsVeg) != 0), SiNo((b & kWouldSkipVeg) != 0), SiNo((b & kSampleVeg) != 0),
               g_applying ? "aplicando" : "mirando", SiNo(a.structure), SiNo(a.settings), SiNo(a.oclusion),
               SiNo(a.early), a.mode, a.mask, a.control, a.ps, a.outputs, SiNo(a.discards), a.vs);
}

// Every 10 s, from the game thread (NFSMW_INFORME_DIFERIDO). The ring's counters, as differences.
void VegetationReport() {
  const int64_t now_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
  if (now_time < g_i_next_ms) {
    return;
  }
  const bool first_item = g_i_next_ms == 0;
  g_i_next_ms = now_time + 10000;
  std::array<uint64_t, kCounters> ring{};
  for (uint32_t i = 0; i < kCounters; ++i) {
    ring[i] = g_counters[i].load(std::memory_order_relaxed);
  }
  if (first_item) {
    REXLOG_INFO("[vegetacion] vegetacion del mapa de sombras filtrada en el juego (build 184): empieza mirando; salta "
                "cuando el anillo haya comprobado {} dibujos que saltaria sin ningun desacuerdo",
                kChecks);
  } else {
    std::array<uint64_t, kCounters> d{};
    for (uint32_t i = 0; i < kCounters; ++i) {
      d[i] = ring[i] - g_previous[i];
    }
    std::string blocks;
    for (uint32_t v = 1; v < 64; ++v) {
      if (g_i_blocks[v]) {
        blocks += fmt::format(" {:02X}:{}", v, g_i_blocks[v]);
      }
    }
    if (!blocks.empty()) {
      blocks = " (valores" + blocks + ")";
    }
    const bool lost_one = g_open_queries.load(std::memory_order_relaxed) == UINT32_MAX;
    NFSMW_DEFERRED_REPORT(
        "[vegetacion] Draw* (build 184), ultimos 10 s: {} mirados, {} de vegetacion para el juego: {} saltados y {} "
        "enviados para comprobar; no se saltan {} en bloques del D3D{}, {} con destinos por volcar, {} con una "
        "consulta de oclusion abierta, {} con los ajustes apagados o alternando, {} por la cuenta | anillo: si y si "
        "{}, no y no {}, juego si y anillo no {}, juego no y anillo si {} ({} por bloque, {} por destinos, {} por "
        "oclusion solo en el juego, {} por ajustes, {} por la cuenta), ajustes distintos {}, sin comparar {}, "
        "muestras bien {} | fase {} | comprobados {} de {}{}",
        g_i_peeked, g_i_si, g_i_skipped, g_i_samples, g_i_block, blocks, g_i_targets, g_i_oclusion, g_i_settings,
        g_i_count, d[kSiSi], d[kNoNo], d[kSiNo], d[kNoSi], d[kNoIfBlock], d[kNoIfTargets], d[kNoSiOclusion],
        d[kNoIfSettings], d[kNoIfCount], d[kDistinctSettings], d[kWithoutCompare], d[kOkSamples],
        g_applying ? "aplicando" : "mirando", ring[kAgreements], kChecks,
        lost_one ? " | consultas de oclusion: SE PERDIO LA CUENTA, no se salta nada" : "");
  }
  g_previous = ring;
  g_i_peeked = g_i_si = g_i_skipped = g_i_samples = g_i_block = g_i_targets = g_i_oclusion = g_i_settings =
      g_i_count = 0;
  g_i_blocks.fill(0);
}

}  // namespace vegetacion
}  // namespace

uint16_t DecideVegetation(DrawFunction func_id, uint8_t* base, uint32_t dev, uint32_t r5, uint32_t r6,
                           uint32_t r7, bool& skip_over) {
  using namespace vegetation;
  skip_over = false;
  if (g_active < 0) {
    g_active = REXCVAR_GET(nfsmw_d3d_vegetacion_juego) ? 1 : 0;
  }
  if (!g_active || g_disabled || !dev || !g_shaders.load(std::memory_order_relaxed) ||
      (func_id != DrawFunction::kVertices && func_id != DrawFunction::kIndexed)) {
    return 0;
  }
  // Relaxed: an acquire load per draw would be an ldar on the A57. The acquire only if the ring has seen
  // something.
  if (g_distinct.load(std::memory_order_relaxed)) {
    std::atomic_thread_fence(std::memory_order_acquire);
    DisableVegetation();
    return 0;
  }
  if ((g_peeked_total++ & 1023) == 0) {
    RefreshVegetationSettings();
    VegetationReport();
  }
  ++g_i_peeked;
  const uint8_t* d = base + dev;
  uint32_t reason = 0;
  const bool si = Structure(d, reason);
  const uint32_t open_items = g_open_queries.load(std::memory_order_relaxed);
  const uint8_t blocks = uint8_t(d[kBlocks] & kMaskBlocks);
  const bool targets = (uint32_t(ReadBe64(d + kDirty20)) & 0x3FFFC000u) != 0;
  const uint32_t num = func_id == DrawFunction::kVertices ? r6 : r7;
  uint16_t flag_bits = uint16_t(kVegHay | (reason << kReasonVeg));
  if (si) {
    flag_bits = uint16_t(flag_bits | kVegSi);
  }
  if (open_items) {
    flag_bits = uint16_t(flag_bits | kVegOclusion);
  }
  if (g_settings) {
    flag_bits = uint16_t(flag_bits | kSettingsVeg);
  }
  if (blocks) {
    flag_bits = uint16_t(flag_bits | kBlockVeg);
  }
  if (targets) {
    flag_bits = uint16_t(flag_bits | kTargetsVeg);
  }
  if (!si) {
    return flag_bits;
  }
  ++g_i_si;
  // Why it would not be skipped, in this order (each draw counts under a single reason).
  if (blocks) {
    ++g_i_block;
    ++g_i_blocks[blocks];
    return flag_bits;
  }
  if (targets) {
    ++g_i_targets;
    return flag_bits;
  }
  if (open_items) {
    ++g_i_oclusion;
    return flag_bits;
  }
  if (!g_settings) {
    ++g_i_settings;
    return flag_bits;
  }
  if (num == 0 || num > 0xFFFF) {
    ++g_i_count;
    return flag_bits;
  }
  flag_bits = uint16_t(flag_bits | kWouldSkipVeg);
  if (!g_applying) {
    const uint64_t checked_items = g_counters[kAgreements].load(std::memory_order_relaxed);
    if (checked_items < kChecks) {
      return flag_bits;  // observing phase: sent, and the ring compares it
    }
    g_applying = true;
    REXLOG_INFO("[vegetacion] {} dibujos de vegetacion que el juego saltaria comprobados en el anillo, 0 desacuerdos: "
                "se saltan ya en el Draw* del D3D (1 de cada {} se sigue enviando para comprobarlo)",
                checked_items, kSampleEvery);
  }
  if ((++g_turn & (kSampleEvery - 1)) == 0) {
    ++g_i_samples;
    return uint16_t(flag_bits | kSampleVeg);
  }
  // The whole Draw* is skipped. The only thing it does to the mirror before FlushState: VGT_INDX_OFFSET and
  // its dirty bit (DrawVertices sets it to its start vertex, r5; DrawIndexedVertices to 0). Everything else
  // stays dirty for the next dump, with the same mirror values.
  uint8_t* mirror = base + dev;
  const uint32_t index = func_id == DrawFunction::kVertices ? r5 : 0u;
  if (ReadBe(mirror + kBaseIndex) != index) {
    WriteBe(mirror + kBaseIndex, index);
    const uint64_t dirty = ReadBe64(mirror + kDirty28) | (uint64_t(1) << 61);
    WriteBe(mirror + kDirty28, uint32_t(dirty >> 32));
    WriteBe(mirror + kDirty28 + 4, uint32_t(dirty));
  }
  ++g_i_skipped;
  skip_over = true;
  return flag_bits;
}

void TrackD3DQuery(const uint8_t* base, uint32_t query, uint32_t flag_bits) {
  using namespace vegetation;
  if (!query || ReadBe(base + query + 4) != kOcclusionType) {
    return;
  }
  const bool starts = (flag_bits & 2) != 0;
  if (!starts && !(flag_bits & 1)) {
    return;
  }
  std::lock_guard<std::mutex> mutex_lock(g_mutex_queries);
  uint32_t i = 0;
  while (i < g_n_queries && g_queries[i] != query) {
    ++i;
  }
  if (starts) {  // BEGIN takes precedence over END, as in the original
    if (i == g_n_queries) {
      if (g_n_queries == kMaxQueries) {
        g_open_queries.store(UINT32_MAX, std::memory_order_relaxed);
        return;
      }
      g_queries[g_n_queries++] = query;
    }
  } else if (i < g_n_queries) {
    g_queries[i] = g_queries[--g_n_queries];
  }
  if (g_open_queries.load(std::memory_order_relaxed) != UINT32_MAX) {
    g_open_queries.store(g_n_queries, std::memory_order_relaxed);
  }
}

uint16_t VegetationIdentity(uint16_t flag_bits, bool con_sus_shaders) {
  using namespace vegetation;
  if (!(flag_bits & kVegHay)) {
    return 0;
  }
  if (con_sus_shaders) {
    return flag_bits;
  }
  Bump(kWithoutCompare);
  if (flag_bits & kVegSi) {
    Mismatch(kThatWithoutIdentity, flag_bits, VegetationDetail{});
  }
  return 0;
}

void TrackRingVegetation(uint16_t flag_bits, const VegetationDetail& ring) {
  using namespace vegetation;
  const bool si = (flag_bits & kVegSi) != 0;
  const bool would_skip = (flag_bits & kWouldSkipVeg) != 0;
  const bool complete = ring.structure && ring.settings && !ring.oclusion;
  Bump(would_skip ? (complete ? kSiSi : kSiNo) : (complete ? kNoSi : kNoNo));
  if (si != ring.structure) {
    Mismatch(si ? kThatGameIf : kThatRingIf, flag_bits, ring);
    return;
  }
  if (would_skip) {
    if (!ring.settings) {
      Bump(kDistinctSettings);  // a setting just changed and each thread reads it at its own time: not an error
    } else if (ring.oclusion) {
      Mismatch(kQueOclusion, flag_bits, ring);
    } else {
      Bump(kAgreements);
      if (flag_bits & kSampleVeg) {
        Bump(kOkSamples);
      }
    }
    return;
  }
  if (complete) {  // same structure, but the game would not skip it: why
    Bump((flag_bits & kBlockVeg)       ? kNoIfBlock
           : (flag_bits & kTargetsVeg)   ? kNoIfTargets
           : (flag_bits & kVegOclusion)   ? kNoSiOclusion
           : !(flag_bits & kSettingsVeg)   ? kNoIfSettings
                                         : kNoIfCount);
  }
}

void TrackModelVegetation(uint16_t flag_bits, const VegetationDetail& ring) {
  vegetation::Mismatch(vegetation::kThatModel, flag_bits, ring);
}

HookStats GetHookStats() {
  HookStats e;
  e.vs_created = g_vs_created.load(std::memory_order_relaxed);
  e.vs_known = g_vs_known.load(std::memory_order_relaxed);
  e.ps_created = g_ps_created.load(std::memory_order_relaxed);
  e.ps_known = g_ps_known.load(std::memory_order_relaxed);
  e.lost_items = g_lost.load(std::memory_order_relaxed);
  return e;
}

}  // namespace nfsmw::nativo

// The constructors and the fetch patcher report what they write into the microcode (IM_LOAD without
// memcmp, see microcode_versions.h).
#include "microcode_versions.h"

// The experimental library (nfsmw_shader_hooks.cpp) hooks the same constructors; with it enabled these
// are not compiled.
// AirborneNX: the guest addresses below are NFS: Most Wanted's XDK D3D functions. In MoH: Airborne they
// point at unrelated code, so the hooks stay off until analysis/d3d_map/identified.md gives our own.
#ifndef AIRBORNE_NATIVE_HOOKS
#define AIRBORNE_NATIVE_HOOKS 0
#endif
#if !defined(NFSMW_NATIVE_SHADER_LIBRARY)
// r3 = original container; they return the object in r3. The original is always called and no PPC
// register is touched.
// AirborneNX: MoH: Airborne's XDK constructors (analysis/d3d_map/identified.md): sub_829A3158 creates a
// pixel shader (microcode pointer at +24) and sub_829A32A8 a vertex shader (container copied to +872, the
// fetches patched at creation by sub_829A18A8, microcode pointer at +32). NFSMW's were sub_8259BC90 and
// sub_8259C038.
REX_EXTERN(__imp__sub_829A3158);
REX_HOOK_RAW(sub_829A3158) {  // pixel shader
  const auto* entry =
      nfsmw::native_renderer::detail_hooks::IdentifyCreation(base, ctx.r3.u32, false);
  __imp__sub_829A3158(ctx, base);
  nfsmw::native_renderer::detail_hooks::RememberCreation(ctx.r3.u32, entry, false);
  nfsmw::native_renderer::ucode::WarnCreation(base, ctx.r3.u32, false);
}

REX_EXTERN(__imp__sub_829A32A8);
REX_HOOK_RAW(sub_829A32A8) {  // vertex shader
  const auto* entry = nfsmw::native_renderer::detail_hooks::IdentifyCreation(base, ctx.r3.u32, true);
  __imp__sub_829A32A8(ctx, base);
  nfsmw::native_renderer::detail_hooks::RememberCreation(ctx.r3.u32, entry, true);
  nfsmw::native_renderer::ucode::WarnCreation(base, ctx.r3.u32, true);
}
#endif

/*
 * The VS fetch patcher reports what it writes (IM_LOAD without memcmp, see microcode_versions.h).
 * sub_825A2FB8(r3 device, r4 VS, r5 destination, r6 declaration) writes the patched fetches to r5.
 *   - In place (r5 = [VS+40], from sub_825A3AF0): bumps the versions of that microcode's slot before and
 *     after writing.
 *   - On the ring's copy (from sub_825A37D8, return 0x825A38D0): not memory of any IM_LOAD; only counted.
 *   - Any other call: also bumps the destination's versions, but is counted separately and the ring turns
 *     off the shortcut.
 * Careful: the two calls in the generated code must go to sub_825A2FB8 and not to __imp__sub_825A2FB8
 * (nfsmw_recomp.58.cpp and nfsmw_recomp.124.cpp, changed by a patch; tools/llamadas_directas.py already
 * handles them because this address appears here). If the one from sub_825A3AF0 went to __imp__, this
 * hook would not see the in-place patches and the ring's guard would stay in the observing phase (or
 * switch off with DIFERENCIA).
 * It changes no PPC register.
 */
// AirborneNX: NFSMW-only (MoH: Airborne patches the fetches once, in the constructor); kept for reference.
#if AIRBORNE_NATIVE_HOOKS
REX_EXTERN(__imp__sub_825A2FB8);
REX_HOOK_RAW(sub_825A2FB8) {
  namespace mc = nfsmw::native_renderer::ucode;
  const uint32_t target = ctx.r5.u32;
  const uint32_t vs = ctx.r4.u32;
  const uint32_t ret_addr = uint32_t(ctx.lr);
  const bool in_place = vs != 0 && mc::ReadGuest32(base, vs + 40) == target;
  if (!in_place && ret_addr == 0x825A38D0u) {
    mc::Bump(mc::g_patches_in_copy);
    __imp__sub_825A2FB8(ctx, base);
    return;
  }
  const uint32_t ranura = mc::RanuraDe(mc::Physical(target) & ~uint32_t(3));
  mc::StartWrite(ranura);
  __imp__sub_825A2FB8(ctx, base);
  // The counts, before TerminarEscritura's global++: a ring that sees the new global value already sees
  // them.
  if (in_place) {
    mc::Bump(mc::g_patches_in_place_);
  } else {
    mc::g_other_return.store(ret_addr, std::memory_order_relaxed);
    mc::g_other_target.store(target, std::memory_order_relaxed);
    mc::Bump(mc::g_other_patches);
  }
  mc::FinishWrite(ranura);
}
#endif  // AIRBORNE_NATIVE_HOOKS
