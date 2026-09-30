// ESP32 smart speaker firmware. Startup, the main loop, and what each
// command from the server does. The work lives in the modules:
//   net    - device ID, Wi-Fi setup, finding the server, WebSocket
//   audio  - speaker, volume, ringing tones, live talk
//   stats  - RAM/CPU/Wi-Fi stats and the flash layout for the web page
//   timesync - real time from NTP, in the device's time zone
// Messages are documented in docs/protocol.md.
#include <ArduinoJson.h>
#include "config.h"
#include "net.h"
#include "audio.h"
#include "stats.h"
#include "timesync.h"

static void blink() {
  digitalWrite(LED_BUILTIN, HIGH);
  delay(300);
  digitalWrite(LED_BUILTIN, LOW);
}

static void onConnected() {
  netSend("{\"type\":\"hello\",\"role\":\"device\",\"deviceId\":\"" + deviceId + "\",\"fw\":\"" FW_VERSION "\"}");
  sendVolume();
  sendPartitions();
}

static void onCommand(uint8_t *payload, size_t length) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) return;
  String cmd = doc["type"] | "";
  Serial.println("Command: " + cmd);

  if (cmd == "blink") blink();
  else if (cmd == "ring") toneStart(doc["tone"] | "");
  else if (cmd == "stop") { toneStop(); talkStop(); }
  else if (cmd == "talk_start") talkStart();
  else if (cmd == "talk_stop") talkFinish();  // finish what's buffered, then stop
  else if (cmd == "volume") {                 // with "value": set it; without: just report it
    if (doc["value"].is<int>()) setVolume(doc["value"].as<int>());
    sendVolume();
  }
  else if (cmd == "partitions") sendPartitions();
  else if (cmd == "timezone") setTimezone(doc["tz"] | "", doc["name"] | "");
  else if (cmd == "reset_wifi") resetWifi();
}

static void onAudio(uint8_t *payload, size_t length) { talkPush(payload, length); }

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);  // GPIO 2 on ESP32, the RGB LED (GPIO 48) on the S3
  audioSetup();
  statsSetup();
  netSetup({onConnected, onCommand, onAudio});
  timeSetup();
}

void loop() {
  netLoop();
  audioLoop();
  statsLoop();

  // Let core 1's idle task run, so CPU load reads true (and the chip runs cooler).
  // Safe for audio: each pass moves 16 ms of sound and the I2S queue holds 128 ms.
  delay(1);
}
