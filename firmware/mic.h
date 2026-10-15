// Microphone: an INMP441 on the second I2S port (pins in config.h). Records on
// request (the owner's microphone test, or a voice request) and streams it to the
// server (ss/dev/<id>/mic: 16 kHz 16-bit mono PCM in 125 ms pieces, QoS 0), with
// its level (mic_level, every 250 ms), then mic_done. The status light shows it's on.
#pragma once

// Records up to `seconds`. With endSilenceMs it's a voice request: a short cue
// says it's listening, and it ends by itself once someone has spoken and then been
// quiet that long. false if a recording is already on (a port that won't start is
// reported in mic_done).
bool micRecord(int seconds, int endSilenceMs);
void micStop();  // end it early (mic_done still follows)
bool micBusy();  // recording now
