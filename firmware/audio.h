// Audio: speaker output, volume, ringing a tone, and live talk.
#pragma once
#include <Arduino.h>

void audioSetup();  // I2S amp + the saved volume
void audioLoop();   // call every loop(): feeds the speaker one small chunk

// Volume 0-100, saved in flash so it survives a restart
void setVolume(int v);
void sendVolume();  // report it to the server

// Ringing: streams tone from the server, loops it until stopped or RING_MAX_MS
void toneStart(const String &tone);
void toneStop();

// Live talk from the browser (16 kHz 16-bit mono PCM)
void talkStart();
void talkPush(const uint8_t *data, size_t length);
void talkFinish();  // play out what's buffered, then stop
void talkStop();    // stop now

bool audioBusy();   // a tone or talk is playing (don't start slow work like mounting a card)
