#include "tonecache.h"
#include "config.h"
#include "net.h"
#include "stats.h"
#include "audio.h"

#include <FFat.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include "mbedtls/sha256.h"

struct Tone {
  String name, sha256;
  uint32_t size;
};

// Where tones are kept. The card comes first: it's checked first when playing.
struct Store {
  const char *label;
  fs::FS *fs;
  bool ready;
};
enum { CARD = 0, BUILTIN = 1, STORES = 2 };
static Store stores[STORES] = {{"SD card", &SD, false}, {"built-in", &FFat, false}};

static Preferences prefs;
static Tone wanted[MAX_ALARMS];
static int wantedCount = 0;
static String wantedJson;

// Download in progress (one at a time): which tone, into which store
static int dl = -1, dlStore = -1;
static HTTPClient http;
static File out;
static mbedtls_sha256_context sha;
static uint32_t received = 0;
static unsigned long lastData = 0, nextTry = 0, lastCardCheck = 0;

// Stored file for a tone: named by its hash, so a re-uploaded tone (new hash)
// is a new file. 8.3 names keep FAT happy.
static String pathOf(const Tone &t) { return "/" + t.sha256.substring(0, 8) + ".wav"; }
static String tmpOf(const Tone &t) { return "/" + t.sha256.substring(0, 8) + ".tmp"; }

static bool isStored(int s, const Tone &t) {
  if (!stores[s].ready || !stores[s].fs->exists(pathOf(t))) return false;
  File f = stores[s].fs->open(pathOf(t), "r");
  bool ok = f && f.size() == t.size;
  f.close();
  return ok;
}

static bool storedAnywhere(const Tone &t) { return isStored(CARD, t) || isStored(BUILTIN, t); }

static uint64_t freeBytes(int s) {
  if (s == CARD) return SD.totalBytes() - SD.usedBytes();
  return FFat.totalBytes() - FFat.usedBytes();
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

// Delete stored tones no alarm uses any more, and leftover partial downloads.
// Only our own files (8 hex chars + .wav/.tmp in the root): anything else the
// user put on the card is left alone.
static void removeUnused(int s) {
  if (!stores[s].ready) return;
  File root = stores[s].fs->open("/");
  if (!root) return;
  std::vector<String> toRemove;
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    String p = String("/") + f.name();
    bool ours = !f.isDirectory() && p.length() == 13 && (p.endsWith(".wav") || p.endsWith(".tmp"));
    bool keep = !ours;
    for (int i = 0; i < wantedCount && !keep; i++) keep = (p == pathOf(wanted[i]));
    if (!keep) toRemove.push_back(p);
    f.close();
  }
  root.close();
  for (const String &p : toRemove) {
    stores[s].fs->remove(p);
    Serial.printf("Removed unused tone %s from %s\n", p.c_str(), stores[s].label);
  }
}

// ---- Downloads ----

static void endDownload(bool ok) {
  const Tone &t = wanted[dl];
  fs::FS &fs = *stores[dlStore].fs;
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
    fs.remove(pathOf(t));
    fs.rename(tmpOf(t), pathOf(t));
    Serial.printf("Stored tone %s on %s (%u bytes)\n", t.name.c_str(), stores[dlStore].label, (unsigned)t.size);
    sendPartitions();  // the page's storage table now includes it
  } else {
    if (stores[dlStore].ready) fs.remove(tmpOf(t));
    nextTry = millis() + TONE_RETRY_MS;
  }
  dl = dlStore = -1;
  sendCacheStatus();
}

static void startDownload(int i, int s) {
  const Tone &t = wanted[i];
  http.setConnectTimeout(TONE_TIMEOUT_MS);
  http.setTimeout(TONE_TIMEOUT_MS);
  http.begin(serverHttp() + "/tones/" + t.name);
  if (http.GET() != HTTP_CODE_OK) {
    http.end();
    nextTry = millis() + TONE_RETRY_MS;
    return;
  }
  out = stores[s].fs->open(tmpOf(t), "w");
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
  dlStore = s;
  Serial.printf("Downloading tone %s to the %s...\n", t.name.c_str(), stores[s].label);
}

// Next tone missing from a store that has room for it: card first, then built-in
static bool startNextDownload() {
  for (int s = 0; s < STORES; s++) {
    if (!stores[s].ready) continue;
    for (int i = 0; i < wantedCount; i++) {
      if (isStored(s, wanted[i])) continue;
      if (freeBytes(s) < wanted[i].size + 64 * 1024) continue;  // too big for this store: the other one will hold it
      startDownload(i, s);
      return true;
    }
  }
  return false;
}

// ---- The card ----

static String cardProblem;  // why the card didn't mount, shown on the page

// The card didn't mount: talk to it directly (no file system) with the very
// first SD command, CMD0 "go idle", to tell "nothing answers" from "answers
// but won't mount".
static String probeCard() {
  SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  pinMode(SD_CS_GPIO, OUTPUT);
  digitalWrite(SD_CS_GPIO, HIGH);
  for (int i = 0; i < 10; i++) SPI.transfer(0xFF);  // 80 clocks with CS high wake the card up
  digitalWrite(SD_CS_GPIO, LOW);
  const uint8_t cmd0[] = {0x40, 0, 0, 0, 0, 0x95};
  for (uint8_t b : cmd0) SPI.transfer(b);
  uint8_t reply = 0xFF;
  for (int i = 0; i < 10 && reply == 0xFF; i++) reply = SPI.transfer(0xFF);
  digitalWrite(SD_CS_GPIO, HIGH);
  SPI.transfer(0xFF);
  SPI.endTransaction();

  if (reply == 0x01) return "the card answers, but won't mount: format it as FAT32 (not exFAT), or try another card";
  if (reply == 0xFF) return "no answer from the card: check the header is soldered, the CS/MOSI/SCK/MISO wires (10/11/12/13) and 3V3 power";
  if (reply == 0x00) return "MISO line stuck low: check the MISO wire (GPIO 13)";
  char buf[90];
  snprintf(buf, sizeof(buf), "unexpected reply 0x%02X from the card: check the wiring and contacts", reply);
  return buf;
}

// Diagnostic for "no answer": try the CMD0 reset with the four signal wires in
// every order on GPIOs 10-13. A reply means the wires are just in a different
// order (and says which); none means power or contact. Once per boot.
static String findWiring() {
  const int pins[4] = {10, 11, 12, 13};
  String found;
  for (int cs = 0; cs < 4 && !found.length(); cs++)
    for (int mosi = 0; mosi < 4 && !found.length(); mosi++)
      for (int sck = 0; sck < 4 && !found.length(); sck++)
        for (int miso = 0; miso < 4 && !found.length(); miso++) {
          if (cs == mosi || cs == sck || cs == miso || mosi == sck || mosi == miso || sck == miso) continue;
          SPI.end();
          SPI.begin(pins[sck], pins[miso], pins[mosi], pins[cs]);
          pinMode(pins[cs], OUTPUT);
          digitalWrite(pins[cs], HIGH);
          SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
          for (int i = 0; i < 10; i++) SPI.transfer(0xFF);
          digitalWrite(pins[cs], LOW);
          const uint8_t cmd0[] = {0x40, 0, 0, 0, 0, 0x95};
          for (uint8_t b : cmd0) SPI.transfer(b);
          uint8_t reply = 0xFF;
          for (int i = 0; i < 10 && reply == 0xFF; i++) reply = SPI.transfer(0xFF);
          digitalWrite(pins[cs], HIGH);
          SPI.endTransaction();
          if (reply == 0x01) {
            char buf[120];
            snprintf(buf, sizeof(buf), " Found it: the card answers with CS=%d MOSI=%d SCK=%d MISO=%d, so the wires are in a different order.",
                     pins[cs], pins[mosi], pins[sck], pins[miso]);
            found = buf;
          }
        }
  SPI.end();
  SPI.begin(SD_SCK_GPIO, SD_MISO_GPIO, SD_MOSI_GPIO, SD_CS_GPIO);  // back to the configured pins
  return found.length() ? found : String(" No wire order works either, so it's power or contact, not the order.");
}

static void mountCard() {
  if (!SD.begin(SD_CS_GPIO, SPI, SD_SPI_HZ, "/sd", 2)) {
    SD.end();
    String problem = probeCard();
    static bool searched = false;
    static String wiringNote;  // the search runs once; its result stays with every later report
    if (problem.startsWith("no answer")) {
      if (!searched) { searched = true; wiringNote = findWiring(); }
      problem += wiringNote;
    }
    if (problem != cardProblem) {  // say it once, and tell the page
      cardProblem = problem;
      Serial.println("SD card: " + problem);
      sendPartitions();
    }
    return;
  }
  cardProblem = "";
  stores[CARD].ready = true;
  static const char *types[] = {"none", "MMC", "SD", "SDHC/SDXC", "unknown"};
  uint8_t type = SD.cardType();
  Serial.printf("SD card found: %s, %llu MB, %llu MB used\n", types[min((int)type, 4)],
                SD.cardSize() / (1024 * 1024), SD.usedBytes() / (1024 * 1024));
  removeUnused(CARD);
  nextTry = 0;  // copy the tones onto it
  sendPartitions();
  sendCacheStatus();
}

static void cardGone() {
  if (dl >= 0 && dlStore == CARD) {  // the download was going to the card
    out.close();
    http.end();
    mbedtls_sha256_free(&sha);
    dl = dlStore = -1;
  }
  SD.end();
  stores[CARD].ready = false;
  Serial.println("SD card removed");
  sendPartitions();
  sendCacheStatus();
}

// Insert: try to mount every few seconds (not while sound plays: starting a
// card can take a moment). Remove: a real read of block 0 fails.
static void checkCard() {
  if (millis() - lastCardCheck < SD_CHECK_MS && lastCardCheck) return;
  lastCardCheck = millis();
  if (!stores[CARD].ready) {
    if (!audioBusy()) mountCard();
    return;
  }
  static uint8_t block[512];
  if (!SD.readRAW(block, 0)) cardGone();
}

void cacheCheckNow() { lastCardCheck = millis() - SD_CHECK_MS; }

// ---- Public ----

void cacheSetup() {
  // formatOnFail: the very first boot formats the empty built-in storage (takes a few seconds)
  stores[BUILTIN].ready = FFat.begin(true, "/ffat", 2, TONE_CACHE_PARTITION);
  if (!stores[BUILTIN].ready) Serial.println("Built-in tone storage unavailable");
  prefs.begin("cache");
  loadWanted(prefs.getString("wanted", "[]"));
  removeUnused(BUILTIN);
  if (stores[BUILTIN].ready)
    Serial.printf("Built-in tone storage: %u KB used of %u KB\n", (unsigned)(FFat.usedBytes() / 1024), (unsigned)(FFat.totalBytes() / 1024));

  SPI.begin(SD_SCK_GPIO, SD_MISO_GPIO, SD_MOSI_GPIO, SD_CS_GPIO);
  mountCard();
  if (!stores[CARD].ready) Serial.println("No SD card mounted (checking again every few seconds)");
  lastCardCheck = millis();
}

void cacheSync(JsonVariantConst tones) {
  String json = "[]";
  if (!tones.isNull()) { json = ""; serializeJson(tones, json); }
  if (json != wantedJson) {
    if (dl >= 0) endDownload(false);  // the list changed under the download: start over
    loadWanted(json);
    prefs.putString("wanted", json);
    for (int s = 0; s < STORES; s++) removeUnused(s);
    nextTry = 0;
  }
  sendCacheStatus();
}

void cacheLoop() {
  checkCard();
  if (WiFi.status() != WL_CONNECTED || serverHttp().isEmpty()) return;

  if (dl < 0) {
    if ((long)(millis() - nextTry) < 0) return;  // signed difference survives millis() wrapping
    if (!startNextDownload()) nextTry = millis() + TONE_RETRY_MS;  // all stored; check again later
    return;
  }

  // One small piece per call, so audio and the WebSocket keep running
  WiFiClient *stream = http.getStreamPtr();
  size_t avail = stream->available();
  if (avail) {
    uint8_t buf[1024];
    size_t n = stream->readBytes(buf, min(avail, sizeof(buf)));
    if (out.write(buf, n) != n) {  // card pulled out, or full
      Serial.println("Writing the tone failed; will retry");
      if (dlStore == CARD) cacheCheckNow();
      endDownload(false);
      return;
    }
    mbedtls_sha256_update(&sha, buf, n);
    received += n;
    lastData = millis();
    if (received >= wanted[dl].size) endDownload(received == wanted[dl].size);
  } else if (millis() - lastData > TONE_TIMEOUT_MS * 3) {
    Serial.println("Tone download stalled; will retry");
    endDownload(false);
  }
}

bool cacheOpen(const String &tone, File &file) {
  for (int i = 0; i < wantedCount; i++) {
    if (wanted[i].name != tone) continue;
    for (int s = 0; s < STORES; s++) {
      if (dl == i && dlStore == s) continue;  // still being written there
      if (!isStored(s, wanted[i])) continue;
      file = stores[s].fs->open(pathOf(wanted[i]), "r");
      if (file) {
        Serial.printf("Playing %s from the %s\n", tone.c_str(), stores[s].label);
        return true;
      }
    }
  }
  return false;
}

bool cacheReady() { return stores[BUILTIN].ready; }

fs::FS *storage(const String &name) {
  int s = name == "card" ? CARD : name == "builtin" ? BUILTIN : -1;
  return s >= 0 && stores[s].ready ? stores[s].fs : nullptr;
}

void cardInfo(JsonObject o) {
  o["present"] = stores[CARD].ready;
  if (!stores[CARD].ready) {
    if (cardProblem.length()) o["problem"] = cardProblem;
    return;
  }
  static const char *types[] = {"none", "MMC", "SD", "SDHC/SDXC", "unknown"};
  o["type"] = types[min((int)SD.cardType(), 4)];
  o["size"] = SD.cardSize();
  o["total"] = SD.totalBytes();
  o["used"] = SD.usedBytes();
}

void sendCacheStatus() {
  int stored = 0;
  for (int i = 0; i < wantedCount; i++) if (storedAnywhere(wanted[i])) stored++;
  netSend("{\"type\":\"cache\",\"stored\":" + String(stored) + ",\"wanted\":" + String(wantedCount) +
          ",\"card\":" + (stores[CARD].ready ? "true" : "false") + "}");
}
