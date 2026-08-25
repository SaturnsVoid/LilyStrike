// ============================================================================
// detect_os.cpp - LED side-channel OS fingerprint (see detect_os.h)
// ============================================================================
#include "detect_os.h"
#include <USB.h>
#include <USBHIDKeyboard.h>

// The keyboard object lives in ducky.cpp - we share it so detection runs
// against the same HID interface the scripts use.
namespace ducky { extern USBHIDKeyboard kb; }

namespace detectos {

static HostOS s_last = HostOS::UNKNOWN;

// ---- shared state filled by the USB event callback -------------------------
static volatile bool ledResp = false;
static volatile int  ledCount = 0;
static volatile bool capsSt = false, numSt = false, scrollSt = false;
static volatile bool numChecked = false;
static unsigned long capsSent = 0, numSent = 0, scrollSent = 0;
static unsigned long capsDelay = 0, numDelay = 0, scrollDelay = 0;

static void onUsbEvent(void*, esp_event_base_t base, int32_t id, void* d) {
    if (base != ARDUINO_USB_HID_KEYBOARD_EVENTS) return;
    auto* data = (arduino_usb_hid_keyboard_event_data_t*)d;
    if (id == ARDUINO_USB_HID_KEYBOARD_LED_EVENT) {
        ledResp = true;
        ledCount++;
        capsSt   = data->capslock != 0;
        numSt    = data->numlock != 0;
        scrollSt = data->scrolllock != 0;
        numChecked = true;
        unsigned long now = millis();
        if (capsSent && !capsDelay)   capsDelay   = now - capsSent;
        if (numSent && !numDelay)     numDelay    = now - numSent;
        if (scrollSent && !scrollDelay) scrollDelay = now - scrollSent;
    }
}

static void toggleKey(uint8_t key, unsigned long* sentAt) {
    *sentAt = millis();
    ledResp = false;
    ducky::kb.press(key);
    delay(300);
    ducky::kb.release(key);
    delay(800);
}

// Restore host lock keys to their original state after testing.
static void resetLocks() {
    delay(500);
    if (capsSt)   { unsigned long t; toggleKey(KEY_CAPS_LOCK, &t);   delay(800); }
    if (numSt)    { unsigned long t; toggleKey(KEY_NUM_LOCK, &t);    delay(800); }
    if (scrollSt) { unsigned long t; toggleKey(KEY_SCROLL_LOCK, &t); delay(800); }
}

// Register the LED-report callback at boot so lock-key state stays current
// for the Control page (not just during detection runs).
void initHook() {
    static bool done = false;
    if (!done) { ducky::kb.onEvent(onUsbEvent); done = true; }
}

// Live host-side lock-key state as a compact string.
String lockState() {
    String s;
    if (capsSt)   s += "+CAPS";
    if (numSt && numChecked)   s += "+NUM";
    if (scrollSt) s += "+SCROLL";
    return s.length() ? s.substring(1) : "";
}

HostOS detect() {
    static bool evtHooked = false;
    if (!evtHooked) { ducky::kb.onEvent(onUsbEvent); evtHooked = true; }

    // ---- reset state ----
    ledCount = 0;
    capsSt = numSt = scrollSt = false;
    capsDelay = numDelay = scrollDelay = 0;
    capsSent = numSent = scrollSent = 0;
    ledResp = false;
    bool initialCaps = capsSt;

    resetLocks();

    // ---- CAPS LOCK probe ----
    toggleKey(KEY_CAPS_LOCK, &capsSent);
    delay(1500);
    bool capsResponded = ledResp;

    // iOS early exit: host never sends LED reports but accepted the keypress.
    if (!ledResp && capsSt != initialCaps) { s_last = HostOS::IOS; resetLocks(); return s_last; }

    // ---- NUM LOCK probe ----
    toggleKey(KEY_NUM_LOCK, &numSent);
    delay(1200);

    // ---- SCROLL LOCK probe ----
    toggleKey(KEY_SCROLL_LOCK, &scrollSent);
    delay(1200);

    // ---- decision tree (ported from reference implementation) ----
    if (ledCount == 0) {
        s_last = (capsSt != initialCaps) ? HostOS::IOS : HostOS::MACOS;
    }
    else if (ledCount >= 3 && capsDelay < 100 && numDelay < 100 && scrollDelay < 100) {
        s_last = HostOS::WINDOWS;
    }
    else {
        bool hasNum = (numDelay > 0), hasScroll = (scrollDelay > 0);
        if (ledCount == 1 && capsSt != initialCaps && capsDelay > 0 &&
            capsDelay < 20 && !hasNum && !hasScroll)
            s_last = HostOS::CHROMEOS;
        else if (numSt && !scrollSt && hasNum && !hasScroll)
            s_last = HostOS::LINUX;
        else if ((capsDelay > 200 || numDelay > 200 || scrollDelay > 200) &&
                 (hasNum || hasScroll))
            s_last = HostOS::ANDROID;
        else if (capsSt != initialCaps)
            s_last = HostOS::IOS;
        else
            s_last = HostOS::UNKNOWN;
    }

    resetLocks();   // restore whatever we toggled during probing
    return s_last;
}

HostOS lastResult() { return s_last; }

String nameOf(HostOS os) {
    switch (os) {
        case HostOS::WINDOWS:  return "Windows";
        case HostOS::LINUX:    return "Linux";
        case HostOS::MACOS:    return "macOS";
        case HostOS::IOS:      return "iOS";
        case HostOS::ANDROID:  return "Android";
        case HostOS::CHROMEOS: return "ChromeOS";
        default:               return "Unknown";
    }
}

bool matches(HostOS a, const String& lowerName) {
    String n = nameOf(a); n.toLowerCase();
    return n == lowerName;
}

} // namespace detectos
