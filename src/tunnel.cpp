// ============================================================================
// tunnel.cpp - External access relay client (see tunnel.h)
// ----------------------------------------------------------------------------
// Protocol with tools/relay_server.py:
//   device: GET  {url}/pull        header X-Token: <token>
//           -> 204 (nothing) or 200 JSON {method,path,body(base64|null)}
//   device: POST {url}/push        JSON {status, body(b64)}  (same token)
// The relay keeps the request/response pairing; owner's browser talks to
// /t/{token}/... on the relay and gets the device's responses transparently.
// ============================================================================
#include "tunnel.h"
#include "config.h"
#include "crypt.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include "util.h"
#include <mbedtls/base64.h>
#include <string>
#include <Preferences.h>

namespace tunnel {

static volatile bool s_running = false;
static bool s_enabled = false;
static String s_url, s_token;

bool enabled() { return s_enabled; }
bool running() { return s_running; }

void loadPrefs() {
    Preferences p; p.begin("tunnel", true);
    s_enabled = p.getBool("on", false);
    s_url = p.getString("url", "");
    s_token = p.getString("token", "");
    p.end();
}

void setEnabled(bool on) {
    s_enabled = on;
    Preferences p; p.begin("tunnel", false);
    p.putBool("on", on);
    p.end();
    logLine(String("tunnel ") + (on ? "enabled" : "disabled"));
}

// Arduino String lacks resize(); use std::string scratch buffers.
static String b64encode(const String& in) {
    size_t olen = ((in.length() + 2) / 3) * 4 + 4;
    std::string out(olen, 0);
    if (mbedtls_base64_encode((uint8_t*)out.data(), olen, &olen,
        (const uint8_t*)in.c_str(), in.length()) != 0) return "";
    return String(out.c_str(), olen);
}
static String b64decode(const String& in) {
    size_t olen = in.length() / 4 * 3 + 3;
    std::string out(olen, 0);
    if (mbedtls_base64_decode((uint8_t*)out.data(), olen, &olen,
        (const uint8_t*)in.c_str(), in.length()) != 0) return "";
    return String(out.c_str(), olen);
}

// Perform a request against our own local web UI and return the response
// as JSON for the relay. Cookie headers pass through so web login works.
static String doLocalRequest(const String& method, const String& path,
                             const String& bodyB64) {
    HTTPClient http;
    String url = "http://127.0.0.1" + path;
    http.begin(url);
    int code;
    if (method == "POST") {
        String body = b64decode(bodyB64);
        http.addHeader("Content-Type", "application/json");
        code = http.POST(body);
    } else {
        code = http.GET();
    }
    String resp = http.getString();

    // Collect Set-Cookie + Content-Type so logins work through the tunnel.
    String hdrs = "{";
    const char* keys[] = {"Set-Cookie", "Content-Type", "Location"};
    bool first = true;
    for (auto k : keys) {
        String v = http.header(k);
        if (v.length()) {
            if (!first) hdrs += ",";
            first = false;
            v.replace("\"", "'");
            hdrs += "\"" + String(k) + "\":\"" + v + "\"";
        }
    }
    hdrs += "}";
    http.end();

    String b64 = b64encode(resp);
    b64.replace("\n","");
    String j = "{\"status\":" + String(code) + ",\"headers\":" + hdrs +
               ",\"body\":\"" + b64 + "\"}";
    return j;
}

static void tunnelTask(void*) {
    logLine("tunnel: task started");
    while (s_enabled && WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        http.begin(s_url + "/pull");
        http.addHeader("X-Token", s_token);
        http.setTimeout(8000);
        int code = http.GET();
        if (code == 200) {
            String job = http.getString();
            http.end();
            // crude JSON field pulls (device only ever talks to OUR relay)
            String method, path, body;
            extractJsonStr(job, "method", method);
            extractJsonStr(job, "path", path);
            extractJsonStr(job, "body", body);
            if (path.length()) {
                String resp = doLocalRequest(method, path, body);
                HTTPClient push;
                push.begin(s_url + "/push");
                push.addHeader("X-Token", s_token);
                push.addHeader("Content-Type", "application/json");
                push.POST(resp);
                push.end();
            }
        } else {
            http.end();
            delay(1000);
        }
        delay(250);
    }
    s_running = false;
    logLine("tunnel: task stopped");
    vTaskDelete(nullptr);
}

void loadAndMaybeStart() {
    loadPrefs();
    if (s_running || !s_enabled || s_url.length() < 8 || s_token.length() < 4) return;
    if (WiFi.status() != WL_CONNECTED) return;
    s_running = true;
    xTaskCreatePinnedToCore(tunnelTask, "tunnel", 12288, nullptr, 1, nullptr, 0);
}

} // namespace tunnel
