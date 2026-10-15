#include "mic.h"
#include "driver/i2s.h"
#include "config.h"
#include "net.h"
#include "audio.h"
#include "wake.h"
#include "wakecore.h"
#include "esp_aec.h"
#include "esp_timer.h"

static const size_t PIECE = SAMPLE_RATE * 12 / 100;  // 120 ms a message (6 Opus frames of 20 ms on the audio link)
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
static volatile bool stopQuiet = false;
// A conversation's turn: the person's (waiting for them, or them speaking), the
// AI working out its answer, or the AI speaking
enum Turn { WAITING, SPEAKING, THINKING, REPLYING };
static volatile Turn turn = WAITING;
static const uint32_t TURN_MAX_MS = 20000;   // a turn this long is ended anyway
static const uint32_t THINK_MAX_MS = 12000;  // no answer this long: the person's turn again
// Interrupting (barge-in): while the speaker plays, the mic hears it through
// echo cancellation, and what's left of it mustn't pass for the person. So
// over the speaker, speech must be (for BARGE_RUN windows):
// - BARGE_X louder than usual, against the room's background (not the
//   cancelled audio's: the canceller makes that near silence while it plays);
// - mostly kept by the canceller: its own echo it takes away (by 15-30 dB),
//   a person's voice it doesn't (double-talk);
// - not in the first BARGE_GRACE_MS of the sound: the canceller is still
//   catching up with it then.
static const float BARGE_X = 4;               // +6 dB: 16 dB above the background
static const uint32_t BARGE_RUN = 6;          // 300 ms
static const float BARGE_KEPT = 0.2f;         // the cancelled audio is at least this much of the mic's (-7 dB)
static const uint32_t BARGE_GRACE_MS = 800;
static volatile bool bargeIn = false;  // the person spoke over the AI: the main loop cuts it (talkStop)

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

// The last few seconds the mic heard (PSRAM). After the wake word the server's
// mic_record says afterWake, and the recording starts where the word started:
// the AI checks it really was the wake word (the speaker's own match is loose),
// and a request said straight after it isn't lost while the server gets ready
static const uint32_t RING = SAMPLE_RATE * 6;
static int16_t *ring = nullptr;
static float *bandRing = nullptr;  // the same, in the speech band (for judging speech)
static volatile uint32_t written = 0;        // samples heard since power-on
static volatile uint32_t wakeMark = 0;       // where the last wake word started
static volatile unsigned long wakeTime = 0;  // and when it was heard (0: never)
static const uint32_t WORD_END = SAMPLE_RATE * 45 / 100;  // a word is over after 450 ms of quiet
static const uint32_t BEFORE_WORD = SAMPLE_RATE * 15 / 100;  // and a little before it, to be safe
static const unsigned long AFTER_WAKE_MS = 4000;  // the server asks within this, or it's too old

// Echo cancellation (ESP-SR): the mic minus what the amp played, lined up by
// the two sample counters (see AEC_DELAY_CAL). Made once, at startup.
static aec_handle_t *aec = nullptr;
static int aecChunk = 0;
static int16_t *aecIn = nullptr, *aecRef = nullptr, *aecOut = nullptr;
// (rx - tx) right after each mic read. The two ports' clocks keep it the same
// while this task keeps up; when the task is late (busy, or the startup rush)
// it reads old samples and the value comes out lower. So the steady one is the
// highest over the last second, not the lowest since boot (that one moved by
// ~60 ms from boot to boot).
static volatile int32_t snapSteady = INT32_MIN;  // the highest in the last whole second
static int32_t snapWindow = INT32_MIN;           // the highest so far in this second
static int snapReads = 0;
static const int SNAP_READS = SAMPLE_RATE / BLOCK;  // a second of reads
static const int32_t AEC_MARGIN = 384;  // the reference leads the echo by 24 ms (the filter needs it ahead; ±16 ms slack)

static bool aecReady() {
  if (aec) return true;
  Serial.printf("Echo cancellation: starting (free: %u internal, %u PSRAM; stack left %u)\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  aec_config_t cfg = {};
  cfg.mic_num = 1;
  cfg.ref_num = 1;
  cfg.out_num = 1;
  cfg.filter_length = 4;
  cfg.sample_rate = SAMPLE_RATE;
  // PSRAM: in internal RAM it takes ~124 KB, and Wi-Fi then can't start
  // (and the library crashes, rather than failing, when internal RAM runs out)
  size_t internalBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t psramBefore = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const bool inside = false;
  cfg.caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  cfg.mode = AEC_MODE_FD_LOW_COST;  // measured: removes as much as FD_HIGH_PERF here, 10.1 ms a 32 ms chunk (vs 13.6)
  cfg.nlp_level = AEC_NLP_LEVEL_AGGR;
  aec = aec_create_from_config(&cfg);
  if (!aec) return false;
  aecChunk = aec_get_chunksize(aec);
  Serial.printf("Echo cancellation in %s RAM: it took %u internal + %u PSRAM bytes\n", inside ? "internal" : "PSRAM",
                (unsigned)(internalBefore - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                (unsigned)(psramBefore - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  aecIn = (int16_t *)heap_caps_aligned_alloc(16, aecChunk * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  aecRef = (int16_t *)heap_caps_aligned_alloc(16, aecChunk * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  aecOut = (int16_t *)heap_caps_aligned_alloc(16, aecChunk * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  Serial.printf("Echo cancellation ready: %d-sample chunks\n", aecChunk);
  return aecIn && aecRef && aecOut;
}

static bool wakeFresh() { return ring && bandRing && wakeTime && millis() - wakeTime < AFTER_WAKE_MS; }

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
  // The room's background, in the speech band (wake::SpeechBand): the quietest
  // recent 50 ms window (it drops at once to a quieter one and rises ~2 dB a
  // second), so someone talking from the very start isn't taken for the background
  wake::SpeechBand band;
  float windowSum = 0;
  uint32_t windowCount = 0;
  float floor = -1;
  // The session under way. Speech or quiet, per 50 ms window in the speech band
  // (thresholds in wakecore.h, shared with the wake word).
  MicSession s = {};
  bool on = false, capturing = false;
  uint32_t gap = 0, want = 0, total = 0, clipped = 0, pieces = 0, dropped = 0, loudRun = 0, quietMs = 0;
  size_t fill = 0;
  bool heard = false, silenceEnded = false;
  float sessionSum = 0;
  int64_t levelSum = 0;
  uint32_t sessionCount = 0, levelCount = 0;
  int32_t levelPeak = 0;
  uint32_t rxStart = 0;  // the first captured sample's number (rx index)
  wake::SpeechBand aecBand;       // the speech band of the cancelled audio
  int aecFill = 0;                // mic samples waiting for a whole chunk
  uint32_t aecRx0 = 0;            // the first one's rx index
  uint64_t aecUs = 0;             // time spent cancelling, and in how many chunks
  uint32_t aecChunks = 0;
  bool aecOn = false;
  bool playing = false;           // the speaker is making a sound, or just was
  float rawPower = 0;             // the mic's latest 50 ms window (speech band), before cancelling
  float outWin[BARGE_RUN] = {}, rawWin[BARGE_RUN] = {};  // the last windows over the speaker: cancelled, and not
  uint32_t playMs = 0;            // how long the speaker has been playing
  double replyOut = 0, replyRaw = 0;  // over this sound: how much the canceller took away (logged)
  float echoPeak = 0;             // the loudest window while playing (x background): for tuning BARGE_X
  uint32_t barges = 0;
  uint32_t turnMs = 0;  // in this turn (waiting, speaking or thinking)
  bool idleEnded = false;
  char json[360];

  auto turnEvent = [&](const char *state) {
    snprintf(json, sizeof(json), "{\"type\":\"turn\",\"state\":\"%s\",\"pieces\":%lu}", state,
             (unsigned long)pieces);
    netSendNow(json);
  };
  auto flush = [&]() {  // send the piece being filled now (the end of a turn)
    if (!fill) return;
    if (netSendMic((const uint8_t *)piece, fill * 2)) pieces++;
    else dropped++;
    fill = 0;
  };

  // One sample of the session (and its speech-band value): sent, and judged speech or quiet
  auto capture = [&](int16_t y, float banded, bool over) {
    if (!total) rxStart = written - 1;
    total++;
    if (over) clipped++;
    piece[fill++] = y;
    if (fill == PIECE) {
      if (netSendMic((const uint8_t *)piece, sizeof(piece))) pieces++;
      else dropped++;  // not connected: lost, never piled up
      fill = 0;
    }
    sessionSum += banded * banded;
    if (++sessionCount == WINDOW) {
      float power = sessionSum / WINDOW;
      sessionSum = 0;
      sessionCount = 0;
      float fl = floor;
      bool strict = aecOn && playing;  // over the speaker's own sound
      float x = strict ? BARGE_X : 1;
      float kept = 1;  // over the last BARGE_RUN windows: the cancelled audio / the mic's
      if (strict) {
        if (fl > 0 && power / fl > echoPeak) echoPeak = power / fl;
        float o = 0, r = 0;
        for (uint32_t k = BARGE_RUN - 1; k > 0; k--) {
          outWin[k] = outWin[k - 1];
          rawWin[k] = rawWin[k - 1];
        }
        outWin[0] = power;
        rawWin[0] = rawPower;
        for (uint32_t k = 0; k < BARGE_RUN; k++) {
          o += outWin[k];
          r += rawWin[k];
        }
        kept = r > 0 ? o / r : 1;
        playMs += 50;
        replyOut += power;
        replyRaw += rawPower;
      } else if (playMs) {
        if (replyOut > 0)
          Serial.printf("Echo cancellation: %.1f dB taken away over %lu ms of sound\n", 10 * log10(replyRaw / replyOut),
                        (unsigned long)playMs);
        playMs = 0;
        replyOut = replyRaw = 0;
        memset(outWin, 0, sizeof(outWin));
        memset(rawWin, 0, sizeof(rawWin));
      }
      bool loud = power > fl * wake::SPEECH_X * x && power > wake::SPEECH_MIN * x;
      if (strict && (kept < BARGE_KEPT || playMs < BARGE_GRACE_MS)) loud = false;  // its own echo, or too early to tell
      loudRun = loud ? loudRun + 1 : 0;
      uint32_t run = strict ? BARGE_RUN : 2;  // 100 ms of it (300 ms over the speaker): someone is speaking
      if (loudRun >= run) heard = true;
      if (power < fl * wake::QUIET_X) {
        if (heard) quietMs += 50;
      } else quietMs = 0;
      if (s.conversation) {
        turnMs += 50;
        if (turn != SPEAKING && loudRun >= run) {  // they speak (again, if the AI is still thinking; over it, if it's speaking)
          if (turn == REPLYING) {
            bargeIn = true;
            barges++;
            Serial.printf("Barge-in: %.1f dB over the background, %.1f dB kept, %lu ms into the sound\n",
                          10 * log10f(power / fl), 10 * log10f(kept), (unsigned long)playMs);
          }
          turn = SPEAKING;
          turnMs = quietMs = 0;
          turnEvent("speaking");
        } else if (turn == SPEAKING && (quietMs >= (uint32_t)s.endSilenceMs || turnMs >= TURN_MAX_MS)) {
          flush();
          turn = THINKING;
          turnMs = 0;
          turnEvent("done");
        } else if (turn == WAITING && s.idleMs && turnMs >= (uint32_t)s.idleMs) {
          idleEnded = true;  // nothing said on their turn: that's all
        } else if (turn == THINKING && turnMs >= THINK_MAX_MS) {
          turn = WAITING;  // no answer came: their turn again
          turnMs = 0;
          cueStart();
        }
      } else if (s.endSilenceMs && heard && quietMs >= (uint32_t)s.endSilenceMs) silenceEnded = true;
    }
    if (s.live || s.conversation) return;
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
      sessionSum = 0;
      levelSum = levelPeak = sessionCount = levelCount = 0;
      turn = WAITING;
      turnMs = 0;
      idleEnded = false;
      wakeSessionStart(s.wakeTake, s.wakeTest);
      // Echo cancellation: asked for, or in a conversation if interrupting is on (BARGE_IN)
      aecOn = (s.aec || (s.conversation && BARGE_IN)) && snapSteady != INT32_MIN && aecReady();
      aecFill = 0;
      aecUs = aecChunks = 0;
      echoPeak = 0;
      barges = 0;
      bargeIn = false;
      // Straight after the wake word: from where it ended, at once
      if (s.afterWake && wakeFresh()) {
        capturing = true;
        uint32_t from = written - wakeMark > RING ? written - RING : wakeMark;
        for (uint32_t i = from; i != written && !silenceEnded && total < want; i++)
          capture(ring[i % RING], bandRing[i % RING], false);
        wakeTime = 0;
      }
    }

    size_t got = 0;
    if (i2s_read(MIC_PORT, frames, sizeof(frames), &got, pdMS_TO_TICKS(100)) != ESP_OK || !got) continue;
    size_t n = got / 8;
    {
      int32_t snap = (int32_t)(written - audioTxIndex());
      if (snap > snapWindow) snapWindow = snap;
      if (++snapReads >= SNAP_READS) {
        snapSteady = snapWindow;
        snapWindow = INT32_MIN;
        snapReads = 0;
      }
    }
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
    // A conversation: the AI speaking is its turn; when it's done, a beep, and
    // it's the person's turn again
    if (on && s.conversation) {
      if (isTalking() && turn != SPEAKING) turn = REPLYING;  // (not when they've just spoken over it: it's being cut)
      else if (turn == REPLYING && !audioBusy()) {
        turn = WAITING;
        turnMs = 0;
        cueStart();
      }
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
      float banded = band.push(y);
      if (ring) {
        ring[written % RING] = (int16_t)y;
        bandRing[written % RING] = banded;
      }
      written = written + 1;
      bool echoFree = speakerQuiet >= ECHO;
      if (speakerQuiet < ECHO) speakerQuiet++;
      playing = !echoFree;
      if (echoFree && wakeHear((int16_t)y)) {  // the wake word: it ended a moment ago
        uint32_t back = WORD_END + (uint32_t)wakeHeardFrames() * wake::HOP + BEFORE_WORD;
        wakeMark = written > back ? written - back : 0;
        wakeTime = millis() | 1;
      }

      windowSum += banded * banded;
      if (++windowCount == WINDOW) {
        float power = windowSum / WINDOW;
        windowSum = 0;
        windowCount = 0;
        rawPower = power;
        if (!playing) floor = floor < 0 || power < floor ? power : floor * 1.023f;  // the room's, not the speaker's
      }
      if (!on || !capturing || silenceEnded || idleEnded || total >= want) continue;
      // The caller's or the AI's voice is playing: send nothing (with echo
      // cancellation a conversation goes on: the person may speak over it)
      if ((s.live || (s.conversation && !aecOn)) && !echoFree) {
        total++;
        continue;
      }
      if (!aecOn) {
        capture((int16_t)y, banded, over);
        continue;
      }
      // Echo cancellation: whole chunks of mic audio, each with what the amp played
      if (!aecFill) aecRx0 = written - 1;
      aecIn[aecFill++] = (int16_t)y;
      if (aecFill == aecChunk) {
        aecFill = 0;
        uint32_t tx = aecRx0 - (uint32_t)(snapSteady + AEC_DELAY_CAL) + AEC_MARGIN;
        audioReference(tx, aecRef, aecChunk);
        int64_t t0 = esp_timer_get_time();
        aec_process(aec, aecIn, aecRef, aecOut);
        aecUs += esp_timer_get_time() - t0;
        aecChunks++;
        for (int k = 0; k < aecChunk; k++) capture(aecOut[k], aecBand.push(aecOut[k]), false);
      }
    }

    if (on && (stopAsked || silenceEnded || idleEnded || total >= want)) {
      if (fill) {  // the last, shorter piece
        if (netSendMic((const uint8_t *)piece, fill * 2)) pieces++;
        else dropped++;
      }
      const char *endedBy = stopAsked ? "stop" : silenceEnded ? "silence" : idleEnded ? "idle" : "time";
      int len = snprintf(json, sizeof(json),
                         "{\"type\":\"mic_done\",\"samples\":%lu,\"pieces\":%lu,\"dropped\":%lu,\"clipped\":%lu,"
                         "\"slot\":\"%s\",\"gainShift\":%d,\"speech\":%s,\"endedBy\":\"%s\",\"floorDb\":%.1f",
                         (unsigned long)total, (unsigned long)pieces, (unsigned long)dropped, (unsigned long)clipped,
                         right ? "right" : "left", MIC_GAIN_SHIFT, heard ? "true" : "false", endedBy,
                         floor > 0 ? 10 * log10f(floor / (32768.0f * 32768.0f)) : -120.0f);
      len += wakeSessionEnd(json + len, sizeof(json) - len - 2);  // a wake word take or test: its result
      uint32_t tx;
      if (s.echoTest && sweepTxIndex(&tx))
        len += snprintf(json + len, sizeof(json) - len - 2,
                        ",\"echo\":{\"rxStart\":%lu,\"sweepTx\":%lu,\"snap\":%ld,\"aec\":%s,\"aecChunk\":%d,"
                        "\"aecUsPerChunk\":%lu}",
                        (unsigned long)rxStart, (unsigned long)tx, (long)snapSteady, aecOn ? "true" : "false", aecChunk,
                        (unsigned long)(aecChunks ? aecUs / aecChunks : 0));
      else if (s.conversation && aecOn)  // how interrupting went: for tuning BARGE_X
        len += snprintf(json + len, sizeof(json) - len - 2,
                        ",\"aec\":{\"barges\":%lu,\"echoPeakDb\":%.1f,\"usPerChunk\":%lu}", (unsigned long)barges,
                        echoPeak > 0 ? 10 * log10f(echoPeak) : 0.0f, (unsigned long)(aecChunks ? aecUs / aecChunks : 0));
      snprintf(json + len, sizeof(json) - len, "}");
      netSend(json);
      Serial.printf("Microphone session over (%s): %lu samples, %lu clipped\n", endedBy, (unsigned long)total,
                    (unsigned long)clipped);
      if (s.conversation && !(stopAsked && stopQuiet)) cueStart(true);  // "that's all"
      on = false;
      turn = WAITING;
      running = false;
    }
  }
}

void micSetup() {
  aecReady();  // now, not on first use: making it takes ~2 s (the mic would lose that much)
  ring = (int16_t *)heap_caps_malloc(RING * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  bandRing = (float *)heap_caps_malloc(RING * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!bandRing) ring = nullptr;  // both or neither
  portOk = portStart();
  if (!portOk) {
    Serial.println("Microphone: its I2S port didn't start");
    return;
  }
  // Its own task, so a busy main loop doesn't lose audio; core 1, like the loop (Wi-Fi is on core 0)
  xTaskCreatePinnedToCore(micTask, "mic", 12288, nullptr, 3, nullptr, 1);  // echo cancellation runs here too
}

void micLoop() {
  if (!bargeIn) return;
  bargeIn = false;
  talkStop();  // at once; what the server still sends is dropped until its next talk_start
}

bool micRecord(const MicSession &s) {
  if (running) return false;  // one session at a time
  if (!portOk) {
    netSend("{\"type\":\"mic_done\",\"error\":\"the microphone's I2S port didn't start\"}");
    return false;
  }
  asked = s;
  asked.seconds = constrain(s.seconds, 1, s.live || s.conversation ? MIC_LIVE_MAX_S : MIC_TEST_MAX_S);
  asked.endSilenceMs = s.live ? 0 : constrain(s.endSilenceMs, 0, 5000);
  asked.idleMs = constrain(s.idleMs, 0, 60000);
  toneStop();               // a quiet room: nothing plays while it listens…
  if (!s.live) talkStop();  // …except, in a call, the caller
  // "I'm listening", or for a call "the room can be heard now" (no beep after
  // the wake word: it sends from the word on, and the server answers if it was)
  asked.afterWake = s.afterWake && wakeFresh();
  if ((asked.endSilenceMs || s.live) && !asked.afterWake) cueStart();
  if (s.echoTest) sweepStart();  // plays while it records
  stopAsked = false;
  stopQuiet = false;
  running = true;
  askedNew = true;
  Serial.printf("Microphone session: up to %d s%s%s\n", asked.seconds, s.live ? ", a call" : "",
                asked.endSilenceMs ? ", until the room is quiet again" : "");
  return true;
}

void micStop(bool quiet) {
  if (!running) return;
  stopQuiet = quiet;
  stopAsked = true;
}

bool micBusy() { return running; }

MicState micState() {
  if (!running) return MIC_OFF;
  return turn == THINKING ? MIC_THINKING : MIC_LISTENING;
}
