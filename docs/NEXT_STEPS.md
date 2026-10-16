# Next steps: handoff and plan (written 2026-10-16, updated after step 0)

Read this file first in a new chat. It covers what is live, how we work, what we measured, and what to do next, in order.

**To start a new chat, say:** *"Read `led-demo/docs/NEXT_STEPS.md` and start with step 1."*

---

## 1. Where things stand

Everything in the table is **pushed to git and deployed on the cloud** (smart-speaker.duckdns.org, Oracle VM in Singapore), except where the next block says otherwise.

| Part | Version / commit | What it has |
|---|---|---|
| Firmware | **0.30.1** (`led-demo` `5780c04`) | Opus audio over its own WebSocket (`wss://…/api/audio-link`). Steady audio output (the amp is always fed). ESP-SR echo canceller, used only by the echo test. TLS buffers in PSRAM, so both secure connections fit. |
| Backend | `smart-speaker-backend` `19feebf` | Audio link service (Opus, falls back to MQTT). Echo test options on the mic test. Old-answer audio dropped after the person speaks again. **66 e2e tests pass.** |
| Web | `smart-speaker-web` `69c5987` | Mic page with **🔊 Echo test** and **🔊 Echo test (AEC)** buttons |
| Cloud server | `.env` has `AUDIO_LINK_URL=wss://smart-speaker.duckdns.org/api/audio-link` | Backup taken before deploy: `~/backups/speaker-20261015-215804.*` |

**Since then (2026-10-16): step 0 done, local only.**
- Backend `47e648a` (calling fix, **67 e2e tests pass**): committed, **not pushed, not deployed**. Deploy it with the owner's go-ahead, for example together with step 1.
- **The speaker is flashed for the local setup** (`secrets.h` → `mqtt://192.168.0.109:1883`). Before the cloud test in step 1, switch it back to `mqtts://smart-speaker.duckdns.org:8883` and reflash.
- The PC's LAN address changed from .103 to **.109**. Check `ipconfig` each time.
- A stale phone session was deleted from the **local** database only (Kashem's). See "Lessons from step 0".

**Decided and closed:**
- **Interrupting Momo by voice (barge-in): not wanted.** It was built and tried, then switched off (`BARGE_IN 0` in `firmware/config.h`). Momo stopped on her own echo and on room noise. Don't bring it back, not even by wake word, unless the owner asks.
- Echo cancellation runs in **FD low-cost mode** in PSRAM. It's created once at boot.

**Not committed (leave alone):** `led-demo/hardware/kicad/`. The owner said the KiCad schematic isn't needed.

---

## 2. How we work (rules)

1. **Test locally first, then the cloud.** The owner tests on the real speaker before any cloud deploy.
   - Backend: `cd smart-speaker-backend && npm run start:dev` (port 4000).
   - Web: `cd smart-speaker-web && npm run dev -- --host`.
   - Speaker: in `firmware/secrets.h` (git-ignored), switch `MQTT_URI` to the local line `mqtt://<this PC's IP>:1883` (check `ipconfig`: it was .103, then .109 on 2026-10-16), then `./fw.sh upload COM11` from `led-demo/firmware`.
2. **Local is plain (`mqtt://`, `ws://`), the cloud is encrypted (`mqtts://`, `wss://`).** Problems that only happen with TLS, such as running out of internal RAM, show up **only on the cloud**. After every cloud reflash, read the boot log. You should see "Server online" and "Audio link up: wss://…".
3. **Cloud deploy** (each step needs the owner's go-ahead):
   - back up the database and files;
   - `git archive --format=tar HEAD | ssh … 'tar -x -C ~/speaker'`;
   - `docker compose -f docker-compose.prod.yml up -d --build backend`;
   - upload the web `dist`;
   - set `secrets.h` back to the cloud URI and reflash.

   Details are in the assistant's memory file `cloud-deploy.md`. Never overwrite the server's `.env` or `certs/`.
4. **Secrets:** never print the Gemini key. `secrets.h`, `.env` and the Firebase key stay git-ignored.
5. **Serial log:** COM11 at 115200. Stop any log capture before flashing (the port is busy otherwise).
6. **Firmware edits:** use the Edit tool for C strings. Python heredocs in Git Bash turned `\n` into real line breaks several times.

---

## 3. What we measured (keep for reference)

| What | Result |
|---|---|
| Opus vs raw audio | about **1/10 of the data** (34–68 KB/min vs 300–440 KB) |
| Network to the cloud | about **55 ms** round trip (Singapore) |
| Echo of the speaker at the mic | loud: **−11 dBFS** at 4 kHz, −25 to −33 dBFS for the low and middle notes |
| Echo canceller (sweep test) | removes **15–30 dB**. A person's voice over the sound is kept. |
| Echo canceller cost | **10.1 ms CPU per 32 ms** chunk (FD low-cost; high-perf 13.6 ms). About 142 KB of PSRAM, about 9–13 KB of internal RAM. |
| Echo delay | stable across reboots: `snap` −9408, `AEC_DELAY_CAL` 1318. The canceller's own output delay is 1024 samples (64 ms). |
| Internal RAM | tight: about 49 KB free at idle on the local setup. Watch it on every change that touches Wi-Fi or TLS. |

---

## 4. Feature reality (checked in the code, 2026-10-16)

A feature list claimed 13 features were "done" and 12 needed "a small change". It was checked against the backend, web, app and firmware. **Only 4 of the 13 "done" items really work end to end**, and most "small change" items aren't built at all. Start from this table, not from that list.

Key: ✅ works today · 🟡 partly there · ❌ not built

### Claimed "done"
| Feature | Reality | What exists / what's missing |
|---|---|---|
| Medication reminder | 🟡 | Reminders work: by voice (Momo `set_reminder` → spoken "মনে করিয়ে দিচ্ছি: …" WAV) or from web/app with a recorded message. **Missing:** no label stored (`Alarm` has no label column), no "ওষুধ খেয়েছেন?" check, no family alert |
| Voice-controlled family calling | ✅ | `call_family` → push + call screen on the phone. Fixed in step 0: if no phone or open app got it, Momo says she couldn't reach them; otherwise "খবর পাঠিয়েছি", never "ringing" |
| Companionship / conversation | ✅ basic | Momo chats in Bangla. Short answers, 180 s cap, **no memory between conversations**, owner account only |
| Prayer-time and routine assistant | ❌ | No prayer times, azan or location. Routines are only alarms set by hand |
| Hydration / activity reminders | 🟡 | Only as ordinary repeating reminders set by hand |
| Remote caregiver routine management | ✅ | Family members (not only the owner) can add, edit and delete alarms and record messages, on web and app |
| Bangla speech failure and recovery | 🟡 | Momo asks again; says "এখন বুঝতে পারছি না" if the AI is down. **Silent** if the session drops mid-talk, and **silent** after the wake word when the internet is down |
| Family circle and special-day greetings | 🟡 | The family circle exists (members, roles, call names). No birthdays or greetings |
| Device health and caregiver dashboard | 🟡 | Technical health is live for all members (RAM, CPU, Wi-Fi, crashes). No caregiver view (activity, missed alarms). The app's "speaker offline / restarted / missed alarm" toggles are saved on the phone only and **do nothing** |
| Family presence mode | ❌ | Nothing. "Presence" in the code only means the speaker is online |
| Family announcement across the home | 🟡 | Hold-to-talk to **one** speaker at a time |
| Reminder confirmation and correction | 🟡 | Momo reads the time back; can cancel only the reminder just set. Can't list or change reminders by voice |
| "Repeat and explain" | 🟡 | Not a feature. Momo may do it as normal chat; untested |

### Claimed "a small change makes it work"
| Feature | Reality | Honest size |
|---|---|---|
| Morning greeting and daily plan | ❌ nothing proactive; talks start only from the wake word or a button | medium |
| Family voice messages | 🟡 record a message as a reminder, or upload a file and play it | **small** ("send a message now") |
| Scheduled wellbeing check-ins | ❌ | medium (the server starts a conversation at set times; the speaker already supports it) |
| Emergency help button | ❌ the speaker has **no buttons at all** (no input pins); voice "call family" is the only substitute | hardware + firmware + push |
| Internet loss and offline mode | 🟡 alarms and stored tones work offline, but **only once the clock is set** (no RTC: after a power cut without internet, alarms stay silent). No feedback after the wake word; no offline commands | medium–large (step 4) |
| Bangla stories, poetry, culture | 🟡 chat only; the "one or two sentences" rule and the 180 s cap block real stories | **small** (a story mode) |
| Lost-item memory | ❌ | medium |
| Appointment and schedule assistant | 🟡 one-time reminders on a date | small–medium |
| Missed check-in and caregiver escalation | ❌ no acknowledgement or missed-alarm tracking, no push | medium |
| Personalised speech preferences | ❌ Momo's voice is one server-wide setting (`GEMINI_VOICE`); Bangla fixed; no speed | small–medium |
| Daily memory and personal notes | ❌ transcripts are saved but only the owner can read them; Momo never uses them | medium |
| Gentle reminder to contact family | 🟡 only as a reminder set by hand | small |

### Gaps the list doesn't mention
- **Phone app:** no Momo, wake word or voice-log screens (web only).
- **Voice log:** visible to the owner only.

### Honest small wins
These are the cheapest real improvements. Each comes early in the building blocks of step 2:
- offline / restarted / missed-alarm **push alerts**: the app toggles already exist, and the backend just has to send them (B1);
- a **label** on reminders, shown in the apps (B3);
- voice message **"send now"** (B3);
- a **"contact your family" nudge** (a B2 template);
- Momo **story mode** (instructions only).

---

## 5. The plan, in order

### Step 0: Fix the calling bug ✅ done 2026-10-16 (backend `47e648a`, local only)
- `FamilyService.call` returns `{ phones, apps }`: the phones the push went to, and the open pages or apps that got the live call (`RealtimeGateway.toUser` now returns that count).
- `call_family` with neither answers `{ ok: false, problem: "their phone can't get calls right now" }`, and the conversation isn't logged as `called`.
- When it goes, Momo is told `{ ok: true, sent, note }` and says "…কে খবর পাঠিয়েছি, উনি দেখলে কল করবেন". She never says the phone is ringing.
- New e2e case: a member with no app and no page. The owner tested both cases on the speaker.

**Lessons from step 0 (open follow-ups, not done):**
- "Reached" means **Google accepted the push**, not that the phone showed it. An old login with a stale token still counts. Kashem had a phone-app session from 2026-10-15 that was never logged out, so his calls "went".
- **Follow-up A (small):** when the phone app switches account, it should log out the old session, so that phone stops getting the old person's calls. Check `smart-speaker-app`'s login and logout flow.
- **Follow-up B (medium, fits B1):** a delivery receipt. The app tells the server when a call notification arrives, and Momo waits about 5 s before saying "it reached them".

### Step 1: Stability check on the cloud (small)
Three loose ends from the deploy. One test session with the owner covers all three.

**Progress 2026-10-16: committed, not pushed, not deployed** (firmware 0.30.2 `led-demo` `3188aa8`, backend `667276f`, on top of step 0's `47e648a`). The cloud upload happens once, together with step 0:
- **Found: the speaker "froze" (watchdog restart) about 12 s after nearly every upload**, from 0.27 to 0.30.1. The boot events in the local database show it: `usb_reset`, then `froze` (task `IDLE1`, pc `0x40380107` = core 1 idle). Cause: with USB plugged into the PC and no serial monitor reading, the core's USB serial (`HWCDC`) waits up to 2 s per print (core messages 100 ms per character), so a few prints in one `loop()` pass trip the 5 s loop watchdog. After `power_on` it didn't happen. **Fix (firmware 0.30.2):** `Serial.setTxTimeoutMs(1)` in `setup()`. Flashed twice with no monitor open: no freeze.
- **The 16:29:56 offline/online on 2026-10-16 was that freeze**, not Wi-Fi. The "audio link down" at 16:30:01 was the *old* link (the dead one from before the restart) being found by the server's 20 s ping. The new link stayed up, so the link *did* come back. The backend now closes an old link as soon as a new one from the same speaker logs in, and logs "old audio link closed (a new one replaced it)". New e2e case in `audio-link.e2e-spec.ts` (**68 e2e tests pass**).
- The firmware now logs a failed audio-link attempt ("couldn't connect", "the server didn't accept the login"): the first one, then about once a minute. Before, failed retries were silent.
- **"Too many open files": found.** FFat and the SD card were mounted with **2 open files at most**. A ringing tone + a tone download already use 2, so any third `open`/`exists` failed with ENFILE. No `File` was left open anywhere. Now `FFAT_MAX_FILES` 6 and `SD_MAX_FILES` 4 (`config.h`); the slots are in PSRAM (`CONFIG_FATFS_ALLOC_PREFER_EXTRAM`), so no internal RAM is used.
- **Tested with the owner on the speaker (local, 0.30.2), 2026-10-16:**
  - No restart for 2 h 44 m. The audio link stayed up the whole time.
  - Hear the room (📞 Call) ran the full 10 min (19:31–19:41) with no dropout: 9,600,000 samples (600 s, none lost), 157–165 KB a minute over the link.
  - RAM: 79 KB free, lowest 68 KB since boot (local; check again on the cloud with TLS).
  - Delay: speaker → phone about 1 s at most. **The owner finds it acceptable**, so the buffers stay as they are.
  - Unplugged while an alarm rang: back online in 5 s, the alarm rang again in the same minute, no file errors (while the page listed files at the same time).
- **Open:** was the cloud dropout at 22:12 UTC on 2026-10-15 the same freeze? Check the cloud database's boot events around then (`Event`, `type='boot'`). If it was `froze`, it's fixed; if there's no boot, it was the network.

1. **Dropout during "Hear the room".** At 22:12 UTC on 2026-10-15 the speaker went offline for about 2 s during a listen session, then came back. Was it a restart or Wi-Fi?
   - Record the serial log (no reset) while the owner uses Hear the room for 2–3 minutes. Look for `rst:`, `Guru`, `abort`, "audio link down".
   - Also check the free internal RAM in the stats module during a listen session.
   - If it's a crash, fix the cause. If it's the link, check the heartbeat (20 s ping) and the `MAX_BUFFERED` (64 KB) close.
2. **"A bit of delay" on live audio.** Ask the owner which direction is late, and by how much.
   - Expected total is about 0.4–0.6 s.
   - **Easy win:** 0.29 added a 128 ms play ring plus 4 DMA buffers, about 64 ms more output delay. Try `PLAY_RING` 1024 in `audio.cpp` and check for underruns.
   - Other knobs: `TALK_PREBUFFER` (190 ms) in `config.h`; mic pieces of 120 ms (`PIECE` in `mic.cpp`); the app's playback buffer.
   - **Seen again on 2026-10-16 (local):** right after a boot, the speaker went offline and online once (16:29:56), then the audio link went down at 16:30:01 and **didn't come back** until the backend restarted about 10 minutes later. Conversations fell back to MQTT audio meanwhile. Check the firmware's reconnect logic in `audiolink.cpp`: after "link down", does it retry?
3. **"Too many open files" once at boot** while an alarm was ringing (FFat `fopen` failed, ENFILE 23). Find which code keeps files open: the tone cache check at boot (`tonecache.cpp`), alarm playback (`audio.cpp`), or `files.cpp`. Make sure every `File` is closed.

**Done when:** Hear the room runs 10 minutes without a dropout, the delay is acceptable to the owner, and a boot with an alarm ringing shows no file errors.

### Step 2: Five building blocks, not 25 features
The owner's rule: use cases must be **dynamic**, so new ones take little effort. Almost every item in section 4 is a combination of the same few parts. So build **five building blocks** once, then each use case is a **template**: settings the family turns on and edits in the app, not new code.

**B1. Notifier: tell the family.** One service decides who gets told, how (push now; SMS later) and how often (quiet hours, no repeats).
- Settings are per family member, on the server. Move the app's Account toggles (`smart-speaker-app/app/(tabs)/account.tsx:53-73`), which today are saved on the phone only.
- Event sources, all already in the bridge (`device-bridge.service.ts`): speaker offline for N minutes, speaker restarted or crashed, alarm fired; later routine results and "help".
- Reuses `PushService` and `FamilyService`.

**B2. Routines: the speaker starts something on its own.** One table and one scheduler. A routine is *when* + *what Momo does* + *what to expect* + *what if not*:
- **When:** a time with repeat days, a date (yearly for birthdays), the prayer times of the day, or an event (alarm fired, morning first alarm, no conversation for N days).
- **What:** play a sound or recorded message; Momo says a sentence; or Momo **asks** and listens (a short conversation, started by the server. The speaker already supports this: `mic_record` with `conversation`).
- **Expect:** nothing, a yes/no, or any answer (Momo gets a tool `routine_answer(ok, note)`).
- **If not:** ask again after M minutes (up to K times), then tell the family through B1 ("আম্মা সকালের ওষুধের উত্তর দেননি").
- Every run is logged (who answered what, when), and that log *is* the caregiver dashboard.

**B3. Speaker tools: one tool set used by Momo and the apps.** Everything the speaker can do, as service calls that both the apps (API) and Momo (tools) use:
- alarms: `list_alarms`, `set_reminder` with a **label** (new `Alarm.label` column) and a **tone/song** by name, `change_alarm`, `delete_alarm`;
- sound: `play_music` / `stop_music`, volume;
- messages: `send_message` (play now, or at a time), `announce` (several speakers at once);
- family: `call_family` (fixed in step 0), `routine_*` (add, change, pause a routine by voice: "রোজ রাত ৯টায় ওষুধের কথা জিজ্ঞেস করো").

**B4. Memory: Momo remembers.** One small table of notes per speaker: `kind` (note, item_place, preference, person, summary), text, time.
- Tools: `remember(kind, text)`, `recall(question)`.
- At the start of each conversation, Momo gets the preferences ("আম্মা বলে ডাকবে", "slow, loud") and a short summary of recent talks.
- After each conversation, a one-line summary is saved (from the existing `VoiceLog` transcript).
- The family can see, edit and delete the notes in the app (privacy).

**B5. Context: what Momo and routines know right now.** One function builds a short context block used by Momo's instructions and by routine sentences:
- now (date, Bangla calendar day), today's and the next alarms with labels;
- weather (free API, by the speaker's location: add `Device.location`);
- prayer times (calculated from location, e.g. the `adhan` library: no API needed);
- family special days, and who called recently.

**What each use case becomes:**
| Use case | Built from |
|---|---|
| Medication reminder + "did you take it?" + family alert | B2 (ask, expect yes, then B1) + B3 label |
| Hydration and gentle activity | B2 (say, every 2 h, daytime) |
| Wellbeing check-in, missed check-in escalation | B2 (ask any, then B1) |
| Morning greeting and daily plan | B2 (event: first alarm) + B5 (weather, today's reminders) |
| Prayer-time / Azan | B2 (prayer times) + B5 (location) + an Azan tone |
| Special-day greetings | B2 (yearly date) + B5 (family days) |
| Gentle reminder to contact family | B2 (event: no call for N days) + B3 `call_family` |
| Appointments | B3 label + B2 (date + reminder the day before) |
| Reminder confirmation and correction | B3 (`list_alarms`, `change_alarm`) |
| Lost items, daily notes, personal preferences, companionship memory | B4 |
| Family voice messages, announcements | B3 (`send_message`, `announce`) |
| Device health alerts, caregiver dashboard | B1 + the B2 run log |
| Remote caregiver routine management | the apps over B2 and B3 (already partly true for alarms) |
| Stories and poetry | a Momo "story mode" (longer answers when asked; instructions only) |
| Emergency help | B1 (urgent) + a trigger: voice ("বাঁচাও" / "সাহায্য") now; a hardware button later |

**Order:** B1 → B3 → B2 → B5 → B4. B1 and B3 are small and useful on their own; B2 needs both. Each block gets backend e2e tests (fake AI and fake speaker, as in `test/voice.e2e-spec.ts`) and a local test with the owner before the cloud.

### Step 3: Templates in the apps
- A **"Care routines"** page (web first, then app) lists ready-made templates: Medicine check, Drink water, Morning greeting, Prayer times, Daily check-in, Birthday greetings, Call family nudge.
- Each has a few plain settings (times, who to alert, the sentence) and an on/off switch.
- Templates are data (JSON in the backend), so a new use case is a new template, not new code.
- The caregiver view shows today's routine runs: answered, missed, alerted.

### Step 4: Offline basic commands (the owner insisted on this)
Some things must work **without internet**. After "হাই মোমো", if the cloud can't be reached, the speaker handles a few common commands itself.

- **Commands:**
  - what time is it;
  - what day or date is it;
  - volume up / down;
  - stop or snooze the alarm;
  - when is the next alarm;
  - a "no internet right now" reply for anything else.
- **How it hears them:** the same on-speaker matcher as the wake word (DTW, `wakecore.cpp`). Each command has a few takes recorded in the family's voices, using the existing "record a take" flow on the Wake word page.
- **How it answers:** short Bangla clips made by the backend's TTS **while online**, then cached on the speaker (FFat, the same SHA-checked download as tones).
  - Numbers and times are built from pieces, for example "এখন" + "সকাল" + "৮টা" + "১৫ মিনিট".
  - The data for the answers is already on the speaker: the clock (NTP plus time zone), the alarm list, the volume.
- **When:** only when the cloud is unreachable (MQTT down, or the conversation can't start). Online, Momo handles everything as now.
- **Note:** "stop the alarm" *while it rings* means listening during playback. That's the one place the echo canceller is needed. It's separate from interrupting Momo, but check with the owner before building it.
- **Files:**
  - firmware: new `offline.cpp/.h`; `wake.cpp` (command takes beside the wake takes); `mic.cpp`; `tonecache.cpp` (clips);
  - backend: the wake service (command takes) and a clip builder (TTS);
  - web: a commands section on the Wake word page.

**Done when:** with the router's internet unplugged, each command works for the owner 9 times out of 10, and online behaviour is unchanged.

### Step 5: A wake word for the whole family
Today's matcher recognises best the person who recorded the takes. The elderly parent at the speaker is often someone else.

- **C0 (quick):** up to 3 people × 3 takes. The score is the closest take of anyone.
  - firmware: `wake.cpp` slots, `wake::TAKES` → 9, about 100 KB of PSRAM;
  - web: a person picker on the Wake word page.
- **C1–C5 (bigger, 1.5–2 weeks): a trained neural wake word (microWakeWord).**
  - Synthetic "হাই মোমো" clips from many Gemini voices, plus real family recordings.
  - Bangla negatives (OpenSLR SLR53, Common Voice) and room noise.
  - Train on Colab.
  - Run TFLite Micro on the speaker (esp-tflite-micro and esp-nn are already in the core). The model file lives in FFat, so it can be updated without new firmware.
  - Pass bar: at least 95% detected at 1 m on real family recordings, and at most 0.5 false wakes per hour on Bangla TV and talk.

---

## 6. Useful places

| What | Where |
|---|---|
| Firmware | `led-demo/firmware/`: `mic.cpp` (mic, turns, echo canceller), `audio.cpp` (output, talk, tones), `audiolink.cpp` (WebSocket and Opus), `net.cpp` (Wi-Fi, MQTT, TLS memory), `wake.cpp` / `wakecore.cpp` (wake word), `config.h` (versions and tuning) |
| Build / flash | `cd led-demo/firmware && ./fw.sh upload COM11` (`build`, `monitor` also work) |
| Backend | `smart-speaker-backend/src/`: `hub/` (device hub, audio link), `voice/` (Momo, Gemini live), `mic/`, `common/opus.ts` |
| Backend tests | `npx jest --config test/jest-e2e.json` (fake speaker in `test/helpers.ts`) |
| Message formats | `led-demo/docs/protocol.md` |
| Voice history and milestones | `led-demo/docs/VOICE_PLAN.md` |
| Product roadmap | `led-demo/docs/ROADMAP.md` |
| Architecture page | https://claude.ai/artifact/9NEybBRJFeiws1sWihqnss |
