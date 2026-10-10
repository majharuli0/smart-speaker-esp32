# Roadmap: from demo to product

A smart speaker that rings custom alarms, works as a doorbell and intercom, and takes voice commands. It's controlled from a mobile app, with the web as the first client, both on one backend.

**Where we are:** the working demo has Wi-Fi setup through a hotspot, a local server, a web page, server-triggered alarms with streamed tones, hold-to-talk, volume, and device health (CPU/RAM/flash). It runs on the ESP32-S3 N16R8.

One person builds everything, so the phases run **in order, one at a time**. Tick `[x]` as tasks are done. Time estimates assume one person.

---

## Decisions

**Made (2026-09-30):**

| Decision | Choice | What it means for the plan |
|---|---|---|
| Market | **Home consumers first** | Home features come first (alarms, doorbell, talk, voice). Business features wait. |
| Cloud messaging | **EMQX**, at **no cost or as low as possible** for the demo | Use EMQX's free tier, or run the open-source EMQX ourselves on the same free VM as the backend. No paid managed services. |
| Team | **Solo** | Phases run one after another, and each task stays small enough for one person. |

**Still open:**

| Decision | Options | Needed by | Recommendation |
|---|---|---|---|
| Firmware build tool | arduino-cli + `sketch.yaml` / PlatformIO | Phase 0 | **arduino-cli**: pins the core and library versions and the S3 board settings in a file, and stays compatible with the Arduino IDE |
| Backend hosting (free) | Free-tier VM (e.g. Oracle Cloud Always Free) running Node + Postgres + EMQX / free platform tiers | Phase 2 | **One free VM for everything.** Free platform tiers put apps to sleep, and a sleeping server can't deliver alarms or talk. |
| Mobile framework | React Native (Expo) / Flutter | Phase 3 | **React Native**: shares TypeScript types and the API client with the web |
| Wake word | Stock "Hi ESP" / custom name | Phase 4 | Choose the product name early; a custom wake word needs Espressif to train it, which takes time |

---

## Phase 0: Foundation (1–2 weeks)

**Goal:** a clean repo that anyone can build and test with one command.

- [x] **0.1 Save the base.** Commit the current work and tag it `v0.1-demo`.
- [x] **0.2 Repo layout.** `firmware/` (from `esp32/`), `backend/`, `web/` (from `frontend/`), `app/` (later), `hardware/` (schematics), `docs/`. Remove leftovers: `esp32/format_flash/` and `backend/claims.json`, `sounds.json`, `sounds/`. Move `docs/audio-alarm-design.md` to `docs/archive/`.
- [x] **0.3 Reproducible firmware builds.** arduino-cli with a `sketch.yaml` profile. It pins esp32 core 3.3.12 and the library versions, and sets the board options: `FlashSize=16M`, `PSRAM=opi`, `PartitionScheme=app3M_fat9M_16MB`, `CDCOnBoot=cdc`. No more IDE menu settings to forget. Add scripts: `build`, `upload`, `monitor`.
- [x] **0.4 Firmware config.** A `config.h` holding the server host/port, feature switches and pins, instead of values scattered through the sketch. The server address can later come from setup and be saved on the device.
- [x] **0.5 Firmware version.** `FW_VERSION` sent in `hello` and shown on the device card.
- [x] **0.6 Backend hygiene.** `npm start` / `npm run dev` / `npm test` / `npm run check`. Settings come from `backend/.env` (`PORT`, `MDNS_HOST`, `MDNS`, `DATA_DIR`; see `.env.example`), read by Node's built-in `.env` support, so no new packages. `.editorconfig` keeps formatting consistent. A full formatter (Prettier) was left out to keep dependencies at zero; add it if the code grows.
- [x] **0.7 Real tests.** Move the ad-hoc simulated-device tests into `backend/test/` using `node:test`. Cover the alarm scheduler (timezones, dedup, weekdays), the relay, tone upload (filename sanitizing), talk routing and volume.
- [x] **0.8 CI.** GitHub Actions: build the firmware for esp32s3 (and esp32), run the backend tests on every push.
- [x] **0.9 Protocol spec.** `docs/protocol.md` as the single source for every message type, moved out of the README.

**Done when:** a fresh clone builds the firmware and passes the tests with one command each, and CI is green.

---

## Phase 1: Firmware v1 (4–6 weeks)

**Goal:** a device that works reliably on its own, can be updated over Wi-Fi, and is set up the standard way.

**Structure**
- [x] **1.1 Split the sketch into modules:** `audio` (player, talk, volume), `net` (Wi-Fi, cloud link), `alarms`, `provisioning`, `stats`, `ota`, `ui` (buttons, LED).

**Alarms that don't need the network**
- [x] **1.2 Time.** NTP with the user's timezone (`configTzTime`, with a POSIX timezone string from the backend), periodic resync, and a "time is valid" flag. Alarms wait until the time is known.
- [x] **1.3 Alarms on the device.** The server sends each device its own list (`alarms_sync`, versioned by content hash); the device saves it in flash, confirms with `alarms_ack`, and rings from its own clock once the time is valid. If the tone can't be downloaded it plays a built-in beep. Network start no longer blocks, so a device that restarts while the router is down keeps running and reconnects by itself.
- [x] **1.3b Alarm extras.** Snooze from the page (rings the same tone again in 9 min, timed by the device so it works offline), 30 s volume fade-in for alarms, one-time alarms (a date instead of weekdays; switched off once they ring). Snooze from a physical button comes with 1.6.
- [x] **1.4 Tone cache (9.9 MB `ffat`).** Formatted on first boot. The alarm sync lists the tones the alarms use (size + SHA-256); the device downloads missing ones in the background to a temp file, checks size and SHA-256, renames it to its hash name, and deletes tones no alarm uses. Alarms play the stored copy (offline, gapless loop), then streaming, then the beep.
- [ ] **1.4b External microSD card.** *(Paused 2026-10-10: built and in the firmware since 0.7.0, but the card never answers (no reply to CMD0 in any wire order: power or contact, likely the unsoldered header). The firmware works without a card; the page shows the reason. Resume after re-soldering.)* SPI module on GPIO 10 (CS), 11 (MOSI), 12 (SCK), 13 (MISO); card formatted FAT32. Gigabytes instead of 9.9 MB, for whole songs and, later, the recorded/labelled audio library (Phase 4).
  - **Mount at boot**, and notice when the card is **inserted or removed** while running (re-check every few seconds).
  - **Tone storage uses the card when present**, with the built-in 9.9 MB `ffat` as the fallback. Same download, SHA-256 check and clean-up as 1.4.
  - **Alarms never depend on the card:** if it's missing or unreadable at ring time, play from `ffat`, then streaming, then the beep.
  - **Page:** an "SD card" row in the storage table (size, used, card type), "no card" when empty.
  - **Done when:** an alarm plays its tone from the card offline; pulling the card out mid-ring falls back without a crash; re-inserting it is picked up without a restart.
- [x] **1.5 Built-in sounds.** A ding-dong (two bell strikes, generated in code) for the doorbell, and the 880 Hz beep used when an alarm tone can't be found. No files needed, so they always work.

**Physical interface**
- [ ] **1.6 Buttons and LED.** Buttons (e.g. GPIO 7, 15, 16): press for stop/snooze, a doorbell button, and a 10 s hold for factory reset. Onboard RGB LED (GPIO 48) states: setup, connecting, online, ringing, error. *(Skipped for now: no buttons fitted yet.)*

**Setup and security**
- [ ] **1.7 Bluetooth setup (Wi-Fi provisioning).** Espressif's Bluetooth provisioning (`WiFiProv` / `network_provisioning`) with a security code (proof of possession) printed in the QR label. Keep the hotspot as a fallback.
- [ ] **1.8 Link device to account (claim).** After Wi-Fi setup, the device sends a one-time claim token, and the backend links it to the user who scanned it.
- [ ] **1.9 Encrypted connections.** WSS/MQTTS verified with the built-in certificate bundle (`esp_crt_bundle`). Connect to a DNS hostname in production; mDNS for development only.
- [ ] **1.10 MQTT client** (if chosen). `esp-mqtt` with topics `devices/{id}/cmd`, `/state`, `/events` and a last-will message for offline status.
- [x] **1.11 Updates over Wi-Fi (OTA).** Upload `firmware.ino.bin` on the page; the server reads board + version from a marker in the file and keeps the newest per board. A device updates on request: download, SHA-256 check, image check, restart. The new version must reach the server within 3 min or the bootloader rolls back. Image signing comes with secure boot in 5.4.

**New features**
- [x] **1.12 Doorbell (web).** 🔔 button on the owner's page and a visitor page (`bell.html?d=<id>`, for a QR code at the door). The device plays the built-in ding-dong; 10 s cooldown; every ring is logged and shown, with a banner and an optional system notification. **Still to do:** a physical button (with 1.6) and ringing over Bluetooth.
- [ ] **1.13 Microphone.** INMP441 on the second I2S port. Streams from the device to the browser so the owner can answer the doorbell, and is the base for voice commands.

**Reliability**
- [ ] **1.14 Watchdog, brownout handling, memory-leak alerts,** and crash dumps saved to the `coredump` partition and uploaded on the next boot.

**Done when:** with the router unplugged, an alarm still rings on time with its custom tone. An OTA update and a rollback are demonstrated. Bluetooth setup works from a test app. The device runs 72 h with no reboot.

---

## Phase 2: Backend and web (4–6 weeks)

**Goal:** a multi-user cloud backend with one API for both web and app.

- [ ] **2.1 Stack.** Node + TypeScript, Fastify, Postgres (Prisma), S3-compatible object storage.
- [ ] **2.2 Data model.** `users`, `households` (sharing), `devices` (owner, name, timezone, firmware version, last seen), `device_claims`, `alarms` (+ version), `media` (label, tags, transcript, duration, hash, storage key), `events` (doorbell, alarm fired/missed, device offline).
- [ ] **2.3 Auth.** Email and Google/Apple sign-in, with short-lived access tokens refreshed by a refresh token. Devices log in with their own per-device credentials, separate from users.
- [ ] **2.4 REST API v1 + OpenAPI spec.** Generate a typed client from the spec, used by both web and app. Endpoints: `/auth`, `/devices` (claim, rename, remove, settings), `/alarms`, `/media` (upload with pre-signed URLs), `/events`, `/devices/:id/commands`.
- [ ] **2.5 Device gateway.** Broker integration, online status, acknowledgements for delivered commands, pushing alarm lists to devices.
- [ ] **2.6 Media pipeline.** The server converts audio to the device format with ffmpeg, instead of only in the browser. It also computes length and a waveform preview, and enforces limits.
- [ ] **2.7 Push notifications.** FCM/APNs for doorbell presses, missed alarms and devices going offline.
- [ ] **2.8 Web app.** React + Vite using the generated client. Pages: login, devices, device detail (alarms, library, health, talk), and setup through Web Bluetooth (Chrome).
- [ ] **2.9 Fleet dashboard.** All devices, firmware versions, health telemetry, OTA rollout controls.
- [ ] **2.10 Tests.** API integration tests, and end-to-end tests with simulated devices (today's fake-device scripts, expanded).

**Done when:** two accounts are fully separated. An alarm created on the web syncs to the device and rings with the network off. Tones are served through a CDN.

---

## Phase 3: Mobile app (6–8 weeks)

**Goal:** the main product for users.

- [ ] **3.1 React Native (Expo with a custom dev build),** sharing types and the API client with the web.
- [ ] **3.2 Screens.** Sign-in, add device (scan QR → Bluetooth setup → link to account), devices, alarms, library (record, upload, label), doorbell and visitor log, talk, settings.
- [ ] **3.3 Bluetooth setup** using Espressif's official provisioning libraries for Android/iOS.
- [ ] **3.4 Push notifications.** Tapping a doorbell notification opens the talk screen.
- [ ] **3.5 Talk.** WebRTC for talking over the internet; the current PCM stream is acceptable at first.
- [ ] **3.6 Release.** TestFlight and Play internal testing; privacy policy covering the mic.

**Done when:** a non-technical tester goes from unboxing to a first alarm in under 2 minutes, on both iOS and Android.

---

## Phase 4: AI voice assistant (6–8 weeks)

**Goal:** "Hey <name>, set an alarm for 7" and "play the birthday song".

- [ ] **4.1 Wake word on the device.** ESP-SR WakeNet (the S3 with PSRAM runs it). Start with a stock wake word; order the custom one early.
- [ ] **4.2 Sending audio after the wake word.** 16 kHz PCM, or Opus, over WSS. Detect when the person stops speaking (voice activity detection).
- [ ] **4.3 Voice service.** Streaming speech-to-text → a language model with tool calling → streaming text-to-speech → device speaker.
- [ ] **4.4 Tools.** `set_alarm`, `delete_alarm`, `list_alarms`, `snooze`, `stop`, `set_volume`, `play_media(query)`, `record_memo(label)`, `announce(rooms)`, time and weather.
- [ ] **4.5 Labelled audio library.** Labels, tags and automatic transcripts, with search by meaning (embeddings), so "that birthday song" finds the right clip.
- [ ] **4.6 Safety and cost.** Read back destructive actions before doing them; per-household rate and cost limits.
- [ ] **4.7 Privacy.** Hardware mic-mute switch with its own LED. Listen only after the wake word. A clear data-retention policy, opt-in.

**Done when:** 90% of a fixed set of 50 commands succeed, and the median response time is under 3 s.

---

## Phase 5: Hardware product (after Phase 4; only 5.1–5.2 can start early, while waiting on board orders)

- [ ] **5.1 Schematic.** ESP32-S3-WROOM-1 N16R8, USB-C 5 V/2 A with protection, **battery-backed clock (e.g. DS3231)** so alarms still ring after a power cut with no internet, MAX98357A (or a higher-power amp), **PCM5102A line-out + 3.5 mm jack that mutes the built-in amp when used** (don't wire a jack to the MAX98357A output), INMP441 mic(s), buttons, RGB LEDs, mic-mute switch.
- [ ] **5.2 Circuit board.** Layout with careful audio grounding; first run of 5–10 boards.
- [ ] **5.3 Enclosure.** Designed for the speaker's sound; 3D-printed prototypes, then a production mould.
- [ ] **5.4 Factory setup.** Flash the firmware, enable secure boot + flash encryption, write each device's ID and credentials, print the QR label (ID + security code). A test jig that checks speaker, mic and buttons.
- [ ] **5.5 Certification.** FCC Part 15B, CE (RED/EMC/LVD), and local approval where sold (e.g. BTRC in Bangladesh). Pre-compliance scan first.

**Done when:** 10 prototypes pass the factory test and the pre-compliance scan.

---

## Phase 6: Deployment and launch (3–4 weeks)

- [ ] **6.1 Infrastructure as code;** dev, staging and prod environments.
- [ ] **6.2 Hosting.** Start on the free VM: Docker Compose running the backend, Postgres and EMQX, plus free TLS certificates (Let's Encrypt) and a domain. Move to paid managed services only when the number of devices needs it.
- [ ] **6.3 Monitoring.** Logs, metrics and alerts: device-offline rate, missed-alarm rate, OTA failures. The device health stats become fleet-wide monitoring.
- [ ] **6.4 OTA rollout policy.** 1% → 10% → 100% of devices, halting automatically if crashes rise.
- [ ] **6.5 Beta** with 20–50 units, plus a feedback and support process.
- [ ] **6.6 Launch.** Pricing, user docs, support, privacy policy and terms.

**Done when:** a 30-day beta with zero missed alarms, and more than 99% of devices crash-free.

---

## Features that set us apart (fold into phases 1–4)

- **Prayer-time alarms (Azan):** worked out daily from the device's location. Phases 1 and 2.
- **Care reminders:** the person presses a button to confirm; if they don't, the family's app is notified. Phases 1–3.
- **Gradual wake-up + spoken morning briefing:** weather and the day's agenda. Phases 1 and 4.
- **Weather/traffic-aware alarms** that ring earlier when needed. Phase 2.
- **Announce to every room.** Phases 2 and 3.
- **Group bell schedules** for schools, factories and mosques. Phase 2, if we go after that market.
