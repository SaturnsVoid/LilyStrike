// ============================================================================
// pins.h - LILYGO T-Dongle-S3 (screen variant, NO PSRAM) hardware pin map
// ----------------------------------------------------------------------------
// Source of truth: T-Dongle-S3 SDK examples/factory_screen/factory_screen.ino
// Keep every hardware reference in one place so future steps (OS detect,
// EvilAP, Qwiic, etc.) can import without hunting.
//
// NOTE: SD card on the T-Dongle-S3 is wired to the SDMMC controller (4-bit
// mode), NOT the SPI bus. The TFT uses its own SPI bus (VSPI-class pins).
// ============================================================================
#pragma once

// ---- Button ---------------------------------------------------------------
#define PIN_BTN_BOOT        0       // BOOT button, active LOW, also strapping

// ---- APA102 RGB LED (DotStar) --------------------------------------------
#define LED_DI_PIN          40      // APA102 data in
#define LED_CI_PIN          39      // APA102 clock in
#define LED_COUNT           1       // single 2020 dotstar on this board
#define APA102_BRIGHTNESS   5       // global brightness scalar (0..31); keep LOW,
                                    // this LED is blinding at full power.

// ---- ST7735 TFT (SPI) ------------------------------------------------------
#define PIN_NUM_MISO        -1      // display has no MISO line
#define PIN_NUM_MOSI        3
#define PIN_NUM_CLK         5
#define PIN_NUM_CS          4
#define PIN_NUM_DC          2
#define PIN_NUM_RST         1
#define PIN_NUM_BCKL        38      // backlight (PWM capable)
#define TFT_WIDTH_PX        160
#define TFT_HEIGHT_PX       80

// ---- Micro-SD (SD_MMC, 4-bit) ---------------------------------------------
#define SD_CLK_PIN          12
#define SD_CMD_PIN          16
#define SD_D0_PIN           14
#define SD_D1_PIN           17      // unused in 1-bit mode but defined for 4-bit
#define SD_D2_PIN           21
#define SD_D3_PIN           18
