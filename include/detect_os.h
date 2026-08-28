// ============================================================================
// detect_os.h - Host OS fingerprinting via keyboard LED side-channel
// ----------------------------------------------------------------------------
// Port of joelsernamoreno/ESP32Sx-DetectOS (reference in project plans).
// Technique: toggle CAPS/NUM/SCROLL lock and watch HID LED report events.
//  * No LED events at all + caps state changed locally => iOS
//  * All 3 events, <100ms each                          => Windows
//  * Only numlock responds + numlock ends set           => Linux
//  * Slow (>200ms) responses                            => Android
//  * Single very fast caps event                        => ChromeOS
//  * Nothing, caps unchanged                            => macOS
// ============================================================================
#pragma once
#include <Arduino.h>

enum class HostOS : uint8_t {
    UNKNOWN, WINDOWS, LINUX, MACOS, IOS, ANDROID, CHROMEOS
};

namespace detectos {

// Run the full detection sequence. BLOCKS for ~8-10s while it toggles the
// host's lock keys (and restores them afterwards). Call only when plugged
// into a real computer - on a powerbank it just times out to UNKNOWN.
HostOS detect();

// Cached result of the last detection (UNKNOWN if never run).
HostOS lastResult();
void initHook();                // register LED-event callback (call at boot)
String lockState();             // "CAPS+NUM", "SCROLL", "" ...
bool capsOn(); bool numOn(); bool scrollOn();   // live host lock states
// Block until the named lock ("caps"|"num"|"scroll") is ON (waitOn=true) or
// OFF, up to timeoutMs. Returns true if the state was reached.
bool waitLock(const String& lock, bool waitOn, uint32_t timeoutMs);
String nameOf(HostOS os);       // "Windows", "Linux", ...
bool matches(HostOS a, const String& lowerName);   // "windows" etc.

} // namespace detectos
