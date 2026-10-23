// Timed RenderDoc capture trigger. Syntax of the `renderdoc_capture_at` cvar:
//   <t>[,<t2>...]   (seconds since StartRenderDocTrigger, e.g. "40,48.5")
// Launch the app under RenderDoc; captures are written next to
// `renderdoc_capture_path` (RenderDoc appends its own suffix).

#include "renderdoc_trigger.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>

REXCVAR_DEFINE_STRING(renderdoc_capture_at, "", "Debug",
                      "Seconds after startup to trigger RenderDoc captures, e.g. \"40,48.5\"");
REXCVAR_DEFINE_STRING(renderdoc_capture_path, "", "Debug",
                      "RenderDoc capture file path template (RenderDoc appends a suffix)");

// REX_PLATFORM_WIN32 comes from rex/platform.h; REX_PLATFORM_WINDOWS is only
// a compile definition inside the SDK and is not set for the app.
#if REX_PLATFORM_WIN32

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <rex/ui/renderdoc_api.h>

namespace {

// Never detached (detach() kills the process on the Switch). A joinable
// std::thread destroyed at exit calls std::terminate, so it is joined then.
struct TriggerThread {
  std::thread thread;
  ~TriggerThread() {
    if (thread.joinable()) thread.join();
  }
} g_trigger;
std::unique_ptr<rex::ui::RenderDocAPI> g_renderdoc;

std::vector<double> ParseTimes(const std::string& text) {
  std::vector<double> times;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t comma = text.find(',', pos);
    if (comma == std::string::npos) comma = text.size();
    std::string item = text.substr(pos, comma - pos);
    pos = comma + 1;
    size_t b = item.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) continue;
    item = item.substr(b, item.find_last_not_of(" \t\r\n") - b + 1);
    char* end = nullptr;
    double v = std::strtod(item.c_str(), &end);
    if (end != item.c_str() + item.size() || v < 0) {
      REXLOG_WARN("[renderdoc] bad time '{}', skipped", item);
      continue;
    }
    times.push_back(v);
  }
  std::sort(times.begin(), times.end());
  return times;
}

}  // namespace

void StartRenderDocTrigger() {
  std::string at = REXCVAR_GET(renderdoc_capture_at);
  if (at.empty() || g_trigger.thread.joinable()) return;

  g_renderdoc = rex::ui::RenderDocAPI::CreateIfConnected();
  if (!g_renderdoc) {
    REXLOG_WARN("[renderdoc] renderdoc_capture_at is set but RenderDoc is not attached");
    return;
  }
  const RENDERDOC_API_1_0_0* api = g_renderdoc->api_1_0_0();

  std::string path = REXCVAR_GET(renderdoc_capture_path);
  if (!path.empty()) api->SetLogFilePathTemplate(path.c_str());

  std::vector<double> times = ParseTimes(at);
  if (times.empty()) {
    REXLOG_WARN("[renderdoc] no valid capture times in '{}'", at);
    return;
  }
  REXLOG_INFO("[renderdoc] {} capture(s) scheduled", times.size());

  auto t0 = std::chrono::steady_clock::now();
  g_trigger.thread = std::thread([api, times, t0]() {
    for (double t : times) {
      std::this_thread::sleep_until(t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                             std::chrono::duration<double>(t)));
      api->TriggerCapture();
      REXLOG_INFO("[renderdoc] capture triggered at t={} s", t);
    }
  });
}

#else

void StartRenderDocTrigger() {}

#endif
