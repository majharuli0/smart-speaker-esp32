// The audio link: one WebSocket to the server for live audio, both ways (the
// mic up; talk and the AI's voice down), so audio doesn't go through the MQTT
// broker (its message limit, its queue). Commands and reports stay on MQTT; the
// server tells us where the link is (retained ss/server/audio). Frames:
// [type][seq: 2 bytes][payload]: 0x01 mic Opus, 0x02 talk Opus (each a run of
// 20 ms frames, every one [length: 2 bytes][frame]); the server also accepts
// 0x03 / 0x04, raw PCM. Opus (16 kHz mono speech, ~24 kb/s) needs a tenth of the
// data of raw PCM (256 kb/s). Text frames are JSON: our hello first, then
// talk_start / talk_stop from the server (in order with the audio). While the
// link is down, audio uses MQTT (raw PCM).
#pragma once
#include <Arduino.h>

void audioLinkSetup();                     // after netSetup: runs on its own task
void audioLinkSetUrl(const String &url);  // from the server (ss/server/audio), main loop
bool audioLinkUp();
bool audioLinkSendMic(const uint8_t *pcm, size_t length);  // false if it can't take it now
