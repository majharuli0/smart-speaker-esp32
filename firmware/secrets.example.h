// Template for secrets.h, which holds the broker address and the device secret, and is NOT in git.
// Copy this file to secrets.h (same folder) and fill in the real values.
// Without secrets.h the firmware still builds (CI does), using these
// placeholders, but it can't connect.
#pragma once

// Our MQTT broker (EMQX). Exactly one MQTT_URI, so keep the others commented out.
// Each device logs in with its own ID and a password derived from DEVICE_SECRET.
//   - the cloud server (what the speakers use day to day):
//       #define MQTT_URI "mqtts://smart-speaker.duckdns.org:8883"
//   - local testing, smart-speaker-backend's docker compose on this PC: its LAN
//     address (ipconfig, "IPv4 Address"; it was 192.168.0.104 on 2026-10-18):
//       #define MQTT_URI "mqtt://192.168.0.104:1883"
//   - or the name the local backend answers to (mDNS), if the address keeps changing:
#define MQTT_URI "mqtt://speaker-server.local:1883"

// Same as DEVICE_SECRET in the backend's .env (the local one and the cloud server's).
// Never put the real one in git: this repo is public, and with it anyone could log in
// as any speaker or work out its label code. Keep a copy in a password manager.
// Each device's QR label code is HMAC-SHA256(DEVICE_SECRET, device ID): it unlocks
// Bluetooth Wi-Fi setup and proves ownership when the device is added to an account.
#define DEVICE_SECRET "change-me-as-well"
