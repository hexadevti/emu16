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

// The three settings rows, as shown in both menus.
static void settingItems(ui::Item out[3]) {
  out[0] = { "Volume", nullptr, ui::ICON_VOLUME, nullptr, (int8_t)g_settings.volume, true };
  out[1] = { "Frameskip", kFrameskipNames[g_settings.frameskip], ui::ICON_SKIP, nullptr, -1, true };
  out[2] = { "Show FPS", g_settings.showFps ? "On" : "Off", ui::ICON_CHART, nullptr, -1, true };
}

static void settingsMenu() {
  int sel = 0;
  ui::Screen scr;
  scr.title = "Settings";
  scr.subtitle = "Saved to /emu16/emu16.cfg";
  scr.hints = "<>|Change;Esc|Back";
  for (;;) {
    ui::Item items[3];
    settingItems(items);
    const uint8_t k = ui::list(scr, items, 3, &sel);
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

static std::string gameTitle(const std::string &rom) {
  const size_t s = rom.find_last_of('/');
  std::string base = s == std::string::npos ? rom : rom.substr(s + 1);
  const size_t dot = base.find_last_of('.');
  if (dot != std::string::npos) base.erase(dot);
  return base;
}

static MenuResult gameMenu(const EmuCore &core, const std::string &rom) {
  hal_audio_silence();
  hal_video_wait();
  ui::setBackdrop(hal_video_front());
  const std::string title = gameTitle(rom);
  ui::Screen scr;
  scr.title = title.c_str();
  scr.subtitle = core.name;
  scr.hints = "Enter|Select;<>|Change;Esc|Resume";
  int sel = 0;
  MenuResult result = MenuResult::Resume;
  for (;;) {
    ui::Item items[8];
    items[0] = { "Resume", nullptr, ui::ICON_PLAY };
    items[1] = { "Reset", nullptr, ui::ICON_RESET };
    items[2] = { "Save state", nullptr, ui::ICON_SAVE };
    items[3] = { "Load state", nullptr, ui::ICON_LOAD };
    settingItems(items + 4);
    items[7] = { "Quit game", nullptr, ui::ICON_QUIT };
    const uint8_t k = ui::list(scr, items, 8, &sel);
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
    if (sel == 7) { result = MenuResult::Quit; break; }
  }
  settingsSave();
  ui::clearBackdrop();
  if (result == MenuResult::Resume) hal_panel_fill(0, 0, PANEL_W, PANEL_H, 0);
  return result;
}

// The FPS meter lives in the black band above the game picture, so it never covers the game.
static void showFps(uint32_t fps, uint32_t drawn, int skip, unsigned load) {
  char b[64];
  snprintf(b, sizeof(b), "%u fps  %u drawn  skip %d  cpu %u%%", (unsigned)fps, (unsigned)drawn, skip, load);
  const int y = SCREEN_Y0 - 18;
  ui::fill(0, y, PANEL_W, 14, 0);
  ui::text(PANEL_W / 2, y + 7, b, load > 100 ? ui::kWarn : ui::kGood, 1, ui::MC);
  hal_panel_push(0, y, PANEL_W, 14, ui::canvas() + y * PANEL_W, PANEL_W);
  hal_log("%s", b);
}

static void runGame(const EmuCore &core, const std::string &rom) {
  ui::message("Loading", gameTitle(rom).c_str());
  if (!core.init() || !core.loadRom(rom.c_str())) {
    ui::message("Error", "Cannot load this ROM", 1500);
    core.shutdown();
    return;
  }
  hal_audio_volume(g_settings.volume);
  hal_panel_fill(0, 0, PANEL_W, PANEL_H, 0);

  uint32_t pad = 0;
  int phase = 0, autoSkip = 0, evalCount = 0;
  uint64_t workSum = 0;
  uint32_t fpsT0 = hal_millis(), fpsFrames = 0, fpsDrawn = 0;
  uint64_t fpsWork = 0;

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
    fpsWork += work;
    if (draw) fpsDrawn++;
    if (g_settings.showFps && hal_millis() - fpsT0 >= 1000) {
      const uint32_t ms = hal_millis() - fpsT0;
      // load: emulation time (without waiting for audio) against the real-time budget
      const unsigned load = (unsigned)(fpsWork * core.frameRate() / 10000 / fpsFrames);
      showFps(fpsFrames * 1000 / ms, fpsDrawn * 1000 / ms, skip, load);
      fpsT0 = hal_millis(); fpsFrames = fpsDrawn = 0; fpsWork = 0;
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
    ui::drawBackground();
    ui::message("No SD card", "Insert a card with /roms/md and restart", 0);
    for (;;) ui::waitKey();   // its hotkeys still reach the loader / restart
  }
#if defined(BOARD_DESKTOP)
  if (const char *rom = getenv("EMU16_ROM")) { runGame(*kCores[0], rom); exit(0); }   // test hook
#endif
  static int sel = 0;
  static std::string subs[kNumCores];
  ui::Item items[kNumCores + 2];
  for (int i = 0; i < kNumCores; i++) {
    subs[i] = std::string("ROMs in /roms/") + kCores[i]->shortName;
    items[i] = { kCores[i]->name, nullptr, ui::ICON_PAD, subs[i].c_str() };
  }
  items[kNumCores] = { "Settings", nullptr, ui::ICON_GEAR, "Volume, frameskip, FPS meter" };
#if defined(BOARD_DESKTOP)
  items[kNumCores + 1] = { "Quit", nullptr, ui::ICON_EXIT, "Close the window" };
#else
  items[kNumCores + 1] = { "Exit to loader", nullptr, ui::ICON_EXIT, "Back to the esp32loader menu" };
#endif
  ui::Screen scr;
  scr.title = "emu16";
  scr.subtitle = "16-BIT SYSTEMS";
  scr.hints = "Up/Dn|Move;Enter|Select";
  scr.logo = true;
  if (ui::list(scr, items, kNumCores + 2, &sel) != KEY_ENTER) return;
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
