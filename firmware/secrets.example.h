// Template for secrets.h, which holds the broker address and the device secret, and is NOT in git.
// Copy this file to secrets.h (same folder) and fill in the real values.
// Without secrets.h the firmware still builds (CI does), using these
// placeholders, but it can't connect.
#pragma once

// Our MQTT broker (EMQX in smart-speaker-backend's docker-compose.yml). On your
// own network: this computer's address. On the cloud server: "mqtts://<domain>:8883".
// Each device logs in with its own ID and a password derived from DEVICE_SECRET.
#define MQTT_URI "mqtt://192.168.0.10:1883"

// Same as DEVICE_SECRET in the backend's .env. Each device's QR label code is
// HMAC-SHA256(DEVICE_SECRET, device ID): it unlocks Bluetooth Wi-Fi setup and
// proves ownership when the device is added to an account.
#define DEVICE_SECRET "change-me-as-well"
