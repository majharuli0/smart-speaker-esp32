// Real time from the internet (NTP), in the device's time zone. A DS3231 clock
// module (if wired) keeps it through power cuts, so alarms ring without internet.
#pragma once
#include <Arduino.h>

void timeFromRtc();  // first thing at boot: set the clock from the DS3231, if it has a good time
void timeSetup();  // after Wi-Fi: apply the saved time zone and start NTP (resyncs hourly)
void timeLoop();   // retries the first sync if Wi-Fi came up late; copies each NTP sync to the DS3231
void setTimezone(const char *posix, const char *name);  // e.g. "<+06>-6", "Asia/Dhaka"; saved
bool timeValid();  // false until the DS3231 or NTP set the clock, so nothing acts on a wrong clock
const char *timeSource();  // "rtc" (DS3231 at boot), "ntp" (internet) or "none" (not set)
const char *rtcState();    // the DS3231: "ok", "not_set" (waiting for NTP) or "missing"
String timeNow();  // local time "2026-10-01 14:05:03", or "" if not valid yet
String timezoneName();
