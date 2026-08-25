// ============================================================================
// msc.h - USB Mass Storage modes (False Thumbdrive + USB_STORAGE, Step 3)
// ----------------------------------------------------------------------------
// TinyUSB MSC exposes the SD card as a mass-storage device. Two features use
// it:
//   * USB_STORAGE script command / setting - card mounts alongside HID so a
//     script (or you) can move files to/from the host.
//   * False Thumbdrive stealth mode - device boots as an innocent read-only
//     USB stick. No WiFi, no screen, no LED. Put disk.zip (or anything) on
//     the card and that's what the victim sees.
//
// IMPORTANT TinyUSB constraint: interfaces are baked into the USB descriptor
// at enumeration, so enabling/disabling MSC requires a re-plug (we emulate
// one with usb_persist_restart where supported). ESCAPE HATCH: holding BOOT
// during boot skips thumbdrive mode for that session so you can't lock
// yourself out.
// ============================================================================
#pragma once
#include <Arduino.h>

namespace msc {

enum class ThumbMode : uint8_t { OFF = 0, FIRST_LOAD = 1, SECOND_LOAD = 2 };

void loadSettings();                 // NVS -> internal state
bool saveSettings();                 // persist

ThumbMode thumbMode();
void setThumbMode(ThumbMode m);

bool storageEnabled();               // USB_STORAGE flag (HID+drive combo)
void setStorageEnabled(bool on);

// Called EARLY in setup(): returns true if this boot should run in stealth
// thumbdrive mode (no WiFi/UI/HID). Handles SECOND_LOAD boot counting and
// the hold-BOOT-to-bypass escape hatch.
bool shouldBootAsThumbdrive();

// Configure + begin MSC against the SD card. Call BEFORE ducky::initOnce()
// (i.e. before USB.begin()). readOnly=true blocks all writes.
void beginCard(bool readOnly);

} // namespace msc
