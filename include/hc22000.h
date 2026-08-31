// ============================================================================
// hc22000.h - WPA handshake pcap -> hashcat 22000 converter (see hc22000.cpp)
// ============================================================================
#pragma once
#include <Arduino.h>

namespace hc {
// Returns 22000-format lines (may be empty). Does NOT write to SD.
String fromPcap(const String& pcapPath);
}
