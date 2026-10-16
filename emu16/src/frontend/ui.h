// ui.h -- menus: a full-panel (320x320) RAM canvas drawn with the emu8 fixed fonts and pushed to
// the panel in one go, plus a generic scrolling list that the home menu, the ROM browser, the
// settings and the in-game menu all use.
//
// Look: a gradient backdrop (or the paused game, dimmed), a header with the title and battery,
// a rounded card holding the rows, a gradient highlight bar on the selected row, 12x12 icons, and
// a footer of keycap hints.
#pragma once

#include <stdint.h>

#define RGB565(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

namespace ui {

constexpr uint16_t kBgTop    = RGB565(14, 18, 46);
constexpr uint16_t kBgBot    = RGB565(28, 10, 44);
constexpr uint16_t kHeadA    = RGB565(26, 34, 84);
constexpr uint16_t kHeadB    = RGB565(54, 22, 86);
constexpr uint16_t kCard     = RGB565(10, 12, 30);
constexpr uint16_t kLine     = RGB565(40, 46, 86);
constexpr uint16_t kText     = RGB565(232, 234, 246);
constexpr uint16_t kDim      = RGB565(132, 140, 182);
constexpr uint16_t kAccent   = RGB565(64, 208, 255);   // cyan
constexpr uint16_t kAccent2  = RGB565(232, 72, 255);   // magenta
constexpr uint16_t kSelA     = RGB565(28, 120, 240);
constexpr uint16_t kSelB     = RGB565(140, 56, 230);
constexpr uint16_t kSelText  = RGB565(255, 255, 255);
constexpr uint16_t kKeycap   = RGB565(46, 52, 98);
constexpr uint16_t kGood     = RGB565(90, 230, 130);
constexpr uint16_t kWarn     = RGB565(255, 96, 96);

enum Datum : uint8_t { TL = 0, TC = 1, TR = 2, ML = 3, MC = 4, MR = 5 };

enum Icon : uint8_t {
  ICON_NONE, ICON_PAD, ICON_GEAR, ICON_EXIT, ICON_FOLDER, ICON_UP, ICON_CART, ICON_PLAY,
  ICON_RESET, ICON_SAVE, ICON_LOAD, ICON_VOLUME, ICON_SKIP, ICON_CHART, ICON_QUIT,
};

bool begin();
uint16_t *canvas();                    // PANEL_W x PANEL_H

// drawing on the canvas (panel coordinates)
void fill(int x, int y, int w, int h, uint16_t c);
void fillRound(int x, int y, int w, int h, int r, uint16_t c);
void fillRoundGrad(int x, int y, int w, int h, int r, uint16_t left, uint16_t right);
void blend(int x, int y, int w, int h, uint16_t c, uint8_t alpha);   // alpha 255 = opaque
void text(int x, int y, const char *s, uint16_t c, uint8_t font = 1, uint8_t datum = TL, int scale = 1);
int  textWidth(const char *s, uint8_t font = 1);
void icon(int x, int y, uint8_t id, uint16_t c);
void present();

// The background every screen starts from: the gradient, or (while set) the paused game frame,
// dimmed, in the middle of the panel. The frame is copied, so the caller may reuse it.
void setBackdrop(const uint16_t *game320x240);
void clearBackdrop();
void drawBackground();

// A centred box over whatever is on the canvas now.
void message(const char *title, const char *line, uint32_t holdMs = 0);

// Next key press (PRESSED or HELD events only), 0 if none arrives within timeoutMs (0 = wait).
// PgUp (Ctrl-Shift-Up) reboots to the loader and F8 (Ctrl-Shift-F3) restarts, from any menu.
uint8_t waitKey(uint32_t timeoutMs = 0);

struct Item {
  const char *label;
  const char *value = nullptr;         // drawn right-aligned, may be null
  uint8_t icon = ICON_NONE;
  const char *sub = nullptr;           // second line (makes the row taller)
  int8_t meter = -1;                   // 0..10: a level meter instead of the value
  bool adjust = false;                 // Left/Right change it: arrows around the value when selected
};

struct Screen {
  const char *title;
  const char *subtitle = nullptr;
  const char *hints = nullptr;         // "Enter|Select;Esc|Back": keycap, label pairs
  bool logo = false;                   // draw the title as the big emu16 logo
};

// Draws the list and handles Up/Down/PgDn/Home/End. Returns the key that ends the
// interaction: KEY_ENTER, KEY_LEFT, KEY_RIGHT, KEY_ESC or KEY_BACKSPACE; *sel is updated.
uint8_t list(const Screen &screen, const Item *items, int n, int *sel);

} // namespace ui
