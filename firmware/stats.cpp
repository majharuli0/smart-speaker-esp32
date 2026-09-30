#include "stats.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <nvs.h>
#include <esp_timer.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include "config.h"
#include "net.h"
#include "timesync.h"
#include "tonecache.h"
#if CONFIG_IDF_TARGET_ESP32S3
// Only on the S3: these add ~100 KB, which the original ESP32's app space can't spare
#include <FFat.h>
#include <LittleFS.h>
#define MEASURE_FILE_STORAGE 1
#endif

// Measured once at boot: ESP.getSketchSize() re-checksums the whole program
// in flash (~1 MB) on every call, which froze audio when it ran every 2 s.
// The size can't change while running anyway.
static uint32_t appUsed = 0, appTotal = 0;
static String partitionsJson;

// CPU load per core = share of time its idle task did NOT run since the last
// call (FreeRTOS run-time stats are enabled in the ESP32 Arduino core).
static void cpuLoad(int out[2]) {
  static uint32_t lastIdle[2] = {0, 0};
  static uint32_t lastTime = 0;
  uint32_t now = (uint32_t)esp_timer_get_time();  // µs; unsigned math survives the wrap
  uint32_t elapsed = now - lastTime;
  for (int core = 0; core < 2; core++) {
    uint32_t idle = ulTaskGetIdleRunTimeCounterForCore(core);
    uint32_t idleDelta = idle - lastIdle[core];
    out[core] = (lastTime && elapsed) ? constrain(100 - (int)(100ULL * idleDelta / elapsed), 0, 100) : 0;
    lastIdle[core] = idle;
  }
  lastTime = now;
}

static void sendStats() {
  nvs_stats_t nvs = {};
  nvs_get_stats(NULL, &nvs);
  int cpu[2];
  cpuLoad(cpu);

  JsonDocument doc;
  doc["type"] = "stats";
  doc["heapFree"] = ESP.getFreeHeap();
  doc["heapTotal"] = ESP.getHeapSize();
  doc["heapMin"] = ESP.getMinFreeHeap();       // lowest free RAM since boot
  doc["heapMaxBlock"] = ESP.getMaxAllocHeap(); // largest single free block
  doc["psramTotal"] = ESP.getPsramSize();      // 0 on boards without PSRAM
  doc["psramFree"] = ESP.getFreePsram();
  doc["appUsed"] = appUsed;
  doc["appTotal"] = appTotal;
  doc["nvsUsed"] = nvs.used_entries;           // settings storage (Wi-Fi, volume)
  doc["nvsTotal"] = nvs.total_entries;
  doc["flashSize"] = ESP.getFlashChipSize();
  doc["cpuMhz"] = ESP.getCpuFreqMHz();
  doc["cpu"][0] = cpu[0];                      // core 0: Wi-Fi
  doc["cpu"][1] = cpu[1];                      // core 1: this sketch
  doc["uptime"] = millis() / 1000;
  doc["rssi"] = WiFi.RSSI();
  doc["fw"] = FW_VERSION;
  doc["time"] = timeNow();                  // local time, "" until NTP has synced
  doc["tz"] = timezoneName();

  String out;
  serializeJson(doc, out);
  netSend(out);
}

// ---- Flash layout: every partition, its size, and how much is used ----

// Mount a file-storage partition read-only for a moment to see how full it is
static void fileStorageUsage(JsonObject o, const esp_partition_t *p) {
#ifdef MEASURE_FILE_STORAGE
  bool fat = p->subtype == ESP_PARTITION_SUBTYPE_DATA_FAT;
  if (fat && cacheReady()) {  // already mounted by the tone storage: measure, don't unmount it
    o["used"] = FFat.usedBytes();
    o["fsTotal"] = FFat.totalBytes();
    o["note"] = "tone storage (FAT)";
    return;
  }
  if (fat && FFat.begin(false, "/ffat", 1, p->label)) {
    o["used"] = FFat.usedBytes();
    o["fsTotal"] = FFat.totalBytes();
    FFat.end();
    o["note"] = "file storage (FAT)";
    return;
  }
  if (!fat && LittleFS.begin(false, "/littlefs", 1, p->label)) {
    o["used"] = LittleFS.usedBytes();
    o["fsTotal"] = LittleFS.totalBytes();
    LittleFS.end();
    o["note"] = "file storage (LittleFS)";
    return;
  }
  o["note"] = "file storage (empty, not formatted yet)";
#else
  o["note"] = "file storage (usage not measured on this board)";
#endif
}

static void buildPartitions() {
  partitionsJson = "";
  JsonDocument doc;
  doc["type"] = "partitions";
  doc["flashSize"] = ESP.getFlashChipSize();
  JsonArray list = doc["list"].to<JsonArray>();
  const esp_partition_t *running = esp_ota_get_running_partition();

  // esp_partition_next() frees the iterator itself when it reaches the end
  for (esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
       it; it = esp_partition_next(it)) {
    const esp_partition_t *p = esp_partition_get(it);
    JsonObject o = list.add<JsonObject>();
    o["name"] = p->label;
    o["offset"] = p->address;
    o["size"] = p->size;

    if (p->type == ESP_PARTITION_TYPE_APP) {
      o["kind"] = "app";
      if (p == running) { o["used"] = appUsed; o["note"] = "running program"; }
      else o["note"] = "spare slot for over-the-air updates";
      continue;
    }
    switch (p->subtype) {
      case ESP_PARTITION_SUBTYPE_DATA_NVS: {
        nvs_stats_t s = {};
        nvs_get_stats(p->label, &s);
        o["kind"] = "nvs";
        if (s.total_entries) o["used"] = (uint64_t)p->size * s.used_entries / s.total_entries;
        o["note"] = "settings (Wi-Fi, volume)";
        break;
      }
      case ESP_PARTITION_SUBTYPE_DATA_FAT:
      case ESP_PARTITION_SUBTYPE_DATA_SPIFFS:  // LittleFS uses this subtype too
        o["kind"] = "files";
        fileStorageUsage(o, p);
        break;
      case ESP_PARTITION_SUBTYPE_DATA_OTA:      o["kind"] = "system"; o["note"] = "which app slot to start"; break;
      case ESP_PARTITION_SUBTYPE_DATA_COREDUMP: o["kind"] = "system"; o["note"] = "crash report storage"; break;
      case ESP_PARTITION_SUBTYPE_DATA_PHY:      o["kind"] = "system"; o["note"] = "radio calibration"; break;
      default:                                  o["kind"] = "other";
    }
  }
  serializeJson(doc, partitionsJson);
}

void statsSetup() {
  appUsed = ESP.getSketchSize();
  appTotal = appUsed + ESP.getFreeSketchSpace();
  buildPartitions();
}

// Rebuilt each time (cheap, unlike the app size) so stored tones show up
void sendPartitions() {
  buildPartitions();
  netSend(partitionsJson);
}

void statsLoop() {
  static unsigned long lastStats = 0;
  if (netConnected() && millis() - lastStats >= STATS_INTERVAL_MS) {
    lastStats = millis();
    sendStats();
  }
}
