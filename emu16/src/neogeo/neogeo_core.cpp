// neogeo_core.cpp -- the Neo Geo (AES/MVS cartridge) core: Geolith (68000 + Z80 + YM2610 + LSPC)
// behind EmuCore. Games load straight from MAME .zip romsets: lithogen converts the set in
// memory (decrypting where needed) and its region buffers become Geolith's ROM data, so nothing
// is written to the SD card. .neo files load as they are. The BIOS comes from the MAME
// neogeo.zip in /roms (or /roms/neogeo), which lithogen also searches for BIOS-side files; a
// clone set needs its parent's zip beside it.
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <string>
#include "../../core.h"
#include "../hal/hal.h"
#include "../frontend/settings.h"

extern "C" {
#include "geolith/geo.h"
#include "geolith/geo_lspc.h"
#include "geolith/geo_mixer.h"
#include "geolith/geo_neo.h"
#include "geolith/geo_ymfm.h"
#include "lithogen/lithogen.h"
}

#define NG_MIXBUF     4096                  // stereo samples of one frame (~940 pairs), with room
#define NG_FIRST_LINE LSPC_LINE_BORDER_TOP  // the LSPC draws lines 8-247, screen rows 0-239
#define NG_BORDER_X   ((SCREEN_W - LSPC_WIDTH_VISIBLE) / 2)    // blanked around the 304x224 picture
#define NG_BORDER_Y   ((SCREEN_H - LSPC_HEIGHT_VISIBLE) / 2)
static_assert(LSPC_WIDTH == SCREEN_W, "the LSPC draws into the back buffer with its stride");
#define NG_SAVE_EVERY 300                   // frames between checks for changed save data

static int16_t        s_mix[NG_MIXBUF];
static int16_t        s_mono[NG_MIXBUF / 2];
static uint8_t       *s_neo = nullptr;      // a .neo file, which Geolith maps in place
static lithogen_image s_img;                // or the regions of a converted zip
static uint32_t       s_pad = 0;
static int            s_system = SYSTEM_AES;
static std::string    s_savePath;           // <rom> without its extension
static int            s_saveTick = 0;
static uint32_t       s_saveSum[GEO_SAVEDATA_MAX];
static std::string    s_err;                // why the last load failed, for the error box
static std::string    s_geoErr;             // Geolith's last error message
static std::string    s_biosPath;           // the neogeo.zip in use

// Logs a load failure and keeps it as the reason the frontend shows.
static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fail(const char *fmt, ...) {
  char b[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  hal_log("neogeo: %s", b);
  s_err = b;
}

// ------------------------------------------------------------------------------------------------
// what Geolith asks of its frontend
// ------------------------------------------------------------------------------------------------
static void ngLog(int level, const char *fmt, ...) {
  char b[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  size_t n = strlen(b);
  while (n && (b[n - 1] == '\n' || b[n - 1] == '\r')) b[--n] = 0;
  if (level == GEO_LOG_DBG) { hal_breadcrumb("neogeo: geolith: %.70s", b); return; }
  hal_log("neogeo: %s", b);
  if (level == GEO_LOG_ERR) s_geoErr = b;
}

static void lithoLog(lithogen_log_level, const char *msg, void *) {
  hal_log("neogeo: %s", msg);
  hal_breadcrumb("neogeo: converting, after \"%.60s\"", msg);
}

// Musashi's opcode jump table (256 KB) asks for internal RAM through this; see m68kops.c.
extern "C" void *ngc_alloc_fast(size_t n) { return hal_alloc_fast(n); }

// Inputs are active low. Joystick bits: up, down, left, right, A, B, C, D.
static unsigned pollJoystick(unsigned port) {
  if (port) return 0xff;
  unsigned b = 0xff;
  if (s_pad & PAD_BTN_UP)    b &= ~0x01u;
  if (s_pad & PAD_BTN_DOWN)  b &= ~0x02u;
  if (s_pad & PAD_BTN_LEFT)  b &= ~0x04u;
  if (s_pad & PAD_BTN_RIGHT) b &= ~0x08u;
  if (s_pad & PAD_BTN_A)     b &= ~0x10u;
  if (s_pad & PAD_BTN_B)     b &= ~0x20u;
  if (s_pad & PAD_BTN_C)     b &= ~0x40u;
  if (s_pad & PAD_BTN_X)     b &= ~0x80u;   // D
  return b;
}
static unsigned pollNone(unsigned) { return 0xff; }

// Coin 1 on Mode. On the MVS the unused coin slots 3/4 read high.
static unsigned pollStatA() {
  unsigned c = 0x07;
  if (s_pad & PAD_BTN_MODE) c &= 0x06;
  return c | (s_system == SYSTEM_AES ? 0x00 : 0x18);
}

// P1 Start and Select; memory card inserted and writable.
static unsigned pollStatB() {
  unsigned s = 0x0f;
  if (s_pad & PAD_BTN_START) s &= 0x0e;
  if (s_pad & PAD_BTN_Y)     s &= 0x0d;     // Select
  return s;
}
static unsigned pollSysType() { return 0xc0; }
static unsigned pollDipSw() { return 0xff; }

static void mixerOut(size_t n) {          // n interleaved stereo values
  const int pairs = (int)(n / 2);
  for (int i = 0; i < pairs; i++) s_mono[i] = (int16_t)(((int)s_mix[i * 2] + s_mix[i * 2 + 1]) / 2);
  hal_audio_write(s_mono, pairs);
}

// ------------------------------------------------------------------------------------------------
// saves: NVRAM (MVS), cartridge RAM and the memory card, as <rom>.nv/.srm/.mcr beside the ROM
// ------------------------------------------------------------------------------------------------
static const char *const kSaveExt[GEO_SAVEDATA_MAX] = { ".nv", ".srm", ".mcr", ".brm" };
static const unsigned kSaveMem[GEO_SAVEDATA_MAX] = {
  GEO_MEMTYPE_NVRAM, GEO_MEMTYPE_CARTRAM, GEO_MEMTYPE_MEMCARD, GEO_MEMTYPE_CDBRAM };

static uint32_t saveSum(int i) {
  size_t n = 0;
  const uint8_t *p = (const uint8_t *)geo_mem_ptr(kSaveMem[i], &n);
  if (!p) return 0;
  uint32_t h = 2166136261u;                // FNV-1a
  for (size_t k = 0; k < n; k++) h = (h ^ p[k]) * 16777619u;
  return h;
}

// Which save types this machine keeps: the MVS has NVRAM, a few carts have battery RAM, and
// on the AES the memory card stands in for both.
static bool saveUsed(int i) {
  switch (i) {
    case GEO_SAVEDATA_NVRAM:   return s_system != SYSTEM_AES;
    case GEO_SAVEDATA_CARTRAM: return geo_cartram_present() != 0;
    case GEO_SAVEDATA_MEMCARD: return true;
    default:                   return false;
  }
}

static void savesLoad() {
  for (int i = 0; i < GEO_SAVEDATA_MAX; i++) {
    if (!saveUsed(i)) continue;
    const std::string p = s_savePath + kSaveExt[i];
    if (geo_savedata_load(i, p.c_str()) == 1) hal_log("neogeo: loaded %s", p.c_str());
    s_saveSum[i] = saveSum(i);
  }
  s_saveTick = 0;
}

// Writes what changed since it was loaded or last written, so untouched saves make no files.
static void savesFlush() {
  for (int i = 0; i < GEO_SAVEDATA_MAX; i++) {
    if (!saveUsed(i)) continue;
    const uint32_t h = saveSum(i);
    if (h == s_saveSum[i]) continue;
    const std::string p = s_savePath + kSaveExt[i];
    if (geo_savedata_save(i, p.c_str()) == 1) hal_log("neogeo: saved %s", p.c_str());
    else hal_log("neogeo: cannot write %s", p.c_str());
    s_saveSum[i] = h;
  }
}

// ------------------------------------------------------------------------------------------------
// loading
// ------------------------------------------------------------------------------------------------
static bool hasExt(const char *path, const char *ext) {
  const char *dot = strrchr(path, '.');
  return dot && !strcasecmp(dot, ext);
}

static void freeGame() {
  hal_free(s_neo);
  s_neo = nullptr;
  lithogen_image_free(&s_img);
}

// The BIOS zip must exist and hold the BIOS of the machine being started (neo-epo.bin for the
// AES, sp-u2.sp1 for the MVS, both with 000-lo.lo); MAME's neogeo.zip has all of them, other
// packs may not. It lives in /roms, shared with other systems' BIOS sets, or beside the games.
static bool loadBios() {
  static const char *const kBiosPaths[] = { "roms/neogeo.zip", "roms/neogeo/neogeo.zip" };
  FILE *f = nullptr;
  for (const char *p : kBiosPaths) {
    s_biosPath = sdPath(p);
    if ((f = fopen(s_biosPath.c_str(), "rb"))) break;
  }
  if (!f) {
    fail("BIOS not found: put MAME's neogeo.zip in %s", sdPath("roms").c_str());
    return false;
  }
  const std::string &bios = s_biosPath;
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fclose(f);
  s_geoErr.clear();
  hal_breadcrumb("neogeo: loading the %s BIOS from %s", s_system == SYSTEM_AES ? "AES" : "MVS", bios.c_str());
  if (geo_bios_load_file(bios.c_str())) return true;
  if (!s_geoErr.empty())
    fail("%s BIOS, %s: %s", s_system == SYSTEM_AES ? "AES" : "MVS", bios.c_str(), s_geoErr.c_str());
  else
    fail("%s (%ld bytes) is not a readable zip", bios.c_str(), size);
  return false;
}

static bool loadNeoFile(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { fail("Cannot open %s", path); return false; }
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 4096) { fclose(f); fail("%s is too small for a .neo file", path); return false; }
  s_neo = (uint8_t *)hal_alloc_big((size_t)size);
  if (!s_neo) { fclose(f); fail("Not enough memory: this game needs %ld MB", (size + 0xfffff) >> 20); return false; }
  const long got = (long)fread(s_neo, 1, (size_t)size, f);
  fclose(f);
  if (got != size) { fail("Read error (%ld of %ld bytes)", got, size); return false; }
  return geo_neo_load(s_neo, (size_t)size) != 0;
}

// The zip's regions go to Geolith in place; what geo_neo_load does with a .neo, minus the copy.
static bool loadZip(const char *path) {
  lithogen_options opt;
  lithogen_options_init(&opt);
  opt.log = lithoLog;
  // lithogen looks for BIOS-side files in a neogeo.zip beside the set; name ours if it's elsewhere
  // (not both: the SD card has few file handles)
  std::string beside(path);
  const size_t slash = beside.find_last_of('/');
  beside = (slash == std::string::npos ? std::string() : beside.substr(0, slash + 1)) + "neogeo.zip";
  const char *aux[] = { s_biosPath.c_str() };
  if (strcasecmp(beside.c_str(), s_biosPath.c_str())) { opt.aux_zips = aux; opt.num_aux_zips = 1; }
  lithogen_report rep;
  const uint32_t t0 = hal_millis();
  hal_breadcrumb("neogeo: converting %s", path);
  const lithogen_status st = lithogen_convert_mem(path, &opt, &rep, &s_img);
  if (st != LITHOGEN_OK) {
    if (st == LITHOGEN_ERR_NOMEM) fail("Not enough memory for this game (the PicoCalc has 32 MB)");
    else fail("%s", rep.errstr[0] ? rep.errstr : "Not a Neo Geo romset lithogen knows");
    return false;
  }
  hal_log("neogeo: %s (NGH %03X) converted in %u ms: P %u S %u M %u V %u+%u C %u KB",
          s_img.title ? s_img.title : "?", (unsigned)s_img.ngh, (unsigned)(hal_millis() - t0),
          (unsigned)(s_img.psz >> 10), (unsigned)(s_img.ssz >> 10), (unsigned)(s_img.msz >> 10),
          (unsigned)(s_img.v1sz >> 10), (unsigned)(s_img.v2sz >> 10), (unsigned)(s_img.csz >> 10));

  romdata_t *rd = geo_romdata_ptr();
  rd->p = s_img.p;   rd->psz = s_img.psz;
  rd->s = s_img.s;   rd->ssz = s_img.ssz;
  rd->m = s_img.m;   rd->msz = s_img.msz;
  rd->v1 = s_img.v1; rd->v1sz = s_img.v1sz;
  rd->v2 = s_img.v2 ? s_img.v2 : s_img.v1;
  rd->v2sz = s_img.v2 ? s_img.v2sz : s_img.v1sz;
  rd->c = s_img.c;   rd->csz = s_img.csz;
  hal_breadcrumb("neogeo: setting up NGH %03X", (unsigned)s_img.ngh);
  if (geo_neo_setup(s_img.ngh)) return true;
  fail("Geolith does not support this game (NGH %03X)", (unsigned)s_img.ngh);
  return false;
}

// Byteswapping the P ROM is the one thing a failed setup leaves behind (it is its own inverse).
static void unswapP() {
  romdata_t *rd = geo_romdata_ptr();
  for (size_t i = 0; i + 1 < rd->psz; i += 2) { const uint8_t t = rd->p[i]; rd->p[i] = rd->p[i + 1]; rd->p[i + 1] = t; }
}

static void startMachine(int system) {
  s_system = system;
  geo_set_system(system);
  geo_set_region(REGION_US);
  geo_mixer_init();                         // always raw in emu16's copy (no resampler)
  geo_lspc_set_palette(0);                  // builds the colour tables (resistor network); without it all is black
  geo_init();
  geo_set_region(REGION_US);
}

static bool ngc_init() {
  geo_log_set_callback(ngLog);
  memset(&s_img, 0, sizeof(s_img));
  return true;
}

static bool ngc_loadRom(const char *path) {
  s_err.clear();
  geo_ymfm_set_async(0);
  freeGame();
  startMachine(SYSTEM_AES);
  if (!loadBios()) {
    // a BIOS pack without the AES BIOS may still have the MVS one
    geo_deinit();
    startMachine(SYSTEM_MVS);
    if (!loadBios()) return false;
    s_err.clear();
  }

  const bool zip = hasExt(path, ".zip");
  bool ok = zip ? loadZip(path) : loadNeoFile(path);
  if (!ok && geo_romdata_ptr()->p && geo_romdata_ptr()->psz) {
    // some sets are MVS-only (the AES BIOS rejects them): try again as an arcade board
    hal_log("neogeo: retrying as MVS");
    unswapP();
    geo_bios_unload();
    geo_deinit();
    startMachine(SYSTEM_MVS);
    ok = loadBios() && geo_neo_setup(zip ? s_img.ngh : (uint32_t)(s_neo[40] | s_neo[41] << 8 | s_neo[42] << 16 | s_neo[43] << 24)) != 0;
  }
  if (!ok) {
    if (s_err.empty()) fail("Geolith rejected %s", path);
    return false;
  }

  s_err.clear();                            // the AES attempt may have failed first
  geo_mixer_set_buffer(s_mix);
  geo_mixer_set_callback(mixerOut);
  geo_input_set_callback(0, pollJoystick);
  geo_input_set_callback(1, pollNone);
  geo_input_sys_set_callback(0, pollStatA);
  geo_input_sys_set_callback(1, pollStatB);
  geo_input_sys_set_callback(2, pollSysType);
  geo_input_sys_set_callback(3, pollDipSw);

  s_savePath = path;
  const size_t dot = s_savePath.find_last_of('.'), slash = s_savePath.find_last_of('/');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) s_savePath.erase(dot);
  hal_breadcrumb("neogeo: loading saves");
  savesLoad();

  hal_breadcrumb("neogeo: reset");
  geo_reset(1);
  // the YM2610's samples are made on the other core (the Z80 sees its timers and flags from here)
  if (!geo_ymfm_set_async(1)) hal_log("neogeo: no memory for the YM2610 job, running it here");
  s_pad = 0;
  // the YM2610 runs at 55.5 kHz (MVS) / 55.9 kHz (AES); the HAL resamples, and its pace sets ours
  hal_audio_start(s_system == SYSTEM_AES ? 55943 : 55555);
  hal_log("neogeo: %s on the %s", path, s_system == SYSTEM_AES ? "AES" : "MVS");
  return true;
}

static void ngc_reset() { geo_ymfm_sync(); geo_reset(1); }
static uint32_t ngc_frameRate() { return 60; }
static void ngc_setInput(int port, uint32_t b) { if (port == 0) s_pad = b; }

// The LSPC drew in RGB565 straight into the back buffer; black out the overscan around the
// 304x224 picture that real sets hide, and show it.
static void finishFrame(uint16_t *fb) {
  memset(fb, 0, (size_t)NG_BORDER_Y * SCREEN_W * 2);
  memset(fb + (NG_BORDER_Y + LSPC_HEIGHT_VISIBLE) * SCREEN_W, 0,
         (size_t)(SCREEN_H - NG_BORDER_Y - LSPC_HEIGHT_VISIBLE) * SCREEN_W * 2);
  for (int y = NG_BORDER_Y; y < NG_BORDER_Y + LSPC_HEIGHT_VISIBLE; y++) {
    uint16_t *row = fb + (size_t)y * SCREEN_W;
    memset(row, 0, NG_BORDER_X * 2);
    memset(row + NG_BORDER_X + LSPC_WIDTH_VISIBLE, 0, (SCREEN_W - NG_BORDER_X - LSPC_WIDTH_VISIBLE) * 2);
  }
  hal_video_submit(0, SCREEN_H);
}

// Profile: geo_exec's own buckets, plus the whole frame and the other core's job (its time, less
// the audio wait), since perfInfo.
extern "C" uint32_t ngc_cycles(void) { return hal_cycles(); }
static uint32_t s_profFrame, s_profVideo, s_profFrames;
static volatile uint32_t s_profJob;

// geo_ymfm.c's job on the other core
static void (*s_jobFn)(void *);
static void *s_jobArg;
static void jobRun(void *) {
  const uint32_t t0 = hal_cycles(), w0 = hal_audio_wait_us();
  s_jobFn(s_jobArg);
  s_profJob += hal_cycles() - t0 - (hal_audio_wait_us() - w0) * hal_cycles_per_us();
}
extern "C" void ngc_job_start(void (*fn)(void *), void *arg) {
  hal_job_wait();
  s_jobFn = fn;
  s_jobArg = arg;
  hal_job_start(jobRun, nullptr);
}
extern "C" void ngc_job_wait(void) { hal_job_wait(); }

static const char *ngc_perfInfo() {
  static char b[80];
  const uint32_t n = s_profFrames ? s_profFrames : 1, perMs = hal_cycles_per_us() * 1000;
  auto ms = [&](uint64_t cyc) { return (float)cyc / perMs / n; };
  const float m68k = ms(geo_prof[GEO_PROF_M68K]), z80 = ms(geo_prof[GEO_PROF_Z80]);
  const float ym = ms(geo_prof[GEO_PROF_YM]), video = ms((uint64_t)geo_prof[GEO_PROF_LSPC] + s_profVideo);
  // GEO_PROF_MIX is the hand-over to the other core, mostly waiting for it (which paces us)
  float other = ms(s_profFrame) - m68k - z80 - ym - video - ms(geo_prof[GEO_PROF_MIX]);
  if (other < 0) other = 0;
  snprintf(b, sizeof(b), "68k %.1f z80 %.1f fm %.1f vid %.1f etc %.1f c0 %.1f",
           m68k, z80, ym, video, other, ms(s_profJob));
  memset(geo_prof, 0, sizeof(geo_prof));
  s_profFrame = s_profVideo = s_profFrames = 0;
  s_profJob = 0;
  return b;
}

static void ngc_runFrame(bool draw) {
  const uint32_t t0 = hal_cycles();
  uint16_t *fb = hal_video_back();
  // line n goes to row n - 8; the LSPC only writes lines 8-247
  if (draw) geo_lspc_set_buffer(fb - NG_FIRST_LINE * SCREEN_W);
  geo_lspc_set_skip_render(!draw);
  geo_exec();                               // the mixer callback writes this frame's audio
  if (draw) {
    const uint32_t tv = hal_cycles();
    finishFrame(fb);
    s_profVideo += hal_cycles() - tv;
  }
  if (++s_saveTick >= NG_SAVE_EVERY) { s_saveTick = 0; savesFlush(); }
  s_profFrame += hal_cycles() - t0;
  s_profFrames++;
}

static bool ngc_saveState(FILE *f) {
  const void *st = geo_state_save_raw();
  const size_t n = geo_state_size();
  return st && fwrite(st, 1, n, f) == n;
}

static bool ngc_loadState(FILE *f) {
  const size_t n = geo_state_size();
  uint8_t *buf = (uint8_t *)hal_alloc_big(n);
  if (!buf) return false;
  const bool ok = fread(buf, 1, n, f) == n && geo_state_load_raw(buf);
  hal_free(buf);
  return ok;
}

static void ngc_shutdown() {
  geo_ymfm_set_async(0);
  if (geo_romdata_ptr()->p) savesFlush();
  geo_bios_unload();
  geo_deinit();
  freeGame();
  romdata_t *rd = geo_romdata_ptr();
  rd->p = rd->s = rd->m = rd->v1 = rd->v2 = rd->c = nullptr;
  rd->psz = rd->ssz = rd->msz = rd->v1sz = rd->v2sz = rd->csz = 0;
}

// The browser lists only zips lithogen knows as a game, by their title; neogeo.zip and other
// non-game zips are hidden.
static bool ngc_romLabel(const char *fileName, std::string &label) {
  if (!hasExt(fileName, ".zip")) return true;
  std::string set(fileName, strrchr(fileName, '.') - fileName);
  for (char &c : set) c = (char)tolower((unsigned char)c);
  const int gi = lithogen_find_game(set.c_str());
  if (gi < 0) return false;
  lithogen_game_desc d;
  if (lithogen_game_desc_get((size_t)gi, &d) && d.title) label = d.title;
  return true;
}

static const char *ngc_loadError() { return s_err.c_str(); }

extern const EmuCore kNeoGeo;
const EmuCore kNeoGeo = {
  "Neo Geo", "neogeo", ".zip.neo",
  ngc_init, ngc_loadRom, ngc_reset, ngc_runFrame, ngc_setInput, ngc_frameRate,
  ngc_saveState, ngc_loadState, ngc_shutdown,
  ngc_romLabel,
  ngc_loadError,
  ngc_perfInfo,
};
