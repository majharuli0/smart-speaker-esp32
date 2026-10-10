# Smart Speaker: real backend (NestJS), React web app, React Native app

## Context
The demo works end to end:
- **Firmware** (`firmware/`, ESP32-S3, MQTT over TLS to EMQX Cloud).
- **Single-file Express server** (`backend/server.js`): MQTT bridge, `.json` files for storage, no accounts.
- **Single HTML page** (`web/index.html`).

The user now wants the production shape, while the Oracle Cloud server is unavailable:
- a backend built on a **proper framework**,
- a **React web app**,
- a **React Native app** that adds a speaker by **scanning its QR code and sending Wi-Fi over Bluetooth**, reusing the approach of the existing **Seenyor Installer** app.

Everything runs locally first (Postgres in Docker) and moves to the cloud later.

**Decisions:**

| Topic | Decision |
|---|---|
| Repos | **Separate repos**: `smart-speaker-backend`, `smart-speaker-web`, `smart-speaker-app` (new GitHub repos). This repo keeps the **firmware** and the product docs. |
| Database | **Postgres in Docker** (docker compose), the same as the future cloud |
| App platforms | **Android and iOS from day one** |
| App MVP | **Login + add device + control** |
| Wi-Fi provisioning | **ESP-IDF unified provisioning**: Arduino's built-in `WiFiProv`, over Bluetooth, with an encrypted session and a PoP (security code). **Not BluFi**: BluFi isn't compiled into our Arduino core (`CONFIG_BT_NIMBLE_BLUFI_ENABLE` off), and enabling it would mean rebuilding the core's libraries. |

## Stack: matched to the Seenyor projects, so it's familiar

| Part | Choice | Same as Seenyor? |
|---|---|---|
| Backend | **NestJS 10 + TypeScript**, Node 20, `/api/v1` prefix with URI versioning, global `ValidationPipe` (class-validator), helmet, CORS | Yes (`E:\seenyor-backend\src\main.ts`) |
| Database | **Postgres + Prisma**, with real migrations | Different: Seenyor uses Mongo with no migration tool. Prisma gives typed queries and migrations. |
| Auth | Hand-rolled JWT **access + refresh** per login session (refresh token hashed in a `Session` row), bcrypt, `@Public()` decorator + global guard | Same pattern as `seenyor-backend/src/modules/auth` + `sessions` |
| Realtime to web/app | **Socket.IO** gateway (`@nestjs/websockets`), JWT on connect, one room per user | Yes: Seenyor's gateways and socket.io-client |
| Devices | **MQTT bridge module** (`mqtt` 5), a port of today's `server.js` logic and topics | Like `seenyor-realtime-gateway` |
| API docs + client | `@nestjs/swagger` **generated from code** at `/api/docs`; **orval** generates typed **React Query hooks** from it, for web and app alike | Improves on Seenyor's hand-maintained swagger.json |
| Tests | Jest + supertest e2e against a test Postgres (docker compose) | Yes |
| Web | **React 18 + Vite + TypeScript**, react-router 6, **TanStack Query 5**, **Tailwind + shadcn/ui**, zustand for auth, react-hook-form + zod | Same libraries; TypeScript throughout |
| App | **Expo SDK 54, prebuild / dev-client**, expo-router, **NativeWind 4**, zustand + MMKV, TanStack Query, expo-camera for QR, socket.io-client | Same as `F:\seenyor-installer` (the newer copy) |
| App provisioning | **`@orbital-systems/react-native-esp-idf-provisioning`** (wraps Espressif's official Android/iOS provisioning libraries; has an Expo config plugin). Check the version against Expo 54 / new architecture when setting up. | Replaces Seenyor's BluFi module; the screen flow is copied |

## How the pieces connect
```
 App (Expo) ─┐  REST /api/v1 (JWT)      ┌─ Postgres (docker)
 Web (React) ┼──────────────────────────► NestJS backend ──┤
             └─ Socket.IO (live status,  │                   └─ files: tones, firmware (local volume → S3/R2 later)
                talk audio)              │ MQTT (TLS)
                                         ▼
                                   EMQX Cloud ◄── ESP32 speakers (unchanged topics ss/dev/<id>/...)
 Phone ── Bluetooth (ESP-IDF provisioning, PoP from QR) ──► ESP32 (first setup only)
```
- **Device protocol unchanged:** the MQTT topics and JSON messages in `docs/protocol.md` stay as they are, so **firmware needs no change for the backend move**. Only provisioning changes (M4).
- **Commands:** web/app → **REST** `POST /api/v1/devices/:id/commands {type, ...}` (ring, stop, snooze, volume, blink, doorbell, partitions). This is simple and works the same from the app.
- **Live data:** device → MQTT → backend → **Socket.IO** to the owner's sockets only (stats, ringing, alarms_ack, cache, ota progress, events). **Hold to talk:** binary audio over Socket.IO → `ss/dev/<id>/audio`.

## Owning a device (claim) with the QR code
- **The device QR code** printed on the speaker contains `SS:<deviceId>:<pop>`, e.g. `SS:esp32-2884856409fc:7f3a91c2`.
  - The PoP is `first 8 hex of HMAC-SHA256(DEVICE_SECRET, deviceId)`.
  - `DEVICE_SECRET` lives in `firmware/secrets.h` and in the backend `.env`, so **no per-device database row is needed at manufacture**.
  - A script `tools/device-qr` prints the QR code for any device ID.
- **The PoP does two jobs:**
  1. It's the **provisioning security code**: the phone needs it to open the encrypted Bluetooth session.
  2. It's the **proof of ownership**: `POST /devices/claim {deviceId, pop}`. The backend recomputes the HMAC; if it matches, the device is bound to the user. A device that's already claimed returns 409, unless the claiming user is the current owner.
- **Releasing:** `DELETE /devices/:id` releases it and sends the device `reset_wifi`, for "remove device / give away".
- **Every REST and Socket.IO action checks ownership.** The open "anyone controls every device" model is gone.

## Data model (Prisma)
- `User` (email, passwordHash, name)
- `Session` (userId, refreshTokenHash, deviceName, pushToken, expiresAt): one row per logged-in phone/browser
- `Device` (id = `esp32-…`, ownerId?, name, timezone, board, fwVersion, lastSeenAt)
- `Alarm` (deviceId, time, days[], date?, toneId, enabled)
- `Tone` (ownerId, name, path, size, sha256, durationMs)
- `Event` (deviceId, type: doorbell | boot | alarm_fired | ota, data json, at)
- `Firmware` (board, version, path, size, sha256)

**Migrating today's data:** a one-off script imports `backend/alarms.json`, `devices.json`, `events.json`, `tones/` and `firmware/` into the new database under the first user.

## Milestones (one at a time; each ends working, tested and committed)

### M1 Backend skeleton (`smart-speaker-backend`)  ✅ *done 2026-10-10 (local repo `E:/LED Project/smart-speaker-backend`, 7 e2e tests)*
- Nest app with:
  - the conventions above;
  - `docker-compose.yml` (postgres:16 + the app), `.env.example`;
  - Prisma schema + first migration.
- **Auth module:** `register`, `login` (returns access + refresh; stores a hashed refresh token in `Session`), `refresh`, `logout`, `me`. Global JWT guard with `@Public()`.
- **Tooling:** Swagger at `/api/docs`; `GET /api/v1/health`; ESLint + Prettier; GitHub Actions running lint + unit + e2e (with a Postgres service).
- **Done when:** register → login → `me` → refresh → logout works in the e2e tests, and Swagger shows them.

### M2 Devices, MQTT bridge and all current features (backend)  🟡 *code done 2026-10-10 (24 e2e tests); check with the real speaker pending*
Port `backend/server.js` into Nest modules, reusing its logic and tests as the spec:

| Module | Contents |
|---|---|
| `mqtt` | Connect / last will / `ss/server/online` + `ss/server/http`, device presence, hello, `watch` |
| `devices` | List mine, claim, release, rename, timezone (POSIX conversion: today's `posixTz()`), commands endpoint, ownership guard |
| `alarms` | CRUD, `alarms_sync` with a content-hash version, one-time alarms switched off on `alarm_fired` |
| `tones` | Upload any audio; **the server converts it to 16 kHz mono WAV with ffmpeg** (`ffmpeg-static`), so the app needs no browser audio code. Size + SHA-256 for device storage. |
| `firmware` | Upload with the `SSFW` marker check, OTA start, progress events |
| `doorbell` | Public visitor page, `POST /bell/:id`, QR page, events |
| `events` / `realtime` | Socket.IO gateway: per-user rooms, replay of the latest device reports, talk audio |

- **Tests:** port the 49 existing tests (relay, alarms, tones, talk, firmware, doorbell, health, MQTT with aedes) to Jest e2e, adding ownership checks (user B can't see or control user A's device).
- **Done when:** the real ESP32, unchanged (firmware 0.11.x), connects through EMQX and works fully against the new backend, driven from Swagger/curl.

### M3 Web app (`smart-speaker-web`)  🟡 *code done 2026-10-10 (checked in headless Edge against the test backend); check with the real speaker pending. Kept simpler than planned: plain Tailwind classes and forms, no shadcn/ui or react-hook-form.*
- Vite + React + TS + Tailwind/shadcn; orval-generated hooks from the backend's OpenAPI; an axios instance with Bearer token and **refresh on 401** (fixing the gap noted in seenyor-frontend); a Socket.IO hook for live data.
- **Pages:**
  - Login / register.
  - Devices list.
  - **Device page:** everything today's page has (sound, doorbell, device, volume, alarms, health & storage, update).
  - Tones.
  - Firmware.
  - **Add device:** type or paste the QR code text → claim, for devices already on Wi-Fi.
- The current `web/index.html` design is the visual reference: white, grouped cards.
- **Done when:** with two accounts, each sees and controls only their own speaker, and every current feature works.

### M4 Firmware: Bluetooth provisioning (this repo, firmware 0.12.0)
- Replace the WiFiManager setup hotspot with **`WiFiProv` over BLE**, `WIFI_PROV_SECURITY_1`, PoP = HMAC as above, Bluetooth name `SS-XXXX` (last 4 hex of the ID), freeing Bluetooth memory after provisioning.
- **Reset Wi-Fi** (and a factory reset) re-enters provisioning.
- Print the device QR text on the serial port at boot, for development.
- **Check first:** `WiFiProv` and NimBLE fit next to Wi-Fi + TLS + MQTT in RAM. The Health page shows it; the S3 has 8 MB PSRAM.
- **Done when:** Espressif's own "ESP BLE Provisioning" app can set up Wi-Fi using the QR code, and the device then comes online through EMQX.

### M5 Mobile app (`smart-speaker-app`), MVP
- **Copy the structure** of `F:\seenyor-installer` (Expo 54 prebuild, expo-router, NativeWind, zustand + MMKV auth store, the `src/api/client.ts` fetch wrapper with refresh, react-query + MMKV persister, `WebSocketContext` pattern).
- **Do not copy:** `sn-ins.jks`, `key.md`, or the BluFi module.
- **Add-device flow**, the same screens as the Seenyor flow:
  1. QR scan (`expo-camera`, parses `SS:<id>:<pop>`)
  2. Bluetooth permissions (copy the Android API-level logic and `withBluetoothScanPermission.js`)
  3. Find `SS-XXXX` → connect with the PoP
  4. Wi-Fi list (from the device)
  5. Password → send → wait for "connected"
  6. `POST /devices/claim`
  7. Wait until online (Socket.IO presence, or poll `GET /devices/:id`) → name it → done
- **Screens:** login/register, my speakers, speaker detail (sound, volume, alarms add/edit/toggle with tone picker, doorbell log, health summary), tones (upload from the phone's files).
- **Done when:** on a real Android phone and a real iPhone, a factory-fresh speaker goes from QR scan to ringing a test alarm, with no laptop involved.

### M6 Push notifications
- Firebase Cloud Messaging (FCM): `firebase-admin` in the backend (Seenyor pattern); `@react-native-firebase/messaging` (or `expo-notifications` + FCM) in the app.
- The push token is stored on the `Session` at login.
- **Pushes for:** doorbell ring, missed alarm, device offline for more than 10 min, crash restart.

### Then
- Retire `backend/` and `web/` from this repo, once the new backend + web reach parity (M2–M3) and the data has been migrated.
- Cloud deployment (Oracle VM: docker compose + Caddy for HTTPS) when it's available again.

## What we'll reuse (with paths)
- **Device behaviour / protocol:**
  - `docs/protocol.md`
  - `backend/server.js` (`posixTz`, `alarmsFor` / `syncAlarms` hashing, `toneInfo`, `readFirmwareMarker`, `ringDoorbell`, the MQTT link bridge, `REPLAYED_REPORTS`, `watch`)
  - `backend/test/*.test.js`: the behaviour spec to port
- **Web reference UI:** `web/index.html` (device card layout), `web/bell.html` (visitor page), `web/mic-worklet.js` (talk capture)
- **App:** `F:\seenyor-installer`:
  - `src/api/client.ts`, `src/api/endpoints.ts`
  - `src/store/authStore.ts`, `src/utils/mmkvPersister.ts`
  - `src/context/WebSocketContext.tsx`
  - the installation screens' structure (`ScanDeviceScreen`, `WifiConfigScreen`, `ConnectingScreen`, `QrScanScreen`)
  - `withBluetoothScanPermission.js`, `app.json` permissions
- **Backend patterns:** `E:\seenyor-backend`:
  - `src/main.ts` (prefix, versioning, pipes)
  - `src/modules/auth` + `src/modules/sessions` (JWT access/refresh, hashed refresh in a session)
  - `src/common/decorators/public.decorator.ts`
  - FCM in `src/modules/alarms/alarms.service.ts`

## Verification
- **M1–M2:**
  - `docker compose up` → Swagger at `/api/docs`.
  - Jest unit + e2e green in CI.
  - With the real speaker: claim it, set an alarm (it rings at the time, even with the backend stopped), test tone, doorbell, volume, OTA update, all through the new backend.
- **M3:** two browser profiles with two accounts: isolation holds, and every feature from today's page works.
- **M4:** Espressif's "ESP BLE Provisioning" app provisions a freshly erased device using the QR code; a wrong PoP is rejected.
- **M5:** on one Android and one iOS phone: erase the device → scan QR → Wi-Fi → claimed → appears online → set an alarm → it rings.
- **M6:** a doorbell ring shows a push notification on a locked phone.

## Needed from the user along the way
- Create 3 empty GitHub repos: `smart-speaker-backend`, `smart-speaker-web`, `smart-speaker-app`.
- Docker Desktop running (for Postgres).
- For M5–M6: a Firebase project (FCM) and, for iOS builds, a Mac with Xcode + an Apple developer account.
