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
#include "websrv_shim.h"
#include <map>
#include <LittleFS.h>
#include <SD_MMC.h>
#include "util.h"
#include "spoof.h"
#include <Preferences.h>
#include <esp_system.h>
#include "power.h"
#include "detect_os.h"
#include "sys.h"
#include "msc.h"
#include "evilap.h"
#include "tunnel.h"
#include "scheduler.h"
#include "mcp.h"
#include "wifiattack.h"
#include "hostrecon.h"
#include "version.h"
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_system.h>
#include "power.h"
#include <mbedtls/base64.h>
#include <esp_system.h>
#include <esp32-hal.h>

static WebSrvShim server;                       // same name as before: all 68
                                                // handlers below compile untouched
static AsyncWebServer asrv(80);
static AsyncWebSocket ws("/ws");                // live event push (WifiPhisher-style)
static String s_sessionToken;
static bool s_running = false;

// ---- shim plumbing: current request + per-request raw body ----
static AsyncWebServerRequest* s_cur = nullptr;
static String* s_curBody = nullptr;
static String s_qHeaders;                       // queued "name:value\n" from sendHeader
static std::map<AsyncWebServerRequest*, String> s_bodies;

void WebSrvShim::on(const char* path, WebRequestMethodComposite method, THandlerFunction fn) {
    auto* h = new AsyncCallbackWebHandler();
    h->setUri(path); h->setMethod(method);
    h->onBody([](AsyncWebServerRequest* r, uint8_t* d, size_t len, size_t index, size_t total) {
        s_bodies[r] += String((const char*)d, len);
        // safety cap: our API bodies are small JSON; prevent RAM abuse
        if (s_bodies[r].length() > 65536) s_bodies[r] = s_bodies[r].substring(0, 65536);
    });
    h->onRequest([fn](AsyncWebServerRequest* r) {
        s_cur = r;
        s_curBody = &s_bodies[r];       // may be empty for GETs
        s_qHeaders = "";
        fn();
        s_bodies.erase(r);
        s_cur = nullptr; s_curBody = nullptr;
    });
    asrv.addHandler(h);
}
void WebSrvShim::onNotFound(THandlerFunction fn) {
    auto* h = new AsyncCallbackWebHandler();
    // "/*" glob = match anything not claimed above. NOTE: "/.+" (regex) only
    // works when the lib is built with ASYNCWEBSERVER_REGEX - otherwise it is
    // matched literally and every static file 501s (seen in the field).
    h->setUri("/*");
    h->setMethod((WebRequestMethodComposite)(HTTP_GET | HTTP_POST));
    h->onRequest([fn](AsyncWebServerRequest* r) {
        s_cur = r; s_curBody = &s_bodies[r]; s_qHeaders = "";
        fn();
        s_bodies.erase(r);
        s_cur = nullptr; s_curBody = nullptr;
    });
    asrv.addHandler(h);
}
String WebSrvShim::arg(const char* key) {
    if (!s_cur) return "";
    if (strcmp(key, "plain") == 0) return s_curBody ? *s_curBody : String();
    if (s_cur->method() == HTTP_GET) {
        // query params only for GET (mirrors old WebServer semantics)
        for (int i = 0; i < s_cur->params(); i++)
            if (s_cur->getParam(i)->name() == key) return s_cur->getParam(i)->value();
    } else {
        if (auto* p = s_cur->getParam(key)) return p->value();
    }
    return "";
}
bool WebSrvShim::hasArg(const char* key) { return arg(key).length() > 0; }
int WebSrvShim::args() { return s_cur ? s_cur->params() : 0; }
String WebSrvShim::argName(int i) {
    return (s_cur && i < s_cur->params()) ? s_cur->getParam(i)->name() : String();
}
String WebSrvShim::uri() { return s_cur ? s_cur->url() : String("/"); }
WebRequestMethod WebSrvShim::method() { return s_cur ? (WebRequestMethod)s_cur->method() : HTTP_GET; }
bool WebSrvShim::hasHeader(const char* name) {
    return s_cur && s_cur->hasHeader(name);
}
String WebSrvShim::header(const char* name) {
    if (!s_cur) return "";
    const AsyncWebHeader* h = s_cur->getHeader(name);
    return h ? h->value() : String();
}
void WebSrvShim::sendHeader(const String& name, const String& value, bool) {
    s_qHeaders += name + ":" + value + "\n";
}
void WebSrvShim::send(int code, const char* ctype, const String& content) {
    if (!s_cur) return;
    AsyncWebServerResponse* res = s_cur->beginResponse(code, ctype, content);
    int start = 0;
    while (start < (int)s_qHeaders.length()) {
        int nl = s_qHeaders.indexOf('\n', start);
        if (nl < 0) break;
        int colon = s_qHeaders.indexOf(':', start);
        if (colon > start) res->addHeader(s_qHeaders.substring(start, colon),
                                         s_qHeaders.substring(colon + 1, nl));
        start = nl + 1;
    }
    s_cur->send(res);
}
void WebSrvShim::streamFile(File& f, const char* type) {
    if (!s_cur) return;
    // Our streamed files are small text (scripts, portal HTML, UI files).
    // Buffer them; async FS streaming of File objects is fiddly across cores.
    String out; out.reserve(f.size() + 1);
    while (f.available()) {
        char buf[1024];
        int n = f.read((uint8_t*)buf, sizeof(buf));
        if (n <= 0) break;
        out.concat(buf, n);
        if (out.length() > 512 * 1024) break;   // hard cap: 512KB
    }
    send(200, type, out);
}
void WebSrvShim::sendFSFile(fs::FS& fs, const String& path, const char* type) {
    if (!s_cur) return;
    // beginResponse(FS,...) + manual send so queued headers (no-cache) apply
    AsyncWebServerResponse* res = s_cur->beginResponse(fs, path, type);
    if (!res) { send(500, (const char*)"text/plain", "file open failed"); return; }
    int start = 0;
    while (start < (int)s_qHeaders.length()) {
        int nl = s_qHeaders.indexOf('\n', start);
        if (nl < 0) break;
        int colon = s_qHeaders.indexOf(':', start);
        if (colon > start) res->addHeader(s_qHeaders.substring(start, colon),
                                         s_qHeaders.substring(colon + 1, nl));
        start = nl + 1;
    }
    s_cur->send(res);
}


namespace web {


struct AutostartEntry { char name[64]; };   // ordered list of scripts to run on USB plug-in

// Async stack: the shim object + the real async server + WS endpoint.

// ---- WebSocket event bus: push JSON events to every connected client ----
// Wire-in points: status changes (script/attack state), log lines. The UI
// subscribes once and replaces most polling.
} // namespace web (shim plumbing ends; wsEvent is intentionally global)

// THREAD SAFETY: AsyncWebSocket is NOT safe to call from arbitrary tasks
// (logLine fires from ducky/sniffer/MCP/loop contexts). Field crash: calling
// textAll directly from another task corrupted the client list -> hang ->
// watchdog reboot -> RAM session token gone -> "logged out everywhere".
// Fix: events go through a FreeRTOS queue, drained to textAll by the loop
// task in web::handle() - the same task that owns the async server.
static QueueHandle_t s_wsQueue = nullptr;
#define WS_QUEUE_LEN 24
void wsEvent(const String& event, const String& payload) {
    if (!s_wsQueue) return;                 // web stack down: drop silently
    String* j = new String("{\"e\":\"" + event + "\",\"d\":" + payload + "}");
    if (xQueueSend(s_wsQueue, &j, 0) != pdTRUE) delete j;   // full: drop (log flood guard)
}
void wsPushStatus() {
    if (ws.count() == 0) return;
    extern String buildStatusJson();        // defined below with hStatus
    wsEvent("status", buildStatusJson());
}

namespace web {

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

// shared status JSON: REST endpoint AND WebSocket push use the same builder
} // namespace web (buildStatusJson is global: shared with ws push)

String buildStatusJson() {
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
         ",\"ip\":\"" + web::localIP() + "\"" +
         ",\"netConnected\":" + String(WiFi.status()==WL_CONNECTED ? "true":"false") +
         ",\"netSsid\":\"" + String(WiFi.SSID()) + "\"" +
         ",\"netIp\":\"" + (WiFi.status()==WL_CONNECTED ? WiFi.localIP().toString() : String("")) + "\"" +
         ",\"scriptState\":\"" + ducky::stateString() + "\"" +
         ",\"scriptName\":\"" + g_state.lastScriptName + "\"" +
         ",\"detectedOS\":\"" + g_state.detectedOS + "\"" +
         ",\"lockKeys\":\"" + detectos::lockState() + "\"" +
         ",\"scriptSince\":" + String((uint32_t)g_state.scriptStateSince) +
         ",\"fw\":\"" FW_VERSION "\",\"fwName\":\"" FW_NAME "\"" +
         ",\"safeMode\":" + String(g_state.safeMode ? "true":"false") +
         ",\"mcpEnabled\":" + String(mcp::enabled() ? "true":"false") +
         ",\"mcpCalls\":" + String(mcp::totalCalls()) +
         ",\"mcpAiConnected\":" + String(mcp::everInitialized() &&
             mcp::lastInitAgoMs() < 300000 ? "true":"false") +
         "}";
    return s;
}

namespace web {

static void hStatus() {
    requireAuth(); if (!isAuthed()) return;
    json(200, buildStatusJson());
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
// ---- script metadata sidecar (description + keyboard layout) ----
static String metaPath(const String& name) { return "/scripts/" + name + ".meta"; }
static String readMeta(const String& name) {
    String j;
    if (!decryptFromFile(metaPath(name).c_str(), j)) return "{}";
    return j;
}
static void writeMeta(const String& name, const String& desc, const String& layout) {
    if (!desc.length() && !layout.length()) { SD_MMC.remove(metaPath(name).c_str()); return; }
    String j = "{\"desc\":\"" + desc + "\",\"layout\":\"" + layout + "\"}";
    encryptToFile(metaPath(name).c_str(), j);
}

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
            // Hide .meta sidecars (desc+layout metadata, not user scripts)
            String fn = String(f.name());
            if (!f.isDirectory() && fn.endsWith(".meta")) { f.close(); continue; }
            if (!f.isDirectory()) {
                if (!first) out += ",";
                first = false;
                String meta = readMeta(String(f.name()));
                String desc;
                extractJsonStr(meta, "desc", desc);
                if (desc.length() > 60) desc = desc.substring(0, 60);
                out += "{\"name\":\"" + String(f.name()) + "\",\"size\":" + String(f.size()) +
                       ",\"desc\":\"" + desc + "\"}";
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
    // meta sidecar (description + layout) dies with its script
    if (ok) SD_MMC.remove(metaPath(name).c_str());
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
// Returns false if the SD write failed (surfaced to the UI).
// NOTE: an EMPTY list must DELETE the file - encryptToFile refuses zero-length
// plaintext, and leaving the stale file behind made removed scripts reappear
// on every reload.
static bool saveAutostart() {
    if (s_autostartCount == 0) {
        SD_MMC.remove("/autostart.enc");
        return true;
    }
    String t;
    for (int i = 0; i < s_autostartCount; i++) t += String(s_autostart[i].name) + "\n";
    return encryptToFile("/autostart.enc", t);
}

static void runScriptTask(void* pv) {
    auto* p = (std::pair<String,String>*)pv;   // <text,name>
    ducky::run(p->first, p->second);
    delete p;
    vTaskDelete(nullptr);
}

static void startRun(const String& text, const String& name) {
    // Per-script keyboard layout (meta sidecar), applied before typing starts.
    String meta = readMeta(name);
    String layout;
    extractJsonStr(meta, "layout", layout);
    if (layout.length()) ducky::setLayout(layout);
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
        if (s_autostartCount >= 5) break;   // product limit: 5 autostart slots
        strlcpy(s_autostart[s_autostartCount++].name, sanitizeName(n).c_str(), 64);
    }
    bool ok = true;   // track SD write result so failures aren't silent
    if (!hw::sdMount()) ok = false;
    else ok = saveAutostart();
    logLine("web: autostart set to " + String(s_autostartCount) + " script(s)" +
            (ok ? "" : " (SD WRITE FAILED)"));
    json(ok ? 200 : 500,
        String("{\"ok\":") + (ok?"true":"false") + ",\"count\":" + s_autostartCount + "}");
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
        String fn = String(f.name());
        // sidecars + internal files stay out of the browser
        if (fn.endsWith(".meta") || fn == "autostart.enc" ||
            fn == "system.log.enc" || fn == "disk.img" || fn == "portal.html.enc") {
            f.close(); continue;
        }
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
    // Decrypt by CONTENT, not filename: if the file carries our "PCE1"
    // envelope it gets decrypted transparently; anything else streams raw.
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return jsonErr(404, "not found");
    uint8_t magic[4] = {0};
    f.read(magic, 4);
    bool encrypted = (magic[0]=='P' && magic[1]=='C' && magic[2]=='E' && magic[3]=='1');
    f.close();
    if (encrypted) {
        String txt;
        if (!decryptFromFile(path.c_str(), txt))
            return jsonErr(500, "decrypt failed (wrong encryption password?)");
        server.sendHeader("Cache-Control", "no-cache");
        return server.send(200, "text/plain", txt);
    }
    f = SD_MMC.open(path, FILE_READ);
    server.streamFile(f, "application/octet-stream");
    f.close();
}

static void hFileSave() {
    requireAuth(); if (!isAuthed()) return;
    String body = server.arg("plain"), path, content;
    if (!extractJsonStr(body, "path", path) || !extractJsonStr(body, "content", content))
        return jsonErr(400, "bad request");
    // Keep the on-disk format consistent with what's already there: files
    // with our PCE1 envelope (or .ds scripts) get written back encrypted.
    bool wasEncrypted = false;
    File chk = SD_MMC.open(path, FILE_READ);
    if (chk) {
        uint8_t m[4] = {0}; chk.read(m, 4); chk.close();
        wasEncrypted = (m[0]=='P' && m[1]=='C' && m[2]=='E' && m[3]=='1');
    }
    bool ok;
    content = normalizeEol(content);
    if (wasEncrypted || path.endsWith(".ds")) {
        ok = encryptToFile(path.c_str(), content);
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

// ---- EvilAP / captive portal --------------------------------------------------
static void hEvilStart() {
    requireAuth(); if (!isAuthed()) return;
    String body = server.arg("plain"), ssid, tpl;
    if (!extractJsonStr(body, "ssid", ssid)) return jsonErr(400, "ssid required");
    extractJsonStr(body, "template", tpl);
    if (!evilap::start(ssid, tpl))
        return jsonErr(500, "failed to start");
    json(200, "{\"ok\":true,\"warn\":\"management AP replaced until stopped\"}");
}
static void hEvilStop() {
    requireAuth(); if (!isAuthed()) return;
    evilap::stop();
    json(200, "{\"ok\":true}");
}
static void hEvilCreds() {
    requireAuth(); if (!isAuthed()) return;
    String txt;
    if (!decryptFromFile("/logs/creds.enc", txt)) txt = "";  // none yet
    server.sendHeader("Cache-Control", "no-cache");
    server.send(200, "text/plain", txt);
}

static void hEvilStatus() {
    requireAuth(); if (!isAuthed()) return;
    auto st = evilap::stats();
    json(200, String("{\"running\":") + (evilap::running()?"true":"false") +
              ",\"hits\":" + st.hits + ",\"captures\":" + st.captures + "}");
}
static void hEvilHtmlGet() {
    requireAuth(); if (!isAuthed()) return;
    String html;
    if (decryptFromFile("/portal.html.enc", html) && html.length())
        return server.send(200, "text/html", html);
    File f = SD_MMC.open("/portal.html", FILE_READ);
    if (f) { server.streamFile(f, "text/html"); f.close(); return; }
    server.send(200, "text/plain", "");   // none yet - editor shows empty
}
static void hEvilHtmlSet() {
    requireAuth(); if (!isAuthed()) return;
    String body, content;
    if (!extractJsonStr(body=server.arg("plain"), "content", content)) return jsonErr(400,"bad");
    // store encrypted so the page can't be read straight off the card
    bool ok = encryptToFile("/portal.html.enc", normalizeEol(content));
    json(ok?200:500, String("{\"ok\":")+ (ok?"true":"false") +"}");
}

// ---- MSC / thumbdrive modes ---------------------------------------------------
static void hMscGet() {
    requireAuth(); if (!isAuthed()) return;
    json(200, String("{\"thumb\":") + (int)msc::thumbMode() +
              ",\"storage\":" + (msc::storageEnabled()?"true":"false") + "}");
}
static void hMscSet() {
    requireAuth(); if (!isAuthed()) return;
    long t = extractJsonNum(server.arg("plain"), "thumb", -1);
    if (t >= 0 && t <= 2) msc::setThumbMode((msc::ThumbMode)t);
    bool st = server.arg("plain").indexOf("\"storage\":true") >= 0;
    bool st2 = server.arg("plain").indexOf("\"storage\":false") >= 0;
    if (st || st2) msc::setStorageEnabled(st);
    logLine("web: msc settings updated");
    json(200, "{\"ok\":true,\"note\":\"applies on next boot/plug-in\"}");
}

// ---- System config: power mode / MAC spoof / tunnel (Step 4) ------------------
static void hSysGet() {
    requireAuth(); if (!isAuthed()) return;
    Preferences p; p.begin("mac", true);
    uint8_t macMode = p.getUChar("mode", 0);
    String macCustom = p.getString("custom", "");
    p.end();
    Preferences t; t.begin("tunnel", true);
    String tok = t.getString("token", "");
    t.end();
    if (tok.length() < 4) {
        // First visit: generate a random token (still user-editable).
        uint8_t rnd[8]; esp_fill_random(rnd, 8);
        tok = "";
        for (uint8_t b : rnd) { char hx[3]; snprintf(hx, 3, "%02x", b); tok += hx; }
        Preferences tw; tw.begin("tunnel", false); tw.putString("token", tok); tw.end();
    }
    Preferences t2; t2.begin("tunnel", true);
    String j = String("{\"powerMode\":") + (int)power::mode() +
      ",\"macMode\":" + macMode +
      ",\"macCustom\":\"" + macCustom + "\"" +
      ",\"tunnelEnabled\":" + (t.getBool("on", false)?"true":"false") +
      ",\"tunnelUrl\":\"" + t.getString("url", "") + "\"" +
      ",\"tunnelToken\":\"" + tok + "\"}";
    t.end();
    json(200, j);
}
static void hSysSet() {
    requireAuth(); if (!isAuthed()) return;
    String body = server.arg("plain"), v;

    long pm = extractJsonNum(body, "powerMode", -1);
    if (pm >= 0 && pm <= 2) { power::set((power::Mode)pm); power::apply(); }

    long mm = extractJsonNum(body, "macMode", -1);
    if (mm >= 0 && mm <= 2) {
        Preferences p; p.begin("mac", false); p.putUChar("mode", (uint8_t)mm); p.end();
    }
    if (extractJsonStr(body, "macCustom", v)) {
        Preferences p; p.begin("mac", false); p.putString("custom", v); p.end();
    }
    if (body.indexOf("\"mcpEnabled\":true") >= 0)   mcp::setEnabled(true);
    if (body.indexOf("\"mcpEnabled\":false") >= 0)  mcp::setEnabled(false);
    Preferences t; t.begin("tunnel", false);
    if (extractJsonStr(body, "tunnelToken", v) && v.length()) t.putString("token", v);
    if (body.indexOf("\"tunnelEnabled\":true") >= 0)  t.putBool("on", true);
    if (body.indexOf("\"tunnelEnabled\":false") >= 0) t.putBool("on", false);
    if (extractJsonStr(body, "tunnelUrl", v))   t.putString("url", v);
    if (extractJsonStr(body, "tunnelToken", v)) t.putString("token", v);
    t.end();
    logLine("web: system config updated");
    json(200, "{\"ok\":true,\"note\":\"MAC changes apply at next boot\"}");
}

// ---- EULA acceptance (one-time gate on first login) ---------------------------
static void hEulaGet() {
    Preferences p; p.begin("pcfg", true);
    bool agreed = p.getBool("eulaOk", false);
    p.end();
    json(200, String("{\"agreed\":") + (agreed?"true":"false") + "}");
}
static void hEulaSet() {
    requireAuth(); if (!isAuthed()) return;
    if (server.arg("plain").indexOf("\"agreed\":true") < 0)
        return jsonErr(400, "must agree");
    Preferences p; p.begin("pcfg", false);
    p.putBool("eulaOk", true);
    p.end();
    logLine("web: EULA accepted");
    json(200, "{\"ok\":true}");
}

// ---- Host recon (ARP sweep + port scan) ----
static void hArpSweep() {
    requireAuth(); if (!isAuthed()) return;
    if (WiFi.status() != WL_CONNECTED) return jsonErr(409, "not connected to a network");
    if (hostrecon::scanning()) return jsonErr(409, "scan in progress");
    auto hosts = hostrecon::arpSweep();
    String out = "[";
    for (size_t i = 0; i < hosts.size(); i++) {
        if (i) out += ",";
        out += "{\"ip\":\"" + hosts[i].ip + "\",\"mac\":\"" + hosts[i].mac + "\"}";
    }
    json(200, out + "]");
}
static const uint16_t TOP_PORTS[] = {80,443,22,445,3389,8080,8000,8443,21,23,25,53,
    110,143,993,995,1433,3306,5432,5900,6379,27017,9100,161,1900};
static void hPortScan() {
    requireAuth(); if (!isAuthed()) return;
    if (WiFi.status() != WL_CONNECTED) return jsonErr(409, "not connected");
    if (hostrecon::scanning()) return jsonErr(409, "scan in progress");
    String body = server.arg("plain"), ip;
    if (!extractJsonStr(body, "ip", ip)) return jsonErr(400, "ip required");
    // optional custom port list "22,80,443" - default top ports
    std::vector<uint16_t> ports;
    String plist;
    if (extractJsonStr(body, "ports", plist) && plist.length()) {
        int start = 0;
        while (start <= (int)plist.length()) {
            int c = plist.indexOf(',', start);
            String p = (c<0)?plist.substring(start):plist.substring(start,c);
            p.trim();
            long v = p.toInt();
            if (v > 0 && v < 65536) ports.push_back((uint16_t)v);
            if (c < 0) break;
            start = c + 1;
        }
    } else {
        for (uint16_t p : TOP_PORTS) ports.push_back(p);
    }
    auto open = hostrecon::portScan(ip, ports);
    String out = "{\"ip\":\"" + ip + "\",\"open\":[";
    for (size_t i = 0; i < open.size(); i++) {
        if (i) out += ",";
        out += String(open[i]);
    }
    json(200, out + "]}");
}

// ---- Karma attack ----
static void hKarmaStart() {
    requireAuth(); if (!isAuthed()) return;
    if (evilap::running()) return jsonErr(409, "portal already running");
    evilap::karmaStart();
    json(200, "{\"ok\":true,\"note\":\"sniffing probe requests\"}");
}
static void hKarmaProbes() {
    requireAuth(); if (!isAuthed()) return;
    auto list = evilap::karmaProbeList();
    String out = "[";
    for (size_t i = 0; i < list.size(); i++) {
        if (i) out += ",";
        String ssid = list[i].first; ssid.replace("\"","'");
        out += "{\"ssid\":\"" + ssid + "\",\"count\":" + String(list[i].second) + "}";
    }
    json(200, out + "]");
}
static void hKarmaSpawn() {
    requireAuth(); if (!isAuthed()) return;
    String body = server.arg("plain"), ssid;
    if (!extractJsonStr(body, "ssid", ssid) || !ssid.length())
        return jsonErr(400, "ssid required");
    if (!evilap::karmaSpawn(ssid)) return jsonErr(500, "cannot spawn portal");
    json(200, "{\"ok\":true,\"warn\":\"device offline while portal runs\"}");
}

// ---- WiFi attack (deauth + pcap, Step 3b) --------------------------------------
// During an attack the device AP goes DOWN (channel conflict) - the UI warns.
// Attacks are "attempt once": bounded time, then normal operation restores.
static void hDeauthStart() {
    requireAuth(); if (!isAuthed()) return;
    String body = server.arg("plain"), ssid;
    long secs = extractJsonNum(body, "seconds", 30);
    long method = extractJsonNum(body, "method", 0);
    if (!extractJsonStr(body, "ssid", ssid) || !ssid.length())
        return jsonErr(400, "ssid required");
    if (wifiattack::attacking()) return jsonErr(409, "attack already running");
    logLine("web: DEAUTH requested for '" + ssid + "' method " + String(method));
    if (!wifiattack::startDeauth(ssid, constrain((long)secs, 5, 300), (uint8_t)constrain((long)method,0,4)))
        return jsonErr(500, "cannot start (busy or script running)");
    server.send(200, "application/json", "{\"ok\":true,\"warn\":\"device goes offline during attack\"}");
}
static void hDeauthStop() {
    requireAuth(); if (!isAuthed()) return;
    wifiattack::stop();
    json(200, "{\"ok\":true}");
}
static void hDeauthStatus() {
    requireAuth(); if (!isAuthed()) return;
    auto st = wifiattack::stats();
    json(200, String("{\"attacking\":") + (wifiattack::attacking()?"true":"false") +
              ",\"deauths\":" + st.deauths + ",\"drops\":" + st.deauthDrops +
              ",\"stations\":" + st.stations +
              ",\"eapol\":" + st.eapol + "}");
}
static void hPcapStart() {
    requireAuth(); if (!isAuthed()) return;
    String body = server.arg("plain"), name;
    long secs = extractJsonNum(body, "seconds", 30);
    long ch = extractJsonNum(body, "channel", 1);
    if (!extractJsonStr(body, "name", name)) name = "cap";
    if (wifiattack::sniffing()) return jsonErr(409, "capture already running");
    if (!wifiattack::startPcap(name, (uint8_t)constrain((long)ch,1,13), constrain((long)secs,5,600)))
        return jsonErr(500, "cannot start");
    server.send(200, "application/json", "{\"ok\":true,\"warn\":\"device goes offline during capture\"}");
}
static void hPcapList() {
    requireAuth(); if (!isAuthed()) return;
    String out = "[";
    File dir = SD_MMC.open("/pcap");
    if (dir && dir.isDirectory()) {
        File f; bool first = true;
        while ((f = dir.openNextFile())) {
            if (!first) out += ",";
            first = false;
            out += "{\"name\":\"" + String(f.name()) + "\",\"size\":" + String(f.size()) + "}";
            f.close();
        }
    }
    json(200, out + "]");
}

// ---- Script scheduler (cron-lite) ---------------------------------------------
static void hSchedGet() {
    requireAuth(); if (!isAuthed()) return;
    json(200, scheduler::statusJson());
}
static void hSchedAdd() {
    requireAuth(); if (!isAuthed()) return;
    String body = server.arg("plain"), script;
    long interval = extractJsonNum(body, "intervalMin", 0);
    long at = extractJsonNum(body, "at", 0);
    if (!extractJsonStr(body, "script", script) || !script.length())
        return jsonErr(400, "script required");
    if (interval <= 0 && at <= 0) return jsonErr(400, "need intervalMin or at");
    scheduler::Entry e;
    e.script = sanitizeName(script);
    e.intervalMin = (interval > 0) ? (uint32_t)interval : 0;
    e.atEpoch = (uint32_t)at;
    e.lastRunEpoch = 0;
    scheduler::add(e);
    logLine("scheduler: + " + e.script);
    json(200, scheduler::statusJson());
}
static void hSchedDelete() {
    requireAuth(); if (!isAuthed()) return;
    long idx = extractJsonNum(server.arg("plain"), "index", -1);
    if (idx < 0 || !scheduler::remove((size_t)idx)) return jsonErr(400, "bad index");
    json(200, scheduler::statusJson());
}
static void hSchedClear() {
    requireAuth(); if (!isAuthed()) return;
    scheduler::clearAll();
    json(200, "[]");
}

// ---- WiFi scanner (recon + diagnostics) ---------------------------------------
static void hWifiScan() {
    requireAuth(); if (!isAuthed()) return;
    // Scan in AP+STA so our own management AP stays up during the scan.
    WiFi.mode(WIFI_AP_STA);
    int n = WiFi.scanNetworks();
    if (n < 0) return jsonErr(500, "scan failed");
    String out = "[";
    for (int i = 0; i < n; i++) {
        if (i) out += ",";
        String ssid = WiFi.SSID(i);
        ssid.replace("\"","'");   // keep JSON valid
        bool hidden = ssid.length()==0;
        uint8_t* b = WiFi.BSSID(i);
        char bmac[18]; snprintf(bmac, sizeof(bmac), "%02X:%02X:%02X:%02X:%02X:%02X",
            b[0],b[1],b[2],b[3],b[4],b[5]);
        out += "{\"ssid\":\"" + (hidden?"(hidden)":ssid) + "\"" +
               ",\"bssid\":\"" + bmac + "\"" +
               ",\"rssi\":" + String(WiFi.RSSI(i)) +
               ",\"ch\":" + String(WiFi.channel(i)) +
               ",\"secure\":" + String(WiFi.encryptionType(i)!=WIFI_AUTH_OPEN?"true":"false") +
               ",\"hidden\":" + String(hidden?"true":"false") + "}";
    }
    WiFi.scanDelete();
    json(200, out + "]");
}

// ---- Script metadata (description + keyboard layout) --------------------------
static void hScriptMetaGet() {
    requireAuth(); if (!isAuthed()) return;
    String name = sanitizeName(server.arg("name"));
    String meta = readMeta(name);
    String desc="0", layout;   // desc default empty
    desc = "";
    extractJsonStr(meta, "desc", desc);
    extractJsonStr(meta, "layout", layout);
    if (!layout.length()) layout = "en_US";
    json(200, "{\"desc\":\"" + desc + "\",\"layout\":\"" + layout + "\"}");
}
static void hScriptMetaSet() {
    requireAuth(); if (!isAuthed()) return;
    String body = server.arg("plain"), name, desc, layout;
    if (!extractJsonStr(body, "name", name)) return jsonErr(400, "name required");
    extractJsonStr(body, "desc", desc);
    if (desc.length() > 60) desc = desc.substring(0, 60);   // hard limit
    extractJsonStr(body, "layout", layout);
    name = sanitizeName(name);
    writeMeta(name, desc, layout);
    json(200, "{\"ok\":true}");
}
static void hLayouts() {
    requireAuth(); if (!isAuthed()) return;
    auto names = ducky::layoutNames();
    String out = "[";
    for (size_t i = 0; i < names.size(); i++) {
        if (i) out += ",";
        out += "\"" + names[i] + "\"";
    }
    json(200, out + "]");
}

// ---- Self destruct -----------------------------------------------------------
static void hSelfDestruct() {
    requireAuth(); if (!isAuthed()) return;
    if (server.arg("confirm") != "DESTROY") return jsonErr(400, "confirm=DESTROY required");
    json(200, "{\"ok\":true}");
    delay(500);              // let the response flush before we die
    sys::selfDestruct();
}

// ---- HID control (Control Page) ---------------------------------------------
static void hHidKey() {
    requireAuth(); if (!isAuthed()) return;
    String body = server.arg("plain"), key, typ;
    if (!extractJsonStr(body, "key", key)) return jsonErr(400, "bad request");
    extractJsonStr(body, "type", typ);
    // "tap" (default) = press + release. Only explicit "up"/"down" hold keys,
    // otherwise the host auto-repeats forever (stuck-key behavior).
    typ.toLowerCase();
    if (typ == "up")          { ducky::hidKey(key, false); }
    else if (typ == "down")   { ducky::hidKey(key, true); }
    else                      { ducky::hidKey(key, true); delay(15); ducky::hidKey(key, false); }
    json(200, "{\"ok\":true}");
}
static void hHidMods() {
    requireAuth(); if (!isAuthed()) return;
    std::vector<String> mods;
    if (!extractJsonArr(server.arg("plain"), "mods", mods)) return jsonErr(400, "bad request");
    // Toggle model: UI sends the FULL desired set; we diff against last set.
    static std::vector<String> s_held;
    for (auto& m : s_held) {
        bool stillWanted = false;
        for (auto& n : mods) if (n.equalsIgnoreCase(m)) { stillWanted = true; break; }
        if (!stillWanted) ducky::hidModifier(m, false);   // released
    }
    for (auto& m : mods) {
        bool already = false;
        for (auto& h : s_held) if (h.equalsIgnoreCase(m)) { already = true; break; }
        if (!already && ducky::isModifierName(m)) { ducky::hidModifier(m, true); }
    }
    s_held = mods;
    json(200, "{\"ok\":true}");
}
static void hHidMouse() {
    requireAuth(); if (!isAuthed()) return;
    String body = server.arg("plain");
    if (body.indexOf("\"button\"") >= 0) {
        String b; extractJsonStr(body, "button", b);
        bool down = body.indexOf("\"down\":true") >= 0;
        ducky::hidMouseButton(b, down);
        return json(200, "{\"ok\":true}");
    }
    if (body.indexOf("\"scroll\"") >= 0) {
        ducky::hidMouseScroll((int)extractJsonNum(body, "scroll", 0));
        return json(200, "{\"ok\":true}");
    }
    int dx = (int)extractJsonNum(body, "dx", 0), dy = (int)extractJsonNum(body, "dy", 0);
    ducky::hidMouseMove(dx, dy);
    json(200, "{\"ok\":true}");
}

// ---- USB identity spoofing -------------------------------------------------
static void hSpoofGet() {
    requireAuth(); if (!isAuthed()) return;
    char hex[12];
    String presets = "[";
    for (size_t i = 0; i < spoof::PRESET_COUNT; i++) {
        snprintf(hex, sizeof(hex), "%04X", spoof::PRESETS[i].vid);
        char hex2[8]; snprintf(hex2, sizeof(hex2), "%04X", spoof::PRESETS[i].pid);
        if (i) presets += ",";
        presets += "{\"vid\":\"" + String(hex) + "\",\"pid\":\"" + hex2 +
                   "\",\"vendor\":\"" + String(spoof::PRESETS[i].vendor) +
                   "\",\"product\":\"" + String(spoof::PRESETS[i].product) + "\"}";
    }
    presets += "]";
    snprintf(hex, sizeof(hex), "%04X", spoof::vid());
    char hex2[8]; snprintf(hex2, sizeof(hex2), "%04X", spoof::pid());
    json(200, "{\"vid\":\"" + String(hex) + "\",\"pid\":\"" + hex2 +
              "\",\"vendor\":\"" + spoof::vendor() +
              "\",\"product\":\"" + spoof::product() +
              "\",\"serial\":\"" + spoof::serial() +
              "\",\"presets\":" + presets + "}");
}

static void hSpoofSet() {
    requireAuth(); if (!isAuthed()) return;
    String body = server.arg("plain"), v;
    // randomPerBoot toggles the identity mode (persists separately)
    if (body.indexOf("\"randomPerBoot\":true") >= 0 ||
        body.indexOf("\"randomPerBoot\":false") >= 0) {
        bool rp = body.indexOf("\"randomPerBoot\":true") >= 0;
        Preferences p; p.begin("spoof", false);
        p.putBool("randBoot", rp);
        p.end();
        if (rp) spoof::randomize();   // immediate effect this boot
        logLine(String("web: identity random-per-boot ") + (rp?"ON":"OFF"));
        return json(200, "{\"ok\":true}");
    }
    if (body.indexOf("\"randomize\":true") >= 0) {
        spoof::randomize();
    } else {
        uint16_t vid = 0, pid = 0;
        long vidIn = extractJsonNum(body, "vid", -1);
        long pidIn = extractJsonNum(body, "pid", -1);
        if (vidIn < 0 || pidIn < 0) {
            // accept hex strings too ("1E7D")
            if (extractJsonStr(body, "vid", v))  vid = (uint16_t)strtol(v.c_str(), nullptr, 16);
            if (extractJsonStr(body, "pid", v))  pid = (uint16_t)strtol(v.c_str(), nullptr, 16);
        } else { vid = (uint16_t)vidIn; pid = (uint16_t)pidIn; }
        if (!vid || !pid) return jsonErr(400, "invalid VID/PID");
        String vendor, product, serial;
        extractJsonStr(body, "vendor", vendor);
        extractJsonStr(body, "product", product);
        extractJsonStr(body, "serial", serial);   // empty -> new random serial
        spoof::set(vid, pid, vendor, product, serial);
    }
    spoof::save();
    logLine("web: identity spoofed -> " + spoof::vendor() + " " + spoof::product());
    json(200, "{\"ok\":true,\"note\":\"applies on next boot/plug-in\"}");
}

// ---- hardware test endpoints (also used by Step-2 SCREEN_/LED_ commands) ---
static void hDevLed() {
    requireAuth(); if (!isAuthed()) return;
    // /api/dev/led?r=255&g=0&b=0   /api/dev/led?off=1
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

// Binary upload: POST /api/filebin?path=/x  body {"b64":"<base64>"}
// The old multipart streaming handler crashed the device; small files are
// fine to buffer whole and this path is deterministic.
static void hFileBin() {
    requireAuth(); if (!isAuthed()) return;
    String path = server.arg("path");
    if (!path.length()) return jsonErr(400, "?path=");
    String b64;
    if (!extractJsonStr(server.arg("plain"), "b64", b64)) return jsonErr(400, "bad request");

    // mbedtls base64 decode (needs padding-aware length calc)
    size_t outLen = (b64.length() / 4) * 3 + 3;
    std::vector<uint8_t> bin(outLen);
    size_t actual = 0;
    if (mbedtls_base64_decode(bin.data(), outLen, &actual,
                              (const uint8_t*)b64.c_str(), b64.length()) != 0)
        return jsonErr(400, "bad base64");
    bin.resize(actual);

    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) return jsonErr(500, "cannot open for write");
    size_t w = f.write(bin.data(), bin.size());
    f.close();
    logLine("web: uploaded " + path + " (" + String(bin.size()) + "B)");
    json(w == bin.size() ? 200 : 500, String("{\"ok\":") + (w==bin.size()) + "}");
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
    if (body.indexOf("\"wifiHidden\":true") >= 0)   cfg.wifiHidden = true;
    if (body.indexOf("\"wifiHidden\":false") >= 0)  cfg.wifiHidden = false;
    if (extractJsonStr(body, "user", v) && v.length()) strlcpy(cfg.webUser, v.c_str(), sizeof(cfg.webUser));
    if (extractJsonStr(body, "hostname", v) && v.length()) strlcpy(cfg.hostname, v.c_str(), sizeof(cfg.hostname));
    if (extractJsonStr(body, "webPass", v) && v.length()) strlcpy(cfg.webPass, v.c_str(), sizeof(cfg.webPass));
    if (extractJsonStr(body, "encPassword", v) && v.length()) strlcpy(cfg.encPassword, v.c_str(), sizeof(cfg.encPassword));

    configSaveWiFi(); configSaveLogin(); configSaveEncryption();

    // display group - brightness is an UNQUOTED JSON number, so it needs
    // extractJsonNum (extractJsonStr requires quotes and silently failed,
    // leaving brightness stuck at its default).
    cfg.screenBrightness = constrain((int)extractJsonNum(body, "brightness", (long)cfg.screenBrightness), 0, 255);
    // interface flags come as booleans - hand-rolled detection:
    if (body.indexOf("\"screenOnBoot\":true") >= 0)  cfg.screenOnBoot = true;
    if (body.indexOf("\"screenOnBoot\":false") >= 0) cfg.screenOnBoot = false;
    if (body.indexOf("\"ledOnBoot\":true") >= 0)     cfg.ledOnBoot = true;
    if (body.indexOf("\"ledOnBoot\":false") >= 0)    cfg.ledOnBoot = false;
    if (body.indexOf("\"autoDetectOS\":true") >= 0)  cfg.autoDetectOS = true;
    if (body.indexOf("\"autoDetectOS\":false") >= 0) cfg.autoDetectOS = false;
    if (body.indexOf("\"tempOff\":true") >= 0)       cfg.ifaceTempOff = true;      // takes effect after reboot
    if (body.indexOf("\"tempOff\":false") >= 0)      cfg.ifaceTempOff = false;
    if (body.indexOf("\"permOff\":true") >= 0)       cfg.ifaceDisabledPerm = true; // WARNING: irreversible without reflash
    configSaveDisplay(); configSaveInterfaceFlags(); configSaveAutoOS();
    hw::applyBrightness();   // brightness slider takes effect immediately

    logLine("web: settings updated");
    json(200, "{\"ok\":true}");
}

// GET current settings so the Settings form reflects saved state.
// Secrets are NOT returned (boxes stay empty = unchanged, matching save).
static void hSettingsGet() {
    requireAuth(); if (!isAuthed()) return;
    String s = "{\"ssid\":\"" + String(cfg.wifiSSID) + "\"" +
        ",\"user\":\"" + String(cfg.webUser) + "\"" +
        ",\"hostname\":\"" + String(cfg.hostname) + "\"" +
        ",\"screenOnBoot\":" + String(cfg.screenOnBoot ? "true" : "false") +
        ",\"ledOnBoot\":" + String(cfg.ledOnBoot ? "true" : "false") +
        ",\"brightness\":" + String(cfg.screenBrightness) +
        ",\"autoDetectOS\":" + String(cfg.autoDetectOS ? "true" : "false") +
        ",\"wifiHidden\":" + String(cfg.wifiHidden ? "true" : "false") +
        ",\"mcpEnabled\":" + String(mcp::enabled() ? "true" : "false") +
        ",\"mcpToken\":\"" + mcp::token() + "\"" +
        ",\"tempOff\":" + String(cfg.ifaceTempOff ? "true" : "false") +
        ",\"permOff\":" + String(cfg.ifaceDisabledPerm ? "true" : "false") +
        "}";
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    json(200, s);
}

// ---- static UI -------------------------------------------------------------------
// NOTE: files live under /www inside LittleFS (data/www -> image root keeps
// folder), so browser paths must be mapped to /www/<path>.
static void serveWWW(const char* browserPath) {
    String fsPath = String("/www") + browserPath;
    if (!LittleFS.exists(fsPath)) {
        server.send(500, "text/plain", "UI missing - run 'pio run -t uploadfs'");
        return;
    }
    // No-cache: browsers otherwise heuristically cache app.js/style.css and
    // keep serving a stale UI after uploadfs updates.
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    // Chunked FS streaming - constant RAM. Buffering app.js (77KB) into a
    // String + the response's own copy exhausted heap and silently produced
    // empty bodies (field bug 192.168.12.209).
    const char* type = String(browserPath).endsWith(".css") ? "text/css" :
                       String(browserPath).endsWith(".js") ? "application/javascript" : "text/html";
    server.sendFSFile(LittleFS, fsPath, type);
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

static void hMcpTokenGet() {
    requireAuth(); if (!isAuthed()) return;
    json(200, "{\"token\":\"" + mcp::token() + "\"}");
}
static void hMcpTokenSet() {
    requireAuth(); if (!isAuthed()) return;
    String body = server.arg("plain"), t;
    extractJsonStr(body, "token", t);
    if (t.length() < 8) return jsonErr(400, "token too short (min 8)");
    mcp::setToken(t);
    logLine("web: MCP token updated");
    json(200, "{\"ok\":true}");
}
void setupRoutes();    // defined below begin(): registers all API routes

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
    // Hostname: helps users find the device without knowing the IP; also
    // registers <hostname>.local via mDNS on any network the device joins.
    WiFi.setHostname(cfg.hostname);
    WiFi.softAP(cfg.wifiSSID, strlen(cfg.wifiPass) >= 8 ? cfg.wifiPass : "dongle1234",
                0, cfg.wifiHidden ? 1 : 0);
    // mDNS on AP-only: the responder must be told about the softAP interface
    // explicitly, otherwise .local never resolves (STA-only default).
    MDNS.begin(cfg.hostname);
    MDNS.setInstanceName(cfg.hostname);
    MDNS.enableWorkstation(ESP_IF_WIFI_AP);   // announce on the AP interface too
    MDNS.addService("http", "tcp", 80);
    logLine(String("mDNS: http://") + cfg.hostname + ".local");

    // Cookie (web auth) + X-MCP-Token (MCP auth): the async server collects
    // ALL headers by default, so no collectHeaders() equivalent is needed.

    // WebSocket endpoint first (claims /ws), then handlers register
    // themselves via server.on(...) below, then notFound -> static UI.
    s_wsQueue = xQueueCreate(WS_QUEUE_LEN, sizeof(String*));
    asrv.addHandler(&ws);
    ws.onEvent([](AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type,
                 void*, uint8_t*, size_t) {
        if (type == WS_EVT_CONNECT) {
            // Do NOT build/send the status here: this callback runs in the
            // async_tcp task and buildStatusJson touches SD stats, racing
            // logLine's SD writes. Flag it; web::handle() pushes next pass.
            s_wsPushOnConnect = true;
        }
    });
    setupRoutes();
    asrv.begin();
    s_running = true;
    logLine(String("web: AP \"") + cfg.wifiSSID + "\" up at " + localIP() + " (async+ws)");
    return true;
}

// Periodic status push while clients are connected. Called from loop() via
// handle() - cheap: one small JSON string only when ws clients exist.
static uint32_t s_lastPush = 0;
static volatile bool s_wsPushOnConnect = false;
void handle() {
    if (!s_running) return;
    ws.cleanupClients();
    if (s_wsPushOnConnect) {
        s_wsPushOnConnect = false;
        if (ws.count()) wsPushStatus();   // safe: wsEvent now queues
    }
    // drain the event queue (loop task = the async server's task)
    if (s_wsQueue) {
        String* j = nullptr;
        while (xQueueReceive(s_wsQueue, &j, 0) == pdTRUE) {
            if (j) {
                if (ws.count()) ws.textAll(*j);
                delete j;
            }
        }
    }
    if (ws.count() && millis() - s_lastPush > 2000) {
        s_lastPush = millis();
        wsPushStatus();
    }
}

// EvilAP needs port 80; two servers can't share it. Suspend = close socket,
// resume = re-bind (clients just refresh; WS clients auto-reconnect).
void suspend() { if (s_running) asrv.end(); }
void resume()  { if (s_running) asrv.begin(); }

String localIP() { return WiFi.softAPIP().toString(); }

// MCP glue: mcp.cpp needs the shim object to register its route.
WebSrvShim* webServerPtr() { return &server; }

// route table (still inside namespace web)
void setupRoutes() {
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
    server.on("/api/evilap/start", HTTP_POST, hEvilStart);
    server.on("/api/evilap/stop", HTTP_POST, hEvilStop);
    server.on("/api/evilap/status", HTTP_GET, hEvilStatus);
    server.on("/api/evilap/creds", HTTP_GET, hEvilCreds);
    server.on("/api/evilap/html", HTTP_GET, hEvilHtmlGet);
    server.on("/api/evilap/html", HTTP_POST, hEvilHtmlSet);
    server.on("/api/msc", HTTP_GET, hMscGet);
    server.on("/api/msc", HTTP_POST, hMscSet);
    server.on("/api/scriptmeta", HTTP_GET, hScriptMetaGet);
    server.on("/api/scriptmeta", HTTP_POST, hScriptMetaSet);
    server.on("/api/layouts", HTTP_GET, hLayouts);
    server.on("/api/mcptoken", HTTP_GET, hMcpTokenGet);
    server.on("/api/mcptoken", HTTP_POST, hMcpTokenSet);
    mcp::begin();
    server.on("/api/deauth/start", HTTP_POST, hDeauthStart);
    server.on("/api/deauth/stop", HTTP_POST, hDeauthStop);
    server.on("/api/deauth/status", HTTP_GET, hDeauthStatus);
    server.on("/api/pcap/start", HTTP_POST, hPcapStart);
    server.on("/api/recon/arp", HTTP_POST, hArpSweep);
    server.on("/api/recon/ports", HTTP_POST, hPortScan);
    server.on("/api/karma/start", HTTP_POST, hKarmaStart);
    server.on("/api/karma/stop", HTTP_POST, [](void){
        requireAuth(); if (!isAuthed()) return;
        evilap::karmaStop();
        json(200, "{\"ok\":true}");
    });
    server.on("/api/karma/probes", HTTP_GET, hKarmaProbes);
    server.on("/api/karma/spawn", HTTP_POST, hKarmaSpawn);
    server.on("/api/sched", HTTP_GET, hSchedGet);
    server.on("/api/sched", HTTP_GET, hSchedGet);
    server.on("/api/sched", HTTP_POST, hSchedAdd);
    server.on("/api/sched", HTTP_DELETE, hSchedDelete);
    server.on("/api/sched/clear", HTTP_POST, hSchedClear);
    server.on("/api/analyzer/start", HTTP_POST, [](void){
        requireAuth(); if (!isAuthed()) return;
        wifiattack::analyzerStart();
        json(200, "{\"ok\":true,\"warn\":\"management AP down while analyzing\"}");
    });
    server.on("/api/analyzer/stop", HTTP_POST, [](void){
        requireAuth(); if (!isAuthed()) return;
        wifiattack::analyzerStop();
        json(200, "{\"ok\":true}");
    });
    server.on("/api/analyzer/live", HTTP_GET, [](){
        requireAuth(); if (!isAuthed()) return;
        auto st = wifiattack::liveStats();
        auto aps = wifiattack::liveAps();
        String out = String("{\"mgmt\":") + st.mgmt + ",\"data\":" + st.data +
            ",\"ctrl\":" + st.ctrl + ",\"total\":" + st.total +
            ",\"bytes\":" + st.bytes + ",\"channel\":" + st.channel + ",\"aps\":[";
        for (size_t i = 0; i < aps.size(); i++) {
            if (i) out += ",";
            String ssid(aps[i].ssid); ssid.replace("\"","'");
            out += "{\"ssid\":\"" + ssid + "\",\"bssid\":\"" + aps[i].bssid +
                   "\",\"channel\":" + String(aps[i].channel) +
                   ",\"secure\":" + String(aps[i].secure?"true":"false") +
                   ",\"rssi\":" + String(aps[i].rssi) + "}";
        }
        json(200, out + "]}");
    });
    server.on("/api/wifiscan", HTTP_GET, hWifiScan);
    server.on("/api/sys", HTTP_GET, hSysGet);
    server.on("/api/sys", HTTP_POST, hSysSet);
    server.on("/api/eula", HTTP_GET, hEulaGet);
    server.on("/api/eula", HTTP_POST, hEulaSet);
    server.on("/api/selfdestruct", HTTP_POST, hSelfDestruct);
    server.on("/api/hid/key", HTTP_POST, hHidKey);
    server.on("/api/hid/mods", HTTP_POST, hHidMods);
    server.on("/api/hid/mouse", HTTP_POST, hHidMouse);
    server.on("/api/spoof", HTTP_GET, hSpoofGet);
    server.on("/api/spoof", HTTP_POST, hSpoofSet);
    server.on("/api/dev/led", HTTP_GET, hDevLed);       // hardware test
    server.on("/api/mkdir", HTTP_POST, hMkdir);
    server.on("/api/filebin", HTTP_POST, hFileBin);   // binary upload (base64)
    server.on("/api/dev/screen", HTTP_GET, hDevScreen); // hardware test
    server.on("/api/settings", HTTP_POST, hSettings);
    server.on("/api/settings", HTTP_GET, hSettingsGet);   // form loads saved values
    server.on("/", HTTP_GET, hIndex);
    server.on("/index.html", HTTP_GET, hIndex);
    server.onNotFound(hStatic);
}

} // namespace web (main: handlers + lifecycle)
