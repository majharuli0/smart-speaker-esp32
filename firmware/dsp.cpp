#include "dsp.h"
#include "config.h"
#include "esp_ns.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/idf_additions.h"

static const size_t FRAME = SAMPLE_RATE / 100;        // 10 ms: what the suppressor works in
static const size_t BUF_BYTES = SAMPLE_RATE * 2 * 4;  // 4 s each way (PSRAM): room for the wake word's pre-roll at once
static const uint32_t STACK = 4096;                   // its task's (internal RAM, only during a session; ~2.6 KB used)

static StreamBufferHandle_t inBuf = nullptr, outBuf = nullptr;  // made once, kept
static SemaphoreHandle_t finished = nullptr;
static ns_handle_t ns = nullptr;  // kept between sessions (41 KB PSRAM; making it takes a moment)
static int nsMode = -1;           // the mode it was made for (0 mild, 1 medium, 2 aggressive)
static volatile int asked = 0;
static volatile bool stopping = false;
static volatile bool running = false;
static volatile uint32_t drops = 0;
static volatile uint32_t stackLeft = 0;

// High-pass at 180 Hz, 4th order (two biquads, Butterworth), like a phone line
// (calls keep 300 Hz and up). Below it: fan rumble, hum, and the short low ticks
// the mic picks up in a call (~100 Hz, 2 ms, a few a second: electrical, likely
// Wi-Fi or the supply; heard as "rain" in a quiet room, 2026-10-19)
static const float HP_HZ = 180;
static const float HP_Q[2] = {0.5412f, 1.3066f};
struct Biquad {
  float b0, b1, b2, a1, a2, x1, x2, y1, y2;
  void setup(float q) {
    float w = 2 * PI * HP_HZ / SAMPLE_RATE, c = cosf(w), alpha = sinf(w) / (2 * q), a0 = 1 + alpha;
    b0 = (1 + c) / 2 / a0;
    b1 = -(1 + c) / a0;
    b2 = b0;
    a1 = -2 * c / a0;
    a2 = (1 - alpha) / a0;
    x1 = x2 = y1 = y2 = 0;
  }
  float run(float x) {
    float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = y;
    return y;
  }
};
static Biquad hp[2];

static void highPassSetup() {
  hp[0].setup(HP_Q[0]);
  hp[1].setup(HP_Q[1]);
}

// A little of the sound before suppression is mixed back in (-18 dB): a faint, even
// room behind the voice, so a call doesn't sound dead or processed, and the
// suppressor's leftovers are covered. The fan stays mostly gone.
static const float BLEND = 0.125f;

static void dspTask(void *) {
  int mode = asked - 1;  // 0 mild, 1 medium, 2 aggressive
  if (!ns || nsMode != mode) {
    if (ns) ns_destroy(ns);
    int64_t t0 = esp_timer_get_time();
    ns = ns_pro_create(10, mode, SAMPLE_RATE);
    nsMode = ns ? mode : -1;
    Serial.printf("Noise suppression: level %d %s in %lld ms\n", asked, ns ? "made" : "FAILED (sent as is)",
                  (esp_timer_get_time() - t0) / 1000);
  }
  int16_t in[FRAME], out[FRAME];
  size_t have = 0;  // bytes of the frame being filled
  for (;;) {
    have += xStreamBufferReceive(inBuf, (uint8_t *)in + have, sizeof(in) - have, pdMS_TO_TICKS(20));
    if (have == sizeof(in)) {
      if (ns) {
        ns_process(ns, in, out);
        for (size_t i = 0; i < FRAME; i++) out[i] = (int16_t)constrain(out[i] + in[i] * BLEND, -32768.0f, 32767.0f);
      } else memcpy(out, in, sizeof(out));
      // Whole frames only (a part would split a sample); full: lost, never late
      if (xStreamBufferSpacesAvailable(outBuf) >= sizeof(out)) xStreamBufferSend(outBuf, out, sizeof(out), 0);
      else drops++;
      have = 0;
    } else if (stopping && xStreamBufferIsEmpty(inBuf)) break;  // done (the last few ms, under a frame, are dropped)
  }
  stackLeft = uxTaskGetStackHighWaterMark(nullptr);
  running = false;
  xSemaphoreGive(finished);
  vTaskDelete(nullptr);
}

bool dspStart(int level) {
  if (running || level < 1 || level > 3) return false;
  if (!inBuf) {
    inBuf = xStreamBufferCreateWithCaps(BUF_BYTES, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    outBuf = xStreamBufferCreateWithCaps(BUF_BYTES, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    finished = xSemaphoreCreateBinary();
    if (!inBuf || !outBuf || !finished) {
      Serial.println("Noise suppression: no memory for its buffers");
      return false;
    }
  }
  xStreamBufferReset(inBuf);  // no task is using them now
  xStreamBufferReset(outBuf);
  xSemaphoreTake(finished, 0);
  highPassSetup();
  asked = level;
  stopping = false;
  drops = 0;
  running = true;
  // Core 0 (Wi-Fi's core, mostly idle), below the network tasks
  if (xTaskCreatePinnedToCore(dspTask, "dsp", STACK, nullptr, 2, nullptr, 0) != pdPASS) {
    running = false;
    Serial.println("Noise suppression: its task didn't start");
    return false;
  }
  return true;
}

void dspPush(const int16_t *x, size_t n) {
  static int16_t chunk[256];
  while (n) {
    size_t k = min(n, sizeof(chunk) / sizeof(chunk[0]));
    for (size_t i = 0; i < k; i++) chunk[i] = (int16_t)constrain(hp[1].run(hp[0].run(x[i])), -32768.0f, 32767.0f);
    if (xStreamBufferSpacesAvailable(inBuf) >= k * 2) xStreamBufferSend(inBuf, chunk, k * 2, 0);
    else drops++;
    x += k;
    n -= k;
  }
}

size_t dspPull(int16_t *y, size_t max) { return xStreamBufferReceive(outBuf, y, max * 2, 0) / 2; }

void dspStop() {
  if (!running) return;
  stopping = true;
  if (xSemaphoreTake(finished, pdMS_TO_TICKS(500)) != pdTRUE) {
    Serial.println("Noise suppression: its task didn't finish");
    return;
  }
  Serial.printf("Noise suppression: session over, %lu frames lost, stack left %lu\n", (unsigned long)drops,
                (unsigned long)stackLeft);
}

uint32_t dspDrops() { return drops; }
