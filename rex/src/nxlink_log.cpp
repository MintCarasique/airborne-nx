// Live log over the network when the NRO is launched with tools/nxlink.py -s.

#include "nxlink_log.h"

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH

#include <switch.h>

#include <memory>

#include <rex/logging.h>
#include <spdlog/sinks/stdout_sinks.h>

void StartNxlinkLog() {
  if (__nxlink_host.s_addr == 0) {
    return;  // not started through nxlink
  }
  // The runtime may already use sockets; a second initialization fails
  // harmlessly and nxlink still connects with the existing service.
  Result rc = socketInitializeDefault();
  int fd = nxlinkStdio();
  if (fd < 0) {
    REXLOG_WARN("[nxlink] cannot connect to the host (socketInitialize 0x{:X})", rc);
    return;
  }
  auto sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
  sink->set_pattern("[%H:%M:%S.%e] [%l] [%n] %v");
  rex::AddSink(sink);
  REXLOG_INFO("[nxlink] log streaming to the host");
}

#else

void StartNxlinkLog() {}

#endif
