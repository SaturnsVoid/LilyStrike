// ============================================================================
// config.cpp - NVS-backed persistent settings + RAM debug log ring buffer
// ============================================================================
#include "config.h"
#include "crypt.h"
#include "msc.h"
#include "wifiattack.h"
#include <SD_MMC.h>

DeviceConfig cfg;
RuntimeState g_state;

// NVS namespace
static const char* NS = "pcfg";
static Preferences prefs;

// --------------------------------------------------------------- defaults
static void applyDefaults() {
    strlcpy(cfg.wifiSSID, "Dongle-Setup", sizeof(cfg.wifiSSID));
    strlcpy(cfg.wifiPass, "dongle1234", sizeof(cfg.wifiPass));   // change me!
    strlcpy(cfg.webUser, "admin", sizeof(cfg.webUser));
    strlcpy(cfg.webPass, "admin", sizeof(cfg.webPass));
    strlcpy(cfg.encPassword, "changeme", sizeof(cfg.encPassword));
    cfg.screenOnBoot = false;            // stealth default: all off on boot
    cfg.ledOnBoot = false;
    cfg.screenBrightness = 128;
    cfg.autoDetectOS = false;
    cfg.wifiHidden = false;
    strlcpy(cfg.hostname, "lilystrike", sizeof(cfg.hostname));
    cfg.ifaceDisabledPerm = false;
    cfg.ifaceTempOff = false;
}

void configLoad(bool safeMode) {
    applyDefaults();
    if (safeMode) return;                // defaults only - saved config untouched
    prefs.begin(NS, true);               // read-only
    prefs.getString("ssid", cfg.wifiSSID, sizeof(cfg.wifiSSID));
    prefs.getString("wpass", cfg.wifiPass, sizeof(cfg.wifiPass));
    prefs.getString("wuser", cfg.webUser, sizeof(cfg.webUser));
    prefs.getString("wpass2", cfg.webPass, sizeof(cfg.webPass));
    prefs.getString("encp", cfg.encPassword, sizeof(cfg.encPassword));
    cfg.screenOnBoot      = prefs.getBool("scrOn", cfg.screenOnBoot);
    cfg.ledOnBoot         = prefs.getBool("ledOn", cfg.ledOnBoot);
    cfg.screenBrightness  = prefs.getUChar("scrBr", cfg.screenBrightness);
    cfg.autoDetectOS      = prefs.getBool("autoOS", cfg.autoDetectOS);
    cfg.wifiHidden        = prefs.getBool("wifiHidden", cfg.wifiHidden);
    prefs.getString("hostname", cfg.hostname, sizeof(cfg.hostname));
    cfg.ifaceDisabledPerm = prefs.getBool("ifacePerm", cfg.ifaceDisabledPerm);
    cfg.ifaceTempOff      = prefs.getBool("ifaceTemp", cfg.ifaceTempOff);
    prefs.end();
}

// Generic save helper so each group is one line at call sites.
template <typename F> static void withPrefs(F fn) {
    prefs.begin(NS, false); fn(prefs); prefs.end();
}

void configSaveWiFi()   { withPrefs([](Preferences& p){ p.putString("ssid", cfg.wifiSSID); p.putString("wpass", cfg.wifiPass); p.putBool("wifiHidden", cfg.wifiHidden); p.putString("hostname", cfg.hostname); }); }
void configSaveLogin()  { withPrefs([](Preferences& p){ p.putString("wuser", cfg.webUser); p.putString("wpass2", cfg.webPass); }); }
void configSaveEncryption(){ withPrefs([](Preferences& p){ p.putString("encp", cfg.encPassword); }); }
void configSaveDisplay(){ withPrefs([](Preferences& p){ p.putBool("scrOn", cfg.screenOnBoot); p.putBool("ledOn", cfg.ledOnBoot); p.putUChar("scrBr", cfg.screenBrightness); }); }
void configSaveAutoOS() { withPrefs([](Preferences& p){ p.putBool("autoOS", cfg.autoDetectOS); }); }
void configSaveInterfaceFlags() { withPrefs([](Preferences& p){ p.putBool("ifacePerm", cfg.ifaceDisabledPerm); p.putBool("ifaceTemp", cfg.ifaceTempOff); }); }

void configFactoryReset() {
    prefs.begin(NS, false);
    prefs.clear();
    prefs.end();
    applyDefaults();
}

// ---------------------------------------------------------------------------
// Debug log ring buffer. Appended encrypted to /logs/system.log.enc when SD
// is mounted; kept last LOG_LINES lines in RAM for the Status page.
// ---------------------------------------------------------------------------
static String s_ring[LOG_LINES];
static int s_head = 0, s_count = 0;
static SemaphoreHandle_t s_logMtx = nullptr;

void logLine(const String& s) {
    if (!s_logMtx) s_logMtx = xSemaphoreCreateMutex();
    String entry = String(millis() / 1000) + "s " + s;
    xSemaphoreTake(s_logMtx, portMAX_DELAY);
    s_ring[s_head] = entry;
    s_head = (s_head + 1) % LOG_LINES;
    if (s_count < LOG_LINES) s_count++;
    xSemaphoreGive(s_logMtx);
    // Live push to connected web clients (WebSocket event bus). Declared here
    // inline so config.cpp doesn't depend on webserver.cpp's header; if the
    // web stack is down, wsEvent is a no-op that checks ws.count().
    extern void wsEvent(const String&, const String&);
    String esc = entry; esc.replace("\\", "\\\\"); esc.replace("\"", "\\\\\"");
    wsEvent("log", "\"" + esc + "\"");
    // Encrypted append to SD - but NEVER while the host owns the raw card
    // (USB_STORAGE/False Thumbdrive): concurrent FS access corrupts both.
    // No SD I/O while the radio is attacking/capturing (FS + radio don't
    // mix well during TX bursts) or while MSC owns the card.
    if (!msc::active() && !wifiattack::busy() && SD_MMC.cardType() != CARD_NONE) {
        // Atomic read-modify-write: two tasks logging simultaneously could
        // interleave decrypt/encrypt and corrupt the log (or deadlock the
        // card). One lock span for the whole update.
        extern void sdLock(), sdUnlock();
        sdLock();
        String existing;
        decryptFromFile("/logs/system.log.enc", existing);   // empty ok (new/corrupt)
        existing += entry + "\n";
        // Keep the file bounded (~32KB) - drop oldest half.
        if (existing.length() > 32768) existing = existing.substring(existing.length()/2);
        encryptToFile("/logs/system.log.enc", existing);
        sdUnlock();
    }
    Serial.println("[LOG] " + entry);
}

void bootBtnAbortReset() { g_state.bootBtnAbort = false; }

String logGetAll() {
    if (!s_logMtx) return "";
    String out;
    xSemaphoreTake(s_logMtx, portMAX_DELAY);
    for (int i = 0; i < s_count; i++)
        out += s_ring[(s_head + LOG_LINES - s_count + i + LOG_LINES) % LOG_LINES] + "\n";
    xSemaphoreGive(s_logMtx);
    return out;
}
