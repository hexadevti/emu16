// system_p4.cpp -- boot order, timing, memory, SD card and the esp32loader hand-off on the P4.
#include "../../../board.h"

#if defined(BOARD_PICOCALC_P4)

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <stdarg.h>
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "esp_ldo_regulator.h"
#include "soc/soc.h"
#include "soc/lp_system_reg.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_core_dump.h"
#include "esp_cpu.h"
#include "../hal.h"

bool picocalcWaitForMainboard();   // input_p4.cpp
void p4DisplayInit();              // display_p4.cpp
void p4InputInit();                // input_p4.cpp
void p4AudioInit();                // audio_p4.cpp

#define ESP32LOADER_MAGIC 0x10ADE125u   // esp32loader main/loader_api.h: boot the loader once

static SPIClass s_sdSpi(HSPI);
static bool s_sdOk = false;

uint32_t hal_millis()          { return millis(); }
uint32_t hal_micros()          { return micros(); }
uint32_t hal_cycles()          { return (uint32_t)esp_cpu_get_cycle_count(); }
uint32_t hal_cycles_per_us()   { return getCpuFrequencyMhz(); }

// The job task sits on core 0 under the video (5) and keyboard (4) tasks, which mostly wait.
static SemaphoreHandle_t s_jobGo = nullptr, s_jobIdle = nullptr;
static void (*volatile s_jobFn)(void *) = nullptr;
static void *volatile s_jobArg = nullptr;
static TaskHandle_t s_jobTask = nullptr;

void p4AudioWaitAdd(uint32_t us);   // audio_p4.cpp

// A task on core 0 with its stack in the main SRAM. A plain xTaskCreate stack can land in the
// 32 KB LP SRAM once the main SRAM runs short, where every stack access crawls (the YM2610 job
// ran 10x slower there); LP SRAM is not DMA capable, so asking for DMA memory rules it out.
void p4CreateTask(TaskFunction_t fn, const char *name, uint32_t bytes, UBaseType_t prio, TaskHandle_t *h) {
  StackType_t *stack = (StackType_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  StaticTask_t *tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  TaskHandle_t t = nullptr;
  if (stack && tcb) t = xTaskCreateStaticPinnedToCore(fn, name, bytes, nullptr, prio, stack, tcb, 0);
  if (!t) {
    free(stack); free(tcb);
    hal_log("%s: no main SRAM for its stack", name);
    xTaskCreatePinnedToCore(fn, name, bytes, nullptr, prio, &t, 0);
  }
  if (h) *h = t;
}
bool p4InJob() { return s_jobTask && xTaskGetCurrentTaskHandle() == s_jobTask; }

static void jobTask(void *) {
  for (;;) {
    xSemaphoreTake(s_jobGo, portMAX_DELAY);
    s_jobFn(s_jobArg);
    xSemaphoreGive(s_jobIdle);
  }
}

void hal_job_wait() {
  if (!s_jobIdle) return;
  const uint32_t t0 = micros();
  xSemaphoreTake(s_jobIdle, portMAX_DELAY);
  xSemaphoreGive(s_jobIdle);
  p4AudioWaitAdd(micros() - t0);
}

// Made at boot, while the main SRAM still has room for its stack (see p4CreateTask).
static void jobInit() {
  s_jobGo = xSemaphoreCreateBinary();
  s_jobIdle = xSemaphoreCreateBinary();
  xSemaphoreGive(s_jobIdle);
  p4CreateTask(jobTask, "job", 8192, 3, &s_jobTask);
}

void hal_job_start(void (*fn)(void *), void *arg) {
  if (!s_jobGo) jobInit();
  static bool wdtOff = false;
  if (!wdtOff) {
    // jobs can keep core 0 busy for long stretches, which would starve its idle task and trip
    // the task watchdog: stop it watching the idle tasks. (disableCore0WDT() leaves the idle
    // hook feeding a task it no longer knows, which logs an error on every idle tick.)
    wdtOff = true;
    esp_task_wdt_config_t c = {};
    c.timeout_ms = 5000;
    c.idle_core_mask = 0;
    c.trigger_panic = true;
    esp_task_wdt_reconfigure(&c);
  }
  const uint32_t t0 = micros();
  xSemaphoreTake(s_jobIdle, portMAX_DELAY);
  p4AudioWaitAdd(micros() - t0);
  s_jobFn = fn;
  s_jobArg = arg;
  xSemaphoreGive(s_jobGo);
}
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

// Plain (not RTC) noinit RAM keeps its contents through a panic reset, not through power-off;
// the magic tells a real breadcrumb from power-on garbage.
#define CRUMB_MAGIC 0xC0FFEE16u
#define PANIC_MAGIC 0xDEAD0016u
#define PANIC_BT    12
static __NOINIT_ATTR uint32_t s_crumbMagic;
static __NOINIT_ATTR char     s_crumb[96];
static __NOINIT_ATTR uint32_t s_panicMagic;
static __NOINIT_ATTR uint32_t s_panicPc, s_panicBt[PANIC_BT];
static __NOINIT_ATTR char     s_panicWhy[48];

static bool isCode(uint32_t v) {
  return (v >= 0x40000000u && v < 0x44000000u) || (v >= 0x4FF00000u && v < 0x4FFC0000u);
}

// The board's partition table (esp32loader's) may have no core dump partition, so a panic
// handler keeps the crash PC, the reason and the code addresses found on the stack in noinit RAM.
// The Arduino core calls it from its esp_panic_handler wrapper, before the reboot.
static void onPanic(arduino_panic_info_t *info, void *) {
  s_panicPc = (uint32_t)info->pc;
  memset(s_panicBt, 0, sizeof(s_panicBt));
  int n = 0;
  for (unsigned i = 1; i < info->backtrace_len && n < PANIC_BT; i++)   // [0] is the SP
    if (isCode(info->backtrace[i])) s_panicBt[n++] = info->backtrace[i];
  const char *why = info->reason ? info->reason : "";
  size_t i = 0;
  for (; i < sizeof(s_panicWhy) - 1 && why[i]; i++) s_panicWhy[i] = why[i];
  s_panicWhy[i] = 0;
  s_panicMagic = PANIC_MAGIC;
}

void hal_breadcrumb(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s_crumb, sizeof(s_crumb), fmt, ap);
  va_end(ap);
  s_crumbMagic = CRUMB_MAGIC;
}

bool hal_crash_report(char *buf, size_t n) {
  const esp_reset_reason_t r = esp_reset_reason();
  const char *why = r == ESP_RST_PANIC    ? "crash (exception)"
                  : r == ESP_RST_INT_WDT  ? "interrupt watchdog"
                  : r == ESP_RST_TASK_WDT ? "task watchdog"
                  : r == ESP_RST_WDT      ? "watchdog"
                  : r == ESP_RST_BROWNOUT ? "brownout (power dip)" : nullptr;
  const bool crumb = s_crumbMagic == CRUMB_MAGIC, panic = s_panicMagic == PANIC_MAGIC;
  s_crumbMagic = s_panicMagic = 0;
  if (!why) return false;
  s_crumb[sizeof(s_crumb) - 1] = 0;
  int len = snprintf(buf, n, "%s, during: %s.", why, crumb ? s_crumb : "?");
  if (panic) {
    s_panicWhy[sizeof(s_panicWhy) - 1] = 0;
    len += snprintf(buf + len, n - len, " | %s, pc %08lx, stack", s_panicWhy, (unsigned long)s_panicPc);
    for (int i = 0; i < PANIC_BT && s_panicBt[i] && len < (int)n - 10; i++)
      len += snprintf(buf + len, n - len, " %08lx", (unsigned long)s_panicBt[i]);
    return true;
  }
  // The core dump in flash says where: the PC, the return address, and code addresses found on
  // the stack (a poor man's backtrace; RISC-V can't unwind on the device). addr2line them.
  static esp_core_dump_summary_t sum;      // ~1.2 KB, too big for the stack
  if (esp_core_dump_image_check() == ESP_OK && esp_core_dump_get_summary(&sum) == ESP_OK) {
    len += snprintf(buf + len, n - len, " | task %s pc %08lx ra %08lx cause %lu val %08lx bt",
                    sum.exc_task, (unsigned long)sum.exc_pc, (unsigned long)sum.ex_info.ra,
                    (unsigned long)sum.ex_info.mcause, (unsigned long)sum.ex_info.mtval);
    int found = 0;
    for (uint32_t i = 0; i + 4 <= sum.exc_bt_info.dump_size && found < 12 && len < (int)n - 10; i += 4) {
      uint32_t v;
      memcpy(&v, sum.exc_bt_info.stackdump + i, 4);
      if (v >= 0x40000000u && v < 0x44000000u) { len += snprintf(buf + len, n - len, " %08lx", (unsigned long)v); found++; }
    }
    esp_core_dump_image_erase();
  }
  return true;
}

void hal_reboot_to_loader() {
  hal_log("rebooting to esp32loader");
  delay(50);
  REG_WRITE(LP_SYSTEM_REG_LP_STORE15_REG, ESP32LOADER_MAGIC);   // without the loader: plain restart
  esp_restart();
}

// A ROM to start right after the next boot ("@run"), kept across the restart.
#define LAUNCH_MAGIC 0x52554E16u
static __NOINIT_ATTR uint32_t s_launchMagic;
static __NOINIT_ATTR char     s_launch[160];

bool hal_take_launch(char *path, size_t n) {
  if (s_launchMagic != LAUNCH_MAGIC) return false;
  s_launchMagic = 0;
  s_launch[sizeof(s_launch) - 1] = 0;
  snprintf(path, n, "%s", s_launch);
  return path[0] != 0;
}

void p4InjectKey(uint8_t code, uint8_t state);   // input_p4.cpp

// Commands from the PC over the console UART, one per line:
//   @uf2menu       reboot into esp32loader (emu8 tools/deploy-p4-wifi.py et al.)
//   @run <path>    restart and start that ROM, e.g. @run /sd/roms/neogeo/trally.zip
//   @key <hex>     press and release a key (hal.h KEY_* codes), e.g. @key b1 for Esc
//   @reboot        plain restart
void p4CheckSerialCmd() {
  static char line[180];
  static uint8_t len = 0;
  while (Serial.available()) {
    const int c = Serial.read();
    if (c < 0) break;
    if (c != '\n' && c != '\r') { if (len < sizeof(line) - 1) line[len++] = (char)c; continue; }
    line[len] = 0;
    len = 0;
    if (!strcmp(line, "@uf2menu")) hal_reboot_to_loader();
    else if (!strcmp(line, "@reboot")) hal_reboot();
    else if (!strncmp(line, "@run ", 5)) {
      snprintf(s_launch, sizeof(s_launch), "%s", line + 5);
      s_launchMagic = LAUNCH_MAGIC;
      hal_log("run %s: restarting", s_launch);
      delay(50);
      esp_restart();
    } else if (!strncmp(line, "@key ", 5)) {
      const uint8_t k = (uint8_t)strtoul(line + 5, nullptr, 16);
      p4InjectKey(k, KEYSTATE_PRESSED);
      vTaskDelay(pdMS_TO_TICKS(60));
      p4InjectKey(k, KEYSTATE_RELEASED);
    }
  }
}

void *hal_alloc_big(size_t n) {
  void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : calloc(1, n);
}
// "Internal" includes the 32 KB LP SRAM, which is slower than PSRAM through the cache: not that.
static bool isLpRam(const void *p) { return (uintptr_t)p >= 0x50100000u && (uintptr_t)p < 0x50200000u; }
void *hal_alloc_fast(size_t n) {
  void *p = heap_caps_calloc(1, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (p && isLpRam(p)) { free(p); p = heap_caps_calloc(1, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT); }
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
  set_arduino_panic_handler(onPanic, nullptr);
  Serial.begin(115200);
  delay(50);
  hal_log("emu16 on " BOARD_NAME ", PSRAM %u KB free, internal %u KB free",
          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10),
          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10));
  const bool board = picocalcWaitForMainboard();
  p4DisplayInit();
  p4AudioInit();                     // before jobInit: both want DMA-capable SRAM
  jobInit();
  if (board) sdInit();
  p4InputInit();
}

#endif // BOARD_PICOCALC_P4
