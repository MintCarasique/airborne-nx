// Stand-ins for the NFS: Most Wanted modules the native renderer calls into
// but that are specific to that game (they hook NFSMW's own functions). Each
// keeps the renderer neutral until MoH: Airborne has its own equivalent:
//   - ring progress / FlushState markers: woke NFSMW's game-side busy waits
//     (nfsmw_espera_anillo.cpp, nfsmw_d3d_registros_nativo.cpp);
//   - guest frame counter: counted by a hook on NFSMW's frame loop;
//   - 30 FPS guard, scene LOD, on-demand road reflection: NFSMW game logic;
//   - render_targets: NFSMW's anti-aliasing mode table.

#include <atomic>
#include <cstdint>
#include <string>

// Guest frames presented by the game (NFSMW counted them in its frame loop).
std::atomic<uint64_t> g_nfsmw_game_frames{0};

namespace nfsmw {

namespace native_renderer {
void WarnRingProgress() {}
void ActivateMarkersConsumer(bool) {}
void TrackMarkerCheck(bool, uint32_t, uint32_t, uint32_t, uint32_t) {}
}  // namespace nativo

namespace guard30 {
// Shadows are skipped exactly when the cvar asks for it; no frame-rate guard.
bool WithoutShadows(bool requested) {
  return requested;
}
void Latir(double) {}
void Report() {}
}  // namespace guardia30

namespace lod_scenario {
std::string Summary() {
  return {};
}
}  // namespace escenario_lod

namespace demand_reflection {
bool MeasureVisibility() {
  return false;
}
bool CheckedVisibility() {
  return false;
}
void TrackCopy() {}
void TrackRead() {}
void TrackHidden() {}
void TrackSwap() {}
void TrackSentinel(bool) {}
void TrackVisible(bool) {}
}  // namespace reflejo_demanda

namespace render_targets {
// Samples of the game's original anti-aliasing mode; 1 until MoH:A's mode
// table is known.
uint32_t OriginalSamplesCurrentMode(const uint8_t*) {
  return 1;
}
}  // namespace render_targets

}  // namespace nfsmw
