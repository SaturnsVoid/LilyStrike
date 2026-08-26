// ============================================================================
// msc.cpp - TinyUSB MSC over SD_MMC (see msc.h)
// ----------------------------------------------------------------------------
// LUN geometry comes straight from the card: 512-byte sectors, sector count
// from SD_MMC.cardSize(). Read-only mode simply returns 0 from onWrite.
// ============================================================================
#include "msc.h"
#include "config.h"
#include "hw.h"
#include <USB.h>
#include <USBMSC.h>
#include <SD_MMC.h>
#include <esp32-hal-tinyusb.h>
#include <Preferences.h>
#include "config.h"

namespace msc {

static ThumbMode s_thumb = ThumbMode::OFF;
static bool s_storage = false;
static USBMSC msc;
static bool s_ro = true;   // static mirror: lambdas cannot capture

void loadSettings() {
    Preferences p; p.begin("msc", true);
    s_thumb   = (ThumbMode)p.getUChar("thumb", 0);
    s_storage = p.getBool("storage", false);
    p.end();
}
bool saveSettings() {
    Preferences p; p.begin("msc", false);
    p.putUChar("thumb", (uint8_t)s_thumb);
    p.putBool("storage", s_storage);
    p.end();
    return true;
}

ThumbMode thumbMode()            { return s_thumb; }
void setThumbMode(ThumbMode m)   { s_thumb = m; saveSettings(); }
bool storageEnabled()            { return s_storage; }
void setStorageEnabled(bool on)  { s_storage = on; saveSettings(); }

bool shouldBootAsThumbdrive() {
    // SECRET RECOVERY TOKEN: /UNLOCK.TXT on the card = one normal boot +
    // factory reset. Checked BEFORE stealth so the owner can always get in.
    if (hw::sdMount()) {
        // Case-insensitive: Windows drops files as UNLOCK.txt, owners type
        // it however they like. FatFs name storage isn't reliably normalized
        // through the Arduino wrapper, so probe common casings.
        const char* tokenNames[] = {"/UNLOCK.TXT","/UNLOCK.txt","/unlock.txt",
                                    "/Unlock.txt","/unlock.TXT"};
        String found = "";
        for (auto n : tokenNames)
            if (SD_MMC.exists(n)) { found = n; break; }
        if (found.length()) {
            Preferences p; p.begin("msc", false);
            p.putUChar("thumb", 0);          // back to Off
            p.putULong("boots2", 0);         // reset second-load counter
            p.end();
            SD_MMC.remove(found);
            logLine("MSC: unlock token found - stealth disabled");
            return false;
        }
    }

    if (s_thumb == ThumbMode::FIRST_LOAD) return true;

    if (s_thumb == ThumbMode::SECOND_LOAD) {
        // First-ever boot runs normally (so you can configure it); every boot
        // after that is a thumbdrive. Counter lives in NVS.
        Preferences p; p.begin("msc", false);
        uint32_t boots = p.getULong("boots2", 0) + 1;
        p.putULong("boots2", boots);
        p.end();
        return boots >= 2;
    }
    return false;
}

void beginCard(bool readOnly) {
    if (!hw::sdMount()) {
        logLine("MSC: no SD card - cannot expose drive");
        return;
    }
    const uint32_t LBA = 512;
    uint64_t bytes = SD_MMC.cardSize();
    uint32_t sectors = (uint32_t)(bytes / LBA);

    // NOTE: do NOT call vendorID/productID here! Those strings share the
    // device-wide descriptor pool and would CLOBBER the spoofed identity set
    // by spoof::applyToUsb(). Only per-LUN settings belong here.
    msc.productRevision("1.0");
    // Lambdas can't capture; readOnly goes through a static mirror.
    s_ro = readOnly;
    msc.onRead([](uint32_t lba, uint32_t offset, void* buf, uint32_t sz) -> int32_t {
        // CRITICAL: readRAW returns a bool - the MSC callback must report
        // BYTES copied. Returning the bool (1/0) made every 512-byte sector
        // look like a 1-byte read and Windows refused to mount the drive.
        return SD_MMC.readRAW((uint8_t*)buf, lba) ? (int32_t)sz : 0;
    });
    msc.onWrite([](uint32_t lba, uint32_t offset, uint8_t* buf, uint32_t sz) -> int32_t {
        if (s_ro) return 0;                                    // swallow writes
        return SD_MMC.writeRAW(buf, lba) ? (int32_t)sz : 0;
    });
    msc.onStartStop([](uint8_t pc, bool start, bool eject) -> bool { return true; });
    msc.isWritable(!readOnly);
    msc.begin(sectors, LBA);
    logLine(String("MSC: card exposed ") + (readOnly ? "READ-ONLY" : "read-write") +
            " (" + String(sectors * LBA / 1048576) + " MB)");
}

} // namespace msc
