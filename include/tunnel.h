// ============================================================================
// tunnel.h - External access relay (Step 4)
// ----------------------------------------------------------------------------
// When the device is on an internet-connected network AND tunnel access is
// enabled, a background task polls a relay server for pending requests and
// answers them against the local web UI. This lets the operator reach the
// device's interface from anywhere (e.g. through the included
// tools/relay_server.py hosted on any small VPS).
// Enable via Settings page or TUNNEL ON / TUNNEL OFF in a script.
// ============================================================================
#pragma once
namespace tunnel {
void loadAndMaybeStart();   // called from loop(): spawns task when conditions met
void setEnabled(bool on);   // persists + starts/stops
bool enabled();
bool running();
}
