#include "led.h"
#include "config.h"
#include "net.h"
#include "audio.h"
#include "ota.h"
#include "mic.h"
#include "buttons.h"

// A color and how it moves: steady, blink (on half the period) or pulse (fades up and down)
enum Pattern : uint8_t { STEADY, BLINK, PULSE };
struct Look {
  uint8_t r, g, b;
  Pattern pattern;
  uint16_t period;  // ms, for BLINK and PULSE
};

static const Look OFF = {0, 0, 0, STEADY, 0};
static const Look STARTING = {255, 255, 255, STEADY, 0};
static const Look UPDATING = {0, 0, 255, BLINK, 200};
static const Look IDENTIFY = {255, 255, 255, BLINK, 300};
static const Look LISTENING = {255, 0, 120, PULSE, 1200};  // pink: the microphone is sending
static const Look THINKING = {0, 90, 255, PULSE, 700};      // blue: the AI is working out its answer
static const Look RINGING = {255, 70, 0, PULSE, 1500};
static const Look MUSIC = {0, 255, 60, PULSE, 3000};  // slow green
static const Look TALKING = {160, 0, 255, STEADY, 0};
static const Look DOORBELL = {0, 255, 255, BLINK, 250};
static const Look SETUP = {0, 0, 255, BLINK, 2000};
static const Look OFFLINE = {255, 0, 0, BLINK, 2000};
static const Look CONNECTING = {255, 160, 0, BLINK, 600};
static const Look SOS = {255, 0, 0, BLINK, 300};  // fast red: the SOS button (buttons.h)
static const Look ONLINE = {0, 40, 0, STEADY, 0};  // dim green

static volatile unsigned long identifyUntil = 0;

void ledIdentify() { identifyUntil = millis() + LED_IDENTIFY_MS; }

// What to show now: the most important state wins
static Look pick(unsigned long now) {
  static unsigned long lastOnline = 0;  // boot counts as "just online": connecting, not offline
  static unsigned long lastActive = 0;  // anything but plain "online" was showing

  Look look;
  bool online = netConnected();
  if (online) lastOnline = now;
  if (otaRunning()) look = UPDATING;
  else if (buttonsSos()) look = SOS;
  else if (micBusy()) {  // always shown while the mic is on (privacy)
    MicState m = micState();
    // A call stays pink from start to end, whether or not the caller is talking
    look = micInCall() ? LISTENING : isTalking() ? TALKING : m == MIC_THINKING ? THINKING : LISTENING;
  }
  else if ((long)(identifyUntil - now) > 0) look = IDENTIFY;
  else if (isMusic()) look = MUSIC;
  else if (isRinging()) look = RINGING;
  else if (isTalking()) look = TALKING;
  else if (isChiming()) look = DOORBELL;
  else if (now < LED_STARTING_MS) look = STARTING;
  else if (netInSetup()) look = SETUP;
  else if (!online) look = now - lastOnline > LED_OFFLINE_MS ? OFFLINE : CONNECTING;
  else {
    // All good: dim green for a few seconds, then dark (no light in the bedroom at night)
    return now - lastActive < LED_ONLINE_MS ? ONLINE : OFF;
  }
  lastActive = now;
  return look;
}

// 0-255 brightness of the pattern at this moment
static uint8_t level(const Look &l, unsigned long now) {
  if (l.pattern == STEADY) return 255;
  uint32_t t = now % l.period;
  if (l.pattern == BLINK) return t < l.period / 2 ? 255 : 0;
  uint32_t half = l.period / 2;  // PULSE: up then down
  uint32_t x = t < half ? t : l.period - t;
  return 20 + 235 * x / half;
}

static void ledTask(void *) {
  for (;;) {
    unsigned long now = millis();
    Look l = pick(now);
    uint8_t k = level(l, now);
    ledcWrite(LED_R_GPIO, l.r * k / 255);
    ledcWrite(LED_G_GPIO, l.g * k / 255);
    ledcWrite(LED_B_GPIO, l.b * k / 255);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void ledSetup() {
  for (int pin : {LED_R_GPIO, LED_G_GPIO, LED_B_GPIO}) ledcAttach(pin, 5000, 8);  // PWM: 8-bit brightness
  xTaskCreatePinnedToCore(ledTask, "led", 2048, nullptr, 1, nullptr, 0);
}
