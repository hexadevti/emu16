// app.cpp -- the frontend: home menu (systems, settings, loader), ROM browser, the game loop with
// keyboard-to-pad mapping and frameskip, and the in-game menu.
//
// Keys in game:   arrows = d-pad, Z/J = A, X/K = B, C/L = C, Enter = Start, Esc/F1 = menu,
//                 F5 = quick save, F7 = quick load
// Anywhere:       Ctrl-Shift-Up (arrives as PgUp in game) = esp32loader, F8 (Ctrl-Shift-F3) = restart
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <string>
#include "app.h"
#include "ui.h"
#include "browser.h"
#include "settings.h"
#include "../hal/hal.h"
#include "../../core.h"

extern const EmuCore kMegaDrive;
const EmuCore *const kCores[] = { &kMegaDrive };
const int kNumCores = sizeof(kCores) / sizeof(kCores[0]);

static const char *const kFrameskipNames[] = { "Auto", "1 of 2", "1 of 3", "1 of 4" };

// ------------------------------------------------------------------------------------------------
// keyboard -> pad
// ------------------------------------------------------------------------------------------------
static uint32_t padBit(uint8_t code) {
  switch (code) {
    case KEY_UP:    return PAD_BTN_UP;
    case KEY_DOWN:  return PAD_BTN_DOWN;
    case KEY_LEFT:  return PAD_BTN_LEFT;
    case KEY_RIGHT: return PAD_BTN_RIGHT;
    case KEY_ENTER: return PAD_BTN_START;
    default: break;
  }
  switch (tolower(code)) {
    case 'z': case 'j': return PAD_BTN_A;
    case 'x': case 'k': return PAD_BTN_B;
    case 'c': case 'l': return PAD_BTN_C;
    case 'a': case 'u': return PAD_BTN_X;
    case 's': case 'i': return PAD_BTN_Y;
    case 'd': case 'o': return PAD_BTN_Z;
    case ' ':           return PAD_BTN_MODE;
    default: return 0;
  }
}

// ------------------------------------------------------------------------------------------------
// settings rows shared by the home and in-game menus
// ------------------------------------------------------------------------------------------------
// Returns true when the key changed the row's setting.
static bool adjustSetting(int row, uint8_t key) {
  const int d = key == KEY_LEFT ? -1 : 1;
  switch (row) {
    case 0:
      g_settings.volume += d;
      if (g_settings.volume < 0) g_settings.volume = 0;
      if (g_settings.volume > 10) g_settings.volume = 10;
      hal_audio_volume(g_settings.volume);
      return true;
    case 1: g_settings.frameskip = (g_settings.frameskip + d + 4) % 4; return true;
    case 2: g_settings.showFps = !g_settings.showFps; return true;
  }
  return false;
}

static void settingValues(char vol[8], const char **fs, const char **fps) {
  snprintf(vol, 8, "%d", g_settings.volume);
  *fs = kFrameskipNames[g_settings.frameskip];
  *fps = g_settings.showFps ? "On" : "Off";
}

static void settingsMenu() {
  int sel = 0;
  for (;;) {
    char vol[8]; const char *fs, *fps;
    settingValues(vol, &fs, &fps);
    const ui::Item items[] = { { "Volume", vol }, { "Frameskip", fs }, { "Log FPS", fps } };
    const uint8_t k = ui::list("Settings", items, 3, &sel, "Left/Right: change   Esc: back");
    if (k == KEY_ESC || k == KEY_BACKSPACE) break;
    adjustSetting(sel, k == KEY_ENTER ? KEY_RIGHT : k);
  }
  settingsSave();
}

// ------------------------------------------------------------------------------------------------
// game
// ------------------------------------------------------------------------------------------------
static std::string statePath(const std::string &rom) {
  const size_t s = rom.find_last_of('/');
  std::string base = s == std::string::npos ? rom : rom.substr(s + 1);
  const size_t dot = base.find_last_of('.');
  if (dot != std::string::npos) base.erase(dot);
  return sdPath("emu16/states/") + base + ".st0";
}

static bool saveState(const EmuCore &core, const std::string &rom) {
  makeDirs(sdPath("emu16/states"));
  const std::string p = statePath(rom);
  FILE *f = fopen(p.c_str(), "wb");
  if (!f) return false;
  const bool ok = core.saveState(f);
  fclose(f);
  hal_log("state %s %s", ok ? "saved to" : "FAILED", p.c_str());
  return ok;
}

static bool loadState(const EmuCore &core, const std::string &rom) {
  FILE *f = fopen(statePath(rom).c_str(), "rb");
  if (!f) return false;
  const bool ok = core.loadState(f);
  fclose(f);
  return ok;
}

enum class MenuResult { Resume, Quit };

static MenuResult gameMenu(const EmuCore &core, const std::string &rom) {
  hal_audio_silence();
  hal_video_wait();
  int sel = 0;
  for (;;) {
    char vol[8]; const char *fs, *fps;
    settingValues(vol, &fs, &fps);
    const ui::Item items[] = {
      { "Resume", nullptr }, { "Reset", nullptr }, { "Save state", nullptr }, { "Load state", nullptr },
      { "Volume", vol }, { "Frameskip", fs }, { "Log FPS", fps }, { "Quit game", nullptr },
    };
    const uint8_t k = ui::list(core.name, items, 8, &sel, "Enter: select   Esc: resume");
    if (k == KEY_ESC) break;
    if (sel >= 4 && sel <= 6) {
      if (k == KEY_LEFT || k == KEY_RIGHT || k == KEY_ENTER) adjustSetting(sel - 4, k == KEY_ENTER ? KEY_RIGHT : k);
      continue;
    }
    if (k != KEY_ENTER) continue;
    if (sel == 0) break;
    if (sel == 1) { core.reset(); break; }
    if (sel == 2) { ui::message("Save state", saveState(core, rom) ? "Saved" : "Save failed", 600); break; }
    if (sel == 3) {
      if (loadState(core, rom)) break;
      ui::message("Load state", "No state for this game", 900);
      continue;
    }
    if (sel == 7) { settingsSave(); return MenuResult::Quit; }
  }
  settingsSave();
  hal_display_fill(0, 0, SCREEN_W, SCREEN_H, 0);
  return MenuResult::Resume;
}

static void runGame(const EmuCore &core, const std::string &rom) {
  ui::message(core.name, "Loading...");
  if (!core.init() || !core.loadRom(rom.c_str())) {
    ui::message("Error", "Cannot load this ROM", 1500);
    core.shutdown();
    return;
  }
  hal_audio_volume(g_settings.volume);
  hal_display_fill(0, 0, SCREEN_W, SCREEN_H, 0);

  uint32_t pad = 0;
  int phase = 0, autoSkip = 0, evalCount = 0;
  uint64_t workSum = 0;
  uint32_t fpsT0 = hal_millis(), fpsFrames = 0, fpsDrawn = 0;

  for (;;) {
    // input
    KeyEvent ev;
    bool menu = false;
    while (hal_key_poll(&ev)) {
      const bool down = ev.state != KEYSTATE_RELEASED;
      if (ev.state == KEYSTATE_PRESSED) {
        if (ev.code == KEY_ESC || ev.code == KEY_F1) { menu = true; continue; }
        if (ev.code == KEY_PGUP) hal_reboot_to_loader();
        if (ev.code == KEY_F8) hal_reboot();
        if (ev.code == KEY_F5) { saveState(core, rom); continue; }
        if (ev.code == KEY_F7) { loadState(core, rom); continue; }
      }
      const uint32_t b = padBit(ev.code);
      if (down) pad |= b; else pad &= ~b;
    }
    if (menu) {
      pad = 0;
      core.setInput(0, 0);
      if (gameMenu(core, rom) == MenuResult::Quit) break;
      phase = autoSkip = evalCount = 0; workSum = 0;
      continue;
    }
    core.setInput(0, pad);

    // frameskip: draw 1 frame in skip+1
    const int skip = g_settings.frameskip ? g_settings.frameskip : autoSkip;
    const bool draw = phase == 0;
    if (++phase > skip) phase = 0;

    const uint32_t t0 = hal_micros(), w0 = hal_audio_wait_us();
    core.runFrame(draw);
    const uint32_t work = (hal_micros() - t0) - (hal_audio_wait_us() - w0);

    // auto: every second, compare the average work per frame to the frame period
    workSum += work;
    if (++evalCount >= (int)core.frameRate()) {
      const uint32_t avg = (uint32_t)(workSum / evalCount), period = 1000000 / core.frameRate();
      if (avg > period * 95 / 100 && autoSkip < 3) autoSkip++;
      else if (avg < period * 60 / 100 && autoSkip > 0) autoSkip--;
      evalCount = 0; workSum = 0;
    }

    fpsFrames++;
    if (draw) fpsDrawn++;
    if (g_settings.showFps && hal_millis() - fpsT0 >= 5000) {
      const uint32_t ms = hal_millis() - fpsT0;
      hal_log("fps: %u emulated, %u drawn, skip %d", (unsigned)(fpsFrames * 1000 / ms),
              (unsigned)(fpsDrawn * 1000 / ms), skip);
      fpsT0 = hal_millis(); fpsFrames = fpsDrawn = 0;
    }
  }
  hal_audio_silence();
  core.shutdown();
}

// ------------------------------------------------------------------------------------------------
void app_setup() {
  hal_init();
  if (!ui::begin()) { hal_log("out of memory for the UI canvas"); for (;;) hal_delay(1000); }
  hal_panel_fill(0, 0, PANEL_W, PANEL_H, 0);
  if (hal_sd_mounted()) settingsLoad();
  hal_audio_volume(g_settings.volume);
}

void app_loop() {
  if (!hal_sd_mounted()) {
    ui::message("No SD card", "Insert a card with /roms/md and restart", 0);
    for (;;) ui::waitKey();   // its hotkeys still reach the loader / restart
  }
#if defined(BOARD_DESKTOP)
  if (const char *rom = getenv("EMU16_ROM")) { runGame(*kCores[0], rom); exit(0); }   // test hook
#endif
  static int sel = 0;
  ui::Item items[kNumCores + 2];
  for (int i = 0; i < kNumCores; i++) items[i] = { kCores[i]->name, nullptr };
  items[kNumCores] = { "Settings", nullptr };
  items[kNumCores + 1] = { "Exit to loader", nullptr };
  if (ui::list("emu16", items, kNumCores + 2, &sel, BOARD_NAME) != KEY_ENTER) return;
  if (sel < kNumCores) {
    std::string rom;
    while (browseRom(*kCores[sel], rom)) {
      settingsSave();
      runGame(*kCores[sel], rom);
    }
  } else if (sel == kNumCores) {
    settingsMenu();
  } else {
    hal_reboot_to_loader();
  }
}
