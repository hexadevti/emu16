// hal_sdl.cpp -- the desktop HAL: an SDL2 window showing the PicoCalc's 320x320 panel, the PC
// keyboard mapped onto PicoCalc key codes, SDL audio, and a folder standing in for the SD card.
//
// Everything runs on the main thread: presenting a frame is a texture upload, cheap enough that
// the double-buffered video API simply copies at submit time.
#include "../../../board.h"

#if defined(BOARD_DESKTOP)

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <string>
#include <chrono>
#include "../hal.h"

#ifndef EMU_WINDOW_SCALE
#define EMU_WINDOW_SCALE 2
#endif

static SDL_Window   *s_win = nullptr;
static SDL_Renderer *s_ren = nullptr;
static SDL_Texture  *s_tex = nullptr;
static uint16_t      s_panel[PANEL_W * PANEL_H];
static uint16_t     *s_vbuf[2];
static int           s_back = 0;
static SDL_AudioDeviceID s_audio = 0;
static uint32_t      s_audioRate = 0;
static int32_t       s_gain = 256;
static std::string   s_sdRoot = EMU_DEFAULT_SD_DIR;
static bool          s_ctrl = false, s_shift = false;
static uint32_t      s_lastPresent = 0;
static uint32_t      s_audioWaitUs = 0;

static const auto s_t0 = std::chrono::steady_clock::now();

uint32_t hal_millis() {
  return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s_t0).count();
}
uint32_t hal_micros() {
  return (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - s_t0).count();
}
void hal_delay(uint32_t ms) { hal_yield(); SDL_Delay(ms); }

void hal_log(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  putchar('\n');
  fflush(stdout);
}

void hal_reboot()           { hal_log("reboot requested - exiting"); exit(0); }
void hal_reboot_to_loader() { hal_log("loader requested - exiting"); exit(0); }
void *hal_alloc_big(size_t n)  { return calloc(1, n); }
void *hal_alloc_fast(size_t n) { return calloc(1, n); }
void  hal_free(void *p)        { free(p); }
int   hal_battery_percent()    { return -1; }
bool  hal_battery_charging()   { return false; }
bool        hal_sd_mounted()   { return true; }
const char *hal_sd_root()      { return s_sdRoot.c_str(); }

static void present() {
  SDL_UpdateTexture(s_tex, nullptr, s_panel, PANEL_W * 2);
  SDL_RenderClear(s_ren);
  SDL_RenderCopy(s_ren, s_tex, nullptr, nullptr);
  SDL_RenderPresent(s_ren);
  s_lastPresent = hal_millis();
}

// ------------------------------------------------------------------------------------------------
// keyboard
// ------------------------------------------------------------------------------------------------
static KeyEvent s_q[128];
static int s_qHead = 0, s_qTail = 0;

static void qPush(uint8_t code, uint8_t state) {
  const int n = (s_qTail + 1) % 128;
  if (n == s_qHead) return;
  s_q[s_qTail] = { code, state };
  s_qTail = n;
}

static uint8_t mapKey(const SDL_Keysym &k) {
  switch (k.sym) {
    case SDLK_BACKSPACE: return KEY_BACKSPACE;
    case SDLK_TAB:       return KEY_TAB;
    case SDLK_RETURN: case SDLK_KP_ENTER: return KEY_ENTER;
    case SDLK_ESCAPE:    return KEY_ESC;
    case SDLK_LEFT:      return KEY_LEFT;
    case SDLK_RIGHT:     return KEY_RIGHT;
    case SDLK_UP:        return KEY_UP;
    case SDLK_DOWN:      return KEY_DOWN;
    case SDLK_LCTRL: case SDLK_RCTRL:   return KEY_CTRL;
    case SDLK_LSHIFT:    return KEY_SHIFT_L;
    case SDLK_RSHIFT:    return KEY_SHIFT_R;
    case SDLK_LALT: case SDLK_RALT:     return KEY_ALT;
    case SDLK_CAPSLOCK:  return KEY_CAPS;
    case SDLK_INSERT:    return KEY_INSERT;
    case SDLK_DELETE:    return KEY_DEL;
    case SDLK_HOME:      return KEY_HOME;
    case SDLK_END:       return KEY_END;
    case SDLK_PAGEUP:    return KEY_PGUP;
    case SDLK_PAGEDOWN:  return KEY_PGDN;
    case SDLK_PAUSE:     return KEY_BREAK;
    case SDLK_F1: case SDLK_F2: case SDLK_F3: case SDLK_F4: case SDLK_F5:
      // the PicoCalc sends F6..F10 for Shift+F1..F5
      return (uint8_t)((s_shift ? KEY_F6 : KEY_F1) + (k.sym - SDLK_F1) + (s_shift && k.sym == SDLK_F5 ? 5 : 0));
    default: break;
  }
  if (k.sym >= 32 && k.sym < 127) {
    int c = k.sym;
    if (s_shift && c >= 'a' && c <= 'z') c -= 32;
    return (uint8_t)c;
  }
  return 0;
}

void hal_yield() {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
      case SDL_QUIT: exit(0);
      case SDL_KEYDOWN:
      case SDL_KEYUP: {
        const bool down = e.type == SDL_KEYDOWN;
        if (e.key.keysym.sym == SDLK_LSHIFT || e.key.keysym.sym == SDLK_RSHIFT) s_shift = down;
        if (e.key.keysym.sym == SDLK_LCTRL || e.key.keysym.sym == SDLK_RCTRL) s_ctrl = down;
        const uint8_t c = mapKey(e.key.keysym);
        if (c) qPush(c, down ? (e.key.repeat ? KEYSTATE_HELD : KEYSTATE_PRESSED) : KEYSTATE_RELEASED);
        break;
      }
      case SDL_WINDOWEVENT:
        if (e.window.event == SDL_WINDOWEVENT_EXPOSED) present();
        break;
    }
  }
}

bool hal_key_poll(KeyEvent *ev) {
  hal_yield();
  if (s_qHead == s_qTail) return false;
  *ev = s_q[s_qHead];
  s_qHead = (s_qHead + 1) % 128;
  return true;
}

// ------------------------------------------------------------------------------------------------
// display
// ------------------------------------------------------------------------------------------------
void hal_panel_fill(int x, int y, int w, int h, uint16_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > PANEL_W) w = PANEL_W - x;
  if (y + h > PANEL_H) h = PANEL_H - y;
  if (w <= 0 || h <= 0) return;
  for (int r = 0; r < h; r++)
    for (int i = 0; i < w; i++) s_panel[(y + r) * PANEL_W + x + i] = c;
  present();
}

void hal_display_fill(int x, int y, int w, int h, uint16_t c) {
  if (y < 0) { h += y; y = 0; }
  if (y + h > SCREEN_H) h = SCREEN_H - y;
  hal_panel_fill(x, y + SCREEN_Y0, w, h, c);
}

void hal_display_push(int x, int y, int w, int h, const uint16_t *px, int stride) {
  if (x < 0) { px -= x; w += x; x = 0; }
  if (y < 0) { px -= y * stride; h += y; y = 0; }
  if (x + w > SCREEN_W) w = SCREEN_W - x;
  if (y + h > SCREEN_H) h = SCREEN_H - y;
  if (w <= 0 || h <= 0) return;
  for (int r = 0; r < h; r++)
    memcpy(&s_panel[(SCREEN_Y0 + y + r) * PANEL_W + x], px + r * stride, w * 2);
  present();
}

uint16_t *hal_video_back() { return s_vbuf[s_back]; }

uint16_t *hal_video_submit(int y, int h) {
  if (y < 0) { h += y; y = 0; }
  if (y + h > SCREEN_H) h = SCREEN_H - y;
  if (h > 0) {
    memcpy(&s_panel[(SCREEN_Y0 + y) * PANEL_W], s_vbuf[s_back] + y * SCREEN_W, (size_t)h * SCREEN_W * 2);
    present();
  }
  s_back ^= 1;
  hal_yield();
  // test hook: EMU16_SHOT=N saves the panel to <sd>/shot.bmp after N frames and quits
  static int shotIn = getenv("EMU16_SHOT") ? atoi(getenv("EMU16_SHOT")) : -1;
  if (shotIn > 0 && --shotIn == 0) {
    SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormatFrom(s_panel, PANEL_W, PANEL_H, 16, PANEL_W * 2, SDL_PIXELFORMAT_RGB565);
    const std::string p = s_sdRoot + "/shot.bmp";
    SDL_SaveBMP(sf, p.c_str());
    SDL_FreeSurface(sf);
    hal_log("screenshot saved to %s", p.c_str());
    exit(0);
  }
  return s_vbuf[s_back];
}

void hal_video_wait() {}

// ------------------------------------------------------------------------------------------------
// audio: SDL converts from the core's rate itself; the queue depth paces the emulator
// ------------------------------------------------------------------------------------------------
static const int16_t kVolume[11] = { 0, 16, 32, 52, 76, 104, 136, 172, 212, 256, 320 };

void hal_audio_volume(int v) {
  if (v < 0) v = 0;
  if (v > 10) v = 10;
  s_gain = kVolume[v];
}

void hal_audio_start(uint32_t rate) {
  if (s_audio && rate == s_audioRate) return;
  if (s_audio) SDL_CloseAudioDevice(s_audio);
  SDL_AudioSpec want = {}, got = {};
  want.freq = (int)rate;
  want.format = AUDIO_S16SYS;
  want.channels = 1;
  want.samples = 1024;
  s_audio = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
  s_audioRate = rate;
  if (!s_audio) { hal_log("audio: %s", SDL_GetError()); return; }
  SDL_PauseAudioDevice(s_audio, 0);
}

void hal_audio_write(const int16_t *mono, int n) {
  if (!s_audio) {                        // no audio device: pace on the clock instead
    static uint32_t next = 0;
    const uint32_t now = hal_micros();
    if (!next || (int32_t)(now - next) > 100000) next = now;
    next += (uint32_t)((uint64_t)n * 1000000 / (s_audioRate ? s_audioRate : 44100));
    const uint32_t t0 = hal_micros();
    while ((int32_t)(next - hal_micros()) > 0) { hal_yield(); SDL_Delay(1); }
    s_audioWaitUs += hal_micros() - t0;
    return;
  }
  static int16_t tmp[4096];
  if (n > 4096) n = 4096;
  for (int i = 0; i < n; i++) {
    int32_t y = (mono[i] * s_gain) >> 8;
    tmp[i] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
  }
  // ~3 frames of latency: wait while more than that is queued
  const uint32_t limit = s_audioRate / 20 * 2;
  const uint32_t t0 = hal_micros();
  while (SDL_GetQueuedAudioSize(s_audio) > limit) { hal_yield(); SDL_Delay(1); }
  s_audioWaitUs += hal_micros() - t0;
  SDL_QueueAudio(s_audio, tmp, (Uint32)n * 2);
}

void hal_audio_silence() { if (s_audio) SDL_ClearQueuedAudio(s_audio); }
uint32_t hal_audio_wait_us() { return s_audioWaitUs; }

// ------------------------------------------------------------------------------------------------
void hal_init() {
  if (const char *d = getenv("EMU16_SD")) s_sdRoot = d;
  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    exit(1);
  }
  s_win = SDL_CreateWindow("emu16 - " BOARD_NAME, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           PANEL_W * EMU_WINDOW_SCALE, PANEL_H * EMU_WINDOW_SCALE, SDL_WINDOW_RESIZABLE);
  s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED);
  if (!s_ren) s_ren = SDL_CreateRenderer(s_win, -1, 0);
  SDL_RenderSetLogicalSize(s_ren, PANEL_W, PANEL_H);
  s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, PANEL_W, PANEL_H);
  for (int i = 0; i < 2; i++) s_vbuf[i] = (uint16_t *)calloc(SCREEN_W * SCREEN_H, 2);
  present();
  hal_log("emu16 on " BOARD_NAME ", SD folder: %s", s_sdRoot.c_str());
}

#endif // BOARD_DESKTOP
