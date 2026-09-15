# Smart Speaker (ESP32) — browser → server → device

**Project vision:** a Wi-Fi-connected smart speaker built on the ESP32 — pairable from a browser, with its own local sound library (upload once, plays without needing the network at playback time) and a speaker loud enough to be useful around a room. Everything below — Wi-Fi provisioning, pairing/ownership, the LED, the I2S amp, the flash-backed sound storage — is the hardware/protocol foundation that vision is built on, developed and verified one working piece at a time rather than all at once.

The **LED flash** was the very first milestone (proving the browser → server → device round-trip worked at all, before any audio hardware existed) — it still exists as a quick sanity check ("is the device even alive and connected?") and costs nothing to keep, but it's no longer what this project is about.

Everything runs on your LAN: laptop server + ESP32 on the same Wi-Fi. Nothing is hardcoded in the ESP32 sketch — no SSID, no password, no server IP.

- **Wi-Fi**: the ESP32 gets it from you, once, via its own setup portal (details below). Saved to flash after that.
- **Server address**: the ESP32 finds the server automatically via mDNS (`led-server.local`), so it keeps working even if the laptop's IP changes.

## Project structure

```
led-demo/              (folder name is legacy from the original LED proof-of-concept — see note above)
├── backend/           Node/Express server: WebSocket relay, pairing, sound upload + manifest
│   ├── server.js
│   ├── claims.json     generated at runtime — pairing data, not source (gitignored)
│   ├── sounds.json      generated at runtime — sound manifest, not source (gitignored)
│   └── sounds/           generated at runtime — uploaded audio files (gitignored)
├── frontend/          Static browser page served by the backend (pairing, device controls, Sounds panel)
├── esp32/
│   ├── esp32.ino        main firmware: Wi-Fi, WebSocket client, LED, I2S speaker, flash storage
│   └── format_flash/    one-time sketch to format a fresh flash chip (run once, see Milestone 6)
└── docs/
    └── audio-alarm-design.md   original design doc the sound-storage feature grew out of
```

## 1. Start the server

```bash
cd led-demo/backend
node server.js
```

Leave it running. It prints:
```
Advertising as led-server.local via mDNS
Server on http://0.0.0.0:3000
```
If Windows Firewall prompts, **allow** access — both for port 3000 and for mDNS (UDP 5353) — otherwise the ESP32 can't reach or find it.

Already verified in this session: the server starts, advertises `led-server.local`, serves the frontend page, and correctly relays a `{"type":"flash"}` message between WebSocket clients.

## 2. Open the browser page

On this laptop or any device on the same network, visit `http://<laptop-ip>:3000` (or `http://led-server.local:3000` if your OS also resolves mDNS — Windows/Android may not without extra software, but the ESP32 doesn't need that, it queries mDNS directly).

You should see "connected". The **Device** dropdown only shows devices *this browser has paired* (see Pairing below) — not every ESP32 that's ever connected to the server. Pick one, click **Flash LED** — the server terminal should print `Received: { type: 'flash', target: '...' }`.

## 3. Flash the ESP32 (Arduino IDE)

1. **Install ESP32 board support** (skip if already installed): File → Preferences → Additional Board Manager URLs → add
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   Then Tools → Board → Boards Manager → search "esp32" → install.
2. **Install libraries** via Tools → Manage Libraries:
   - **"WebSockets"** by Markus Sattler
   - **"WiFiManager"** by tzapu
   - **"ArduinoJson"** by Benoit Blanchon
   - **"Adafruit SPIFlash"** by Adafruit
   - **"SdFat - Adafruit Fork"** by Adafruit (installs alongside Adafruit SPIFlash as a dependency, but confirm it's there)
   (`ESPmDNS` and `HTTPClient` are bundled with the ESP32 core, no separate install needed.)
3. Open `esp32/esp32.ino` in Arduino IDE.
4. Board: **Tools → Board → ESP32 Arduino → ESP32 Dev Module**.
5. Port: **Tools → Port →** select your board's COM port.
6. Click **Upload**, then open **Tools → Serial Monitor** at **115200** baud.

### First-time Wi-Fi setup (no code editing needed)

On first boot, the ESP32 can't connect to anything yet, so it opens its own access point instead of trying a hardcoded network:

1. On your phone (it must be a device with Wi-Fi — a desktop with only Ethernet can't reach the ESP32's own access point), connect to the Wi-Fi network **`LED-Demo-Setup`**.
2. A configuration page should open automatically (captive portal). If it doesn't, open a browser and go to `192.168.4.1`.
3. The page shows this board's **Device ID** (e.g. `esp32-AABBCCDDEEFF`) — note it down, you'll paste it into the frontend in a moment to pair it.
4. Tap **"Configure WiFi"**, pick your home network from the list, enter its password, and save.
5. The ESP32 connects to your real Wi-Fi and remembers it in flash — you won't see this portal again unless it can't reconnect (e.g. you change routers), or you deliberately reset it (see below).

### Pairing a device to the frontend

A device connecting to the server doesn't automatically show up for every browser — the server only shows a browser the devices *it* has paired, so unrelated users on the same server can't see or control each other's boards.

1. Open the frontend page. It generates a random ID for your browser on first load (stored in `localStorage`, so it persists across reloads).
2. Paste the **Device ID** from the setup portal (step 3 above) into the "Pair a device" field and click **Pair**.
3. It should show "Paired esp32-...", and the device now appears in the dropdown once it's online.

Pairing is first-come-first-served, not real authentication — anyone who has the 12-character ID can pair it. That's enough to stop random strangers from seeing/controlling your device on a shared server, but isn't login-level security. If that ever matters (e.g. a real multi-user product), the next step up is proper accounts.

### Switching Wi-Fi networks later

A plain reboot/power-cycle reuses the saved network — that's intentional, a real device shouldn't ask for Wi-Fi on every restart. To deliberately forget it and reconfigure, either:

- Click **"Reconfigure Wi-Fi"** in the browser page (only works while the device is already online and reachable), or
- Hold the board's **BOOT** button for **10 seconds** while it's running — works even if it's stuck on a network it can't use.

Either way, it wipes the saved credentials and reboots straight into the `LED-Demo-Setup` portal from step above.

### Verify, in order

- **Milestone 1 — Wi-Fi + server connection.** Serial Monitor should print:
  ```
  WiFi ready, IP: 192.168.x.x
  Looking for led-server.local on the network...
  Found server at: 192.168.x.x
  Connected to server
  ```
  and the `node server.js` terminal should print `Client connected`. If `led-server.local` never resolves, double-check the server is running and mDNS/UDP 5353 isn't blocked by the firewall.

- **Milestone 2 — browser reaches server.** Already verified above (step 2).

- **Milestone 3 — full relay.** With the ESP32 connected and the browser page open, click **Flash LED**. Serial Monitor should print:
  ```
  Received: {"type":"flash"}
  Received FLASH
  ```

- **Milestone 4 — the LED itself.** The onboard LED on GPIO2 should blink for ~300ms per click. Wrong pin for your board variant → update `LED_GPIO` in `esp32.ino`.

- **Milestone 5 — the speaker.** With a MAX98357A I2S amp wired as below, click **Play Sound**. Serial Monitor should print `Received: {"type":"play_sound"...}` then `Playing test tone`, and the speaker should emit a clean ~0.5s 1kHz beep. No sound but no errors either → double check the three signal wires aren't swapped (BCLK/LRC swapped is the usual culprit, and sounds like silence or garbled static rather than an obvious "wrong pitch").

  | Amp pin | ESP32 GPIO |
  |---|---|
  | Vin | 3V3 |
  | GND | GND |
  | DIN | 33 |
  | BCLK | 25 |
  | LRC | 32 |
  | GAIN, SD | not connected (default gain, amp enabled) |

  This is a hardcoded, synthesized tone — no audio file or flash storage involved yet. It exists purely to prove the amp/speaker/I2S wiring works before real sound files are layered on.

- **Milestone 6 — flash storage + real sound files.** A W25Q64JV SPI NOR flash chip (8MB) holds uploaded `.wav` files persistently, formatted as a FAT filesystem so it can be written/read like a tiny USB drive.

  | Flash pin | ESP32 GPIO |
  |---|---|
  | VCC | 3V3 (**3.3V only** — 5V destroys this chip) |
  | GND | GND |
  | CLK | 18 |
  | DI (MOSI) | 23 |
  | DO (MISO) | 19 |
  | CS | 5 |

  These are the ESP32's standard hardware VSPI pins, so no custom `SPI.begin()` call is needed — only chip-select is board-specific.

  **One-time step for a fresh chip:** a blank (or differently-formatted) W25Q64JV won't mount. Serial Monitor will print `Error mounting FAT filesystem on flash — it may need formatting once`. Upload [`esp32/format_flash/format_flash.ino`](esp32/format_flash/format_flash.ino) once to format it (same board/port as `esp32.ino`; open Serial Monitor at 115200 baud and type `OK` when prompted — **this erases the chip**), then switch back to `esp32.ino` and re-upload. This is a one-time step per physical chip, not something that happens on every boot.

  **Required library patch:** Adafruit_SPIFlash has a [known bug on ESP32](https://github.com/adafruit/Adafruit_SPIFlash/issues/120) — `Adafruit_SPIFlashBase::begin()` unconditionally force-casts the transport to `Adafruit_FlashTransport_ESP32*` (meant only for the ESP32's own *internal* flash) even when using `Adafruit_FlashTransport_SPI` (an external chip, our case), crashing with a `Guru Meditation Error: LoadProhibited` panic. Both `esp32.ino` and `format_flash.ino` need this fixed in the **installed library itself**: open `Adafruit_SPIFlashBase.cpp` (Arduino's libraries folder → `Adafruit_SPIFlash/src/`) and remove `defined(ARDUINO_ARCH_ESP32) ||` from the `#if` guarding that fast-path, so ESP32 falls through to the same generic JEDEC-ID auto-detect path other platforms already use (it already recognizes `W25Q64JV_IQ`). This patch lives outside this project folder — reapply it if the library is ever reinstalled/updated via Library Manager.

  Once mounted, use the **Sounds** panel in the frontend: choose a `.wav` file and click **Upload**. The status badge next to it moves `pending` → `ready` once the ESP32 has downloaded and verified it (or `failed` with a reason if something went wrong). Click **Play** on a `ready` sound to hear it through the speaker.

  **Uploaded files must be 16-bit PCM mono WAV at 16000 Hz.** Playback doesn't parse the WAV header's format fields or resample — it assumes a standard 44-byte header and streams the raw bytes straight to an I2S peripheral fixed at 16kHz/16-bit (`I2S_SAMPLE_RATE` in `esp32.ino`). A file exported at a different rate (44.1kHz is a common default) will still "play" without any error, just sped up and pitched wrong. In Audacity: Tracks → Resample → 16000 Hz, Tracks → Mix → Mix Stereo Down to Mono if needed, then File → Export Audio → WAV, Signed 16-bit PCM.

## How device targeting and pairing work

The server tracks which connected WebSocket clients are ESP32s vs browsers, and which browser owns which device:
- On connect, each ESP32 sends `{"type":"hello","role":"device","deviceId":"esp32-<mac>"}`.
- Each browser sends `{"type":"hello","role":"browser","clientId":"<random id from localStorage>"}`.
- Pairing (`{"type":"claim_device", deviceId, clientId}`) records `deviceId -> clientId` in `backend/claims.json`, first-claim-wins.
- A browser's device list (`{"type":"devices","devices":[...]}`) only includes devices it owns *and* that are currently online; pushed automatically whenever a device connects/disconnects or a new claim is made.
- Commands (`flash`, `reset_wifi`, `play_sound`, `stop_sound`, `delete_sound`) carry a `target` deviceId, and the server only relays them if the sending browser's `clientId` owns that device.

### Sounds: upload, storage, and playback

Extends the same pairing/ownership model rather than replacing it — uploads and playback both still go through the paired backend server, never directly browser-to-ESP32 (kept deliberately consistent with everything above, including working the same way over a remote/NAT'd connection as it does on your LAN):

1. Browser uploads raw file bytes to `POST /upload/<deviceId>?clientId=<clientId>` (ownership-checked the same way WS commands are). The server stores the file, assigns it a short random `id`, and adds a `pending` entry to `backend/sounds.json`.
2. Server tells the device to fetch it: `{"type":"sync_file", id, url, expectedSize}` over the existing WebSocket.
3. The ESP32 downloads it via a plain HTTP GET to that `url`, writes it to a temp file on flash, verifies the byte count, then atomically renames it into place — a half-downloaded file can never be mistaken for a valid one, even across a power loss mid-download.
4. The device reports the outcome (`sync_result {id, ok, reason?}`), and separately sends a full `fs_report {files:[{id,size}]}` after every mount/sync/delete — the **device** is always the source of truth for what's actually on its flash chip; the server's manifest is just a cache of what it last heard and gets overwritten accordingly, including marking a sound `missing` if the device no longer has it.
5. Browser requests `play_sound {target, id}` / `delete_sound {target, id}`; the server relays them the same ownership-checked way as `flash`/`reset_wifi`.

## Notes / next steps

- `flashLed()` blocks the WebSocket loop for 300ms — fine for this demo; move real work off-loop if you add more commands later.
- Sound playback (`playWavFromFlash`) is similarly blocking for the duration of the clip, which also means **`stop_sound` can't interrupt playback that's already started** — it's relayed end-to-end but the device just logs it as a no-op mid-stream today. Fixing this for real needs playback moved off the main loop (e.g. a FreeRTOS task) so the WebSocket keeps pumping while audio streams.
- `fs_report` is only sent on WebSocket (re)connect and after every sync/delete — there's no periodic background poll for a card/chip going bad or a file changing outside the system, unlike the periodic re-check described in `docs/audio-alarm-design.md`. Fine for a single demo device; worth adding if this becomes multi-device or long-running.
- Going public (hosting the server outside your LAN) forces `ws://` → `wss://`, which needs a TLS/cert setup on the ESP32 side. That's a separate, independent step from everything above — the JSON message contract doesn't change.
