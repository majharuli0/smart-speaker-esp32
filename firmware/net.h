// Network: device ID, Wi-Fi setup, and the MQTT link to the broker (EMQX).
#pragma once
#include <Arduino.h>

extern String deviceId;  // "esp32-" + factory MAC, e.g. "esp32-2884856409fc"

// Called from netLoop() (the main loop, never the MQTT task); set before netSetup()
struct NetHandlers {
  void (*connected)();                          // the server is reachable: say hello
  void (*text)(uint8_t *data, size_t length);   // JSON command (ss/dev/<id>/cmd)
  void (*binary)(uint8_t *data, size_t length); // talk audio (ss/dev/<id>/audio)
};

// Only blocks on first boot (no Wi-Fi saved yet: runs the setup hotspot).
// Otherwise returns at once; netLoop() connects and reconnects in the background.
void netSetup(const NetHandlers &handlers);
void netLoop();
bool netConnected();                 // broker connected and the server online
void netSend(const String &json);    // to the server (ss/dev/<id>/evt)
void netSend(const char *json);
String serverHttp();                 // base URL for downloads, e.g. "http://192.168.0.102:3000", "" if unknown
void resetWifi();                    // forget Wi-Fi and restart into the setup hotspot
