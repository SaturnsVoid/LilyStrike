// ============================================================================
// main.cpp - ProjectCodename firmware entry point (Step 1)
// ----------------------------------------------------------------------------
// Boot flow:
//   1. Load NVS settings (factory defaults on first boot)
//   2. Init hardware: APA102 LED off, TFT (off by default), SD card
//   3. Start WiFi AP + web interface (unless disabled via Settings)
//   4. Loop: serve web, watch BOOT button (re-enables temp-disabled UI),
//      detect USB host plug-in to fire autostart script chain.
//
// Autostart: /autostart.enc on SD holds an ordered list of script names.
// When a host computer is detected, each runs to completion in order.
// ============================================================================
#include <Arduino.h>
#include <WiFi.h>
#include "config.h"
#include "hw.h"
#include "pins.h"
#include "ducky.h"
#include "webserver.h"
#include "crypt.h"
#include <USB.h>
#include "detect_os.h"
#include "tusb.h"   // tud_connected(): true once a host configures the device
#include <SD_MMC.h>

static bool s_lastUsb = false;
static TaskHandle_t s_chainTask = nullptr;

// Runs every autostart script in order inside one dedicated task so delays
// inside scripts never block the web UI. If autoDetectOS is enabled the
// detection runs FIRST here so autostart scripts can rely on its result.
struct ChainCtx { std::vector<String> names; bool detect; };
static void plugInTask(void* pv) {
    auto* ctx = (ChainCtx*)pv;
    if (ctx->detect && !ducky::isRunning()) {
        logLine("plug-in: auto-running DETECT_OS");
        HostOS h = detectos::detect();
        g_state.detectedOS = detectos::nameOf(h);
        logLine("plug-in: detected " + g_state.detectedOS);
    }
    logLine("autostart: running " + String(ctx->names.size()) + " script(s)");
    for (auto& n : ctx->names) {
        String text;
        decryptFromFile(("/scripts/" + n).c_str(), text);
        if (text.length()) ducky::run(text, n);
        else logLine("autostart: cannot read/decrypt " + n);
        if (!ducky::isRunning()) break;   // stopped via web UI -> abort chain
    }
    delete ctx;
    s_chainTask = nullptr;
    vTaskDelete(nullptr);
}

void setup() {
    Serial.begin(115200);
    configLoad();
    logLine("boot: ProjectCodename starting");

    // USB HID up first so scripts can run immediately after plug-in.
    ducky::initOnce();

    if (!hw::initAll()) {
        // SD missing is non-fatal but note it on screen briefly
        hw::screenText("SD CARD ERROR");
    }

    // USB HID device presence detection:
    // TinyUSB reports "connected" when a host enumerates/configures the CDC -
    // good enough to tell "plugged into computer" from "powerbank".
    g_state.usbHostPresent = tud_connected();

    web::begin();

    if (cfg.ledOnBoot) { RGB p = {0x80, 0x00, 0xC0}; hw::ledSet(p); }   // purple
}

void loop() {
    web::handle();
    delay(2);

    // ---- BOOT button -------------------------------------------------------
    // If the interface was temporarily disabled, holding BOOT re-enables it
    // until next reboot. Otherwise a short press toggles the screen.
    static uint32_t btnDownAt = 0;
    if (digitalRead(PIN_BTN_BOOT) == LOW) {
        if (!btnDownAt) btnDownAt = millis();
        else if (millis() - btnDownAt > 1500 && cfg.ifaceTempOff) {
            cfg.ifaceTempOff = false;              // re-enable just this boot
            configSaveInterfaceFlags();
            web::begin();
            logLine("btn: interface re-enabled");
            btnDownAt = 0xFFFFFFFF - 2000;         // don't retrigger
        }
    } else if (btnDownAt && btnDownAt != 0xFFFFFFF5 && millis() - btnDownAt > 50) {
        // short press: toggle screen backlight (quick stealth control)
        static bool scrOn = false;
        scrOn = !scrOn;
        if (scrOn) hw::screenOn(); else hw::screenOff();
        btnDownAt = 0;
    } else if (digitalRead(PIN_BTN_BOOT) == HIGH) btnDownAt = 0;

    // ---- USB host detection / autostart chain -------------------------------
    bool nowUsb = tud_connected();
    g_state.usbHostPresent = nowUsb;
    // Unplug: clear the cached OS result so stale detections never persist
    // across hosts (a new computer may be a different OS).
    if (!nowUsb && s_lastUsb) g_state.detectedOS = "Unknown";

    if (nowUsb && !s_lastUsb && s_chainTask == nullptr && !ducky::isRunning()) {
        // Host just plugged in: auto-detect OS if enabled, then run the
        // /autostart.enc queue (one script name per line, PCE1 envelope).
        String t;
        if (decryptFromFile("/autostart.enc", t) && t.length()) {
            auto* ctx = new ChainCtx();
            int start = 0;
            while (start < (int)t.length()) {
                int nl = t.indexOf('\n', start);
                String name = t.substring(start, nl < 0 ? t.length() : nl);
                name.trim();
                if (name.length()) ctx->names.push_back(name);
                if (nl < 0) break;
                start = nl + 1;
            }
            ctx->detect = cfg.autoDetectOS;
            if (ctx->names.size() || ctx->detect)
                xTaskCreatePinnedToCore(plugInTask, "plugin", 8192, ctx, 1, &s_chainTask, 0);
            else delete ctx;
        } else if (cfg.autoDetectOS) {
            // No autostart scripts but detection enabled -> still detect.
            auto* ctx = new ChainCtx{{}, true};
            xTaskCreatePinnedToCore(plugInTask, "plugin", 8192, ctx, 1, &s_chainTask, 0);
        }
    }
    s_lastUsb = nowUsb;
}
