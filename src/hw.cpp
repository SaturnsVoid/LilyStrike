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
#include <APA102.h>

namespace hw {

Adafruit_ST7735* tft = nullptr;
static bool sdOK = false;
static volatile bool btnFlag = false;
static bool s_screenOn = false;

// Pololu APA102 software-SPI on the SDK pin map (DI=40, CI=39).
static APA102<LED_DI_PIN, LED_CI_PIN> apaStrip;
static const uint16_t APA_COUNT = 1;
static rgb_color apaBuf[APA_COUNT];
static const uint8_t APA_BRIGHT = 10;   // 0-31; this LED is blinding at max

void ledSet(const RGB& c) {
    apaBuf[0] = rgb_color(c.r, c.g, c.b);
    apaStrip.write(apaBuf, APA_COUNT, APA_BRIGHT);
    apaLatch();
}
void ledOff() {
    apaBuf[0] = rgb_color(0, 0, 0);
    apaStrip.write(apaBuf, APA_COUNT, 0);   // brightness 0 = fully dark
    apaLatch();
}
// Extra clock pulses with data LOW after a frame - required for the global
// brightness register to actually latch on some APA102 batches. Without
// these the LED ignores dark frames at boot and keeps showing garbage.
static void apaLatch() {
    pinMode(LED_DI_PIN, OUTPUT); digitalWrite(LED_DI_PIN, LOW);
    pinMode(LED_CI_PIN, OUTPUT);
    for (int i = 0; i < 36; i++) {
        digitalWrite(LED_CI_PIN, HIGH);
        digitalWrite(LED_CI_PIN, LOW);
    }
}

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
    s_screenOn = true;
    if (tft) { tft->fillScreen(ST77XX_BLACK); tft->setTextColor(ST77XX_MAGENTA); }
}
bool screenIsOn() { return s_screenOn; }
// Push cfg.screenBrightness to the PWM without touching anything else.
void applyBrightness() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    if (backlightPWM) ledcWrite(PIN_NUM_BCKL, cfg.screenBrightness);
#else
    if (backlightPWM) ledcWrite(0, cfg.screenBrightness);
#endif
}
void screenOff() {
    // After ledcAttach() the pin is PWM-owned: digitalWrite is ignored.
    // Must zero/detach the LEDC channel or "off" silently does nothing.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    if (backlightPWM) { ledcWrite(PIN_NUM_BCKL, 0); ledcDetach(PIN_NUM_BCKL); backlightPWM = false; }
#endif
    pinMode(PIN_NUM_BCKL, OUTPUT); digitalWrite(PIN_NUM_BCKL, LOW);
    backlightPWM = false;   // pin released from PWM - digitalWrite works again
    s_screenOn = false;
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
