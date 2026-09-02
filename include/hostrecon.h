// ============================================================================
// hostrecon.h - Host discovery (ARP sweep) + TCP port scanner (Step 4 add)
// ----------------------------------------------------------------------------
// Works when the device is a client on a target network (CONNECT_AP).
//  * ARP sweep: lwIP etharp_request across the /24, live hosts' IP+MAC from
//    the ARP table (lwIP etharp API - no raw sockets).
//  * TCP port scan: connect()-based; SYN scan would need raw lwIP access.
// ============================================================================
#pragma once
#include <Arduino.h>

namespace hostrecon {

struct Host { String ip, mac; };

// ARP sweep of x.y.z.1-254 around our own IP, run in a background task
// (254 WiFi broadcasts block way too long for an HTTP handler).
// arpStart() kicks it off; poll arpResults() until scanning() is false.
bool arpStart();                       // false if already scanning / no WiFi
std::vector<Host> arpResults();        // last/current sweep results
uint8_t arpProgress();                 // % of addresses probed

// Blocking TCP connect scan on one host. ports = sorted list.
// Returns open port numbers. ~150ms per closed port, instant on open.
std::vector<uint16_t> portScan(const String& ip, const std::vector<uint16_t>& ports);

bool scanning();
} // namespace hostrecon
