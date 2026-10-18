// hal.h -- everything the frontend and the cores need from the machine. Two implementations:
// src/hal/picocalc (the P4 module in the PicoCalc) and src/hal/desktop (SDL2).
//
// Coordinates are LOGICAL 320x240 (SCREEN_W x SCREEN_H) unless a name says "panel". Pixels are
// native-endian RGB565.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "../../board.h"

// ---------------------------------------------------------------------------------------------
// keys: the PicoCalc keyboard's own codes are the key space on every target (the desktop maps
// SDL keys onto them). Printable keys arrive as their ASCII code.
// ---------------------------------------------------------------------------------------------
enum : uint8_t {
  KEY_BACKSPACE = 0x08, KEY_TAB = 0x09, KEY_ENTER = 0x0A, KEY_SPACE = 0x20,
  KEY_F1 = 0x81, KEY_F2, KEY_F3, KEY_F4, KEY_F5,
  KEY_F6 = 0x86, KEY_F7, KEY_F8, KEY_F9, KEY_F10 = 0x90,
  KEY_ALT = 0xA1, KEY_SHIFT_L = 0xA2, KEY_SHIFT_R = 0xA3, KEY_SYM = 0xA4, KEY_CTRL = 0xA5,
  KEY_ESC = 0xB1, KEY_LEFT = 0xB4, KEY_UP = 0xB5, KEY_DOWN = 0xB6, KEY_RIGHT = 0xB7,
  KEY_CAPS = 0xC1,
  KEY_BREAK = 0xD0, KEY_INSERT = 0xD1, KEY_HOME = 0xD2, KEY_DEL = 0xD4, KEY_END = 0xD5,
  KEY_PGUP = 0xD6, KEY_PGDN = 0xD7,
  KEY_RELEASE_ALL = 0x00,   // with KEYSTATE_RELEASED: an event may have been lost, treat every key as up
};

enum KeyState : uint8_t { KEYSTATE_PRESSED = 1, KEYSTATE_HELD = 2, KEYSTATE_RELEASED = 3 };

struct KeyEvent {
  uint8_t code;
  uint8_t state;     // KeyState
};

// ---------------------------------------------------------------------------------------------
// lifecycle / system
// ---------------------------------------------------------------------------------------------
void     hal_init();                       // everything below is usable afterwards
uint32_t hal_millis();
uint32_t hal_micros();
uint32_t hal_cycles();                     // fast free-running counter for profiling (wraps)
uint32_t hal_cycles_per_us();
// Work run on the other core (P4: a task on core 0; desktop: a thread), one job at a time;
// hal_job_start waits for the previous job first. The job must not touch the display or input.
void     hal_job_start(void (*fn)(void *), void *arg);
void     hal_job_wait();                   // until the last job has returned
void     hal_delay(uint32_t ms);
void     hal_yield();                      // let other tasks / the SDL event loop run
void     hal_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void     hal_reboot();
void     hal_reboot_to_loader();           // esp32loader menu on the P4; quit on the desktop
// A ROM to start at boot, asked for over the P4 console ("@run <path>"); false on the desktop.
bool     hal_take_launch(char *path, size_t n);
// Crash diagnostics. A breadcrumb notes what the program is doing in RAM that survives a crash
// reboot; after one, hal_crash_report describes it (reset reason, where the CPU stopped, the
// last breadcrumb) and returns false on a normal boot. Both are no-ops on the desktop.
void     hal_breadcrumb(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
bool     hal_crash_report(char *buf, size_t n);
void    *hal_alloc_big(size_t n);          // PSRAM on the P4 (zeroed)
void    *hal_alloc_fast(size_t n);         // internal RAM if it fits, else PSRAM (zeroed)
void     hal_free(void *p);
int      hal_battery_percent();            // -1 = unknown
bool     hal_battery_charging();

// ---------------------------------------------------------------------------------------------
// display
// ---------------------------------------------------------------------------------------------
// Synchronous rectangle push (UI). Waits for a pending hal_video_submit() first.
void hal_display_push(int x, int y, int w, int h, const uint16_t *px, int stride);
void hal_display_fill(int x, int y, int w, int h, uint16_t color);
void hal_panel_fill(int x, int y, int w, int h, uint16_t color);   // raw panel coordinates
void hal_panel_push(int x, int y, int w, int h, const uint16_t *px, int stride);   // same

// Game video, double buffered. The core draws a whole frame into hal_video_back() (320x240,
// stride 320), then hal_video_submit() hands it to the display and returns the other buffer.
// Only rows [y, y+h) are sent. submit() waits if the previous frame is still being sent.
uint16_t *hal_video_back();
uint16_t *hal_video_submit(int y, int h);
void      hal_video_wait();                // until the last submitted frame is on the panel
const uint16_t *hal_video_front();        // the last submitted frame (the in-game menu backdrop)

// ---------------------------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------------------------
bool hal_key_poll(KeyEvent *ev);           // next queued key event, false when none

// ---------------------------------------------------------------------------------------------
// audio: mono signed 16-bit at a core-chosen rate, resampled to the output. Writes block while
// the output buffer is full, which is what paces emulation at real speed.
// ---------------------------------------------------------------------------------------------
void hal_audio_start(uint32_t rate);
void hal_audio_write(const int16_t *mono, int n);
void hal_audio_volume(int vol);            // 0..10
void hal_audio_silence();                  // drop anything queued (menus, pauses)
// Total time the calling thread spent blocked in hal_audio_write(), plus, outside a job, in
// hal_job_start()/hal_job_wait(): a job that writes audio paces whoever waits for it.
uint32_t hal_audio_wait_us();

// ---------------------------------------------------------------------------------------------
// storage: plain stdio/dirent below this root
// ---------------------------------------------------------------------------------------------
bool        hal_sd_mounted();
const char *hal_sd_root();                 // "/sd" on the P4, the sdcard folder on the desktop
