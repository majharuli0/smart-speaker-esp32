#include "timesync.h"
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>
#include "config.h"

static Preferences prefs;
static String tzPosix, tzName;

void timeSetup() {
  prefs.begin("time");
  tzPosix = prefs.getString("tz", DEFAULT_TZ_POSIX);
  tzName = prefs.getString("tzname", DEFAULT_TZ_NAME);
  // Starts SNTP; ESP-IDF then resyncs by itself every hour
  configTzTime(tzPosix.c_str(), NTP_SERVER_1, NTP_SERVER_2);
  Serial.println("Time zone: " + tzName + " (" + tzPosix + "), waiting for NTP...");
}

void setTimezone(const char *posix, const char *name) {
  if (!posix || !*posix || (tzPosix == posix && tzName == name)) return;
  tzPosix = posix;
  tzName = name;
  prefs.putString("tz", tzPosix);
  prefs.putString("tzname", tzName);
  setenv("TZ", posix, 1);
  tzset();
  Serial.println("Time zone set: " + tzName + " (" + tzPosix + ")");
}

void timeLoop() {
  static unsigned long lastTry = 0;
  if (timeValid() || WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastTry < 30000) return;
  lastTry = millis();
  configTzTime(tzPosix.c_str(), NTP_SERVER_1, NTP_SERVER_2);
}

bool timeValid() {
  return time(nullptr) > 1704067200;  // after 2024-01-01: the clock has been set by NTP
}

String timeNow() {
  if (!timeValid()) return "";
  time_t now = time(nullptr);
  struct tm local;
  localtime_r(&now, &local);
  char buf[20];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
  return buf;
}

String timezoneName() { return tzName; }
