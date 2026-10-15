# Voice plan: wake word, reminders and calling family, in Bangla

## Goal
Someone near the speaker says their **wake word**, then speaks **Bangla** naturally:
- **"কাল সকাল ৮টায় প্রেসারের ওষুধের কথা মনে করিয়ে দিও"** (remind me about my blood pressure medicine tomorrow at 8 am) → a reminder is set, after the speaker reads it back and the person says yes.
- **"রাকিবকে একটু ফোন দাও"** (please call Rakib) → Rakib's phone gets a call notification, and he talks through the speaker.

Family members can also **record a reminder in their own voice** from the app.

**Rules for this plan:**
- **No cost:** AI services only on free tiers. The feature is used a few times a day, so the free limits are enough for the prototype and a pilot.
- **Reuse what exists:** alarms, file uploads, the talk path, MQTT request/reply, Socket.IO, `toDeviceWav()`.
- **Every component has one job** (next section).
- **Test before the mic:** everything except the last step can be built and tested with the **phone's mic**. The INMP441 only replaces where the audio comes from.

## Who does what
| Component | Its job in voice | Never does |
|---|---|---|
| **Speaker firmware** | Hears its wake word (on the speaker, offline). Records the question and sends it. Plays the answer and the reminders. Shows the state on the status light. | Never talks to an AI service; doesn't know which one is used |
| **EMQX broker** | Carries one new topic: the speaker's mic audio | — |
| **Backend** | Turns speech into an action: sends audio to the AI → gets a structured result → acts through **existing** services (alarms, notifications) → makes the spoken reply → sends it to the speaker. Keeps API keys, daily quota and a log. | Never detects the wake word |
| **AI services** (external, free tiers) | **Understand:** Bangla audio → structured result (Gemini). **Speak:** text → Bangla speech (Azure `bn-BD` voice). Each sits behind a small adapter, so it can be replaced. | — |
| **Phone app** | Setup: record the wake word, family members, record reminder messages. Receives calls. **"Test with phone mic"**, which goes through the same backend path as the speaker's mic. | Never calls an AI service directly |
| **Web app** | The same settings pages and the voice log (recording is optional) | — |

```
                    ┌──────────────────────── Backend ────────────────────────┐
 Phone app ─────────►  Socket.IO voice_*  ─┐                                  │
 (test mic, setup)  │                      ├─► VoiceModule ─► Understand (Gemini)    ──► AI
 Speaker ── wake ───►  MQTT …/mic  ────────┘      │          Speak (Azure bn-BD)     ──► AI
 (offline detect)   │                             ├─► AlarmsService   (existing: reminders)
                    │                             ├─► FamilyService   (new: members, call → push)
 Speaker ◄──────────┤  MQTT …/audio  ◄────────────┘   reply via the talk path (existing)
 Speaker ◄──────────┤  MQTT …/cmd  voice_sync, listen, wake_test                        │
 Speaker ◄──────────┤  HTTP  wake recordings, reminder sounds (existing download path)  │
                    └─────────────────────────────────────────────────────────┘
```

## How one request flows
1. **Wake word.** The speaker matches its wake word on the device. A short beep plays, and the light shows **listening**.
2. **Record.** The speaker sends the mic audio on `ss/dev/<id>/mic` until 1 s of silence (8 s at most). The light shows **thinking**. *(Before the mic exists: the phone app sends the same audio over Socket.IO.)*
3. **Understand.** The backend sends the audio, the **family members' names**, the **current Dhaka time** and the list of allowed actions to Gemini, and gets back for example:
   ```json
   {"intent":"set_reminder","time":"08:00","date":"2026-10-16","days":[],"label":"প্রেসারের ওষুধ",
    "reply":"কাল সকাল ৮টায় প্রেসারের ওষুধের কথা মনে করাব। ঠিক আছে?"}
   ```
   Allowed intents for now: `set_reminder`, `call_family`, `confirm` (yes / no), `unknown`.
4. **Reply.** The backend turns `reply` into speech (Azure `bn-BD`) and plays it through the **talk path**. Then it sends `listen` so the speaker records the answer **without** the wake word again.
5. **Confirm, then act.** "হ্যাঁ" (yes) → the backend uses `AlarmsService`, exactly as the app does today. A reminder **always** needs a yes; a call doesn't.
6. **When it fails** (over quota, AI down, didn't understand): the speaker says one recorded sentence, "এখন বুঝতে পারছি না, পরে আবার বলুন" (I can't understand right now, please try again later). Reminders that are already set are unaffected; they ring on the speaker as they do today.

## What we reuse vs. what's new
| Need | Reuse | New |
|---|---|---|
| Reminder rings on time, even offline | Alarms + `alarms_sync` + stored sounds | `fade:false` for voice reminders (the 30 s fade-in would make the first words too quiet) |
| Reminder in a family member's voice | Files upload (ffmpeg → WAV → the speaker downloads it), an alarm with `sound: "card:/Reminders/…"` | A **Record** button in the alarm form |
| Reminder spoken from AI text | The same file path: the backend "uploads" the generated speech through `FilesService` | — |
| Spoken reply | Talk path (`ss/dev/<id>/audio`, the speaker's 1 s buffer) | `DeviceHub.say(deviceId, pcm)`: sends the speech **at real-time speed**, between `talk_start` and `talk_stop` |
| Delivering wake word recordings | The `alarms_sync` pattern + the tone downloader (HTTP + SHA-256) | `voice_sync` / `voice_ack` |
| Testing the speaker without a mic | `hub.request()` (request/reply with `req`) | `wake_test` → `wake_result` |
| Mic audio up | The talk audio batching, reversed | Topic `ss/dev/<id>/mic` + permission for it in the broker topic check |
| Live updates in the app | Socket.IO user rooms | `voice_result` (what was heard, the intent, the reply), `call` |
| Family | — | Members per speaker + push notifications (= APP_PLAN **M8.3** and **M6**, minimal versions) |

## Interfaces (all new messages)
**MQTT, server → speaker (`cmd`):**
- `voice_sync {version, enabled, sensitivity, wake:[{name, size, sha256}]}`: the wake word recordings to download and use
- `listen {ms}`: record now without the wake word, for the yes / no answer
- `wake_test {name}`: run the detector on this downloaded clip
- `say_fail`: play the built-in "can't understand right now" sentence

**MQTT, speaker → server:**
- `evt`:
  - `voice_ack {version}`
  - `wake_result {req, matched, score}`
  - `wake {score}`: logged, so false wakes can be counted
  - `listening` / `mic_end`
- `ss/dev/<id>/mic`: binary, 16 kHz 16-bit mono, 125 ms pieces, QoS 0

**Socket.IO:**
- app → backend: `voice_start {deviceId}`, `voice_audio` (binary), `voice_end`
- backend → app: `voice_result {heard, intent, reply}`, `call {deviceId, from}`

**REST:**
- `GET / PATCH /devices/:uid/voice`: on/off, sensitivity, status
- `POST /devices/:uid/voice/wake`: 3 recordings
- `POST /devices/:uid/voice/wake/test`: 1 recording → the speaker's result
- `GET / POST / DELETE /devices/:uid/family`: members, each with a **name used for calling** ("রাকিব", "বড় ছেলে" = eldest son)
- `GET /devices/:uid/voice/log`: recent requests with result and time taken

**Database:**
- `DeviceMember {deviceId, userId, role: owner | member, callName}`
- `VoiceSettings {deviceId, enabled, sensitivity, wakeFiles}`
- `VoiceLog {deviceId, at, heard, intent, ok, ms, provider}`
- `Session.pushToken` (the column already exists)

## Milestones
Each one ends working, tested and committed. **Only V5 needs the mic.**

### V1 Spoken reminders (quick win, no AI)  ✅ *done 2026-10-15: firmware 0.18.0, backend (alarm `fade`, `voice` uploads, 34 e2e tests), app and web; checked on the real speaker (local setup)*
- **Lesson (sound):** recorded voices are uploaded with `voice: true` and kept natural: only the rumble below 100 Hz is cut, then the loudest point is raised to −1 dB. The music filter's compression lifted the phone's room noise between words by about 11 dB (a "wind" sound), and noise removal sounded worse than the original in listening tests. Set `AUDIO_DEBUG_DIR` in a local `.env` to keep each upload as recorded and as converted, so the two can be compared.
- **App and web:** in the alarm form, **Record message** (phone mic, `@siteed/audio-studio`, already in the app) → existing files upload into `/Reminders` → the alarm's `sound` points to it. Preview and re-record.
- **Backend:** set `fade:false` in `alarms_sync` when the sound is a reminder message.
- **Firmware (0.18.0):** obey `fade:false`.
- **Done when:** a daughter records "আম্মু, এখন ওষুধ খাও" (Mum, take your medicine now) in the app, and it plays at the alarm time on the speaker, including with the internet unplugged.

### V2 Family and calling, without voice  ✅ *done 2026-10-15: backend (`DeviceMember`, family API, push through Firebase, 40 e2e tests), Family screens on web and app, call screen and toast; deployed to the cloud; web push checked (browsers, and iPhone from the Home Screen); push in the phone app waits for its first build with Firebase*
- **Backend `FamilyModule`:** invite a member by email; `callName` per member; ownership checks extended so members can control the speaker (only the owner can remove it or manage members). **Push notifications** with Firebase Cloud Messaging (free) to every member's phone.
- **App:** a family screen; an incoming **call** notification opens the speaker page with hold-to-talk ready.
- **Firmware:** none.
- **Done when:** two accounts share one speaker, and `POST /devices/:uid/family/:memberId/call` (used later by voice) shows a push on that member's phone, even with the app closed.
- **Web push (added):** the web app registers a service worker with Firebase (`public/firebase-messaging-sw.js`) and a **Turn on notifications** button; calls and visitor rings reach a closed page, and tapping one opens the speaker. An iPhone needs the site on the Home Screen (iOS 16.4+), so the app has a manifest and icons. While the page is open a toast with **Answer** shows instead. Notifications expire after 5 minutes.

### V3 The voice brain, tested with the phone mic
- **Backend `VoiceModule`:**
  - `Understand` adapter → **Gemini Flash-Lite** (free tier; the key is in `.env`). The prompt has the allowed intents, the family `callName`s, the current time in the speaker's time zone, and "answer in short Bangla, never give medical advice".
  - `Speak` adapter → **Azure `bn-BD-NabanitaNeural`** (free tier). Short replies are cached by their text.
  - Confirm step: a pending action is kept per speaker for 30 s.
  - Daily **quota counter**: when it's reached, the "can't understand" answer is played.
  - `DeviceHub.say()` for real-time playback; `VoiceLog`.
- **App:** **Test with phone mic** under the speaker's settings (hold to speak; shows what was heard, the intent and the reply; the speaker plays the reply).
- **Bangla test set** (`voice-lab/commands/`): about 100 recorded commands from 5–10 people, including elderly people, a few dialects and mixed Bangla-English ("রিমাইন্ডার সেট করো", "মেডিসিন", "কল দাও" = set a reminder, medicine, call). Times like সাড়ে আটটা / পৌনে নয়টা / সকাল / রাত / কাল / পরশু (half past 8 / quarter to 9 / morning / night / tomorrow / day after). A script plays them through the backend and reports **% of correct actions**. (Not word accuracy: what matters is whether the right thing happened.)
- **Done when:** ≥ 85% of the test commands give the right action; the reply starts within 3 s; the phone-mic test sets a real reminder that rings, and a call reaches the family member.

### V4 Wake word on the speaker, with recordings instead of a mic
- **Detector**, in plain C++ with no Arduino calls (`firmware/wake/`), so the **same code** builds for the speaker and for a PC test program (`voice-lab/wake/`):
  - 13 MFCCs + deltas (10 ms steps), mean-normalised;
  - DTW against the user's 3 recordings, run only when there's voice;
  - the threshold comes from how far apart the 3 recordings are, adjusted by `sensitivity`.
- **PC first:** test on recordings (≥ 3 people × 20 takes; 1 h of TV and conversation as negatives). **Targets:** ≥ 90% detection at 1 m; ≤ 1 false wake per hour.
- **Firmware:** `wake.cpp` runs the detector in its own task (buffers in PSRAM). `voice_sync` downloads the recordings with the existing tone downloader. `wake_test` runs a downloaded clip and replies with the score.
- **Backend:** `POST …/voice/wake` (convert with `toDeviceWav()`, store, `voice_sync`); `…/wake/test` with `hub.request()`.
- **App:** **Wake word** screen: record 3 takes (level meter, re-record), sensitivity, **Test** (the score is computed **on the speaker**).
- **Done when:** the PC targets are met, and Test in the app gives the same scores as the PC program for the same clips.

### V5 Microphone: the whole loop on the speaker
- **Wiring:** INMP441 on `I2S_NUM_1` (receive), suggested GPIO 7 (SCK), 8 (WS), 9 (SD); check them against the header. L/R to GND, 3V3. Use the same (legacy) I2S driver as `audio.cpp`.
- **Firmware:**
  - the mic feeds `wake.cpp` (the input switches from test clips to the mic);
  - after a wake: beep → light **listening** → send on `…/mic` → light **thinking**;
  - `listen` reopens the mic for the yes / no;
  - **no listening while sound is playing** (simple echo handling).
- **Wake word recorded on the speaker itself:** the app says "say it 3 times now" and the speaker records it. Phone recordings sound different from the INMP441, so this makes matching better.
- **Backend:** `DeviceHub` subscribes to `ss/dev/+/mic`; the broker check allows a speaker to publish its own `mic`; `VoiceModule` treats it like the phone audio. Nothing else changes.
- **Done when:** from 1 m and 3 m, with a TV on, "wake word → কাল সকাল ৮টায় ওষুধ (medicine tomorrow at 8 am) → হ্যাঁ (yes)" sets the reminder; "রাকিবকে ফোন দাও" (call Rakib) reaches Rakib; the wake-word targets hold.

### V6 Pilot
- **Privacy:**
  - a **voice on/off** switch, with a consent text in the app (Gemini's free tier may use the audio to improve Google's products);
  - the light always shows when the mic is sending;
  - no audio is stored by default.
- **One week with a family.** Measure false wakes per day, % of correct actions, and % of reminders confirmed.
- **Done when:** the pilot results are written up.

## Cost (all free)
| Part | Service | Cost |
|---|---|---|
| Wake word, reminders ringing, calling over the existing path | Ours (speaker + backend) | 0 |
| Understanding Bangla | Gemini Flash-Lite free tier: a few hundred to 1,500 requests a day (check the live number in AI Studio) ≈ 50–100 speakers at 5–10 requests a day | 0 |
| Spoken replies | Azure Speech free tier (`bn-BD` voices); replies cached | 0 |
| Push notifications | Firebase Cloud Messaging | 0 |
| Server | Existing cloud server | unchanged |

Free limits can change. The adapters keep a switch to another provider (Groq Whisper + a free AI model, or a self-hosted Bangla speech model) to a few files in the backend, with **no change** to the firmware or the apps.

## Later (not in the first goal)
- A **typed** wake word (microWakeWord, trained from text-to-speech samples).
- A self-hosted Bangla speech-to-text fallback for working without the AI service.
- Real echo cancellation, so people can interrupt the speaker.
- Two-way calls (the family hears the person through the speaker's mic).
- More intents: list today's reminders, what time is it, cancel a reminder.

## Docs to update along the way
- `docs/protocol.md`: the new topic and messages.
- Backend `README.md`: the new endpoints and `.env` keys (`GEMINI_API_KEY`, `AZURE_SPEECH_KEY`, `AZURE_SPEECH_REGION`, `VOICE_DAILY_LIMIT`, Firebase).
- `docs/APP_PLAN.md`: mark M6 and M8.3 as started by V2.
