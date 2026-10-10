// Template for secrets.h, which holds the MQTT broker login and is NOT in git.
// Copy this file to secrets.h (same folder) and fill in the real values.
// Without secrets.h the firmware still builds (CI does), using these
// placeholders, but it can't connect.
#pragma once

#define MQTT_HOST     "your-deployment.ala.asia-southeast1.emqxsl.com"  // EMQX: Deployment Overview → Address
#define MQTT_PORT     8883                                               // MQTT over TLS
#define MQTT_USERNAME "device"                                           // EMQX: Access Control → Authentication
#define MQTT_PASSWORD "change-me"
