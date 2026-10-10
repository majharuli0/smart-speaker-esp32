// The user's sounds on the speaker: folders and files under /sounds on the SD
// card ("card") and the built-in storage ("builtin"), managed from the app
// through files_* commands (list, new folder, rename, move, delete, download
// into place, play). Paths are relative to /sounds, e.g. "/Music/Rain.wav".
// The alarm-tone copies (tonecache) live outside /sounds and never show here.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>

void filesCommand(JsonDocument &msg);  // a files_* command from the server
void filesLoop();                      // downloads and copies, a small piece per call

// "card:/Music/Rain.wav" or "builtin:/Rain.wav": a sound to play (an alarm or a preview)
bool isFileSound(const String &sound);
bool filesOpen(const String &sound, File &file);
