// Microphone: an INMP441 on the second I2S port (pins in config.h). For now a test
// recording: the speaker records a few seconds and streams them to the server
// (ss/dev/<id>/mic: 16 kHz 16-bit mono PCM in 125 ms pieces, QoS 0), reports its
// level (mic_level, every 250 ms), then mic_done. The status light shows it's on.
#pragma once

bool micTest(int seconds);  // false if a recording is already on, or the mic's port didn't start
void micStop();             // end it early (mic_done still follows)
bool micBusy();             // recording now
