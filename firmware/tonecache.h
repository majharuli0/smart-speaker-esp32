// Tones stored on the device, so alarms play the real tone with no network.
// The server lists the tones the device's alarms use (with size + SHA-256);
// this module downloads missing ones in the background, checks them, and
// deletes ones no alarm uses any more. S3 only (TONE_CACHE in config.h).
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

void cacheSetup();                     // mount the storage (formats it the very first time)
void cacheSync(JsonVariantConst tones); // the wanted list, from alarms_sync
void cacheLoop();                      // downloads a small piece per call
String cachedPath(const String &tone); // stored copy ("/xxxxxxxx.wav"), or "" if not stored
bool cacheReady();                     // storage mounted
void sendCacheStatus();                // {type:"cache", stored, wanted} for the page
