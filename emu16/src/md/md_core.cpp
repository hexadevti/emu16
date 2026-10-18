// md_core.cpp -- the Mega Drive / Genesis core: Gwenesis (68000 + Z80 + VDP + YM2612 + SN76489)
// driven one frame at a time, the way bzhxx's Game & Watch port drives it
// (gnw_main_gwenesis.c), drawing into the HAL's video back buffer and writing the mixed sound
// to the HAL audio stream.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <string>
#include "../../core.h"
#include "../hal/hal.h"

extern "C" {
#include "../cpu/m68k/m68k.h"
#include "gwenesis/z80inst.h"
#include "gwenesis/gwenesis_bus.h"
#include "gwenesis/gwenesis_io.h"
#include "gwenesis/gwenesis_vdp.h"
#include "gwenesis/gwenesis_savestate.h"
#include "../sound/ym2612.h"
#include "../sound/gwenesis_sn76489.h"
}

// ------------------------------------------------------------------------------------------------
// globals Gwenesis expects from its frontend
// ------------------------------------------------------------------------------------------------
#define MD_AUDIO_BUF 1200      // one frame of samples: 888 NTSC, ~1061 PAL (Gwenesis' 1056 is short)

extern "C" {
void *md_alloc_fast(size_t n) { return hal_alloc_fast(n); }   // ym2612.c's tables
void  md_free(void *p)        { hal_free(p); }
int system_clock;
int16_t gwenesis_sn76489_buffer[MD_AUDIO_BUF];
int sn76489_index;
int sn76489_clock;
int16_t gwenesis_ym2612_buffer[MD_AUDIO_BUF];
int ym2612_index;
int ym2612_clock;
int frame_counter;
unsigned int lines_per_frame = LINES_PER_FRAME_NTSC;
unsigned int scan_line;

extern unsigned char button_state[3];
extern unsigned char io_reg[16];
extern unsigned char gwenesis_vdp_regs[0x20];
extern unsigned short gwenesis_vdp_status;
extern int screen_width, screen_height;
extern int hint_pending;
extern int gwenesis_H32upscaler;

// The pads are latched by md_setInput(); the core asks on every pad read.
void gwenesis_io_get_buttons() {}
}

static uint8_t *s_rom = nullptr;
static int16_t  s_mix[MD_AUDIO_BUF];
static bool     s_pal = false;

// cartridge SRAM, saved to <rom>.srm about a second after the game stops writing to it
static uint8_t    *s_sram = nullptr;
static uint32_t    s_sramSize = 0;
static std::string s_sramPath;
static bool        s_sramPending = false;
static int         s_sramQuiet = 0;

static void sramFlush() {
  if (!s_sram || (!s_sramPending && !gwenesis_sram_dirty)) return;
  if (FILE *f = fopen(s_sramPath.c_str(), "wb")) {
    fwrite(s_sram, 1, s_sramSize, f);
    fclose(f);
    hal_log("md: SRAM saved to %s", s_sramPath.c_str());
  } else {
    hal_log("md: cannot write %s", s_sramPath.c_str());
  }
  s_sramPending = false;
  gwenesis_sram_dirty = 0;
}

static void sramTick() {
  if (gwenesis_sram_dirty) { gwenesis_sram_dirty = 0; s_sramPending = true; s_sramQuiet = 0; return; }
  if (s_sramPending && ++s_sramQuiet >= 60) sramFlush();
}

// The header's "RA" block declares the SRAM range (GPGX's rules: at most 64 KB, odd end).
// Read from the file image, before Gwenesis byte-swaps the ROM.
static void sramSetup(const char *romPath, long romSize) {
  hal_free(s_sram);
  s_sram = nullptr;
  s_sramSize = 0;
  s_sramPending = false;
  gwenesis_sram_setup(nullptr, 0, 0, 0);
  if (romSize < 0x1BC || s_rom[0x1B0] != 'R' || s_rom[0x1B1] != 'A') return;
  auto be32 = [](const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; };
  uint32_t start = be32(s_rom + 0x1B4), end = be32(s_rom + 0x1B8);
  if (start > end || end - start >= 0x10000) end = start + 0xFFFF;
  start &= 0xFFFFFE;
  end |= 1;
  if (start >= 0x800000 || end >= 0x800000) return;
  s_sramSize = end - start + 1;
  s_sram = (uint8_t *)hal_alloc_big(s_sramSize);
  if (!s_sram) return;
  memset(s_sram, 0xFF, s_sramSize);

  s_sramPath = romPath;
  const size_t dot = s_sramPath.find_last_of('.'), slash = s_sramPath.find_last_of('/');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) s_sramPath.erase(dot);
  s_sramPath += ".srm";
  if (FILE *f = fopen(s_sramPath.c_str(), "rb")) {
    const size_t got = fread(s_sram, 1, s_sramSize, f);
    fclose(f);
    hal_log("md: SRAM %06X-%06X loaded from %s (%u bytes)", (unsigned)start, (unsigned)end, s_sramPath.c_str(), (unsigned)got);
  } else {
    hal_log("md: SRAM %06X-%06X (new)", (unsigned)start, (unsigned)end);
  }
  // mapped all the time when it sits past the end of the ROM, else switched by 0xA130F1
  gwenesis_sram_setup(s_sram, start, end, (uint32_t)romSize <= start);
}

// ------------------------------------------------------------------------------------------------
static bool md_init() {
  if (!s_rom) s_rom = (uint8_t *)hal_alloc_big(MAX_ROM_SIZE + ROM_PAD);
  return s_rom != nullptr;
}

static void md_shutdown() {
  sramFlush();
  hal_free(s_sram);
  s_sram = nullptr;
  gwenesis_sram_setup(nullptr, 0, 0, 0);
  hal_free(s_rom);
  s_rom = nullptr;
  ROM_DATA = nullptr;
  YM2612FreeTables();                   // 46 KB of internal RAM back for the other cores
}

// .smd dumps: a 512-byte header, then 16 KB blocks holding the odd bytes in their first half and
// the even bytes in the second.
static bool isSmd(const char *path, const uint8_t *hdr, long size) {
  if (size <= 512 || (size - 512) % 16384) return false;
  const char *dot = strrchr(path, '.');
  if (dot && !strcasecmp(dot, ".smd")) return true;
  return hdr[8] == 0xAA && hdr[9] == 0xBB;
}

static void deinterleaveSmd(uint8_t *rom, long size) {
  uint8_t *blk = (uint8_t *)hal_alloc_big(16384);   // not a static: internal RAM is scarce
  if (!blk) return;
  for (long off = 0; off < size; off += 16384) {
    memcpy(blk, rom + off, 16384);
    for (int i = 0; i < 8192; i++) {
      rom[off + i * 2 + 1] = blk[i];
      rom[off + i * 2]     = blk[8192 + i];
    }
  }
  hal_free(blk);
}

static void detectRegion() {
  s_pal = (io_reg[0] & 0x40) != 0;
  lines_per_frame = s_pal ? LINES_PER_FRAME_PAL : LINES_PER_FRAME_NTSC;
  hal_audio_start((uint32_t)((uint64_t)lines_per_frame * VDP_CYCLES_PER_LINE * (s_pal ? 50 : 60) / AUDIO_FREQ_DIVISOR));
}

static bool md_loadRom(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { hal_log("md: cannot open %s", path); return false; }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0 || size > MAX_ROM_SIZE + 512) { fclose(f); hal_log("md: bad ROM size %ld", size); return false; }
  memset(s_rom, 0, MAX_ROM_SIZE + ROM_PAD);
  const long got = (long)fread(s_rom, 1, (size_t)size, f);
  fclose(f);
  if (got != size) { hal_log("md: short read (%ld of %ld)", got, size); return false; }

  if (isSmd(path, s_rom, size)) {
    size -= 512;
    memmove(s_rom, s_rom + 512, (size_t)size);
    memset(s_rom + size, 0, 512);
    deinterleaveSmd(s_rom, size);
    hal_log("md: SMD image de-interleaved");
  }
  if (size > MAX_ROM_SIZE) size = MAX_ROM_SIZE;
  if (size & 1) size++;                 // the byte swap works on words
  sramSetup(path, size);

  gwenesis_H32upscaler = 1;             // 256-wide modes fill the 320-wide screen
  load_cartridge(s_rom, (size_t)size);
  power_on();
  reset_emulation();
  detectRegion();
  memset(button_state, 0xFF, 3);
  hal_log("md: %s, %ld KB, %s", path, size >> 10, s_pal ? "PAL 50Hz" : "NTSC 60Hz");
  return true;
}

static void md_reset() {
  reset_emulation();
  memset(button_state, 0xFF, 3);
}

static uint32_t md_frameRate() { return s_pal ? 50 : 60; }

static void md_setInput(int port, uint32_t b) {
  if (port < 0 || port > 1) return;
  uint8_t s = 0;
  if (b & PAD_BTN_UP)    s |= 1 << PAD_UP;
  if (b & PAD_BTN_DOWN)  s |= 1 << PAD_DOWN;
  if (b & PAD_BTN_LEFT)  s |= 1 << PAD_LEFT;
  if (b & PAD_BTN_RIGHT) s |= 1 << PAD_RIGHT;
  if (b & PAD_BTN_A)     s |= 1 << PAD_A;
  if (b & PAD_BTN_B)     s |= 1 << PAD_B;
  if (b & PAD_BTN_C)     s |= 1 << PAD_C;
  if (b & PAD_BTN_START) s |= 1 << PAD_S;
  button_state[port] = (uint8_t)~s;     // active low
}

static void md_runFrame(bool draw) {
  int hint_counter = gwenesis_vdp_regs[10];

  screen_height = REG1_240_LINE ? 240 : 224;
  screen_width  = REG12_MODE_H40 ? 320 : 256;
  const int y0  = (SCREEN_H - screen_height) / 2;
  uint16_t *fb  = hal_video_back();
  gwenesis_vdp_set_buffer(fb + y0 * SCREEN_W);
  gwenesis_vdp_render_config();

  system_clock = 0;
  zclk = 0;
  ym2612_clock = 0;  ym2612_index = 0;
  sn76489_clock = 0; sn76489_index = 0;

  scan_line = 0;
  while (scan_line < lines_per_frame) {
    m68k_run(system_clock + VDP_CYCLES_PER_LINE);
    z80_run(system_clock + VDP_CYCLES_PER_LINE);
    if (GWENESIS_AUDIO_ACCURATE == 0) {
      gwenesis_SN76489_run(system_clock + VDP_CYCLES_PER_LINE);
      ym2612_run(system_clock + VDP_CYCLES_PER_LINE);
    }
    if (draw && (int)scan_line < screen_height) gwenesis_vdp_render_line(scan_line);

    // the line counter interrupt reloads on these lines
    if (scan_line == 0 || (int)scan_line > screen_height) hint_counter = REG10_LINE_COUNTER;
    if (--hint_counter < 0) {
      if (REG0_LINE_INTERRUPT != 0 && (int)scan_line <= screen_height) {
        hint_pending = 1;
        if ((gwenesis_vdp_status & STATUS_VIRQPENDING) == 0) m68k_update_irq(4);
      }
      hint_counter = REG10_LINE_COUNTER;
    }

    scan_line++;
    if ((int)scan_line == screen_height) {          // vblank starts after the last visible line
      if (REG1_VBLANK_INTERRUPT != 0) {
        gwenesis_vdp_status |= STATUS_VIRQPENDING;
        m68k_set_irq(6);
      }
      z80_irq_line(1);
    }
    if ((int)scan_line == screen_height + 1) z80_irq_line(0);
    system_clock += VDP_CYCLES_PER_LINE;
  }
  if (GWENESIS_AUDIO_ACCURATE == 1) {
    gwenesis_SN76489_run(system_clock);
    ym2612_run(system_clock);
  }
  m68k.cycles -= system_clock;
  frame_counter++;
  sramTick();

  if (draw) hal_video_submit(y0, screen_height);

  int n = ym2612_index > sn76489_index ? ym2612_index : sn76489_index;
  if (n > MD_AUDIO_BUF) n = MD_AUDIO_BUF;
  for (int i = 0; i < n; i++) {
    const int32_t v = gwenesis_ym2612_buffer[i] + gwenesis_sn76489_buffer[i];
    s_mix[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
  }
  hal_audio_write(s_mix, n);
}

// ------------------------------------------------------------------------------------------------
// save states: Gwenesis serialises itself through the tagged store in md_state.cpp
// ------------------------------------------------------------------------------------------------
bool mdStateWrite(FILE *f);   // md_state.cpp
bool mdStateRead(FILE *f);

static bool md_saveState(FILE *f) { return mdStateWrite(f); }

static bool md_loadState(FILE *f) {
  if (!mdStateRead(f)) return false;
  detectRegion();
  return true;
}

extern const EmuCore kMegaDrive;
const EmuCore kMegaDrive = {
  "Mega Drive / Genesis", "md", ".md.gen.bin.smd",
  md_init, md_loadRom, md_reset, md_runFrame, md_setInput, md_frameRate,
  md_saveState, md_loadState, md_shutdown,
};
