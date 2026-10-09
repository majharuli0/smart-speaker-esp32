#include "ota.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include "mbedtls/sha256.h"
#include "config.h"
#include "net.h"
#include "audio.h"

// Found by the server inside an uploaded .bin, so it knows the board and
// version without asking, and can refuse files that aren't this firmware.
static const char FW_MARKER[] = "SSFW:" CONFIG_IDF_TARGET ":" FW_VERSION ":END";

static bool onProbation = false;  // running a new version that isn't confirmed yet

// The Arduino core normally confirms every new version at boot. Returning
// true here means "we'll confirm it ourselves" (after reaching the server).
extern "C" bool verifyRollbackLater() { return true; }

const char *boardName() { return CONFIG_IDF_TARGET; }

static void report(const char *state, int progress = -1, const String &error = "") {
  String msg = String("{\"type\":\"ota\",\"state\":\"") + state + "\"";
  if (progress >= 0) msg += ",\"progress\":" + String(progress);
  if (error.length()) msg += ",\"error\":\"" + error + "\"";
  msg += ",\"version\":\"" FW_VERSION "\"}";
  netSend(msg);
}

void otaSetup() {
  Serial.println(FW_MARKER);  // also keeps the marker in the binary
  esp_ota_img_states_t state;
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
    onProbation = true;
    Serial.println("New firmware on probation: must reach the server to keep it");
  }
}

void otaLoop() {
  if (!onProbation) return;
  if (netConnected()) {
    esp_ota_mark_app_valid_cancel_rollback();
    onProbation = false;
    Serial.println("New firmware confirmed");
    report("done");
  } else if (millis() > OTA_CONFIRM_MS) {
    Serial.println("New firmware never reached the server: going back to the previous version");
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}

void otaReport() {
  // A previous update that failed and was rolled back leaves its slot marked
  // invalid (until the next update overwrites it). Say so once per boot.
  static bool reported = false;
  if (reported) return;
  reported = true;
  if (esp_ota_get_last_invalid_partition()) report("rolled_back", -1, "the new firmware failed to start, so the device went back to this version");
}

static bool fail(const String &why) {
  Serial.println("Update failed: " + why);
  report("failed", -1, why);
  return false;
}

// Blocking: the device does nothing else for the few seconds this takes
void otaStart(JsonDocument &msg) {
  const uint32_t size = msg["size"] | 0;
  const String want = msg["sha256"] | "";
  const String path = msg["path"] | "";
  if (!size || want.length() != 64 || !path.startsWith("/firmware/")) { fail("bad update request"); return; }

  toneStop();
  talkStop();
  Serial.printf("Updating to %s (%u bytes)\n", (const char *)(msg["version"] | "?"), (unsigned)size);
  report("downloading", 0);

  HTTPClient http;
  http.setTimeout(10000);
  http.begin("http://" + serverAddress().toString() + ":" + String(SERVER_PORT) + path);
  if (http.GET() != HTTP_CODE_OK || (uint32_t)http.getSize() != size) { http.end(); fail("download failed"); return; }
  if (!Update.begin(size)) { http.end(); fail(Update.errorString()); return; }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[2048];
  uint32_t done = 0;
  int lastTenth = 0;
  unsigned long lastData = millis();
  bool ok = true;

  while (done < size) {
    size_t avail = stream->available();
    if (!avail) {
      if (millis() - lastData > 10000) { ok = fail("download stalled"); break; }
      delay(1);
      continue;
    }
    size_t n = stream->readBytes(buf, min(avail, sizeof(buf)));
    if (Update.write(buf, n) != n) { ok = fail(Update.errorString()); break; }
    mbedtls_sha256_update(&sha, buf, n);
    done += n;
    lastData = millis();
    int tenth = done * 10 / size;
    if (tenth != lastTenth) { lastTenth = tenth; report("downloading", tenth * 10); }
  }
  http.end();

  if (ok) {
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", digest[i]);
    if (want != hex) ok = fail("checksum mismatch");
  }
  mbedtls_sha256_free(&sha);
  if (!ok) { Update.abort(); return; }

  // Checks the image and makes it the one to boot (on probation, see otaLoop)
  if (!Update.end()) { fail(Update.errorString()); return; }
  report("restarting", 100);
  Serial.println("Update installed, restarting");
  delay(500);
  ESP.restart();
}
