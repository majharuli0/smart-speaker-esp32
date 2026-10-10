// Reliability: why the device last restarted (incl. crash details saved in
// the coredump partition), and a watchdog that restarts it if it freezes.
#pragma once

void healthSetup();     // first thing at boot: read the restart reason and any crash report
void healthWatchdog();  // last thing in setup(): restart if loop() ever stops for 5 s
void healthReport();    // on connect: send {type:"boot", ...} to the server, once per boot
