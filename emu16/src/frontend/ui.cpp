// ui.cpp -- see ui.h.
#include <string.h>
#include <stdio.h>
#include "ui.h"
#include "uitext.h"
#include "../hal/hal.h"

namespace ui {

static uint16_t *s_cv = nullptr;

bool begin() {
  if (!s_cv) s_cv = (uint16_t *)hal_alloc_big(SCREEN_W * SCREEN_H * 2);
  return s_cv != nullptr;
}

uint16_t *canvas() { return s_cv; }

void fill(int x, int y, int w, int h, uint16_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > SCREEN_W) w = SCREEN_W - x;
  if (y + h > SCREEN_H) h = SCREEN_H - y;
  for (int r = 0; r < h; r++) {
    uint16_t *p = s_cv + (y + r) * SCREEN_W + x;
    for (int i = 0; i < w; i++) p[i] = c;
  }
}

void clear(uint16_t c) { fill(0, 0, SCREEN_W, SCREEN_H, c); }

int textWidth(const char *s, uint8_t font) { return uiTextWidth(s, font); }

void text(int x, int y, const char *s, uint16_t c, uint8_t font, uint8_t datum) {
  int32_t left, top;
  uiTextOrigin(s, x, y, datum, font, left, top);
  uiTextDraw(s, left, top, font, [&](int32_t px, int32_t py) {
    if ((uint32_t)px < SCREEN_W && (uint32_t)py < SCREEN_H) s_cv[py * SCREEN_W + px] = c;
  });
}

void present() { hal_display_push(0, 0, SCREEN_W, SCREEN_H, s_cv, SCREEN_W); }

void message(const char *title, const char *line, uint32_t holdMs) {
  const int w = 260, h = 64, x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
  fill(x - 2, y - 2, w + 4, h + 4, kAccent);
  fill(x, y, w, h, kBg);
  fill(x, y, w, 18, kTitleBg);
  text(SCREEN_W / 2, y + 9, title, kText, 2, MC);
  if (line) text(SCREEN_W / 2, y + 40, line, kText, 1, MC);
  present();
  if (holdMs) hal_delay(holdMs);
}

uint8_t waitKey(uint32_t timeoutMs) {
  const uint32_t t0 = hal_millis();
  for (;;) {
    KeyEvent ev;
    while (hal_key_poll(&ev)) {
      if (ev.state != KEYSTATE_PRESSED && ev.state != KEYSTATE_HELD) continue;
      // global hotkeys: Ctrl-Shift-Up arrives as PgUp, Ctrl-Shift-F3 as F8
      if (ev.code == KEY_PGUP && ev.state == KEYSTATE_PRESSED) hal_reboot_to_loader();
      if (ev.code == KEY_F8 && ev.state == KEYSTATE_PRESSED) hal_reboot();
      return ev.code;
    }
    if (timeoutMs && hal_millis() - t0 >= timeoutMs) return 0;
    hal_delay(10);
  }
}

static void drawBattery() {
  const int pct = hal_battery_percent();
  if (pct < 0) return;
  char b[16];
  snprintf(b, sizeof(b), "%s%d%%", hal_battery_charging() ? "+" : "", pct);
  text(SCREEN_W - 6, 9, b, kText, 1, MR);
}

uint8_t list(const char *title, const Item *items, int n, int *sel, const char *footer) {
  const int rowH = 16, top = 22, rows = (SCREEN_H - top - 16) / rowH;
  int first = 0;
  for (;;) {
    if (n <= 0) *sel = 0;
    else if (*sel < 0) *sel = 0;
    else if (*sel >= n) *sel = n - 1;
    if (*sel < first) first = *sel;
    if (*sel >= first + rows) first = *sel - rows + 1;

    clear();
    fill(0, 0, SCREEN_W, 18, kTitleBg);
    text(6, 9, title, kText, 2, ML);
    drawBattery();
    for (int i = 0; i < rows && first + i < n; i++) {
      const Item &it = items[first + i];
      const int y = top + i * rowH;
      const bool on = first + i == *sel;
      if (on) fill(2, y, SCREEN_W - 4, rowH - 1, kSelBg);
      const uint16_t c = on ? kSelText : kText;
      // keep long names inside the row, leaving room for the value column
      char buf[64];
      const int maxChars = (SCREEN_W - 16 - (it.value ? textWidth(it.value, 3) + 12 : 0)) / 7;
      snprintf(buf, sizeof(buf), "%s", it.label);
      if ((int)strlen(buf) > maxChars && maxChars > 3) { buf[maxChars - 2] = '.'; buf[maxChars - 1] = '.'; buf[maxChars] = 0; }
      text(8, y + rowH / 2, buf, c, 3, ML);
      if (it.value) text(SCREEN_W - 8, y + rowH / 2, it.value, on ? kSelText : kAccent, 3, MR);
    }
    if (n > rows) {                      // scroll bar
      const int trackH = rows * rowH, barH = trackH * rows / n > 8 ? trackH * rows / n : 8;
      const int barY = top + (trackH - barH) * first / (n - rows);
      fill(SCREEN_W - 2, top, 2, trackH, kTitleBg);
      fill(SCREEN_W - 2, barY, 2, barH, kAccent);
    }
    if (footer) text(SCREEN_W / 2, SCREEN_H - 7, footer, kDim, 1, MC);
    present();

    switch (const uint8_t k = waitKey()) {
      case KEY_UP:   *sel = *sel > 0 ? *sel - 1 : n - 1; break;
      case KEY_DOWN: *sel = *sel < n - 1 ? *sel + 1 : 0; break;
      case KEY_PGDN: *sel += rows; break;
      case KEY_HOME: *sel = 0; break;
      case KEY_END:  *sel = n - 1; break;
      case KEY_ENTER: case KEY_LEFT: case KEY_RIGHT: case KEY_ESC: case KEY_BACKSPACE:
        return k;
      default: break;
    }
  }
}

} // namespace ui
