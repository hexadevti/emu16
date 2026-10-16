// core.h -- what a system emulator (a "core") offers the frontend. The frontend owns the screen,
// keyboard, menus and files; a core owns its machine and draws/plays through the HAL.
#pragma once

#include <stdint.h>
#include <stdio.h>

// Generic pad, the union of what the 16-bit systems need. Cores map these onto their own buttons.
enum : uint32_t {
  PAD_BTN_UP = 1u << 0, PAD_BTN_DOWN = 1u << 1, PAD_BTN_LEFT = 1u << 2, PAD_BTN_RIGHT = 1u << 3,
  PAD_BTN_A  = 1u << 4, PAD_BTN_B    = 1u << 5, PAD_BTN_C    = 1u << 6,
  PAD_BTN_X  = 1u << 7, PAD_BTN_Y    = 1u << 8, PAD_BTN_Z    = 1u << 9,
  PAD_BTN_START = 1u << 10, PAD_BTN_MODE = 1u << 11,
};

struct EmuCore {
  const char *name;           // "Mega Drive / Genesis"
  const char *shortName;      // "md": ROM folder /roms/<shortName>, state/save file prefixes
  const char *romExts;        // ".md.gen.bin.smd"

  bool (*init)();                              // allocate the machine; false = out of memory
  bool (*loadRom)(const char *path);           // full path; powers the machine on
  void (*reset)();
  // One video frame. draw = false skips rendering (frameskip); audio is always produced and
  // written with hal_audio_write(), which is what paces the frame.
  void (*runFrame)(bool draw);
  void (*setInput)(int port, uint32_t buttons);   // PAD_BTN_* bits held
  uint32_t (*frameRate)();                     // 60 / 50
  bool (*saveState)(FILE *f);
  bool (*loadState)(FILE *f);
  void (*shutdown)();                          // free everything init()/loadRom() took
};

extern const EmuCore *const kCores[];
extern const int kNumCores;
