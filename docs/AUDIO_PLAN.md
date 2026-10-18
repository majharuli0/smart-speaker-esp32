# Audio plan: reliable, lower-delay, full-duplex two-way voice (and a cleaner mic)

Status (2026-10-18, evening): Phases 0 and 1 done, tested, merged to main and deployed to the cloud, plus a round of Momo fixes (see "Momo fixes" below). Next: Phase 2 (full-duplex calls). Work happens on branch `feat/audio-duplex` in all four repos; each tested step is fast-forwarded into main and pushed.

Deployed on the cloud: backend `8101c34`, web `5856910`, speaker firmware 0.34.0 (flashed over USB with the cloud `secrets.h`). App `5b95393` (built by the owner on the Mac).

**How we work on this (keep to it):**
- One phase or fix at a time; the owner tests each locally or on the cloud before the next. Don't start the next phase without them.
- Branch `feat/audio-duplex` in all four repos; after the owner's OK: commit, push, `git merge --ff-only` into `main`, push `main`.
- Run the backend e2e suite before every deploy: `npx jest --config test/jest-e2e.json --runInBand` (needs `docker compose up -d` in `smart-speaker-backend`).
- Local testing: backend `npm run start:dev`, web `npm run dev`, EMQX and Postgres in Docker; `firmware/secrets.h` `MQTT_URI` set to the PC's LAN address (check `ipconfig`, it changes); flash with `./fw.sh upload COM11` (BOOT + RESET if it won't connect). The owner runs the app on a Mac and sets its `.env` himself. API docs only load at `http://localhost:4000/api/docs` (a LAN address is upgraded to https).
- Cloud deploy: back up first (database dump and the `speaker_storage` volume), `git archive <commit> | ssh … 'tar -x -C ~/speaker'`, `docker compose -f docker-compose.prod.yml up -d --build backend`; web: `npm run build`, then replace `~/speaker/web`. Switch `secrets.h` back to the cloud line before a cloud firmware build. Check the speaker's `audio link up` in `docker logs speaker-backend-1`.
- Reading results: Momo's timings are in the backend log ("AI ready", "answer started … after their turn", "<tool> took … on the speaker: …"); conversations in the `VoiceLog` table; the speaker's audio counters in `GET /api/v1/devices/:id/audio-stats`.
- No barge-in (the owner rejected interrupting Momo by voice). Full duplex is for calls only. See also [protocol.md](protocol.md) (its audio sections predate the audio link).

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

Test batch A (you, once): 5-minute call each way on Wi-Fi at your usual spot; one run with the router far away; stop and restart the backend for 2 minutes (watch link recovery and the serial log); probe five times.

### Phase 0 results (2026-10-18, done)

Built and tested on the local network: backend, web and EMQX on the PC (192.168.0.104), speaker on firmware 0.32.0, app on the Mac. Not yet measured on the cloud.

| Measure | Result |
|---|---|
| Round trip through the speaker (latency probe, 5 runs) | **255 ms** median (241-259) |
| Server to speaker audio | 0 missing, 0 dropped, 0 frame gaps; Opus decode 0.2 ms avg |
| Talk playback ran dry (`talkStarves`) | 3 |
| Most talk audio waiting (`talkPeak`) | 5,120 samples (320 ms) |
| Mic pieces dropped on the speaker (`outboxDrops`) | **48** (about 6 s). Cause not confirmed: two backend restarts during the test, or the wake word's pre-roll burst |
| Speaker back after a backend restart | about 1 s after the server is reachable (clean restart only; a silent network loss is not tested) |
| App, first "Hear the room" after opening | 1.5-2 s delay that stays for the call; gone on the second try (phone-side backlog: Phase 1 step 7) |

Notes:
- Noise and buffering sounded better than on the cloud. Phase 0 changed no audio behaviour, so this is the local network, not a fix.
- The `underruns` counter counted idle silence (`talkPlaying` stays true after a talk ends). Fixed in the code (it now also needs `talking`), but the speaker runs the old build until the next flash. Ignore `underruns` from 0.32.0.
- The round trip covers the speaker only: not the phone legs, not the server's 125 ms talk batch, not the cloud.
- Local testing: `firmware/secrets.h` points MQTT at the PC; switch it back to the cloud line before a cloud build. The API docs page only loads at `http://localhost:4000/api/docs` (a LAN IP is upgraded to https by the security headers). The speaker may need BOOT + RESET to accept a USB upload.

## Phase 1: latency and reliability quick wins

1. **No batching on the link.** `sendAudio` (`device-hub.service.ts:284`): when the audio link is up, send at once. Keep the 125 ms batch for the MQTT fallback. `say()` and `speak()` use 40 ms pieces on the link and 125 ms on MQTT.
2. **Faster ack.** `realtime.gateway.ts` `talkStart`: ack right after `hub.send(talk_start)`. `callLog.create` runs fire-and-forget with error logging.
3. **Fast dead-link detection.** `PING_MS` 20 s to 5 s; terminate after 2 missed pongs (10-15 s). `MAX_BUFFERED` 64 KB to about 1 s of audio. Terminate if over the limit for more than 3 s.
4. **Firmware receive watchdog.** In `linkTask`, if nothing arrives for 30 s while up, drop and reconnect. First verify the IDF WebSocket transport answers ping frames; if not, use an app-level ping/pong. Log the failing stage on every reconnect (TLS allocation, login, `netConnected`).
5. **Downlink buffers** (`config.h`, `audio.cpp`): `TALK_PREBUFFER` 3000 to 2000 (125 ms), `PLAY_RING` 2048 to 1024 (64 ms). Do not change the I2S TX DMA (4 x 256): `AEC_DELAY_CAL 1318` was measured with it. Catch-up rule in `talkPush`: when more than 500 ms is waiting while playing (`TALK_CATCHUP_AT`), drop the oldest down to 250 ms (`TALK_CATCHUP_TO`) and count it (`talkSkips`). 500 ms, not 400: over MQTT the server keeps up to 375 ms of the AI's voice ahead on purpose.
6. **Web talk start** (`smart-speaker-web/src/live/talk.ts`): pre-warm `AudioContext` and the worklet on the device page. On press, run `talk_start` and `getUserMedia` in parallel and keep early chunks (max 8) until the ack, as the app already does. Set `autoGainControl` explicitly. Flush the last partial chunk on release.
7. **Client room playback** (`web listen.ts`, app `roomPlayer.ts` / `roomPlayer.web.ts`): start lookahead 250 to 120 ms. After an underrun add 40 ms, up to 250 ms, and let it decay. If more than 500 ms is scheduled ahead, skip pieces (counted) so delay can't grow forever.

Acceptance: round-trip probe improves by about 150-250 ms against baseline; `underruns` and `talkStarves` do not rise in a 5-minute call; link back within 15 s of a backend restart; three AI conversations still work (they use the same talk path).

### Phase 1 results (2026-10-18, done)

Built as planned, with these details:
- Backend: on the audio link talk goes on as it comes; the AI's voice in 40 ms pieces, 240 ms ahead (`LINK_PACE`; MQTT keeps 125 ms pieces, 375 ms ahead). Pings every 5 s, closed after 2 missed; about 1 s of audio may wait (`MAX_BUFFERED` 4 KB Opus, 32 KB PCM); a link backed up for 3 s is closed. The server also sends a text keep-alive `{"type":"ka"}` every 5 s.
- Firmware 0.33.0: the speaker's WebSocket library answers pings itself and never shows them, so it watches for `ka` instead and reconnects after 30 s of silence (only once a `ka` has come, so an older server doesn't make it drop).
- Web: talk audio context and worklet load with the device page; mic and server ok start together; the last part chunk is sent on release. Web and app: room audio starts 120 ms behind, adds 40 ms after each dry spell (up to 250 ms), and skips pieces when more than 500 ms behind (`playClock.ts`, the same file in web and app).
- Owner's test, local then cloud: "looks faster"; the 1.5-2 s first-listen delay in the app is gone. The probe was not re-run after Phase 1 (baseline 255 ms).

## Momo fixes (2026-10-18, done, deployed)

Found in the cloud conversation log (`VoiceLog`) and the timing lines added to the backend log:

| Problem | Cause | Fix |
|---|---|---|
| Momo repeated an answer | each tool result went back to Gemini on its own, and it speaks after each | results of calls made together go back in one reply (`gemini.ts`, `Batch`) |
| Momo stopped mid-sentence ~10 s, then went on | a noise "turn" while it was preparing an answer cut that answer | a turn is held on the server until it ends; only a real one cuts the old answer; 300 ms hush after it (`HUSH_MS`) |
| Momo answered noise ("জি, আমি শুনছি") | the speaker's detector is loudness only | turns with under 200 ms of sound never reach Gemini (`MIN_SPEECH_MS`; firmware 0.34.0 reports `speech` ms in `turn done`, and `turn_again` sends it back to listening without a beep); `ignore_noise` tool; 3 in a row end the conversation quietly (`NOISE_END`) |
| Bulk requests: "can't set many at once", 6.8 s for 5 alarms | no way to do several; one sync and speaker save per alarm | calls made together run one after another and send the speaker one list (`AlarmsService.hold/release`); `set_reminder` takes `count` + `every_minutes` (times worked out on the server); `delete_alarms` with `all: true` or `ns`; new alarms answer their number `n` |
| "Is it really done?" | database writes were awaited, the speaker was not | alarm tools answer `onSpeaker` after waiting up to 3 s for `alarms_ack`; the prompt forbids claiming success without `ok` |
| More than 20 alarms would never ring | the speaker keeps 20 (`MAX_ALARMS` in firmware) | `set_reminder` refuses past 20 and says how many fit |

Timings (cloud, owner's test): Gemini ready 0.8-0.9 s after the wake; a simple answer starts 1.0-2.0 s after the turn (plus the 0.7 s end-of-speech wait); "5 alarms every 30 min" 4.1 s (was 6.8 s; the tool itself 1.1 s), "10 more every 45 min" 4.0 s (tool 0.26 s); deleting 5 chosen alarms 4.2 s (needs list_alarms first). The rest is Gemini thinking (~2 s) and starting to speak (~1 s), plus the speaker confirmation (0.3-1 s; could be made non-blocking if speed matters more than the confirmation).

Tests: the e2e suite had been failing since the held-turn change and was not run before those deploys. It now reflects the new behaviour and covers batches, series, the 20-alarm limit, delete all, noise and short turns: 14 suites, 111 tests pass (`npx jest --config test/jest-e2e.json --runInBand`, needs `docker compose up -d`). Run it before every deploy.

### Open after the Momo fixes

1. A false wake at 16:33 UTC was answered in Spanish ("Hola, estoy muy bien…") instead of `not_for_me`. Tighten Momo's prompt: Bangla only, and silence for anything after a wake that isn't clearly addressed to it.
2. Stray single words ("A") still sometimes get "জি, কিছু বলবেন?". Left for the local model the ML engineer is training.
3. One conversation ended `unavailable` at 16:07 UTC (Gemini unreachable); its logs were lost with the container. Watch for it.
4. The speaker's `underruns` counter fix (count only while talk is on) is in firmware 0.33.0+; the latency probe should be re-run to measure Phase 1 (`POST /api/v1/devices/:id/latency-probe`).

## Phase 2: full-duplex calls

Firmware:
1. `MicSession` (`mic.h`) and `firmware.ino:74`: add `fd` (bool), `ns` (0-3), `agc` (bool). Parse `doc["fd"]|false`, `doc["ns"]|0`, `doc["agc"]|false`.
2. `mic.cpp`, where a new session starts (the line `aecOn = (s.aec || (s.conversation && BARGE_IN)) && …`): add `s.fd`, i.e. `aecOn = (s.aec || s.fd || (s.conversation && BARGE_IN)) && snapSteady != INT32_MIN && aecReady()`.
3. `mic.cpp`, the per-sample gate `if ((s.live || (s.conversation && !aecOn)) && !echoFree)`: becomes `if ((s.live || s.conversation) && !aecOn && !echoFree)`. With AEC on, call audio keeps flowing while the speaker plays. If AEC is not ready, it falls back to today's half-duplex, and `mic_done` says so.
4. Barge-in logic only for conversations: in `capture()`, `bool strict = aecOn && playing;` becomes `aecOn && playing && s.conversation`.
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
