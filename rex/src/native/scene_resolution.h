// AirborneNX - the scene drawn at a lower resolution (airborne_scene_resolution).
//
// UE3's client takes its viewport size from ResX=/ResY= on the game's command line (FXenonViewport; the
// defaults are 1280x720). With a smaller size the game draws the scene, the post-processing and the UI into
// that corner of its 1280x720 render targets, so fewer pixels are shaded. The full-screen movie player does
// not follow the viewport: it keeps drawing 1280x720.
//
// The output shows only the corner, scaled to the screen, and only in frames where the game drew through its
// viewport (a draw to a 1280-pitch target with exactly that viewport at 0,0). Every frame also has a few
// full-target passes (1280x720 rect lists and quads, seen on the PC in Tra_Jmp_P and the menus), so "nothing
// drawn outside the corner" never holds. A frame without a viewport draw (the movie player) is shown whole,
// as before. This file holds the per-frame state: the ring thread notes it in Draw and reads it in Present,
// so there is no locking.

#pragma once

#include <algorithm>
#include <cstdint>

namespace airborne::scene {

// Width and height from airborne_scene_resolution; 0 x 0 when it is the game's 1280x720.
struct Size {
  uint32_t width_px = 0;
  uint32_t height_px = 0;
};
Size Configured();

// At startup, before the kernel builds the guest command line: appends " ResX=W ResY=H" to the cl cvar.
void ApplyToCommandLine();

// The current frame's draws to 1280-pitch targets: their right and bottom edges in pixels, and whether one
// used the scene viewport.
struct FrameBounds {
  uint32_t right = 0;
  uint32_t bottom = 0;
  bool scene_viewport = false;
};
inline FrameBounds g_frame;

inline void NoteDraw(uint32_t right, uint32_t bottom, bool scene_viewport) {
  g_frame.right = std::max(g_frame.right, right);
  g_frame.bottom = std::max(g_frame.bottom, bottom);
  g_frame.scene_viewport |= scene_viewport;
}

// The frame's bounds; starts the next frame.
inline FrameBounds TakeFrame() {
  const FrameBounds bounds = g_frame;
  g_frame = FrameBounds{};
  return bounds;
}

}  // namespace airborne::scene
