#include "wakecore.h"
#include <math.h>
#include <string.h>

namespace wake {

static const float PI_F = 3.14159265f;
static const int BINS = NFFT / 2 + 1;
static const int MELS = 26;  // mel filters, 60 Hz - 7 kHz
static const float LOW_HZ = 150, HIGH_HZ = 7000;  // from 150 Hz: a fan's rumble is below
static const float BIG = 1e30f;
static const int END_QUIET = 45;    // 450 ms of quiet ends a word (a pause between two words is shorter)

// Tables shared by every listener, made once
static float hamming[WINDOW];
static float cosT[NFFT / 2], sinT[NFFT / 2];
static uint16_t bitrev[NFFT];
static int melEdge[MELS + 2];  // FFT bins where the filters start, peak and end
static float dct[CEPS][MELS];  // mel energies → MFCCs 1-12
static bool tablesReady = false;

static float toMel(float hz) { return 2595 * log10f(1 + hz / 700); }
static float fromMel(float mel) { return 700 * (powf(10, mel / 2595) - 1); }

static void makeTables() {
  if (tablesReady) return;
  for (int i = 0; i < WINDOW; i++) hamming[i] = 0.54f - 0.46f * cosf(2 * PI_F * i / (WINDOW - 1));
  for (int i = 0; i < NFFT / 2; i++) {
    cosT[i] = cosf(2 * PI_F * i / NFFT);
    sinT[i] = -sinf(2 * PI_F * i / NFFT);
  }
  for (int i = 0; i < NFFT; i++) {
    int r = 0;
    for (int b = 0; b < 9; b++)  // 512 = 2^9
      if (i & (1 << b)) r |= 1 << (8 - b);
    bitrev[i] = r;
  }
  float lo = toMel(LOW_HZ), hi = toMel(HIGH_HZ);
  for (int i = 0; i < MELS + 2; i++)
    melEdge[i] = (int)floorf((NFFT + 1) * fromMel(lo + (hi - lo) * i / (MELS + 1)) / RATE);
  for (int k = 0; k < CEPS; k++)
    for (int m = 0; m < MELS; m++) dct[k][m] = cosf(PI_F * (k + 1) * (m + 0.5f) / MELS);
  tablesReady = true;
}

// In-place radix-2 FFT of NFFT points
static void fft(float *re, float *im) {
  for (int i = 0; i < NFFT; i++) {
    int j = bitrev[i];
    if (j > i) {
      float t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (int size = 2; size <= NFFT; size <<= 1) {
    int half = size / 2, step = NFFT / size;
    for (int i = 0; i < NFFT; i += size)
      for (int k = 0; k < half; k++) {
        float wr = cosT[k * step], wi = sinT[k * step];
        int a = i + k, b = a + half;
        float tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
        re[b] = re[a] - tr;
        im[b] = im[a] - ti;
        re[a] += tr;
        im[a] += ti;
      }
  }
}

static void butterworth(float b0, float b1, float b2, float a0, float a1, float a2, float *out) {
  out[0] = b0 / a0; out[1] = b1 / a0; out[2] = b2 / a0; out[3] = a1 / a0; out[4] = a2 / a0;
}

SpeechBand::SpeechBand() {
  for (int i = 0; i < 4; i++) {
    bool high = i < 2;
    float w = 2 * PI_F * (high ? 100 : 1000) / RATE, alpha = sinf(w) / (2 * 0.7071f), c = cosf(w);
    float k[5];
    if (high) butterworth((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha, k);
    else butterworth((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + alpha, -2 * c, 1 - alpha, k);
    stages[i].b0 = k[0]; stages[i].b1 = k[1]; stages[i].b2 = k[2]; stages[i].a1 = k[3]; stages[i].a2 = k[4];
  }
}

float SpeechBand::push(float x) {
  for (Stage &s : stages) {
    float y = s.b0 * x + s.b1 * s.x1 + s.b2 * s.x2 - s.a1 * s.y1 - s.a2 * s.y2;
    s.x2 = s.x1; s.x1 = x;
    s.y2 = s.y1; s.y1 = y;
    x = y;
  }
  return x;
}

Listener::Listener() {
  makeTables();
  memset(ring, 0, sizeof(ring));
}

float Listener::floorDb() const {
  return floor > 0 ? 10 * log10f(floor / (32768.0f * 32768.0f)) : -120;
}

void Listener::interrupt() {
  start = -1;
  loudRun = quietRun = 0;
  filled = 0;  // the window fills with new sound before the next frame
}

// The MFCCs of the last 25 ms
void Listener::frame(float *mfcc) {
  for (int i = 0; i < WINDOW; i++) re[i] = ring[(pos + i) % WINDOW] * hamming[i];  // oldest first
  for (int i = WINDOW; i < NFFT; i++) re[i] = 0;
  memset(im, 0, sizeof(im));
  fft(re, im);
  float mel[MELS];
  for (int m = 0; m < MELS; m++) {
    int left = melEdge[m], peak = melEdge[m + 1], right = melEdge[m + 2];
    float sum = 0;
    for (int b = left; b <= right && b < BINS; b++) {
      float w = b < peak ? (float)(b - left) / (peak - left)
                         : right > peak ? (float)(right - b) / (right - peak) : 1;
      sum += w * (re[b] * re[b] + im[b] * im[b]);
    }
    mel[m] = logf(sum + 1);
  }
  for (int k = 0; k < CEPS; k++) {
    float c = 0;
    for (int m = 0; m < MELS; m++) c += dct[k][m] * mel[m];
    mfcc[k] = c;
  }
}

bool Listener::push(int16_t sample) {
  ring[pos] = sample - 0.97f * prev;  // pre-emphasis: the consonants count as much as the vowels
  prev = sample;
  pos = (pos + 1) % WINDOW;
  if (filled < WINDOW) filled++;
  float b = band.push(sample);
  hopPower += b * b;
  if (++sinceFrame < HOP) return false;

  // Every 10 ms: the level of the last 50 ms, and the MFCCs of the last 25 ms
  sinceFrame = 0;
  recent[recentAt] = hopPower / HOP;
  recentAt = (recentAt + 1) % 5;
  hopPower = 0;
  if (filled < WINDOW) return false;
  float level = (recent[0] + recent[1] + recent[2] + recent[3] + recent[4]) / 5;
  long f = frames++;
  frame(history[f % KEEP]);
  levels[f % KEEP] = level;

  // Speech or quiet, as the microphone judges it: in the speech band, against
  // the quietest recent moments (that background rises ~2 dB a second). A
  // stretch is noticed by its speech, and reaches out to the quiet on either
  // side (the soft sounds at the start and end of a word count too).
  floor = floor < 0 || level < floor ? level : floor * 1.00455f;
  bool loud = level > floor * SPEECH_X && level > SPEECH_MIN;
  bool quiet = level < floor * QUIET_X;
  if (start < 0) {
    loudRun = loud ? loudRun + 1 : 0;
    if (loudRun < 10) return false;  // 100 ms of it: someone is speaking
    long s = f - 9;                  // where the speech began…
    while (s > 0 && f - s < 60 && levels[(s - 1) % KEEP] >= floor * QUIET_X) s--;  // …and the sound before it
    start = s >= 5 ? s - 5 : 0;      // from 50 ms before that
    lastHeard = f;
    quietRun = 0;
    tooLong = false;
    return false;
  }
  if (!quiet) lastHeard = f;
  quietRun = quiet ? quietRun + 1 : 0;
  if (f - start >= KEEP - 1) tooLong = true;  // talking, not a wake word: wait for a pause
  if (quietRun < END_QUIET) return false;     // a long enough pause: the stretch has ended
  long last = lastHeard + 5;                  // 50 ms after the last sound
  long n = last - start + 1;
  bool word = !tooLong && n >= MIN_FRAMES && n <= MAX_FRAMES;
  if (word) ended(start, last);
  start = -1;
  loudRun = 0;
  return word;
}

// The stretch's MFCCs → a Word, with their average taken off
void Listener::ended(long first, long last) {
  int n = (int)(last - first + 1);
  out.n = n;
  float mean[CEPS] = {};
  for (int t = 0; t < n; t++) {
    const float *c = history[(first + t) % KEEP];
    for (int k = 0; k < CEPS; k++) {
      out.f[t][k] = c[k];
      mean[k] += c[k] / n;
    }
  }
  for (int t = 0; t < n; t++)
    for (int k = 0; k < CEPS; k++) out.f[t][k] -= mean[k];
}

static float frameDistance(const float *a, const float *b) {
  float s = 0;
  for (int k = 0; k < CEPS; k++) s += (a[k] - b[k]) * (a[k] - b[k]);
  return sqrtf(s);
}

// DTW within a band around the diagonal; steps: one word or the other moves
// on (cost d), or both do (cost 2d); the total over n + m
float distance(const Word &a, const Word &b) {
  int n = a.n, m = b.n;
  if (n < 1 || m < 1 || n > 2 * m || m > 2 * n) return NO_MATCH;
  float rows[2][MAX_FRAMES + 1];
  float *prevRow = rows[0], *row = rows[1];
  int band = (n > m ? n : m) / 4;
  if (band < 10) band = 10;
  for (int j = 0; j <= m; j++) prevRow[j] = BIG;
  prevRow[0] = 0;
  for (int i = 1; i <= n; i++) {
    for (int j = 0; j <= m; j++) row[j] = BIG;
    int centre = (int)((long)i * m / n);
    int lo = centre - band > 1 ? centre - band : 1, hi = centre + band < m ? centre + band : m;
    for (int j = lo; j <= hi; j++) {
      float d = frameDistance(a.f[i - 1], b.f[j - 1]);
      float best = prevRow[j - 1] + 2 * d;
      if (prevRow[j] + d < best) best = prevRow[j] + d;
      if (row[j - 1] + d < best) best = row[j - 1] + d;
      row[j] = best;
    }
    float *t = prevRow; prevRow = row; row = t;
  }
  return prevRow[m] >= BIG / 2 ? NO_MATCH : prevRow[m] / (n + m);
}

float score(const Word &word, const Word *takes, int count) {
  int shortest = MAX_FRAMES, longest = 0;
  for (int i = 0; i < count; i++) {
    if (takes[i].n < shortest) shortest = takes[i].n;
    if (takes[i].n > longest) longest = takes[i].n;
  }
  if (!count || word.n * 4 < shortest * 3 || word.n * 3 > longest * 4) return NO_MATCH;  // under 3/4, over 4/3
  float best = NO_MATCH;
  for (int i = 0; i < count; i++) {
    float d = distance(word, takes[i]);
    if (d < best) best = d;
  }
  return best;
}

float threshold(const Word *takes, int count, int sensitivity, float *spread) {
  float sum = 0;
  int pairs = 0;
  for (int i = 0; i < count; i++)
    for (int j = i + 1; j < count; j++) {
      float d = distance(takes[i], takes[j]);
      if (d < NO_MATCH) {
        sum += d;
        pairs++;
      }
    }
  if (spread) *spread = pairs ? sum / pairs : 0;
  if (!pairs) return 0;  // nothing to match against
  if (sensitivity < 1) sensitivity = 1;
  if (sensitivity > 10) sensitivity = 10;
  // 5: 1.35 x the takes' own spread. Loose on purpose: the owner's genuine words
  // scored up to 1.34 x (said later, from elsewhere in the room), and the AI
  // checks every wake, so a near miss costs only a silent check
  return sum / pairs * (1.0f + 0.07f * sensitivity);
}

}  // namespace wake
