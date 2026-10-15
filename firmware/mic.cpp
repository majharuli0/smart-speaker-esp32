#include "mic.h"
#include "driver/i2s.h"
#include "config.h"
#include "net.h"
#include "audio.h"

static const size_t PIECE = SAMPLE_RATE / 8;  // 125 ms per MQTT message: 8 a second
static const size_t BLOCK = 256;              // frames per I2S read (16 ms)

static const uint32_t WINDOW = SAMPLE_RATE / 20;  // 50 ms: the step for telling speech from quiet

static TaskHandle_t task = nullptr;
static volatile bool running = false;
static volatile bool stopAsked = false;
static int seconds = 5;
static int endSilenceMs = 0;  // 0: record the whole time

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

static void micTask(void *) {
  static int32_t frames[2 * BLOCK];  // [left, right, left, right, …]
  static int16_t piece[PIECE];
  const uint32_t want = (uint32_t)seconds * SAMPLE_RATE;
  uint32_t total = 0, clipped = 0, pieces = 0, dropped = 0;
  size_t fill = 0;
  uint64_t energyL = 0, energyR = 0;  // which slot the mic answers in
  bool right = false;
  int32_t dc = 0;                     // the mic's DC offset (x16), a slow average
  bool first = true;
  uint32_t skip = SAMPLE_RATE * 3 / 10;  // the first 300 ms: the mic waking up, and its start-up thump
  int64_t levelSum = 0;
  int32_t levelPeak = 0;
  uint32_t levelCount = 0;
  // Speech or quiet, per 50 ms window, against the room's own background (the
  // first 300 ms set it, quiet moments keep it current): speech is 9 dB above it
  int64_t windowSum = 0;
  uint32_t windowCount = 0, windows = 0, loudRun = 0, quietMs = 0;
  float background = 0;
  bool heard = false, silenceEnded = false;
  char json[200];

  while (!stopAsked && !silenceEnded && total < want) {
    size_t got = 0;
    if (i2s_read(MIC_PORT, frames, sizeof(frames), &got, pdMS_TO_TICKS(100)) != ESP_OK || !got) continue;
    size_t n = got / 8;
    for (size_t i = 0; i < n; i++) {
      energyL += abs(frames[2 * i] >> 8);
      energyR += abs(frames[2 * i + 1] >> 8);
    }
    if (energyR > 2 * energyL) right = true;
    else if (energyL > 2 * energyR) right = false;

    for (size_t i = 0; i < n && total < want && !silenceEnded; i++) {
      int32_t x = frames[2 * i + (right ? 1 : 0)] >> 8;  // the mic's 24-bit sample
      if (first) { dc = x << 4; first = false; }
      dc += ((x << 4) - dc) >> 10;  // ~2.5 Hz: follows the offset, not the voice
      int32_t y = (x - (dc >> 4)) >> MIC_GAIN_SHIFT;
      if (skip) { skip--; continue; }
      if (y > 32767 || y < -32768) {
        clipped++;
        y = constrain(y, -32768, 32767);
      }
      piece[fill++] = (int16_t)y;
      total++;
      levelSum += (int64_t)y * y;
      levelPeak = max(levelPeak, abs(y));
      levelCount++;
      windowSum += (int64_t)y * y;

      if (++windowCount == WINDOW) {
        float power = (float)windowSum / WINDOW;
        windowSum = windowCount = 0;
        if (windows++ < 6) background += power / 6;  // the first 300 ms
        else {
          bool loud = power > background * 8 && power > 3400;  // +9 dB, and above -55 dB
          loudRun = loud ? loudRun + 1 : 0;
          if (loudRun >= 3) heard = true;  // 150 ms of it: someone is speaking
          if (power < background * 4) {
            if (heard) quietMs += 50;
            background = background * 0.95f + power * 0.05f;  // follow the room
          } else quietMs = 0;
          if (endSilenceMs && heard && quietMs >= (uint32_t)endSilenceMs) silenceEnded = true;
        }
      }

      if (fill == PIECE) {
        if (netSendMic((const uint8_t *)piece, sizeof(piece))) pieces++;
        else dropped++;  // not connected: lost, never piled up
        fill = 0;
      }
      if (levelCount == SAMPLE_RATE / 4) {  // the live level meter, 4 times a second
        float rms = dbfs(sqrtf((float)levelSum / levelCount)), peak = dbfs(levelPeak);
        snprintf(json, sizeof(json), "{\"type\":\"mic_level\",\"rms\":%.1f,\"peak\":%.1f}", rms, peak);
        netSendNow(json);
        Serial.printf("Mic: %.1f dB, peak %.1f dB\n", rms, peak);
        levelSum = levelPeak = levelCount = 0;
      }
    }
  }
  if (fill) {  // the last, shorter piece
    if (netSendMic((const uint8_t *)piece, fill * 2)) pieces++;
    else dropped++;
  }
  i2s_driver_uninstall(MIC_PORT);
  const char *endedBy = stopAsked ? "stop" : silenceEnded ? "silence" : "time";
  snprintf(json, sizeof(json),
           "{\"type\":\"mic_done\",\"samples\":%lu,\"pieces\":%lu,\"dropped\":%lu,\"clipped\":%lu,"
           "\"slot\":\"%s\",\"gainShift\":%d,\"speech\":%s,\"endedBy\":\"%s\"}",
           (unsigned long)total, (unsigned long)pieces, (unsigned long)dropped, (unsigned long)clipped,
           right ? "right" : "left", MIC_GAIN_SHIFT, heard ? "true" : "false", endedBy);
  netSend(json);
  Serial.printf("Microphone off: %lu samples, %lu clipped, %s slot, gain shift %d\n", (unsigned long)total,
                (unsigned long)clipped, right ? "right" : "left", MIC_GAIN_SHIFT);
  running = false;
  task = nullptr;
  vTaskDelete(nullptr);
}

bool micRecord(int s, int endSilence) {
  if (running) return false;  // one recording at a time
  seconds = constrain(s, 1, MIC_TEST_MAX_S);
  endSilenceMs = constrain(endSilence, 0, 5000);
  toneStop();  // a quiet room: the speaker doesn't play while it listens
  talkStop();
  if (!portStart()) {
    netSend("{\"type\":\"mic_done\",\"error\":\"the microphone's I2S port didn't start\"}");
    return false;
  }
  stopAsked = false;
  running = true;
  Serial.printf("Microphone on: up to %d s%s\n", seconds, endSilenceMs ? ", until the room is quiet again" : "");
  // Its own task, so a busy main loop doesn't lose audio; core 1, like the loop (Wi-Fi is on core 0)
  xTaskCreatePinnedToCore(micTask, "mic", 6144, nullptr, 3, &task, 1);
  return true;
}

void micStop() {
  if (running) stopAsked = true;
}

bool micBusy() { return running; }
