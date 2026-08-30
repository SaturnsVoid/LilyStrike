// ============================================================================
// websrv_shim.h - WebServer-compatible facade over ESPAsyncWebServer
// ----------------------------------------------------------------------------
// WHY: ~68 route handlers in webserver.cpp + mcp.cpp were written against the
// synchronous ESP32 WebServer API (server.arg("plain"), server.send(...),
// server.sendHeader(...), server.header("Cookie"), ...). Rewriting every
// handler for the async callback model was high-risk. Instead this shim keeps
// the ORIGINAL handler code untouched: it captures the currently-dispatched
// AsyncWebServerRequest in a module pointer and serves the same method surface.
//
// Design notes:
// - Only ONE request is dispatched at a time in practice (handlers are short,
//   synchronous). The "current request" pointer is set by the router in
//   webserver.cpp right before calling the handler.
// - Raw JSON bodies arrive via the async body callback; the router stores
//   them per-request and arg("plain") serves them (mirrors ESP8266-style
//   WebServer behavior that our handlers expect).
// - sendHeader() queues headers; send() applies them to the response
//   (AsyncWebServer only allows headers before the response is begun).
// - EvilAP keeps its OWN separate synchronous WebServer on port 80 (evilap.cpp)
//   - deliberately untouched; it only runs while the main server is suspended.
// ============================================================================
#pragma once
#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <FS.h>

class WebSrvShim {
public:
    typedef void (*THandlerFunction)();
    explicit WebSrvShim(uint16_t port) : _srv(port) {}

    // ---- registration (routed through the async server by the shim impl) ----
    void on(const char* path, WebRequestMethodComposite method, THandlerFunction fn);
    void onNotFound(THandlerFunction fn);

    // ---- request surface used by handlers ----
    String arg(const char* key);            // "plain" = raw body; else query param
    String arg(const String& key) { return arg(key.c_str()); }
    bool hasArg(const char* key);
    int args();                             // count of query params
    String argName(int i);
    String uri();
    WebRequestMethod method();

    bool hasHeader(const char* name);
    String header(const char* name);

    void sendHeader(const String& name, const String& value, bool first = false);
    void send(int code, const char* content_type = "text/plain", const String& content = String(""));
    void streamFile(File& f, const char* type);   // small files -> buffered send
    // Chunked FS streaming (constant RAM) - REQUIRED for large files like
    // app.js (77KB): buffering a String + beginResponse copies it, and the
    // double ~77KB heap alloc silently failed -> 200/500 with EMPTY body.
    void sendFSFile(fs::FS& fs, const String& path, const char* type);

    // ---- lifecycle (also shimmed: no handleClient needed) ----
    void begin() { _srv.begin(); }
    void stop()  { _srv.end();  }
    void handleClient() {}   // async server needs no polling
    // Escape hatch for the WS endpoint + event wiring in webserver.cpp
    AsyncWebServer& raw() { return _srv; }
private:
    AsyncWebServer _srv;
};
