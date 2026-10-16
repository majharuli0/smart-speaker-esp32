#include "audiolink.h"
#include "esp_crt_bundle.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ws.h"
#include "freertos/ringbuf.h"
#include "freertos/idf_additions.h"
#include "net.h"
#include "opus.h"

static const uint8_t MIC_OPUS = 0x01, TALK_OPUS = 0x02, MIC_PCM = 0x03, TALK_PCM = 0x04;
static const int OPUS_FRAME = 320;     // samples: 20 ms
static const int OPUS_BITRATE = 24000;
static const int OPUS_COMPLEXITY = 2;  // 0-10: low keeps the CPU free for the rest
static const int MAX_FRAMES = 16;      // in one message (talk comes in up to 125 ms)

// Opus, used only on the link task (it needs a deep stack)
static OpusEncoder *encoder = nullptr;
static OpusDecoder *decoder = nullptr;

// A mic piece (PCM) → [MIC_OPUS][seq][frames…]; a short last piece is padded with silence
static size_t encodeMic(const uint8_t *item, size_t size, uint8_t *out, size_t room) {
  const int16_t *pcm = (const int16_t *)(item + 3);
  int samples = (size - 3) / 2;
  out[0] = MIC_OPUS;
  out[1] = item[1];
  out[2] = item[2];
  size_t n = 3;
  int16_t frame[OPUS_FRAME];
  for (int at = 0; at < samples && n + 2 + 200 <= room; at += OPUS_FRAME) {
    int take = min(OPUS_FRAME, samples - at);
    memcpy(frame, pcm + at, take * 2);
    if (take < OPUS_FRAME) memset(frame + take, 0, (OPUS_FRAME - take) * 2);
    int bytes = opus_encode(encoder, frame, OPUS_FRAME, out + n + 2, room - n - 2);
    if (bytes <= 0) break;
    out[n] = bytes >> 8;
    out[n + 1] = bytes & 255;
    n += 2 + bytes;
  }
  return n;
}

// [frames…] from the server → PCM for the speaker, handed to the main loop
static void decodeTalk(const uint8_t *in, int length) {
  static int16_t pcm[OPUS_FRAME * MAX_FRAMES];
  int samples = 0;
  for (int at = 0; at + 2 <= length && samples + OPUS_FRAME <= OPUS_FRAME * MAX_FRAMES;) {
    int bytes = (in[at] << 8) | in[at + 1];
    if (at + 2 + bytes > length) break;
    int got = opus_decode(decoder, in + at + 2, bytes, pcm + samples, OPUS_FRAME, 0);
    if (got > 0) samples += got;
    at += 2 + bytes;
  }
  if (samples) netDeliverAudio((const uint8_t *)pcm, samples * 2);
}
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
  int err;
  encoder = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &err);
  if (encoder) {
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(OPUS_BITRATE));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(OPUS_COMPLEXITY));
    opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  }
  decoder = opus_decoder_create(16000, 1, &err);
  if (!encoder || !decoder) {  // no memory: raw PCM still works
    Serial.println("Audio link: Opus didn't start, sending raw audio");
    encoder = nullptr;
    decoder = nullptr;
  }
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
    const char *failed = nullptr;
    bool ok = esp_transport_connect(ws, host.c_str(), port, 5000) >= 0;
    if (!ok) failed = "couldn't connect";
    if (ok) {  // log in like on MQTT: the device ID and its password
      String hello = "{\"type\":\"hello\",\"id\":\"" + deviceId + "\",\"password\":\"" + netPassword() + "\"" +
                     (encoder ? ",\"codec\":\"opus\"}" : "}");
      ok = sendText(ws, hello);
      if (!ok) failed = "sending the login failed";
    }
    if (ok) {
      int r = readFrame(ws, rx, sizeof(rx) - 1, 5000, &op);
      ok = r > 0 && op == WS_TRANSPORT_OPCODES_TEXT;
      if (ok) {
        rx[r] = 0;
        ok = strstr((char *)rx, "\"ok\"") != nullptr;
      }
      if (!ok) failed = "the server didn't accept the login";
    }
    static unsigned tries = 0;  // failed in a row: said on the first, then about once a minute
    if (ok) {
      up = true;
      tries = 0;
      Serial.println("Audio link up: " + u);
    } else if (tries++ % 20 == 0) {
      Serial.printf("Audio link: %s (%s, errno %d, try %u)\n", failed, u.c_str(), esp_transport_get_errno(ws), tries);
    }
    unsigned long lastSent = millis(), lastReport = millis();
    size_t sentBytes = 0;
    const char *why = "the server went away";
    while (ok && netConnected() && !urlChanged) {
      size_t size;
      void *item;
      while (ok && (item = xRingbufferReceive(outbox, &size, 0)) != nullptr) {  // mic audio out
        if (encoder && ((uint8_t *)item)[0] == MIC_PCM) {
          static uint8_t packed[2048];
          size_t n = encodeMic((const uint8_t *)item, size, packed, sizeof(packed));
          ok = sendFrame(ws, true, packed, n);
          sentBytes += n;
        } else {
          ok = sendFrame(ws, true, item, size);
          sentBytes += size;
        }
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
      else if (r > 3 && op == WS_TRANSPORT_OPCODES_BINARY && rx[0] == TALK_OPUS && decoder) decodeTalk(rx + 3, r - 3);
      if (millis() - lastReport > 60000) {  // how much it sends, and how deep Opus goes into the stack
        Serial.printf("Audio link: %u bytes of mic audio in the last minute; stack left %u\n", (unsigned)sentBytes,
                      (unsigned)uxTaskGetStackHighWaterMark(nullptr));
        sentBytes = 0;
        lastReport = millis();
      }
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
  // Core 0, with Wi-Fi; a TLS handshake and Opus (which works on the stack) need
  // a deep one: in PSRAM, as fast internal RAM is scarce (it never touches flash)
  xTaskCreatePinnedToCoreWithCaps(linkTask, "audiolink", 32768, nullptr, 2, nullptr, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
