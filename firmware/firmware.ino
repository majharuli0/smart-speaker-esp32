// ESP32 smart speaker firmware. Startup, the main loop, and what each
// command from the server does. The work lives in the modules:
//   net    - device ID, Wi-Fi setup, the MQTT link to the broker (EMQX)
//   audio  - speaker, volume, ringing tones, live talk
//   stats  - RAM/CPU/Wi-Fi stats and the flash layout for the web page
//   timesync - real time from NTP, in the device's time zone
//   alarms - alarms stored on the device, rung from its own clock
//   tonecache - the tones those alarms use, stored on the device
//   ota    - firmware updates over Wi-Fi, with automatic rollback
//   health - restart reason, crash reports, and a watchdog against freezes
// Messages are documented in docs/protocol.md.
#include <ArduinoJson.h>
#include "config.h"
#include "net.h"
#include "audio.h"
#include "stats.h"
#include "timesync.h"
#include "alarms.h"
#include "tonecache.h"
#include "ota.h"
#include "health.h"

static void blink() {
  digitalWrite(LED_BUILTIN, HIGH);
  delay(300);
  digitalWrite(LED_BUILTIN, LOW);
}

static void onConnected() {
  netSend("{\"type\":\"hello\",\"role\":\"device\",\"deviceId\":\"" + deviceId + "\",\"fw\":\"" FW_VERSION "\",\"board\":\"" CONFIG_IDF_TARGET "\"}");
  sendVolume();
  sendPartitions();
  otaReport();
  healthReport();
}

static void onCommand(uint8_t *payload, size_t length) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) return;
  String cmd = doc["type"] | "";
  Serial.println("Command: " + cmd);

  if (cmd == "blink") blink();
  else if (cmd == "ring") toneStart(doc["tone"] | "");
  else if (cmd == "stop") { toneStop(); talkStop(); }
  else if (cmd == "snooze") alarmsSnooze();
  else if (cmd == "talk_start") talkStart();
  else if (cmd == "talk_stop") talkFinish();  // finish what's buffered, then stop
  else if (cmd == "volume") {                 // with "value": set it; without: just report it
    if (doc["value"].is<int>()) setVolume(doc["value"].as<int>());
    sendVolume();
  }
  else if (cmd == "partitions") sendPartitions();
  else if (cmd == "watch") setWatching(doc["on"] | false);
  else if (cmd == "alarms_sync") { alarmsSync(doc); cacheSync(doc["tones"]); }
  else if (cmd == "timezone") setTimezone(doc["tz"] | "", doc["name"] | "");
  else if (cmd == "doorbell") chimeStart();
  else if (cmd == "ota_start") otaStart(doc);
  else if (cmd == "reset_wifi") resetWifi();
}

static void onAudio(uint8_t *payload, size_t length) { talkPush(payload, length); }

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);  // ESP32 core error messages to this (USB) port too
  healthSetup();
  otaSetup();
  pinMode(LED_BUILTIN, OUTPUT);  // the board's RGB LED (GPIO 48)
  audioSetup();
  cacheSetup();  // before statsSetup, which measures the storage it mounts
  statsSetup();
  alarmsSetup();
  netSetup({onConnected, onCommand, onAudio});
  timeSetup();
  healthWatchdog();  // last: setup may block (formatting storage on first boot)
}

void loop() {
  netLoop();
  audioLoop();
  statsLoop();
  timeLoop();
  alarmsLoop();
  cacheLoop();
  otaLoop();

  // Let core 1's idle task run, so CPU load reads true (and the chip runs cooler).
  // Safe for audio: each pass moves 16 ms of sound and the I2S queue holds 128 ms.
  delay(1);
}
