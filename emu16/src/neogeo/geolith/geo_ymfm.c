/*
Copyright (c) 2022-2026 Rupert Carmichael
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ymfm/ymfm.h"
#include "ymfm/ymfm_opn.h"
#include "ymfm/ymfm_ssg.h"

#include "geo.h"
#include "geo_mixer.h"
#include "geo_serial.h"
#include "geo_ymfm.h"
#include "geo_z80.h"

#define SIZE_YMBUF 2048
#define DIVISOR 144 // 144 for medium fidelity, 16 for high

static romdata_t *romdata = NULL;

static int16_t ymbuf[SIZE_YMBUF];
static size_t bufpos;
static int32_t busytimer;
static int32_t timer[2];
static int32_t output[3];

/* emu16: async mode. The samples are made on the other core, a frame late, from the register
   writes the Z80 made, each stamped with the number of samples before it. What the Z80 can see --
   the timers, their IRQ, the busy flag, the SSG registers and the end-of-sample flags -- is kept
   here on the Z80's side ("front"), following ymfm's rules; the end-of-sample flags come back from
   the other side a frame or two late. ngc_job_start/ngc_job_wait (neogeo_core.cpp) run a function
   on the other core.
*/
void ngc_job_start(void (*fn)(void *), void *arg);
void ngc_job_wait(void);

typedef struct { uint16_t samp; uint8_t port, data; } ymev_t;
#define YMEV_MAX 8192 // per frame; the Z80 cannot make more than about 6000 writes in one
#define YMEV_RESET 0x10 // port values past 0-3
#define YMEV_CSM 0x11

static int async;
static ymev_t *evq[2];
static unsigned evbuf, evn; // the buffer being filled, and its count
static unsigned samps; // samples so far this frame

// The job: the other buffer, filled last frame
static unsigned jbuf, jnev, jsamps;
static volatile uint8_t jeos; // end-of-sample flags the job saw rise

// Front state
static uint16_t f_address;
static uint8_t f_reg[4]; // 0x24-0x27
static uint8_t f_ssg[0x0e];
static uint8_t f_status, f_irq_state, f_running[2], f_total_clocks;
static uint8_t f_eos, f_flag_mask = 0xbf;

uint8_t ymfm_external_read(uint32_t type, uint32_t address) {
    switch (type) {
        case ACCESS_ADPCM_A:
            return address > romdata->v1sz ? 0 : romdata->v1[address];
        case ACCESS_ADPCM_B:
            return address > romdata->v2sz ? 0 : romdata->v2[address];
        default:
            return 0;
    }
}

void ymfm_external_write(uint32_t type, uint32_t address, uint8_t data) {
    if (type || address || data) { }
}

void ymfm_sync_mode_write(uint8_t data) {
    fm_engine_mode_write(data);
}

void ymfm_sync_check_interrupts(void) {
    fm_engine_check_interrupts();
}

// emu16: in async mode the front keeps the timers, busy flag and IRQ, so ymfm's are ignored
void ymfm_set_timer(uint32_t tnum, int32_t duration) {
    if (!async)
        timer[tnum] = duration;
}

void ymfm_set_busy_end(uint32_t clocks) {
    // Store full-resolution busy duration
    if (!async)
        busytimer += clocks;
}

bool ymfm_is_busy(void) {
    return busytimer > 0;
}

void ymfm_update_irq(bool asserted) {
    if (!async)
        asserted ? geo_z80_assert_irq(0) : geo_z80_clear_irq();
}

static void ev_push(unsigned port, uint8_t data) {
    if (evn < YMEV_MAX) {
        ymev_t *e = &evq[evbuf][evn++];
        e->samp = samps;
        e->port = port;
        e->data = data;
    }
}

// The front's copy of ymfm's fm_engine timer and status code
static void front_check_irq(void) {
    uint8_t old = f_irq_state;
    f_irq_state = (f_status & (STATUS_TIMERA | STATUS_TIMERB)) != 0;
    if (old != f_irq_state)
        f_irq_state ? geo_z80_assert_irq(0) : geo_z80_clear_irq();
}

static void front_set_reset_status(uint8_t set, uint8_t reset) {
    f_status = (f_status | set) & ~(reset | STATUS_BUSY);
    front_check_irq();
}

static void front_update_timer(unsigned tnum, unsigned enable, int32_t delta) {
    if (enable && !f_running[tnum]) {
        int32_t period = (tnum == 0) ?
            1024 - ((f_reg[0] << 2) | (f_reg[1] & 0x03)) : 16 * (256 - f_reg[2]);
        timer[tnum] = (period + delta) * DIVISOR; // OPERATORS * prescale
        f_running[tnum] = 1;
    }
    else if (!enable) {
        timer[tnum] = -1;
        f_running[tnum] = 0;
    }
}

static void front_timer_expired(unsigned tnum) {
    uint8_t mode = f_reg[3];
    if (tnum == 0 && (mode & 0x04))
        front_set_reset_status(STATUS_TIMERA, 0);
    else if (tnum == 1 && (mode & 0x08))
        front_set_reset_status(STATUS_TIMERB, 0);

    if (tnum == 0 && (mode >> 6) == 2) // CSM
        ev_push(YMEV_CSM, 0);

    f_running[tnum] = 0;
    front_update_timer(tnum, 1, 0);
}

static void front_mode_write(uint8_t data) {
    f_reg[3] = data;
    uint8_t reset = 0;
    if (data & 0x20)
        reset |= STATUS_TIMERB;
    if (data & 0x10)
        reset |= STATUS_TIMERA;
    front_set_reset_status(0, reset);
    front_update_timer(1, data & 0x02, -(f_total_clocks & 15));
    front_update_timer(0, data & 0x01, 0);
}

static void front_reset(void) {
    front_set_reset_status(0, 0xff);
    memset(f_reg, 0, sizeof(f_reg));
    front_mode_write(0);
    memset(f_ssg, 0, sizeof(f_ssg));
    f_eos = 0;
    f_flag_mask = 0xbf;
}

// Take the front state from the chip (after a state load, or when going async)
static void front_from_chip(void) {
    uint8_t ts[5];
    fm_engine_get_timer_state(ts);
    f_status = ts[0];
    f_irq_state = ts[1];
    f_running[0] = ts[2];
    f_running[1] = ts[3];
    f_total_clocks = ts[4];
    for (unsigned i = 0; i < 4; ++i)
        f_reg[i] = ym2610_peek(0x24 + i);
    for (unsigned i = 0; i < sizeof(f_ssg); ++i)
        f_ssg[i] = ssg_engine_read(i);
    f_address = ym2610_get_address();
    f_eos = ym2610_get_eos() & 0xbf;
    f_flag_mask = ym2610_get_flag_mask();
}

static void front_to_chip(void) {
    uint8_t ts[5] = {
        f_status, f_irq_state, f_running[0], f_running[1], f_total_clocks
    };
    fm_engine_set_timer_state(ts);
}

static void ev_apply(const ymev_t *e) {
    if (e->port < 4)
        ym2610_write(e->port, e->data);
    else if (e->port == YMEV_RESET)
        ym2610_reset();
    else if (e->port == YMEV_CSM)
        fm_engine_csm_trigger();
}

static inline void geo_ymfm_timer_tick(void) {
    if (busytimer > 0) {
        busytimer -= DIVISOR;
        if (busytimer < 0)
            busytimer = 0;
    }

    for (int i = 0; i < 2; ++i) {
        if (timer[i] < 0)
            continue;
        timer[i] -= DIVISOR;
        if (timer[i] <= 0) {
            if (async)
                front_timer_expired(i);
            else
                fm_engine_timer_expired(i);
        }
    }
}

// Grab the pointer to the buffer
int16_t* geo_ymfm_get_buffer(void) {
    bufpos = 0;
    return &ymbuf[0];
}

// Initialize the YM2610
void geo_ymfm_init(void) {
    ym2610_init();
    ym2610_set_fidelity(OPN_FIDELITY_MED);
    fm_engine_init();
    romdata = geo_romdata_ptr();
}

// Perform a reset - required at least once before clocking
void geo_ymfm_reset(void) {
    geo_ymfm_chip_reset();
}

// Mix FM and SSG channels while maintaining a value within the int16_t range
static inline int16_t mix(int32_t samp0, int32_t samp1) {
    if (samp0 + samp1 >= 32767)
        return 32767;
    else if (samp0 + samp1 <= -32768)
        return -32768;
    return samp0 + samp1;
}

static inline void geo_ymfm_sample(void) {
    ym2610_generate(output);

    // Mix stereo FM/ADPCM output (0,1) with mono SSG output (2)
    if (bufpos < SIZE_YMBUF) {
        ymbuf[bufpos++] = mix(output[0], output[2]);
        ymbuf[bufpos++] = mix(output[1], output[2]);
    }
}

// Clock the YM2610
size_t geo_ymfm_exec(void) {
    geo_ymfm_timer_tick();
    if (async) { // emu16: the sample is made later, on the other core
        ++f_total_clocks;
        ++samps;
        return 1;
    }
    geo_ymfm_sample();
    return 1;
}

// emu16: the YM2610 ports, as the Z80 sees them
uint8_t geo_ymfm_read(uint32_t port) {
    if (!async)
        return ym2610_read(port);

    switch (port & 3) {
        case 0: // status
            return (f_status & (STATUS_TIMERA | STATUS_TIMERB)) |
                (busytimer > 0 ? STATUS_BUSY : 0);
        case 1: // data (only SSG)
            if (f_address < 0x0e)
                return f_ssg[f_address];
            if (f_address < 0x10)
                return 0xff;
            return f_address == 0xff ? 1 : 0;
        case 2: // end-of-sample flags
            return f_eos & f_flag_mask;
        default:
            return 0;
    }
}

void geo_ymfm_write(uint32_t port, uint8_t data) {
    if (!async) {
        ym2610_write(port, data);
        return;
    }

    switch (port & 3) {
        case 0:
            f_address = data;
            break;
        case 2:
            f_address = 0x100 | data;
            break;
        case 1:
            if (f_address & 0x100)
                break;
            if (f_address < 0x0e) {
                f_ssg[f_address] = data;
            }
            else if (f_address == 0x1c) {
                f_flag_mask = ~data & 0xbf;
                f_eos &= ~(data & 0xbf);
            }
            else if (f_address >= 0x24 && f_address <= 0x26) {
                f_reg[f_address - 0x24] = data;
            }
            else if (f_address == 0x27) {
                front_mode_write(data);
            }
            busytimer += 32 * fm_engine_clock_prescale();
            break;
        case 3:
            if (!(f_address & 0x100))
                break;
            busytimer += 32 * fm_engine_clock_prescale();
            break;
    }
    ev_push(port & 3, data);
}

// emu16: reset the chip (the Z80's reset line)
void geo_ymfm_chip_reset(void) {
    if (!async) {
        ym2610_reset();
        return;
    }
    front_reset();
    ev_push(YMEV_RESET, 0);
}

// emu16: the job -- last frame's samples, then the audio out
static void geo_ymfm_render(void *arg) {
    (void)arg;
    const ymev_t *ev = evq[jbuf];
    unsigned j = 0;
    uint8_t prev = ym2610_get_eos(), rise = 0;
    bufpos = 0;

    for (unsigned s = 0; s < jsamps; ++s) {
        while (j < jnev && ev[j].samp == s) {
            ev_apply(&ev[j++]);
            prev = ym2610_get_eos();
        }
        geo_ymfm_sample();
        uint8_t e = ym2610_get_eos();
        rise |= e & ~prev;
        prev = e;
    }
    while (j < jnev)
        ev_apply(&ev[j++]);

    jeos |= rise;
    geo_mixer_output(jsamps);
}

// emu16: end of a frame: hand its writes to the other core (or mix now, when not async)
void geo_ymfm_frame_end(size_t n) {
    if (!async) {
        geo_mixer_output(n);
        return;
    }
    geo_ymfm_sync();
    jbuf = evbuf;
    jnev = evn;
    jsamps = n;
    evbuf ^= 1;
    evn = 0;
    samps = 0;
    ngc_job_start(geo_ymfm_render, NULL);
}

// emu16: wait for the other core; between frames, also apply writes made since (a reset)
void geo_ymfm_sync(void) {
    if (!async)
        return;
    ngc_job_wait();
    f_eos |= jeos & 0xbf;
    jeos = 0;
    if (samps == 0 && evn) {
        for (unsigned j = 0; j < evn; ++j)
            ev_apply(&evq[evbuf][j]);
        evn = 0;
    }
}

int geo_ymfm_set_async(int on) {
    if (on == async)
        return 1;
    if (on) {
        for (int i = 0; i < 2; ++i) {
            if (!evq[i])
                evq[i] = (ymev_t*)malloc(YMEV_MAX * sizeof(ymev_t));
            if (!evq[i])
                return 0;
        }
        evn = samps = 0;
        jeos = 0;
        front_from_chip();
        async = 1;
    }
    else {
        geo_ymfm_sync();
        front_to_chip();
        async = 0;
        for (int i = 0; i < 2; ++i) {
            free(evq[i]);
            evq[i] = NULL;
        }
    }
    return 1;
}

// Hacks
void geo_ymfm_adpcm_wrap(int w) {
    adpcm_a_set_accum_wrap(w);
}

// States
void geo_ymfm_state_load(uint8_t *st, unsigned ver) {
    geo_ymfm_sync();
    evn = 0;
    busytimer = geo_serial_pop32(st);
    if (ver == 0x00) {
        busytimer *= DIVISOR; // Best effort
        geo_serial_pop32(st); // Empty, was busyfrac
    }
    timer[0] = geo_serial_pop32(st);
    timer[1] = geo_serial_pop32(st);
    opn_state_load(st);
    adpcm_state_load(st);
    ssg_state_load(st);
    if (async)
        front_from_chip();
}

void geo_ymfm_state_save(uint8_t *st) {
    geo_ymfm_sync();
    if (async)
        front_to_chip();
    geo_serial_push32(st, busytimer);
    geo_serial_push32(st, timer[0]);
    geo_serial_push32(st, timer[1]);
    opn_state_save(st);
    adpcm_state_save(st);
    ssg_state_save(st);
}
