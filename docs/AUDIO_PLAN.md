# Audio plan: reliable, lower-delay, full-duplex two-way voice (and a cleaner mic)

Status: approved 2026-10-18. Phase 0 in progress. Companion notes: [VOICE_STREAMING.md](VOICE_STREAMING.md) (how voice streams today) and [protocol.md](protocol.md).

## Context

Two-way voice works for demos but:
- The speaker mutes its own mic while it plays (half-duplex).
- The mic sends raw, unfiltered audio, and both clients add a fixed +16 dB to the room audio, which also lifts the noise.
- Delay is high (estimated 0.4-0.7 s phone to speaker).
- A dead audio link takes 20-40 s to notice, and once never came back.
- There are no counters for drops or latency.

Findings (code read 2026-10-18; latency numbers are estimates, not measurements):
- The ESP-SR echo canceller (AEC, `AEC_MODE_FD_LOW_COST`, aggressive NLP, 15-30 dB measured) exists in `mic.cpp` but only runs for echo tests. The call gate `(s.live || ...) && !echoFree` (`mic.cpp:400`) mutes the mic even with AEC on.
- No noise suppression, no gain control, no high-pass on the sent audio. Only DC removal and a fixed x1 (`MIC_GAIN_SHIFT 8`).
- Both clients add a fixed +16 dB (`GAIN = 6`, tanh) to room audio: `smart-speaker-web/src/live/listen.ts:9`, `smart-speaker-app/src/lib/roomPlayer.ts:11`.
- The installed ESP-SR (arduino-esp32 3.3.12, esp32s3) provides `esp_ns.h` (`ns_pro_create`, mild/medium/aggressive), `esp_agc.h`, and full-duplex AEC modes with split `aec_linear_process` / `aec_nlp_process`.
- The backend batches talk audio for 125 ms (`AUDIO_BATCH_MS`, `device-hub.service.ts:33`) because of the old EMQX Serverless limit of 10 messages a second. The audio link and self-hosted EMQX don't have that limit.
- Dead-link detection: the server pings every 20 s and drops after a missed pong (`audio-link.service.ts:31,194`). The 64 KB drop threshold is about 21 s of Opus.

Scope decisions:
- Full duplex is for calls (hear the room and talk). AI conversations stay half-duplex. Barge-in stays off.
- No WebRTC now. Keep WebSocket + Opus on the speaker leg and Socket.IO PCM on the phone legs. Opus on the phone legs is last.
- Wake word and the speech/quiet detector keep reading the raw mic signal. New processing only affects the audio that is sent.
- Everything new is switchable: server env flags, per-session fields, or one-line constants. Tested locally first; cloud deploy later, owner runs the restart.

## Phase 0: see it (counters only, no behaviour change)

Firmware (`FW_VERSION` 0.32.0):
- Counters in a shared `audiostats.h`: `talkDrops` (talk buffer full), `talkStarves` (talk playback ran dry), `talkPeak` (most samples waiting), `underruns` (output blocks padded with silence while talk played), `inboxDrops` (main-loop queue full, audio only), `outboxDrops` (mic piece dropped, link outbox full), `linkDowns`, `sendMaxMs` (slowest link send), `rxGaps` (talk frames whose `seq` skipped).
- Counters are totals since boot, reported in the existing `stats` message under `audio`.

Backend:
- `audio-link.service.ts`: per-link counters: mic frames in, mic `seq` gaps, talk messages out, bytes dropped by `MAX_BUFFERED`, peak `bufferedAmount`, Opus decode time. One log line every 60 s while a link is up, and one on close.
- Round-trip probe (owner only, next to mic-test): the server sends a short click burst to the speaker and records with `mic.record`. The estimated capture time of the burst (arrival time of its piece minus the samples after it) minus the send time gives the round trip through the speaker's audio path. Five runs, median and max.

Test batch A (you, once): 5-minute call each way on Wi-Fi at your usual spot; one run with the router far away; stop and restart the backend for 2 minutes (watch link recovery and the serial log); probe five times. Save the numbers next to the budget table in VOICE_STREAMING.md.

## Phase 1: latency and reliability quick wins

1. **No batching on the link.** `sendAudio` (`device-hub.service.ts:284`): when the audio link is up, send at once. Keep the 125 ms batch for the MQTT fallback. `say()` and `speak()` use 40 ms pieces on the link and 125 ms on MQTT.
2. **Faster ack.** `realtime.gateway.ts` `talkStart`: ack right after `hub.send(talk_start)`. `callLog.create` runs fire-and-forget with error logging.
3. **Fast dead-link detection.** `PING_MS` 20 s to 5 s; terminate after 2 missed pongs (10-15 s). `MAX_BUFFERED` 64 KB to about 1 s of audio. Terminate if over the limit for more than 3 s.
4. **Firmware receive watchdog.** In `linkTask`, if nothing arrives for 30 s while up, drop and reconnect. First verify the IDF WebSocket transport answers ping frames; if not, use an app-level ping/pong. Log the failing stage on every reconnect (TLS allocation, login, `netConnected`).
5. **Downlink buffers** (`config.h`, `audio.cpp`): `TALK_PREBUFFER` 3000 to 2000 (125 ms), `PLAY_RING` 2048 to 1024 (64 ms). Do not change the I2S TX DMA (4 x 256): `AEC_DELAY_CAL 1318` was measured with it. Catch-up rule in `talkPush`: when more than 400 ms is waiting while playing, drop the oldest down to 200 ms and count it.
6. **Web talk start** (`smart-speaker-web/src/live/talk.ts`): pre-warm `AudioContext` and the worklet on the device page. On press, run `talk_start` and `getUserMedia` in parallel and keep early chunks (max 8) until the ack, as the app already does. Set `autoGainControl` explicitly. Flush the last partial chunk on release.
7. **Client room playback** (`web listen.ts`, app `roomPlayer.ts` / `roomPlayer.web.ts`): start lookahead 250 to 120 ms. After an underrun add 40 ms, up to 250 ms, and let it decay. If more than 500 ms is scheduled ahead, skip pieces (counted) so delay can't grow forever.

Acceptance: round-trip probe improves by about 150-250 ms against baseline; `underruns` and `talkStarves` do not rise in a 5-minute call; link back within 15 s of a backend restart; three AI conversations still work (they use the same talk path).

## Phase 2: full-duplex calls

Firmware:
1. `MicSession` (`mic.h`) and `firmware.ino:74`: add `fd` (bool), `ns` (0-3), `agc` (bool). Parse `doc["fd"]|false`, `doc["ns"]|0`, `doc["agc"]|false`.
2. `mic.cpp:306`: `aecOn = (s.aec || s.fd || (s.conversation && BARGE_IN)) && snapSteady != INT32_MIN && aecReady()`.
3. `mic.cpp:400` gate becomes `((s.live || s.conversation) && !aecOn) && !echoFree`. With AEC on, call audio keeps flowing while the speaker plays. If AEC is not ready, it falls back to today's half-duplex, and `mic_done` says so.
4. Barge-in logic only for conversations: `strict = aecOn && playing && s.conversation` (`mic.cpp:214`).
5. The wake word stays gated by `echoFree`, so it can't fire on the far end's voice.
6. Tune with the existing echo test (`mic-test` with echo and aec) at volume 40, 70 and 100, with fan and TV on. Options: `AEC_MODE_FD_HIGH_PERF` (13.6 vs 10.1 ms per chunk), `filter_length` 4 to 6, NLP `AGGR` to `MODERATE`, or split linear and NLP. Target: at least 25 dB at volume 70 with the caller still clear.
7. Only if step 6 is not enough: a soft residual guard. While far-end audio plays and the cancelled-to-raw ratio says "mostly echo", attenuate the sent audio by 12 dB with a 100 ms release. Never a full mute.
8. Costs: about 64 ms on the mic path, about 10 ms CPU per 32 ms, about 142 KB in PSRAM, about 2 s at boot (already done at boot).

Backend:
9. `mic.service.ts` `RecordOptions` and the `mic_record` JSON: add `fd`, `ns`, `agc`. `listen.gateway.ts` `start` sends them from env: `CALL_DUPLEX=1` by default; `CALL_DUPLEX=0` returns to today's behaviour without a reflash.

Docs: add the new fields to `protocol.md` under `mic_record`.

Acceptance (batch C): while the room talks, the caller's own voice comes back to the phone at least 25 dB down; the room voice stays understandable while the speaker plays; core 1 under 70% CPU; `dropped` = 0; `heapMin` at least 40 KB. Risk: the round trip is over 1 s, so keep call volume sensible.

## Phase 3: cleaner voice (noise suppression and gain) without losing words

Order: high-pass, then AEC, then NS, then AGC, then send. The wake word and the speech detector keep the raw or band signal.

1. **Spike first.** Create `ns_pro_create(10, mode, 16000)` and `esp_agc_open(...)`, print internal and PSRAM free before and after, and time one 10 ms frame. Keep at least 40 KB internal RAM free. If either doesn't fit, fall back to a software high-pass and noise filter.
2. New `dsp.cpp/h`: 100 Hz high-pass biquad; a FIFO to re-frame (AEC works in 512-sample chunks, NS and AGC in 160-sample frames); created at session start and destroyed at the end. `capture(y, banded, over)` already separates the sent value from the detector value.
3. Defaults to tune: calls `ns=1` (try mild first) and digital AGC with a limiter and a moderate target. AI conversations: NS and AGC off until an A/B test shows a gain.
4. Remove the clients' +16 dB (`GAIN 6` to about 1.5, keep the soft limiter) in the same release that turns AGC on. Never change one without the other.
5. A/B tool: `mic-test` gains `ns`, `agc` and `aec` options and saves `mic/<id>-raw.wav` and `-proc.wav`. The web and app test pages get raw and processed play buttons.
6. Check that no words were lost: about 20 short Bangla phrases read at 1 m, fan on and TV on, recorded raw and processed. Accept if the noise floor drops by at least 6 dB, no word is cut by ear, `clipped` is 0, and the AI transcript (`VoiceLog.dialogue`) is no worse than raw.
7. Hardware checklist, in parallel: mic distance and direction from the amp, short I2S wires away from the amp, a filter cap at the mic, foam against case vibration, USB supply (brownout reports). Record a `floorDb` and `clipped` baseline first.

Acceptance (batch D): on a call the phone hears clearly less hiss and fan with the same clarity; AI conversations are no worse.

## Phase 4: later, only if measurements say so

- Opus on the phone and web legs (WebCodecs on web; a native decoder in the app). Mainly bandwidth.
- Opus tuning on the speaker (complexity, DTX). Use `seq` for packet-loss concealment.
- A WebRTC spike for the phone-to-speaker call, only if TCP stalls still dominate after Phase 1.
- Server-side noise suppression for the AI path. A dedicated FreeRTOS task for `pumpTalk`. Opus in a worker thread if many speakers.

## Test batches

| Step | Effort | Owner tests |
|---|---|---|
| Phase 0 | small | Batch A: baseline numbers |
| Phase 1 | medium | Batch B: latency, link recovery, AI still fine |
| Phase 2 | medium-large | Batch C: full-duplex call |
| Phase 3 | large (spike first) | Batch D: noise A/B |

## Rollback

- Server env: `CALL_DUPLEX=0`, `MIC_NS=0`, `MIC_AGC=0`.
- Single-constant reverts in `config.h`.
- OTA rollback (3-minute probation) if a new firmware doesn't reach the server.

## Not verified yet

- Whether the IDF WebSocket transport answers ping frames (Phase 1, step 4).
- NS and AGC memory placement and CPU cost on this build (Phase 3 spike).
- Real echo reduction at high volume with the final settings (Phase 2, step 6).
- Android has no echo cancellation or noise suppression set for the phone mic in app code (iOS has a voice-chat session). A phone-side check is needed if the phone's own echo shows up in tests.
