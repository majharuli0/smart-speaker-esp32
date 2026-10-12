#!/usr/bin/env bash
# Builds one speaker, ready to box: one command per unit (Git Bash on Windows).
#
#   tools/make-unit.sh COM11          wipe the chip, flash the firmware, make the label
#   tools/make-unit.sh COM11 --keep   flash without wiping (keeps Wi-Fi, settings and files)
#
# Steps: read the speaker ID from the chip → build the firmware (cloud address and
# secret from firmware/secrets.h) → wipe → flash → label (QR + ID) opened for
# printing → a line in tools/units.csv. On first start the speaker waits for
# Bluetooth setup (blue light) and downloads the built-in tones from the server.
set -e
cd "$(dirname "$0")/.."
PORT=${1:?give the port, e.g. tools/make-unit.sh COM11}
KEEP=$2
BACKEND=../smart-speaker-backend
CLI=arduino-cli
command -v "$CLI" >/dev/null || CLI="/c/Program Files/Arduino CLI/arduino-cli.exe"
ESPTOOL=$(ls -d ~/AppData/Local/Arduino15/packages/esp32/tools/esptool_py/*/esptool* 2>/dev/null | tail -1)
[ -x "$ESPTOOL" ] || { echo "esptool not found: build the firmware once with firmware/fw.sh build"; exit 1; }

# The firmware must point at the cloud, and share the label secret with the backend
secret() { sed -n "s/^$1[ =]*\"\{0,1\}\([^\"]*\)\"\{0,1\}.*/\1/p" "$2" | head -1 | tr -d '\r'; }
grep -q 'MQTT_URI "mqtts://' firmware/secrets.h || { echo "firmware/secrets.h: MQTT_URI should be the cloud (mqtts://...)"; exit 1; }
[ "$(secret '#define DEVICE_SECRET' firmware/secrets.h)" = "$(secret DEVICE_SECRET $BACKEND/.env)" ] ||
  { echo "DEVICE_SECRET differs between firmware/secrets.h and $BACKEND/.env: labels wouldn't match"; exit 1; }

echo "1/5 Reading the speaker ID…"
UID_=$("$ESPTOOL" --port "$PORT" read-mac | sed -n 's/^MAC: *//p' | head -1 | tr -d ':' | tr a-f A-F)
[[ $UID_ =~ ^[0-9A-F]{12}$ ]] || { echo "Couldn't read the chip on $PORT"; exit 1; }
echo "    Speaker ID $UID_"

echo "2/5 Building the firmware…"
firmware/fw.sh build >/dev/null

if [ "$KEEP" != "--keep" ]; then
  echo "3/5 Wiping the chip (a fresh unit: no Wi-Fi, no settings, no files)…"
  "$ESPTOOL" --port "$PORT" erase-flash >/dev/null
else
  echo "3/5 Keeping what's on the chip (--keep)"
fi

echo "4/5 Flashing…"
"$CLI" upload --profile s3 --port "$PORT" --input-dir firmware/build/s3 firmware >/dev/null

echo "5/5 Making the label…"
LABELS="$(pwd -W 2>/dev/null || pwd)/tools/labels"
LABEL=$(cd "$BACKEND" && npm run -s label -- "$UID_" "$LABELS")
VERSION=$(sed -n 's/#define FW_VERSION "\(.*\)"/\1/p' firmware/config.h | tr -d '\r')
[ -f tools/units.csv ] || echo "uid,firmware,made" > tools/units.csv
echo "$UID_,$VERSION,$(date '+%Y-%m-%d %H:%M')" >> tools/units.csv
command -v cmd >/dev/null && cmd //c start "" "$LABEL" || true

echo
echo "Done: speaker $UID_ (firmware $VERSION)"
echo "Label: $LABEL"
echo "To add it: scan the QR in the app, or type $(echo "$UID_" | tail -c 5)."
