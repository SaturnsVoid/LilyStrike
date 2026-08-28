// ============================================================================
// sys.cpp - self destruct implementation
// ============================================================================
#include "sys.h"
#include "config.h"
#include "hw.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_system.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>

namespace sys {

void selfDestruct() {
    logLine("!!! SELF DESTRUCT INITIATED !!!");
    // 1) user settings + spoofed identity
    configFactoryReset();
    { Preferences p; p.begin("spoof", false); p.clear(); p.end(); }

    // 2) web UI assets
    if (LittleFS.begin(true)) LittleFS.format();

    // 3) everything on the SD card
    if (hw::sdMount()) hw::sdWipe();

    // 4) TRUE flash wipe - per plan: "Recovery only by Re-Flashing Firmware".
    //    Erase every partition except the bootloader, INCLUDING the one we
    //    are executing from (vectors last -> instant death afterwards).
    //    The bootloader partition is deliberately left intact so the user
    //    can recover via download mode + pio upload.
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* cur = nullptr;
    esp_partition_iterator_t it;
    // iterate APP then DATA partitions (NULL subtype = any)
    for (int pass = 0; pass < 2; pass++) {
        esp_partition_type_t t = pass ? ESP_PARTITION_TYPE_DATA
                                      : ESP_PARTITION_TYPE_APP;
        it = esp_partition_find(t, ESP_PARTITION_SUBTYPE_ANY, NULL);
        while (it) {
            cur = esp_partition_get(it);
            // skip bootloader (not a partition obj) and OTA-data marker
            bool isOtaData = (cur->type == ESP_PARTITION_TYPE_DATA &&
                              cur->subtype == ESP_PARTITION_SUBTYPE_DATA_OTA);
            if (cur->address != running->address && !isOtaData) {
                esp_partition_erase_range(cur, 0, cur->size);
            }
            it = esp_partition_next(it);
        }
        esp_partition_iterator_release(it);
    }
    if (true) {
        const esp_partition_t* otadata = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
        if (otadata) esp_partition_erase_range(otadata, 0, otadata->size);
    }
    // 5) decapitate the running app LAST: erase its first 32KB (vector table
    //    + startup code). Cache suspend/resume happens inside the flash API;
    //    the next instruction fetch from that range crashes the CPU ->
    //    bootloader finds no valid app -> reboot loop until re-flash.
    esp_partition_erase_range(running, 0, 0x8000);
    // If execution somehow survives (cache quirk), force it:
    while (true) { delay(1000); }
}

} // namespace sys
