// Audio: speaker output, volume, ringing a tone, and live talk.
#pragma once
#include <Arduino.h>

void audioSetup();  // I2S amp + the saved volume
void audioLoop();   // call every loop(): feeds the speaker one small chunk

// Volume 0-100, saved in flash so it survives a restart
void setVolume(int v);
void sendVolume();  // report it to the server

// Ringing: a stored copy of the tone, or one of the user's files ("card:/Music/Rain.wav"),
// or streamed from the server. Loops until stopped or RING_MAX_MS. music (played from
// the file manager): the whole file, then again until Stop if repeat is on, else once.
// fadeIn: alarms start quiet and rise.
void toneStart(const String &tone, bool fadeIn = false, bool music = false);

// Music repeat on/off, saved in flash like the volume
void setRepeat(bool on);
void sendRepeat();  // report it to the server
void toneStop();

// Doorbell: a built-in ding-dong, played once (skipped if an alarm or talk is playing)
void chimeStart();

// "I'm listening": two short rising notes, before the mic opens for a voice request.
// falling: the same notes down, "the conversation is over".
void cueStart(bool falling = false);
void callCueStart(bool ending = false);  // a call connected (three rising notes) or ended (falling)

// Live talk from the browser (16 kHz 16-bit mono PCM)
void talkStart();
void talkPush(const uint8_t *data, size_t length);
void talkFinish();  // play out what's buffered, then stop
void talkStop();    // stop now

bool isRinging();
bool isMusic();     // the ringing is music from the file manager, not an alarm
bool isTalking();
bool isChiming();
bool isCueing();
String ringingToneName();
bool audioBusy();   // a tone or talk is playing (don't start slow work like mounting a card)

// What the loudspeaker played, numbered (silence too): for echo cancellation.
// txIndex counts every sample sent to the amp; the reference keeps the last ~2 s.
uint32_t audioTxIndex();
void audioReference(uint32_t from, int16_t *out, size_t n);

// The echo test: a 2 s sweep (150 Hz - 7 kHz); once it has started playing, its
// first sample's tx index
void sweepStart();
bool sweepTxIndex(uint32_t *tx);
