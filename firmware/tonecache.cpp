#include "tonecache.h"
#include "config.h"
#include "net.h"
#include "stats.h"

#include <FFat.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include "mbedtls/sha256.h"

struct Tone {
  String name, sha256;
  uint32_t size;
};

static bool ready = false;
static Preferences prefs;
static Tone wanted[MAX_ALARMS];
static int wantedCount = 0;
static String wantedJson;

// Download in progress (one at a time)
static int dl = -1;  // index into wanted, or -1
static HTTPClient http;
static File out;
static mbedtls_sha256_context sha;
static uint32_t received = 0;
static unsigned long lastData = 0, nextTry = 0;

// Stored file for a tone: named by its hash, so a re-uploaded tone (new hash)
// is a new file. 8.3 names keep FAT happy.
static String pathOf(const Tone &t) { return "/" + t.sha256.substring(0, 8) + ".wav"; }
static String tmpOf(const Tone &t) { return "/" + t.sha256.substring(0, 8) + ".tmp"; }

static bool isStored(const Tone &t) {
  if (!FFat.exists(pathOf(t))) return false;
  File f = FFat.open(pathOf(t), "r");
  bool ok = f && f.size() == t.size;
  f.close();
  return ok;
}

static void loadWanted(const String &json) {
  JsonDocument doc;
  wantedCount = 0;
  wantedJson = json;
  if (deserializeJson(doc, json)) return;
  for (JsonObjectConst o : doc.as<JsonArrayConst>()) {
    if (wantedCount == MAX_ALARMS) break;
    Tone &t = wanted[wantedCount];
    t.name = o["name"] | "";
    t.sha256 = o["sha256"] | "";
    t.size = o["size"] | 0;
    if (t.name.length() && t.sha256.length() == 64 && t.size > 44) wantedCount++;
  }
}

// Delete stored tones no alarm uses any more, and leftover partial downloads
static void removeUnused() {
  File root = FFat.open("/");
  std::vector<String> toRemove;
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    String p = String("/") + f.name();
    bool keep = false;
    for (int i = 0; i < wantedCount; i++) if (p == pathOf(wanted[i])) keep = true;
    if (!keep) toRemove.push_back(p);
    f.close();
  }
  root.close();
  for (const String &p : toRemove) {
    FFat.remove(p);
    Serial.println("Removed unused tone " + p);
  }
}

static void endDownload(bool ok) {
  const Tone &t = wanted[dl];
  out.close();
  http.end();
  if (ok) {
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", digest[i]);
    ok = t.sha256 == hex;
    if (!ok) Serial.println("Tone " + t.name + ": checksum mismatch");
  }
  mbedtls_sha256_free(&sha);
  if (ok) {
    FFat.remove(pathOf(t));
    FFat.rename(tmpOf(t), pathOf(t));
    Serial.printf("Stored tone %s (%u bytes)\n", t.name.c_str(), (unsigned)t.size);
    sendPartitions();  // the page's flash table: storage now holds the tone
  } else {
    FFat.remove(tmpOf(t));
    nextTry = millis() + TONE_RETRY_MS;
  }
  dl = -1;
  sendCacheStatus();
}

static void startDownload(int i) {
  const Tone &t = wanted[i];
  http.setConnectTimeout(TONE_TIMEOUT_MS);
  http.setTimeout(TONE_TIMEOUT_MS);
  http.begin("http://" + serverAddress().toString() + ":" + String(SERVER_PORT) + "/tones/" + t.name);
  if (http.GET() != HTTP_CODE_OK) {
    http.end();
    nextTry = millis() + TONE_RETRY_MS;
    return;
  }
  out = FFat.open(tmpOf(t), "w");
  if (!out) {
    http.end();
    nextTry = millis() + TONE_RETRY_MS;
    return;
  }
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);  // 0 = SHA-256
  received = 0;
  lastData = millis();
  dl = i;
  Serial.println("Downloading tone " + t.name + " to store on the device...");
}

void cacheSetup() {
  // formatOnFail: the very first boot formats the empty storage (takes a few seconds)
  if (!FFat.begin(true, "/ffat", 2, TONE_CACHE_PARTITION)) {
    Serial.println("Tone storage unavailable; tones will stream instead");
    return;
  }
  ready = true;
  prefs.begin("cache");
  loadWanted(prefs.getString("wanted", "[]"));
  removeUnused();
  Serial.printf("Tone storage: %u KB used of %u KB\n", (unsigned)(FFat.usedBytes() / 1024), (unsigned)(FFat.totalBytes() / 1024));
}

void cacheSync(JsonVariantConst tones) {
  if (!ready) return;
  String json = "[]";
  if (!tones.isNull()) { json = ""; serializeJson(tones, json); }
  if (json != wantedJson) {
    if (dl >= 0) endDownload(false);  // the list changed under the download: start over
    loadWanted(json);
    prefs.putString("wanted", json);
    removeUnused();
    nextTry = 0;
  }
  sendCacheStatus();
}

void cacheLoop() {
  if (!ready || WiFi.status() != WL_CONNECTED || serverAddress() == IPAddress(0, 0, 0, 0)) return;

  if (dl < 0) {
    if ((long)(millis() - nextTry) < 0) return;  // signed difference survives millis() wrapping
    for (int i = 0; i < wantedCount; i++) {
      if (!isStored(wanted[i])) { startDownload(i); return; }
    }
    nextTry = millis() + TONE_RETRY_MS;  // all stored; check again later
    return;
  }

  // One small piece per call, so audio and the WebSocket keep running
  WiFiClient *stream = http.getStreamPtr();
  size_t avail = stream->available();
  if (avail) {
    uint8_t buf[1024];
    size_t n = stream->readBytes(buf, min(avail, sizeof(buf)));
    out.write(buf, n);
    mbedtls_sha256_update(&sha, buf, n);
    received += n;
    lastData = millis();
    if (received >= wanted[dl].size) endDownload(received == wanted[dl].size);
  } else if (millis() - lastData > TONE_TIMEOUT_MS * 3) {
    Serial.println("Tone download stalled; will retry");
    endDownload(false);
  }
}

String cachedPath(const String &tone) {
  if (!ready || (dl >= 0 && wanted[dl].name == tone)) return "";  // still downloading: stream instead
  for (int i = 0; i < wantedCount; i++) {
    if (wanted[i].name == tone && isStored(wanted[i])) return pathOf(wanted[i]);
  }
  return "";
}

bool cacheReady() { return ready; }

void sendCacheStatus() {
  int stored = 0;
  for (int i = 0; i < wantedCount; i++) if (isStored(wanted[i])) stored++;
  netSend("{\"type\":\"cache\",\"stored\":" + String(stored) + ",\"wanted\":" + String(wantedCount) + "}");
}

