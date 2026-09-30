#include "net.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <WebSocketsClient.h>
#include "config.h"

String deviceId;

static WebSocketsClient webSocket;
static WiFiManager wm;
static IPAddress serverIp;
static NetHandlers handlers;

static void makeDeviceId() {
  // Factory MAC from eFuse, e.g. "esp32-246f28ae5278"
  uint64_t mac = ESP.getEfuseMac();
  char id[19];
  snprintf(id, sizeof(id), "esp32-%02x%02x%02x%02x%02x%02x",
           (uint8_t)mac, (uint8_t)(mac >> 8), (uint8_t)(mac >> 16),
           (uint8_t)(mac >> 24), (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
  deviceId = id;
}

static void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_CONNECTED) {
    Serial.println("Connected to server");
    if (handlers.connected) handlers.connected();
  } else if (type == WStype_TEXT) {
    if (handlers.text) handlers.text(payload, length);
  } else if (type == WStype_BIN) {
    if (handlers.binary) handlers.binary(payload, length);
  }
}

void netSetup(const NetHandlers &h) {
  handlers = h;
  makeDeviceId();
  Serial.println("Device ID: " + deviceId + "  firmware " FW_VERSION);

  // No saved Wi-Fi (or it's unreachable)? Opens hotspot "LED-Setup-xxxx":
  // join it from a phone, pick your network, enter the password.
  String apName = SETUP_AP_PREFIX + deviceId.substring(deviceId.length() - 4);
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

void netLoop() { webSocket.loop(); }
bool netConnected() { return webSocket.isConnected(); }
IPAddress serverAddress() { return serverIp; }

void netSend(const String &json) { webSocket.sendTXT(json.c_str(), json.length()); }

void netSend(const char *json) { webSocket.sendTXT(json); }

void resetWifi() {
  wm.resetSettings();
  ESP.restart();
}
