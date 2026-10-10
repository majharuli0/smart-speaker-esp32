#include "health.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <esp_core_dump.h>
#include "config.h"
#include "net.h"

static String bootReport;  // sent once, when the server is reachable

static const char *reasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "power_on";    // plugged in
    case ESP_RST_SW:        return "restart";     // ESP.restart(): update, Wi-Fi reset, ...
    case ESP_RST_PANIC:     return "crash";       // exception: details in "crash"
    case ESP_RST_TASK_WDT:  return "froze";       // loop() stopped for 5 s (task watchdog)
    case ESP_RST_INT_WDT:   return "froze";       // interrupts blocked too long
    case ESP_RST_WDT:       return "froze";       // other watchdog
    case ESP_RST_BROWNOUT:  return "brownout";    // supply voltage dipped (weak USB cable/port, loud audio)
    case ESP_RST_EXT:       return "reset_pin";   // RST button
    case ESP_RST_USB:       return "usb_reset";   // reset over USB (e.g. after uploading)
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    default:                return "unknown";
  }
}

void healthSetup() {
  JsonDocument doc;
  doc["type"] = "boot";
  doc["reason"] = reasonName(esp_reset_reason());
  doc["fw"] = FW_VERSION;

  // A crash leaves a report in the 64 KB coredump partition: summarise it, then clear it
  if (esp_core_dump_image_check() == ESP_OK) {
    static esp_core_dump_summary_t summary;  // ~150 bytes: keep it off the stack
    if (esp_core_dump_get_summary(&summary) == ESP_OK) {
      JsonObject crash = doc["crash"].to<JsonObject>();
      crash["task"] = summary.exc_task;
      char hex[11];
      snprintf(hex, sizeof(hex), "0x%08lx", (unsigned long)summary.exc_pc);
      crash["pc"] = hex;
      JsonArray bt = crash["backtrace"].to<JsonArray>();
      for (uint32_t i = 0; i < summary.exc_bt_info.depth && i < 16; i++) {
        snprintf(hex, sizeof(hex), "0x%08lx", (unsigned long)summary.exc_bt_info.bt[i]);
        bt.add(hex);
      }
    }
    esp_core_dump_image_erase();
  }
  serializeJson(doc, bootReport);
  Serial.println("Boot: " + bootReport);
}

void healthWatchdog() {
  // From here on loop() must come round within 5 s (CONFIG_ESP_TASK_WDT_TIMEOUT_S),
  // or the chip restarts and reports "froze". Long jobs (firmware updates) call feedLoopWDT().
  enableLoopWDT();
}

void healthReport() {
  if (bootReport.isEmpty()) return;
  netSend(bootReport);
  bootReport = "";  // once per boot
}
