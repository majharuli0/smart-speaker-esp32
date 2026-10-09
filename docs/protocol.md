# Protocol

How the ESP32, the server and the web page talk to each other. This is the single reference: if code and this file disagree, fix one of them.

**Version:** matches firmware `0.6.0` and backend `0.2.0`.

---

## 1. Connections

| What | Where | Used by |
|---|---|---|
| Web page | `GET http://<server>:3000/` (serves `web/`) | Browser |
| **WebSocket** (all messages below) | `ws://<server>:3000/` | Devices and browsers, one connection each |
| Tone upload | `POST /tones` | Browser |
| Tone download | `GET /tones/<name>.wav` | Device (while ringing), browser (preview) |
| Mic processor | `GET /mic-worklet.js` | Browser (hold to talk) |
| Firmware upload | `POST /firmware` | Browser |
| Firmware download | `GET /firmware/<board>.bin` | Device (while updating) |
| Server discovery | mDNS: `led-server.local` → the server's LAN IP (A record) | Device, at boot |

- The device looks up `led-server.local` once at boot, and keeps retrying every second until it's found. It then keeps a WebSocket open to that IP, reconnecting every 3 s if it drops.
- All HTTP responses allow any origin (CORS `*`, header `X-Filename` allowed), so the page also works when opened as a file.

---

## 2. Identities

- **Device ID:** `esp32-` + the chip's factory MAC address as 12 lowercase hex characters, e.g. `esp32-2884856409fc`. It never changes for a given chip.
- **Displayed UID:** the page shows the 12 hex characters in uppercase (`2884856409FC`). Messages always use the full ID.
- **Browsers** are anonymous for now: no login, and every browser sees every device.

---

## 3. WebSocket messages

All text messages are JSON objects with a `type`. The only binary messages are talk audio (section 4).

### 3.1 Hello (first message on every connection)

| From | Message | Server's response |
|---|---|---|
| Device | `{type:"hello", role:"device", deviceId, fw, board}` | Registers the device, then sends `devices` to all browsers. The device follows up with `volume` and `partitions`. |
| Browser | `{type:"hello", role:"browser"}` | Sends that browser `devices`, `alarms`, `tones` and `firmware` |

If a device reconnects before its old connection closes, the new connection replaces the old one. The device stays online.

### 3.2 Server → browsers

| Message | When |
|---|---|
| `{type:"devices", devices:[deviceId, ...]}` | On browser hello, and whenever a device comes online or goes offline |
| `{type:"alarms", alarms:[Alarm, ...], versions:{deviceId: version}}` | On browser hello, and after any alarm change. All alarms for all devices, plus the version each device should confirm (see 5). |
| `{type:"tones", tones:["name.wav", ...]}` | On browser hello, and after an upload |
| `{type:"talk_denied", deviceId, reason:"busy"\|"offline"}` | Only to the browser whose `talk_start` was refused |
| `{type:"firmware", firmware:{board: {board, version, size, sha256, uploaded}}}` | On browser hello, and after a firmware upload. The newest file per board. |

### 3.3 Browser → server (handled by the server, not relayed)

| Message | Effect |
|---|---|
| `{type:"alarm_save", alarm:Alarm}` | Create (no `id`) or replace (same `id`). Invalid alarms are ignored silently. See section 5. |
| `{type:"alarm_delete", id}` | Delete the alarm |
| `{type:"talk_start", target}` | Starts a talk session if nobody else is talking to `target` and it's online. Otherwise replies with `talk_denied`. On success it's also relayed to the device. |
| `{type:"talk_stop", target}` | Ends the session; also relayed to the device |
| `{type:"ota", target}` | Update `target` to the newest firmware for its board: sends it `ota_start`. If there's no file for its board, or it's offline, replies `{type:"ota", deviceId, state:"failed", error}`. |
| `{type:"set_timezone", target, tz}` | Saves an IANA zone name (e.g. `Asia/Dhaka`) for that device and sends it a `timezone`. Unknown names are ignored. |

### 3.4 Browser → device (relayed as-is to `target`)

The server forwards any other browser message to the device named in `target`. If the device is offline, the message is dropped.

| Message | Device does |
|---|---|
| `{type:"blink", target}` | Lights the built-in LED for 300 ms |
| `{type:"ring", target, tone}` | Plays `tone` now (the **Test tone** button). Same behaviour as an alarm. |
| `{type:"stop", target}` | Stops ringing and talk |
| `{type:"talk_start", target}` / `{type:"talk_stop", target}` | See 3.3 and section 4 |
| `{type:"volume", target, value}` | Sets volume 0–100 and saves it on the device, then replies with `volume` |
| `{type:"volume", target}` | Only asks; the device replies with `volume` |
| `{type:"partitions", target}` | Asks for the flash layout; the device replies with `partitions` |
| `{type:"reset_wifi", target}` | Forgets Wi-Fi and restarts into the setup hotspot `LED-Setup-xxxx` |

### 3.5 Server → device (from the server itself)

| Message | When |
|---|---|
| `{type:"timezone", tz, name}` | On connect, then every hour. `tz` is the POSIX rule the ESP32 uses, `name` the IANA zone. See section 8. |
| `{type:"alarms_sync", version, alarms:[{id, time, days, tone, enabled}], tones:[{name, size, sha256}]}` | On connect, after any change to this device's alarms, and when a tone they use is re-uploaded. Only its own alarms, plus the tones they use (section 5). |
| `{type:"talk_stop"}` | The browser that was talking closed its tab or lost its connection |
| `{type:"ota_start", path, version, size, sha256}` | After a browser's `ota`. See section 9. |

### 3.6 Device → browsers (relayed to all browsers, with `deviceId` added)

| Message | When |
|---|---|
| `{type:"ringing"}` / `{type:"stopped"}` | A tone (or the built-in beep) starts / stops: Stop pressed, or the 60 s limit reached |
| `{type:"alarms_ack", version, count}` | After every `alarms_sync`: the list is saved on the device |
| `{type:"alarm_fired", alarmId, time}` | The device rang an alarm from its own clock |
| `{type:"ota", state, progress?, error?, version}` | Update progress: `downloading` (0–100, every 10%), `restarting`, then from the new version `done`, or `failed`. `rolled_back` means the new version failed to start and the device went back to `version`. |
| `{type:"cache", stored, wanted}` | How many of its alarms' tones are stored on the device. After each sync and each finished download. |
| `{type:"talking"}` / `{type:"talk_stopped"}` | Talk playback starts / ends |
| `{type:"volume", value}` | On connect, and after any `volume` request |
| `{type:"stats", ...}` | Every 2 s while connected. See 6.1. |
| `{type:"partitions", ...}` | On connect, and when asked. Measured once at boot. See 6.2. |

---

## 4. Talk audio (binary)

```
browser: talk_start → binary chunk, chunk, ... → talk_stop
```
- **Format:** raw PCM, 16 kHz, 16-bit signed little-endian, mono. The browser sends **640-byte chunks** (320 samples = 20 ms).
- **Routing:** the server forwards a browser's binary messages only to its current talk target, and only between `talk_start` and `talk_stop`. Everything else is dropped.
- **Backpressure:** the server drops a chunk if more than 16,000 bytes (~0.5 s) are already waiting to be sent to that device, so the delay can't keep growing.
- **Device:**
  - Holds a 4,096-sample buffer (256 ms). It starts playing once 100 ms are buffered, and drops the oldest audio when the buffer is full.
  - If no audio arrives for 150 ms, it waits for 100 ms of audio to build up again before resuming.
  - On `talk_stop` it plays what's left in the buffer, then stops.
- **Priority:** an alarm, or a `ring` from the **Test tone** button, stops talk.
- The mic only works on `https://` or `http://localhost`, a browser rule.

---

## 5. Alarms

Alarms **ring on the device**, from its own clock and time zone, so they work with the network or server down. The server only keeps the list and delivers it.

```json
{ "id": "uuid", "deviceId": "esp32-…", "time": "07:00", "days": [1,2,3,4,5],
  "tone": "wake.wav", "enabled": true }
```

| Field | Rule |
|---|---|
| `id` | Created by the server when missing |
| `time` | `HH:MM`, 24-hour, in the **device's** time zone (section 8); must match `^dd:dd$` |
| `days` | Weekdays, `0` = Sunday … `6` = Saturday; other numbers are dropped |
| `tone` | A file in the tone library. Only the file name is kept (paths are stripped). |
| `enabled` | Defaults to `true` |

**Delivery:**
```
page: alarm_save / alarm_delete → server saves alarms.json
server → device: alarms_sync {version, alarms}      (on connect + after each change)
device: saves the list in flash → alarms_ack {version, count}
```
- **`version`** is a hash of the device's list. An unchanged list keeps its version, so the device doesn't re-save it on every reconnect. The page compares `alarms_ack.version` with `alarms.versions[deviceId]` to show "saved on the device".
- **Storage:** server in `backend/alarms.json` (or `DATA_DIR`); device in its settings storage, up to 20 alarms. Both survive restarts.

**Ringing (on the device):**
- It checks the list once per minute, right as the minute starts, **only after its clock is set** (`timeValid`). So an alarm never rings at a wrong time, and never twice in the same minute.
- If several alarms are due in the same minute, the first one wins.
- It plays the tone **from its own storage** if stored, looping with no gap. Otherwise it streams it over HTTP, downloading it again on each loop. Either way it rings until `stop`, or for **60 s** at most.
- If the tone **can't be downloaded** (no network, server down, file missing, or 3 s timeout), it plays a **built-in beep** (880 Hz, 0.25 s on / off) instead of staying silent.
- **Stored tones:** `tones` in `alarms_sync` lists every tone the device's alarms use. The device:
  - downloads missing ones in the background, one small piece per loop, so audio and commands keep working;
  - writes each to a temporary file, checks size and **SHA-256**, then renames it to `/<first 8 hex of sha256>.wav`;
  - deletes stored tones no alarm uses any more;
  - retries a failed download 30 s later.

  Because files are named by their hash, a re-uploaded tone (new content) is a new file. Storage is the 9.9 MB `ffat` partition, formatted on first boot.
- **After a restart with no internet,** the clock can't be set (no battery-backed clock yet), so alarms wait until the internet is back.

---

## 6. Device reports

### 6.1 `stats` (every 2 s)

| Field | Meaning |
|---|---|
| `heapFree`, `heapTotal` | Internal RAM free / total (bytes) |
| `heapMin` | Lowest free RAM since boot. If this keeps falling, memory is leaking. |
| `heapMaxBlock` | Largest single free block |
| `psramTotal`, `psramFree` | PSRAM, `0` on boards without it |
| `appUsed`, `appTotal` | Program size / program space, measured once at boot |
| `nvsUsed`, `nvsTotal` | Settings storage, in entries |
| `flashSize` | Flash chip size in bytes |
| `cpuMhz` | Clock speed |
| `cpu` | `[core0 %, core1 %]`. Core 0 runs Wi-Fi; core 1 runs the sketch. |
| `uptime` | Seconds since boot |
| `rssi` | Wi-Fi signal in dBm |
| `fw` | Firmware version |
| `board` | Chip type (`esp32s3`, `esp32`): which firmware file fits |
| `time` | Device's local time `YYYY-MM-DD HH:MM:SS`, or `""` until the first internet time sync |
| `tz` | Device's time zone name |

### 6.2 `partitions`

```json
{ "type": "partitions", "flashSize": 16777216,
  "list": [ { "name": "app0", "kind": "app", "offset": 65536, "size": 3145728,
              "used": 1334751, "note": "running program" }, … ] }
```
- `kind`: `app` | `nvs` | `files` | `system` | `other`.
- `used` is only present when measured: the running app, NVS, and a formatted file storage (which also has `fsTotal`).
- The page adds a `bootloader` row for the space before the first partition.

---

## 7. HTTP: tones

- **Upload:** `POST /tones`
  - The file name goes in the `X-Filename` header, and the body is the file itself. It works with or without a `Content-Type` header.
  - The name is reduced to a file name, and characters outside `[A-Za-z0-9_.-]` become `_`. It must end in `.wav`.
  - Limit is 20 MB. The response is `200`, or `400` for a bad name or an empty body.
  - On success, all browsers get a `tones` message.
- **Format:** the page converts every upload to **16 kHz, 16-bit, mono WAV with a 44-byte header**, first 30 s only. The device skips exactly 44 bytes, so other WAV layouts will play as noise.
- **Download:** `GET /tones/<name>.wav` includes `Content-Length`. The device uses it to know where the file ends.

---

## 8. Time

- **Clock:** the device gets the time from NTP (`pool.ntp.org`, `time.google.com`) after Wi-Fi connects, and ESP-IDF resyncs it every hour. Until the first sync, `time` in `stats` is empty and the clock must not be trusted.
- **Time zone:** the server keeps one IANA zone per device in `devices.json` (in `DATA_DIR`). If none is set, it uses `TZ_DEFAULT`, or the server machine's own zone.
- **POSIX rule:** the ESP32 needs the zone as a POSIX rule, built from the zone's current UTC offset. POSIX counts hours *west* of UTC, so the sign flips: `Asia/Dhaka` → `<+06>-6`, `Asia/Kolkata` → `<+0530>-5:30`, `America/Argentina/Buenos_Aires` → `<-03>3`, UTC → `UTC0`.
- **Daylight saving:** the rule has no DST dates in it. The server re-sends every hour, so a device is at most an hour late switching, and only if it's online.
- The device saves the last zone it received, so it keeps the right local time after a restart even before the server connects.

---

## 9. Firmware updates over Wi-Fi

- **Upload:** `POST /firmware` with `firmware/build/<profile>/firmware.ino.bin` as the body (made by `./fw.sh build`). The server checks:
  - the first byte is `0xE9`, the ESP32 program-image byte;
  - the marker `SSFW:<board>:<version>:END` is in the file. The firmware carries it (`ota.cpp`), so the board and version come from the file itself;
  - the file is at most 3 MB, the program slot (the 16 MB `merged.bin` is refused).

  It keeps only the newest file per board, in `firmware/<board>.bin` + `.json` under `DATA_DIR`.
- **Update:**
  ```
  page: ota {target} → server: ota_start {path, version, size, sha256} → device
  device: downloads into the spare program slot, checks SHA-256, the update library checks the image,
          marks it to boot, restarts → reports ota progress throughout
  ```
  Audio stops during the update. The device does nothing else for the few seconds it takes.
- **Rollback:** the new version starts **on probation**. It becomes permanent once it connects to the server (`ota` `done`). If it doesn't within **3 minutes**, or it crashes and restarts, the bootloader starts the previous version, which reports `rolled_back` once on its next connect.
- **Not yet:** signed firmware. It needs secure boot, which comes with the factory setup (roadmap 5.4).
