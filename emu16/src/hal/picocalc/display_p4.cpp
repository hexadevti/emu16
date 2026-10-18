// display_p4.cpp -- the PicoCalc's ST7365P/ILI9488 320x320 SPI panel behind an ESP32-P4.
//
// Transport ported from emu8 (src/picocalc/display_picocalc.cpp, the BOARD_PICOCALC_P4 path):
//  * ESP-IDF spi_master on SPI2 with DMA. CS is a plain GPIO held low for good: the panel is alone
//    on this bus, and no CS edge can then land between a RAMWR and its pixel data.
//  * ESP SPI sends bytes in memory order, so RGB565 is byte-swapped into one of P4_NBUF DMA-capable
//    bounce buffers on the way out. The caller's pixels are free once txBuf() returns.
//  * Commands are short polling transactions issued after every queued pixel transfer finished.
//
// New for emu16: a video task on core 0 owns the game frame push, so the emulation core never
// waits for the ~14ms it takes to send 320x224 at 80MHz (hal_video_submit()).
#include "../../../board.h"

#if defined(BOARD_PICOCALC_P4)

#include <Arduino.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include "hal/usb_serial_jtag_ll.h"
#include <string.h>
#include "../hal.h"

void picocalcBacklightOn();              // input_p4.cpp: the backlight lives on the STM32

void p4CreateTask(TaskFunction_t fn, const char *name, uint32_t bytes, UBaseType_t prio, TaskHandle_t *h);  // system_p4.cpp

#define P4_BOUNCE_PX 8192                // pixels per bounce buffer / per DMA transaction
#define P4_NBUF      3                   // bounce buffers = transactions in flight

static spi_device_handle_t s_dev = nullptr;
static uint16_t *s_bnc[P4_NBUF];
static uint16_t *s_solidBuf = nullptr;   // one color, pre-swapped, repeated by txSolid()
static uint16_t  s_solidColor = 0;
static uint32_t  s_solidLen = 0;         // pixels of s_solidBuf currently holding s_solidColor
static spi_transaction_t s_trn[P4_NBUF];
static uint8_t s_next = 0;               // next slot (the oldest in-flight one when all are busy)
static uint8_t s_inflight = 0;
static SemaphoreHandle_t s_lock = nullptr;   // the SPI device: video task vs. UI pushes

static inline void dcCmd()  { gpio_set_level((gpio_num_t)LCD_DC_PIN, 0); }
static inline void dcData() { gpio_set_level((gpio_num_t)LCD_DC_PIN, 1); }

// Transactions complete in queue order, so reaping one always frees the oldest slot.
static void reapOne() {
  spi_transaction_t *r;
  if (spi_device_get_trans_result(s_dev, &r, portMAX_DELAY) == ESP_OK) s_inflight--;
}
static void txWait() { while (s_inflight) reapOne(); }
static uint8_t slotTake() {
  if (s_inflight == P4_NBUF) reapOne();
  return s_next;
}
static void slotQueue(uint8_t i, const void *buf, uint32_t px) {
  spi_transaction_t &t = s_trn[i];
  memset(&t, 0, sizeof(t));
  t.length = px * 16;
  t.tx_buffer = buf;
  if (spi_device_queue_trans(s_dev, &t, portMAX_DELAY) != ESP_OK) return;
  s_inflight++;
  s_next = (uint8_t)((i + 1) % P4_NBUF);
}

// `w` pixels per row, `h` rows, rows `stride` apart in the source.
static void txRect(const uint16_t *src, uint32_t w, uint32_t h, uint32_t stride) {
  uint32_t x = 0;
  while (h) {
    const uint8_t i = slotTake();
    uint16_t *d = s_bnc[i];
    uint32_t k = 0;
    while (h && k < P4_BOUNCE_PX) {
      uint32_t run = w - x;
      if (run > P4_BOUNCE_PX - k) run = P4_BOUNCE_PX - k;
      const uint16_t *s = src + x;
      for (uint32_t j = 0; j < run; j++) d[k + j] = __builtin_bswap16(s[j]);
      k += run; x += run;
      if (x == w) { x = 0; src += stride; h--; }
    }
    slotQueue(i, d, k);
  }
}
static void txSolid(uint16_t c, uint32_t n) {
  const uint32_t want = n > P4_BOUNCE_PX ? P4_BOUNCE_PX : n;
  if (c != s_solidColor || want > s_solidLen) {
    txWait();                            // queued fills may still be reading the old color
    const uint16_t v = __builtin_bswap16(c);
    for (uint32_t j = 0; j < want; j++) s_solidBuf[j] = v;
    s_solidColor = c;
    s_solidLen = want;
  }
  while (n) {
    const uint32_t k = n > P4_BOUNCE_PX ? P4_BOUNCE_PX : n;
    slotQueue(slotTake(), s_solidBuf, k);
    n -= k;
  }
}

// Short blocking writes. Anything over 4 bytes goes through an idle bounce buffer: the init table
// lives in flash, which the SPI DMA cannot read.
static void wrBytes(const uint8_t *d, size_t n) {
  if (!n) return;
  spi_transaction_t t = {};
  t.length = n * 8;
  if (n <= 4) {
    t.flags = SPI_TRANS_USE_TXDATA;
    memcpy(t.tx_data, d, n);
  } else {
    uint8_t *b = (uint8_t *)s_bnc[s_next];   // idle: wrBytes only runs after txWait()
    memcpy(b, d, n);
    t.tx_buffer = b;
  }
  spi_device_polling_transmit(s_dev, &t);
}
static void wrCmd(uint8_t c) {
  txWait();
  dcCmd();
  wrBytes(&c, 1);
  dcData();
}

// CASET / RASET / RAMWR in raw panel coordinates (inclusive bounds).
static void panelWindow(int x0, int y0, int x1, int y1) {
  uint8_t b[4];
  wrCmd(0x2A);
  b[0] = (uint8_t)(x0 >> 8); b[1] = (uint8_t)x0; b[2] = (uint8_t)(x1 >> 8); b[3] = (uint8_t)x1;
  wrBytes(b, 4);
  wrCmd(0x2B);
  b[0] = (uint8_t)(y0 >> 8); b[1] = (uint8_t)y0; b[2] = (uint8_t)(y1 >> 8); b[3] = (uint8_t)y1;
  wrBytes(b, 4);
  wrCmd(0x2C);
}

// GPIO24/25 (DC/RST) come out of reset owned by the USB-Serial-JTAG PHY with its D+ pull-up on.
// Hand them to the GPIO matrix first. (The console is UART0 on the module's own USB-C.)
static void releaseUsbJtagPads() {
  USB_SERIAL_JTAG.conf0.pad_pull_override = 1;
  USB_SERIAL_JTAG.conf0.dp_pullup = 0;
  USB_SERIAL_JTAG.conf0.dm_pullup = 0;
  usb_serial_jtag_ll_phy_enable_pad(false);
}

static void lcdHwInit() {
  releaseUsbJtagPads();
  spi_bus_config_t bus = {};
  bus.mosi_io_num = LCD_MOSI_PIN;
  bus.miso_io_num = LCD_MISO_PIN;
  bus.sclk_io_num = LCD_SCK_PIN;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = P4_BOUNCE_PX * 2;
  esp_err_t e = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
  spi_device_interface_config_t dc = {};
  dc.mode = 0;
  dc.clock_speed_hz = LCD_SPI_HZ;
  dc.spics_io_num = -1;                  // CS is ours (held low)
  dc.queue_size = P4_NBUF;
  if (e == ESP_OK) e = spi_bus_add_device(SPI2_HOST, &dc, &s_dev);
  for (int i = 0; i < P4_NBUF; i++)
    s_bnc[i] = (uint16_t *)spi_bus_dma_memory_alloc(SPI2_HOST, P4_BOUNCE_PX * 2, 0);
  s_solidBuf = (uint16_t *)spi_bus_dma_memory_alloc(SPI2_HOST, P4_BOUNCE_PX * 2, 0);
  if (e != ESP_OK || !s_solidBuf || !s_bnc[P4_NBUF - 1]) {
    hal_log("LCD: SPI init failed (%s)", esp_err_to_name(e));
    while (true) delay(1000);
  }
  int khz = 0;
  spi_device_get_actual_freq(s_dev, &khz);
  hal_log("LCD: SPI at %d kHz", khz);

  pinMode(LCD_CS_PIN, OUTPUT);  digitalWrite(LCD_CS_PIN, HIGH);
  pinMode(LCD_DC_PIN, OUTPUT);  digitalWrite(LCD_DC_PIN, HIGH);
  pinMode(LCD_RST_PIN, OUTPUT); digitalWrite(LCD_RST_PIN, HIGH);
  delay(10);                             // ClockworkPi reset timings: 10ms low, 200ms settle
  digitalWrite(LCD_RST_PIN, LOW);  delay(10);
  digitalWrite(LCD_RST_PIN, HIGH); delay(200);
  digitalWrite(LCD_CS_PIN, LOW);         // asserted from here on, for good
}

// ST7365P/ILI9488 init: {cmd, nargs, args...}. Same table as emu8 (MADCTL 0x48, COLMOD 0x55 =
// RGB565, INVON -- the panel needs inversion on).
static const uint8_t kInit[] = {
  0xE0, 15, 0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78, 0x4C, 0x09, 0x0A, 0x08, 0x16, 0x1A, 0x0F,
  0xE1, 15, 0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45, 0x46, 0x04, 0x0E, 0x0D, 0x35, 0x37, 0x0F,
  0xC0, 2, 0x17, 0x15,
  0xC1, 1, 0x41,
  0xC5, 3, 0x00, 0x12, 0x80,
  0x36, 1, 0x48,
  0x3A, 1, 0x55,
  0xB0, 1, 0x00,
  0xB1, 1, 0xA0,
  0x21, 0,
  0xB4, 1, 0x02,
  0xB6, 3, 0x02, 0x02, 0x3B,
  0xB7, 1, 0xC6,
  0xE9, 1, 0x00,
  0xF7, 4, 0xA9, 0x51, 0x2C, 0x82,
};

static void panelFillRawLocked(int x, int y, int w, int h, uint16_t c) {
  if (w <= 0 || h <= 0) return;
  panelWindow(x, y, x + w - 1, y + h - 1);
  txSolid(c, (uint32_t)w * (uint32_t)h);
  txWait();
}

// ------------------------------------------------------------------------------------------------
// video task
// ------------------------------------------------------------------------------------------------
static uint16_t *s_vbuf[2];
static uint8_t   s_back = 0;
static int       s_vy = 0, s_vh = 0;
static SemaphoreHandle_t s_vGo = nullptr, s_vIdle = nullptr;

static void videoTask(void *) {
  for (;;) {
    xSemaphoreTake(s_vGo, portMAX_DELAY);
    const uint16_t *src = s_vbuf[s_back ^ 1] + s_vy * SCREEN_W;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    panelWindow(0, SCREEN_Y0 + s_vy, SCREEN_W - 1, SCREEN_Y0 + s_vy + s_vh - 1);
    txRect(src, SCREEN_W, s_vh, SCREEN_W);
    txWait();
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_vIdle);
  }
}

void hal_video_wait() {
  xSemaphoreTake(s_vIdle, portMAX_DELAY);
  xSemaphoreGive(s_vIdle);
}

uint16_t *hal_video_back() { return s_vbuf[s_back]; }
const uint16_t *hal_video_front() { return s_vbuf[s_back ^ 1]; }

uint16_t *hal_video_submit(int y, int h) {
  if (y < 0) { h += y; y = 0; }
  if (y + h > SCREEN_H) h = SCREEN_H - y;
  if (h <= 0) return s_vbuf[s_back];
  xSemaphoreTake(s_vIdle, portMAX_DELAY);     // the other buffer is free once this returns
  s_vy = y; s_vh = h;
  s_back ^= 1;
  xSemaphoreGive(s_vGo);
  return s_vbuf[s_back];
}

// ------------------------------------------------------------------------------------------------
// synchronous pushes (UI)
// ------------------------------------------------------------------------------------------------
void hal_panel_fill(int x, int y, int w, int h, uint16_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > PANEL_W) w = PANEL_W - x;
  if (y + h > PANEL_H) h = PANEL_H - y;
  hal_video_wait();
  xSemaphoreTake(s_lock, portMAX_DELAY);
  panelFillRawLocked(x, y, w, h, c);
  xSemaphoreGive(s_lock);
}

void hal_display_fill(int x, int y, int w, int h, uint16_t c) {
  if (y < 0) { h += y; y = 0; }
  if (y + h > SCREEN_H) h = SCREEN_H - y;
  hal_panel_fill(x, y + SCREEN_Y0, w, h, c);
}

void hal_panel_push(int x, int y, int w, int h, const uint16_t *px, int stride) {
  if (x < 0) { px -= x; w += x; x = 0; }
  if (y < 0) { px -= y * stride; h += y; y = 0; }
  if (x + w > PANEL_W) w = PANEL_W - x;
  if (y + h > PANEL_H) h = PANEL_H - y;
  if (w <= 0 || h <= 0) return;
  hal_video_wait();
  xSemaphoreTake(s_lock, portMAX_DELAY);
  panelWindow(x, y, x + w - 1, y + h - 1);
  txRect(px, w, h, stride);
  txWait();
  xSemaphoreGive(s_lock);
}

void hal_display_push(int x, int y, int w, int h, const uint16_t *px, int stride) {
  if (y < 0) { px -= y * stride; h += y; y = 0; }
  if (y + h > SCREEN_H) h = SCREEN_H - y;
  hal_panel_push(x, y + SCREEN_Y0, w, h, px, stride);
}

void p4DisplayInit() {
  s_lock = xSemaphoreCreateMutex();
  lcdHwInit();
  for (size_t i = 0; i < sizeof(kInit);) {
    const uint8_t cmd = kInit[i], n = kInit[i + 1];
    wrCmd(cmd);
    wrBytes(&kInit[i + 2], n);
    i += 2 + n;
  }
  wrCmd(0x11); delay(120);               // SLPOUT
  wrCmd(0x29); delay(120);               // DISPON
  panelFillRawLocked(0, 0, PANEL_W, PANEL_H, 0x0000);
  picocalcBacklightOn();

  for (int i = 0; i < 2; i++) s_vbuf[i] = (uint16_t *)hal_alloc_big(SCREEN_W * SCREEN_H * 2);
  s_vGo = xSemaphoreCreateBinary();
  s_vIdle = xSemaphoreCreateBinary();
  xSemaphoreGive(s_vIdle);
  p4CreateTask(videoTask, "video", 4096, 5, nullptr);
}

#endif // BOARD_PICOCALC_P4
