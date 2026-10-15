// Wake word: the person's own recordings of it (3 takes, made on this speaker
// through the mic), matched here on the speaker against what the mic hears
// (wakecore). Nothing the mic hears leaves the speaker for this: on a match it
// beeps, reports "wake", and the server starts a voice request (the mic sends
// what was said since the word: see mic.cpp). The takes are kept
// in the built-in storage; on/off and sensitivity come from the server.
#pragma once
#include <stddef.h>
#include <stdint.h>

void wakeSetup();  // after the built-in storage is mounted (cacheSetup), before micSetup
void wakeLoop();   // call every loop(): keeps a new take, reports a match
void wakeConfig(bool enabled, int sensitivity);
void wakeClear();   // forget the takes
void wakeReport();  // wake_status to the server

// From the mic task
bool wakeHear(int16_t sample);  // the room, while the speaker makes no sound; true: the wake word
void wakeInterrupt();           // the speaker is making a sound
// A mic session starts: take 1-3 records that take, test scores against the takes,
// neither ignores what's said (a voice request, a call)
void wakeSessionStart(int take, bool test);
// …and ends: its result as JSON members (",\"wake\":{…}"), or nothing; returns the length
int wakeSessionEnd(char *out, size_t size);
