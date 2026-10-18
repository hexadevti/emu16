// input_p4.cpp -- the PicoCalc keyboard: an STM32 at I2C 0x1F that also owns both backlights and
// reads the battery gauge. Protocol and timings from emu8 (src/picocalc/input_picocalc.cpp):
//
//  * FIFO poll is split-phase: writing register 0x09 selects (and dequeues) the next event; a
//    2-byte read some time later returns {state, code}, {0,0} when empty. At the 10kHz the STM32
//    needs, one read costs ~3ms, so emu16 polls from its own task on core 0 every 10ms and hands
//    events over through a queue -- the emulation core never touches the bus.
//  * The battery register 0x0B rides the same poll every 10s, only in place of a FIFO select that
//    came back empty: {0x0B, v}, percent = v & 0x7F, bit 7 = charging.
//  * Backlights: write (reg | 0x80, level); 0x0A = LCD, 0x05 = keyboard.
#include "../../../board.h"

#if defined(BOARD_PICOCALC_P4)

#include <Arduino.h>
#include <Wire.h>
#include "../hal.h"

#define KBD_REG_FIF   0x09
#define KBD_REG_BAT   0x0B
#define KBD_REG_BKL   0x05        // keyboard backlight
#define KBD_REG_BK2   0x0A        // LCD backlight
#define KBD_REG_WRITE 0x80
#define BAT_POLL_MS   10000

void p4CheckSerialCmd();          // system_p4.cpp

static QueueHandle_t s_keys = nullptr;
static bool s_wireUp = false;
static bool s_selPending = false, s_batPending = false;
static volatile int  s_batPercent = -1;
static volatile bool s_batCharging = false;
bool g_bootedHeadless = false;

static void wireBegin() {
  if (s_wireUp) return;
  Wire.begin(KBD_I2C_SDA_PIN, KBD_I2C_SCL_PIN, KBD_I2C_HZ);
  s_wireUp = true;
}

static void selectReg(uint8_t reg) {
  Wire.beginTransmission(KBD_I2C_ADDR);
  Wire.write(reg);
  const bool ok = Wire.endTransmission() == 0;
  s_selPending = ok && reg == KBD_REG_FIF;
  s_batPending = ok && reg == KBD_REG_BAT;
}

static void readBat() {
  s_batPending = false;
  if (Wire.requestFrom((uint8_t)KBD_I2C_ADDR, (size_t)2) != 2) return;
  const uint8_t r = (uint8_t)Wire.read(), v = (uint8_t)Wire.read();
  if (r != KBD_REG_BAT || (v & 0x7F) > 100) return;
  s_batPercent = v & 0x7F;
  s_batCharging = (v & 0x80) != 0;
}

// Selecting the FIFO already dequeued an event, so a failed read loses it -- possibly a release,
// which would leave a game button held for good. Then (and when our own queue overflows) a
// KEY_RELEASE_ALL goes to the frontend instead; a key still down is picked up again on its next press.
static bool s_lost = false;

static void flushLost() {
  if (!s_lost) return;
  const KeyEvent all = { KEY_RELEASE_ALL, KEYSTATE_RELEASED };
  if (xQueueSend(s_keys, &all, 0) == pdTRUE) s_lost = false;
}

static void sendKey(uint8_t code, uint8_t state) {
  flushLost();
  if (s_lost) return;                    // still full: this one is covered by the release-all
  const KeyEvent ev = { code, state };
  if (xQueueSend(s_keys, &ev, 0) != pdTRUE) s_lost = true;
}

static bool readFifo(uint8_t *state, uint8_t *code) {
  if (!s_selPending) return false;
  s_selPending = false;
  if (Wire.requestFrom((uint8_t)KBD_I2C_ADDR, (size_t)2) != 2) { s_lost = true; return false; }
  const uint8_t s = (uint8_t)Wire.read(), c = (uint8_t)Wire.read();
  if (s == 0 && c == 0) return false;
  *state = s; *code = c;
  return true;
}

static bool mainboardPresent() {
  wireBegin();
  Wire.beginTransmission(KBD_I2C_ADDR);
  return Wire.endTransmission() == 0;
}

// The module has its own USB power, so the PicoCalc's switch does not reset it. If the mainboard
// is off at boot, nothing on it (panel, SD, keyboard) can be set up: wait for it a while.
bool picocalcWaitForMainboard() {
  if (mainboardPresent()) return true;
  hal_log("Waiting for the PicoCalc mainboard - turn the unit on");
  const uint32_t t0 = millis();
  while (millis() - t0 < PICOCALC_MAINBOARD_WAIT_MS) {
    if (mainboardPresent()) { delay(300); return true; }
    delay(100);
  }
  hal_log("mainboard never came up; will reboot when it does");
  g_bootedHeadless = true;
  return false;
}

static bool bklWrite(uint8_t reg, uint8_t level) {
  Wire.beginTransmission(KBD_I2C_ADDR);
  Wire.write((uint8_t)(reg | KBD_REG_WRITE));
  Wire.write(level);
  return Wire.endTransmission() == 0;
}

void picocalcBacklightOn() {
  wireBegin();
  const bool a = bklWrite(KBD_REG_BK2, 0xFF);
  const bool b = bklWrite(KBD_REG_BKL, 0xFF);
  if (!a || !b) hal_log("backlight write failed");
}

static void kbdTask(void *) {
  uint32_t lastBat = 0;
  bool batOnce = false;
  selectReg(KBD_REG_FIF);
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(10));
    p4CheckSerialCmd();
    if (g_bootedHeadless) {
      if (mainboardPresent()) { hal_log("mainboard is up - rebooting"); delay(50); ESP.restart(); }
      continue;
    }
    if (s_batPending) { readBat(); selectReg(KBD_REG_FIF); continue; }
    uint8_t state, code;
    // Drain a few events per tick so a burst (several keys at once) is not spread over frames.
    int got = 0;
    while (got < 4 && readFifo(&state, &code)) {
      sendKey(code, state);
      got++;
      selectReg(KBD_REG_FIF);
      delayMicroseconds(2000);           // STM32 turnaround after a select
    }
    flushLost();
    if (got) { selectReg(KBD_REG_FIF); continue; }
    const uint32_t now = millis();
    if (!batOnce || now - lastBat >= BAT_POLL_MS) {   // only in place of an empty FIFO select
      batOnce = true; lastBat = now;
      selectReg(KBD_REG_BAT);
    } else {
      selectReg(KBD_REG_FIF);
    }
  }
}

void p4InputInit() {
  wireBegin();
  s_keys = xQueueCreate(64, sizeof(KeyEvent));
  xTaskCreatePinnedToCore(kbdTask, "kbd", 4096, nullptr, 4, nullptr, 0);
}

// A key typed on the PC (the console's "@key" command), queued like one from the keyboard.
void p4InjectKey(uint8_t code, uint8_t state) { if (s_keys) sendKey(code, state); }

bool hal_key_poll(KeyEvent *ev) { return s_keys && xQueueReceive(s_keys, ev, 0) == pdTRUE; }
int  hal_battery_percent()      { return s_batPercent; }
bool hal_battery_charging()     { return s_batCharging; }

#endif // BOARD_PICOCALC_P4
