// ============================================================================
// websrv_shim.cpp - implementation of the WebServer-compatible facade
// ----------------------------------------------------------------------------
// Extracted from webserver.cpp during the Step-5 reorg. See websrv_shim.h for
// the design rationale: ~68 handlers written against the sync WebServer API
// run unmodified on top of ESPAsyncWebServer via a per-request context.
#include "websrv_shim.h"
#include <map>

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
    _srv.addHandler(h);
}
void WebSrvShim::adoptRequest(AsyncWebServerRequest* r, const String& body) {
    s_cur = r;
    s_bodies[r] = body;
    s_curBody = &s_bodies[r];
    s_qHeaders = "";
}
void WebSrvShim::releaseRequest() {
    if (s_cur) { s_bodies.erase(s_cur); }
    s_cur = nullptr; s_curBody = nullptr;
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
    _srv.addHandler(h);
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
    // belt+braces: API responses must never be cached by the browser
    // (heuristically cached stale /api/* responses caused field confusion)
    res->addHeader("Cache-Control", "no-store");
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



// Queue a response header for the current request; applied by send().
// (kept in the impl: sendHeader/send are a matched pair)
