// Alarms stored on the device and checked against its own clock, so they
// ring even when the network or server is down.
#pragma once
#include <ArduinoJson.h>

void alarmsSetup();                  // load the saved list from flash
void alarmsLoop();                   // once a minute (on the minute): ring any alarm that is due
void alarmsSync(JsonDocument &msg);  // new list from the server: save it, then confirm with alarms_ack
void alarmsSnooze();                 // stop ringing now, ring the same tone again in SNOOZE_MS
