#include "mic.h"
#include "driver/i2s.h"
#include "config.h"
#include "net.h"
#include "audio.h"
#include "wake.h"

static const size_t PIECE = SAMPLE_RATE / 8;  // 125 ms per MQTT message: 8 a second
static const size_t BLOCK = 256;              // frames per I2S read (16 ms)

static const uint32_t WINDOW = SAMPLE_RATE / 20;     // 50 ms: the step for telling speech from quiet
static const uint32_t SETTLE = SAMPLE_RATE * 3 / 10;  // after power-on: the mic waking up, and its start-up thump
static const uint32_t ECHO = SAMPLE_RATE / 5;         // after the speaker's own sound: what's still queued, and the room's echo
static const uint32_t CUE_GAP = SAMPLE_RATE / 10;     // a recording starts this long after its cue

static bool portOk = false;
static MicSession asked;  // set by micRecord (main loop), taken by the mic task
static volatile bool askedNew = false;
static volatile bool running = false;
static volatile bool stopAsked = false;

// The mic answers in one slot of each stereo frame (which one, its L/R pin
// decides): both are read, and the louder one is kept
static bool portStart() {
  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,  // 24-bit samples, sent in 32-bit slots
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = BLOCK,
    .use_apll = false,
  };
  i2s_pin_config_t pins = {
    .mck_io_num = I2S_PIN_NO_CHANGE,  // the mic needs no master clock
    .bck_io_num = MIC_BCLK_GPIO,
    .ws_io_num = MIC_WS_GPIO,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = MIC_SD_GPIO,
  };
  if (i2s_driver_install(MIC_PORT, &config, 0, NULL) != ESP_OK) return false;
  i2s_set_pin(MIC_PORT, &pins);
  i2s_zero_dma_buffer(MIC_PORT);
  return true;
}

static float dbfs(float v) { return v > 0 ? 20 * log10f(v / 32768) : -120; }

// The last few seconds the mic heard (PSRAM), so a request said straight after
// the wake word isn't lost while the server gets ready: sent from where the
// word ended, when the server's mic_record says afterWake
static const uint32_t RING = SAMPLE_RATE * 6;
static int16_t *ring = nullptr;
static volatile uint32_t written = 0;        // samples heard since power-on
static volatile uint32_t wakeMark = 0;       // where the last wake word ended
static volatile unsigned long wakeTime = 0;  // and when it was heard (0: never)
static const uint32_t WORD_END = SAMPLE_RATE * 45 / 100;  // a word is over after 450 ms of quiet
static const unsigned long AFTER_WAKE_MS = 4000;  // the server asks within this, or it's too old

static bool wakeFresh() { return ring && wakeTime && millis() - wakeTime < AFTER_WAKE_MS; }

static void micTask(void *) {
  static int32_t frames[2 * BLOCK];  // [left, right, left, right, …]
  static int16_t piece[PIECE];
  uint64_t energyL = 0, energyR = 0;  // which slot the mic answers in: the first second decides
  uint32_t sampled = 0;
  bool right = false;
  int32_t dc = 0;                     // the mic's DC offset (x16), a slow average
  bool first = true;
  uint32_t settle = SETTLE;
  uint32_t speakerQuiet = 0;          // samples since the speaker last made a sound
  // The room's background: the quietest recent 50 ms window (it drops at once to
  // a quieter one and rises ~2 dB a second), so someone talking from the very
  // start isn't taken for the background
  int64_t windowSum = 0;
  uint32_t windowCount = 0;
  float floor = -1;
  // The session under way. Speech or quiet, per 50 ms window: speech is 12 dB
  // above the background; quiet is under 8 dB above it (on the prototype the
  // background wanders 0-8 dB above its floor, and speech is 15-34 dB above).
  MicSession s = {};
  bool on = false, capturing = false;
  uint32_t gap = 0, want = 0, total = 0, clipped = 0, pieces = 0, dropped = 0, loudRun = 0, quietMs = 0;
  size_t fill = 0;
  bool heard = false, silenceEnded = false;
  int64_t sessionSum = 0, levelSum = 0;
  uint32_t sessionCount = 0, levelCount = 0;
  int32_t levelPeak = 0;
  char json[360];

  // One sample of the session: sent, and judged speech or quiet
  auto capture = [&](int16_t y, bool over) {
    total++;
    if (over) clipped++;
    piece[fill++] = y;
    if (fill == PIECE) {
      if (netSendMic((const uint8_t *)piece, sizeof(piece))) pieces++;
      else dropped++;  // not connected: lost, never piled up
      fill = 0;
    }
    sessionSum += (int64_t)y * y;
    if (++sessionCount == WINDOW) {
      float power = (float)sessionSum / WINDOW;
      sessionSum = sessionCount = 0;
      bool loud = power > floor * 16 && power > 1100;  // +12 dB, and above -60 dB
      loudRun = loud ? loudRun + 1 : 0;
      if (loudRun >= 2) heard = true;  // 100 ms of it: someone is speaking
      if (power < floor * 6.3f) {      // under +8 dB
        if (heard) quietMs += 50;
      } else quietMs = 0;
      if (s.endSilenceMs && heard && quietMs >= (uint32_t)s.endSilenceMs) silenceEnded = true;
    }
    if (s.live) return;
    levelSum += (int64_t)y * y;
    levelPeak = max(levelPeak, abs((int32_t)y));
    if (++levelCount == SAMPLE_RATE / 4) {  // the live level meter, 4 times a second
      float rms = dbfs(sqrtf((float)levelSum / levelCount)), peak = dbfs(levelPeak);
      snprintf(json, sizeof(json), "{\"type\":\"mic_level\",\"rms\":%.1f,\"peak\":%.1f}", rms, peak);
      netSendNow(json);
      levelSum = levelPeak = levelCount = 0;
    }
  };

  for (;;) {
    if (askedNew) {  // a new session
      s = asked;
      askedNew = false;
      on = true;
      capturing = false;
      gap = CUE_GAP;
      want = (uint32_t)s.seconds * SAMPLE_RATE;
      total = clipped = pieces = dropped = loudRun = quietMs = 0;
      fill = 0;
      heard = silenceEnded = false;
      sessionSum = levelSum = levelPeak = sessionCount = levelCount = 0;
      wakeSessionStart(s.wakeTake, s.wakeTest);
      // Straight after the wake word: from where it ended, at once
      if (s.afterWake && wakeFresh()) {
        capturing = true;
        uint32_t from = written - wakeMark > RING ? written - RING : wakeMark;
        for (uint32_t i = from; i != written && !silenceEnded && total < want; i++) capture(ring[i % RING], false);
        wakeTime = 0;
      }
    }

    size_t got = 0;
    if (i2s_read(MIC_PORT, frames, sizeof(frames), &got, pdMS_TO_TICKS(100)) != ESP_OK || !got) continue;
    size_t n = got / 8;
    if (sampled < SAMPLE_RATE) {
      for (size_t i = 0; i < n; i++) {
        energyL += abs(frames[2 * i] >> 8);
        energyR += abs(frames[2 * i + 1] >> 8);
      }
      sampled += n;
      right = energyR > energyL;
    }
    // The speaker's own sound mustn't be taken for the room: the wake word
    // listener pauses, and a call's room audio isn't sent (no echo of the caller)
    if (audioBusy()) {
      if (speakerQuiet) wakeInterrupt();
      speakerQuiet = 0;
    }
    // A recording starts after its cue (a call's room audio starts at once)
    if (on && !capturing) {
      if (isCueing()) gap = CUE_GAP;
      else if (gap > n) gap -= n;
      else capturing = true;
      if (s.live) capturing = true;
    }

    for (size_t i = 0; i < n; i++) {
      int32_t x = frames[2 * i + (right ? 1 : 0)] >> 8;  // the mic's 24-bit sample
      if (first) { dc = x << 4; first = false; }
      dc += ((x << 4) - dc) >> 10;  // ~2.5 Hz: follows the offset, not the voice
      int32_t y = (x - (dc >> 4)) >> MIC_GAIN_SHIFT;
      if (settle) { settle--; continue; }
      bool over = y > 32767 || y < -32768;
      y = constrain(y, -32768, 32767);
      if (ring) ring[written % RING] = (int16_t)y;
      written = written + 1;
      bool echoFree = speakerQuiet >= ECHO;
      if (speakerQuiet < ECHO) speakerQuiet++;
      if (echoFree && wakeHear((int16_t)y)) {  // the wake word: it ended a moment ago
        wakeMark = written > WORD_END ? written - WORD_END : 0;
        wakeTime = millis() | 1;
      }

      windowSum += (int64_t)y * y;
      if (++windowCount == WINDOW) {
        float power = (float)windowSum / WINDOW;
        windowSum = windowCount = 0;
        floor = floor < 0 || power < floor ? power : floor * 1.023f;
      }
      if (!on || !capturing || silenceEnded || total >= want) continue;
      if (s.live && !echoFree) {  // the caller's voice is playing: send nothing
        total++;
        continue;
      }
      capture((int16_t)y, over);
    }

    if (on && (stopAsked || silenceEnded || total >= want)) {
      if (fill) {  // the last, shorter piece
        if (netSendMic((const uint8_t *)piece, fill * 2)) pieces++;
        else dropped++;
      }
      const char *endedBy = stopAsked ? "stop" : silenceEnded ? "silence" : "time";
      int len = snprintf(json, sizeof(json),
                         "{\"type\":\"mic_done\",\"samples\":%lu,\"pieces\":%lu,\"dropped\":%lu,\"clipped\":%lu,"
                         "\"slot\":\"%s\",\"gainShift\":%d,\"speech\":%s,\"endedBy\":\"%s\",\"floorDb\":%.1f",
                         (unsigned long)total, (unsigned long)pieces, (unsigned long)dropped, (unsigned long)clipped,
                         right ? "right" : "left", MIC_GAIN_SHIFT, heard ? "true" : "false", endedBy,
                         floor > 0 ? 10 * log10f(floor / (32768.0f * 32768.0f)) : -120.0f);
      len += wakeSessionEnd(json + len, sizeof(json) - len - 2);  // a wake word take or test: its result
      snprintf(json + len, sizeof(json) - len, "}");
      netSend(json);
      Serial.printf("Microphone session over (%s): %lu samples, %lu clipped\n", endedBy, (unsigned long)total,
                    (unsigned long)clipped);
      on = false;
      running = false;
    }
  }
}

void micSetup() {
  ring = (int16_t *)heap_caps_malloc(RING * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  portOk = portStart();
  if (!portOk) {
    Serial.println("Microphone: its I2S port didn't start");
    return;
  }
  // Its own task, so a busy main loop doesn't lose audio; core 1, like the loop (Wi-Fi is on core 0)
  xTaskCreatePinnedToCore(micTask, "mic", 8192, nullptr, 3, nullptr, 1);
}

bool micRecord(const MicSession &s) {
  if (running) return false;  // one session at a time
  if (!portOk) {
    netSend("{\"type\":\"mic_done\",\"error\":\"the microphone's I2S port didn't start\"}");
    return false;
  }
  asked = s;
  asked.seconds = constrain(s.seconds, 1, s.live ? MIC_LIVE_MAX_S : MIC_TEST_MAX_S);
  asked.endSilenceMs = s.live ? 0 : constrain(s.endSilenceMs, 0, 5000);
  toneStop();               // a quiet room: nothing plays while it listens…
  if (!s.live) talkStop();  // …except, in a call, the caller
  // "I'm listening", or for a call "the room can be heard now" (after the wake
  // word it beeped already, and sends what was said since)
  asked.afterWake = s.afterWake && wakeFresh();
  if ((asked.endSilenceMs || s.live) && !asked.afterWake) cueStart();
  stopAsked = false;
  running = true;
  askedNew = true;
  Serial.printf("Microphone session: up to %d s%s%s\n", asked.seconds, s.live ? ", a call" : "",
                asked.endSilenceMs ? ", until the room is quiet again" : "");
  return true;
}

void micStop() {
  if (running) stopAsked = true;
}

bool micBusy() { return running; }
