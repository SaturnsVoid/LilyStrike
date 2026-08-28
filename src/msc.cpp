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
#include <sdmmc_cmd.h>

// Reaches the protected sdmmc_card_t* inside SDMMCFS by subclassing with an
// IDENTICAL layout (adds methods only) - same technique USBArmyKnife uses.
// This lets the MSC callbacks use sdmmc_read/write_sectors: the native
// multi-sector driver, far more reliable than per-sector SD_MMC.readRAW.
namespace fs {
  class SDMMCFS2 : public SDMMCFS {
  public:
    sdmmc_card_t* getCard() { return _card; }
  };
}
static fs::SDMMCFS2* cardFS() { return (fs::SDMMCFS2*)&SD_MMC; }
#include "config.h"

namespace msc {

static ThumbMode s_thumb = ThumbMode::OFF;
static bool s_storage = false;
static bool s_active = false;    // card currently exposed raw to host
static USBMSC msc;

bool active() { return s_active; }
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
    // NOTE: this runs BEFORE hw::initAll(), so the SD may not be mounted yet.
    // Mount it here independently or the token check silently never fires.
    if (!hw::sdMount()) {
        SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN,
                       SD_D0_PIN, SD_D1_PIN, SD_D2_PIN, SD_D3_PIN);
        if (!SD_MMC.begin("/sdcard", true)) logLine("MSC token check: no SD");
    }
    if (SD_MMC.cardType() != CARD_NONE) {
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

// ---------------------------------------------------------------------------
// Stealth drive: an innocent FAT16 image FILE built on the card. The victim
// sees a small drive containing ONLY disk.zip - our real files (scripts,
// logs, creds) are not on that filesystem at all and physically unreachable
// (LUN is read-only). Same isolation technique USBArmyKnife's mountDiskImage
// uses. Image layout: 4MB, 512B sectors, 4-sector clusters, 2 FATs.
// ---------------------------------------------------------------------------
static const char* IMG_PATH = "/disk.img";
#define IMG_MB        4
#define IMG_SECTORS   (IMG_MB * 1024 * 1024 / 512)   // 8192
#define SEC_PER_CLUS  4
#define FAT_SECTORS   8
#define ROOT_SECTORS  32
#define DATA_START    (1 + 2 * FAT_SECTORS + ROOT_SECTORS)   // sector 49

static bool buildStealthImage() {
    File zip = SD_MMC.open("/disk.zip", FILE_READ);
    uint32_t payloadSize = 0;
    if (zip) { payloadSize = zip.size(); }

    uint32_t clustersNeeded = (payloadSize + SEC_PER_CLUS*512 - 1) / (SEC_PER_CLUS*512);
    uint32_t dataClusters = (IMG_SECTORS - DATA_START) / SEC_PER_CLUS;
    if (clustersNeeded + 1 > dataClusters) {
        logLine("MSC: disk.zip too large for image - building empty drive");
        payloadSize = 0;
        clustersNeeded = 0;
        zip = File();   // drop handle
    }

    SD_MMC.remove(IMG_PATH);
    File img = SD_MMC.open(IMG_PATH, FILE_WRITE);
    if (!img) { logLine("MSC: cannot create disk image"); return false; }

    uint8_t sec[512];
    auto wr = [&](const uint8_t* buf) { img.write(buf, 512); };
    auto zero = [&]() { memset(sec, 0, 512); };

    // --- sector 0: BPB ---
    zero();
    sec[0]=0xEB; sec[1]=0x3C; sec[2]=0x90;
    memcpy(sec+3, "MSDOS5.0", 8);
    sec[11]=0x00; sec[12]=0x02;                    // bytes/sector = 512
    sec[13]=SEC_PER_CLUS;
    sec[14]=0x01; sec[15]=0x00;                    // reserved sectors = 1
    sec[16]=2;                                     // 2 FATs
    sec[17]=0x00; sec[18]=0x02;                    // root entries = 512
    sec[19]=IMG_SECTORS & 0xFF; sec[20]=IMG_SECTORS >> 8;   // total 16-bit
    sec[21]=0xF8;                                  // media
    sec[22]=FAT_SECTORS & 0xFF; sec[23]=FAT_SECTORS >> 8;
    sec[24]=0x20; sec[25]=0x00;                    // sectors/track = 32
    sec[26]=0x40; sec[27]=0x00;                    // heads = 64
    sec[28]=sec[29]=sec[30]=sec[31]=0;             // hidden
    sec[32]=sec[33]=sec[34]=sec[35]=0;             // total 32-bit = 0
    sec[36]=0x80; sec[37]=0; sec[38]=0x29;         // drive, reserved, ext boot sig
    uint32_t vid = esp_random();
    memcpy(sec+39, &vid, 4);
    memcpy(sec+43, "DISK        ", 11);            // volume label
    memcpy(sec+54, "FAT16   ", 8);
    sec[510]=0x55; sec[511]=0xAA;
    wr(sec);

    // --- FATs: cluster chain for disk.zip ---
    auto buildFat = [&]() {
        zero();
        sec[0]=0xF8; sec[1]=0xFF; sec[2]=0xFF; sec[3]=0xFF;   // media + EOC
        for (uint32_t c = 0; c < clustersNeeded; c++) {
            uint32_t idx = 2 + c;                              // first cluster = 2
            uint32_t val = (c == clustersNeeded-1) ? 0xFFFF : idx+1;
            sec[idx*2]   = val & 0xFF;
            sec[idx*2+1] = val >> 8;
        }
        wr(sec);
    };
    buildFat(); buildFat();                          // 2 FATs (sectors 1..16)

    // --- root directory (sectors 17..48) ---
    zero();
    if (payloadSize) {
        memcpy(sec+0, "DISK    ZIP", 11);            // 8.3 name: DISK.ZIP
        sec[11]=0x20;                                // archive
        // fixed sane timestamp: 2020-01-01 12:00:00
        sec[13]=0;                                   // tenths
        sec[14]=0x60; sec[15]=0x8C;                  // time
        sec[16]=0x21; sec[17]=0x54;                  // date
        sec[18]=sec[19]=sec[20]=sec[21]=0;           // last access/write
        sec[22]=sec[23]=0;
        sec[26]=0x02; sec[27]=0x00;                  // first cluster = 2
        memcpy(sec+28, &payloadSize, 4);
    }
    for (int r = 0; r < ROOT_SECTORS; r++) wr(sec);   // dir entry + zeros

    // --- data area: disk.zip content padded to sector boundaries ---
    zero();
    for (uint32_t s2 = DATA_START; s2 < IMG_SECTORS; s2++) {
        if (zip && zip.available()) {
            size_t got = zip.read(sec, 512);
            if (got < 512) { memset(sec+got, 0, 512-got); if (!zip.available()) zip.close(); }
        }
        wr(sec);
    }

    img.close();
    logLine(String("MSC: stealth image built (") + payloadSize + "B payload)");
    return true;
}

// Image-backed LUN state (used instead of raw sectors in stealth mode).
static File mscImg;

void beginCard(bool readOnly) {
    if (!hw::sdMount()) {
        logLine("MSC: no SD card - cannot expose drive");
        return;
    }

    if (readOnly) {
        // Stealth: serve the self-contained image FILE, never raw sectors.
        if (!SD_MMC.exists(IMG_PATH)) buildStealthImage();
        mscImg = SD_MMC.open(IMG_PATH, FILE_READ);
        if (!mscImg || mscImg.size() < 512) {
            logLine("MSC: image unavailable - no stealth drive");
            return;
        }
        const uint32_t LBA = 512;
        uint32_t sectors = mscImg.size() / LBA;
        msc.productRevision("1.0");
        msc.onRead([](uint32_t lba, uint32_t offset, void* buf, uint32_t sz) -> int32_t {
            if (!mscImg.seek(lba * 512 + offset)) return -1;
            size_t got = mscImg.read((uint8_t*)buf, sz);
            return (got == sz) ? (int32_t)sz : (got ? (int32_t)got : -1);
        });
        msc.onWrite([](uint32_t, uint32_t, uint8_t*, uint32_t) -> int32_t {
            return -1;                                 // strictly read-only
        });
        msc.onStartStop([](uint8_t, bool, bool) -> bool { return true; });
        msc.mediaPresent(true);
        msc.isWritable(false);
        msc.begin(sectors, LBA);
        s_active = false;                              // FatFs stays safe to use
        logLine(String("MSC: STEALTH drive (image file) ") + sectors * LBA / 1048576 + " MB");
        return;
    }
    const uint32_t LBA = 512;
    uint64_t bytes = SD_MMC.cardSize();
    uint32_t sectors = (uint32_t)(bytes / LBA);
    if (!sectors) {
        // cardSize()==0 would create an EMPTY LUN -> exactly the "grayed-out
        // USB Drive" symptom. Bail loudly instead.
        logLine("MSC ERROR: card reports 0 sectors!");
        return;
    }

    // NOTE: do NOT call vendorID/productID here! Those strings share the
    // device-wide descriptor pool and would CLOBBER the spoofed identity set
    // by spoof::applyToUsb(). Only per-LUN settings belong here.
    msc.productRevision("1.0");
    // Lambdas can't capture; readOnly goes through a static mirror.
    s_ro = readOnly;
    msc.onRead([](uint32_t lba, uint32_t offset, void* buf, uint32_t sz) -> int32_t {
        // Two CRITICAL requirements:
        //  1. Return BYTES copied (readRAW returns bool - reporting 1/0 made
        //     every sector look like a 1-byte read).
        //  2. Fill the WHOLE buffer: TinyUSB may request multi-sector chunks
        //     (bufsize > 512). Reading only the first sector and claiming
        //     success streams garbage after it -> volume never mounts.
        auto* card = cardFS()->getCard();
        if (!card) return -1;
        // Native multi-sector read - handles any bufsize in one driver call.
        size_t nSectors = sz / 512;
        if (sdmmc_read_sectors(card, buf, lba, nSectors) == ESP_OK)
            return (int32_t)sz;
        return -1;
    });
    msc.onWrite([](uint32_t lba, uint32_t offset, uint8_t* buf, uint32_t sz) -> int32_t {
        if (s_ro) return 0;                                    // swallow writes
        auto* card = cardFS()->getCard();
        if (!card) return -1;
        size_t nSectors = sz / 512;
        if (sdmmc_write_sectors(card, buf, lba, nSectors) == ESP_OK)
            return (int32_t)sz;
        return -1;
    });
    msc.onStartStop([](uint8_t pc, bool start, bool eject) -> bool { return true; });
    msc.mediaPresent(true);
    msc.isWritable(!readOnly);
    msc.begin(sectors, LBA);
    s_active = true;
    logLine(String("MSC: card exposed ") + (readOnly ? "READ-ONLY" : "read-write") +
            " (" + String(sectors * LBA / 1048576) + " MB)");
}

} // namespace msc
