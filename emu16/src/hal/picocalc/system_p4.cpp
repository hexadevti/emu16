// system_p4.cpp -- boot order, timing, memory, SD card and the esp32loader hand-off on the P4.
#include "../../../board.h"

#if defined(BOARD_PICOCALC_P4)

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <stdarg.h>
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "soc/soc.h"
#include "soc/lp_system_reg.h"
#include "esp_system.h"
#include "../hal.h"

bool picocalcWaitForMainboard();   // input_p4.cpp
void p4DisplayInit();              // display_p4.cpp
void p4InputInit();                // input_p4.cpp

#define ESP32LOADER_MAGIC 0x10ADE125u   // esp32loader main/loader_api.h: boot the loader once
#define LOADER_SERIAL_CMD "@uf2menu\n"  // same command the emu8 deploy tools send

static SPIClass s_sdSpi(HSPI);
static bool s_sdOk = false;

uint32_t hal_millis()          { return millis(); }
uint32_t hal_micros()          { return micros(); }
void     hal_delay(uint32_t ms){ delay(ms); }
void     hal_yield()           { taskYIELD(); }

void hal_log(const char *fmt, ...) {
  char b[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  Serial.println(b);
}

void hal_reboot() { delay(50); esp_restart(); }

void hal_reboot_to_loader() {
  hal_log("rebooting to esp32loader");
  delay(50);
  REG_WRITE(LP_SYSTEM_REG_LP_STORE15_REG, ESP32LOADER_MAGIC);   // without the loader: plain restart
  esp_restart();
}

// The PC can ask for the loader over the console UART (emu8 tools/deploy-p4-wifi.py et al.).
void p4CheckSerialCmd() {
  static const char cmd[] = LOADER_SERIAL_CMD;
  static uint8_t matched = 0;
  while (Serial.available()) {
    const int c = Serial.read();
    if (c < 0) break;
    if (c != (uint8_t)cmd[matched]) matched = 0;
    if (c == (uint8_t)cmd[matched] && !cmd[++matched]) hal_reboot_to_loader();
  }
}

void *hal_alloc_big(size_t n) {
  void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : calloc(1, n);
}
void *hal_alloc_fast(size_t n) {
  void *p = heap_caps_calloc(1, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return p ? p : hal_alloc_big(n);
}
void hal_free(void *p) { free(p); }

bool        hal_sd_mounted() { return s_sdOk; }
const char *hal_sd_root()    { return SD_MOUNT; }

static void sdInit() {
  // The SD pins sit in an IO domain fed by an internal LDO that is off at boot; without it
  // GPIO46-48 are dead and every mount fails.
  static esp_ldo_channel_handle_t ldo = nullptr;
  if (!ldo) {
    esp_ldo_channel_config_t c = {};
    c.chan_id = SD_IO_LDO_CHAN;
    c.voltage_mv = 3300;
    if (esp_ldo_acquire_channel(&c, &ldo) != ESP_OK) hal_log("SD: IO LDO enable failed");
  }
  s_sdSpi.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  for (int i = 0; i < 10; i++) {
    if (SD.begin(SD_CS_PIN, s_sdSpi, SD_SPI_HZ, SD_MOUNT)) { s_sdOk = true; break; }
    SD.end();
    delay(120);
  }
  if (s_sdOk) hal_log("SD: %lluMB card mounted at %s", SD.cardSize() >> 20, SD_MOUNT);
  else        hal_log("SD: mount failed");
}

void hal_init() {
  Serial.begin(115200);
  delay(50);
  hal_log("emu16 on " BOARD_NAME ", PSRAM %u KB free, internal %u KB free",
          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10),
          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10));
  const bool board = picocalcWaitForMainboard();
  p4DisplayInit();
  if (board) sdInit();
  p4InputInit();
}

#endif // BOARD_PICOCALC_P4
