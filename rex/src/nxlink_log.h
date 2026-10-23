// Live log over the network when the NRO is launched with tools/nxlink.py -s.

#pragma once

// On the Switch, if the NRO was started through nxlink (libnx sets
// __nxlink_host from the _NXLINK_ argument), redirects stdout/stderr to the
// PC and adds a stdout sink to the logger. The file log is unchanged. Does
// nothing elsewhere or when not started through nxlink.
void StartNxlinkLog();
