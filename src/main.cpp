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
#include "tusb.h"   // tud_connected(): true once a host configures the device
#include <SD_MMC.h>

static bool s_lastUsb = false;
static TaskHandle_t s_chainTask = nullptr;

// Runs every autostart script in order inside one dedicated task so delays
// inside scripts never block the web UI.
struct ChainCtx { std::vector<String> names; };
static void chainTask(void* pv) {
    auto* ctx = (ChainCtx*)pv;
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
    if (nowUsb && !s_lastUsb && s_chainTask == nullptr && !ducky::isRunning()) {
        // host just plugged in -> run autostart queue if any.
        // /autostart.enc holds one script name per line, same envelope as web API.
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
            if (ctx->names.size())
                xTaskCreatePinnedToCore(chainTask, "chain", 8192, ctx, 1, &s_chainTask, 0);
            else delete ctx;
        }
    }
    s_lastUsb = nowUsb;
}
