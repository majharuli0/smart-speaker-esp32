#include "alarms.h"
#include <Preferences.h>
#include <time.h>
#include "config.h"
#include "net.h"
#include "audio.h"
#include "timesync.h"

struct Alarm {
  String id;
  uint8_t hour, minute;
  uint8_t days;  // bit 0 = Sunday … bit 6 = Saturday (repeating alarms)
  String date;   // "2026-10-12" for a one-time alarm, else empty
  String tone;
  bool enabled;
};

static Preferences prefs;
static Alarm alarms[MAX_ALARMS];
static int alarmCount = 0;
static String version;

// Snooze: ring the same tone again later. Timed by millis(), so it works offline.
static String snoozeTone;
static unsigned long snoozeAt = 0;
static bool snoozed = false;

// The list arrives as JSON; the same JSON is what's saved in flash
static void load(const String &json) {
  JsonDocument doc;
  alarmCount = 0;
  if (deserializeJson(doc, json)) return;
  for (JsonObjectConst a : doc.as<JsonArrayConst>()) {
    if (alarmCount == MAX_ALARMS) break;
    int h, m;
    if (sscanf(a["time"] | "", "%d:%d", &h, &m) != 2) continue;
    Alarm &x = alarms[alarmCount++];
    x.id = a["id"] | "";
    x.hour = h;
    x.minute = m;
    x.days = 0;
    for (int d : a["days"].as<JsonArrayConst>()) if (d >= 0 && d <= 6) x.days |= 1 << d;
    x.date = a["date"] | "";
    x.tone = a["tone"] | "";
    x.enabled = a["enabled"] | true;
  }
}

static void sendAck() {
  netSend("{\"type\":\"alarms_ack\",\"version\":\"" + version + "\",\"count\":" + String(alarmCount) + "}");
}

void alarmsSetup() {
  prefs.begin("alarms");
  version = prefs.getString("ver", "");
  load(prefs.getString("list", "[]"));
  Serial.printf("%d alarm(s) loaded (version %s)\n", alarmCount, version.c_str());
}

void alarmsSync(JsonDocument &msg) {
  String newVersion = msg["version"] | "";
  if (newVersion != version) {
    String json;
    serializeJson(msg["alarms"], json);
    load(json);
    version = newVersion;
    prefs.putString("list", json);
    prefs.putString("ver", version);
    Serial.printf("Alarms updated: %d alarm(s), version %s\n", alarmCount, version.c_str());
  }
  sendAck();
}

void alarmsSnooze() {
  if (!isRinging()) return;
  snoozeTone = ringingToneName();
  toneStop();
  snoozed = true;
  snoozeAt = millis() + SNOOZE_MS;
  time_t until = time(nullptr) + SNOOZE_MS / 1000;
  struct tm t;
  localtime_r(&until, &t);
  char hhmm[6];
  snprintf(hhmm, sizeof(hhmm), "%02d:%02d", t.tm_hour, t.tm_min);
  Serial.printf("Snoozed until %s\n", hhmm);
  netSend(String("{\"type\":\"snoozed\",\"until\":\"") + (timeValid() ? hhmm : "") + "\"}");
}

void alarmsLoop() {
  if (snoozed && (long)(millis() - snoozeAt) >= 0) {  // snooze is over: ring again
    snoozed = false;
    toneStart(snoozeTone, true);
  }

  static unsigned long lastCheck = 0;
  static long handledMinute = -1;  // each minute is checked once, so an alarm can't ring twice
  if (millis() - lastCheck < 1000) return;
  lastCheck = millis();
  if (!timeValid()) return;  // never ring on a clock that hasn't been set

  time_t now = time(nullptr);
  long minute = now / 60;
  if (minute == handledMinute) return;
  handledMinute = minute;

  struct tm t;
  localtime_r(&now, &t);
  char today[11];
  strftime(today, sizeof(today), "%Y-%m-%d", &t);
  for (int i = 0; i < alarmCount; i++) {
    const Alarm &a = alarms[i];
    if (!a.enabled || a.hour != t.tm_hour || a.minute != t.tm_min) continue;
    bool due = a.date.length() ? a.date == today : (a.days & (1 << t.tm_wday));
    if (!due) continue;
    char hhmm[6];
    snprintf(hhmm, sizeof(hhmm), "%02d:%02d", a.hour, a.minute);
    Serial.printf("Alarm %s: ringing %s\n", hhmm, a.tone.c_str());
    snoozed = false;  // a new alarm replaces a pending snooze
    toneStart(a.tone, true);
    netSend("{\"type\":\"alarm_fired\",\"alarmId\":\"" + a.id + "\",\"time\":\"" + hhmm + "\"}");
    break;  // one sound at a time; the first due alarm wins
  }
}
