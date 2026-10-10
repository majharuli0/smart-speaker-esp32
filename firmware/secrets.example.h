// Template for secrets.h, which holds the MQTT broker login and the device secret, and is NOT in git.
// Copy this file to secrets.h (same folder) and fill in the real values.
// Without secrets.h the firmware still builds (CI does), using these
// placeholders, but it can't connect.
#pragma once

#define MQTT_HOST     "your-deployment.ala.asia-southeast1.emqxsl.com"  // EMQX: Deployment Overview → Address
#define MQTT_PORT     8883                                               // MQTT over TLS
#define MQTT_USERNAME "device"                                           // EMQX: Access Control → Authentication
#define MQTT_PASSWORD "change-me"

// Same as DEVICE_SECRET in the backend's .env. Each device's QR label code is
// HMAC-SHA256(DEVICE_SECRET, device ID): it unlocks Bluetooth Wi-Fi setup and
// proves ownership when the device is added to an account.
#define DEVICE_SECRET "change-me-as-well"
