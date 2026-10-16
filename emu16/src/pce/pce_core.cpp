// pce_core.cpp -- the PC Engine / TurboGrafx-16 core: pce-go (HuC6280 + VDC/VCE + PSG, from
// retro-go) driven one scanline at a time, so the PSG is sampled in step with the CPU. pce-go
// draws 8-bit palette indices into its own line buffer; each drawn frame is converted to RGB565
// and scaled to the 320-wide HAL video buffer. HuCards only: no CD-ROM, no SuperGrafx.
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "../../core.h"
#include "../hal/hal.h"

extern "C" {
#include "pcego/pce-go.h"
#include "pcego/pce.h"
#include "pcego/psg.h"
}

#define PCE_AUDIO_RATE  22080                       // 368 samples a frame; pce-go's DDA timing assumes ~22 kHz
#define PCE_FRAME_SAMPLES (PCE_AUDIO_RATE / 60)
#define PCE_MAX_ROM     (8u << 20)
#define PCE_MAX_ROWS    224                         // taller modes are cropped to the TV-safe middle

static uint8_t  *s_rom = nullptr;                  // the HuCard image, header included
static uint8_t  *s_xbuf = nullptr;                 // pce-go's 8-bit line buffer, with a spare row each side
static uint8_t  *s_xdata = nullptr;                // its first visible pixel
static bool      s_draw = true;
static uint16_t  s_pal[256];
static int16_t   s_stereo[PCE_FRAME_SAMPLES * 2];
static int16_t   s_mono[PCE_FRAME_SAMPLES];
static uint16_t  s_xmap[SCREEN_W];                 // screen column -> line buffer column
static int       s_xmapWidth = -1;

// ------------------------------------------------------------------------------------------------
// what pce-go asks of its frontend
// ------------------------------------------------------------------------------------------------
extern "C" {

void osd_log(const char *fmt, ...) {
  char b[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  size_t n = strlen(b);
  while (n && (b[n - 1] == '\n' || b[n - 1] == '\r')) b[--n] = 0;
  hal_log("pce: %s", b);
}

void *osd_alloc_fast(size_t n) { return hal_alloc_fast(n); }
void  osd_free(void *p) { hal_free(p); }

uint32_t crc32_le(uint32_t crc, const uint8_t *buf, size_t len) {
  crc = ~crc;
  while (len--) {
    crc ^= *buf++;
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
  }
  return ~crc;
}

// NULL tells pce-go to skip rendering (frameskip)
uint8_t *osd_gfx_framebuffer(int, int) { return s_draw ? s_xdata : nullptr; }

}  // extern "C"

// ------------------------------------------------------------------------------------------------
static bool pcec_init() {
  if (InitPCE(PCE_AUDIO_RATE, true) != 0) return false;
  const size_t xbufSize = (size_t)XBUF_WIDTH * (XBUF_HEIGHT + 2);
  if (!s_xbuf) s_xbuf = (uint8_t *)hal_alloc_fast(xbufSize);
  if (!s_xbuf) return false;
  s_xdata = s_xbuf + XBUF_WIDTH;
  PalettePCE565(s_pal);
  s_xmapWidth = -1;
  return true;
}

static void pcec_shutdown() {
  ShutdownPCE();
  hal_free(s_rom);
  s_rom = nullptr;
  hal_free(s_xbuf);
  s_xbuf = s_xdata = nullptr;
}

static bool pcec_loadRom(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { hal_log("pce: cannot open %s", path); return false; }
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size < 0x2000 || (unsigned long)size > PCE_MAX_ROM + 512) {
    fclose(f);
    hal_log("pce: bad ROM size %ld", size);
    return false;
  }
  // 16 spare bytes: 16-bit operand reads at the very end of the last bank
  uint8_t *rom = (uint8_t *)hal_alloc_big((size_t)size + 16);
  if (!rom) { fclose(f); hal_log("pce: out of memory for the ROM"); return false; }
  const long got = (long)fread(rom, 1, (size_t)size, f);
  fclose(f);
  if (got != size) { hal_free(rom); hal_log("pce: short read (%ld of %ld)", got, size); return false; }

  if (LoadCard(rom, (size_t)size) != 0) { hal_free(rom); return false; }
  hal_free(s_rom);                                 // the previous game's, no longer mapped
  s_rom = rom;
  ResetPCE(true);
  memset(PCE.Joypad.regs, 0, sizeof(PCE.Joypad.regs));
  hal_audio_start(PCE_AUDIO_RATE);
  hal_log("pce: %s, %ld KB, CRC %08X", path, size >> 10, (unsigned)PCE.ROM_CRC);
  return true;
}

static void pcec_reset() { ResetPCE(true); }

static uint32_t pcec_frameRate() { return 60; }

// Pad: II on the left button (A), I on the right (B); Select on C and on Mode.
static void pcec_setInput(int port, uint32_t b) {
  if (port < 0 || port > 4) return;
  uint8_t s = 0;
  if (b & PAD_BTN_UP)    s |= JOY_UP;
  if (b & PAD_BTN_DOWN)  s |= JOY_DOWN;
  if (b & PAD_BTN_LEFT)  s |= JOY_LEFT;
  if (b & PAD_BTN_RIGHT) s |= JOY_RIGHT;
  if (b & PAD_BTN_A)     s |= JOY_B;
  if (b & PAD_BTN_B)     s |= JOY_A;
  if (b & (PAD_BTN_C | PAD_BTN_MODE)) s |= JOY_SELECT;
  if (b & PAD_BTN_START) s |= JOY_RUN;
  PCE.Joypad.regs[port] = s;
}

// The line buffer to the 320x240 screen: widths are scaled to 320 (256 -> 5:4), heights centred.
// Games that set 240 lines leave junk in the overscan a TV hides (Strip Fighter II shows its
// score bar again at the bottom), so at most PCE_MAX_ROWS of the middle are shown.
static void blitFrame() {
  const int w = (int)PCE.VDC.screen_width;
  int h = (int)PCE.VDC.screen_height;
  uint16_t *fb = hal_video_back();
  if (w <= 0 || h <= 0) {                          // not set up yet: a black screen
    memset(fb, 0, (size_t)SCREEN_W * SCREEN_H * 2);
    hal_video_submit(0, SCREEN_H);
    return;
  }
  if (h > XBUF_HEIGHT) h = XBUF_HEIGHT;
  if (w != s_xmapWidth) {
    for (int x = 0; x < SCREEN_W; x++) s_xmap[x] = (uint16_t)(x * w / SCREEN_W);
    s_xmapWidth = w;
  }
  const int rows = h > PCE_MAX_ROWS ? PCE_MAX_ROWS : h;
  const int skipRows = (h - rows) / 2;
  const int y0 = (SCREEN_H - rows) / 2;

  if (rows < SCREEN_H) {                           // borders: shorter modes leave stale rows
    memset(fb, 0, (size_t)y0 * SCREEN_W * 2);
    memset(fb + (y0 + rows) * SCREEN_W, 0, (size_t)(SCREEN_H - y0 - rows) * SCREEN_W * 2);
  }
  for (int y = 0; y < rows; y++) {
    const uint8_t *src = s_xdata + (size_t)(y + skipRows) * XBUF_WIDTH;
    uint16_t *dst = fb + (y0 + y) * SCREEN_W;
    if (w == SCREEN_W) {
      for (int x = 0; x < SCREEN_W; x++) dst[x] = s_pal[src[x]];
    } else {
      for (int x = 0; x < SCREEN_W; x++) dst[x] = s_pal[src[s_xmap[x]]];
    }
  }
  hal_video_submit(0, SCREEN_H);
}

static void pcec_runFrame(bool draw) {
  s_draw = draw;
  pce_frame_begin();

  // run line by line, catching the PSG up every few lines so DDA (sample) playback keeps its timing
  int made = 0;
  for (int line = 0; line < PCE_LINES; line++) {
    pce_run_line();
    const int due = (line + 1) * PCE_FRAME_SAMPLES / PCE_LINES;
    if (due - made >= 16 || line == PCE_LINES - 1) {
      psg_update(s_stereo + made * 2, (size_t)(due - made), 0xFF);
      made = due;
    }
  }

  if (draw) blitFrame();

  for (int i = 0; i < PCE_FRAME_SAMPLES; i++) s_mono[i] = (int16_t)((s_stereo[i * 2] + s_stereo[i * 2 + 1]) / 2);
  hal_audio_write(s_mono, PCE_FRAME_SAMPLES);
}

static bool pcec_saveState(FILE *f) { return SaveState(f) == 0; }
static bool pcec_loadState(FILE *f) { return LoadState(f) == 0; }

extern const EmuCore kPcEngine;
const EmuCore kPcEngine = {
  "PC Engine / TurboGrafx-16", "pce", ".pce",
  pcec_init, pcec_loadRom, pcec_reset, pcec_runFrame, pcec_setInput, pcec_frameRate,
  pcec_saveState, pcec_loadState, pcec_shutdown,
};
