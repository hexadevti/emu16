// board.h -- the two targets emu16 builds for.
//
//   BOARD_PICOCALC_P4  ClockworkPi PicoCalc mainboard + Waveshare ESP32-P4-WIFI6 module
//                      (arduino-cli, esp32:esp32:esp32p4, see .vscode/tasks.json)
//   BOARD_DESKTOP      SDL2 debug build for Windows/Linux (CMakeLists.txt)
//
// The P4 pin map is the one emu8 brought up (emu8/board.h, BOARD_PICOCALC_P4 section).
#pragma once

#if !defined(BOARD_PICOCALC_P4) && !defined(BOARD_DESKTOP)
#define BOARD_PICOCALC_P4   // arduino-cli is only ever pointed at the P4
#endif

// --- the panel: 320x320, the emulators use a 320x240 window in its middle -------------------
#define PANEL_W           320
#define PANEL_H           320
#define SCREEN_W          320                     // logical screen
#define SCREEN_H          240
#define SCREEN_Y0         ((PANEL_H - SCREEN_H) / 2)   // panel row of logical row 0 (40)

#if defined(BOARD_PICOCALC_P4)

#define BOARD_NAME        "PicoCalc (ESP32-P4)"

// ST7365P/ILI9488 panel on SPI2. GPIO24/25 (DC/RST) belong to the USB-Serial-JTAG PHY at reset.
#define LCD_SCK_PIN       3
#define LCD_MOSI_PIN      2
#define LCD_MISO_PIN      8
#define LCD_CS_PIN        7
#define LCD_DC_PIN        24
#define LCD_RST_PIN       25
#define LCD_SPI_HZ        80000000

// STM32 keyboard controller (also owns both backlights and the battery gauge).
#define KBD_I2C_SDA_PIN   50
#define KBD_I2C_SCL_PIN   49
#define KBD_I2C_ADDR      0x1F
#define KBD_I2C_HZ        10000
#define PICOCALC_MAINBOARD_WAIT_MS 30000

// microSD over SPI. The pins sit in an IO domain fed by internal LDO channel 4, off at boot.
#define SD_SCK_PIN        46
#define SD_MISO_PIN       48
#define SD_MOSI_PIN       33
#define SD_CS_PIN         47
#define SD_IO_LDO_CHAN    4
#define SD_SPI_HZ         20000000
#define SD_MOUNT          "/sd"

// Speakers: I2S0 PDM TX in DAC mode, left on dout, right on dout2.
#define AUDIO_L_PIN       23
#define AUDIO_R_PIN       22
#define AUDIO_OUT_RATE    44100

#elif defined(BOARD_DESKTOP)

#define BOARD_NAME        "Desktop (SDL2)"
#define AUDIO_OUT_RATE    44100
#ifndef EMU_DEFAULT_SD_DIR
#define EMU_DEFAULT_SD_DIR "sdcard"
#endif

#endif
