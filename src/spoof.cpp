// ============================================================================
// spoof.cpp - USB identity spoofing (see spoof.h)
// ============================================================================
#include "spoof.h"
#include <Preferences.h>
#include <USB.h>
#include <esp_system.h>

namespace spoof {

// Presets from the project plan - look like ordinary consumer HID devices.
const SpoofPreset PRESETS[] = {
    {0x1E7D, 0x2E7D, "ROCCAT",             "Nyth White Mouse"},
    {0x1E7D, 0x2F76, "ROCCAT",             "Sova Keyboard"},
    {0x1532, 0x000A, "Razer USA, Ltd",     "Mamba (Wired)"},
    {0x1532, 0x005F, "Razer USA, Ltd",     "DeathAdder 2000"},
    {0x03F0, 0x0122, "HP, Inc",            "HID Internet Keyboard"},
    {0x03F0, 0x2B4A, "HP, Inc",            "Business Slim Keyboard"},
    {0x046D, 0xC063, "Logitech, Inc.",     "DELL Laser Mouse"},
    {0x045E, 0x07F8, "Microsoft Corporation", "Wired Keyboard 600 (model 1576)"},
};
const size_t PRESET_COUNT = sizeof(PRESETS)/sizeof(PRESETS[0]);

static uint16_t s_vid = 0, s_pid = 0;
static String s_vendor, s_product, s_serial;

void load() {
    Preferences p; p.begin("spoof", true);
    bool have = p.getBool("set", false);
    bool randomPerBoot = p.getBool("randBoot", false);
    if (randomPerBoot) {
        // Explicit user-selected mode: fresh identity every power-up.
        randomize();
        return;
    }
    s_vid = p.getUShort("vid", 0);
    s_pid = p.getUShort("pid", 0);
    s_vendor  = p.getString("vendor", "");
    s_product = p.getString("product", "");
    s_serial  = p.getString("serial", "");
    p.end();
    if (!have || !s_vid || !s_pid) {
        // First boot: pick one preset AND persist it, otherwise every boot
        // would re-randomize (identity must be stable unless user opts in).
        randomize();
        save();
    }
}

void save() {
    Preferences p; p.begin("spoof", false);
    p.putBool("set", true);
    p.putUShort("vid", s_vid);
    p.putUShort("pid", s_pid);
    p.putString("vendor", s_vendor);
    p.putString("product", s_product);
    p.putString("serial", s_serial);
    p.end();
}

void randomize() {
    const SpoofPreset& pr = PRESETS[esp_random() % PRESET_COUNT];
    set(pr.vid, pr.pid, pr.vendor, pr.product, "");   // "" = generate serial
}

void set(uint16_t vid, uint16_t pid, const String& vendor,
         const String& product, const String& serial) {
    s_vid = vid; s_pid = pid;
    s_vendor = vendor; s_product = product;
    if (serial.length()) s_serial = serial;
    else {
        // Plan: random 12-digit serial when unset.
        s_serial = "";
        for (int i = 0; i < 12; i++) s_serial += char('0' + esp_random() % 10);
        if (s_serial.startsWith("0")) s_serial[0] = '1';   // keep it non-zero-leading
    }
}

uint16_t vid()      { return s_vid; }
uint16_t pid()      { return s_pid; }
String   vendor()   { return s_vendor; }
String   product()  { return s_product; }
String   serial()   { return s_serial; }

void applyToUsb() {
    // These setters only take effect at enumeration time - i.e. before the
    // single USB.begin() in ducky::initOnce().
    USB.VID(s_vid);
    USB.PID(s_pid);
    USB.manufacturerName(s_vendor.c_str());
    USB.productName(s_product.c_str());
    USB.serialNumber(s_serial.c_str());
}

} // namespace spoof
