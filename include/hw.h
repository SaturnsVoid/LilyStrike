// ============================================================================
// hw.h - Hardware abstraction: SD, TFT, APA102 LED, BOOT button
// ============================================================================
#pragma once
#include <Arduino.h>
#include <SD_MMC.h>
#include <Adafruit_ST7735.h>

struct RGB { uint8_t r, g, b; };

namespace hw {

// Bring up SD (4-bit SD_MMC), TFT, LED, button. Safe to call once in setup().
bool initAll();

// ---- LED (APA102) ---------------------------------------------------------
void ledSet(const RGB& c);          // set + show
void ledOff();

// ---- Screen ---------------------------------------------------------------
void screenOn();                    // backlight on
void screenOff();
void screenClear();                 // fill black
void screenText(const String& t);   // simple status text line(s)
extern Adafruit_ST7735* tft;        // raw access for future steps

// ---- Button ---------------------------------------------------------------
// Call periodically from loop(); returns true on a fresh press.
bool buttonPressed();
// Blocking wait with timeout (ms). Returns false if timed out. Used by
// WAIT_BUTTON custom command (step 2) and temp-UI re-enable.
bool buttonWait(uint32_t timeoutMs);

// ---- SD helpers -----------------------------------------------------------
bool sdMount();                     // true if /sd is usable
uint64_t sdUsedBytes();
uint64_t sdTotalBytes();
bool sdWipe();          // delete everything on SD, recreate folder structure
} // namespace hw
