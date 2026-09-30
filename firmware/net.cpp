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

  WiFi.mode(WIFI_STA);
  if (!wm.getWiFiIsSaved()) {
    // First boot: nothing to do until Wi-Fi is set up, so wait in hotspot
    // "LED-Setup-xxxx": join it from a phone, pick your network, enter the password.
    String apName = SETUP_AP_PREFIX + deviceId.substring(deviceId.length() - 4);
    if (!wm.autoConnect(apName.c_str())) ESP.restart();
  } else {
    // Saved Wi-Fi: connect in the background and never block. If the router is
    // down (e.g. still booting after a power cut) the device keeps running and
    // alarms keep ringing; the Wi-Fi driver reconnects by itself when it's back.
    WiFi.setAutoReconnect(true);
    WiFi.begin();
  }
  WiFi.setSleep(false);  // power-save mode drops mDNS replies
  webSocket.onEvent(onWsEvent);
}

// Wi-Fi up → find the server by mDNS (retrying every 5 s) → keep the WebSocket open
void netLoop() {
  static bool wifiWasUp = false, mdnsStarted = false, wsStarted = false;
  static unsigned long lastLookup = 0;

  bool wifiUp = WiFi.status() == WL_CONNECTED;
  if (wifiUp != wifiWasUp) {
    wifiWasUp = wifiUp;
    Serial.println(wifiUp ? "Wi-Fi connected, IP: " + WiFi.localIP().toString() : String("Wi-Fi lost, reconnecting..."));
  }
  if (!wifiUp) return;
  if (!mdnsStarted) mdnsStarted = MDNS.begin(deviceId.c_str());

  if (!wsStarted) {
    if (lastLookup && millis() - lastLookup < 5000) return;
    lastLookup = millis();
    IPAddress ip = MDNS.queryHost(SERVER_MDNS_NAME, 1000);  // short timeout: audio keeps playing
    if (ip == IPAddress(0, 0, 0, 0)) {
      Serial.println("Looking for " SERVER_MDNS_NAME ".local ...");
      return;
    }
    serverIp = ip;
    Serial.println("Server found at " + serverIp.toString());
    webSocket.begin(serverIp.toString(), SERVER_PORT, "/");
    webSocket.setReconnectInterval(3000);
    wsStarted = true;
  }
  webSocket.loop();
}
bool netConnected() { return webSocket.isConnected(); }
IPAddress serverAddress() { return serverIp; }

void netSend(const String &json) { webSocket.sendTXT(json.c_str(), json.length()); }

void netSend(const char *json) { webSocket.sendTXT(json); }

void resetWifi() {
  wm.resetSettings();
  ESP.restart();
}
