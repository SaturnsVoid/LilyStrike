// ============================================================================
// webserver.cpp - WiFi AP + auth + REST API for the web UI
// ----------------------------------------------------------------------------
// Auth model: POST /api/login checks cfg.webUser/webPass, then issues a random
// session token kept ONLY in RAM (reboot = logout everywhere). The token rides
// in the "sid" cookie; every /api/* route (except login) requires it.
//
// API surface (all JSON unless noted):
//   POST /api/login {user,pass}          -> {ok}
//   GET  /api/status                     -> system stats + script state
//   GET  /api/log                        -> debug log text
//   POST /api/reboot | /api/reset | /api/format-sd
//   GET  /api/scripts                    -> [{name,size}] (.ds on SD)
//   GET  /api/script?name=x              -> decrypted text
//   POST /api/script {name,text}         -> save encrypted
//   DEL  /api/script?name=x              -> delete
//   POST /api/run    {name?|text?,autostart?} -> run now or set autostart order
//   GET  /api/autostart                  -> [names in order]
//   POST /api/stop
//   GET  /api/files?path=/x              -> dir listing
//   GET  /api/file?path=/x               -> raw content (text)
//   POST /api/file {path,content}        -> write
//   POST /api/upload?path=/x             -> binary body
//   DEL  /api/file?path=/x
// Settings: POST /api/settings {wifi|login|enc|display|interface groups}
// ============================================================================
#include "webserver.h"
#include "config.h"
#include "crypt.h"
#include "hw.h"
#include "ducky.h"
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <SD_MMC.h>
#include "util.h"
#include <esp_system.h>
#include <esp32-hal.h>

namespace web {

struct AutostartEntry { char name[64]; };   // ordered list of scripts to run on USB plug-in

static WebServer server(80);
static String s_sessionToken;
static bool s_running = false;

// ------------------------------------------------------------------ helpers
static bool isAuthed() {
    if (!s_sessionToken.length()) return false;
    if (!server.hasHeader("Cookie")) return false;
    return server.header("Cookie").indexOf("sid=" + s_sessionToken) >= 0;
}

static void json(int code, const String& body) {
    server.send(code, "application/json", body);
}
static void jsonErr(int code, const String& msg) {
    json(code, "{\"ok\":false,\"error\":\"" + msg + "\"}");
}

// Wrap handler bodies with the auth gate.
static void requireAuth() {
    if (!isAuthed()) { jsonErr(401, "unauthorized"); }
}

static bool sendArgJson(const char* key, String& out) {
    out = server.arg(key); return true;
}

// ------------------------------------------------------------------- routes
static void hLogin() {
    String u = server.arg("plain");
    // parse tiny JSON by hand to avoid ArduinoJson dependency here
    String user, pass;
    if (!extractJsonStr(u, "user", user) || !extractJsonStr(u, "pass", pass))
        return jsonErr(400, "bad request");
    if (user != cfg.webUser || pass != cfg.webPass) {
        logLine("web: failed login");
        return jsonErr(401, "bad credentials");
    }
    // 24 hex chars from hardware RNG
    uint8_t rnd[12]; esp_fill_random(rnd, sizeof(rnd));
    s_sessionToken = "";
    for (uint8_t b : rnd) { char t[3]; snprintf(t, 3, "%02x", b); s_sessionToken += t; }
    server.sendHeader("Set-Cookie", "sid=" + s_sessionToken + "; Path=/; HttpOnly");
    logLine("web: login ok");
    json(200, "{\"ok\":true}");
}

static void hStatus() {
    requireAuth(); if (!isAuthed()) return;
    uint64_t freeSk = hw::sdTotalBytes() ? hw::sdUsedBytes() : 0;
    String s = "{";
    s += "\"heap\":" + String(ESP.getFreeHeap()) +
         ",\"heapMin\":" + String(ESP.getMinFreeHeap()) +
         ",\"cpuMhz\":" + String(getCpuFrequencyMhz()) +
         ",\"uptime\":" + String(millis() / 1000) +
         ",\"flashSize\":" + String(ESP.getFlashChipSize()) +
         ",\"sdTotal\":" + String((uint32_t)hw::sdTotalBytes()) +
         ",\"sdFree\":" + String((uint32_t)(hw::sdTotalBytes() ? (hw::sdTotalBytes()-SD_MMC.usedBytes()) : 0)) +
         ",\"usbHost\":" + String(g_state.usbHostPresent ? "true" : "false") +
         ",\"wifiClients\":" + String(WiFi.softAPgetStationNum()) +
         ",\"ip\":\"" + localIP() + "\"" +
         ",\"scriptState\":\"" + ducky::stateString() + "\"" +
         ",\"scriptName\":\"" + g_state.lastScriptName + "\"" +
         ",\"scriptSince\":" + String((uint32_t)g_state.scriptStateSince) +
         "}";
    json(200, s);
}

static void hLogGet() {
    requireAuth(); if (!isAuthed()) return;
    server.send(200, "text/plain", logGetAll());
}

static void hReboot() {
    requireAuth(); if (!isAuthed()) return;
    json(200, "{\"ok\":true}");
    logLine("web: reboot requested");
    delay(300);
    ESP.restart();
}

static void hFactoryReset() {
    requireAuth(); if (!isAuthed()) return;
    configFactoryReset();
    logLine("web: factory reset done");
    json(200, "{\"ok\":true}");
}

static void hFormatSD() {
    requireAuth(); if (!isAuthed()) return;
    bool ok = hw::sdWipe();
    logLine(String("web: SD wipe ") + (ok ? "OK" : "FAILED"));
    json(ok ? 200 : 500, String("{\"ok\":") + (ok?"true":"false") + "}");
}

// ---- scripts ---------------------------------------------------------------
static String sanitizeName(const String& n) {
    String s = n;
    s.replace("/", ""); s.replace("..", "");
    return s.isEmpty() ? String("untitled.ds") : s;
}

static void hScriptsList() {
    requireAuth(); if (!isAuthed()) return;
    String out = "[";
    File dir = SD_MMC.open("/scripts");
    if (dir && dir.isDirectory()) {
        bool first = true;
        File f;
        while ((f = dir.openNextFile())) {
            if (!f.isDirectory()) {
                if (!first) out += ",";
                first = false;
                out += "{\"name\":\"" + String(f.name()) + "\",\"size\":" + String(f.size()) + "}";
            }
            f.close();
        }
    }
    out += "]";
    json(200, out);
}

static void hScriptGet() {
    requireAuth(); if (!isAuthed()) return;
    String name = sanitizeName(server.arg("name"));
    String txt;
    if (!decryptFromFile(("/scripts/" + name).c_str(), txt))
        return jsonErr(404, "not found or wrong encryption password");
    server.send(200, "text/plain", txt);
}

static void hScriptSave() {
    requireAuth(); if (!isAuthed()) return;
    String body = server.arg("plain"), name, text;
    if (!extractJsonStr(body, "name", name) || !extractJsonStr(body, "text", text))
        return jsonErr(400, "bad request");
    name = sanitizeName(name);
    text = normalizeEol(text);   // cross-platform line-ending hygiene
    bool ok = encryptToFile(("/scripts/" + name).c_str(), text);
    logLine("web: saved script " + name);
    json(ok ? 200 : 500, String("{\"ok\":") + (ok?"true":"false") + "}");
}

static void hScriptDelete() {
    requireAuth(); if (!isAuthed()) return;
    String name = sanitizeName(server.arg("name"));
    bool ok = SD_MMC.remove(("/scripts/" + name).c_str());
    json(ok ? 200 : 500, String("{\"ok\":") + (ok ? "true" : "false") + "}");
}

// ---- run/stop/autostart -----------------------------------------------------
static AutostartEntry s_autostart[8]; static int s_autostartCount = 0;

static void loadAutostart() {
    s_autostartCount = 0;
    String t;
    if (decryptFromFile("/autostart.enc", t)) {
        int start = 0;
        while (start < (int)t.length() && s_autostartCount < 8) {
            int nl = t.indexOf('\n', start);
            String line = t.substring(start, nl < 0 ? t.length() : nl);
            line.trim();
            if (line.length()) strlcpy(s_autostart[s_autostartCount++].name, line.c_str(), 64);
            if (nl < 0) break; start = nl + 1;
        }
    }
}
static void saveAutostart() {
    String t;
    for (int i = 0; i < s_autostartCount; i++) t += String(s_autostart[i].name) + "\n";
    encryptToFile("/autostart.enc", t);
}

static void runScriptTask(void* pv) {
    auto* p = (std::pair<String,String>*)pv;   // <text,name>
    ducky::run(p->first, p->second);
    delete p;
    vTaskDelete(nullptr);
}

static void startRun(const String& text, const String& name) {
    auto* p = new std::pair<String,String>(text, name);
    xTaskCreatePinnedToCore(runScriptTask, "ducky", 8192, p, 1, nullptr, 0);
}

static void hRun() {
    requireAuth(); if (!isAuthed()) return;
    if (!hw::sdMount() && !server.arg("plain").length()) return jsonErr(500, "no sd");
    String body = server.arg("plain"), name, text;
    extractJsonStr(body, "name", name);
    extractJsonStr(body, "text", text);
    if (name.length() && !text.length())
        decryptFromFile(("/scripts/" + sanitizeName(name)).c_str(), text);
    if (!text.length()) return jsonErr(400, "nothing to run");
    logLine("web: running script " + (name.isEmpty()?String("(inline)"):name));
    startRun(text, name.isEmpty() ? String("inline") : name);
    json(200, "{\"ok\":true}");
}

static void hStop() {
    requireAuth(); if (!isAuthed()) return;
    ducky::stop();
    json(200, "{\"ok\":true}");
}

static void hAutostartGet() {
    requireAuth(); if (!isAuthed()) return;
    loadAutostart();
    String out = "[";
    for (int i = 0; i < s_autostartCount; i++) {
        if (i) out += ",";
        out += "\"" + String(s_autostart[i].name) + "\"";
    }
    json(200, out + "]");
}

static void hAutostartSet() {
    requireAuth(); if (!isAuthed()) return;
    // body: {"names":["a.ds","b.ds"]}
    std::vector<String> names;
    if (!extractJsonArr(server.arg("plain"), "names", names)) return jsonErr(400, "bad request");
    s_autostartCount = 0;
    for (auto& n : names) {
        if (s_autostartCount >= 8) break;
        strlcpy(s_autostart[s_autostartCount++].name, sanitizeName(n).c_str(), 64);
    }
    saveAutostart();
    json(200, "{\"ok\":true}");
}

// ---- file browser ------------------------------------------------------------
static void hFilesList() {
    requireAuth(); if (!isAuthed()) return;
    String path = server.arg("path"); if (!path.length()) path = "/";
    if (path.endsWith("/") && path.length() > 1) path.remove(path.length()-1);
    File dir = SD_MMC.open(path);
    if (!dir || !dir.isDirectory()) return jsonErr(404, "no such dir");
    String out = "["; bool first = true; File f;
    while ((f = dir.openNextFile())) {
        if (!first) out += ",";
        first = false;
        out += "{\"name\":\"" + String(f.name()) + "\",\"dir\":" +
               (f.isDirectory() ? String("true") : String("false")) +
               ",\"size\":" + String(f.size()) + "}";
        f.close();
    }
    json(200, out + "]");
}

static void hFileGet() {
    requireAuth(); if (!isAuthed()) return;
    String path = server.arg("path"); if (!path.length()) return jsonErr(400, "?path=");
    // .ds files are stored encrypted on SD - decrypt transparently so the
    // browser shows readable text. Other files pass through raw.
    if (path.endsWith(".ds")) {
        String txt;
        if (!decryptFromFile(path.c_str(), txt))
            return jsonErr(500, "decrypt failed (wrong encryption password?)");
        return server.send(200, "text/plain", txt);
    }
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return jsonErr(404, "not found");
    server.streamFile(f, "application/octet-stream");
    f.close();
}

static void hFileSave() {
    requireAuth(); if (!isAuthed()) return;
    String body = server.arg("plain"), path, content;
    if (!extractJsonStr(body, "path", path) || !extractJsonStr(body, "content", content))
        return jsonErr(400, "bad request");
    bool ok;
    if (path.endsWith(".ds")) {
        ok = encryptToFile(path.c_str(), content);   // keep scripts at rest
    } else {
        File f = SD_MMC.open(path, FILE_WRITE);
        if (!f) return jsonErr(500, "cannot open");
        size_t w = f.print(content);
        f.close();
        ok = (w == content.length());
    }
    logLine("web: saved " + path);
    json(ok ? 200 : 500, String("{\"ok\":") + (ok ? "true":"false") + "}");
}

static void hMkdir() {
    requireAuth(); if (!isAuthed()) return;
    String path = server.arg("path"); if (!path.length()) return jsonErr(400, "?path=");
    json(SD_MMC.mkdir(path) ? 200 : 500, "{\"ok\":true}");
}

// ---- hardware test endpoints (also used by Step-2 SCREEN_/LED_ commands) ---
static void hDevLed() {
    requireAuth(); if (!isAuthed()) return;
    // /api/dev/led?r=255&g=0&b=0   or   /api/dev/led?off=1
    if (server.arg("off") == "1") { hw::ledOff(); return json(200, "{\"ok\":true}"); }
    RGB c{(uint8_t)server.arg("r").toInt(),
          (uint8_t)server.arg("g").toInt(),
          (uint8_t)server.arg("b").toInt()};
    hw::ledSet(c);
    json(200, "{\"ok\":true}");
}
static void hDevScreen() {
    requireAuth(); if (!isAuthed()) return;
    String a = server.arg("action");
    if      (a == "on")  hw::screenOn();
    else if (a == "off") hw::screenOff();
    else if (a == "text") hw::screenText(server.arg("t"));
    else if (a == "clear")hw::screenClear();
    else return jsonErr(400, "action=on|off|text|clear");
    json(200, "{\"ok\":true}");
}

static void hUpload() {
    requireAuth(); if (!isAuthed()) return;
    String path = server.arg("path");
    HTTPUpload& up = server.upload();
    static File tmp;
    if (up.status == UPLOAD_FILE_START) {
        tmp = SD_MMC.open(path, FILE_WRITE);
    } else if (up.status == UPLOAD_FILE_WRITE && tmp) {
        tmp.write(up.buf, up.currentSize);
    } else if (up.status == UPLOAD_FILE_END && tmp) {
        tmp.close();
        logLine("web: uploaded " + path);
    }
    // IMPORTANT: never send a response from inside the upload handler -
    // WebServer calls our completion lambda afterwards; sending twice panics.
}

static void hFileDelete() {
    requireAuth(); if (!isAuthed()) return;
    String path = server.arg("path");
    bool ok = SD_MMC.remove(path) || SD_MMC.rmdir(path);
    json(ok ? 200 : 500, String("{\"ok\":") + (ok ? "true":"false") + "}");
}

// ---- settings ------------------------------------------------------------------
static void hSettings() {
    requireAuth(); if (!isAuthed()) return;
    String body = server.arg("plain"), v;

    // Empty fields = "leave unchanged" (UI sends only filled boxes).
    if (extractJsonStr(body, "ssid", v) && v.length())
        strlcpy(cfg.wifiSSID, v.c_str(), sizeof(cfg.wifiSSID));
    if (extractJsonStr(body, "wifiPass", v) && v.length()) strlcpy(cfg.wifiPass, v.c_str(), sizeof(cfg.wifiPass));
    if (extractJsonStr(body, "user", v) && v.length()) strlcpy(cfg.webUser, v.c_str(), sizeof(cfg.webUser));
    if (extractJsonStr(body, "webPass", v) && v.length()) strlcpy(cfg.webPass, v.c_str(), sizeof(cfg.webPass));
    if (extractJsonStr(body, "encPassword", v) && v.length()) strlcpy(cfg.encPassword, v.c_str(), sizeof(cfg.encPassword));

    configSaveWiFi(); configSaveLogin(); configSaveEncryption();

    // display group
    extractJsonStr(body, "brightness", v);
    // interface flags come as booleans - hand-rolled detection:
    if (body.indexOf("\"screenOnBoot\":true") >= 0)  cfg.screenOnBoot = true;
    if (body.indexOf("\"screenOnBoot\":false") >= 0) cfg.screenOnBoot = false;
    if (body.indexOf("\"ledOnBoot\":true") >= 0)     cfg.ledOnBoot = true;
    if (body.indexOf("\"ledOnBoot\":false") >= 0)    cfg.ledOnBoot = false;
    if (body.indexOf("\"tempOff\":true") >= 0)       cfg.ifaceTempOff = true;      // takes effect after reboot
    if (body.indexOf("\"tempOff\":false") >= 0)      cfg.ifaceTempOff = false;
    if (body.indexOf("\"permOff\":true") >= 0)       cfg.ifaceDisabledPerm = true; // WARNING: irreversible without reflash
    configSaveDisplay(); configSaveInterfaceFlags();

    logLine("web: settings updated");
    json(200, "{\"ok\":true}");
}

// ---- static UI -------------------------------------------------------------------
// NOTE: files live under /www inside LittleFS (data/www -> image root keeps
// folder), so browser paths must be mapped to /www/<path>.
static void serveWWW(const char* browserPath) {
    String fsPath = String("/www") + browserPath;
    File f = LittleFS.open(fsPath, "r");
    if (!f) { server.send(500, "text/plain", "UI missing - run 'pio run -t uploadfs'"); return; }
    server.streamFile(f, String(browserPath).endsWith(".css") ? "text/css" :
                          String(browserPath).endsWith(".js") ? "application/javascript" : "text/html");
    f.close();
}
static void hIndex() {
    if (!isAuthed()) { server.sendHeader("Location", "/login.html"); server.send(302); return; }
    serveWWW("/index.html");
}
static void hStatic() {
    String p = server.uri();
    if (p != "/login.html" && p != "/app.js" && p != "/style.css") { server.send(404); return; }
    serveWWW(p.c_str());
}

// ---------------------------------------------------------------------------
bool begin() {
    // Mount the internal flash filesystem that holds /www (web UI).
    if (!LittleFS.begin(true)) {           // true = format-on-fail (first boot)
        logLine("web: LittleFS mount FAILED");
        return false;
    }
    if (!LittleFS.exists("/www/index.html"))
        logLine("web: warning - /www/index.html absent, did you run 'pio run -t uploadfs'?");

    if (cfg.ifaceDisabledPerm) {           // permanent kill switch
        logLine("iface disabled permanently (settings)");
        return false;
    }
    if (cfg.ifaceTempOff) {                // temporary off until button press
        logLine("iface temp-disabled; press BOOT to re-enable");
        return false;
    }
    WiFi.mode(WIFI_AP);
    WiFi.softAP(cfg.wifiSSID, strlen(cfg.wifiPass) >= 8 ? cfg.wifiPass : "dongle1234");

    server.on("/api/login", HTTP_POST, hLogin);
    server.on("/api/status", HTTP_GET, hStatus);
    server.on("/api/log", HTTP_GET, hLogGet);
    server.on("/api/reboot", HTTP_POST, hReboot);
    server.on("/api/reset", HTTP_POST, hFactoryReset);
    server.on("/api/format-sd", HTTP_POST, hFormatSD);
    server.on("/api/scripts", HTTP_GET, hScriptsList);
    server.on("/api/script", HTTP_GET, hScriptGet);
    server.on("/api/script", HTTP_POST, hScriptSave);
    server.on("/api/script", HTTP_DELETE, hScriptDelete);
    server.on("/api/run", HTTP_POST, hRun);
    server.on("/api/stop", HTTP_POST, hStop);
    server.on("/api/autostart", HTTP_GET, hAutostartGet);
    server.on("/api/autostart", HTTP_POST, hAutostartSet);
    server.on("/api/files", HTTP_GET, hFilesList);
    server.on("/api/file", HTTP_GET, hFileGet);
    server.on("/api/file", HTTP_POST, hFileSave);
    server.on("/api/file", HTTP_DELETE, hFileDelete);
    server.on("/api/upload", HTTP_POST, [](){ json(200,"{\"ok\":true}"); }, hUpload);
    server.on("/api/mkdir", HTTP_POST, hMkdir);
    server.on("/api/dev/led", HTTP_GET, hDevLed);       // hardware test
    server.on("/api/dev/screen", HTTP_GET, hDevScreen); // hardware test
    server.on("/api/settings", HTTP_POST, hSettings);
    server.on("/", HTTP_GET, hIndex);
    server.on("/index.html", HTTP_GET, hIndex);
    server.onNotFound(hStatic);
    // collect cookies
    const char* hdrKeys[] = {"Cookie"};
    server.collectHeaders(hdrKeys, 1);
    server.begin();
    s_running = true;
    logLine(String("web: AP \"") + cfg.wifiSSID + "\" up at " + localIP());
    return true;
}

void handle() { if (s_running) server.handleClient(); }

String localIP() { return WiFi.softAPIP().toString(); }

} // namespace web
