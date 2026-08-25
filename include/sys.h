// ============================================================================
// sys.h - destructive system operations (Self Destruct, Step 3)
// ----------------------------------------------------------------------------
// Self destruct wipes every user-reachable store: NVS settings (including the
// spoof identity), the internal web-UI filesystem, and all SD card files -
// then reboots. Firmware itself remains (erasing the running app partition is
// not safely possible from inside it); recovery to factory behavior is a
// normal re-flash. Triggered from the Status page or SELF_DESTRUCT in script.
// ============================================================================
#pragma once

namespace sys {

// Wipe everything and reboot. No confirmation here - callers must do that.
void selfDestruct();

} // namespace sys
