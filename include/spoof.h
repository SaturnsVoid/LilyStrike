// ============================================================================
// spoof.h - USB identity spoofing (Step 3)
// ----------------------------------------------------------------------------
// Changes what the device enumerates as on the host: VID/PID, manufacturer,
// product name and serial number. Applied in ducky::initOnce() BEFORE
// USB.begin() - descriptors are baked into enumeration so a new identity
// only takes effect at the next plug-in/boot.
// Defaults to a RANDOM preset from the table below on first boot; the user
// can pin a specific one (or custom values) via Settings.
// ============================================================================
#pragma once
#include <Arduino.h>

struct SpoofPreset {
    uint16_t vid, pid;
    const char* vendor;
    const char* product;
};

namespace spoof {

extern const SpoofPreset PRESETS[];
extern const size_t PRESET_COUNT;

void load();                    // read NVS -> apply defaults if unset
void save();                    // persist current values to NVS

// Randomize from the preset table (also sets serial random 12-digit).
void randomize();

uint16_t vid();
uint16_t pid();
String   vendor();
String   product();
String   serial();

void set(uint16_t vid, uint16_t pid, const String& vendor,
         const String& product, const String& serial);

// Push values into the TinyUSB descriptors. MUST be called before
// USB.begin() (ducky::initOnce does this).
void applyToUsb();

} // namespace spoof
