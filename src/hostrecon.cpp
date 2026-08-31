// ============================================================================
// hostrecon.cpp - ARP sweep + TCP connect port scan (see hostrecon.h)
// ----------------------------------------------------------------------------
// ARP sweep technique from WifiPhisher networking/scanner.c: batch
// etharp_request() broadcasts, wait, then read lwIP's ARP table via
// etharp_find_addr(). Runs entirely inside lwIP - no raw sockets needed.
// Port scan: lwip_connect() with short timeouts per port (TCP connect scan).
// ============================================================================
#include "hostrecon.h"
#include "config.h"
#include "ducky.h"
#include <WiFi.h>
#include <lwip/sockets.h>
#include <lwip/netif.h>
#include <lwip/etharp.h>
#include <esp_netif.h>
#include <esp_netif_net_stack.h>
#include <lwip/tcpip.h>
#include <vector>
#include <algorithm>

// Resolve the STA lwIP netif the WifiPhisher way - netif_default can be the
// AP netif in AP+STA mode, and calling etharp against the wrong netif is
// what reset the device. (definition inside namespace hostrecon below)
namespace hostrecon {

static volatile bool s_scanning = false;
bool scanning() { return s_scanning; }

static struct netif* staNetif() {
    esp_netif_t* en = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!en) return nullptr;
    return (struct netif*)esp_netif_get_netif_impl(en);
}

static String macStr(const uint8_t* m) {
    char b[18];
    snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X",
             m[0],m[1],m[2],m[3],m[4],m[5]);
    return String(b);
}

// ---- async sweep: dedicated task + cached results (HTTP handler must not
// block 60s+ on 254 WiFi broadcasts) ----
static std::vector<Host> s_result;
static SemaphoreHandle_t s_resMtx = nullptr;
static uint8_t s_progress = 0;
static void resLock() { if (!s_resMtx) s_resMtx = xSemaphoreCreateMutex(); xSemaphoreTake(s_resMtx, portMAX_DELAY); }
static void resUnlock() { if (s_resMtx) xSemaphoreGive(s_resMtx); }

static void sweepTask(void*) {
    std::vector<Host> out;
    if (WiFi.status() == WL_CONNECTED) {
        struct netif* nif = staNetif();
        if (nif) {
            IPAddress mine = WiFi.localIP();
            // BUGFIX (round 2, verified by tcpdump): lwIP ip4_addr_t.addr is
            // NETWORK byte order. IPAddress's uint32_t conversion is byte-
            // REVERSED relative to that, so BOTH earlier attempts targeted
            // garbage IPs (tcpdump showed "who-has 1.168.12.209"). Correct
            // address for last octet L: keep the low 3 bytes of the converted
            // IP and put L in the top byte.
            uint32_t mineU32 = (uint32_t)mine;
            uint32_t base = mineU32 & 0x00FFFFFFUL;
            uint8_t myLast = mine[3];

            // ALL etharp access runs on the tcpip thread (this lwIP build has
            // no core locking - direct calls from a foreign task are unsafe).
            struct SweepCtx {
                struct netif* nif; uint32_t base; uint8_t myLast;
                struct { uint32_t ip; uint8_t mac[6]; } out[253];
                int n;
            };
            static SweepCtx ctx;   // static: no heap on the tcpip thread
            ctx = SweepCtx{ nif, base, myLast, {}, 0 };

            auto sendReqs = [](void* p) {
                auto* m = (SweepCtx*)p;
                for (int last = 1; last < 255; last++) {
                    if (last == m->myLast) continue;
                    ip4_addr_t dest;
                    dest.addr = m->base | ((uint32_t)last << 24);
                    etharp_request(m->nif, &dest);
                    if ((last % 16) == 0) delay(2);
                }
            };
            tcpip_callback_wait(sendReqs, &ctx);
            logLine("arp-sweep: requests sent");
            delay(1500);   // let replies land

            auto collect = [](void* p) {
                auto* m = (SweepCtx*)p;
                for (int last = 1; last < 255; last++) {
                    if (last == m->myLast) continue;
                    ip4_addr_t dest;
                    dest.addr = m->base | ((uint32_t)last << 24);
                    struct eth_addr* eth = nullptr;
                    const ip4_addr_t* ipret = nullptr;
                    if (etharp_find_addr(m->nif, &dest, &eth, &ipret) >= 0 && eth) {
                        m->out[m->n].ip = dest.addr;
                        memcpy(m->out[m->n].mac, eth->addr, 6);
                        m->n++;
                    }
                    if ((last % 32) == 0) delay(1);
                }
            };
            tcpip_callback_wait(collect, &ctx);

            for (int i = 0; i < ctx.n; i++) {
                Host h;
                uint32_t a = ctx.out[i].ip;
                IPAddress ip(a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, (a >> 24) & 0xFF);
                h.ip = ip.toString();
                h.mac = macStr(ctx.out[i].mac);
                out.push_back(h);
            }
            logLine("arp-sweep: found " + String(ctx.n) + " live hosts");
        }
    }
    resLock(); s_result = out; resUnlock();
    s_progress = 100;
    s_scanning = false;
    vTaskDelete(nullptr);
}

bool arpStart() {
    if (s_scanning || WiFi.status() != WL_CONNECTED) return false;
    WiFi.setSleep(WIFI_PS_NONE);   // power save drops broadcast replies
    s_scanning = true;
    s_progress = 0;
    resLock(); s_result.clear(); resUnlock();
    if (xTaskCreatePinnedToCore(sweepTask, "arpsweep", 8192, nullptr, 1, nullptr, 0) != pdPASS) {
        s_scanning = false;
        return false;
    }
    return true;
}

std::vector<Host> arpResults() {
    resLock(); std::vector<Host> out = s_result; resUnlock();
    return out;
}

uint8_t arpProgress() { return s_progress; }

std::vector<Host> arpSweep() {   // legacy sync wrapper (tests)
    if (!arpStart()) return {};
    while (scanning()) delay(100);
    return arpResults();
}
std::vector<uint16_t> portScan(const String& ipStr, const std::vector<uint16_t>& ports) {
    std::vector<uint16_t> open;
    if (WiFi.status() != WL_CONNECTED) return open;
    s_scanning = true;

    IPAddress ip;
    if (!ip.fromString(ipStr)) { s_scanning = false; return open; }

    for (uint16_t port : ports) {
        int sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) break;
        // short timeout: closed ports die on refusal, filtered ones timeout
        timeval tv = { .tv_sec = 0, .tv_usec = 120000 };
        lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        lwip_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        sockaddr_in sa = {};
        sa.sin_family = AF_INET;
        sa.sin_port = lwip_htons(port);
        sa.sin_addr.s_addr = static_cast<uint32_t>(ip);
        if (lwip_connect(sock, (sockaddr*)&sa, sizeof(sa)) == 0)
            open.push_back(port);
        lwip_close(sock);
        delay(1);
    }
    s_scanning = false;
    return open;
}

} // namespace hostrecon
