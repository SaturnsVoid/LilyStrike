// ============================================================================
// sys.cpp - self destruct implementation
// ============================================================================
#include "sys.h"
#include "config.h"
#include "hw.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_system.h>

namespace sys {

void selfDestruct() {
    logLine("!!! SELF DESTRUCT INITIATED !!!");
    // 1) user settings + spoofed identity
    configFactoryReset();
    { Preferences p; p.begin("spoof", false); p.clear(); p.end(); }

    // 2) web UI assets
    if (LittleFS.begin(true)) LittleFS.format();

    // 3) everything on the SD card
    if (hw::sdMount()) hw::sdWipe();

    logLine("self destruct complete - rebooting");
    delay(500);
    ESP.restart();
}

} // namespace sys
