// ============================================================================
// mcp.cpp - MCP server over Streamable HTTP (see mcp.h)
// ----------------------------------------------------------------------------
// JSON-RPC 2.0 methods handled:
//   initialize                 -> protocol version + capabilities + serverInfo
//   notifications/initialized  -> 202 (no body per spec; we ack silently)
//   tools/list                 -> tool schema array
//   tools/call                 -> dispatch table, compact text results
//   ping                       -> {}
// Everything else -> method-not-found error.
// ============================================================================
#include "mcp.h"
#include "config.h"
#include "version.h"
#include "ducky.h"
#include "hw.h"
#include "detect_os.h"
#include "power.h"
#include "tunnel.h"
#include "crypt.h"
#include <WiFi.h>
#include "websrv_shim.h"
#include <Preferences.h>
#include <esp_system.h>

#include "util.h"
namespace web { WebSrvShim* webServerPtr(); }   // glue in webserver.cpp
using web::webServerPtr;

namespace mcp {

static String s_token;
static bool s_enabled = true;
static uint32_t s_callsTotal = 0;
static uint32_t s_lastInitMs = 0xFFFFFFFF;   // last LLM initialize handshake

String token() {
    if (!s_token.length()) {
        Preferences p; p.begin("mcp", true);
        s_token = p.getString("token", "");
        p.end();
        if (!s_token.length()) {           // first use: generate + persist
            uint8_t rnd[16];
            esp_fill_random(rnd, sizeof(rnd));
            s_token = "";
            for (uint8_t b : rnd) { char hx[3]; snprintf(hx, 3, "%02x", b); s_token += hx; }
            setToken(s_token);
        }
    }
    return s_token;
}
void setToken(const String& t) {
    s_token = t;
    Preferences p; p.begin("mcp", false);
    p.putString("token", t);
    p.end();
}

// ------------------------------------------------------------------ helpers
static void sendJson(int code, const String& j) {
    // NOTE: srv->send(code, "application/json", ...) already emits
    // Content-Type; adding another produces a duplicate header
    // ("application/json, application/json") which strict MCP clients
    // reject with "Unexpected content type".
    WebSrvShim* srv = webServerPtr();
    srv->send(code, "application/json", j);
}
static void jsonError(int code, int rpcCode, const String& msg, const String& id = "null") {
    sendJson(code, "{\"jsonrpc\":\"2.0\",\"id\":" + id +
             ",\"error\":{\"code\":" + String(rpcCode) + ",\"message\":\"" + msg + "\"}}");
}
static void jsonResult(const String& id, const String& result) {
    sendJson(200, "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + result + "}");
}
// tool result wrapper: MCP wants content[] with text blocks
static String toolText(const String& text) {
    String t = text; t.replace("\\", "\\\\"); t.replace("\"", "\\\"");
    t.replace("\n", "\\n"); t.replace("\r", "");
    return "{\"content\":[{\"type\":\"text\",\"text\":\"" + t + "\"}]}";
}
static String argStr(const String& body, const char* key) {
    String v;
    extractJsonStr(body, key, v);
    return v;
}
static long argNum(const String& body, const char* key, long def) {
    return extractJsonNum(body, key, def);
}
// tools/call args live in params.arguments; searching the whole body matches
// the TOOL name first ("run_script") instead of the argument ("light_show.ds").
static String argsBody(const String& body) {
    int i = body.indexOf("\"arguments\"");
    if (i < 0) return "{}";
    int ob = body.indexOf('{', i);
    if (ob < 0) return "{}";
    // find matching close brace (arguments is flat in our schemas)
    int depth = 0;
    for (int p = ob; p < (int)body.length(); p++) {
        if (body[p] == '{') depth++;
        else if (body[p] == '}') { depth--; if (!depth) return body.substring(ob, p+1); }
    }
    return "{}";
}
static String argStrA(const String& body, const char* key) {
    return argStr(argsBody(body), key);
}
static long argNumA(const String& body, const char* key, long def) {
    return extractJsonNum(argsBody(body), key, def);
}

// ---------------------------------------------------------------- tool defs
static const char* TOOL_SCHEMAS = R"MCPTOOLS([
{"name":"device_status","description":"Get device state: RAM, uptime, IPs, WiFi, USB, script state, detected OS","inputSchema":{"type":"object","properties":{}}}
,{"name":"run_script","description":"Start a DuckyScript payload on the host computer. Returns immediately; poll device_status for completion.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"Script name from list_scripts"},"text":{"type":"string","description":"OR inline DuckyScript text"}}}}
,{"name":"stop_script","description":"Abort the currently running script","inputSchema":{"type":"object","properties":{}}}
,{"name":"list_scripts","description":"List saved scripts with descriptions","inputSchema":{"type":"object","properties":{}}}
,{"name":"write_script","description":"Create or update a script (text + optional description and keyboard layout)","inputSchema":{"type":"object","properties":{"name":{"type":"string"},"text":{"type":"string"},"desc":{"type":"string"},"layout":{"type":"string","description":"en_US,de_DE,es_ES,fr_CH,fr_FR,it_IT,pt_PT,pt_BR,sv_SE,da_DK,hu_HU,ja_JP"}},"required":["name","text"]}}
,{"name":"read_script","description":"Read a script's text","inputSchema":{"type":"object","properties":{"name":{"type":"string"}},"required":["name"]}}
,{"name":"keystroke","description":"Tap one key on the host (e.g. 'a','ENTER','GUI r' style combos use run_script instead)","inputSchema":{"type":"object","properties":{"key":{"type":"string"}},"required":["key"]}}
,{"name":"mouse","description":"Move/click/scroll host mouse","inputSchema":{"type":"object","properties":{"dx":{"type":"integer"},"dy":{"type":"integer"},"click":{"type":"string","enum":["left","right","middle","double"]},"scroll":{"type":"integer"}}}}
,{"name":"wifi_scan","description":"Scan nearby WiFi networks","inputSchema":{"type":"object","properties":{}}}
,{"name":"led","description":"Set device LED color (#RRGGBB) or off","inputSchema":{"type":"object","properties":{"color":{"type":"string"},"off":{"type":"boolean"}}}}
,{"name":"screen","description":"Device screen: on/off/clear/text","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["on","off","clear","text"]},"text":{"type":"string"}}}}
,{"name":"system_config","description":"Read/adjust device config: powerMode(0=low,1=normal,2=high), tunnel on/off. MAC+identity NOT settable here.","inputSchema":{"type":"object","properties":{"powerMode":{"type":"integer","minimum":0,"maximum":2},"tunnelOn":{"type":"boolean"}}}}
])MCPTOOLS";

// ---------------------------------------------------------------- dispatch
static String sysSet(const String& body);   // defined below (system_config)
void duckyRunAsync(const String& text, const String& name);
static String toolCall(const String& name, const String& body) {
    const String& A = body;   // argsBody applied below via helpers
    WebSrvShim* srv = webServerPtr();
    if (name == "device_status") {
        // Reuse the status JSON builder logic inline (compact subset)
        String s = "{";
        s += "\"heapKB\":" + String(ESP.getFreeHeap()/1024);   // BUGFIX: "heap":107KB was invalid JSON (unquoted unit)
        s += ",\"uptime_s\":" + String(millis()/1000);
        s += ",\"cpuMhz\":" + String(getCpuFrequencyMhz());
        s += ",\"usbHost\":" + String(g_state.usbHostPresent?"true":"false");
        s += ",\"detectedOS\":\"" + g_state.detectedOS + "\"";
        s += ",\"scriptState\":\"" + ducky::stateString() + "\"";
        s += ",\"scriptName\":\"" + g_state.lastScriptName + "\"";
        s += ",\"apIp\":\"" + WiFi.softAPIP().toString() + "\"";
        s += ",\"stationConnected\":" + String(WiFi.status()==WL_CONNECTED?"true":"false");
        s += ",\"stationIp\":\"" + (WiFi.status()==WL_CONNECTED ? WiFi.localIP().toString() : String("")) + "\"";
        s += "}";
        return toolText(s);
    }
    if (name == "run_script") {
        String sn = argStrA(body, "name"), text = argStrA(body, "text");
        if (!sn.length() && !text.length()) return toolText("error: need name or text");
        if (ducky::isRunning()) return toolText("error: a script is already running; poll device_status");
        if (sn.length() && !text.length()) {
            if (!decryptFromFile(("/scripts/" + sn).c_str(), text) || !text.length())
                return toolText("error: cannot read/decrypt " + sn);
        }
        // reuse webserver's runner via a tiny duplicate (direct ducky call)
        duckyRunAsync(text, sn.length()?sn:String("mcp-inline"));
        return toolText("started. poll device_status for scriptState");
    }
    if (name == "stop_script") { ducky::stop(); return toolText("stop requested"); }
    if (name == "list_scripts") {
        // iterate SD directly (compact - no size fields)
        String out = "[";
        File dir = SD_MMC.open("/scripts");
        if (dir) {
            File f; bool first = true;
            while ((f = dir.openNextFile())) {
                String fn = f.name();
                if (fn.endsWith(".meta")) { f.close(); continue; }
                if (!first) out += ",";
                first = false;
                out += "\"" + fn + "\"";
                f.close();
            }
        }
        return toolText(out + "]");
    }
    if (name == "write_script") {
        String sn = argStrA(body, "name"), text = argStrA(body, "text");
        String desc = argStrA(body, "desc"), layout = argStrA(body, "layout");
        if (!sn.length() || !text.length()) return toolText("error: need name + text");
        if (!sn.endsWith(".ds")) sn += ".ds";
        sn.replace("/", "");
        bool ok = encryptToFile(("/scripts/" + sn).c_str(), text);
        if (ok && (desc.length() || layout.length())) {
            String j = "{\"desc\":\"" + desc + "\",\"layout\":\"" + (layout.length()?layout:String("en_US")) + "\"}";
            encryptToFile(("/scripts/" + sn + ".meta").c_str(), j);
        }
        return toolText(ok ? "saved " + sn : "error: SD write failed");
    }
    if (name == "read_script") {
        String sn = argStrA(body, "name");
        if (!sn.length()) return toolText("error: need name");
        if (!sn.endsWith(".ds")) sn += ".ds";
        String t;
        if (!decryptFromFile(("/scripts/" + sn).c_str(), t)) return toolText("error: not found");
        return toolText(t);
    }
    if (name == "keystroke") {
        String k = argStrA(body, "key");
        if (!k.length()) return toolText("error: need key");
        // single named key or char
        extern void duckyTapKey(const String& key);   // see bottom
        duckyTapKey(k);
        return toolText("tapped " + k);
    }
    if (name == "mouse") {
        long dx = argNumA(body, "dx", 0), dy = argNumA(body, "dy", 0);
        String click = argStrA(body, "click");
        long scroll = argNumA(body, "scroll", 0);
        if (click.length()) { extern void duckyMouseClick(const String& b); duckyMouseClick(click); }
        if (dx || dy) { extern void duckyMouseMove(int, int); duckyMouseMove((int)dx,(int)dy); }
        if (scroll) { extern void duckyMouseScroll(int); duckyMouseScroll((int)scroll); }
        return toolText("ok");
    }
    if (name == "wifi_scan") {
        WiFi.mode(WIFI_AP_STA);
        int n = WiFi.scanNetworks();
        String out = "[";
        for (int i = 0; i < n; i++) {
            if (i) out += ",";
            String ssid = WiFi.SSID(i); ssid.replace("\"","'");
            out += "{\"ssid\":\"" + (ssid.length()?ssid:String("(hidden)")) +
                   "\",\"rssi\":" + String(WiFi.RSSI(i)) +
                   ",\"ch\":" + String(WiFi.channel(i)) +
                   ",\"secure\":" + String(WiFi.encryptionType(i)!=WIFI_AUTH_OPEN?"true":"false") + "}";
        }
        WiFi.scanDelete();
        return toolText(out + "]");
    }
    if (name == "led") {
        if (argStrA(body, "off") == "true" || argNumA(body,"off",0)==1) { hw::ledOff(); return toolText("off"); }
        String c = argStrA(body, "color");
        if (c.length() != 7 || c[0] != '#') return toolText("error: color like #RRGGBB");
        auto nyb=[](char ch){ return (ch>='0'&&ch<='9')?ch-'0':(ch|32)-'a'+10; };
        hw::ledSet({(uint8_t)(nyb(c[1])*16+nyb(c[2])),(uint8_t)(nyb(c[3])*16+nyb(c[4])),(uint8_t)(nyb(c[5])*16+nyb(c[6]))});
        return toolText("set " + c);
    }
    if (name == "screen") {
        String a2 = argStrA(body, "action");
        if (a2=="on") hw::screenOn();
        else if (a2=="off") hw::screenOff();
        else if (a2=="clear") hw::screenClear();
        else if (a2=="text") { hw::screenOn(); hw::screenText(argStrA(body,"text")); }
        else return toolText("error: action on|off|clear|text");
        return toolText(a2);
    }
    if (name == "system_config") {
        return sysSet(body);
    }
    return toolText("error: unknown tool " + name);
}

// ---- glue helpers used by tools (implemented below toolCall) ----
void duckyRunAsync(const String& text, const String& name);
void duckyTapKey(const String& key);
void duckyMouseMove(int dx, int dy);
void duckyMouseClick(const String& b);
void duckyMouseScroll(int n);

// minimal system_config implementation (power + tunnel only)
static String sysSet(const String& body) {
    long pm = argNum(body, "powerMode", -1);
    if (pm >= 0 && pm <= 2) { power::set((power::Mode)pm); power::apply(); }
    if (body.indexOf("\"tunnelOn\":true") >= 0) tunnel::setEnabled(true);
    if (body.indexOf("\"tunnelOn\":false") >= 0) tunnel::setEnabled(false);
    String s = "powerMode=" + String((int)power::mode()) +
               ",tunnel=" + (tunnel::enabled()?"on":"off");
    return toolText(s);
}

// ---------------------------------------------------------------- handler
static void handleMcp() {
    if (!s_enabled) {
        jsonError(403, -32002, "MCP is disabled in device settings");
        return;
    }
    WebSrvShim* srv = webServerPtr();
    // auth: header OR ?token= query param (dumb clients can't set headers)
    bool authed = false;
    if (srv->hasHeader("X-MCP-Token") && srv->header("X-MCP-Token") == token())
        authed = true;
    if (!authed && srv->hasArg("token") && srv->arg("token") == token())
        authed = true;
    if (!authed) {
        jsonError(401, -32001, "bad or missing X-MCP-Token");
        return;
    }
    String body = srv->arg("plain");
    String method, id;
    extractJsonStr(body, "method", method);
    // id can be number or string; grab raw between quotes if string form
    if (!extractJsonStr(body, "id", id)) {
        // numeric id
        int i = body.indexOf("\"id\":");
        if (i >= 0) {
            int s = i + 5, e = s;
            while (e < (int)body.length() && (isdigit(body[e]) || body[e]=='-')) e++;
            id = body.substring(s, e);
        }
    }
    if (!id.length()) id = "null";
    s_callsTotal++;
    logLine("mcp: " + method);

    if (method == "initialize") {
        String r = "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{\"tools\":{}},"
                   "\"serverInfo\":{\"name\":\"" FW_NAME "\",\"version\":\"" FW_VERSION "\"}}";
        jsonResult(id, r);
    } else if (method == "notifications/initialized") {
        srv->send(202, "text/plain", "");
    } else if (method == "tools/list") {
        jsonResult(id, String("{\"tools\":") + TOOL_SCHEMAS + "}");
    } else if (method == "tools/call") {
        String params = body;   // crude: pass whole body, argStr finds keys
        String name = argStr(body, "name");
        // arguments object: we cheat by searching the raw body for arg keys
        jsonResult(id, toolCall(name, body));
    } else if (method == "ping") {
        jsonResult(id, "{}");
    } else {
        jsonError(200, -32601, "method not found: " + method, id);
    }
}

bool enabled() { return s_enabled; }
static void registerRoute();   // fwd: defined below, needed by setEnabled
void setEnabled(bool on) {
    s_enabled = on;
    // BUGFIX: /mcp was only registered in begin() at boot, so enabling MCP
    // at runtime never took effect until reboot. Register on the spot.
    if (on) registerRoute();
    Preferences p; p.begin("mcp", false);
    p.putBool("on", on);
    p.end();
    logLine(String("mcp: ") + (on ? "enabled" : "disabled"));
}
void load() {
    Preferences p; p.begin("mcp", true);
    s_enabled = p.getBool("on", false);   // default OFF - opt-in feature
    p.end();
}
uint32_t totalCalls() { return s_callsTotal; }
bool everInitialized() { return s_lastInitMs != 0xFFFFFFFF; }
uint32_t lastInitAgoMs() { return (s_lastInitMs==0xFFFFFFFF) ? 0xFFFFFFFF : millis()-s_lastInitMs; }

static bool s_routeRegistered = false;
static void registerRoute() {
    if (s_routeRegistered) return;
    WebSrvShim* srv = webServerPtr();
    if (srv) { srv->on("/mcp", HTTP_POST, handleMcp); s_routeRegistered = true; }
}
void begin() {
    load();
    // ALWAYS register the route: the catch-all "/*" handler is added at the
    // end of setupRoutes, so any handler registered later (runtime enable)
    // is shadowed by it and /mcp 404s. Gate on s_enabled per-request instead
    // (see handleMcp) - runtime enable/disable then just works.
    registerRoute();
    if (!s_enabled) logLine("mcp: disabled by setting");
}

// ---- async script runner mirroring webserver.cpp's pattern ----
static void runTask(void* pv) {
    auto* p = (std::pair<String,String>*)pv;
    ducky::run(p->first, p->second);
    delete p;
    vTaskDelete(nullptr);
}
void duckyRunAsync(const String& text, const String& name) {
    auto* p = new std::pair<String,String>(text, name);
    xTaskCreatePinnedToCore(runTask, "mcpducky", 8192, p, 1, nullptr, 0);
}
void duckyTapKey(const String& key) {
    // reuse ducky's HID helpers
    ducky::hidKey(key, true); delay(15); ducky::hidKey(key, false);
}
void duckyMouseMove(int dx, int dy)   { ducky::hidMouseMove(dx, dy); }
void duckyMouseClick(const String& b) {
    // small press/release cycle via the button helper
    ducky::hidMouseButton(b, true); delay(25); ducky::hidMouseButton(b, false);
}
void duckyMouseScroll(int n)          { ducky::hidMouseScroll(n); }

} // namespace mcp
