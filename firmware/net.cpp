#include "net.h"
#include <WiFi.h>
#include <WiFiProv.h>
#include "network_provisioning/scheme_ble.h"
#include <ESPmDNS.h>
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "esp_wifi.h"
#include "mbedtls/md.h"
#include "config.h"

// Topics (see docs/protocol.md):
//   ss/dev/<id>/cmd     ← commands         ss/dev/<id>/evt     → replies and status
//   ss/dev/<id>/audio   ← talk audio       ss/dev/<id>/online  → "1" retained, "0" last will
//   ss/server/online    ← "1" when the server is up: say hello again
//   ss/server/http      ← base URL for downloads

String deviceId;
String devicePop;

static NetHandlers handlers;
static esp_mqtt_client_handle_t mqtt = nullptr;
static String topicCmd, topicAudio, topicEvt, topicOnline;
static volatile bool brokerUp = false;  // set on the MQTT task
static volatile bool bluetoothOn = false;  // Wi-Fi setup over Bluetooth in progress (set on the event task)
static volatile bool credsReceived = false;  // the app sent Wi-Fi details in this setup session
static volatile bool setupEnded = false;     // the setup session finished: close it on the main loop
static bool lowLatency = false;              // Wi-Fi power saving off (only while Bluetooth is off)
static String setupName;                     // Bluetooth name during setup: "SS-09FC"
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

// The code on the device's QR label: first 8 hex of HMAC-SHA256(DEVICE_SECRET, device ID).
// It unlocks Bluetooth setup, and the server checks it when someone adds the device.
static void makePop() {
  uint8_t mac[32];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t *)DEVICE_SECRET, strlen(DEVICE_SECRET),
                  (const uint8_t *)deviceId.c_str(), deviceId.length(), mac);
  char hex[9];
  snprintf(hex, sizeof(hex), "%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3]);
  devicePop = hex;
}

// Our MQTT broker asks the backend about every login: a device logs in as itself
// (username = client ID = device ID) with HMAC-SHA256(DEVICE_SECRET, "mqtt:" + ID)
// in hex. Not the label code, so the printed label doesn't reveal it.
static String mqttPassword() {
  uint8_t mac[32];
  String msg = "mqtt:" + deviceId;
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t *)DEVICE_SECRET, strlen(DEVICE_SECRET),
                  (const uint8_t *)msg.c_str(), msg.length(), mac);
  char hex[65];
  for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", mac[i]);
  return hex;
}

// Wi-Fi setup over Bluetooth (Espressif's provisioning: an encrypted session that
// needs the code from the QR label). Runs on the event task: only logs and flags.
static void onProvEvent(arduino_event_t *e) {
  switch (e->event_id) {
    case ARDUINO_EVENT_PROV_START:
      bluetoothOn = true;
      credsReceived = false;
      Serial.println("Wi-Fi setup: waiting for the app over Bluetooth");
      break;
    case ARDUINO_EVENT_PROV_CRED_RECV:
      credsReceived = true;
      Serial.printf("Wi-Fi setup: got the details for \"%s\"\n", (const char *)e->event_info.prov_cred_recv.ssid);
      break;
    case ARDUINO_EVENT_PROV_CRED_FAIL:
      Serial.println(e->event_info.prov_fail_reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR ? "Wi-Fi setup: wrong password"
                                                                                       : "Wi-Fi setup: network not found");
      network_prov_mgr_reset_wifi_sm_state_on_failure();  // let the app try again
      break;
    case ARDUINO_EVENT_PROV_CRED_SUCCESS:
      Serial.println("Wi-Fi setup: done");
      break;
    case ARDUINO_EVENT_PROV_END:
      setupEnded = true;  // the app has its answer: close Bluetooth (on the main loop)
      break;
    case ARDUINO_EVENT_PROV_DEINIT:
      bluetoothOn = false;  // Bluetooth is off and its memory released
      break;
    default:
      break;
  }
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

// The broker's address. With "mqtt://speaker-server.local:1883" the computer
// running the backend is looked up by name on the local network (mDNS; the
// backend answers it), so it doesn't matter which address the router gives
// that computer. Any other host (a domain, an IP) is used as it is.
// Returns "" while the name can't be found.
static String brokerUri() {
  String uri = MQTT_URI;
  int start = uri.indexOf("://") + 3;
  int end = uri.indexOf(':', start);
  if (end < 0) end = uri.length();
  String host = uri.substring(start, end);
  if (!host.endsWith(".local")) return uri;
  static bool mdnsUp = false;
  if (!mdnsUp) mdnsUp = MDNS.begin("ss-" + deviceId.substring(deviceId.length() - 4));
  IPAddress ip = MDNS.queryHost(host.substring(0, host.length() - 6), 1500);  // blocks up to 1.5 s
  if (ip == IPAddress()) return "";
  return uri.substring(0, start) + ip.toString() + uri.substring(end);
}

static String brokerAt;  // the resolved address in use

static bool startMqtt() {
  String uri = brokerUri();
  if (uri.isEmpty()) {
    Serial.println("Broker " MQTT_URI " not found on the network yet, trying again shortly");
    return false;
  }
  brokerAt = uri;
  static String password = mqttPassword();
  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.uri = brokerAt.c_str();
  if (uri.startsWith("mqtts://"))
    cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;  // a broker with a public certificate
  cfg.credentials.client_id = deviceId.c_str();
  cfg.credentials.username = deviceId.c_str();
  cfg.credentials.authentication.password = password.c_str();
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
  return true;
}

static bool hasSavedWifi() {
  wifi_config_t saved;
  return esp_wifi_get_config(WIFI_IF_STA, &saved) == ESP_OK && saved.sta.ssid[0];
}

// Espressif's Wi-Fi provisioning over Bluetooth (security 1: an encrypted session
// that needs the label code), the same service WiFiProv sets up, so the same
// apps find it. Can be opened at any time and closed again: Bluetooth's memory is
// kept (no "free BLE" handler), so it can reopen later.
static const uint8_t SETUP_SERVICE_UUID[16] = {0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
                                               0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02};

static void startBluetoothSetup() {
  if (bluetoothOn) return;
  WiFi.setSleep(true);  // Wi-Fi and Bluetooth share the radio: Wi-Fi must allow power saving
  lowLatency = false;
  network_prov_mgr_config_t cfg = {};
  cfg.scheme = network_prov_scheme_ble;
  cfg.scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE;
  if (network_prov_mgr_init(cfg) != ESP_OK) {
    Serial.println("Wi-Fi setup: Bluetooth didn't start");
    return;
  }
  network_prov_scheme_ble_set_service_uuid((uint8_t *)SETUP_SERVICE_UUID);
  if (network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1, devicePop.c_str(), setupName.c_str(), nullptr) !=
      ESP_OK) {
    network_prov_mgr_deinit();
    Serial.println("Wi-Fi setup: Bluetooth didn't start");
    return;
  }
  WiFiProv.printQR(setupName.c_str(), devicePop.c_str(), "ble");  // for testing with Espressif's "ESP BLE Provisioning" app
}

static void stopBluetoothSetup() { network_prov_mgr_deinit(); }  // safe to call twice

void netSetup(const NetHandlers &h) {
  handlers = h;
  makeDeviceId();
  topicCmd = "ss/dev/" + deviceId + "/cmd";
  topicAudio = "ss/dev/" + deviceId + "/audio";
  topicEvt = "ss/dev/" + deviceId + "/evt";
  topicOnline = "ss/dev/" + deviceId + "/online";
  inbox = xQueueCreate(64, sizeof(Inbox));
  makePop();
  String uid = deviceId.substring(6);  // people see the 12-character UID; "esp32-" stays internal
  uid.toUpperCase();
  Serial.println("Device UID: " + uid + "  firmware " FW_VERSION);
  Serial.println("QR label: SS:" + uid + ":" + devicePop);

  // Saved Wi-Fi: connects in the background and never blocks. If the router is
  // down (e.g. still booting after a power cut) the device keeps running and
  // alarms keep ringing; the Wi-Fi driver reconnects by itself when it's back.
  // No Wi-Fi yet: Bluetooth setup as "SS-09FC" until the app sends it (alarms and
  // the clock keep working meanwhile). See netLoop() for Wi-Fi recovery.
  setupName = PROV_NAME_PREFIX + deviceId.substring(deviceId.length() - 4);
  setupName.toUpperCase();
  WiFi.onEvent(onProvEvent);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  if (hasSavedWifi()) WiFi.begin();
  else startBluetoothSetup();
}

void netLoop() {
  static bool wifiWasUp = false;
  bool wifiUp = WiFi.status() == WL_CONNECTED;
  if (wifiUp != wifiWasUp) {
    wifiWasUp = wifiUp;
    Serial.println(wifiUp ? "Wi-Fi connected, IP: " + WiFi.localIP().toString() : String("Wi-Fi lost, reconnecting..."));
  }

  // Start the MQTT client once the broker is found (the client handles every
  // reconnect after that). If the broker stays unreachable, look its name up
  // again: the computer may have been given a new address.
  static unsigned long lastLookup = 0, downSince = 0;
  bool due = !lastLookup || millis() - lastLookup > 10000;
  if (wifiUp && !mqtt && due) {
    lastLookup = millis();
    startMqtt();
  }
  if (brokerUp || !mqtt) downSince = 0;
  else if (!downSince) downSince = millis();
  if (wifiUp && downSince && millis() - downSince > 30000 && millis() - lastLookup > 30000) {
    lastLookup = millis();
    String uri = brokerUri();
    if (uri.length() && uri != brokerAt) {
      brokerAt = uri;
      esp_mqtt_client_set_uri(mqtt, brokerAt.c_str());  // used from the next reconnect
      Serial.println("Broker moved: " + brokerAt);
    }
  }
  // Wi-Fi recovery: if the saved network has been unreachable for WIFI_RECOVERY_MS
  // (new router, changed password), also open Bluetooth setup so the app can send
  // new details, while still trying the old network. If the old one comes back
  // first, Bluetooth closes again.
  static unsigned long wifiDownSince = millis();
  if (wifiUp) wifiDownSince = 0;
  else if (!wifiDownSince) wifiDownSince = millis();
  if (!wifiUp && !bluetoothOn && wifiDownSince && millis() - wifiDownSince > WIFI_RECOVERY_MS && hasSavedWifi()) {
    Serial.println("Wi-Fi unreachable for " + String(WIFI_RECOVERY_MS / 60000) + " min: opening Bluetooth setup too");
    startBluetoothSetup();
  }
  if (setupEnded) {  // the app got its answer
    setupEnded = false;
    stopBluetoothSetup();
  }
  if (wifiUp && bluetoothOn && !credsReceived) {
    Serial.println("Wi-Fi is back: closing Bluetooth setup");
    stopBluetoothSetup();
  }

  // Low latency for talk audio. Not allowed while Bluetooth is on (they share the radio).
  if (wifiUp && !lowLatency && !bluetoothOn) lowLatency = WiFi.setSleep(false);

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

// Forget the Wi-Fi and restart: the device comes back in Bluetooth setup
void resetWifi() {
  WiFi.eraseAP();
  ESP.restart();
}
