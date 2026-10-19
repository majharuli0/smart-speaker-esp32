#include "buttons.h"
#include "config.h"
#include "audio.h"
#include "mic.h"
#include "net.h"

struct Button {
  uint8_t pin;
  bool raw;                  // as read now (pressed: pulled to GND)
  bool down;                 // after debouncing
  unsigned long changed;     // when raw last changed
  unsigned long pressedAt;   // when down last became true
  bool held;                 // its hold action has run for this press
};
static Button stopButton = {BTN_STOP_GPIO}, momoButton = {BTN_MOMO_GPIO};

static unsigned long sosSince = 0;  // an SOS was asked for at this time (0: none)
static unsigned long sosSentAt = 0; // the server answered (0: not yet)
static unsigned long sosTriedAt = 0;

// Debounced; true when it has just changed
static bool update(Button &b, unsigned long now) {
  bool raw = digitalRead(b.pin) == LOW;
  if (raw != b.raw) {
    b.raw = raw;
    b.changed = now;
  }
  if (b.raw == b.down || now - b.changed < BTN_DEBOUNCE_MS) return false;
  b.down = b.raw;
  if (b.down) {
    b.pressedAt = now;
    b.held = false;
  }
  return true;
}

void buttonsSetup() {
  for (Button *b : {&stopButton, &momoButton}) {
    pinMode(b->pin, INPUT_PULLUP);
    b->raw = b->down = digitalRead(b->pin) == LOW;  // held at power-on: not a press
    b->held = true;
  }
}

static void sosSend(unsigned long now) {
  sosTriedAt = now;
  if (netConnected()) netSend("{\"type\":\"sos\"}");
}

void buttonsLoop() {
  unsigned long now = millis();

  // STOP: everything stops on the press itself; still held, an SOS
  if (update(stopButton, now) && stopButton.down) {
    toneStop();
    talkStop();
    micStop();
    netSend("{\"type\":\"button\",\"button\":\"stop\"}");
  }
  if (stopButton.down && !stopButton.held && now - stopButton.pressedAt >= SOS_HOLD_MS) {
    stopButton.held = true;
    Serial.println("SOS: button held");
    sosSince = now | 1;
    sosSentAt = 0;
    callCueStart();  // the soft chime: help is being called
    sosSend(now);
  }
  if (sosSentAt && now - sosSentAt >= SOS_SHOW_MS) sosSince = sosSentAt = 0;  // shown long enough
  // Not answered yet (offline, or lost): again every few seconds, for a while
  if (sosSince && !sosSentAt) {
    if (now - sosSince > SOS_RETRY_FOR_MS) {
      Serial.println("SOS: no answer from the server, given up");
      sosSince = 0;
      cueStart(true);
    } else if (now - sosTriedAt >= SOS_RETRY_MS) sosSend(now);
  }

  // MOMO: a short press (released within BTN_SHORT_MS) starts a conversation
  if (update(momoButton, now) && !momoButton.down && !momoButton.held && now - momoButton.pressedAt < BTN_SHORT_MS) {
    if (micBusy()) return;  // already listening (or in a call)
    if (!netConnected()) {
      cueStart(true);  // can't: offline
      return;
    }
    Serial.println("Momo button");
    netSend("{\"type\":\"button\",\"button\":\"momo\"}");
  }
}

void buttonsSosSent(int people) {
  if (!sosSince) return;
  sosSentAt = millis() | 1;
  Serial.printf("SOS: the server told %d people\n", people);
  if (!people) cueStart(true);  // it went, but nobody could be reached (the server says so too)
}

// (Read by the light's task: it only reads)
bool buttonsSos() { return sosSince && (!sosSentAt || millis() - sosSentAt < SOS_SHOW_MS); }
