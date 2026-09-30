// Network: device ID, Wi-Fi setup, finding the server, and the WebSocket link.
#pragma once
#include <Arduino.h>
#include <IPAddress.h>

extern String deviceId;  // "esp32-" + factory MAC, e.g. "esp32-2884856409fc"

// Called from the WebSocket; set before netSetup()
struct NetHandlers {
  void (*connected)();                          // link to the server is up
  void (*text)(uint8_t *data, size_t length);   // JSON command
  void (*binary)(uint8_t *data, size_t length); // talk audio
};

// Only blocks on first boot (no Wi-Fi saved yet: runs the setup hotspot).
// Otherwise returns at once; netLoop() connects and reconnects in the background.
void netSetup(const NetHandlers &handlers);
void netLoop();
bool netConnected();
void netSend(const String &json);
void netSend(const char *json);
IPAddress serverAddress();
void resetWifi();  // forget Wi-Fi and restart into the setup hotspot
