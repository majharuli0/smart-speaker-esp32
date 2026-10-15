#include "wake.h"
#include <Arduino.h>
#include <FFat.h>
#include <Preferences.h>
#include <new>
#include "wakecore.h"
#include "net.h"
#include "audio.h"

using wake::TAKES;

static Preferences prefs;
static bool enabled = false;
static int sensitivity = 5;

// In PSRAM (~70 KB). The takes are written only by the mic task (wakeSessionEnd)
// and at setup; the main loop saves them and works out the threshold.
static wake::Listener *listener = nullptr;
static wake::Word *takes = nullptr;      // [TAKES]
static wake::Word *candidate = nullptr;  // the take being recorded: its longest word
static bool have[TAKES];
static volatile float limit = 0;  // a word this close to the takes is the wake word
static float spread = 0;          // how far apart the takes are

// What the mic is hearing: the room (matched against the takes), a take, a test,
// or a voice request or call (ignored)
enum Mode { DETECT, IGNORE, TAKE, TEST };
static volatile Mode mode = DETECT;
static int sessionTake = 0;
static float testScore = wake::NO_MATCH;
static int testFrames = 0;
static volatile int takeReady = 0;   // a new take to save (main loop)
static volatile float matched = -1;  // a match to report (main loop)
static volatile unsigned long quietUntil = 0;  // no matching until then

template <typename T> static T *make(int count) {
  T *p = (T *)heap_caps_malloc(sizeof(T) * count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = (T *)malloc(sizeof(T) * count);
  if (p)
    for (int i = 0; i < count; i++) new (&p[i]) T();
  return p;
}

static bool allTakes() { return have[0] && have[1] && have[2]; }

// The features' version is in the name: takes made with older features (0.22-0.23:
// from 60 Hz) don't compare with new words, so they're left unused (record again)
static String path(int take) { return "/wake/take" + String(take) + "-v2.bin"; }

static void computeLimit() {
  float s = 0;
  limit = allTakes() ? wake::threshold(takes, TAKES, sensitivity, &s) : 0;
  spread = s;
}

// A take in the built-in storage: its frame count, then its MFCCs
static void save(int take) {
  if (!FFat.exists("/wake")) FFat.mkdir("/wake");
  File f = FFat.open(path(take), "w");
  if (!f) return;
  const wake::Word &w = takes[take - 1];
  f.write((const uint8_t *)&w.n, sizeof(w.n));
  f.write((const uint8_t *)w.f, sizeof(w.f[0]) * w.n);
  f.close();
}

static void load(int take) {
  File f = FFat.open(path(take), "r");
  if (!f) return;
  wake::Word &w = takes[take - 1];
  int n = 0;
  bool ok = f.read((uint8_t *)&n, sizeof(n)) == sizeof(n) && n >= wake::MIN_FRAMES && n <= wake::MAX_FRAMES &&
            f.read((uint8_t *)w.f, sizeof(w.f[0]) * n) == sizeof(w.f[0]) * n;
  f.close();
  w.n = ok ? n : 0;
  have[take - 1] = ok;
}

void wakeSetup() {
  prefs.begin("wake");
  enabled = prefs.getBool("on", false);
  sensitivity = prefs.getInt("sens", 5);
  listener = make<wake::Listener>(1);
  takes = make<wake::Word>(TAKES);
  candidate = make<wake::Word>(1);
  if (!listener || !takes || !candidate) {
    Serial.println("Wake word: out of memory");
    listener = nullptr;
    return;
  }
  for (int t = 1; t <= TAKES; t++) load(t);
  computeLimit();
  Serial.printf("Wake word: %s, %d of %d takes, threshold %.1f\n", enabled ? "on" : "off",
                have[0] + have[1] + have[2], TAKES, (float)limit);
}

void wakeReport() {
  char json[220];
  snprintf(json, sizeof(json),
           "{\"type\":\"wake_status\",\"enabled\":%s,\"sensitivity\":%d,\"takes\":[%.2f,%.2f,%.2f],"
           "\"threshold\":%.1f,\"spread\":%.1f,\"listening\":%s}",
           enabled ? "true" : "false", sensitivity, have[0] ? takes[0].n / 100.0f : 0.0f,
           have[1] ? takes[1].n / 100.0f : 0.0f, have[2] ? takes[2].n / 100.0f : 0.0f, (float)limit, spread,
           listener && enabled && limit > 0 ? "true" : "false");
  netSend(json);
}

void wakeConfig(bool on, int sens) {
  enabled = on;
  sensitivity = constrain(sens, 1, 10);
  prefs.putBool("on", enabled);
  prefs.putInt("sens", sensitivity);
  computeLimit();
  wakeReport();
}

void wakeClear() {
  for (int t = 1; t <= TAKES; t++) {
    have[t - 1] = false;
    FFat.remove(path(t));
  }
  computeLimit();
  wakeReport();
}

void wakeLoop() {
  if (takeReady) {
    int take = takeReady;
    takeReady = 0;
    save(take);
    computeLimit();
    Serial.printf("Wake word: take %d kept (%.2f s); threshold %.1f\n", take, takes[take - 1].n / 100.0f,
                  (float)limit);
    wakeReport();
  }
  if (matched >= 0) {
    char json[96];
    snprintf(json, sizeof(json), "{\"type\":\"wake\",\"score\":%.1f,\"threshold\":%.1f}", (float)matched,
             (float)limit);
    matched = -1;
    Serial.println("Wake word heard");
    if (netConnected()) cueStart();  // "I'm listening", at once: say the request now
    netSend(json);
  }
}

bool wakeHear(int16_t sample) {
  if (!listener || !listener->push(sample)) return false;
  const wake::Word &w = listener->word();  // a word just ended
  if (mode == TAKE) {
    if (w.n > candidate->n) *candidate = w;  // the longest word said is the take
    return false;
  }
  bool ready = allTakes() && limit > 0;
  if (mode == TEST) {
    float score = ready ? wake::score(w, takes, TAKES) : wake::NO_MATCH;
    if (!testFrames || score < testScore) {
      testScore = score;
      testFrames = w.n;
    }
    return false;
  }
  if (mode != DETECT || !enabled || !ready || (long)(quietUntil - millis()) > 0) return false;
  float score = wake::score(w, takes, TAKES);
  if (score < limit * 1.3f) Serial.printf("Wake word? %.1f (threshold %.1f)\n", score, (float)limit);
  if (score > limit) return false;
  matched = score;
  quietUntil = millis() + 3000;
  return true;
}

void wakeInterrupt() {
  if (listener) listener->interrupt();
}

void wakeSessionStart(int take, bool test) {
  sessionTake = take >= 1 && take <= TAKES ? take : 0;
  if (candidate) candidate->n = 0;
  testScore = wake::NO_MATCH;
  testFrames = 0;
  mode = sessionTake ? TAKE : test ? TEST : IGNORE;
}

int wakeSessionEnd(char *out, size_t size) {
  Mode was = mode;
  mode = DETECT;
  quietUntil = millis() + 1500;  // the person may still be finishing: don't match it
  if (was == TAKE) {
    if (!candidate || !candidate->n)
      return snprintf(out, size, ",\"wake\":{\"take\":%d,\"ok\":false}", sessionTake);
    takes[sessionTake - 1] = *candidate;
    have[sessionTake - 1] = true;
    takeReady = sessionTake;
    return snprintf(out, size, ",\"wake\":{\"take\":%d,\"ok\":true,\"seconds\":%.2f}", sessionTake,
                    candidate->n / 100.0f);
  }
  if (was == TEST) {
    if (!testFrames) return snprintf(out, size, ",\"wake\":{\"test\":true,\"heard\":false}");
    bool comparable = testScore < wake::NO_MATCH;
    char score[16] = "null";
    if (comparable) snprintf(score, sizeof(score), "%.1f", testScore);
    return snprintf(out, size,
                    ",\"wake\":{\"test\":true,\"heard\":true,\"seconds\":%.2f,\"score\":%s,\"threshold\":%.1f,"
                    "\"match\":%s}",
                    testFrames / 100.0f, score, (float)limit,
                    comparable && limit > 0 && testScore <= limit ? "true" : "false");
  }
  return 0;
}
