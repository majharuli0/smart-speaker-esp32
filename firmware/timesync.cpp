#include "timesync.h"
#include <Preferences.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>
#include "config.h"

static Preferences prefs;
static String tzPosix, tzName;
static const char *source = "none";      // where the clock came from: "rtc", "ntp" or "none"
static bool rtcFound = false;            // a DS3231 answered at boot
static bool rtcSet = false;              // …and holds a good time (read at boot, or written since)
static volatile bool ntpSynced = false;  // set by SNTP (network task), handled in timeLoop()
static volatile bool rtcStale = false;   // NTP just set the clock: copy it to the DS3231

// ---- DS3231 (I2C 0x68): keeps UTC through power cuts, on its coin battery ----
static const uint8_t RTC_ADDR = 0x68;
static const uint8_t RTC_CONTROL = 0x0E, RTC_STATUS = 0x0F;
static const uint8_t EOSC = 0x80;  // control: 1 = the clock stops when on battery
static const uint8_t OSF = 0x80;   // status: 1 = the clock stopped (new module, flat battery)

static uint8_t bcd(int v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }
static int unbcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

static bool rtcRead(uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(RTC_ADDR, n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static bool rtcWrite(uint8_t reg, const uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(reg);
  Wire.write(buf, n);
  return Wire.endTransmission() == 0;
}

// Days from 1970-01-01 to a date (H. Hinnant's days_from_civil): newlib has no timegm()
static int32_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  int yoe = y - era * 400;
  int doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// The clock as it is now, into the DS3231 (UTC, 24-hour), and mark it running
static void rtcSave() {
  time_t now = time(nullptr);
  struct tm u;
  gmtime_r(&now, &u);
  uint8_t r[7] = {bcd(u.tm_sec), bcd(u.tm_min), bcd(u.tm_hour), (uint8_t)(u.tm_wday + 1), bcd(u.tm_mday),
                  (uint8_t)(bcd(u.tm_mon + 1) | (u.tm_year >= 200 ? 0x80 : 0)),  // century bit: 2100+
                  bcd(u.tm_year % 100)};
  uint8_t control, status;
  if (!rtcWrite(0x00, r, 7) || !rtcRead(RTC_CONTROL, &control, 1) || !rtcRead(RTC_STATUS, &status, 1)) {
    Serial.println("Clock: couldn't write the DS3231");
    return;
  }
  if (control & EOSC) {
    control &= ~EOSC;
    rtcWrite(RTC_CONTROL, &control, 1);
  }
  if (status & OSF) {
    status &= ~OSF;
    rtcWrite(RTC_STATUS, &status, 1);
    Serial.println("Clock: DS3231 set from NTP");
  }
  rtcSet = true;
}

void timeFromRtc() {
  Wire.begin(RTC_SDA_GPIO, RTC_SCL_GPIO, 100000);
  uint8_t r[7], status;
  if (!rtcRead(0x00, r, 7) || !rtcRead(RTC_STATUS, &status, 1)) {
    Serial.println("Clock: no DS3231 found, waiting for NTP");
    return;
  }
  rtcFound = true;
  int sec = unbcd(r[0] & 0x7F), min = unbcd(r[1] & 0x7F), hour = unbcd(r[2] & 0x3F);
  int day = unbcd(r[4] & 0x3F), month = unbcd(r[5] & 0x1F);
  int year = 2000 + unbcd(r[6]) + (r[5] & 0x80 ? 100 : 0);
  bool twelveHour = r[2] & 0x40;  // never written by us: rewritten at the next NTP sync
  if ((status & OSF) || twelveHour || year < 2024 || month < 1 || month > 12 || day < 1 || day > 31 ||
      hour > 23 || min > 59 || sec > 59) {
    Serial.println("Clock: DS3231 not set yet (new, or its battery ran out), waiting for NTP");
    return;
  }
  struct timeval tv = {(time_t)daysFromCivil(year, month, day) * 86400 + hour * 3600 + min * 60 + sec, 0};
  settimeofday(&tv, nullptr);
  source = "rtc";
  rtcSet = true;
  Serial.printf("Clock from DS3231: %04d-%02d-%02d %02d:%02d:%02d UTC\n", year, month, day, hour, min, sec);
}

static void onNtpSync(struct timeval *) {
  ntpSynced = true;
  rtcStale = true;
}

void timeSetup() {
  prefs.begin("time");
  tzPosix = prefs.getString("tz", DEFAULT_TZ_POSIX);
  tzName = prefs.getString("tzname", DEFAULT_TZ_NAME);
  sntp_set_time_sync_notification_cb(onNtpSync);
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
  if (rtcStale) {  // after every NTP sync (hourly), so the DS3231 stays right
    rtcStale = false;
    source = "ntp";
    if (rtcFound) rtcSave();
  }
  static unsigned long lastTry = 0;
  if (ntpSynced || WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastTry < 30000) return;
  lastTry = millis();
  configTzTime(tzPosix.c_str(), NTP_SERVER_1, NTP_SERVER_2);
}

bool timeValid() {
  return time(nullptr) > 1704067200;  // after 2024-01-01: the clock has been set (DS3231 or NTP)
}

const char *timeSource() { return source; }

const char *rtcState() { return !rtcFound ? "missing" : rtcSet ? "ok" : "not_set"; }

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
