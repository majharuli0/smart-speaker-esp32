#!/usr/bin/env bash
# Firmware helper (Git Bash / Linux / macOS). Board settings live in sketch.yaml.
#
#   ./fw.sh build              compile for the ESP32-S3 (file for web updates: build/s3/firmware.ino.bin)
#   ./fw.sh upload COM11       compile and upload
#   ./fw.sh monitor COM11      open the serial monitor (Ctrl+C to quit)
#   PROFILE=esp32 ./fw.sh build   original ESP32 board instead
set -e
cd "$(dirname "$0")"

CLI=arduino-cli
command -v "$CLI" >/dev/null || CLI="/c/Program Files/Arduino CLI/arduino-cli.exe"  # PATH not refreshed yet
PROFILE=${PROFILE:-s3}
PORT=$2

case "$1" in
  build)   "$CLI" compile --profile "$PROFILE" --output-dir "build/$PROFILE" . && echo "Firmware file: firmware/build/$PROFILE/firmware.ino.bin" ;;
  upload)  "$CLI" compile --profile "$PROFILE" --upload --port "${PORT:?give the port, e.g. COM11}" . ;;
  monitor) "$CLI" monitor --port "${PORT:?give the port, e.g. COM11}" --config baudrate=115200 ;;
  *)       sed -n '2,7p' "$0"; exit 1 ;;
esac
