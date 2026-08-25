// ============================================================================
// config.h - Persistent settings (NVS) + runtime state declarations
// ----------------------------------------------------------------------------
// All user-configurable settings live in NVS (flash) so they survive reboots.
// SD-card file encryption derives its key from cfg.encPassword at use time.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <Preferences.h>

struct DeviceConfig {
    // WiFi AP the device hosts by default
    char     wifiSSID[33];
    char     wifiPass[65];
    // Web interface login
    char     webUser[33];
    char     webPass[65];
    // Encryption passphrase for scripts/logs stored on SD (PBKDF2 -> AES key)
    char     encPassword[65];
    // Default screen/LED behaviour on boot ("normally all off")
    bool     screenOnBoot;
    bool     ledOnBoot;
    uint8_t  screenBrightness;   // 0..255 PWM backlight
    // Auto-run OS detection when plugged into a computer (Step 2)
    bool     autoDetectOS;       // result cached until device unplugged
    // Interface availability
    bool     ifaceDisabledPerm;  // permanent: no UI until reflash
    bool     ifaceTempOff;       // temporary: BOOT button re-enables
};

extern DeviceConfig cfg;

// Load all settings from NVS into `cfg`, applying factory defaults for any
// missing key. Called once from setup().
void configLoad();

// Persist a single field group. Fine-grained saves avoid wearing NVS.
void configSaveWiFi();
void configSaveLogin();
void configSaveEncryption();
void configSaveDisplay();
void configSaveInterfaceFlags();
void configSaveAutoOS();

// Wipe everything back to defaults (Settings page "Reset Firmware" light path).
void configFactoryReset();

// ---- Runtime state shared between modules ---------------------------------
enum class ScriptState : uint8_t { STANDBY, RUNNING, FINISHED };
struct RuntimeState {
    ScriptState scriptState = ScriptState::STANDBY;
    time_t      scriptStateSince = 0;
    String      lastScriptName = "-";
    bool        usbHostPresent = false;   // plugged into a PC vs powerbank
    String      detectedOS = "Unknown";   // Step 2: last DETECT_OS result
    bool        thumbMode = false;        // Step 3: running as false thumbdrive
    bool        evilApRunning = false;    // Step 3: portal active
};
extern RuntimeState g_state;

// ---- Debug log ring buffer -------------------------------------------------
// Kept in RAM (last N lines) for the Status page AND appended encrypted to
// /logs/system.log.enc on the SD card when it is mounted.
#define LOG_LINES 40
void logLine(const String& s);            // add to RAM ring + SD (encrypted)
String logGetAll();                       // newline-joined RAM log for web UI
