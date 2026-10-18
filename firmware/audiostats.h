// Counters for the audio path, reported in the stats message (audio: {...}). Totals
// since boot. Written from several tasks without a lock: a rare lost increment is
// fine for tuning, and a lock here would cost more than the count is worth.
#pragma once
#include <Arduino.h>

struct AudioCounters {
  uint32_t talkDrops;     // talk audio dropped: the 1 s buffer was full
  uint32_t talkStarves;   // talk playback ran dry (silence, then it buffers up again)
  uint32_t talkPeak;      // most samples ever waiting in the talk buffer (delay growing)
  uint32_t talkSkips;     // times talk skipped ahead after a burst (TALK_CATCHUP_AT)
  uint32_t underruns;     // output blocks padded with silence while talk played
  uint32_t inboxDrops;    // audio messages dropped: the main loop's queue was full
  uint32_t outboxDrops;   // mic pieces dropped: the audio link's outbox was full
  uint32_t linkDowns;     // the audio link was up and then lost
  uint32_t sendMaxMs;     // the slowest single send on the audio link
  uint32_t rxGaps;        // talk frames the server numbered out of order (missing ones)
};

extern AudioCounters audioStats;
