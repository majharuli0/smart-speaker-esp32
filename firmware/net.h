// Network: device ID, Wi-Fi setup (over Bluetooth), and the MQTT link to the broker (EMQX).
#pragma once
#include <Arduino.h>

extern String deviceId;   // "esp32-" + factory MAC, e.g. "esp32-2884856409fc"
extern String devicePop;  // the code on its QR label "SS:<UID>:<pop>": proves you have it when adding it remotely

// Called from netLoop() (the main loop, never the MQTT task); set before netSetup()
struct NetHandlers {
  void (*connected)();                          // the server is reachable: say hello
  void (*text)(uint8_t *data, size_t length);   // JSON command (ss/dev/<id>/cmd)
  void (*binary)(uint8_t *data, size_t length); // talk audio (ss/dev/<id>/audio)
};

// Returns at once. No Wi-Fi saved: starts Bluetooth setup; netLoop() connects
// and reconnects in the background.
void netSetup(const NetHandlers &handlers);
void netLoop();
bool netConnected();                 // broker connected and the server online
bool netInSetup();                   // waiting for Wi-Fi details over Bluetooth
String netTakeClaim();               // the setup token the app sent over Bluetooth, once ("" if none)
void netSend(const String &json);    // to the server (ss/dev/<id>/evt)
void netSend(const char *json);
// Live data, sent at once from the calling task (QoS 0, never queued): false if it
// couldn't go. netSend's queue goes out about one message per pass of the MQTT
// task: fine for reports, far too slow for 8 audio pieces a second.
bool netSendMic(const uint8_t *pcm, size_t length);  // microphone audio (the audio link, or ss/dev/<id>/mic)
bool netSendNow(const char *json);                   // a live reading (ss/dev/<id>/evt)
String serverHttp();                 // base URL for downloads, e.g. "http://192.168.0.102:3000", "" if unknown
String netPassword();                // this device's broker password (the audio link logs in with it too)
// The audio link's incoming frames, handled on the main loop like MQTT messages
void netDeliverCommand(const uint8_t *json, size_t length);
void netDeliverAudio(const uint8_t *pcm, size_t length);
void resetWifi();                    // forget Wi-Fi and restart into Bluetooth setup
