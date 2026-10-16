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

## Replacing the non-commercial parts

The 68000 and VDP are not affected. To make emu16 commercially distributable, the Z80 core can be
replaced by emu8's own Z80, and the YM2612 by a permissively licensed FM core such as
ymfm (BSD-3-Clause).
