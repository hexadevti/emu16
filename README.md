# emu16

16-bit console emulators for the **ClockworkPi PicoCalc** fitted with a **Waveshare ESP32-P4-WIFI6**
module, plus an SDL2 desktop build of the same code for debugging. emu16 is the 16-bit sibling of
emu8, and its P4 display, keyboard, audio and SD bring-up come from emu8.

| System | Status | Core |
|---|---|---|
| Sega Mega Drive / Genesis | first core | Gwenesis (Musashi 68000, Z80, YM2612, SN76489) |
| PC Engine / TurboGrafx-16 | new, HuCards only | pce-go from retro-go (HuC6280, VDC/VCE, PSG) |
| SNES, Neo Geo, Amiga 500 | planned | — |

## Using it

Put ROMs on the SD card under `/roms/md/` (`.md`, `.gen`, `.bin`, `.smd`) and `/roms/pce/` (`.pce`).
At boot, pick the system, then the game. On the PC Engine, F3 is button II, F4 is button I, and F5 or
Space is Select. CD-ROM, SuperGrafx and the 512-pixel video mode, which is cropped to 352, are not
supported.

| Key | In game | In menus |
|---|---|---|
| Arrows | D-pad | move |
| F3, F4, F5 | A, B, C | |
| Caps, Del, Backspace, Space | X, Y, Z, Mode (6-button, for later cores) | Backspace: up a folder |
| Enter | Start | select |
| Esc or F1 | in-game menu (reset, save/load state, volume, frameskip, quit) | back |
| Ctrl-S / Ctrl-L | quick save / quick load | |
| Ctrl-Shift-Up | reboot to esp32loader | same |
| Ctrl-Shift-F3 | restart | same |

Settings are stored in `/emu16/emu16.cfg` and save states in `/emu16/states/<game>.st0`. Games
with battery-backed saves write them to `<game>.srm` next to the ROM, about a second after the game
stops writing and again when you quit the game. **Settings > Show FPS** puts a frame rate and CPU
load meter in the black band above the picture.

## Building

### PicoCalc P4 (arduino-cli)

The P4 build uses the same isolated Arduino-ESP32 3.x install as emu8 (`~/.emu6502-p4`; see the emu8
README, "P4 build", for the one-time setup). In VS Code, run **P4: Build** or **P4: Build & Upload**
(COM9). From a shell:

```sh
export ARDUINO_DIRECTORIES_DATA=~/.emu6502-p4/data ARDUINO_DIRECTORIES_USER=~/.emu6502-p4/user
arduino-cli compile --fqbn esp32:esp32:esp32p4:PSRAM=enabled,FlashSize=16M,ChipVariant=postv3,PartitionScheme=huge_app,DebugLevel=error \
  --build-property compiler.optimization_flags=-O2 \
  --build-property compiler.cpp.extra_flags=-DBOARD_PICOCALC_P4 \
  --build-path ~/.emu16-p4-build emu16
arduino-cli upload -p COM9 --fqbn <same fqbn> --input-dir ~/.emu16-p4-build emu16
```

### Desktop (SDL2)

You need MSYS2 with the MinGW32 toolchain, SDL2 and CMake (`mingw-w64-i686-gcc`, `mingw-w64-i686-SDL2`,
`mingw-w64-i686-cmake`). Run the **desktop: build** task, or from a MinGW32 shell:

```sh
cmake -G "Unix Makefiles" -S . -B build -DCMAKE_BUILD_TYPE=Debug && make -C build -j8
```

The repo's `sdcard/` folder plays the SD card; the `EMU16_SD` environment variable points it
elsewhere. The window shows the whole 320x320 panel at 2x.

Test hooks: `EMU16_ROM=<file>` starts a game at once; `EMU16_SHOT=N` saves `<sd>/shot.bmp` after N
screen updates and quits; `EMU16_KEYS` plays a comma-separated key script (`up`, `enter`, `esc`,
`f5`, a single character, `w500` to wait 500 ms, `shot` to save `<sd>/shotN.bmp`, `quit`).

## Layout

```
emu16/emu16.ino          setup()/loop() -> src/frontend/app.cpp
emu16/board.h            pins and screen geometry (P4 and desktop)
emu16/core.h             the interface every system core implements
emu16/src/hal/           hal.h, picocalc/ (ESP32-P4), desktop/ (SDL2)
emu16/src/frontend/      menus, ROM browser, settings, game loop
emu16/src/cpu/           CPU cores shared between systems (m68k, z80)
emu16/src/sound/         sound chips (YM2612, SN76489)
emu16/src/md/            Mega Drive: Gwenesis machine + md_core.cpp glue
emu16/src/pce/           PC Engine: pce-go machine + pce_core.cpp glue
```

To add a system, write `src/<system>/<system>_core.cpp` exporting an `EmuCore`, and add it to
`kCores[]` in `src/frontend/app.cpp`.

## License

AGPL-3.0, inherited from Gwenesis. The PC Engine core is GPL-2.0-or-later. The Z80 core and the YM2612 core may not be distributed
commercially. See [THIRD_PARTY.md](THIRD_PARTY.md).
