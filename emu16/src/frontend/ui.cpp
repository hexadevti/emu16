// ui.cpp -- see ui.h.
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "ui.h"
#include "uitext.h"
#include "../hal/hal.h"

namespace ui {

static uint16_t *s_cv = nullptr;       // what present() pushes
static uint16_t *s_bg = nullptr;       // the background, rebuilt when the backdrop changes
static bool      s_bgValid = false;

// ------------------------------------------------------------------------------------------------
// colour helpers
// ------------------------------------------------------------------------------------------------
static inline uint16_t mix(uint16_t a, uint16_t b, int t) {      // t: 0 = a .. 256 = b
  const int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
  const int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
  return (uint16_t)(((ar + ((br - ar) * t >> 8)) << 11) | ((ag + ((bg - ag) * t >> 8)) << 5) |
                    (ab + ((bb - ab) * t >> 8)));
}

static inline int alpha256(uint8_t a) { return a + (a >> 7); }

static inline bool onPanel(int x, int y) { return (unsigned)x < PANEL_W && (unsigned)y < PANEL_H; }

// ------------------------------------------------------------------------------------------------
bool begin() {
  if (!s_cv) s_cv = (uint16_t *)hal_alloc_big(PANEL_W * PANEL_H * 2);
  if (!s_bg) s_bg = (uint16_t *)hal_alloc_big(PANEL_W * PANEL_H * 2);
  return s_cv && s_bg;
}

uint16_t *canvas() { return s_cv; }

void fill(int x, int y, int w, int h, uint16_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > PANEL_W) w = PANEL_W - x;
  if (y + h > PANEL_H) h = PANEL_H - y;
  for (int r = 0; r < h; r++) {
    uint16_t *p = s_cv + (y + r) * PANEL_W + x;
    for (int i = 0; i < w; i++) p[i] = c;
  }
}

void blend(int x, int y, int w, int h, uint16_t c, uint8_t alpha) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > PANEL_W) w = PANEL_W - x;
  if (y + h > PANEL_H) h = PANEL_H - y;
  const int t = alpha256(alpha);
  for (int r = 0; r < h; r++) {
    uint16_t *p = s_cv + (y + r) * PANEL_W + x;
    for (int i = 0; i < w; i++) p[i] = mix(p[i], c, t);
  }
}

// A rounded rectangle with antialiased corners. col(px, dst) gives the colour of a fully
// covered pixel at column px over the current pixel dst.
template <class Col>
static void roundShape(int x, int y, int w, int h, int r, Col col) {
  if (r * 2 > h) r = h / 2;
  if (r * 2 > w) r = w / 2;
  for (int j = 0; j < h; j++) {
    const int py = y + j;
    if ((unsigned)py >= PANEL_H) continue;
    float inset = 0;
    const int d = j < r ? j : (j >= h - r ? h - 1 - j : -1);
    if (d >= 0) {
      const float dy = r - d - 0.5f;
      inset = r - sqrtf((float)(r * r) - dy * dy);
    }
    const float L = x + inset, R = x + w - inset;
    const int i0 = (int)floorf(L), i1 = (int)ceilf(R);
    uint16_t *row = s_cv + py * PANEL_W;
    for (int px = i0; px < i1; px++) {
      if ((unsigned)px >= PANEL_W) continue;
      float cov = 1;
      if (px < L) cov -= L - px;
      if (px + 1 > R) cov -= px + 1 - R;
      if (cov <= 0.02f) continue;
      const uint16_t c = col(px, row[px]);
      row[px] = cov >= 0.98f ? c : mix(row[px], c, (int)(cov * 256));
    }
  }
}

void fillRound(int x, int y, int w, int h, int r, uint16_t c) {
  roundShape(x, y, w, h, r, [c](int, uint16_t) { return c; });
}

void fillRoundGrad(int x, int y, int w, int h, int r, uint16_t left, uint16_t right) {
  roundShape(x, y, w, h, r, [=](int px, uint16_t) {
    return mix(left, right, w > 1 ? (px - x) * 256 / (w - 1) : 0);
  });
}

static void blendRound(int x, int y, int w, int h, int r, uint16_t c, uint8_t alpha) {
  const int t = alpha256(alpha);
  roundShape(x, y, w, h, r, [=](int, uint16_t dst) { return mix(dst, c, t); });
}

// ------------------------------------------------------------------------------------------------
// text
// ------------------------------------------------------------------------------------------------
int textWidth(const char *s, uint8_t font) { return uiTextWidth(s, font); }

template <class Col>
static void textWith(int x, int y, const char *s, uint8_t font, uint8_t datum, int scale, Col col) {
  int32_t left, top;
  uiTextOrigin(s, x, y, datum, font, left, top);
  if (scale > 1) {                       // re-anchor: the origin was computed for scale 1
    const int w = uiTextWidth(s, font), h = uiFont(font).boxH;
    const int horiz = datum % 3, vert = datum / 3;
    left = horiz == 1 ? x - w * scale / 2 : horiz == 2 ? x - w * scale : x;
    top  = vert == 1 ? y - h * scale / 2 : vert == 2 ? y - h * scale : y;
  }
  uiTextDraw(s, 0, 0, font, [&](int32_t px, int32_t py) {
    for (int dy = 0; dy < scale; dy++)
      for (int dx = 0; dx < scale; dx++) {
        const int X = left + px * scale + dx, Y = top + py * scale + dy;
        if (onPanel(X, Y)) s_cv[Y * PANEL_W + X] = col(X, Y);
      }
  });
}

void text(int x, int y, const char *s, uint16_t c, uint8_t font, uint8_t datum, int scale) {
  textWith(x, y, s, font, datum, scale, [c](int, int) { return c; });
}

// ------------------------------------------------------------------------------------------------
// icons: 12x12, '#' = lit
// ------------------------------------------------------------------------------------------------
static const char *const kIcons[][12] = {
  {},
  { // pad
    "............", "............", "..########..", ".##########.", "##.#####.###", "#...###.#.##",
    "##.#####.###", "############", "####....####", ".##......##.", "............", "............" },
  { // gear
    ".....##.....", "..#.####.#..", ".##########.", "..###..###..", ".###....###.", "####....####",
    "####....####", ".###....###.", "..###..###..", ".##########.", "..#.####.#..", ".....##....." },
  { // exit
    "............", "######......", "#....#......", "#....#...#..", "#....#...##.", "#...########",
    "#...########", "#....#...##.", "#....#...#..", "#....#......", "######......", "............" },
  { // folder
    "............", "............", "####........", "######......", "###########.", "#.........#.",
    "###########.", "###########.", "###########.", "###########.", "............", "............" },
  { // up
    ".....##.....", "....####....", "...######...", "..###..###..", ".###....###.", ".##......##.",
    ".....##.....", ".....##.....", ".....##.....", ".....##.....", ".....##.....", "............" },
  { // cartridge
    "..########..", ".##########.", ".##......##.", ".##......##.", ".##......##.", ".##......##.",
    ".##########.", ".##########.", ".##########.", ".#.#.#.#.##.", ".#.#.#.#.##.", "............" },
  { // play
    "............", "..##........", "..####......", "..######....", "..########..", "..#########.",
    "..#########.", "..########..", "..######....", "..####......", "..##........", "............" },
  { // reset
    "....####.#..", "..##....###.", ".#.....####.", ".#..........", "#...........", "#...........",
    "#...........", "#..........#", ".#........#.", ".#........#.", "..##....##..", "....####...." },
  { // save (floppy)
    "##########..", "#.#....#.##.", "#.#....#..##", "#.######...#", "#..........#", "#.########.#",
    "#.#......#.#", "#.#......#.#", "#.#......#.#", "#.#......#.#", "############", "............" },
  { // load
    ".....##.....", ".....##.....", ".....##.....", ".....##.....", "..########..", "...######...",
    "....####....", ".....##.....", "#..........#", "#..........#", "############", "............" },
  { // volume
    "............", ".....#......", "....##...#..", "...###....#.", "#####..#..#.", "#####...#..#",
    "#####...#..#", "#####..#..#.", "...###....#.", "....##...#..", ".....#......", "............" },
  { // frameskip
    "............", "............", "#.....#.....", "##....##....", "###...###...", "####..####..",
    "####..####..", "###...###...", "##....##....", "#.....#.....", "............", "............" },
  { // chart
    "............", ".........##.", ".........##.", ".....##..##.", ".....##..##.", ".##..##..##.",
    ".##..##..##.", ".##..##..##.", ".##..##..##.", ".##..##..##.", "############", "............" },
  { // quit
    "............", ".##......##.", ".###....###.", "..###..###..", "...######...", "....####....",
    "....####....", "...######...", "..###..###..", ".###....###.", ".##......##.", "............" },
};

void icon(int x, int y, uint8_t id, uint16_t c) {
  if (id == ICON_NONE || id >= sizeof(kIcons) / sizeof(kIcons[0])) return;
  for (int r = 0; r < 12; r++)
    for (int i = 0; i < 12; i++)
      if (kIcons[id][r][i] == '#' && onPanel(x + i, y + r)) s_cv[(y + r) * PANEL_W + x + i] = c;
}

// ------------------------------------------------------------------------------------------------
// background
// ------------------------------------------------------------------------------------------------
static const uint16_t *s_backdrop = nullptr;   // non-null while a game frame is the backdrop
static uint16_t *s_frameCopy = nullptr;

static void buildBackground() {
  // vertical gradient with two soft glows: violet top right, teal bottom left
  for (int y = 0; y < PANEL_H; y++) {
    const uint16_t base = mix(kBgTop, kBgBot, y * 256 / PANEL_H);
    for (int x = 0; x < PANEL_W; x++) {
      uint16_t c = base;
      const float d1 = hypotf((float)(x - 270), (float)(y - 20)) / 200.0f;
      const float d2 = hypotf((float)(x - 30), (float)(y - 310)) / 180.0f;
      if (d1 < 1) c = mix(c, RGB565(78, 34, 130), (int)((1 - d1) * (1 - d1) * 150));
      if (d2 < 1) c = mix(c, RGB565(10, 74, 104), (int)((1 - d2) * (1 - d2) * 150));
      s_bg[y * PANEL_W + x] = c;
    }
  }
  if (s_backdrop) {                     // the paused game, dimmed into the gradient
    for (int y = 0; y < SCREEN_H; y++)
      for (int x = 0; x < SCREEN_W; x++) {
        uint16_t &d = s_bg[(SCREEN_Y0 + y) * PANEL_W + x];
        d = mix(d, s_backdrop[y * SCREEN_W + x], 84);
      }
  }
  s_bgValid = true;
}

void setBackdrop(const uint16_t *frame) {
  if (!s_frameCopy) s_frameCopy = (uint16_t *)hal_alloc_big(SCREEN_W * SCREEN_H * 2);
  if (s_frameCopy && frame) {
    memcpy(s_frameCopy, frame, SCREEN_W * SCREEN_H * 2);
    s_backdrop = s_frameCopy;
  }
  s_bgValid = false;
}

void clearBackdrop() {
  s_backdrop = nullptr;
  s_bgValid = false;
}

void drawBackground() {
  if (!s_bgValid) buildBackground();
  memcpy(s_cv, s_bg, PANEL_W * PANEL_H * 2);
}

void present() { hal_panel_push(0, 0, PANEL_W, PANEL_H, s_cv, PANEL_W); }

// ------------------------------------------------------------------------------------------------
// pieces of a screen
// ------------------------------------------------------------------------------------------------
constexpr int kHeadH = 40, kFootY = PANEL_H - 22;

static void drawBattery() {
  const int pct = hal_battery_percent();
  if (pct < 0) return;
  const int x = PANEL_W - 30, y = 9;
  fillRound(x, y, 20, 11, 2, kDim);
  fill(x + 20, y + 3, 2, 5, kDim);
  fill(x + 1, y + 1, 18, 9, kCard);
  const int w = pct * 16 / 100;
  if (w > 0) fill(x + 2, y + 2, w, 7, pct <= 15 ? kWarn : kGood);
  char b[16];
  snprintf(b, sizeof(b), "%s%d%%", hal_battery_charging() ? "+" : "", pct);
  text(x - 4, y + 6, b, kText, 1, MR);
}

static void drawHeader(const Screen &s) {
  // translucent band, then a cyan-to-magenta rule under it
  for (int x = 0; x < PANEL_W; x++) {
    const uint16_t c = mix(kHeadA, kHeadB, x * 256 / PANEL_W);
    for (int y = 0; y < kHeadH; y++) {
      uint16_t &d = s_cv[y * PANEL_W + x];
      d = mix(d, c, 200);
    }
    const uint16_t rule = mix(kAccent, kAccent2, x * 256 / PANEL_W);
    s_cv[kHeadH * PANEL_W + x] = rule;
    s_cv[(kHeadH + 1) * PANEL_W + x] = mix(rule, kBgTop, 140);
  }
  if (s.logo) {
    textWith(14, 22, s.title, 2, ML, 2, [](int, int) { return (uint16_t)0; });            // shadow
    textWith(12, 20, s.title, 2, ML, 2, [](int, int y) {
      return mix(kAccent, kAccent2, (y - 8) * 256 / 24 < 0 ? 0 : (y - 8) * 256 / 24 > 256 ? 256 : (y - 8) * 256 / 24);
    });
    if (s.subtitle) text(12 + textWidth(s.title, 2) * 2 + 8, 25, s.subtitle, kDim, 1, ML);
  } else {
    char t[40];
    snprintf(t, sizeof(t), "%s", s.title);
    const int maxChars = (PANEL_W - 24 - 60) / 8;
    if ((int)strlen(t) > maxChars) { t[maxChars - 2] = '.'; t[maxChars - 1] = '.'; t[maxChars] = 0; }
    text(13, s.subtitle ? 14 : 21, t, 0, 2, ML);
    text(12, s.subtitle ? 13 : 20, t, kText, 2, ML);
    if (s.subtitle) {
      char sub[56];
      snprintf(sub, sizeof(sub), "%s", s.subtitle);
      const int maxSub = (PANEL_W - 24) / 6;
      if ((int)strlen(sub) > maxSub) {        // keep the end of a long path
        const int n = (int)strlen(sub);
        memmove(sub + 2, sub + n - (maxSub - 2), (size_t)(maxSub - 2) + 1);
        sub[0] = sub[1] = '.';
      }
      text(12, 30, sub, kDim, 1, ML);
    }
  }
  drawBattery();
}

static void drawHints(const char *hints) {
  blend(0, kFootY, PANEL_W, PANEL_H - kFootY, kCard, 170);
  fill(0, kFootY, PANEL_W, 1, kLine);
  if (!hints) return;
  // measure, then draw centred
  struct Pair { char key[12]; char label[24]; } pairs[6];
  int n = 0;
  for (const char *p = hints; *p && n < 6;) {
    const char *bar = strchr(p, '|');
    if (!bar) break;
    const char *semi = strchr(bar, ';');
    const size_t kl = (size_t)(bar - p), ll = semi ? (size_t)(semi - bar - 1) : strlen(bar + 1);
    snprintf(pairs[n].key, sizeof(pairs[n].key), "%.*s", (int)kl, p);
    snprintf(pairs[n].label, sizeof(pairs[n].label), "%.*s", (int)ll, bar + 1);
    n++;
    p = semi ? semi + 1 : bar + 1 + ll;
  }
  int total = 0;
  for (int i = 0; i < n; i++)
    total += textWidth(pairs[i].key) + 8 + 4 + textWidth(pairs[i].label) + (i ? 12 : 0);
  int x = (PANEL_W - total) / 2;
  const int y = kFootY + 5;
  for (int i = 0; i < n; i++) {
    if (i) x += 12;
    const int kw = textWidth(pairs[i].key) + 8;
    fillRound(x, y, kw, 13, 3, kKeycap);
    fill(x + 2, y, kw - 4, 1, mix(kKeycap, kText, 70));
    text(x + kw / 2, y + 7, pairs[i].key, kText, 1, MC);
    x += kw + 4;
    text(x, y + 7, pairs[i].label, kDim, 1, ML);
    x += textWidth(pairs[i].label);
  }
}

// small left/right arrows around an adjustable value
static void arrow(int x, int y, bool right, uint16_t c) {
  for (int i = 0; i < 4; i++) fill(right ? x + i : x + 3 - i, y - 3 + i, 1, 7 - 2 * i, c);
}

static void drawMeter(int right, int cy, int level, bool on) {
  const int segW = 5, gap = 2, w = 10 * segW + 9 * gap, x0 = right - w;
  for (int i = 0; i < 10; i++) {
    const int h = 4 + i / 2 + i % 2, x = x0 + i * (segW + gap);
    const uint16_t lit = mix(kAccent, kAccent2, i * 256 / 9);
    fill(x, cy + 5 - h, segW, h, i < level ? (on ? kSelText : lit) : (on ? mix(kSelB, kSelText, 70) : kLine));
  }
}

// ------------------------------------------------------------------------------------------------
void message(const char *title, const char *line, uint32_t holdMs) {
  const int w = 248, h = 74, x = (PANEL_W - w) / 2, y = (PANEL_H - h) / 2;
  blendRound(x + 4, y + 5, w, h, 8, 0, 130);                 // shadow
  fillRoundGrad(x - 1, y - 1, w + 2, h + 2, 9, kAccent, kAccent2);
  fillRound(x + 1, y + 1, w - 2, h - 2, 7, kCard);
  text(PANEL_W / 2, y + 17, title, kText, 2, MC);
  for (int i = 0; i < 120; i++)
    fill(PANEL_W / 2 - 60 + i, y + 29, 1, 1, mix(kAccent, kAccent2, i * 256 / 120));
  if (line) text(PANEL_W / 2, y + 50, line, kDim, 3, MC);
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

uint8_t list(const Screen &screen, const Item *items, int n, int *sel) {
  bool tall = false;
  for (int i = 0; i < n; i++) tall |= items[i].sub != nullptr;
  const int rowH = tall ? 40 : 24;
  const int cardX = 6, cardY = kHeadH + 8, cardW = PANEL_W - 12;
  const int rows = (kFootY - 6 - cardY - 8) / rowH;
  const int cardH = (n < rows ? (n > 0 ? n : 1) : rows) * rowH + 8;      // shrink to the content
  const int top = cardY + 4;
  int first = 0;
  for (;;) {
    if (n <= 0) *sel = 0;
    else if (*sel < 0) *sel = 0;
    else if (*sel >= n) *sel = n - 1;
    if (*sel < first) first = *sel;
    if (*sel >= first + rows) first = *sel - rows + 1;

    drawBackground();
    drawHeader(screen);
    blendRound(cardX, cardY, cardW, cardH, 10, kCard, 190);

    const bool scroll = n > rows;
    const int rowX = cardX + 4, rowW = cardW - 8 - (scroll ? 8 : 0);
    for (int i = 0; i < rows && first + i < n; i++) {
      const Item &it = items[first + i];
      const int y = top + i * rowH, cy = y + rowH / 2;
      const bool on = first + i == *sel;
      if (on) {
        fillRoundGrad(rowX, y + 1, rowW, rowH - 2, 6, kSelA, kSelB);
        blend(rowX + 5, y + 2, rowW - 10, 1, kSelText, 70);         // gloss
      } else if (i > 0 && first + i - 1 != *sel) {
        fill(rowX + 10, y, rowW - 20, 1, kLine);
      }
      const uint16_t fg = on ? kSelText : kText;
      int lx = rowX + 10;
      if (it.icon) {
        icon(lx, cy - 6, it.icon, on ? kSelText : kAccent);
        lx += 20;
      }

      // value column
      int valueW = 0;
      const int right = rowX + rowW - 10;
      if (it.meter >= 0) {
        drawMeter(right - (it.adjust ? 10 : 0), cy, it.meter, on);
        valueW = 68 + (it.adjust ? 20 : 0);
        if (on && it.adjust) { arrow(right - 88, cy, false, kSelText); arrow(right - 4, cy, true, kSelText); }
      } else if (it.value) {
        const int vw = textWidth(it.value, 3);
        const int vr = right - (it.adjust ? 10 : 0);
        text(vr, cy, it.value, on ? kSelText : kAccent, 3, MR);
        valueW = vw + (it.adjust ? 20 : 0);
        if (on && it.adjust) { arrow(vr - vw - 10, cy, false, kSelText); arrow(right - 4, cy, true, kSelText); }
      }

      // label, cut to fit before the value column
      char buf[72];
      const int maxChars = (right - lx - (valueW ? valueW + 10 : 0)) / 7;
      snprintf(buf, sizeof(buf), "%s", it.label);
      if ((int)strlen(buf) > maxChars && maxChars > 3) { buf[maxChars - 2] = '.'; buf[maxChars - 1] = '.'; buf[maxChars] = 0; }
      if (it.sub) {
        text(lx, cy - 7, buf, fg, 2, ML);
        text(lx, cy + 8, it.sub, on ? mix(kSelText, kSelA, 70) : kDim, 1, ML);
      } else {
        text(lx, cy, buf, fg, 3, ML);
      }
    }
    if (scroll) {
      const int trackY = top + 2, trackH = rows * rowH - 4;
      const int barH = trackH * rows / n > 14 ? trackH * rows / n : 14;
      const int barY = trackY + (trackH - barH) * first / (n - rows);
      fillRound(cardX + cardW - 9, trackY, 4, trackH, 2, kLine);
      fillRound(cardX + cardW - 9, barY, 4, barH, 2, kAccent);
    }
    drawHints(screen.hints);
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
