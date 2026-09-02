// ============================================================================
// config.cpp - NVS-backed persistent settings + RAM debug log ring buffer
// ============================================================================
#include "config.h"
#include "mcp.h"
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

static String timeStr(time_t t) {
    struct tm tmv; gmtime_r(&t, &tmv);
    char buf[20];
    strftime(buf, sizeof(buf), "%m-%d %H:%M:%S", &tmv);
    return String(buf);
}

static void ringPush(const String& entry) {
    if (!s_logMtx) s_logMtx = xSemaphoreCreateMutex();
    xSemaphoreTake(s_logMtx, portMAX_DELAY);
    s_ring[s_head] = entry;
    s_head = (s_head + 1) % LOG_LINES;
    if (s_count < LOG_LINES) s_count++;
    xSemaphoreGive(s_logMtx);
}

void logRestoreTail() {
    // Boot: pull the last lines of the encrypted SD log into the RAM ring so
    // a crash's final moments survive the reboot.
    if (SD_MMC.cardType() == CARD_NONE) return;
    String existing;
    if (!decryptFromFile("/logs/system.log.enc", existing) || existing.length() < 5) return;
    int nl = 0, cut = 0;
    for (int i = existing.length() - 1; i >= 0; i--)
        if (existing[i] == '\n' && ++nl >= 8) { cut = i + 1; break; }
    String tail = existing.substring(cut);
    int start = 0;
    while (start < (int)tail.length()) {
        int e = tail.indexOf('\n', start);
        if (e < 0) e = tail.length();
        String ln = tail.substring(start, e);
        ln.trim();
        if (ln.length()) ringPush(ln);
        start = e + 1;
    }
}

void logLine(const String& s) {
    if (!s_logMtx) s_logMtx = xSemaphoreCreateMutex();
    // absolute UTC timestamp once NTP has synced; relative seconds before that
    time_t now = time(nullptr);
    String entry = (now > 1000000000)
        ? String("[") + timeStr(now) + "] " + s
        : String(millis() / 1000) + "s " + s;
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
    mcp::resBump("lilystrike://logs/system");   // Phase 2: resource version bump
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
        // CRASH-SAFE WRITE: the log is the most frequent SD write, and a
        // crash mid-write corrupts the FAT (field bug: boot loop). Write to
        // a temp file and atomically rename over the target.
        if (encryptToFile("/logs/system.log.tmp", existing)) {
            SD_MMC.remove("/logs/system.log.enc");
            SD_MMC.rename("/logs/system.log.tmp", "/logs/system.log.enc");
        }
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
