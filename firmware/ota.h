// Firmware updates over Wi-Fi, with automatic rollback.
// A new version starts "on probation": it becomes permanent only once it
// connects to the server. If it can't within OTA_CONFIRM_MS, or it crashes
// and restarts, the bootloader goes back to the previous version.
#pragma once
#include <ArduinoJson.h>

void otaSetup();                     // prints the firmware marker; notes if we're on probation
void otaLoop();                      // confirms the new version once connected, or rolls back
void otaStart(JsonDocument &msg);    // ota_start from the server: download, check, install, restart
void otaReport();                    // on connect: tell the page if an update just failed and rolled back
const char *boardName();             // "esp32s3" / "esp32"
