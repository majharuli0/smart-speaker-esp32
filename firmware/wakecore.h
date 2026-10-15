// Wake word matching, in plain C++ (no Arduino), so the PC test program in
// voice-lab/wake builds exactly the code the speaker runs.
//   Words: a stretch of speech 0.25-2.5 s long, with quiet before and after it
//     (judged against the room's background, as the microphone does).
//   Features: every 10 ms, 12 MFCCs of a 25 ms window; the average over the word
//     is taken off, so the mic and the room matter less. (Tested on Bangla clips
//     in voice-lab/wake: deltas and liftering made impostors score closer.)
//   Matching: DTW between a word and each of the person's recorded takes; the
//     closest take counts. A word much shorter or longer than the takes doesn't
//     match. The takes' distance from each other sets the threshold.
#pragma once
#include <stdint.h>

namespace wake {

const int RATE = 16000;
const int HOP = 160;          // a feature frame every 10 ms
const int WINDOW = 400;       // of the last 25 ms
const int CEPS = 12;          // MFCCs 1-12 (0, the loudness, isn't used)
const int MIN_FRAMES = 25;    // a word: 0.25 s ...
const int MAX_FRAMES = 250;   // ... to 2.5 s
const int TAKES = 3;          // recordings of the wake word
const float NO_MATCH = 1e9f;  // distance when two words can't be compared
const int NFFT = 512;

struct Word {
  int n = 0;  // frames
  float f[MAX_FRAMES][CEPS];
};

// Samples in; a Word out each time a stretch of speech ends
class Listener {
 public:
  Listener();
  bool push(int16_t sample);  // true: a word just ended, and word() holds it
  const Word &word() const { return out; }
  void interrupt();           // the speaker made a sound: forget the stretch under way
  float floorDb() const;      // the room's background

 private:
  void frame(float *mfcc);
  void ended(long first, long last);

  float ring[WINDOW];    // the last 25 ms of samples (pre-emphasised)
  int pos = 0, sinceFrame = 0, filled = 0;
  float re[NFFT], im[NFFT];  // FFT work area
  float prev = 0;        // for pre-emphasis
  float hopPower = 0;    // this 10 ms
  float recent[5] = {};  // the last 5 x 10 ms, for a 50 ms level
  int recentAt = 0;
  float floor = -1;      // the quietest recent 50 ms
  // Each frame's MFCCs, kept for a word's length and a little more
  static const int KEEP = MAX_FRAMES + 40;
  float history[KEEP][CEPS];
  float levels[KEEP];    // and their 50 ms level
  long frames = 0;       // frames made so far
  long start = -1;       // the stretch of speech under way: its first frame
  long lastHeard = -1;   // its last frame above quiet
  int loudRun = 0, quietRun = 0;
  bool tooLong = false;
  Word out;
};

// How far apart two words are: 0 = the same. NO_MATCH if one is over twice as long.
float distance(const Word &a, const Word &b);

// How far a word is from the closest take; NO_MATCH if its length is far from theirs
float score(const Word &word, const Word *takes, int count);

// The match threshold for these takes (count >= 2): their average distance from
// each other, scaled by sensitivity 1-10 (5 the default; higher wakes more easily).
// spread: that average.
float threshold(const Word *takes, int count, int sensitivity, float *spread);

}  // namespace wake
