// All the settings you might change, in one place.
#pragma once
#include <Arduino.h>  // board definitions (CONFIG_IDF_TARGET_*, LED_BUILTIN)

// ---- Firmware version: bump on every release (shown on the web page) ----
#define FW_VERSION "0.14.0"

// ---- Broker address and device secret: in secrets.h, which is not in git ----
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"  // placeholders: builds, but can't connect
#endif
#ifndef DEVICE_SECRET
#error "Add DEVICE_SECRET to secrets.h: the same value as DEVICE_SECRET in the backend's .env"
#endif
#define PROV_NAME_PREFIX "SS-"   // Bluetooth name during Wi-Fi setup + last 4 of the device ID: "SS-09FC"
#define WIFI_RECOVERY_MS (5 * 60 * 1000UL)  // saved Wi-Fi unreachable this long: open Bluetooth setup too

// ---- Time ----
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.google.com"
#define DEFAULT_TZ_POSIX "UTC0"  // until the server sends the real zone
#define DEFAULT_TZ_NAME  "UTC"

// ---- Board: ESP32-S3-WROOM-1 N16R8 (16 MB flash, 8 MB PSRAM) only ----
#if !CONFIG_IDF_TARGET_ESP32S3
#error "This firmware is for the ESP32-S3 (N16R8). Build with ./fw.sh, which selects it."
#endif

// MAX98357A amp: GPIO 4/5/6 sit next to each other on the left header.
// Avoid 35-37 (octal PSRAM), 19/20 (USB), 43/44 (serial), 0/3/45/46 (boot pins).
#define I2S_DOUT_GPIO 4
#define I2S_BCLK_GPIO 5
#define I2S_LRC_GPIO  6
#define I2S_PORT I2S_NUM_0
// LED_BUILTIN is the board's RGB LED (GPIO 48), defined by the S3 core

// ---- Audio ----
#define SAMPLE_RATE 16000     // tones are converted to 16 kHz 16-bit mono WAV by the web page
#define DEFAULT_VOLUME 70     // 0-100, used until the user changes it
#define RING_MAX_MS 60000     // stop ringing after 1 minute if nobody presses Stop
#define MAX_ALARMS 20         // alarms stored on the device (saved in settings storage)
#define SNOOZE_MS (9 * 60 * 1000UL)  // snooze rings again after 9 minutes
#define FADE_IN_MS 30000      // alarms rise from 10% to full volume over 30 s
#define TONE_TIMEOUT_MS 3000  // give up downloading a tone after this and beep instead

// Tones stored on the device (in the 9.9 MB "ffat" area) so alarms play the real tone offline
#define TONE_CACHE_PARTITION "ffat"
#define TONE_RETRY_MS 30000   // after a failed download, try again this much later

// microSD card module (SPI, 3V3): GPIO 10-13 sit together on the left header
// and are the S3's own fast SPI pins. Card formatted FAT32.
#define SD_CS_GPIO   10
#define SD_MOSI_GPIO 11
#define SD_SCK_GPIO  12
#define SD_MISO_GPIO 13
#define SD_SPI_HZ    8000000  // 8 MHz: plenty for audio, reliable over jumper wires
#define SD_CHECK_MS  5000     // how often to notice a card being inserted or removed

// Live voice from the browser (same 16 kHz mono format as tones). It arrives in
// 125 ms pieces (the cloud broker allows the server only 10 messages a second).
#define TALK_BUF_SAMPLES 16000  // 1 s ring buffer; when full the oldest audio is dropped
#define TALK_PREBUFFER   3000   // wait for ~190 ms (2 pieces) before playing, to ride out gaps between pieces
#define TALK_DRY_MS      150   // no audio for longer than the I2S queue holds → buffer up again

// ---- Updates over Wi-Fi ----
#define OTA_CONFIRM_MS 180000  // a new version must reach the server within 3 min of boot, or it's rolled back

// ---- Web page stats ----
#define STATS_INTERVAL_MS 2000     // stats while a page is open (the server says "watch")
#define STATS_IDLE_MS     300000   // otherwise a 5-minute heartbeat, to save MQTT traffic
