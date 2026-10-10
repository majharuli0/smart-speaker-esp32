#include "net.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "config.h"

// Topics (see docs/protocol.md):
//   ss/dev/<id>/cmd     ← commands         ss/dev/<id>/evt     → replies and status
//   ss/dev/<id>/audio   ← talk audio       ss/dev/<id>/online  → "1" retained, "0" last will
//   ss/server/online    ← "1" when the server is up: say hello again
//   ss/server/http      ← base URL for downloads

String deviceId;

static WiFiManager wm;
static NetHandlers handlers;
static esp_mqtt_client_handle_t mqtt = nullptr;
static String topicCmd, topicAudio, topicEvt, topicOnline;
static volatile bool brokerUp = false;  // set on the MQTT task
static bool serverUp = false;            // main loop only
static String httpBase;

// Messages arrive on the MQTT client's own task; they're queued and handled in
// netLoop() on the main loop, so no other code ever runs on two tasks at once.
enum Kind : uint8_t { CMD, AUDIO, SERVER_ONLINE, SERVER_HTTP, BROKER_DOWN };
struct Inbox {
  Kind kind;
  size_t length;
  uint8_t *data;  // malloc'd, freed after handling
};
static QueueHandle_t inbox;

static void makeDeviceId() {
  // Factory MAC from eFuse, e.g. "esp32-246f28ae5278"
  uint64_t mac = ESP.getEfuseMac();
  char id[19];
  snprintf(id, sizeof(id), "esp32-%02x%02x%02x%02x%02x%02x",
           (uint8_t)mac, (uint8_t)(mac >> 8), (uint8_t)(mac >> 16),
           (uint8_t)(mac >> 24), (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
  deviceId = id;
}

static void queue(Kind kind, const char *data, size_t length) {
  Inbox item{kind, length, nullptr};
  if (length) {
    item.data = (uint8_t *)malloc(length + 1);
    if (!item.data) return;
    memcpy(item.data, data, length);
    item.data[length] = 0;
  }
  if (xQueueSend(inbox, &item, 0) != pdTRUE) free(item.data);  // full: drop (only audio arrives that fast)
}

static bool topicIs(const esp_mqtt_event_handle_t e, const String &topic) {
  return e->topic_len == (int)topic.length() && strncmp(e->topic, topic.c_str(), e->topic_len) == 0;
}

// Runs on the MQTT task: only queue things up
static void onMqttEvent(void *, esp_event_base_t, int32_t id, void *data) {
  auto e = (esp_mqtt_event_handle_t)data;
  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
      brokerUp = true;
      esp_mqtt_client_subscribe(mqtt, topicCmd.c_str(), 1);
      esp_mqtt_client_subscribe(mqtt, topicAudio.c_str(), 0);
      esp_mqtt_client_subscribe(mqtt, "ss/server/online", 1);
      esp_mqtt_client_subscribe(mqtt, "ss/server/http", 1);
      esp_mqtt_client_publish(mqtt, topicOnline.c_str(), "1", 1, 1, 1);  // retained; "0" is the last will
      break;
    case MQTT_EVENT_DISCONNECTED:
      brokerUp = false;
      queue(BROKER_DOWN, nullptr, 0);
      break;
    case MQTT_EVENT_DATA:
      if (e->current_data_offset != 0 || e->data_len != e->total_data_len) break;  // larger than the buffer: ignore
      if (topicIs(e, topicCmd)) queue(CMD, e->data, e->data_len);
      else if (topicIs(e, topicAudio)) queue(AUDIO, e->data, e->data_len);
      else if (topicIs(e, "ss/server/online")) queue(SERVER_ONLINE, e->data, e->data_len);
      else if (topicIs(e, "ss/server/http")) queue(SERVER_HTTP, e->data, e->data_len);
      break;
    default:
      break;
  }
}

static void startMqtt() {
  static String uri = "mqtts://" + String(MQTT_HOST) + ":" + String(MQTT_PORT);
  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.uri = uri.c_str();
  cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;  // trusts the broker's public (DigiCert) certificate
  cfg.credentials.client_id = deviceId.c_str();
  cfg.credentials.username = MQTT_USERNAME;
  cfg.credentials.authentication.password = MQTT_PASSWORD;
  cfg.session.keepalive = 30;
  cfg.session.last_will.topic = topicOnline.c_str();
  cfg.session.last_will.msg = "0";
  cfg.session.last_will.msg_len = 1;
  cfg.session.last_will.qos = 1;
  cfg.session.last_will.retain = 1;
  cfg.buffer.size = 8192;  // fits the largest command (alarms_sync with 20 alarms)
  cfg.buffer.out_size = 4096;
  cfg.network.reconnect_timeout_ms = 5000;
  mqtt = esp_mqtt_client_init(&cfg);
  esp_mqtt_client_register_event(mqtt, MQTT_EVENT_ANY, onMqttEvent, nullptr);
  esp_mqtt_client_start(mqtt);  // connects, and reconnects by itself whenever needed
  Serial.println("Connecting to the broker " + uri);
}

void netSetup(const NetHandlers &h) {
  handlers = h;
  makeDeviceId();
  topicCmd = "ss/dev/" + deviceId + "/cmd";
  topicAudio = "ss/dev/" + deviceId + "/audio";
  topicEvt = "ss/dev/" + deviceId + "/evt";
  topicOnline = "ss/dev/" + deviceId + "/online";
  inbox = xQueueCreate(64, sizeof(Inbox));
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
  WiFi.setSleep(false);  // keeps latency low for talk audio
}

void netLoop() {
  static bool wifiWasUp = false;
  bool wifiUp = WiFi.status() == WL_CONNECTED;
  if (wifiUp != wifiWasUp) {
    wifiWasUp = wifiUp;
    Serial.println(wifiUp ? "Wi-Fi connected, IP: " + WiFi.localIP().toString() : String("Wi-Fi lost, reconnecting..."));
    if (wifiUp && !mqtt) startMqtt();  // once; the client handles every reconnect after that
  }

  Inbox item;
  while (xQueueReceive(inbox, &item, 0) == pdTRUE) {
    switch (item.kind) {
      case CMD:
        if (handlers.text) handlers.text(item.data, item.length);
        break;
      case AUDIO:
        if (handlers.binary) handlers.binary(item.data, item.length);
        break;
      case SERVER_ONLINE: {
        bool up = item.length == 1 && item.data[0] == '1';
        if (up && !serverUp && handlers.connected) {
          Serial.println("Server online");
          serverUp = true;
          handlers.connected();  // hello, volume, flash layout, ...
        }
        serverUp = up;
        break;
      }
      case SERVER_HTTP:
        httpBase = String((const char *)item.data);
        break;
      case BROKER_DOWN:
        serverUp = false;  // on reconnect the retained "1" arrives again, and we say hello again
        break;
    }
    free(item.data);
  }
}

bool netConnected() { return brokerUp && serverUp; }
String serverHttp() { return httpBase; }

void netSend(const char *json) {
  if (!brokerUp) return;
  // enqueue, not publish: returns at once instead of waiting on the network
  esp_mqtt_client_enqueue(mqtt, topicEvt.c_str(), json, strlen(json), 1, 0, true);
}

void netSend(const String &json) { netSend(json.c_str()); }

void resetWifi() {
  wm.resetSettings();
  ESP.restart();
}
