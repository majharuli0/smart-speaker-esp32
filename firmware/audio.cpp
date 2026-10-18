#include "audio.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include "driver/i2s.h"
#include "config.h"
#include "net.h"
#include "tonecache.h"
#include "files.h"
#include "audiostats.h"

AudioCounters audioStats = {};

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
static bool cueing = false;   // the "I'm listening" cue playing
static bool cueFalling = false;  // …or the "that's all" one
static uint32_t cueSample = 0;
static int32_t toneBytesLeft = 0;
static File toneFile;           // stored copy of the tone, when there is one
static bool fromFile = false;
static unsigned long ringStart = 0;
static bool fading = false;  // alarm: volume rises from 10% over FADE_IN_MS
static bool music = false;    // played from the file manager: no time limit, repeat or once
static bool repeatOn = true;  // music starts again at the end (saved setting)
static int readRetries = 0;   // reopening a file after a failed read

// ---- Output ----
// Everything played goes into playRing (mono, volume applied); the output task
// moves it to the amp in 16 ms blocks, with silence when there's nothing, so
// the amp never runs dry and the delay from here to the loudspeaker stays the
// same. Each block played is also kept in refRing, numbered (txIndex): what the
// mic hears back from the loudspeaker, for echo cancellation.
static const size_t PLAY_RING = 2048;    // 128 ms waiting to play
static const uint32_t REF_RING = 32768;  // the last ~2 s played
static const size_t BLOCK = 256;         // 16 ms
static int16_t *playRing = nullptr;
static volatile size_t playHead = 0, playCount = 0;
static portMUX_TYPE playMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t pushed = 0;     // samples ever put in playRing
static int16_t *refRing = nullptr;
static volatile uint32_t txIndex = 0;    // samples ever sent to the amp (silence too)
// The echo test's sweep: which pushed sample it starts at, and so which tx index
static volatile uint32_t sweepPush = 0, sweepTx = 0;
static volatile bool sweepPushed = false, sweepPlayed = false;

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

void setRepeat(bool on) {
  repeatOn = on;
  prefs.putBool("repeat", on);
}

void sendRepeat() {
  netSend(repeatOn ? "{\"type\":\"repeat\",\"on\":true}" : "{\"type\":\"repeat\",\"on\":false}");
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
    .dma_buf_count = 4,  // 64 ms; the output task keeps it full
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

// Mono samples, volume applied, into playRing (blocks only while it's full,
// i.e. at playback speed, as writing to the amp did)
static void playSamples(const int16_t *mono, size_t n) {
  // Fading alarm: 10% of the set volume at first, full after FADE_IN_MS
  int32_t fade = 256;
  if (fading && ringing) {
    unsigned long t = millis() - ringStart;
    fade = t >= FADE_IN_MS ? 256 : 26 + (int32_t)(230 * t / FADE_IN_MS);
  }
  for (size_t i = 0; i < n; i++) {
    while (playCount >= PLAY_RING) vTaskDelay(1);  // full: wait for the output task
    int16_t v = (int32_t)applyVolume(mono[i]) * fade >> 8;
    taskENTER_CRITICAL(&playMux);
    playRing[(playHead + playCount) % PLAY_RING] = v;
    playCount = playCount + 1;
    pushed = pushed + 1;
    taskEXIT_CRITICAL(&playMux);
  }
}

// Stop at once: what's waiting is dropped (the amp still plays its last 64 ms)
static void playFlush() {
  taskENTER_CRITICAL(&playMux);
  playHead = (playHead + playCount) % PLAY_RING;
  playCount = 0;
  taskEXIT_CRITICAL(&playMux);
}

static void outputTask(void *) {
  static int16_t mono[BLOCK];
  static int16_t stereo[BLOCK * 2];
  for (;;) {
    taskENTER_CRITICAL(&playMux);
    size_t n = playCount < BLOCK ? playCount : BLOCK;
    uint32_t first = pushed - playCount;  // the pushed number of the first one taken
    for (size_t i = 0; i < n; i++) mono[i] = playRing[(playHead + i) % PLAY_RING];
    playHead = (playHead + n) % PLAY_RING;
    playCount = playCount - n;
    taskEXIT_CRITICAL(&playMux);
    for (size_t i = n; i < BLOCK; i++) mono[i] = 0;  // nothing to play: silence
    if (n < BLOCK && talking && talkPlaying) audioStats.underruns++;  // talk was playing but ran short
    if (sweepPushed && !sweepPlayed && sweepPush - first < n) {
      sweepTx = txIndex + (sweepPush - first);
      sweepPlayed = true;
    }
    for (size_t i = 0; i < BLOCK; i++) {
      stereo[2 * i] = stereo[2 * i + 1] = mono[i];
      refRing[(txIndex + i) % REF_RING] = mono[i];
    }
    size_t written;
    i2s_write(I2S_PORT, stereo, sizeof(stereo), &written, portMAX_DELAY);
    txIndex = txIndex + BLOCK;
  }
}

uint32_t audioTxIndex() { return txIndex; }

void audioReference(uint32_t from, int16_t *out, size_t n) {
  for (size_t i = 0; i < n; i++) out[i] = refRing[(from + i) % REF_RING];
}

// ---- The echo test: a 2 s sweep, 150 Hz to 7 kHz ----
static bool sweeping = false;
static uint32_t sweepSample = 0;
static const uint32_t SWEEP_SAMPLES = SAMPLE_RATE * 2;

void sweepStart() {
  taskENTER_CRITICAL(&playMux);
  sweepPush = pushed;  // the next sample pushed is the sweep's first
  sweepPushed = true;
  sweepPlayed = false;
  taskEXIT_CRITICAL(&playMux);
  sweepSample = 0;
  sweeping = true;
}

bool sweepTxIndex(uint32_t *tx) {
  if (!sweepPlayed) return false;
  *tx = sweepTx;
  return true;
}

static void pumpSweep() {
  if (!sweeping) return;
  int16_t mono[BLOCK];
  const float f0 = 150, f1 = 7000, T = (float)SWEEP_SAMPLES / SAMPLE_RATE, k = logf(f1 / f0);
  for (size_t i = 0; i < BLOCK; i++, sweepSample++) {
    float t = (float)sweepSample / SAMPLE_RATE;
    float phase = 2 * PI * f0 * T / k * (expf(t * k / T) - 1);  // exponential sweep
    mono[i] = sweepSample < SWEEP_SAMPLES ? (int16_t)(sinf(phase) * 16000) : 0;
  }
  playSamples(mono, BLOCK);
  if (sweepSample >= SWEEP_SAMPLES) sweeping = false;
}

void audioSetup() {
  i2sSetup();
  playRing = (int16_t *)heap_caps_malloc(PLAY_RING * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  refRing = (int16_t *)heap_caps_malloc(REF_RING * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  // Core 1, above the mic task: the amp must never wait
  xTaskCreatePinnedToCore(outputTask, "audio out", 4096, nullptr, 5, nullptr, 1);
  prefs.begin("audio");
  applyVolumeSetting(prefs.getUChar("vol", DEFAULT_VOLUME));
  repeatOn = prefs.getBool("repeat", true);
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
  // One of the user's files, or the stored copy of a tone (SD card first, then built-in)
  bool opened = isFileSound(ringingTone) ? filesOpen(ringingTone, toneFile) : cacheOpen(ringingTone, toneFile);
  if (opened && toneFile.seek(44)) {  // skip the 44-byte WAV header
    fromFile = true;
    toneBytesLeft = toneFile.size() - 44;
    return toneBytesLeft > 0;
  }
  http.setConnectTimeout(TONE_TIMEOUT_MS);  // don't hang when offline: beep instead
  http.setTimeout(TONE_TIMEOUT_MS);
  if (isFileSound(ringingTone)) return false;  // a file that isn't there: beep instead
  if (serverHttp().isEmpty()) return false;  // download address not known yet (server not seen)
  http.begin(serverHttp() + "/tones/" + ringingTone);
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
  playFlush();
  Serial.println("Ringing stopped");
  netSend("{\"type\":\"stopped\"}");
}

void toneStart(const String &tone, bool fadeIn, bool playMusic) {
  chiming = false;
  talkStop();  // an alarm wins over live talk
  toneStop();
  ringing = true;
  ringingTone = tone;
  ringStart = millis();
  fading = fadeIn;
  music = playMusic;
  readRetries = 0;
  if (!openTone()) {
    if (music) {  // a file that can't be read: just stop
      toneStop();
      return;
    }
    startBeep();
  }
  Serial.println("Ringing: " + tone + (beeping ? "" : fromFile ? "" : " (streaming)"));
  netSend("{\"type\":\"ringing\"}");
}

// One small chunk per call so the WebSocket keeps being serviced and
// "stop" works mid-ring. Loops the tone until stopped or RING_MAX_MS (alarms);
// music has no time limit.
static void pumpTone() {
  if (!ringing) return;
  if (!music && millis() - ringStart > RING_MAX_MS) return toneStop();
  if (beeping) return pumpBeep();
  if (toneBytesLeft < 2) {  // end of file: play it again (music only if repeat is on)
    if (music && !repeatOn) return toneStop();
    if (!openTone()) {
      if (music) return toneStop();
      startBeep();
    }
    return;
  }

  if (fromFile) {
    int16_t mono[256];
    size_t want = min(sizeof(mono), (size_t)toneBytesLeft) & ~(size_t)1;
    int samples = toneFile.read((uint8_t *)mono, want) / 2;
    if (samples <= 0) {  // read failed before the end: a card hiccup, or pulled out?
      cacheCheckNow();    // check it, then carry on from the same place (a tone may
      int32_t left = toneBytesLeft;  // reopen from built-in storage if the card is gone)
      if (++readRetries <= 3 && openTone()) {
        if (fromFile && left < toneBytesLeft && toneFile.seek(toneFile.size() - left)) toneBytesLeft = left;
        return;
      }
      toneBytesLeft = 0;  // give up on this pass: treated as the end of the file
      return;
    }
    readRetries = 0;
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

// cut: stop at once (stop, an alarm); otherwise (all played) the last words
// already waiting for the amp still play
static void talkEnd(bool cut) {
  if (!talking) return;
  talking = false;
  talkCount = 0;
  if (cut) playFlush();
  Serial.println("Talk stopped");
  netSend("{\"type\":\"talk_stopped\"}");
}

void talkStop() { talkEnd(true); }

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
      audioStats.talkDrops++;
    }
    talkBuf[(talkHead + talkCount) % TALK_BUF_SAMPLES] = (int16_t)(data[i] | (data[i + 1] << 8));
    talkCount++;
  }
  if (talkCount > audioStats.talkPeak) audioStats.talkPeak = talkCount;
}

static void pumpTalk() {
  if (!talking) return;
  if (!talkPlaying) {
    if (talkCount < TALK_PREBUFFER && !talkEnding) return;
    talkPlaying = true;
  }
  if (talkCount == 0) {
    if (talkEnding) talkEnd(false);                                              // played out the last words
    else if (millis() - talkLastPlayed > TALK_DRY_MS) {  // starved: buffer up again
      talkPlaying = false;
      audioStats.talkStarves++;
    }
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
    chiming = false;  // its last notes still play out
  }
}

// ---- "I'm listening" cue ----

// Two short rising notes (A5, then E6), 0.26 s: the speaker is about to listen.
// Falling (E6, then A5): the conversation is over. Built in, so it plays at once.
void cueStart(bool falling) {
  chiming = false;
  cueFalling = falling;
  cueing = true;
  cueSample = 0;
}

static void pumpCue() {
  if (!cueing) return;
  const uint32_t length = SAMPLE_RATE * 26 / 100;
  int16_t mono[256];
  for (int i = 0; i < 256; i++, cueSample++) {
    float t = (float)cueSample / SAMPLE_RATE;
    bool second = t >= 0.13f;
    float local = second ? t - 0.13f : t;  // time into this note
    float env = cueSample >= length || (t >= 0.12f && t < 0.13f) ? 0 : min(1.0f, local * 200) * expf(-12 * local);
    bool high = second != cueFalling;
    mono[i] = (int16_t)(sinf(2 * PI * (high ? 1318.5f : 880.0f) * t) * env * 12000);
  }
  playSamples(mono, 256);
  if (cueSample >= length) {
    cueing = false;  // its last notes still play out
  }
}

void audioLoop() {
  pumpTone();
  pumpTalk();
  pumpChime();
  pumpCue();
  pumpSweep();
}

bool audioBusy() { return ringing || talking || chiming || cueing; }
bool isRinging() { return ringing; }
bool isMusic() { return ringing && music; }
bool isTalking() { return talking; }
bool isChiming() { return chiming; }
bool isCueing() { return cueing; }
String ringingToneName() { return ringingTone; }
