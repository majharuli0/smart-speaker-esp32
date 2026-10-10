// Device health for the web page: RAM, CPU, Wi-Fi, and the flash layout.
#pragma once

void statsSetup();          // measure program size + flash layout once (slow; do it at boot)
void statsLoop();           // stats every 2 s while a page is open, every 5 min otherwise
void sendPartitions();      // the flash layout (and SD card)
void setWatching(bool on);  // from the server's "watch": is a page open?
