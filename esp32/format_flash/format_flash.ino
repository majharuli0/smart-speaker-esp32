#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <WebSocketsClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <nvs.h>
#include <esp_timer.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include "driver/i2s.h"
#if CONFIG_IDF_TARGET_ESP32S3
// Only on the S3 (3 MB app space): these add ~100 KB, which the original
// ESP32's default 1.3 MB app space (already ~96% full) can't fit
#include <FFat.h>
#include <LittleFS.h>
#define MEASURE_FILE_STORAGE 1
#endif

#define SERVER_MDNS_NAME "led-server"  // server.js answers for led-server.local
#define SERVER_PORT 3000

// MAX98357A amp wiring, picked by which board you compile for
#if CONFIG_IDF_TARGET_ESP32S3
// ESP32-S3-WROOM-1 N16R8: GPIO 4/5/6 sit next to each other on the left header.
// Avoid 35-37 (octal PSRAM), 19/20 (USB), 43/44 (serial), 0/3/45/46 (boot pins).
#define I2S_DOUT_GPIO 4
#define I2S_BCLK_GPIO 5
#define I2S_LRC_GPIO  6
#else
// Original ESP32 (physically traced, don't re-derive from a diagram)
#define I2S_DOUT_GPIO 33
#define I2S_BCLK_GPIO 25
#define I2S_LRC_GPIO  32
#endif
#define I2S_PORT I2S_NUM_0

#ifndef LED_BUILTIN
#define LED_BUILTIN 2  // original ESP32 dev board's blue LED (the S3 core defines its RGB LED)
#endif
#define SAMPLE_RATE 16000   // tones are converted to 16 kHz 16-bit mono WAV by the web page
#define RING_MAX_MS 60000   // stop ringing after 1 minute if nobody presses Stop

// Live voice from the browser (same 16 kHz mono format as tones)
#define TALK_BUF_SAMPLES 4096  // 256 ms ring buffer; when full the oldest audio is dropped
#define TALK_PREBUFFER   1600  // wait for 100 ms of audio before playing, to ride out Wi-Fi hiccups
#define TALK_DRY_MS      150   // no audio for longer than the I2S queue holds → buffer up again

#define STATS_INTERVAL_MS 2000  // how often RAM/storage/CPU stats go to the web page

WebSocketsClient webSocket;
WiFiManager wm;
String deviceId;
IPAddress serverIp;
Preferences prefs;

int volume = 70;          // 0-100, saved in flash so it survives a restart
int32_t volumeGain = 0;   // 0-256 multiplier applied to every sample

HTTPClient http;
WiFiClient *toneStream = nullptr;
String ringingTone;           // empty = not ringing
int32_t toneBytesLeft = 0;
unsigned long ringStart = 0;

int16_t talkBuf[TALK_BUF_SAMPLES];
size_t talkHead = 0, talkCount = 0;  // read position, samples buffered
bool talking = false, talkPlaying = false, talkEnding = false;
unsigned long talkLastPlayed = 0;

void blink() {
  digitalWrite(LED_BUILTIN, HIGH);
  delay(300);
  digitalWrite(LED_BUILTIN, LOW);
}

// Squared curve: ears hear loudness logarithmically, so a linear slider
// would do almost nothing in its top half.
void setVolume(int v) {
  volume = constrain(v, 0, 100);
  volumeGain = volume * volume * 256 / 10000;
}

inline int16_t applyVolume(int16_t s) {
  return (int32_t)s * volumeGain >> 8;
}

void sendVolume() {
  String msg = "{\"type\":\"volume\",\"value\":" + String(volume) + "}";
  webSocket.sendTXT(msg);
}

// CPU load per core = share of time its idle task did NOT run since the last
// call (FreeRTOS run-time stats are enabled in the ESP32 Arduino core).
void cpuLoad(int out[2]) {
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

// Measured once in setup(): ESP.getSketchSize() re-checksums the whole
// program in flash (~1 MB) on every call, which froze audio when it ran
// every 2 s. The size can't change while running anyway.
uint32_t appUsed = 0, appTotal = 0;

void sendStats() {
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

  String out;
  serializeJson(doc, out);
  webSocket.sendTXT(out);
}

// ---- Flash layout: every partition, its size, and how much is used ----
// Built once in setup() (the layout can't change while running) and sent
// whenever the server connects or a page asks.
String partitionsJson;

// Mount a file-storage partition read-only for a moment to see how full it is
void fileStorageUsage(JsonObject o, const esp_partition_t *p) {
#ifdef MEASURE_FILE_STORAGE
  bool fat = p->subtype == ESP_PARTITION_SUBTYPE_DATA_FAT;
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

void buildPartitions() {
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

void i2sSetup() {
  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = true
  };
  i2s_pin_config_t pins = {
    .bck_io_num = I2S_BCLK_GPIO,
    .ws_io_num = I2S_LRC_GPIO,
    .data_out_num = I2S_DOUT_GPIO,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(I2S_PORT, &config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pins);
  i2s_zero_dma_buffer(I2S_PORT);
}

// Starts (or restarts, for looping) the HTTP download of the ringing tone
bool openTone() {
  http.end();
  toneBytesLeft = 0;
  http.begin("http://" + serverIp.toString() + ":" + String(SERVER_PORT) + "/tones/" + ringingTone);
  if (http.GET() != HTTP_CODE_OK) return false;
  toneStream = http.getStreamPtr();
  uint8_t header[44];  // standard WAV header, as written by the web page
  if (toneStream->readBytes(header, sizeof(header)) != sizeof(header)) return false;
  toneBytesLeft = http.getSize() - sizeof(header);
  return toneBytesLeft > 0;
}

void stopTone() {
  if (ringingTone.isEmpty()) return;
  http.end();
  ringingTone = "";
  i2s_zero_dma_buffer(I2S_PORT);
  Serial.println("Ringing stopped");
  webSocket.sendTXT("{\"type\":\"stopped\"}");
}

void stopTalk() {
  if (!talking) return;
  talking = false;
  talkCount = 0;
  i2s_zero_dma_buffer(I2S_PORT);
  Serial.println("Talk stopped");
  webSocket.sendTXT("{\"type\":\"talk_stopped\"}");
}

void startTalk() {
  stopTone();
  stopTalk();
  talking = true;
  talkPlaying = talkEnding = false;
  talkHead = talkCount = 0;
  Serial.println("Talk started");
  webSocket.sendTXT("{\"type\":\"talking\"}");
}

// Binary WebSocket chunk: little-endian 16-bit samples (read byte-wise, the payload may be unaligned)
void talkPush(const uint8_t *data, size_t len) {
  for (size_t i = 0; i + 1 < len; i += 2) {
    if (talkCount == TALK_BUF_SAMPLES) {  // full (browser clock slightly fast): drop oldest
      talkHead = (talkHead + 1) % TALK_BUF_SAMPLES;
      talkCount--;
    }
    talkBuf[(talkHead + talkCount) % TALK_BUF_SAMPLES] = (int16_t)(data[i] | (data[i + 1] << 8));
    talkCount++;
  }
}

// Same idea as pumpTone(): one small chunk per loop() so the WebSocket keeps being serviced
void pumpTalk() {
  if (!talking) return;
  if (!talkPlaying) {
    if (talkCount < TALK_PREBUFFER && !talkEnding) return;
    talkPlaying = true;
  }
  if (talkCount == 0) {
    if (talkEnding) stopTalk();                                        // played out the last words
    else if (millis() - talkLastPlayed > TALK_DRY_MS) talkPlaying = false;  // starved: buffer up again
    return;
  }

  int16_t stereo[512];
  size_t n = min(talkCount, (size_t)256);
  for (size_t i = 0; i < n; i++) {
    stereo[2 * i] = stereo[2 * i + 1] = applyVolume(talkBuf[talkHead]);
    talkHead = (talkHead + 1) % TALK_BUF_SAMPLES;
  }
  talkCount -= n;
  size_t written;
  i2s_write(I2S_PORT, stereo, n * 4, &written, portMAX_DELAY);
  talkLastPlayed = millis();
}

void startTone(const String &tone) {
  stopTalk();  // an alarm wins over live talk
  stopTone();
  ringingTone = tone;
  ringStart = millis();
  Serial.println("Ringing: " + tone);
  if (openTone()) webSocket.sendTXT("{\"type\":\"ringing\"}");
  else stopTone();
}

// Plays one small chunk per call so webSocket.loop() keeps running and
// "stop" works mid-ring. Loops the tone until stopped or RING_MAX_MS.
void pumpTone() {
  if (ringingTone.isEmpty()) return;
  if (millis() - ringStart > RING_MAX_MS) return stopTone();
  if (toneBytesLeft < 2) {  // end of file: play it again
    if (!openTone()) stopTone();
    return;
  }

  size_t avail = toneStream->available();
  if (avail < 2) {
    if (!http.connected()) stopTone();  // server went away mid-stream
    return;
  }

  int16_t mono[256], stereo[512];
  size_t want = min(min(avail, sizeof(mono)), (size_t)toneBytesLeft) & ~(size_t)1;
  int samples = toneStream->read((uint8_t *)mono, want) / 2;
  toneBytesLeft -= samples * 2;
  for (int i = 0; i < samples; i++) stereo[2 * i] = stereo[2 * i + 1] = applyVolume(mono[i]);
  size_t written;
  i2s_write(I2S_PORT, stereo, samples * 4, &written, portMAX_DELAY);
}

void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_CONNECTED) {
    Serial.println("Connected to server");
    String hello = "{\"type\":\"hello\",\"role\":\"device\",\"deviceId\":\"" + deviceId + "\"}";
    webSocket.sendTXT(hello);
    sendVolume();
    webSocket.sendTXT(partitionsJson);
  } else if (type == WStype_BIN) {
    if (talking) talkPush(payload, length);
  } else if (type == WStype_TEXT) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, length)) return;
    String cmd = doc["type"] | "";
    Serial.println("Command: " + cmd);

    if (cmd == "blink") blink();
    else if (cmd == "ring") startTone(doc["tone"] | "");
    else if (cmd == "stop") { stopTone(); stopTalk(); }
    else if (cmd == "talk_start") startTalk();
    else if (cmd == "talk_stop") talkEnding = true;  // finish what's buffered, then stop
    else if (cmd == "volume") {  // with "value": set it; without: just report it
      if (doc["value"].is<int>()) {
        setVolume(doc["value"].as<int>());
        prefs.putUChar("vol", volume);
      }
      sendVolume();
    }
    else if (cmd == "partitions") webSocket.sendTXT(partitionsJson);
    else if (cmd == "reset_wifi") { wm.resetSettings(); ESP.restart(); }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);  // GPIO 2 on ESP32, the RGB LED (GPIO 48) on the S3
  i2sSetup();
  prefs.begin("audio");
  setVolume(prefs.getUChar("vol", 70));
  appUsed = ESP.getSketchSize();
  appTotal = appUsed + ESP.getFreeSketchSpace();
  buildPartitions();

  // Factory MAC from eFuse, e.g. "esp32-246f28ae5278"
  uint64_t mac = ESP.getEfuseMac();
  char id[19];
  snprintf(id, sizeof(id), "esp32-%02x%02x%02x%02x%02x%02x",
           (uint8_t)mac, (uint8_t)(mac >> 8), (uint8_t)(mac >> 16),
           (uint8_t)(mac >> 24), (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
  deviceId = id;
  Serial.println("Device ID: " + deviceId);

  // No saved Wi-Fi (or it's unreachable)? Opens hotspot "LED-Setup-xxxx":
  // join it from a phone, pick your network, enter the password.
  String apName = "LED-Setup-" + deviceId.substring(deviceId.length() - 4);
  if (!wm.autoConnect(apName.c_str())) ESP.restart();
  Serial.println("Wi-Fi connected, IP: " + WiFi.localIP().toString());

  WiFi.setSleep(false);  // power-save mode drops mDNS replies
  MDNS.begin(deviceId.c_str());
  while ((serverIp = MDNS.queryHost(SERVER_MDNS_NAME)) == IPAddress(0, 0, 0, 0)) {
    Serial.println("Looking for " SERVER_MDNS_NAME ".local ...");
    delay(1000);
  }
  Serial.println("Server found at " + serverIp.toString());

  webSocket.begin(serverIp.toString(), SERVER_PORT, "/");
  webSocket.onEvent(onWsEvent);
  webSocket.setReconnectInterval(3000);
}

void loop() {
  webSocket.loop();
  pumpTone();
  pumpTalk();

  static unsigned long lastStats = 0;
  if (webSocket.isConnected() && millis() - lastStats >= STATS_INTERVAL_MS) {
    lastStats = millis();
    sendStats();
  }

  // Let core 1's idle task run, so CPU load reads true (and the chip runs cooler).
  // Safe for audio: each pass moves 16 ms of sound and the I2S queue holds 128 ms.
  delay(1);
}
