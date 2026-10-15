// Wake word test on the PC: the speaker's own matching code (firmware/wakecore.cpp)
// run on WAV files (16 kHz 16-bit mono), so it can be tuned on recordings.
//
//   wakelab [--sens N] take1.wav take2.wav take3.wav -- clip.wav ...
//
// Each take gives its longest word; every word found in the clips is scored
// against them. Prints one line per word: clip, where, how long, score, match.
// Build: see README.md in this folder.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>
#include "../../firmware/wakecore.h"

static std::vector<int16_t> readWav(const char *path) {
  std::vector<int16_t> pcm;
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "can't open %s\n", path); exit(1); }
  char id[4];
  uint32_t size;
  fread(id, 1, 4, f); fread(&size, 4, 1, f); fread(id, 1, 4, f);  // RIFF, size, WAVE
  uint16_t format = 0, channels = 0, bits = 0;
  uint32_t rate = 0;
  while (fread(id, 1, 4, f) == 4 && fread(&size, 4, 1, f) == 1) {
    if (!memcmp(id, "fmt ", 4)) {
      std::vector<uint8_t> b(size);
      fread(b.data(), 1, size, f);
      memcpy(&format, &b[0], 2); memcpy(&channels, &b[2], 2);
      memcpy(&rate, &b[4], 4); memcpy(&bits, &b[14], 2);
    } else if (!memcmp(id, "data", 4)) {
      if (format != 1 || channels != 1 || bits != 16 || rate != wake::RATE) {
        fprintf(stderr, "%s: needs 16 kHz 16-bit mono PCM\n", path);
        exit(1);
      }
      pcm.resize(size / 2);
      fread(pcm.data(), 2, pcm.size(), f);
      break;
    } else fseek(f, size + (size & 1), SEEK_CUR);
  }
  fclose(f);
  return pcm;
}

struct Found {
  float at, seconds;
  wake::Word *word;
};

// Every word in a file (a new listener per file, like a quiet room before it)
static std::vector<Found> words(const char *path) {
  std::vector<Found> out;
  wake::Listener *l = new wake::Listener();
  std::vector<int16_t> pcm = readWav(path);
  for (size_t i = 0; i < pcm.size(); i++)
    if (l->push(pcm[i])) {
      wake::Word *w = new wake::Word(l->word());
      out.push_back({(float)i / wake::RATE - (w->n + 30) / 100.0f, w->n / 100.0f, w});
    }
  delete l;
  return out;
}

static const char *name(const char *path) {
  const char *s = strrchr(path, '/');
  const char *b = strrchr(path, '\\');
  if (b > s) s = b;
  return s ? s + 1 : path;
}

int main(int argc, char **argv) {
  int sens = 5, i = 1;
  if (argc > 2 && !strcmp(argv[1], "--sens")) { sens = atoi(argv[2]); i = 3; }
  wake::Word *takes = new wake::Word[wake::TAKES];
  int count = 0;
  for (; i < argc && strcmp(argv[i], "--"); i++) {
    std::vector<Found> w = words(argv[i]);
    if (w.empty()) { printf("take %s: no word found\n", name(argv[i])); continue; }
    Found *longest = &w[0];
    for (auto &x : w) if (x.seconds > longest->seconds) longest = &x;
    if (count < wake::TAKES) takes[count++] = *longest->word;
    printf("take %s: %.2f s (of %d words)\n", name(argv[i]), longest->seconds, (int)w.size());
  }
  float spread;
  float limit = wake::threshold(takes, count, sens, &spread);
  printf("takes %d, spread %.2f, threshold %.2f (sensitivity %d)\n", count, spread, limit, sens);
  for (i++; i < argc; i++)
    for (auto &x : words(argv[i])) {
      float best = wake::score(*x.word, takes, count);
      printf("%s\t%.2f\t%.2f\t%.2f\t%s\n", name(argv[i]), x.at, x.seconds, best,
             count >= 2 && best <= limit ? "MATCH" : "-");
    }
  return 0;
}
