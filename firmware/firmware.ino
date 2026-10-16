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
//   led    - the RGB status light
//   mic    - the microphone, always on: sessions streamed to the server (test,
//            voice requests, the room during a call); the rest goes to wake
//   wake   - the wake word, matched on the speaker (wakecore: the matching itself)
//   audiolink - live audio over its own WebSocket (MQTT carries it while that's down)
// Messages are documented in docs/protocol.md.
#include <ArduinoJson.h>
#include "config.h"
#include "net.h"
#include "audio.h"
#include "stats.h"
#include "timesync.h"
#include "alarms.h"
#include "tonecache.h"
#include "files.h"
#include "ota.h"
#include "health.h"
#include "led.h"
#include "mic.h"
#include "wake.h"
#include "audiolink.h"

static void onConnected() {
  netSend("{\"type\":\"hello\",\"role\":\"device\",\"deviceId\":\"" + deviceId + "\",\"fw\":\"" FW_VERSION "\",\"board\":\"" CONFIG_IDF_TARGET "\"}");
  String claim = netTakeClaim();  // set up from the app over Bluetooth: add it to that account
  if (claim.length()) netSend("{\"type\":\"claim\",\"token\":\"" + claim + "\"}");
  sendVolume();
  sendRepeat();
  sendPartitions();
  otaReport();
  healthReport();
  wakeReport();
}

static void onCommand(uint8_t *payload, size_t length) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) return;
  String cmd = doc["type"] | "";
  Serial.println("Command: " + cmd);

  if (cmd == "blink") ledIdentify();
  else if (cmd == "ring") toneStart(doc["tone"] | "");
  else if (cmd == "stop") { toneStop(); talkStop(); micStop(); }
  else if (cmd == "snooze") alarmsSnooze();
  else if (cmd == "talk_start") talkStart();
  else if (cmd == "talk_stop") talkFinish();  // finish what's buffered, then stop
  else if (cmd == "volume") {                 // with "value": set it; without: just report it
    if (doc["value"].is<int>()) setVolume(doc["value"].as<int>());
    sendVolume();
  }
  else if (cmd == "repeat") {                 // with "on": set it; without: just report it
    if (doc["on"].is<bool>()) setRepeat(doc["on"].as<bool>());
    sendRepeat();
  }
  else if (cmd == "partitions") sendPartitions();
  else if (cmd == "watch") setWatching(doc["on"] | false);
  else if (cmd == "alarms_sync") { alarmsSync(doc); cacheSync(doc["tones"]); }
  else if (cmd == "timezone") setTimezone(doc["tz"] | "", doc["name"] | "");
  else if (cmd == "doorbell") chimeStart();
  else if (cmd == "ota_start") otaStart(doc);
  else if (cmd == "reset_wifi") resetWifi();
  else if (cmd.startsWith("files_")) filesCommand(doc);
  else if (cmd == "mic_record")
    micRecord({doc["seconds"] | 5, doc["endSilence"] | 0, doc["live"] | false, doc["wakeTake"] | 0,
               doc["wakeTest"] | false, doc["afterWake"] | false, doc["conversation"] | false,
               doc["idle"] | 0, doc["echoTest"] | false, doc["aec"] | false});
  else if (cmd == "mic_stop") micStop(doc["quiet"] | false);
  else if (cmd == "wake_config") wakeConfig(doc["enabled"] | false, doc["sensitivity"] | 5);
  else if (cmd == "wake_clear") wakeClear();
}

static void onAudio(uint8_t *payload, size_t length) { talkPush(payload, length); }

void setup() {
  Serial.begin(115200);
  // USB plugged into a PC with no serial monitor reading: by default each print
  // waits up to 2 s for room (core messages 100 ms per character), enough to
  // trip the 5 s loop watchdog ("froze" ~12 s after every upload). 1 ms per try
  // still keeps up with a monitor that is reading.
  Serial.setTxTimeoutMs(1);
  Serial.setDebugOutput(true);  // ESP32 core error messages to this (USB) port too
  healthSetup();
  otaSetup();
  ledSetup();
  audioSetup();
  cacheSetup();  // before statsSetup, which measures the storage it mounts
  wakeSetup();   // its takes are in that storage
  statsSetup();
  alarmsSetup();
  micSetup();
  netSetup({onConnected, onCommand, onAudio});
  audioLinkSetup();
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
  filesLoop();
  otaLoop();
  wakeLoop();
  micLoop();

  // Let core 1's idle task run, so CPU load reads true (and the chip runs cooler).
  // Safe for audio: each pass moves 16 ms of sound and the I2S queue holds 128 ms.
  delay(1);
}
