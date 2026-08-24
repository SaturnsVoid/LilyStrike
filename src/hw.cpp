// ============================================================================
// hw.cpp - T-Dongle-S3 hardware bring-up and drivers
// ----------------------------------------------------------------------------
// LED  : APA102 driven by bit-banged SPI frames on DI/CI pins (avoids the
//        FastLED dependency for a single pixel).
// TFT  : Adafruit_ST7735 initR(GREEN) at 160x80. Backlight PWM via ledc.
// SD   : SD_MMC in 4-bit mode (board wires D0..D3). Mount failure is not
//        fatal - the web UI still works, SD features degrade gracefully.
// BTN  : BOOT GPIO0, active LOW, simple debounce.
// ============================================================================
#include "hw.h"
#include "pins.h"
#include "config.h"
#include <SPI.h>

namespace hw {

Adafruit_ST7735* tft = nullptr;
static bool sdOK = false;
static volatile bool btnFlag = false;

// ---------------------------------------------------------------------------
// APA102 protocol: 32 zero start bits, one 0xFF-frame per LED
// (111A AAAA BBBB BBBB GGGG GGGG RRRR RRRR), then >=32 one stop bits.
// We bit-bang because the LED shares no hardware SPI bus with anything else.
// ---------------------------------------------------------------------------
// Bit-bang style copied from lily_ducky's proven sendAPA102(): clock idles
// LOW, data is set first, then the clock pulses HIGH->LOW (rising-edge latch).
static void apaBit(uint8_t b) {
    digitalWrite(s_ledPins.ci, LOW);
    digitalWrite(s_ledPins.di, b & 1);
    digitalWrite(s_ledPins.ci, HIGH);
    digitalWrite(s_ledPins.ci, LOW);
}
static void apaByte(uint8_t b) { for (int i = 7; i >= 0; i--) apaBit((b >> i) & 1); }

// Switch the DI/CI pin mapping at runtime (board revision probing).
void ledUsePins(uint8_t di, uint8_t ci) {
    s_ledPins = {di, ci};
    pinMode(s_ledPins.di, OUTPUT); digitalWrite(s_ledPins.di, LOW);
    pinMode(s_ledPins.ci, OUTPUT); digitalWrite(s_ledPins.ci, LOW);
}

void ledSet(const RGB& c) {
    // APA102 global brightness: 5-bit scalar packed as 111xxxxx.
    uint8_t gb = 0xE0 | APA102_BRIGHTNESS;
    for (int i = 0; i < 4; i++) apaByte(0x00);          // start frame (32 zero bits)
    apaByte(0xFF); apaByte(gb);
    apaByte(c.b); apaByte(c.g); apaByte(c.r);           // B,G,R order!
    for (int i = 0; i < 4; i++) apaByte(0xFF);          // end frame
}

void ledOff() { RGB z = {0,0,0}; ledSet(z); }

static bool backlightPWM = false;   // true once ledc attached

// ---------------------------------------------------------------------------
bool initAll() {
    // Kill backlight FIRST - a floating BCKL pin lights the panel showing
    // un-initialized display RAM ("multi-color static").
    pinMode(PIN_NUM_BCKL, OUTPUT);
    digitalWrite(PIN_NUM_BCKL, LOW);

    pinMode(LED_DI_PIN, OUTPUT); digitalWrite(LED_DI_PIN, LOW);
    pinMode(LED_CI_PIN, OUTPUT); digitalWrite(LED_CI_PIN, LOW);
    ledOff();   // send explicit "all dark" frame so LED can't latch random boot noise

    // Button: BOOT pin has external pullup on board; enable ours anyway.
    pinMode(PIN_BTN_BOOT, INPUT_PULLUP);

    // --- TFT (hardware SPI on its dedicated pins) ---
    SPI.begin(PIN_NUM_CLK, PIN_NUM_MISO, PIN_NUM_MOSI, PIN_NUM_CS);
    tft = new Adafruit_ST7735(&SPI, PIN_NUM_CS, PIN_NUM_DC, PIN_NUM_RST);
    // Always init + clear GRAM at boot even if screen stays "off", otherwise
    // whatever garbage is in RAM shows when backlight comes on later.
    // INITR_MINI160x80_PLUGIN = colstart 26 / rowstart 1 - the offsets this
    // exact panel needs (confirmed against USBArmyKnife's Panel_ST7735S
    // config: offset_x=26, offset_y=1). BLACKTAB writes off-screen => static.
    tft->initR(INITR_MINI160x80_PLUGIN);
    tft->setRotation(3);                 // landscape matching dongle shell
    tft->fillScreen(ST77XX_BLACK);
    if (cfg.screenOnBoot) screenOn(); else screenOff();

    // --- SD (4-bit SD_MMC) ---
    SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN,
                   SD_D0_PIN, SD_D1_PIN, SD_D2_PIN, SD_D3_PIN);
    sdOK = SD_MMC.begin("/sdcard", true);   // mode1bit=false => 4-bit
    if (!sdOK) logLine("SD mount FAILED");
    else {
        if (!SD_MMC.exists("/scripts")) SD_MMC.mkdir("/scripts");
        if (!SD_MMC.exists("/logs"))    SD_MMC.mkdir("/logs");
    }
    return sdOK;
}

// ---------------------------------------------------------------------------
void screenOn() {
    if (!backlightPWM) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        ledcAttach(PIN_NUM_BCKL, 5000, 8);
#else
        ledcSetup(0, 5000, 8); ledcAttachPin(PIN_NUM_BCKL, 0);
#endif
        backlightPWM = true;
    }
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(PIN_NUM_BCKL, cfg.screenBrightness);
#else
    ledcWrite(0, cfg.screenBrightness);
#endif
    if (tft) { tft->fillScreen(ST77XX_BLACK); tft->setTextColor(ST77XX_MAGENTA); }
}
void screenOff() {
    // After ledcAttach() the pin is PWM-owned: digitalWrite is ignored.
    // Must zero/detach the LEDC channel or "off" silently does nothing.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    if (backlightPWM) { ledcWrite(PIN_NUM_BCKL, 0); ledcDetach(PIN_NUM_BCKL); backlightPWM = false; }
#endif
    pinMode(PIN_NUM_BCKL, OUTPUT); digitalWrite(PIN_NUM_BCKL, LOW);
}

void screenClear() { if (tft) tft->fillScreen(ST77XX_BLACK); }

void screenText(const String& t) {
    if (!tft) return;
    tft->fillScreen(ST77XX_BLACK);
    tft->setCursor(2, 2);
    tft->setTextSize(1);
    tft->setTextWrap(true);
    tft->print(t);
}

// ---------------------------------------------------------------------------
// IRAM ISR just latches a flag; debounce handled in buttonPressed().
static void IRAM_ATTR btnISR() { btnFlag = true; }

bool buttonPressed() {
    if (!btnFlag) return false;
    btnFlag = false;
    delay(30);                                  // crude debounce
    return digitalRead(PIN_BTN_BOOT) == LOW;
}

bool buttonWait(uint32_t timeoutMs) {
    uint32_t t0 = millis();
    while (millis() - t0 < timeoutMs) {
        if (buttonPressed()) return true;
        delay(10);
    }
    return false;
}

// Wipe all user files/dirs on the SD card (recursive delete).
// NOTE(step1): this is a logical wipe, not a low-level FAT re-format -
// sufficient for "wipe current card"; raw mkfs can be added later.
static void wipeDir(File dir) {
    File f;
    while ((f = dir.openNextFile())) {
        if (f.isDirectory()) { wipeDir(f); f.close(); continue; }
        String p = f.path();
        f.close();
        SD_MMC.remove(p);
    }
}
bool sdWipe() {
    if (!sdOK) return false;
    File root = SD_MMC.open("/");
    if (!root) return false;
    wipeDir(root);
    root.close();
    // recreate standard folders
    if (!SD_MMC.exists("/scripts")) SD_MMC.mkdir("/scripts");
    if (!SD_MMC.exists("/logs"))    SD_MMC.mkdir("/logs");
    return true;
}

// ---------------------------------------------------------------------------
bool sdMount() { return sdOK; }
uint64_t sdUsedBytes()  { return sdOK ? (uint64_t)(SD_MMC.totalBytes() - SD_MMC.usedBytes()) : 0; }
uint64_t sdTotalBytes() { return sdOK ? SD_MMC.totalBytes() : 0; }

} // namespace hw
