// Two buttons, each between its GPIO and GND (internal pull-ups; pins in config.h):
//   STOP · SOS: a press stops everything at once (an alarm or reminder, music,
//     Momo, a call); held for SOS_HOLD_MS it sends an SOS: the server alerts the
//     family and rings their apps, the speaker chimes and blinks red until the
//     server says it went (sent again while offline).
//   MOMO: a short press starts a conversation with Momo, like the wake word.
//     (Held: push-to-talk, later.)
#pragma once

void buttonsSetup();
void buttonsLoop();              // main loop
void buttonsSosSent(int people); // the server's answer to an SOS: how many it reached
bool buttonsSos();               // an SOS is on its way or just went (for the light)
