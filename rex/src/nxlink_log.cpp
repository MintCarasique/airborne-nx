// Live log over the network when the NRO is launched with tools/nxlink.py -s,
// and the switches that a network launch cannot set on the SD by hand.

#include "nxlink_log.h"

#include <rex/cvar.h>
#include <rex/platform.h>

// The SDK profiler (switch_perf.cpp) samples stacks at 1 kHz only when
// <NRO folder>/logs/rex/perfil_pilas.flag exists when it starts (8 s after
// boot). profile_stacks creates or removes that file, so a launch through
// nxlink can turn sampling on with --profile_stacks=true.
REXCVAR_DEFINE_BOOL(profile_stacks, false, "Debug",
                    "Switch: enable the SDK profiler's 1 kHz stack sampling for this run");

#if REX_PLATFORM_SWITCH

// struct in_addr for libnx's __nxlink_host; switch.h does not include it.
#include <arpa/inet.h>
#include <switch.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include <rex/logging.h>
#include <spdlog/sinks/stdout_sinks.h>

extern "C" const char* RexSwitchLogDir(void);

namespace {

void SetStackSamplingFlag() {
  const std::string flag = std::string(RexSwitchLogDir()) + "perfil_pilas.flag";
  if (REXCVAR_GET(profile_stacks)) {
    if (FILE* f = std::fopen(flag.c_str(), "w")) {
      std::fclose(f);
      REXLOG_INFO("[perf] stack sampling on ({})", flag);
    } else {
      REXLOG_WARN("[perf] cannot create {}", flag);
    }
  } else {
    std::remove(flag.c_str());
  }
}

// The profiler writes its 10 s reports only to the SD; with nxlink they are
// copied to stdout as they grow, so the host sees them live.
std::atomic<bool> g_forward_stop{false};
std::thread g_forward_thread;

void ForwardProfilerReport() {
  const std::string path = std::string(RexSwitchLogDir()) + "rex_perfil.log";
  long offset = 0;
  char buffer[4096];
  while (!g_forward_stop.load(std::memory_order_relaxed)) {
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
      std::fseek(f, 0, SEEK_END);
      const long size = std::ftell(f);
      if (size < offset) {
        offset = 0;  // the profiler truncates the file at its start
      }
      std::fseek(f, offset, SEEK_SET);
      size_t n;
      while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) {
        std::fwrite(buffer, 1, n, stdout);
        offset += long(n);
      }
      std::fclose(f);
      std::fflush(stdout);
    }
    for (int i = 0; i < 20 && !g_forward_stop.load(std::memory_order_relaxed); ++i) {
      svcSleepThread(100'000'000);
    }
  }
}

}  // namespace

void StopNxlinkLog() {
  g_forward_stop.store(true, std::memory_order_relaxed);
  if (g_forward_thread.joinable()) {
    g_forward_thread.join();
  }
}

void StartNxlinkLog() {
  if (__nxlink_host.s_addr != 0) {
    // The runtime may already use sockets; a second initialization fails
    // harmlessly and nxlink still connects with the existing service.
    Result rc = socketInitializeDefault();
    if (nxlinkStdio() < 0) {
      REXLOG_WARN("[nxlink] cannot connect to the host (socketInitialize 0x{:X})", rc);
    } else {
      auto sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
      sink->set_pattern("[%H:%M:%S.%e] [%l] [%n] %v");
      rex::AddSink(sink);
      REXLOG_INFO("[nxlink] log streaming to the host");
      g_forward_thread = std::thread(ForwardProfilerReport);
    }
  }
  SetStackSamplingFlag();
}

#else

void StartNxlinkLog() {}
void StopNxlinkLog() {}

#endif
