// Status light: one RGB LED (common cathode). Shows the most important thing
// going on (updating, microphone on, music, ringing, talking, doorbell, setup, offline,
// connecting, online). Runs on its own task, so it keeps blinking while the main loop is busy.
#pragma once

void ledSetup();
void ledIdentify();  // "find my speaker" (the Blink button): flash white for a few seconds
