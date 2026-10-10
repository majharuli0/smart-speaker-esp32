#include "files.h"
#include "audio.h"
#include "config.h"
#include "net.h"
#include "stats.h"
#include "tonecache.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include "mbedtls/sha256.h"

static const char *ROOT = "/sounds";

// ---- Paths ----

// A path from the server: "/Music/Rain.wav". No "..", no empty parts, nothing a
// FAT name can't hold. Returns "" if it's not acceptable.
static String cleanPath(const String &p) {
  if (!p.startsWith("/") || p.length() > 200 || p.indexOf("..") >= 0 || p.indexOf("//") >= 0) return "";
  for (char c : p)
    if ((uint8_t)c < 0x20 || strchr("\\:*?\"<>|", c)) return "";
  return p == "/" ? "" : (p.endsWith("/") ? p.substring(0, p.length() - 1) : p);
}

static String full(const String &path) { return String(ROOT) + path; }  // "" (the root) → "/sounds"

static String nameOf(const String &path) { return path.substring(path.lastIndexOf('/') + 1); }

static void ensureRoot(fs::FS &fs) {
  if (!fs.exists(ROOT)) fs.mkdir(ROOT);
}

// ---- Replies ----

// Every command answers once: {type:"file_reply", req, ok, error?, ...}
static void reply(JsonDocument &msg, bool ok, const char *error = nullptr, JsonDocument *extra = nullptr) {
  JsonDocument out;
  if (extra) out.set(*extra);
  out["type"] = "file_reply";
  out["req"] = msg["req"];
  out["ok"] = ok;
  if (error) out["error"] = error;
  String s;
  serializeJson(out, s);
  netSend(s);
}

// ---- Listing ----

static const int MAX_ENTRIES = 150;  // one reply must fit an MQTT message

static void list(JsonDocument &msg, fs::FS &fs, const String &path, bool recursive) {
  File dir = fs.open(full(path));
  if (!dir || !dir.isDirectory()) return reply(msg, false, "no such folder");
  JsonDocument out;
  JsonArray entries = out["entries"].to<JsonArray>();
  // Breadth-first through the folders (recursive: every audio file below, with its path)
  std::vector<String> folders{path};
  bool truncated = false;
  for (size_t i = 0; i < folders.size(); i++) {
    File d = i == 0 ? dir : fs.open(full(folders[i]));
    if (!d) continue;
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
      String name = nameOf(f.name());
      if (name.endsWith(".part")) continue;  // a download or copy in progress
      String child = folders[i] + "/" + name;
      if (f.isDirectory() && recursive) folders.push_back(child);
      if (recursive && f.isDirectory()) continue;
      if ((int)entries.size() == MAX_ENTRIES) { truncated = true; break; }
      JsonObject e = entries.add<JsonObject>();
      e[recursive ? "path" : "name"] = recursive ? child : name;
      e["dir"] = f.isDirectory();
      if (!f.isDirectory()) e["size"] = f.size();
    }
    if (truncated) break;
  }
  out["store"] = msg["store"];
  out["path"] = path.isEmpty() ? "/" : path;
  out["truncated"] = truncated;
  reply(msg, true, nullptr, &out);
}

// ---- Deleting (folders with everything in them) ----

static bool removeAll(fs::FS &fs, const String &p) {
  File f = fs.open(p);
  if (!f) return false;
  if (!f.isDirectory()) {
    f.close();
    return fs.remove(p);
  }
  std::vector<String> children;
  for (File c = f.openNextFile(); c; c = f.openNextFile()) children.push_back(p + "/" + nameOf(c.name()));
  f.close();
  for (const String &c : children) removeAll(fs, c);
  return fs.rmdir(p);
}

// ---- Jobs: a download into place, or a copy to the other storage ----
// One at a time, a small piece per loop, so sound and commands keep running.

struct Job {
  bool active = false, isCopy = false;
  JsonDocument msg;          // the command, for the reply
  fs::FS *fs = nullptr;      // where it's written
  String path, part;         // final and temporary file (full paths)
  uint32_t size = 0, done = 0;
  String sha;                // download: expected SHA-256
  File src, out;             // copy: source; both: destination
  fs::FS *srcFs = nullptr;
  String srcPath;            // copy: delete the source after (a move)
  unsigned long lastData = 0, lastProgress = 0;
};
static Job job;
static HTTPClient http;
static mbedtls_sha256_context sha;
static uint8_t chunk[4096];

static void finish(bool ok, const char *error = nullptr) {
  job.out.close();
  if (job.src) job.src.close();
  if (!job.isCopy) {
    http.end();
    if (ok) {
      uint8_t digest[32];
      mbedtls_sha256_finish(&sha, digest);
      char hex[65];
      for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", digest[i]);
      if (job.sha != hex) {
        ok = false;
        error = "the download was damaged";
      }
    }
    mbedtls_sha256_free(&sha);
  }
  if (ok) ok = job.fs->rename(job.part, job.path);
  if (ok && job.isCopy) removeAll(*job.srcFs, job.srcPath);  // a move: the original goes
  if (!ok) job.fs->remove(job.part);
  JsonDocument out;
  out["type"] = "file_done";
  out["req"] = job.msg["req"];
  out["ok"] = ok;
  if (!ok) out["error"] = error ? error : "couldn't save the file";
  String s;
  serializeJson(out, s);
  netSend(s);
  Serial.printf("%s %s: %s\n", job.isCopy ? "Move" : "Download", job.path.c_str(), ok ? "done" : (error ? error : "failed"));
  job.active = false;
  job.msg.clear();
  sendPartitions();  // storage use changed
}

static void progress() {
  if (millis() - job.lastProgress < 1000) return;
  job.lastProgress = millis();
  JsonDocument out;
  out["type"] = "file_progress";
  out["req"] = job.msg["req"];
  out["done"] = job.done;
  out["size"] = job.size;
  String s;
  serializeJson(out, s);
  netSend(s);
}

static void startDownload(JsonDocument &msg, fs::FS &fs, const String &path) {
  String url = msg["url"] | "";
  uint32_t size = msg["size"] | 0;
  String want = msg["sha256"] | "";
  if (!url.startsWith("/") || !size || want.length() != 64) return reply(msg, false, "bad request");
  if (serverHttp().isEmpty()) return reply(msg, false, "the speaker doesn't know the server's address yet");
  http.setConnectTimeout(TONE_TIMEOUT_MS);
  http.setTimeout(TONE_TIMEOUT_MS);
  http.begin(serverHttp() + url);
  if (http.GET() != HTTP_CODE_OK) {
    http.end();
    return reply(msg, false, "couldn't download the file");
  }
  job.part = full(path) + ".part";
  job.out = fs.open(job.part, "w");
  if (!job.out) {
    http.end();
    return reply(msg, false, "couldn't write to this storage");
  }
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  job.isCopy = false;
  job.fs = &fs;
  job.path = full(path);
  job.size = size;
  job.sha = want;
  job.done = 0;
  job.lastData = job.lastProgress = millis();
  job.msg.set(msg);
  job.active = true;
  reply(msg, true);  // started; file_progress and file_done follow
}

static void startCopy(JsonDocument &msg, fs::FS &from, const String &fromPath, fs::FS &to, const String &toPath) {
  File src = from.open(full(fromPath));
  if (!src || src.isDirectory()) return reply(msg, false, "only files can move to the other storage");
  job.part = full(toPath) + ".part";
  job.out = to.open(job.part, "w");
  if (!job.out) return reply(msg, false, "couldn't write to this storage");
  job.isCopy = true;
  job.src = src;
  job.srcFs = &from;
  job.srcPath = full(fromPath);
  job.fs = &to;
  job.path = full(toPath);
  job.size = src.size();
  job.done = 0;
  job.lastData = job.lastProgress = millis();
  job.msg.set(msg);
  job.active = true;
  reply(msg, true);
}

void filesLoop() {
  if (!job.active) return;
  size_t n;
  if (job.isCopy) {
    n = job.src.read(chunk, sizeof(chunk));
    if (n == 0) return finish(job.done == job.size, "couldn't read the file");
  } else {
    WiFiClient *stream = http.getStreamPtr();
    size_t avail = stream->available();
    if (!avail) {
      if (millis() - job.lastData > TONE_TIMEOUT_MS * 3) finish(false, "the download stalled");
      return;
    }
    n = stream->readBytes(chunk, min(avail, sizeof(chunk)));
    mbedtls_sha256_update(&sha, chunk, n);
  }
  if (job.out.write(chunk, n) != n) return finish(false, "the storage is full");
  job.done += n;
  job.lastData = millis();
  progress();
  if (job.done >= job.size) finish(job.done == job.size);
}

// ---- Commands ----

void filesCommand(JsonDocument &msg) {
  String cmd = msg["type"] | "";
  fs::FS *fs = storage(msg["store"] | "");
  if (!fs) return reply(msg, false, "that storage isn't available (no card?)");
  ensureRoot(*fs);
  String path = msg["path"] == "/" ? String("") : cleanPath(msg["path"] | "");
  bool root = msg["path"] == "/";
  if (!root && path.isEmpty()) return reply(msg, false, "bad path");

  if (cmd == "files_list") return list(msg, *fs, path, msg["recursive"] | false);
  if (root) return reply(msg, false, "bad path");  // everything else needs a real file or folder

  if (cmd == "files_mkdir") {
    if (fs->exists(full(path))) return reply(msg, false, "that name is already taken");
    return reply(msg, fs->mkdir(full(path)), "couldn't create the folder");
  }
  if (cmd == "files_delete") {
    if (!fs->exists(full(path))) return reply(msg, false, "no such file or folder");
    if (job.active && job.path.startsWith(full(path))) return reply(msg, false, "it's being written right now");
    bool ok = removeAll(*fs, full(path));
    if (ok) sendPartitions();
    return reply(msg, ok, "couldn't delete it");
  }
  if (cmd == "files_play") {
    if (!fs->exists(full(path))) return reply(msg, false, "no such file");
    toneStart(String(msg["store"] | "") + ":" + path, false, true);
    return reply(msg, true);
  }
  if (job.active) return reply(msg, false, "busy with another upload or move; try again in a moment");

  if (cmd == "files_put") {
    if (fs->exists(full(path))) return reply(msg, false, "a file with that name is already there");
    return startDownload(msg, *fs, path);
  }
  if (cmd == "files_move") {  // rename, move to another folder, or to the other storage
    fs::FS *toFs = storage(msg["toStore"] | (const char *)(msg["store"] | ""));
    String to = cleanPath(msg["to"] | "");
    if (!toFs || to.isEmpty()) return reply(msg, false, "bad destination");
    ensureRoot(*toFs);
    if (!fs->exists(full(path))) return reply(msg, false, "no such file or folder");
    if (toFs->exists(full(to))) return reply(msg, false, "that name is already taken");
    if (toFs == fs) return reply(msg, fs->rename(full(path), full(to)), "couldn't move it");
    return startCopy(msg, *fs, path, *toFs, to);
  }
  reply(msg, false, "unknown command");
}

// ---- Playing ----

bool isFileSound(const String &sound) { return sound.startsWith("card:/") || sound.startsWith("builtin:/"); }

bool filesOpen(const String &sound, File &file) {
  int colon = sound.indexOf(':');
  fs::FS *fs = storage(sound.substring(0, colon));
  String path = cleanPath(sound.substring(colon + 1));
  if (!fs || path.isEmpty()) return false;
  file = fs->open(full(path), "r");
  return file && !file.isDirectory();
}
