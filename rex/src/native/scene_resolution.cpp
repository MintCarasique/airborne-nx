// AirborneNX - the scene drawn at a lower resolution (see scene_resolution.h).

#include "scene_resolution.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <cstdlib>
#include <string>

REXCVAR_DECLARE(std::string, cl);  // kernel/xboxkrnl/xboxkrnl_module.cpp (SDK): the guest command line

// Widths are multiples of 80 and heights of 16, the EDRAM tile size: at 912x512 the console showed a black
// vertical stripe a few millimetres from the right edge (build 5dd45df), 960x544 none. 880x496 keeps the scene
// inside the first predicated tile (rows 0-511), so the second tile has nothing left to draw.
REXCVAR_DEFINE_STRING(airborne_scene_resolution, "1280x720", "AirborneNX",
                      "Resolution the game draws the scene and the UI at, shown scaled to the screen. 1280x720 is "
                      "the Xbox 360's (ResX/ResY are not passed). Smaller sizes shade fewer pixels; frames that draw "
                      "outside that corner (the full-screen movies) are shown whole")
    .allowed({"1280x720", "1120x624", "1040x576", "960x544", "880x496", "800x448"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace airborne::scene {

Size Configured() {
  static const Size size = [] {
    const std::string value = REXCVAR_GET(airborne_scene_resolution);
    const size_t x = value.find('x');
    Size s;
    if (x != std::string::npos) {
      s.width_px = uint32_t(std::strtoul(value.c_str(), nullptr, 10));
      s.height_px = uint32_t(std::strtoul(value.c_str() + x + 1, nullptr, 10));
    }
    if (s.width_px == 0 || s.height_px == 0 || s.width_px > 1280 || s.height_px > 720 ||
        (s.width_px == 1280 && s.height_px == 720)) {
      s = Size{};
    }
    return s;
  }();
  return size;
}

void ApplyToCommandLine() {
  const Size size = Configured();
  if (!size.width_px) {
    return;
  }
  std::string command_line = REXCVAR_GET(cl);
  if (command_line.find("ResX=") != std::string::npos || command_line.find("ResY=") != std::string::npos) {
    REXLOG_INFO("[scene] the command line already has ResX/ResY ({}): airborne_scene_resolution {}x{} not added",
                command_line, size.width_px, size.height_px);
    return;
  }
  command_line += (command_line.empty() ? "" : " ") + std::string("ResX=") + std::to_string(size.width_px) +
                  " ResY=" + std::to_string(size.height_px);
  if (!rex::cvar::SetFlagByName("cl", command_line)) {
    REXLOG_WARN("[scene] cannot set cl = {}", command_line);
    return;
  }
  REXLOG_INFO("[scene] scene at {}x{}: guest command line \"{}\"", size.width_px, size.height_px, command_line);
}

}  // namespace airborne::scene
