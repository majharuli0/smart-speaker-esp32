// Device health for the web page: RAM, CPU, Wi-Fi, and the flash layout.
#pragma once

void statsSetup();      // measure program size + flash layout once (slow; do it at boot)
void statsLoop();       // sends stats every STATS_INTERVAL_MS while connected
void sendPartitions();  // the flash layout measured in statsSetup()
