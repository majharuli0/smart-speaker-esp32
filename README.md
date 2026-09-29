# ESP32 LED Demo: browser → local server → device

Minimal starting point: the ESP32 joins Wi-Fi, finds the local server on its own, and shows up in a browser page listing every online device.

```
led-demo/
├── backend/server.js     Express + WebSocket relay, answers mDNS for led-server.local
├── frontend/index.html   Device list with Blink / Reset Wi-Fi buttons
└── esp32/esp32.ino       Wi-Fi setup, server discovery, command handling
```

The earlier sound/flash-storage version is in git history (commit `340a074`), along with `docs/audio-alarm-design.md` and `esp32/format_flash/`.

## 1. Start the server

```bash
cd backend
npm install
node server.js
```

If Windows Firewall asks, allow it (TCP 3000 and UDP 5353 for mDNS). Open `http://localhost:3000`.

## 2. Flash the ESP32 (Arduino IDE)

- Board: **ESP32 Dev Module** (esp32 core by Espressif)
- Libraries: **WiFiManager** (tzapu), **WebSockets** (Markus Sattler), **ArduinoJson** (v7)
- Upload `esp32/esp32.ino`, Serial Monitor at 115200

## 3. Connect it to Wi-Fi (first boot only)

1. On your phone, join the hotspot **`LED-Setup-xxxx`** (last 4 characters of the device ID).
2. The setup page opens (or go to `192.168.4.1`). Pick your Wi-Fi, enter the password, save.
3. The device joins your network, finds `led-server.local`, and appears on the web page.

Wi-Fi is saved on the device. The hotspot comes back automatically if the saved network is unreachable, or when you click **Reset Wi-Fi** on the page.

## Alarms

1. Under **Tones**, upload any audio file. The browser converts it to 16 kHz mono WAV (first 30 s) before uploading, so the ESP32 needs no decoder.
2. On a device card, pick a time, weekdays and a tone, then click **Add alarm**. **Test tone** plays it right away.
3. At that time the server sends `ring` to the device. The device streams the tone from `http://<server>:3000/tones/<name>` and loops it until **Stop** is pressed, or for 1 minute at most.

The server holds the schedule (`backend/alarms.json`), so it must be running when an alarm is due. Each alarm stores the browser's timezone, so it rings at the right local time even if the server runs on UTC.

## Hold to talk

Press and hold **Hold to talk** on a device card and speak. Your voice plays on the device's speaker about 0.3 s later.

- **Mic access:** browsers allow the microphone only on `https://` or `http://localhost`. Use `http://localhost:3000` on the laptop running the server. From another machine on your network, the mic is blocked until the server has HTTPS.
- **Format:** the page sends 16 kHz 16-bit mono PCM in 20 ms binary WebSocket messages. The device buffers 100 ms before playing, to ride out Wi-Fi hiccups.
- **Priority:** an alarm going off interrupts talk.
- **Browser:** use Chrome or Edge. Firefox can't capture the mic into a 16 kHz audio context.

Amp wiring (MAX98357A). The sketch picks the pins from the board you compile for:

| Amp pin | ESP32-S3-WROOM-1 N16R8 | Original ESP32 |
|---|---|---|
| DIN | GPIO 4 | GPIO 33 |
| BCLK | GPIO 5 | GPIO 25 |
| LRC | GPIO 6 | GPIO 32 |
| VIN | 5V | 5V |
| GND | GND | GND |

ESP32-S3 Arduino settings: Board **ESP32S3 Dev Module**, Flash Size **16MB**, PSRAM **OPI PSRAM**, Partition Scheme **16M Flash (3MB APP/9.9MB FATFS)**, upload through the USB-C port labelled **COM**.

## Protocol

All messages are JSON over one WebSocket.

| From → To | Message |
|---|---|
| device → server | `{type:"hello", role:"device", deviceId}` |
| browser → server | `{type:"hello", role:"browser"}` → replies with `devices`, `alarms`, `tones` |
| server → browsers | `{type:"devices"\|"alarms"\|"tones", ...}` whenever one changes |
| browser → server | `{type:"alarm_save", alarm:{id?, deviceId, time:"07:00", days:[0-6], tone, tz, enabled}}`, `{type:"alarm_delete", id}` |
| server → device | `{type:"ring", tone}` when an alarm is due |
| server → browsers | `{type:"alarm_fired", alarmId, deviceId, time, delivered}` |
| browser → device | any other `{type, target, ...}` relayed as-is: `blink`, `ring` (test), `stop`, `reset_wifi` |
| browser → device | `{type:"talk_start", target}`, then **binary** PCM chunks, then `{type:"talk_stop", target}`. One talker per device; otherwise `{type:"talk_denied", reason:"busy"\|"offline"}` |
| browser → device | `{type:"volume", target, value:0-100}` sets and saves it on the device; without `value` it only asks |
| device → browsers | anything, relayed with `deviceId` added: `ringing`, `stopped`, `talking`, `talk_stopped`, `volume`, and every 2 s `stats` (RAM, PSRAM, app flash, settings storage, per-core CPU %, uptime, Wi-Fi signal) |

Commands other than the alarm messages are relayed without the server knowing what they mean, so adding one means changing only the frontend and the sketch.
