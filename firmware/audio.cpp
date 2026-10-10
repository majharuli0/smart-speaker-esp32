#include "audio.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include "driver/i2s.h"
#include "config.h"
#include "net.h"
#include "tonecache.h"

static Preferences prefs;
static int volume = DEFAULT_VOLUME;
static int32_t volumeGain = 0;  // 0-256 multiplier applied to every sample

static HTTPClient http;
static WiFiClient *toneStream = nullptr;
static bool ringing = false;
static String ringingTone;
static bool beeping = false;  // built-in beep instead of the tone
static uint32_t beepSample = 0;
static bool chiming = false;  // doorbell ding-dong playing
static uint32_t chimeSample = 0;
static int32_t toneBytesLeft = 0;
static File toneFile;           // stored copy of the tone, when there is one
static bool fromFile = false;
static unsigned long ringStart = 0;
static bool fading = false;  // alarm: volume rises from 10% over FADE_IN_MS

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
  // Fading alarm: 10% of the set volume at first, full after FADE_IN_MS
  int32_t fade = 256;
  if (fading && ringing) {
    unsigned long t = millis() - ringStart;
    fade = t >= FADE_IN_MS ? 256 : 26 + (int32_t)(230 * t / FADE_IN_MS);
  }
  for (size_t i = 0; i < n; i++) stereo[2 * i] = stereo[2 * i + 1] = (int32_t)applyVolume(mono[i]) * fade >> 8;
  size_t written;
  i2s_write(I2S_PORT, stereo, n * 4, &written, portMAX_DELAY);
}

void audioSetup() {
  i2sSetup();
  prefs.begin("audio");
  applyVolumeSetting(prefs.getUChar("vol", DEFAULT_VOLUME));
}

// ---- Ringing a tone ----

// Starts (or restarts, for looping) the ringing tone: the copy stored on the
// device if there is one (works offline, loops with no gap), else a download
static bool openTone() {
  http.end();
  toneBytesLeft = 0;
  if (ringingTone.isEmpty()) return false;
  fromFile = false;
  if (toneFile) toneFile.close();
  // Stored copy: SD card first, then built-in storage
  if (cacheOpen(ringingTone, toneFile) && toneFile.seek(44)) {  // skip the 44-byte WAV header
    fromFile = true;
    toneBytesLeft = toneFile.size() - 44;
    return toneBytesLeft > 0;
  }
  http.setConnectTimeout(TONE_TIMEOUT_MS);  // don't hang when offline: beep instead
  http.setTimeout(TONE_TIMEOUT_MS);
  http.begin("http://" + serverAddress().toString() + ":" + String(SERVER_PORT) + "/tones/" + ringingTone);
  feedLoopWDT();  // connect + first data can take up to 2 x TONE_TIMEOUT_MS, near the 5 s watchdog
  if (http.GET() != HTTP_CODE_OK) return false;
  feedLoopWDT();
  toneStream = http.getStreamPtr();
  uint8_t header[44];  // standard WAV header, as written by the web page
  if (toneStream->readBytes(header, sizeof(header)) != sizeof(header)) return false;
  toneBytesLeft = http.getSize() - sizeof(header);
  return toneBytesLeft > 0;
}

// The tone can't be downloaded (no network, server down, file missing):
// ring with a built-in beep rather than stay silent
static void startBeep() {
  http.end();
  beeping = true;
  beepSample = 0;
  Serial.println("Tone unavailable, beeping instead");
}

// 880 Hz beep: 0.25 s on, 0.25 s off
static void pumpBeep() {
  int16_t mono[256];
  for (int i = 0; i < 256; i++, beepSample++) {
    bool on = (beepSample % (SAMPLE_RATE / 2)) < SAMPLE_RATE / 4;
    mono[i] = on ? (int16_t)(12000 * sinf(2 * PI * 880 * beepSample / SAMPLE_RATE)) : 0;
  }
  playSamples(mono, 256);
}

void toneStop() {
  if (!ringing) return;
  ringing = beeping = false;
  http.end();
  if (toneFile) toneFile.close();
  fromFile = false;
  ringingTone = "";
  i2s_zero_dma_buffer(I2S_PORT);
  Serial.println("Ringing stopped");
  netSend("{\"type\":\"stopped\"}");
}

void toneStart(const String &tone, bool fadeIn) {
  chiming = false;
  talkStop();  // an alarm wins over live talk
  toneStop();
  ringing = true;
  ringingTone = tone;
  ringStart = millis();
  fading = fadeIn;
  if (!openTone()) startBeep();
  Serial.println("Ringing: " + tone + (beeping ? "" : fromFile ? "" : " (streaming)"));
  netSend("{\"type\":\"ringing\"}");
}

// One small chunk per call so the WebSocket keeps being serviced and
// "stop" works mid-ring. Loops the tone until stopped or RING_MAX_MS.
static void pumpTone() {
  if (!ringing) return;
  if (millis() - ringStart > RING_MAX_MS) return toneStop();
  if (beeping) return pumpBeep();
  if (toneBytesLeft < 2) {  // end of file: play it again
    if (!openTone()) startBeep();
    return;
  }

  if (fromFile) {
    int16_t mono[256];
    size_t want = min(sizeof(mono), (size_t)toneBytesLeft) & ~(size_t)1;
    int samples = toneFile.read((uint8_t *)mono, want) / 2;
    if (samples <= 0) {  // read failed before the end: card pulled out? Check it, then reopen
      cacheCheckNow();    // (from built-in storage if the card is gone)
      toneBytesLeft = 0;
      return;
    }
    toneBytesLeft -= samples * 2;
    playSamples(mono, samples);
    return;
  }

  size_t avail = toneStream->available();
  if (avail < 2) {
    if (!http.connected()) startBeep();  // network or server went away mid-stream
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
  chiming = false;
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

// ---- Doorbell chime ----

// One bell strike: the note plus a slightly inharmonic overtone (what makes a
// bell sound like a bell, not a beep), fading out
static float bell(float hz, float t) {
  if (t < 0) return 0;
  return (sinf(2 * PI * hz * t) + 0.35f * sinf(2 * PI * hz * 2.76f * t)) * expf(-3.5f * t);
}

// "Ding" (E5) then "dong" (C5) half a second later; built in, so it needs no file or network
void chimeStart() {
  if (ringing || talking) return;  // an alarm or talk is playing: don't talk over it
  chiming = true;
  chimeSample = 0;
}

static void pumpChime() {
  if (!chiming) return;
  const uint32_t length = SAMPLE_RATE * 2;  // 2 s, by when the dong has faded
  int16_t mono[256];
  for (int i = 0; i < 256; i++, chimeSample++) {
    float t = (float)chimeSample / SAMPLE_RATE;
    float v = 0.5f * bell(659.25f, t) + 0.5f * bell(523.25f, t - 0.55f);
    mono[i] = (int16_t)(constrain(v, -1.0f, 1.0f) * 20000);
  }
  playSamples(mono, 256);
  if (chimeSample >= length) {
    chiming = false;
    i2s_zero_dma_buffer(I2S_PORT);
  }
}

void audioLoop() {
  pumpTone();
  pumpTalk();
  pumpChime();
}

bool audioBusy() { return ringing || talking || chiming; }
bool isRinging() { return ringing; }
String ringingToneName() { return ringingTone; }
