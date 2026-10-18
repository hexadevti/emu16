# Third-party code in emu16

emu16 as a whole is distributed under the GNU Affero General Public License v3 (see `LICENSE`),
because its Mega Drive core is derived from Gwenesis. Some components carry additional terms,
listed below. **Two of them forbid commercial distribution**, so emu16 builds must not be sold or
bundled into a commercial product while those components are included.

| Component | Path | Origin | License / terms |
|---|---|---|---|
| Gwenesis (bus, VDP, I/O, Z80 glue, save states) | `emu16/src/md/gwenesis/` | [bzhxx/gwenesis](https://github.com/bzhxx/gwenesis) | AGPL-3.0 (`emu16/src/md/gwenesis/LICENSE`) |
| Musashi 68000 core, as modified by Gwenesis | `emu16/src/cpu/m68k/` | Karl Stenerud, via Gwenesis | Musashi license (free use, attribution; see `readme.txt`) |
| Z80 core | `emu16/src/cpu/z80/` | Marat Fayzullin, via Gwenesis | **No commercial distribution**; author asks to be notified of changes |
| YM2612 (FM) | `emu16/src/sound/ym2612.*` | MAME `fm.c` (Jarek Burczynski, Tatsuyuki Satoh) with Genesis Plus GX fixes (Eke-Eke), via Gwenesis | **MAME non-commercial license** (pre-2016 MAME code) |
| SN76489 (PSG) | `emu16/src/sound/gwenesis_sn76489.*` | Maxim's SN76489 emulation, via Gwenesis | Free use with attribution |
| pce-go (HuC6280, VDC/VCE, PSG, save states) | `emu16/src/pce/pcego/` | [retro-go](https://github.com/ducalex/retro-go) `retro-core/components/pce-go`, derived from HuExpress / Hu-Go! | GPL-2.0 (`emu16/src/pce/pcego/LICENSE`); the HuExpress / Hu-Go! sources it derives from say "version 2 or (at your option) any later version", which lets it be combined with AGPL-3.0 |
| Geolith (Neo Geo AES/MVS: LSPC, memory map, boards, mixer, save data, save states) | `emu16/src/neogeo/geolith/` | [jgemu/geolith](https://gitlab.com/jgemu/geolith) @ `05ddef568212e7225ceec22f3ecb20b5a9b640e6` (0.5.0, 2026-10-11), Rupert Carmichael, Romain Tisserand | BSD-3-Clause (`geolith/LICENSE`; bundled parts in `geolith/LICENSES`) |
| Musashi 68000 core, as shipped with Geolith | `emu16/src/neogeo/geolith/m68k/` | Karl Stenerud, via Geolith | MIT (`geolith/LICENSES`) |
| Z80 core, as shipped with Geolith | `emu16/src/neogeo/geolith/z80/` | Nicolas Allemand, Rupert Carmichael, rofl0r | MIT (`geolith/z80/LICENSE`) |
| ymfm (YM2610), C port | `emu16/src/neogeo/geolith/ymfm/` | Aaron Giles; C port by Rupert Carmichael, via Geolith | BSD-3-Clause (`geolith/LICENSES`) |
| lithogen (MAME Neo Geo .zip to ROM regions, with decryption) | `emu16/src/neogeo/lithogen/` | [carmiker/lithogen](https://github.com/carmiker/lithogen) @ `ef0f9bd94e31689def44b49e0f8351e0206b622d` (2026-10-11), Rupert Carmichael; tables and algorithms from MAME | BSD-3-Clause (`lithogen/LICENSE`) |
| miniz (zip reading) | `emu16/src/neogeo/miniz/` | miniz 3.1.2 (Rich Geldreich, RAD Game Tools, Valve), as shipped with Geolith and lithogen | MIT (`miniz/LICENSE`) |
| UI bitmap fonts | `emu16/src/frontend/fonts/uifont.h` | X11 misc-fixed 6x10 / 7x13 (via FabGL, via emu8) | Public domain |
| UI text renderer, P4 display/keyboard/audio bring-up | `emu16/src/frontend/uitext.h`, `emu16/src/hal/picocalc/` | emu8 | Same author as emu16 |

## Changes to Gwenesis

Recorded here as the AGPL and Marat Fayzullin's terms ask:

- Include paths rewritten for the emu16 tree (`src/cpu`, `src/sound`, `src/md/gwenesis`).
- `gwenesis_bus.c`: the 8 MB static ROM array is a pointer (`ROM_DATA`) to a caller-owned buffer
  in PSRAM; `load_cartridge()` uses it in place.
- `gwenesis_bus.h`: `ROM_PAD`, the zeroed tail the caller allocates past `MAX_ROM_SIZE`.
- `m68k.h`: declares `ROM_DATA` as a pointer.
- `gwenesis_vdp_gfx.c`: `<stdint.h>` types; the RGB565 line output is always used.
- The Z80 core (`Z80.c`/`Z80.h`) is unmodified apart from include paths.
- `ym2612.c`/`ym2612.h`: `tl_tab`, `sin_tab` and `lfo_pm_table` are heap pointers allocated by
  `YM2612Init` (through `md_alloc_fast`), instead of static arrays. The new `YM2612FreeTables()`
  frees them when the Mega Drive core shuts down, so that internal RAM is free for the other cores.

## Changes to pce-go

Recorded here as the GPL asks (section 2a):

- `pce-go.h`: the retro-go logging, allocation and CRC hooks are replaced by `osd_log`,
  `osd_alloc_fast`, `osd_free` and `crc32_le`, implemented in `pce_core.cpp`. `XBUF_WIDTH` has a
  32-pixel margin for sprites. `osd_input_read`, `osd_vsync`, `RunPCE`, `LoadFile` and `PalettePCE`
  are removed. Added: `PalettePCE565`, `pce_frame_begin`, `pce_run_line`, and `PCE_LINES`.
- `pce-go.c`: `LoadCard` uses the caller's ROM buffer and does not free it. Banks past a ROM whose
  size is not a power of two mirror it. `LoadState`/`SaveState` take a `FILE *`.
- `pce.c`: the frame loop is split into `pce_frame_begin()` and `pce_run_line()`, so the frontend can
  produce sound line by line. Hot buffers use `osd_alloc_fast`. VRAM-to-VRAM DMA is masked to
  VRAM. Reset re-seeds the noise channels and flags a mode change.
- `pce.h`: `GFX_SCREEN_WIDTH`, the screen width clamped to the line buffer. 16-bit reads are
  byte-wise, so unaligned or bank-crossing reads are safe.
- `h6280_instr.h`: immediate-operand addresses wrap at 64 KB.
- `gfx.c`: rendering is clamped to the line buffer (wide modes, tall modes, the sprite margin), and
  the SATB copy wraps inside VRAM.
- `psg.c`: integer volume math, and the mix is summed in 32 bits and clamped to 16.

## Changes to Geolith, lithogen and miniz

These are BSD/MIT licensed and do not require a change log; it is kept so the upstream code can be
updated later.

**Left out of Geolith:** the disc readers (`geo_disc.c`, `chd/`, `cue/`, `dr_flac`), whose
`geo_disc_*` functions are stubs in `emu16/src/neogeo/neogeo_nocd.c`, the Speex resampler
(`deps/speex`), and the jg frontend (`jg.c`). `geo_cd.c` and `geo_lc8951.c` stay, because the
cartridge code calls into them behind `ngsys.cdmode`, which emu16 never sets. **Left out of
lithogen:** its command line (`cli.c`).

Every change is marked `emu16` in the source:

- **Include paths:** `geo.c` and lithogen's `zipio.c` include `../miniz/miniz.h`, and `ymfm/ymfm.h`
  includes `../geo_serial.h`. Arduino gives no `-I` for sketch subfolders.
- **Renamed files:** Arduino compiles every `.c` under `src/` and copies only known extensions, so
  `m68k/m68kfpu.c` became `m68kfpu_inc.h` and `ymfm/ymfm_fm.inc` became `ymfm_fm_inc.h`. The files
  that include them were updated to match.
- **Musashi symbol prefix:** `m68k/geo_m68k_prefix.h` renames this Musashi's external symbols to
  `gm_m68k_*` etc., because the Mega Drive core links its own Musashi into the same binary. It is
  included by `m68k.h` and first thing in `m68kcpu.c`, `m68kops.c` and `m68kdasm.c`. The
  disassembler's memory callbacks are stubs in `neogeo_nocd.c`.
- **Static arrays moved to the heap.** The ESP32-P4 puts `.bss` in its small internal SRAM, so
  these became pointers, allocated when first needed:
  - `geo_cd.c`: the CD RAMs (`pram`, `spr_dram`, `pcm_dram`, `z80_ram_cd`, `fix_ram`, `bram`)
    and `lc`, 7.4 MB in all. They are allocated in `geo_cd_init` and freed in `geo_cd_deinit`.
  - `m68k/m68kops.c`: the opcode jump table and the cycle table, allocated in
    `m68ki_build_opcode_table`.
  - `geo.h`/`geo.c`: `ngsys.nvram` and `ngsys.extram`, allocated in `geo_init`.
  - `geo_m68k.c`: the 68K work RAM (`ram`), allocated in `geo_m68k_init`, and `dynfix`, allocated
    only for the KOF10TH board.
  - `geo_lspc.c`: `lspc` and the two output palettes, allocated by `lspc_alloc()` from every entry
    point that can come before `geo_lspc_init`.
  - The hot ones (jump table, work RAM, `lspc`, palettes) use `ngc_alloc_fast`, which is
    `hal_alloc_fast` (internal RAM when it fits).
- `geo_mixer.c`: `GEO_NO_RESAMPLER` leaves out the Speex code. The mixer always outputs at the
  YM2610 rate, and emu16's HAL resamples.
- `geo_neo.c`:
  - `geo_neo_load` is split, so `geo_neo_setup(ngh)` sets up a game from regions that are already
    filled. emu16 fills them from lithogen without building a `.neo` file in memory.
  - P ROM reads that indexed the `.neo` buffer go through `romdata->p`, bounds-checked.
- Speed, for the P4:
  - `geo.c`: `geo_exec` runs the 68000 in slices instead of one instruction at a time. A slice
    ends at the next LSPC event (`geo_lspc_cycs_to_event()`), the end of the frame or the IRQ2
    timer running out. Writes to `REG_LSPCMODE` and `REG_TIMERLOW` end it early
    (`m68k_end_timeslice()` in `geo_m68k.c`). The Z80 runs up to the next YM2610 sample at a time.
    `geo_prof[]` times each part for emu16's profile line.
  - `m68k/m68kcpu.c`: `m68k_end_timeslice()` keeps `m68k_execute`'s return value the cycles used.
  - `m68k/m68kcpu.h` and `geo_m68k.c`: fast paths for the 68000's reads and writes to work RAM and
    to the fixed P ROM bank (`geo_m68k_fast_ram`, `geo_m68k_fast_rom`, refreshed by
    `geo_m68k_fast_update()`), wait states included. They skip the handler pointers and the
    address compares. They are off in CD mode, and for boards whose fixed bank is not plain ROM.
  - `m68k/m68kcpu.c` and `m68kcpu.h`: idle-loop skipping (`m68ki_idle_check()`, called from
    `m68ki_branch_8`). A short backward Bcc/BRA over one flag-setting instruction (cmp, cmpa.w,
    cmpi, tst or btst) that reads a register or work RAM gives the same result until an interrupt,
    the Z80 or the LSPC changes something. So the rest of the slice is used up at once.
  - `z80/z80.c` and `geo_z80.c`: the Z80's memory reads and writes go through tables of 32 2K pages
    (`z80_rpage`, `z80_wpage`) when a page is plain memory. `geo_z80_pages()` rebuilds the tables
    after every bank switch, reset, M ROM change or state load. A NULL page, and all of CD mode,
    use the callbacks as before.
  - `geo_lspc.c`: `geo_lspc_set_skip_render()` skips drawing for cartridges too (it was CD only),
    and the output buffer and palettes are RGB565 instead of XRGB8888, so the LSPC draws into
    emu16's frame buffer directly. The sprite line decodes each 16-pixel tile row once
    (`geo_lspc_trow()`: one aligned 32-bit C ROM read and a 256-entry bit-plane table), instead
    of decoding every pixel separately. Horizontally flipped rows are reversed whole
    (`geo_lspc_nibrev()`). Empty rows are skipped, and unshrunk sprites that are fully on screen
    take a straight copy loop. The tile offset is wrapped to the C ROM size only when it is past
    the end. The sprite chains are resolved once into a list (`sprlist`) when the 68K writes
    SCB2-4, instead of on every line. Each 16-line band has a bitmask of the sprites that reach
    into it, and a skipped frame leaves no sprites behind.
  - `geo_ymfm.c`: an optional async mode (`geo_ymfm_set_async()`). The Z80 side keeps a model of
    the timers, the busy flag, the IRQ line and the SSG registers, and queues timestamped register
    writes. The FM, SSG and ADPCM samples for a frame are generated during the next frame, on the
    other core (`ngc_job_start`), and handed to `geo_mixer_output` from there.
    End-of-sample flags reach the Z80 a frame or two late. `geo_z80.c` reaches the chip through
    `geo_ymfm_read`/`geo_ymfm_write`/`geo_ymfm_chip_reset`, and `geo_exec` ends with
    `geo_ymfm_frame_end()`.
  - `ymfm/ymfm_opn.c`, `ymfm_opn.h` and `ymfm_fm_inc.h`: accessors for that model (register
    peek, address latch, EOS flags, flag mask, the timer state, and the CSM key-on).
- lithogen:
  - `lithogen_convert_mem()` and `lithogen_image_free()` (`lithogen.h`) convert a set into
    memory. `lithogen_convert` is now a wrapper around the same code.
  - `neowrite.c`: `nc_neo_take()` hands the region buffers over, trimmed the way `nc_neo_write`
    trims them for a file.
  - `crypt/nc_sma.c`: the Garou and Metal Slug 3 68000 decryptors take their 32 KB and 64 KB
    scratch buffers from the heap instead of the stack, which is 32 KB on the P4.
- `miniz.h`: the functions the ESP32 ROM also has (`tinfl_*`, `tdefl_*`, `mz_adler32`, `mz_free`)
  are renamed `emu16_*`. The ROM linker scripts would otherwise bind them to the ROM's older
  miniz, whose `tinfl_decompressor` layout differs.

## Replacing the non-commercial parts

The 68000 and VDP are not affected. To make emu16 commercially distributable, the Z80 core can be
replaced by emu8's own Z80, and the YM2612 by a permissively licensed FM core such as
ymfm (BSD-3-Clause).
