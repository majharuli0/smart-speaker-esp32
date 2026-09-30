#include "audio.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include "driver/i2s.h"
#include "config.h"
#include "net.h"

static Preferences prefs;
static int volume = DEFAULT_VOLUME;
static int32_t volumeGain = 0;  // 0-256 multiplier applied to every sample

static HTTPClient http;
static WiFiClient *toneStream = nullptr;
static String ringingTone;  // empty = not ringing
static int32_t toneBytesLeft = 0;
static unsigned long ringStart = 0;

static int16_t talkBuf[TALK_BUF_SAMPLES];
static size_t talkHead = 0, talkCount = 0;  // read position, samples buffered
static bool talking = false, talkPlaying = false, talkEnding = false;
static unsigned long talkLastPlayed = 0;

// ---- Volume ----

// Squared curve: ears hear loudness logarithmically, so a linear slider
// would do almost nothing in its top half.
static void applyVolumeSetting(int v) {
  volume = constrain(v, 0, 100);
  volumeGain = volume * volume * 256 / 10000;
}

static inline int16_t applyVolume(int16_t s) {
  return (int32_t)s * volumeGain >> 8;
}

void setVolume(int v) {
  applyVolumeSetting(v);
  prefs.putUChar("vol", volume);
}

void sendVolume() {
  netSend("{\"type\":\"volume\",\"value\":" + String(volume) + "}");
}

// ---- Speaker ----

static void i2sSetup() {
  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = true
  };
  i2s_pin_config_t pins = {
    .bck_io_num = I2S_BCLK_GPIO,
    .ws_io_num = I2S_LRC_GPIO,
    .data_out_num = I2S_DOUT_GPIO,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(I2S_PORT, &config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pins);
  i2s_zero_dma_buffer(I2S_PORT);
}

// Mono samples → both channels, volume applied, into the I2S queue
// (blocks only while the queue is full, i.e. at playback speed)
static void playSamples(const int16_t *mono, size_t n) {
  int16_t stereo[512];
  for (size_t i = 0; i < n; i++) stereo[2 * i] = stereo[2 * i + 1] = applyVolume(mono[i]);
  size_t written;
  i2s_write(I2S_PORT, stereo, n * 4, &written, portMAX_DELAY);
}

void audioSetup() {
  i2sSetup();
  prefs.begin("audio");
  applyVolumeSetting(prefs.getUChar("vol", DEFAULT_VOLUME));
}

// ---- Ringing a tone ----

// Starts (or restarts, for looping) the HTTP download of the ringing tone
static bool openTone() {
  http.end();
  toneBytesLeft = 0;
  http.begin("http://" + serverAddress().toString() + ":" + String(SERVER_PORT) + "/tones/" + ringingTone);
  if (http.GET() != HTTP_CODE_OK) return false;
  toneStream = http.getStreamPtr();
  uint8_t header[44];  // standard WAV header, as written by the web page
  if (toneStream->readBytes(header, sizeof(header)) != sizeof(header)) return false;
  toneBytesLeft = http.getSize() - sizeof(header);
  return toneBytesLeft > 0;
}

void toneStop() {
  if (ringingTone.isEmpty()) return;
  http.end();
  ringingTone = "";
  i2s_zero_dma_buffer(I2S_PORT);
  Serial.println("Ringing stopped");
  netSend("{\"type\":\"stopped\"}");
}

void toneStart(const String &tone) {
  talkStop();  // an alarm wins over live talk
  toneStop();
  ringingTone = tone;
  ringStart = millis();
  Serial.println("Ringing: " + tone);
  if (openTone()) netSend("{\"type\":\"ringing\"}");
  else toneStop();
}

// One small chunk per call so the WebSocket keeps being serviced and
// "stop" works mid-ring. Loops the tone until stopped or RING_MAX_MS.
static void pumpTone() {
  if (ringingTone.isEmpty()) return;
  if (millis() - ringStart > RING_MAX_MS) return toneStop();
  if (toneBytesLeft < 2) {  // end of file: play it again
    if (!openTone()) toneStop();
    return;
  }

  size_t avail = toneStream->available();
  if (avail < 2) {
    if (!http.connected()) toneStop();  // server went away mid-stream
    return;
  }

  int16_t mono[256];
  size_t want = min(min(avail, sizeof(mono)), (size_t)toneBytesLeft) & ~(size_t)1;
  int samples = toneStream->read((uint8_t *)mono, want) / 2;
  toneBytesLeft -= samples * 2;
  playSamples(mono, samples);
}

// ---- Live talk ----

void talkStop() {
  if (!talking) return;
  talking = false;
  talkCount = 0;
  i2s_zero_dma_buffer(I2S_PORT);
  Serial.println("Talk stopped");
  netSend("{\"type\":\"talk_stopped\"}");
}

void talkStart() {
  toneStop();
  talkStop();
  talking = true;
  talkPlaying = talkEnding = false;
  talkHead = talkCount = 0;
  Serial.println("Talk started");
  netSend("{\"type\":\"talking\"}");
}

void talkFinish() { talkEnding = true; }

// Little-endian 16-bit samples, read byte-wise (the payload may be unaligned)
void talkPush(const uint8_t *data, size_t len) {
  if (!talking) return;
  for (size_t i = 0; i + 1 < len; i += 2) {
    if (talkCount == TALK_BUF_SAMPLES) {  // full (browser clock slightly fast): drop oldest
      talkHead = (talkHead + 1) % TALK_BUF_SAMPLES;
      talkCount--;
    }
    talkBuf[(talkHead + talkCount) % TALK_BUF_SAMPLES] = (int16_t)(data[i] | (data[i + 1] << 8));
    talkCount++;
  }
}

static void pumpTalk() {
  if (!talking) return;
  if (!talkPlaying) {
    if (talkCount < TALK_PREBUFFER && !talkEnding) return;
    talkPlaying = true;
  }
  if (talkCount == 0) {
    if (talkEnding) talkStop();                                              // played out the last words
    else if (millis() - talkLastPlayed > TALK_DRY_MS) talkPlaying = false;  // starved: buffer up again
    return;
  }

  int16_t mono[256];
  size_t n = min(talkCount, sizeof(mono) / sizeof(mono[0]));
  for (size_t i = 0; i < n; i++) {
    mono[i] = talkBuf[talkHead];
    talkHead = (talkHead + 1) % TALK_BUF_SAMPLES;
  }
  talkCount -= n;
  playSamples(mono, n);
  talkLastPlayed = millis();
}

void audioLoop() {
  pumpTone();
  pumpTalk();
}
