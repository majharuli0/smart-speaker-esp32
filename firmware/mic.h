// Microphone: an INMP441 on the second I2S port (pins in config.h), always on.
// Its own task reads it and hands the sound to:
//   - the wake word listener (wake.h), which keeps it on the speaker, and
//   - a session, when the server asks for one: a recording (the microphone test,
//     a voice request, a wake word take or test) or the room for a call (live).
// A session streams to the server (ss/dev/<id>/mic: 16 kHz 16-bit mono PCM in
// 125 ms pieces, QoS 0), then reports mic_done. The status light shows it's on.
// A conversation also keeps the turns: it tells the server the moment the person
// starts and stops speaking, beeps when it's their turn again, and ends itself
// (falling tone) when they say nothing for a while.
#pragma once

struct MicSession {
  int seconds;       // at most this long
  int endSilenceMs;  // a voice request: ends once someone has spoken and then been quiet this long
  bool live;         // the room for a call: the speaker keeps talking; nothing is sent while it does
  int wakeTake;      // 1-3: this recording is that take of the wake word
  bool wakeTest;     // this recording is scored against the takes
  bool afterWake;    // after the wake word: from where the word started
  bool conversation; // a conversation: turns, with the AI's voice in between (endSilenceMs ends a turn)
  int idleMs;        // a conversation ends when the person says nothing this long on their turn
  bool echoTest;     // plays a sweep while recording; mic_done tells where both started (rx/tx sample numbers)
  bool aec;          // the recording goes through echo cancellation (ESP-SR) first (being measured)
};

enum MicState { MIC_OFF, MIC_LISTENING, MIC_THINKING };

void micSetup();                      // after audioSetup and wakeSetup
bool micRecord(const MicSession &s);  // false if a session is already on
void micStop(bool quiet = false);     // end it early (mic_done still follows); quiet: no end tone
bool micBusy();                       // a session is on
void micLoop();                       // main loop: cuts the AI's voice when the person speaks over it
MicState micState();                  // and what it's doing (for the light)
