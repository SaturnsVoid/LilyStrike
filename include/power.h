// ============================================================================
// power.h - Power modes (Step 4)
// Low    = 80 MHz CPU | 10 dBm WiFi  (max stealth, slowest scripts)
// Normal = 160 MHz    | 17 dBm
// High   = 240 MHz    | 19.5 dBm   (fastest, may trip weak USB ports)
// NOTE: script DELAY timings shrink/grow with clock speed - payload authors
// may need to re-tune delays when switching modes.
// ============================================================================
#pragma once
#include <Arduino.h>

namespace power {
// NOTE: can't use LOW/HIGH as identifiers - Arduino defines them as macros!
enum Mode : uint8_t { PM_LOW = 0, PM_NORMAL = 1, PM_HIGH = 2 };
void load();                       // NVS -> state (default NORMAL)
void set(Mode m);                  // persist
Mode mode();
void apply();                      // set CPU freq + WiFi TX power live
}
