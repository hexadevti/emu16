#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

// emu16: logging, memory and the CRC come from the frontend (src/pce/pce_core.cpp)
void osd_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void *osd_alloc_fast(size_t n);   // zeroed; internal RAM when it fits
void osd_free(void *p);
uint32_t crc32_le(uint32_t crc, const uint8_t *buf, size_t len);
#define LOG_PRINTF(level, x...) osd_log(x)

#define MESSAGE_ERROR(x...) LOG_PRINTF(1, "!! " x)
#define MESSAGE_WARN(x...)  LOG_PRINTF(2, " ! " x)
#define MESSAGE_INFO(x...)  LOG_PRINTF(3, " * " x)
#define MESSAGE_TRACE(tag, x...) LOG_PRINTF(4, " & (" tag ") " x)
#if ENABLE_DEBUG
#define MESSAGE_DEBUG(x...) LOG_PRINTF(4, " > " x)
#else
#define MESSAGE_DEBUG(x...) {}
#endif

#if ENABLE_SPR_TRACING
#define TRACE_SPR(x...) MESSAGE_TRACE("SPR", x)
#else
#define TRACE_SPR(x...) {}
#endif

#if ENABLE_GFX_TRACING
#define TRACE_GFX(x...) MESSAGE_TRACE("GFX", x)
#else
#define TRACE_GFX(x...) {}
#endif

#if ENABLE_IO_TRACING
#define TRACE_IO(x...) MESSAGE_TRACE("IO", x)
#else
#define TRACE_IO(x...) {}
#endif

#if ENABLE_CPU_TRACING
#define TRACE_CPU(x...) MESSAGE_TRACE("CPU", x)
#else
#define TRACE_CPU(x...) {}
#endif

#undef MIN
#define MIN(a,b) ({__typeof__(a) _a = (a); __typeof__(b) _b = (b);_a < _b ? _a : _b; })
#undef MAX
#define MAX(a,b) ({__typeof__(a) _a = (a); __typeof__(b) _b = (b);_a > _b ? _a : _b; })

#define JOY_A       0x01
#define JOY_B       0x02
#define JOY_SELECT  0x04
#define JOY_RUN     0x08
#define JOY_UP      0x10
#define JOY_RIGHT   0x20
#define JOY_DOWN    0x40
#define JOY_LEFT    0x80

// We need scratch area on both side of each line, shared by adjacent lines.
// emu16: 32 bytes, not 16, as sprites start up to 31 pixels left of the line.
// The buffer should look like [32 bytes] [line 1] [32 bytes] ... [32 bytes] [line 242] [32 bytes]
#define XBUF_SCRATCH	32
#define XBUF_WIDTH 	(352 + XBUF_SCRATCH)
#define	XBUF_HEIGHT	(242)

// emu16: states go through the frontend's FILE; the caller owns the ROM buffer, which must be
// readable up to the next power of two of 8 KB banks plus 16 bytes.
int LoadState(FILE *fp);
int SaveState(FILE *fp);
void ResetPCE(bool);
void ShutdownPCE();
int InitPCE(int samplerate, bool stereo);
int LoadCard(uint8_t *data, size_t size);
void PalettePCE565(uint16_t palette[256]);

// emu16: one frame is pce_frame_begin() then pce_run_line() for each of PCE_LINES lines
#define PCE_LINES 263
void pce_frame_begin(void);
void pce_run_line(void);

extern uint8_t *osd_gfx_framebuffer(int width, int height);
