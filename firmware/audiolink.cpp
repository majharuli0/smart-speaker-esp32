#include "audiolink.h"
#include "esp_crt_bundle.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ws.h"
#include "freertos/ringbuf.h"
#include "net.h"

static const uint8_t MIC_PCM = 0x03, TALK_PCM = 0x04;
static const size_t OUTBOX_BYTES = 32 * 1024;  // ~1 s of mic audio waiting to go
static const unsigned long KEEPALIVE_MS = 15000;

static SemaphoreHandle_t urlLock;
static String url;                // set from the main loop, read by the link task
static volatile bool urlChanged = false;
static volatile bool up = false;
static RingbufHandle_t outbox;    // mic frames, whole, ready to send
static uint16_t micSeq = 0;       // mic task only

void audioLinkSetUrl(const String &u) {
  xSemaphoreTake(urlLock, portMAX_DELAY);
  if (u != url) {
    url = u;
    urlChanged = true;
  }
  xSemaphoreGive(urlLock);
}

bool audioLinkUp() { return up; }

// From the mic task: the frame is built straight in the outbox (no extra copy)
bool audioLinkSendMic(const uint8_t *pcm, size_t length) {
  if (!up) return false;
  uint8_t *slot = nullptr;
  if (xRingbufferSendAcquire(outbox, (void **)&slot, length + 3, 0) != pdTRUE) return false;  // full: dropped
  slot[0] = MIC_PCM;
  slot[1] = micSeq >> 8;
  slot[2] = micSeq & 255;
  micSeq++;
  memcpy(slot + 3, pcm, length);
  xRingbufferSendComplete(outbox, slot);
  return true;
}

// "wss://host[:port]/path" or "ws://…"
static bool parseUrl(const String &u, bool &tls, String &host, int &port, String &path) {
  tls = u.startsWith("wss://");
  if (!tls && !u.startsWith("ws://")) return false;
  int start = tls ? 6 : 5;
  int slash = u.indexOf('/', start);
  String hostPort = slash < 0 ? u.substring(start) : u.substring(start, slash);
  path = slash < 0 ? "/" : u.substring(slash);
  int colon = hostPort.indexOf(':');
  host = colon < 0 ? hostPort : hostPort.substring(0, colon);
  port = colon < 0 ? (tls ? 443 : 80) : hostPort.substring(colon + 1).toInt();
  return host.length() && port > 0;
}

// One whole frame: its length, 0 if none came in time (or it was too big for
// the buffer, and skipped), -1 if the link broke. The whole frame is always
// read, so the next read starts at the next frame.
static int readFrame(esp_transport_handle_t ws, uint8_t *buf, size_t size, int timeoutMs, ws_transport_opcodes_t *op) {
  int r = esp_transport_read(ws, (char *)buf, size, timeoutMs);
  if (r < 0) return -1;
  *op = esp_transport_ws_get_read_opcode(ws);
  if (*op == WS_TRANSPORT_OPCODES_CLOSE) return -1;
  if (r == 0) return 0;
  int total = esp_transport_ws_get_read_payload_len(ws);
  while (r < total) {
    // Past the buffer: read the rest into its end, to throw it away
    bool skipping = r >= (int)size;
    int at = skipping ? size - 256 : r;
    int want = skipping ? min(256, total - r) : min((int)size - r, total - r);
    int more = esp_transport_read(ws, (char *)buf + at, want, 1000);
    if (more <= 0) return -1;
    r += more;
  }
  return r > (int)size ? 0 : r;
}

// The data must be writable RAM: the WebSocket client masks it in place (as
// the protocol requires of clients), so it's changed afterwards
static bool sendFrame(esp_transport_handle_t ws, bool binary, void *data, size_t length) {
  auto op = (ws_transport_opcodes_t)((binary ? WS_TRANSPORT_OPCODES_BINARY : WS_TRANSPORT_OPCODES_TEXT) |
                                     WS_TRANSPORT_OPCODES_FIN);
  return esp_transport_ws_send_raw(ws, op, (const char *)data, length, 2000) >= 0;
}

static bool sendText(esp_transport_handle_t ws, const String &text) {
  char copy[160];  // never a constant in flash: see sendFrame
  size_t n = min(text.length(), sizeof(copy));
  memcpy(copy, text.c_str(), n);
  return sendFrame(ws, false, copy, n);
}

static void linkTask(void *) {
  static uint8_t rx[8192];  // a frame: 125 ms of talk audio (4,000 bytes) + its header, with room to spare
  for (;;) {
    if (!netConnected()) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    xSemaphoreTake(urlLock, portMAX_DELAY);
    String u = url;
    urlChanged = false;
    xSemaphoreGive(urlLock);
    bool tls;
    int port;
    String host, path;
    if (u.isEmpty() || !parseUrl(u, tls, host, port, path)) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    esp_transport_handle_t base = tls ? esp_transport_ssl_init() : esp_transport_tcp_init();
    if (tls) esp_transport_ssl_crt_bundle_attach(base, esp_crt_bundle_attach);  // a public certificate
    esp_transport_handle_t ws = esp_transport_ws_init(base);
    esp_transport_ws_set_path(ws, path.c_str());
    ws_transport_opcodes_t op;
    bool ok = esp_transport_connect(ws, host.c_str(), port, 5000) >= 0;
    if (ok) {  // log in like on MQTT: the device ID and its password
      String hello = "{\"type\":\"hello\",\"id\":\"" + deviceId + "\",\"password\":\"" + netPassword() + "\"}";
      ok = sendText(ws, hello);
    }
    if (ok) {
      int r = readFrame(ws, rx, sizeof(rx) - 1, 5000, &op);
      ok = r > 0 && op == WS_TRANSPORT_OPCODES_TEXT;
      if (ok) {
        rx[r] = 0;
        ok = strstr((char *)rx, "\"ok\"") != nullptr;
      }
    }
    if (ok) {
      up = true;
      Serial.println("Audio link up: " + u);
    }
    unsigned long lastSent = millis();
    const char *why = "the server went away";
    while (ok && netConnected() && !urlChanged) {
      size_t size;
      void *item;
      while (ok && (item = xRingbufferReceive(outbox, &size, 0)) != nullptr) {  // mic audio out
        ok = sendFrame(ws, true, item, size);
        vRingbufferReturnItem(outbox, item);
        lastSent = millis();
        if (!ok) why = "sending mic audio failed";
      }
      if (!ok) break;
      int r = readFrame(ws, rx, sizeof(rx), 10, &op);  // talk audio and commands in
      if (r < 0) {
        why = op == WS_TRANSPORT_OPCODES_CLOSE ? "the server closed it" : "reading failed";
        break;
      }
      if (r > 0 && op == WS_TRANSPORT_OPCODES_TEXT) netDeliverCommand(rx, r);
      else if (r > 3 && op == WS_TRANSPORT_OPCODES_BINARY && rx[0] == TALK_PCM) netDeliverAudio(rx + 3, r - 3);
      if (millis() - lastSent > KEEPALIVE_MS) {  // keeps NAT and proxies from closing it
        ok = sendText(ws, "{\"type\":\"ping\"}");
        if (!ok) why = "the keep-alive failed";
        lastSent = millis();
      }
    }
    if (up)
      Serial.printf("Audio link down: %s (errno %d, frames waiting %u)\n", why, esp_transport_get_errno(ws),
                    (unsigned)(OUTBOX_BYTES - xRingbufferGetCurFreeSize(outbox)));
    up = false;
    size_t size;
    void *item;
    while ((item = xRingbufferReceive(outbox, &size, 0)) != nullptr) vRingbufferReturnItem(outbox, item);
    esp_transport_close(ws);
    esp_transport_destroy(ws);
    esp_transport_destroy(base);
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

void audioLinkSetup() {
  urlLock = xSemaphoreCreateMutex();
  outbox = xRingbufferCreateWithCaps(OUTBOX_BYTES, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  // Core 0, with Wi-Fi; a TLS handshake needs a deep stack
  xTaskCreatePinnedToCore(linkTask, "audiolink", 12288, nullptr, 2, nullptr, 0);
}
