# ESP32 Smart Speaker

An ESP32-S3 speaker controlled from the browser: custom alarms, hold-to-talk, volume and device health. It joins Wi-Fi, finds the local server on its own, and shows up on the web page. See [docs/ROADMAP.md](docs/ROADMAP.md) for the plan to production.

```
├── firmware/               ESP32 program: firmware.ino (startup + commands), net, audio, stats, config.h
├── backend/server.js       Express + WebSocket relay, alarm scheduler, tones, mDNS for led-server.local
├── web/                    Web page (index.html) and mic processor (mic-worklet.js)
├── hardware/               Schematics and enclosure (later)
└── docs/                   Roadmap; archive/ holds the earlier design doc
```

The earlier SPI-flash sound storage and the `format_flash` sketch are in git history (commit `340a074`).

## 1. Start the server

```bash
cd backend
npm install
npm start        # or: npm run dev  (restarts automatically when server.js changes)
```

If Windows Firewall asks, allow it (TCP 3000 and UDP 5353 for mDNS). Open `http://localhost:3000`.

Optional settings (port, data folder, mDNS name) go in `backend/.env`. Copy [backend/.env.example](backend/.env.example) to start.

## 2. Flash the ESP32

**From the command line (recommended).** All board settings and library versions come from [firmware/sketch.yaml](firmware/sketch.yaml), so there are no IDE menus to get wrong. Needs [arduino-cli](https://arduino.github.io/arduino-cli/) (`winget install ArduinoSA.CLI`).

```bash
cd firmware
./fw.sh build                 # compile for the ESP32-S3
./fw.sh upload COM11          # compile + upload (use your port)
./fw.sh monitor COM11         # serial monitor at 115200, Ctrl+C to quit
```

**Over Wi-Fi (once a board runs firmware 0.6.0 or later):** run `./fw.sh build`, upload `firmware/build/s3/firmware.ino.bin` in the page's **Firmware** section, then press **Update** on the device. If the new version can't reach the server within 3 minutes, the device goes back to the previous one by itself.

**From the Arduino IDE:**

- Board: **ESP32S3 Dev Module** (esp32 core by Espressif). Board settings are under "Amp wiring" below.
- Libraries: **WiFiManager** (tzapu), **WebSockets** (Markus Sattler), **ArduinoJson** (v7)
- Open `firmware/firmware.ino`, upload, Serial Monitor at 115200

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

Board: **ESP32-S3-WROOM-1 N16R8** (16 MB flash, 8 MB PSRAM). The firmware only builds for this board.

Amp wiring (MAX98357A; GAIN and SD unconnected):

| Amp pin | ESP32-S3 |
|---|---|
| DIN | GPIO 4 |
| BCLK | GPIO 5 |
| LRC | GPIO 6 |
| VIN | 5V |
| GND | GND |

microSD card module (SPI, powered from **3V3** unless the module has its own regulator; card formatted FAT32):

| SD module pin | ESP32-S3 |
|---|---|
| CS | GPIO 10 |
| MOSI | GPIO 11 |
| SCK | GPIO 12 |
| MISO | GPIO 13 |
| VCC | 3V3 |
| GND | GND |

Arduino IDE settings (`./fw.sh` sets these for you): Board **ESP32S3 Dev Module**, Flash Size **16MB**, PSRAM **OPI PSRAM**, Partition Scheme **16M Flash (3MB APP/9.9MB FATFS)**, upload through the USB-C port labelled **COM**.

## Protocol

Every message between the device, server and web page is documented in [docs/protocol.md](docs/protocol.md).

The server passes most browser commands straight to the device without knowing what they mean, so adding a new command usually means changing only the web page and the firmware.
