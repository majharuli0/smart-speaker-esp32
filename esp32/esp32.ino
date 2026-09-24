#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <WebSocketsClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <SdFat.h>
#include <Adafruit_SPIFlash.h>
#include "driver/i2s.h"
#include <math.h>

// esp_read_mac()/ESP_MAC_WIFI_STA moved from esp_system.h to esp_mac.h in
// newer ESP32 cores; guarded so this compiles against either.
#if __has_include("esp_mac.h")
#include "esp_mac.h"
#else
#include "esp_system.h"
#endif

#define LED_GPIO 2
#define SERVER_MDNS_NAME "led-server"  // resolves led-server.local, advertised by server.js
#define WS_PORT 3000
#define RESET_BUTTON_GPIO 0        // BOOT button
#define RESET_HOLD_MS 10000        // hold 10s to force a Wi-Fi reset

// MAX98357A I2S amp wiring (current build — see docs/audio-alarm-design.md
// for where this is headed). GAIN and SD are left floating on the amp
// board: default gain, amp not shut down.
// Confirmed by physically tracing each wire (two earlier guesses from
// photos were both wrong) — don't re-derive this from a diagram again.
#define I2S_DOUT_GPIO 33   // amp DIN  (audio data)
#define I2S_BCLK_GPIO 25   // amp BCLK (bit clock)
#define I2S_LRC_GPIO  32   // amp LRC  (word/L-R select)
#define I2S_PORT I2S_NUM_0
#define I2S_SAMPLE_RATE 16000

// W25Q64JV SPI flash (8MB), on the ESP32's hardware VSPI bus. VSPI's
// default pins (SCK=18, MISO=19, MOSI=23) already match the physical
// wiring, so the global `SPI` object needs no custom SPI.begin() pins —
// only chip-select is board-specific.
#define FLASH_CS_GPIO 5

WebSocketsClient webSocket;
WiFiManager wm;
String deviceId;

Adafruit_FlashTransport_SPI flashTransport(FLASH_CS_GPIO, SPI);
Adafruit_SPIFlash flash(&flashTransport);
FatVolume fatfs;
bool flashReady = false;

static void flashLed() {
  digitalWrite(LED_GPIO, HIGH);
  delay(300);
  digitalWrite(LED_GPIO, LOW);
}

static void i2sSetup() {
  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = I2S_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
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

// Fresh W25Q64JV chips (or ones formatted with something other than
// FAT12/16) won't mount — that's a one-time setup step, not a wiring bug.
// See esp32/format_flash/format_flash.ino: flash it once, format, then come
// back to this sketch.
static void flashSetup() {
  // On a generic ESP32 dev board (not one of Adafruit's own, which this
  // library was primarily built for), the VSPI bus needs to be explicitly
  // started before the flash transport touches it — otherwise
  // Adafruit_SPIFlash ends up dereferencing an uninitialized internal SPI
  // handle, which crashes with exactly a LoadProhibited/null-pointer panic.
  SPI.begin();
  if (!flash.begin()) {
    Serial.println("Error initializing W25Q64 SPI flash chip (check wiring/CS pin).");
    return;
  }

  // flash.begin() auto-speeds the SPI clock up to this chip's rated
  // maximum (133MHz for the W25Q64JV) -- far beyond what breadboard
  // jumper wires can reliably carry. Confirmed on the bench: every bulk
  // read/write after begin() silently returned garbage (all zeros) at
  // that speed while still reporting success, traced by dumping the
  // Adafruit_SPIFlash cache's actual buffer contents in format_flash.ino.
  // Forcing it back down to a speed the wiring can sustain.
  flashTransport.setClockSpeed(1000000, 1000000);

  if (!fatfs.begin(&flash)) {
    Serial.println("Error mounting FAT filesystem on flash — it may need formatting once "
                    "(see esp32/format_flash/format_flash.ino).");
    return;
  }
  flashReady = true;
  Serial.println("External SPI flash mounted.");
}

static void sendJson(JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  webSocket.sendTXT(out);
}

static void sendSyncResult(const String &id, bool ok, const String &reason = "") {
  StaticJsonDocument<256> doc;
  doc["type"] = "sync_result";
  doc["id"] = id;
  doc["ok"] = ok;
  if (reason.length()) doc["reason"] = reason;
  sendJson(doc);
}

static void sendPlayFailed(const String &id, const String &reason) {
  StaticJsonDocument<192> doc;
  doc["type"] = "play_failed";
  doc["id"] = id;
  doc["reason"] = reason;
  sendJson(doc);
}

// Full reconciliation of what's actually on the flash chip — sent after
// every mount/sync/delete so the server's manifest never drifts from
// reality (the device is the source of truth, never the server's memory
// of what it last heard).
static void sendFsReport() {
  StaticJsonDocument<2048> doc;
  doc["type"] = "fs_report";
  doc["present"] = flashReady;
  JsonArray files = doc.createNestedArray("files");

  if (flashReady) {
    File32 root = fatfs.open("/");
    File32 entry = root.openNextFile();
    while (entry) {
      if (!entry.isDirectory()) {
        char name[64];
        entry.getName(name, sizeof(name));
        String n(name);
        if (!n.startsWith(".tmp_")) {  // half-downloaded files aren't real sounds yet
          JsonObject o = files.createNestedObject();
          o["id"] = n;
          o["size"] = entry.size();
        }
      }
      entry.close();
      entry = root.openNextFile();
    }
    root.close();
  }
  sendJson(doc);
}

// Streams a sound straight from flash to the I2S DMA buffer. Assumes a
// standard 44-byte PCM WAV header, which is skipped. Blocking, same
// tradeoff flashLed() already makes (see README) — fine for a single demo
// device, would need a background task if multiple long sounds and
// mid-playback "stop" both matter later.
static void playWavFromFlash(const String &id) {
  if (!flashReady) {
    sendPlayFailed(id, "no_flash");
    return;
  }
  String path = "/" + id;
  File32 audioFile = fatfs.open(path.c_str(), O_READ);
  if (!audioFile) {
    Serial.printf("Failed to open %s\n", path.c_str());
    sendPlayFailed(id, "file_missing");
    return;
  }

  Serial.printf("Playing %s from flash\n", path.c_str());
  audioFile.seek(44);  // skip the WAV header

  const int bufSize = 512;
  uint8_t buf[bufSize];
  while (audioFile.available()) {
    int n = audioFile.read(buf, bufSize);
    if (n <= 0) break;
    size_t bytesWritten;
    i2s_write(I2S_PORT, buf, n, &bytesWritten, portMAX_DELAY);
  }
  audioFile.close();
  i2s_zero_dma_buffer(I2S_PORT);
  Serial.println("Playback complete");
}

// Hardcoded placeholder "sound" — a plain 1kHz tone, synthesized on the
// fly, no flash file involved. Kept as the fallback for a `play_sound`
// with no `id` (e.g. manual testing), same role it had before flash
// storage existed: proving the amp+speaker chain works on its own.
static void playTestTone() {
  const float freqHz = 1000.0f;
  const int durationMs = 500;
  const int totalSamples = I2S_SAMPLE_RATE * durationMs / 1000;
  const int16_t amplitude = 6000; // headroom under full-scale (32767)

  Serial.println("Playing test tone");
  int16_t buf[128 * 2]; // stereo interleaved, L=R (mono duplicated)
  int samplesDone = 0;
  while (samplesDone < totalSamples) {
    int chunk = min(128, totalSamples - samplesDone);
    for (int i = 0; i < chunk; i++) {
      int16_t sample = (int16_t)(amplitude * sinf(2.0f * PI * freqHz * (samplesDone + i) / I2S_SAMPLE_RATE));
      buf[i * 2] = sample;
      buf[i * 2 + 1] = sample;
    }
    size_t bytesWritten;
    i2s_write(I2S_PORT, buf, chunk * 2 * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
    samplesDone += chunk;
  }
}

static void deleteSoundById(const String &id) {
  if (!flashReady) return;
  fatfs.remove(("/" + id).c_str());
  sendFsReport();
}

// Downloads a sound from the backend server and saves it to flash.
// Temp-name-then-atomic-rename: a half-downloaded or corrupted file can
// never be mistaken for a valid one, even across a power loss mid-download.
static void handleSyncFile(const String &id, const String &url, size_t expectedSize) {
  if (!flashReady) {
    sendSyncResult(id, false, "no_flash");
    return;
  }
  Serial.printf("Syncing %s from %s (expecting %u bytes)\n", id.c_str(), url.c_str(), (unsigned)expectedSize);

  String tmpPath = "/.tmp_" + id;
  String finalPath = "/" + id;
  fatfs.remove(tmpPath.c_str());  // clear out any stale temp file from a previous failed attempt

  File32 f = fatfs.open(tmpPath.c_str(), O_WRITE | O_CREAT | O_TRUNC);
  if (!f) {
    Serial.println("Failed to open temp file for writing");
    sendSyncResult(id, false, "flash_write_error");
    return;
  }

  HTTPClient http;
  http.begin(url);
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("Download failed, HTTP %d\n", httpCode);
    f.close();
    fatfs.remove(tmpPath.c_str());
    http.end();
    sendSyncResult(id, false, "download_failed");
    return;
  }

  WiFiClient *stream = http.getStreamPtr();
  size_t written = 0;
  uint8_t buf[512];
  unsigned long lastByte = millis();
  while (http.connected() && written < expectedSize) {
    size_t avail = stream->available();
    if (avail) {
      int n = stream->readBytes(buf, min(avail, sizeof(buf)));
      f.write(buf, n);
      written += n;
      lastByte = millis();
    } else if (millis() - lastByte > 10000) {
      Serial.println("Download stalled, giving up");
      break;
    } else {
      delay(1);
    }
  }
  f.close();
  http.end();

  bool ok = (written == expectedSize);
  if (ok) {
    fatfs.remove(finalPath.c_str());  // overwriting an existing sound with the same id
    fatfs.rename(tmpPath.c_str(), finalPath.c_str());
    Serial.printf("Synced %s (%u bytes)\n", id.c_str(), (unsigned)written);
  } else {
    Serial.printf("Size mismatch on %s: got %u, expected %u\n", id.c_str(), (unsigned)written, (unsigned)expectedSize);
    fatfs.remove(tmpPath.c_str());
  }
  sendSyncResult(id, ok, ok ? "" : "size_mismatch");
  sendFsReport();
}

static void resetWifiAndRestart() {
  Serial.println("Forgetting Wi-Fi, restarting into setup portal...");
  wm.resetSettings();
  delay(500);
  ESP.restart();
}

static void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      Serial.println("Connected to server");
      String hello = "{\"type\":\"hello\",\"role\":\"device\",\"deviceId\":\"" + deviceId + "\"}";
      webSocket.sendTXT(hello);
      sendFsReport();  // reconcile the server's manifest with what's really on flash
      break;
    }
    case WStype_DISCONNECTED:
      Serial.println("WS disconnected");
      break;
    case WStype_TEXT: {
      String msg((char *)payload, length);
      Serial.printf("Received: %s\n", msg.c_str());

      StaticJsonDocument<512> doc;
      DeserializationError err = deserializeJson(doc, msg);
      if (err) {
        Serial.println("Bad JSON, ignoring");
        break;
      }

      const char *msgType = doc["type"] | "";
      const char *target = doc["target"] | "";
      // A command with no "target" field is legacy/broadcast; otherwise it
      // must name this device specifically.
      bool forThisDevice = (strlen(target) == 0) || (deviceId == target);
      if (!forThisDevice) break;

      if (strcmp(msgType, "reset_wifi") == 0) {
        resetWifiAndRestart();
      } else if (strcmp(msgType, "flash") == 0) {
        Serial.println("Received FLASH");
        flashLed();
      } else if (strcmp(msgType, "play_sound") == 0) {
        const char *id = doc["id"] | "";
        if (strlen(id) > 0) playWavFromFlash(String(id));
        else playTestTone();  // no id given: manual test-tone path
      } else if (strcmp(msgType, "stop_sound") == 0) {
        // Playback is blocking (see playWavFromFlash), so there's no
        // running playback to interrupt by the time this is processed —
        // noted as a known limitation, not silently ignored.
        Serial.println("stop_sound received (playback isn't interruptible mid-stream yet)");
      } else if (strcmp(msgType, "delete_sound") == 0) {
        const char *id = doc["id"] | "";
        if (strlen(id) > 0) deleteSoundById(String(id));
      } else if (strcmp(msgType, "sync_file") == 0) {
        const char *id = doc["id"] | "";
        const char *url = doc["url"] | "";
        long expectedSize = doc["expectedSize"] | 0;
        if (strlen(id) > 0 && strlen(url) > 0) handleSyncFile(String(id), String(url), (size_t)expectedSize);
      }
      break;
    }
    default:
      break;
  }
}

static IPAddress resolveServer() {
  Serial.printf("Looking for %s.local on the network...\n", SERVER_MDNS_NAME);
  IPAddress ip;
  for (int attempt = 0; attempt < 15; attempt++) {
    ip = MDNS.queryHost(SERVER_MDNS_NAME);
    if (ip != IPAddress(0, 0, 0, 0)) return ip;
    Serial.println("  not found yet, retrying...");
    delay(1000);
  }
  return IPAddress(0, 0, 0, 0);
}

static void checkResetButton() {
  static unsigned long pressStart = 0;
  if (digitalRead(RESET_BUTTON_GPIO) == LOW) {
    if (pressStart == 0) {
      pressStart = millis();
    } else if (millis() - pressStart >= RESET_HOLD_MS) {
      resetWifiAndRestart();
    }
  } else {
    pressStart = 0;
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(LED_GPIO, OUTPUT);
  digitalWrite(LED_GPIO, LOW);
  pinMode(RESET_BUTTON_GPIO, INPUT_PULLUP);

  i2sSetup();
  flashSetup();

  // WiFi.macAddress() can read back all zeros depending on exactly when
  // it's called relative to the Wi-Fi driver's init state. Reading the
  // factory-burned MAC straight from eFuse instead sidesteps that
  // entirely — it doesn't depend on the Wi-Fi driver being started at all.
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char macStr[13];
  snprintf(macStr, sizeof(macStr), "%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  // Computed early so it can be shown on the setup portal page itself, not
  // just printed to a serial console nobody but us can see during onboarding.
  // Already lowercase (matches the server, which lowercases every deviceId
  // it handles — pairing input, claims, relayed "target" fields).
  deviceId = "esp32-" + String(macStr);
  Serial.print("Device ID: ");
  Serial.println(deviceId);

  // No SSID/password in code: on first boot (or if the saved network is
  // unreachable) this opens a "LED-Demo-Setup" access point with a web
  // form where you pick your Wi-Fi and enter the password. It's saved to
  // flash, so this only happens again if the network changes (or you use
  // the "Reconfigure Wi-Fi" button in the frontend).
  String idHtml = "<p><b>Device ID (enter this in the app to pair):</b><br>" + deviceId + "</p>";
  WiFiManagerParameter idDisplay(idHtml.c_str());
  wm.addParameter(&idDisplay);

  if (!wm.autoConnect("LED-Demo-Setup")) {
    Serial.println("Wi-Fi setup failed/timed out, restarting");
    ESP.restart();
  }
  Serial.print("WiFi ready, IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("Device ID: ");
  Serial.println(deviceId);
  Serial.println("Hold the BOOT button 10s at any time to forget Wi-Fi and reopen setup.");

  // Wi-Fi power-save mode makes the radio sleep between beacons, which
  // often drops incoming multicast frames (mDNS responses included) even
  // though ordinary unicast traffic keeps working fine. Disable it so
  // mDNS resolves reliably.
  WiFi.setSleep(false);

  if (!MDNS.begin("esp32-led")) {
    Serial.println("mDNS init failed");
  }

  IPAddress serverIp = resolveServer();
  if (serverIp == IPAddress(0, 0, 0, 0)) {
    Serial.println("Could not resolve server via mDNS, restarting to retry");
    ESP.restart();
  }
  Serial.print("Found server at: ");
  Serial.println(serverIp);

  webSocket.begin(serverIp.toString().c_str(), WS_PORT, "/");
  webSocket.onEvent(onWsEvent);
  webSocket.setReconnectInterval(3000);
}

void loop() {
  webSocket.loop();
  checkResetButton();
}
