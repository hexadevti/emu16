// ui.h -- menus: a 320x240 RAM canvas drawn with the emu8 fixed fonts and pushed to the panel
// in one go, plus a generic scrolling list that both the ROM browser and the menus use.
#pragma once

#include <stdint.h>

#define RGB565(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

namespace ui {

constexpr uint16_t kBg      = RGB565(16, 20, 40);
constexpr uint16_t kTitleBg = RGB565(40, 60, 130);
constexpr uint16_t kText    = RGB565(230, 230, 230);
constexpr uint16_t kDim     = RGB565(140, 150, 170);
constexpr uint16_t kSelBg   = RGB565(230, 180, 40);
constexpr uint16_t kSelText = RGB565(10, 10, 10);
constexpr uint16_t kAccent  = RGB565(120, 200, 255);

enum Datum : uint8_t { TL = 0, TC = 1, TR = 2, ML = 3, MC = 4, MR = 5 };

bool begin();
uint16_t *canvas();                    // SCREEN_W x SCREEN_H
void clear(uint16_t c = kBg);
void fill(int x, int y, int w, int h, uint16_t c);
void text(int x, int y, const char *s, uint16_t c, uint8_t font = 1, uint8_t datum = TL);
int  textWidth(const char *s, uint8_t font = 1);
void present();
void message(const char *title, const char *line, uint32_t holdMs = 0);   // centred box

// Next key press (PRESSED or HELD events only), 0 if none arrives within timeoutMs (0 = wait).
// PgUp (Ctrl-Shift-Up) reboots to the loader and F8 (Ctrl-Shift-F3) restarts, from any menu.
uint8_t waitKey(uint32_t timeoutMs = 0);

struct Item {
  const char *label;
  const char *value;                   // drawn right-aligned, may be null
};

// Draws the list and handles Up/Down/PgDn/Home/End. Returns the key that ends the
// interaction: KEY_ENTER, KEY_LEFT, KEY_RIGHT, KEY_ESC or KEY_BACKSPACE; *sel is updated.
uint8_t list(const char *title, const Item *items, int n, int *sel, const char *footer);

} // namespace ui
