// neogeo_nocd.c -- stands in for Geolith's disc layer (geo_disc/geo_cue/geo_chd), which emu16
// does not ship: it is cartridges only. geo_cd.c and geo_lc8951.c stay vendored because the
// cartridge code calls into them behind `if (ngsys.cdmode)`, which is never set here.
#include <stdint.h>
#include "geolith/geo_disc.h"

int geo_disc_open(const char *path) { (void)path; return 0; }
void geo_disc_close(void) {}
int geo_disc_read_sector(uint32_t disc_lba, uint8_t *buf) { (void)disc_lba; (void)buf; return 0; }
int geo_disc_read_audio(uint32_t disc_lba, int16_t *buf) { (void)disc_lba; (void)buf; return 0; }
unsigned geo_disc_num_tracks(void) { return 0; }
int geo_disc_track_is_audio(unsigned track) { (void)track; return 0; }
uint32_t geo_disc_track_start(unsigned track) { (void)track; return 0; }
uint32_t geo_disc_track_frames(unsigned track) { (void)track; return 0; }
uint32_t geo_disc_leadout(void) { return 0; }

void geo_disc_lba_to_msf(uint32_t lba, uint8_t *m, uint8_t *s, uint8_t *f) {
  lba += 150;
  *m = (uint8_t)(lba / (60 * 75));
  *s = (uint8_t)((lba / 75) % 60);
  *f = (uint8_t)(lba % 75);
}
uint32_t geo_disc_msf_to_lba(uint8_t m, uint8_t s, uint8_t f) { return ((uint32_t)m * 60 + s) * 75 + f - 150; }

// Musashi's disassembler (m68kdasm.c) reads memory through these host callbacks. Geolith never
// disassembles, so they are only here to satisfy the linker.
#include "geolith/m68k/geo_m68k_prefix.h"
unsigned m68k_read_disassembler_16(unsigned address) { (void)address; return 0; }
unsigned m68k_read_disassembler_32(unsigned address) { (void)address; return 0; }
