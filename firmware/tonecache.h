// Tones stored on the device, so alarms play the real tone with no network.
// The server lists the tones the device's alarms use (with size + SHA-256);
// this module downloads missing ones in the background, checks them, and
// deletes ones no alarm uses any more.
//
// Two places hold tones: the microSD card (big, removable) and the built-in
// 9.9 MB "ffat" area (small, always there). Each tone is kept in both when it
// fits, so an alarm still has its tone if the card is pulled out.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>

void cacheSetup();                      // mount built-in storage (formats it the first time) and the card
void cacheSync(JsonVariantConst tones); // the wanted list, from alarms_sync
void cacheLoop();                       // card in/out checks + downloads, a small piece per call
bool cacheOpen(const String &tone, File &file);  // open the stored copy (card first), false if none
void cacheCheckNow();                   // a read failed mid-play: check the card on the next loop
bool cacheReady();                      // built-in storage mounted
void cardInfo(JsonObject o);            // SD card details for the page's storage table
void sendCacheStatus();                 // {type:"cache", stored, wanted, card} for the page
