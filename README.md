# ESP32 Smart Speaker: firmware

Firmware and product docs for an ESP32-S3 smart speaker: alarms with your own tones (they ring even
without internet), a doorbell, hold-to-talk, volume, device health, updates over Wi-Fi, and Wi-Fi setup
over Bluetooth. See [docs/ROADMAP.md](docs/ROADMAP.md) and [docs/APP_PLAN.md](docs/APP_PLAN.md).

The rest of the system lives in its own repos:

| Repo | What |
|---|---|
| **smart-speaker-esp32** (this one) | Firmware, hardware notes, product docs, the device protocol |
| [smart-speaker-backend](https://github.com/majharuli0/smart-speaker-backend) | NestJS API + Postgres, accounts, and the MQTT broker (EMQX) the speakers connect to |
| [smart-speaker-web](https://github.com/majharuli0/smart-speaker-web) | React web app |
| [smart-speaker-app](https://github.com/majharuli0/smart-speaker-app) | Expo (React Native) phone app: adds a speaker by its QR label and Bluetooth |

```
├── firmware/    ESP32 program: firmware.ino (startup + commands), net, audio, alarms, tonecache, ota, health, stats, config.h
├── hardware/    Schematics and enclosure (later)
└── docs/        Roadmap, app plan, protocol; archive/ holds the earlier design doc
```

## 1. Secrets

Copy `firmware/secrets.example.h` to `firmware/secrets.h` (not in git) and set:
- `MQTT_URI`: the broker. At home: `mqtt://speaker-server.local:1883`. The computer running the
  backend's `docker compose` answers to that name (mDNS), so its address can change. In the cloud:
  `mqtts://<domain>:8883`. Each speaker logs in as itself, and the backend checks it.
- `DEVICE_SECRET`: the same value as `DEVICE_SECRET` in the backend's `.env`. It produces each speaker's
  QR label code and its MQTT password.

## 2. Flash the ESP32

All board settings and library versions come from [firmware/sketch.yaml](firmware/sketch.yaml). Needs
[arduino-cli](https://arduino.github.io/arduino-cli/) (`winget install ArduinoSA.CLI`).

```bash
cd firmware
./fw.sh build                 # compile for the ESP32-S3 → build/s3/firmware.ino.bin
./fw.sh upload COM11          # compile + upload (use your port)
./fw.sh monitor COM11         # serial monitor at 115200, Ctrl+C to quit
```

**Over Wi-Fi:** upload `firmware/build/s3/firmware.ino.bin` on the web app's **Firmware** page (admins),
then press **Update** under the speaker's Health. If the new version can't reach the server within
3 minutes, the device goes back to the previous one by itself.

At boot the serial monitor prints the speaker's UID and its label text: `QR label: SS:<UID>:<code>`.

## 3. Wi-Fi setup (over Bluetooth)

A speaker with no Wi-Fi saved advertises over Bluetooth as **`SS-XXXX`** (last 4 characters of its UID)
and needs the code from its QR label.

- **Phone app:** *Add a speaker* → scan the label → pick your Wi-Fi → password. It's added to your account.
- **For testing, Espressif's "ESP BLE Provisioning" app:** scan the QR code the serial monitor shows (in
  the app's settings, clear the device name prefix "PROV_").

Bluetooth is switched off once Wi-Fi is set up. **Reset Wi-Fi**, or removing the speaker from your
account, brings it back to Bluetooth setup. Alarms keep working while it waits.

## Hardware

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

Arduino IDE settings, if not using `./fw.sh`: Board **ESP32S3 Dev Module**, Flash Size **16MB**, PSRAM
**OPI PSRAM**, Partition Scheme **16M Flash (3MB APP/9.9MB FATFS)**, library **ArduinoJson** (v7),
upload through the USB-C port labelled **COM**.

## Protocol

The MQTT topics and every device message are in [docs/protocol.md](docs/protocol.md).

The old single-file server (`backend/`) and page (`web/`) were replaced by the repos above; they're in
this repo's git history.
