// ============================================================================
// blehid.h - BLE radio subsystem: HID (keyboard/mouse/media), advertisement
// scanner + tracker detector, and popup-spam attack (see blehid.cpp)
// ----------------------------------------------------------------------------
// Everything here is ON-DEMAND: the NimBLE host only runs while a BLE feature
// is active and is de-initialized afterwards (frees ~35KB heap back).
// ============================================================================
#pragma once
#include <Arduino.h>

namespace blehid {

// ---- lifecycle ----
bool begin(bool wifiOff = false, int stage = 5);  // stage 1-5 self-test bisect (5=full)
void deinit();           // stop advertising + free the host
bool ready();            // host initialized?
bool connected();        // a BLE host (keyboard consumer) is paired+connected?

// ---- HID primitives (dispatch target chosen by ducky's g_hidTarget) ----
void kbWriteChar(char c, const uint8_t* layout);  // layout-aware ASCII typing
void kbPress(uint8_t k);                          // raw keycode (same codes as USB)
void kbRelease(uint8_t k);
void kbReleaseAll();
void mouseButtons(uint8_t b);                     // bit0 L, bit1 R, bit2 M
void mouseMove(int8_t x, int8_t y, int8_t wheel = 0);
void media(uint16_t usage);                       // consumer page (see MEDIA_* codes)

// ---- scanner (BLE devices + tracker classification) ----
struct BleDev {
    String mac, name, kind;    // kind: "" | "Apple FindMy?" | "Samsung" | "Tile?"
    int rssi;
};
bool scanStart(uint32_t seconds, bool wifiOff = false, int stage = 5);  // passive+active scan, background task
bool scanBusy();
uint32_t scanProgress();                // %
std::vector<BleDev> scanResults();      // snapshot (sorted by RSSI)

// ---- popup spam (dual-use; AUTHORIZED USE ONLY) ----
// mode: 0=all, 1=apple, 2=windows, 3=samsung
bool spamStart(uint32_t seconds, uint8_t mode);
void spamStop();
bool spamBusy();

} // namespace blehid

// Media usage codes (USB HID Consumer Control page)
#define MEDIA_PLAY    0x00CD
#define MEDIA_PAUSE   0x00B1
#define MEDIA_NEXT    0x00B5
#define MEDIA_PREV    0x00B6
#define MEDIA_VOLUP   0x00E9
#define MEDIA_VOLDOWN 0x00EA
#define MEDIA_MUTE    0x00E2
