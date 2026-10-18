// Cleaner call audio (audio plan, phase 3): a 180 Hz high-pass (fan rumble, hum, ticks),
// then ESP-SR noise suppression (steady noise: a fan, hiss). The suppressor costs
// 17% of a core, and core 1 already runs the echo canceller in a call, so it runs
// in its own task on core 0. Only the audio that is sent goes through it: the
// wake word and the speech detector keep the raw signal.
// The mic task pushes samples in and pulls the cleaned ones out (~20 ms later).
#pragma once
#include <Arduino.h>

bool dspStart(int level);                   // 1 mild, 2 medium, 3 strong; false: couldn't (send raw)
void dspPush(const int16_t *x, size_t n);   // samples in (high-passed here)
size_t dspPull(int16_t *y, size_t max);     // cleaned samples out, as many as are ready
void dspStop();                             // the end of the session: waits for the last of it
uint32_t dspDrops();                        // 10 ms frames lost in this session (a buffer was full)
