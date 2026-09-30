// Real time from the internet (NTP), in the device's time zone.
#pragma once
#include <Arduino.h>

void timeSetup();  // after Wi-Fi: apply the saved time zone and start NTP (resyncs hourly)
void timeLoop();   // retries the first sync if Wi-Fi came up late (e.g. router still booting)
void setTimezone(const char *posix, const char *name);  // e.g. "<+06>-6", "Asia/Dhaka"; saved
bool timeValid();  // false until the first NTP sync, so nothing acts on a wrong clock
String timeNow();  // local time "2026-10-01 14:05:03", or "" if not valid yet
String timezoneName();
