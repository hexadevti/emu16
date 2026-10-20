/* SPDX-License-Identifier: BSD-3-Clause
 * liblithogen - region construction.
 *
 * Replicates MAME's romload semantics (see MAME src/emu/romload.cpp):
 *   1. copy phase: each load writes `group` bytes at a time (reversed
 *      within the group if the load is a _SWAP variant), skipping `skip`
 *      region bytes after each group; ROM_CONTINUE resumes the same file
 *      at a new region offset; ROM_IGNORE discards file bytes.
 *   2. post phase: 16-bit big-endian regions (maincpu) are byte-pair
 *      swapped, emulating a little-endian host.  The result places
 *      standard 16_WORD_SWAP P data in raw file order, which is both the
 *      layout the ported decryption code expects through uint16_t access
 *      on a little-endian machine and the layout the .neo format stores.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lithogen_internal.h"

static int region_index(const nc_game *g, nc_region_kind kind)
{
    int i;
    for (i = 0; i < g->nregions; i++)
        if (g->regions[i].kind == kind)
            return i;
    return -1;
}

/* Region bytes spanned by a load of `len` file bytes in group/skip mode:
 * the trailing skip after the final group is not written. */
static uint32_t load_span(uint32_t len, unsigned group, unsigned skip)
{
    uint32_t ngroups;
    if (len == 0 || group == 0)
        return 0;
    ngroups = (len + group - 1) / group;
    return (ngroups - 1) * (group + skip) + group;
}

static void copy_groups(uint8_t *dst, uint32_t dstoff, uint32_t dstsize,
                        const uint8_t *src, uint32_t len,
                        unsigned group, unsigned skip, int reverse)
{
    uint32_t s = 0;
    uint32_t d = dstoff;
    while (s < len) {
        unsigned g = group;
        unsigned j;
        if (g > len - s)
            g = (unsigned)(len - s);
        for (j = 0; j < g; j++) {
            uint32_t off = d + (reverse ? (g - 1 - j) : j);
            if (off < dstsize)
                dst[off] = src[s + j];
        }
        s += g;
        d += g + skip;
    }
}

/* emu16: low-memory mode (lithogen_options.mem_avail) ----------------- */

#include "../miniz/miniz.h"

/* carts whose recipe leaves C and V alone */
static int lowmem_cart(const char *cart)
{
    static const char *const ok[] = { "rom", "rom_vliner", "rom_fatfur2",
                                      "rom_mslugx", "rom_kof98" };
    size_t i;
    for (i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
        if (!strcmp(cart, ok[i]))
            return 1;
    return 0;
}

/* A C region dedup can stream: only odd/even file pairs (LOAD16_BYTE,
 * the first at an even offset, both the same length) whose interleaved
 * images are whole 32-byte chunks in order inside the region. */
static int c_dedup_ok(const nc_game *g, const nc_region *reg)
{
    uint32_t end = 0;
    uint16_t li;
    if (!reg->nloads || (reg->nloads & 1) || (reg->size & 31))
        return 0;
    for (li = 0; li < reg->nloads; li += 2) {
        const nc_load *a = &g->loads[reg->load0 + li];
        const nc_load *b = a + 1;
        if (a->kind != NC_LOAD || b->kind != NC_LOAD ||
            a->group != 1 || a->skip != 1 || a->reverse ||
            b->group != 1 || b->skip != 1 || b->reverse ||
            a->optional || b->optional ||
            b->offset != a->offset + 1 || a->length != b->length ||
            (a->offset & 31) || ((a->length * 2) & 31) || a->offset < end ||
            a->offset + a->length * 2 > reg->size)
            return 0;
        end = a->offset + a->length * 2;
    }
    return 1;
}

/* A V region spill can follow: file loads only (a ROM_COPY reads another
 * region's data, which a spilled V region no longer has in memory). */
static int v_spill_ok(const nc_game *g, const nc_region *reg)
{
    uint16_t li;
    for (li = 0; li < reg->nloads; li++) {
        const nc_load *ld = &g->loads[reg->load0 + li];
        if (ld->kind == NC_COPY || ld->kind == NC_FILL)
            return 0;
    }
    return 1;
}

/* what the caller's spill cache is keyed on: the region's loads */
static uint32_t v_spill_key(const nc_game *g, const nc_region *reg)
{
    uint32_t key = (uint32_t)mz_crc32(MZ_CRC32_INIT,
                                      (const unsigned char *)g->name,
                                      strlen(g->name));
    uint16_t li;
    for (li = 0; li < reg->nloads; li++) {
        const nc_load *ld = &g->loads[reg->load0 + li];
        uint32_t w[5];
        w[0] = ld->kind | (uint32_t)ld->group << 8 |
               (uint32_t)ld->skip << 16 | (uint32_t)ld->reverse << 24;
        w[1] = ld->offset;
        w[2] = ld->length;
        w[3] = ld->crc;
        w[4] = reg->size;
        key = (uint32_t)mz_crc32(key, (const unsigned char *)w, sizeof(w));
    }
    return key;
}

static uint32_t chunk_hash(const uint8_t *c)
{
    uint32_t h = 0x9e3779b9u, w;
    int i;
    for (i = 0; i < 32; i += 4) {
        memcpy(&w, c + i, 4);
        h = (h ^ w) * 0x85ebca6bu;
        h ^= h >> 15;
    }
    return h;
}

/* Streams the C file pairs out of the zip and keeps each distinct 32-byte
 * chunk (a quarter tile) once: many games repeat tiles, and aof3's 28 MB
 * C ROM holds 18.6 MB of distinct ones.  The pool takes the largest block
 * the heap has (less room for the zip streams) and is trimmed after.
 * The hash table is lossy - a chunk whose slot another chunk took may be
 * stored twice - which costs little and keeps the table small. */
static int build_c_dedup(const nc_game *g, const nc_region *reg,
                         nc_zipset *zs, nc_regions *r,
                         const lithogen_options *opt, lithogen_report *rep)
{
    enum { IN = 4096, SLACK = 0x80000 };
    const uint32_t nchunks = reg->size / 32;
    uint32_t hbits = 12, hmask, used = 0, cap;
    uint32_t *cidx, *htab;
    uint8_t *pool = NULL, *ina, *inb, *win;
    uint16_t li;
    int st = LITHOGEN_ERR_NOMEM;

    while (hbits < 18 && (1u << hbits) < nchunks / 4)
        hbits++;
    hmask = (1u << hbits) - 1;
    cidx = (uint32_t *)malloc((size_t)nchunks * 4);
    htab = (uint32_t *)malloc((size_t)4 << hbits);
    ina = (uint8_t *)malloc(IN);
    inb = (uint8_t *)malloc(IN);
    win = (uint8_t *)malloc(IN * 2);
    if (!cidx || !htab || !ina || !inb || !win)
        goto out;
    /* the largest pool the heap gives, then room back for the streams */
    for (cap = reg->size; cap >= SLACK * 2; cap -= SLACK)
        if ((pool = (uint8_t *)malloc(cap)) != NULL)
            break;
    if (!pool)
        goto out;
    if (cap != reg->size) {
        uint8_t *p = (uint8_t *)realloc(pool, cap - SLACK);
        if (p) {
            pool = p;
            cap -= SLACK;
        }
    }
    memset(htab, 0xff, (size_t)4 << hbits);
    /* chunk 0: the erase fill, where unloaded parts of the region point */
    memset(pool, reg->erasefill, 32);
    htab[chunk_hash(pool) & hmask] = 0;
    used = 32;
    memset(cidx, 0, (size_t)nchunks * 4);

    for (li = 0; li < reg->nloads; li += 2) {
        const nc_load *la = &g->loads[reg->load0 + li];
        const nc_load *lb = la + 1;
        size_t alen = 0, blen = 0;
        uint32_t acrc = 0, bcrc = 0, done = 0, ci = la->offset / 32;
        nc_zipstream *sa, *sb = NULL;
        int ok, cut = 0;

        sa = nc_zipset_stream(zs, la->file, la->crc, la->length, &alen, &acrc);
        if (sa)
            sb = nc_zipset_stream(zs, lb->file, lb->crc, lb->length,
                                  &blen, &bcrc);
        if (!sa || !sb) {
            const nc_load *m = sa ? lb : la;
            nc_zipstream_close(sa);
            snprintf(rep->errstr, LITHOGEN_ERRSTR_MAX,
                     "missing ROM: %s (crc %08x) for set %s",
                     m->file, m->crc, g->name);
            st = LITHOGEN_ERR_MISSING_ROM;
            goto out;
        }
        if (acrc != la->crc || bcrc != lb->crc) {
            const nc_load *m = acrc != la->crc ? la : lb;
            rep->crc_mismatches++;
            nc_warn(opt, "%s: crc %08x, expected %08x", m->file,
                    acrc != la->crc ? acrc : bcrc, m->crc);
            if (opt->strict_crc) {
                nc_zipstream_close(sa);
                nc_zipstream_close(sb);
                snprintf(rep->errstr, LITHOGEN_ERRSTR_MAX,
                         "CRC mismatch on %s", m->file);
                st = LITHOGEN_ERR_CRC;
                goto out;
            }
        }
        while (done < la->length && !cut) {
            uint32_t n = la->length - done, i;
            size_t ga, gb;
            if (n > IN)
                n = IN;
            ga = nc_zipstream_read(sa, ina, n);
            gb = nc_zipstream_read(sb, inb, n);
            if (ga < n || gb < n) {
                /* a short file: the rest of the pair stays erase fill */
                nc_warn(opt, "%s: file is shorter than %u bytes",
                        ga < n ? la->file : lb->file, la->length);
                memset(ina + ga, reg->erasefill, n - ga);
                memset(inb + gb, reg->erasefill, n - gb);
                cut = 1;
            }
            for (i = 0; i < n; i++) {
                win[i * 2] = ina[i];
                win[i * 2 + 1] = inb[i];
            }
            for (i = 0; i < n * 2; i += 32, ci++) {
                const uint8_t *c = win + i;
                uint32_t *h = &htab[chunk_hash(c) & hmask];
                if (*h != 0xffffffffu && !memcmp(pool + *h, c, 32)) {
                    cidx[ci] = *h;
                    continue;
                }
                if (used + 32 > cap) {
                    nc_zipstream_close(sa);
                    nc_zipstream_close(sb);
                    goto out;
                }
                memcpy(pool + used, c, 32);
                *h = cidx[ci] = used;
                used += 32;
            }
            done += n;
        }
        /* a stream read to its end reports bad data on close */
        ok = nc_zipstream_close(sa) | (alen > la->length) | cut;
        ok &= nc_zipstream_close(sb) | (blen > lb->length) | cut;
        if (!ok) {
            snprintf(rep->errstr, LITHOGEN_ERRSTR_MAX,
                     "cannot read %s/%s (bad zip data)", la->file, lb->file);
            st = LITHOGEN_ERR_ZIP;
            goto out;
        }
    }

    {
        uint8_t *p = (uint8_t *)realloc(pool, used);
        if (p)
            pool = p;
    }
    r->data[NC_REG_C] = pool;
    r->size[NC_REG_C] = reg->size;
    r->cidx = cidx;
    r->cpoolsz = used;
    pool = NULL;
    cidx = NULL;
    st = LITHOGEN_OK;
out:
    if (st == LITHOGEN_ERR_NOMEM)
        snprintf(rep->errstr, LITHOGEN_ERRSTR_MAX, "out of memory");
    free(pool);
    free(cidx);
    free(htab);
    free(ina);
    free(inb);
    free(win);
    return st;
}

int nc_regions_build(const nc_game *g, nc_zipset *zs, nc_regions *r,
                     const lithogen_options *opt, lithogen_report *rep)
{
    int ri;

    uint32_t p_extent = 0, s_extent = 0;
    uint32_t v1_extent = 0, v2_extent = 0;
    uint32_t m_extent = 0, mx_extent = 0;
    uint32_t v2_copy_src = 0xffffffffu;   /* lowest V1 offset copied into V2 */
    int v2_all_copies = -1;               /* -1: no V2 loads seen yet        */
#define TRACK_P(end) do { \
        if (reg->kind == NC_REG_P && (uint32_t)(end) > p_extent) \
            p_extent = (uint32_t)(end); \
        if (reg->kind == NC_REG_S && from_file && (uint32_t)(end) > s_extent) \
            s_extent = (uint32_t)(end); \
        if (reg->kind == NC_REG_M && from_file && (uint32_t)(end) > m_extent) \
            m_extent = (uint32_t)(end); \
        if (reg->kind == NC_REG_MX && from_file && (uint32_t)(end) > mx_extent) \
            mx_extent = (uint32_t)(end); \
        if (reg->kind == NC_REG_V1 && from_file && (uint32_t)(end) > v1_extent) \
            v1_extent = (uint32_t)(end); \
        if (reg->kind == NC_REG_V2 && from_file && (uint32_t)(end) > v2_extent) \
            v2_extent = (uint32_t)(end); \
    } while (0)

    int lowmem = 0, vspill = 0;          /* emu16: low-memory mode */
    memset(r, 0, sizeof(*r));

    if (opt->mem_avail && lowmem_cart(g->cart)) {
        uint64_t total = 0;
        for (ri = 0; ri < g->nregions; ri++)
            total += g->regions[ri].size;
        lowmem = total > opt->mem_avail;
    }
    if (lowmem && opt->v_spill) {
        vspill = 1;
        for (ri = 0; ri < g->nregions; ri++)
            if ((g->regions[ri].kind == NC_REG_V1 ||
                 g->regions[ri].kind == NC_REG_V2) &&
                !v_spill_ok(g, &g->regions[ri]))
                vspill = 0;
    }

    for (ri = 0; ri < g->nregions; ri++) {
        const nc_region *reg = &g->regions[ri];
        /* A few MAME audio descrambles (cthd family, kf2k5uni, matrimbl)
         * address the Z80 region through its mirrored address-space image.
         * Rather than modeling the mirrors, allocate generous scratch and
         * emit only the logical m1-sized image; the consumer regenerates
         * mirrors itself. */
        uint32_t alloc = reg->size;
        uint8_t *buf;
        const char *curfile = NULL;
        uint8_t *fdata = NULL;
        size_t flen = 0, fpos = 0;
        uint16_t li;
        /* emu16: low-memory mode */
        const int spill = vspill &&
            (reg->kind == NC_REG_V1 || reg->kind == NC_REG_V2);
        uint32_t key = 0;

        if (lowmem && reg->kind == NC_REG_C && c_dedup_ok(g, reg)) {
            int st = build_c_dedup(g, reg, zs, r, opt, rep);
            if (st != LITHOGEN_OK)
                return st;
            continue;
        }
        if (spill) {
            const int which = reg->kind == NC_REG_V2;
            key = v_spill_key(g, reg);
            r->vspill = 1;
            if (which)
                r->v2region = 1;
            if (opt->v_spill(which, key, NULL, 0, opt->v_spill_user)) {
                /* the caller still has it: only the extents are needed */
                const int from_file = 1;
                r->size[reg->kind] = reg->size;
                for (li = 0; li < reg->nloads; li++) {
                    const nc_load *ld = &g->loads[reg->load0 + li];
                    if (which)
                        v2_all_copies = 0;
                    if ((ld->kind == NC_LOAD && ld->group) ||
                        ld->kind == NC_CONTINUE)
                        TRACK_P(ld->offset +
                                load_span(ld->length, ld->group, ld->skip));
                }
                continue;
            }
        }

        if (reg->kind == NC_REG_M && alloc < 0x90000)
            alloc = 0x90000;
        buf = (uint8_t *)malloc(alloc ? alloc : 1);
        if (!buf) {
            snprintf(rep->errstr, LITHOGEN_ERRSTR_MAX, "out of memory");
            return LITHOGEN_ERR_NOMEM;
        }
        memset(buf, reg->erasefill, alloc);
        r->data[reg->kind] = buf;
        r->size[reg->kind] = reg->size;

        for (li = 0; li < reg->nloads; li++) {
            const nc_load *ld = &g->loads[reg->load0 + li];

            int from_file = (ld->kind == NC_LOAD || ld->kind == NC_CONTINUE);
            if (reg->kind == NC_REG_V2 && ld->kind != NC_COPY)
                v2_all_copies = 0;
            switch ((nc_load_kind)ld->kind) {
            case NC_LOAD: {
                size_t got = 0;
                uint32_t gotcrc = 0;

                if (ld->group == 0) {   /* NO_DUMP: leave erase fill */
                    rep->missing_optional++;
                    free(fdata);
                    fdata = NULL;
                    curfile = NULL;
                    break;
                }
                free(fdata);
                fdata = nc_zipset_read(zs, ld->file, ld->crc, ld->length,
                                       &got, &gotcrc);
                if (!fdata) {
                    if (ld->optional) {
                        nc_warn(opt, "optional ROM %s not found; region"
                                " left empty", ld->file);
                        rep->missing_optional++;
                        curfile = NULL;
                        break;
                    }
                    snprintf(rep->errstr, LITHOGEN_ERRSTR_MAX,
                             "missing ROM: %s (crc %08x) for set %s",
                             ld->file, ld->crc, g->name);
                    return LITHOGEN_ERR_MISSING_ROM;
                }
                curfile = ld->file;
                flen = got;
                fpos = 0;
                if (gotcrc != ld->crc) {
                    rep->crc_mismatches++;
                    nc_warn(opt, "%s: crc %08x, expected %08x",
                            ld->file, gotcrc, ld->crc);
                    if (opt->strict_crc) {
                        snprintf(rep->errstr, LITHOGEN_ERRSTR_MAX,
                                 "CRC mismatch on %s", ld->file);
                        free(fdata);
                        return LITHOGEN_ERR_CRC;
                    }
                }
                if (flen < ld->length)
                    nc_warn(opt, "%s: file is %zu bytes, expected %u",
                            ld->file, flen, ld->length);
                {
                    uint32_t n = ld->length;
                    if (fpos + n > flen)
                        n = (uint32_t)(flen > fpos ? flen - fpos : 0);
                    copy_groups(buf, ld->offset, alloc, fdata + fpos, n,
                                ld->group, ld->skip, ld->reverse);
                    TRACK_P(ld->offset + load_span(n, ld->group, ld->skip));
                    fpos += ld->length;
                }
                break;
            }
            case NC_CONTINUE: {
                uint32_t n = ld->length;
                if (!curfile || !fdata) {
                    snprintf(rep->errstr, LITHOGEN_ERRSTR_MAX,
                             "ROM_CONTINUE without file in %s", g->name);
                    return LITHOGEN_ERR_ARGS;
                }
                if (fpos + n > flen)
                    n = (uint32_t)(flen > fpos ? flen - fpos : 0);
                copy_groups(buf, ld->offset, alloc, fdata + fpos, n,
                            ld->group, ld->skip, ld->reverse);
                TRACK_P(ld->offset + load_span(n, ld->group, ld->skip));
                fpos += ld->length;
                break;
            }
            case NC_RELOAD: {
                uint32_t n = ld->length;
                if (!curfile || !fdata)
                    break;              /* preceding load was optional+absent */
                if (n > flen)
                    n = (uint32_t)flen;
                copy_groups(buf, ld->offset, alloc, fdata, n,
                            ld->group, ld->skip, ld->reverse);
                if (reg->kind == NC_REG_M) {
                    r->m_window_off = ld->offset;
                    r->m_window_len = ld->length;
                }
                break;
            }
            case NC_IGNORE:
                fpos += ld->length;
                break;
            case NC_FILL:
                /* fills initialize RAM windows (jockeygp) or scratch;
                 * they do not extend the populated extent */
                if (ld->offset + ld->length <= reg->size)
                    memset(buf + ld->offset, (int)(ld->crc & 0xff), ld->length);
                break;
            case NC_COPY: {
                /* source region resolved by kind name emitted by gendb */
                nc_region_kind sk = NC_REG_COUNT;
                if (!strcmp(ld->file, "P")) sk = NC_REG_P;
                else if (!strcmp(ld->file, "S")) sk = NC_REG_S;
                else if (!strcmp(ld->file, "M")) sk = NC_REG_M;
                else if (!strcmp(ld->file, "MX")) sk = NC_REG_MX;
                else if (!strcmp(ld->file, "V1")) sk = NC_REG_V1;
                else if (!strcmp(ld->file, "V2")) sk = NC_REG_V2;
                else if (!strcmp(ld->file, "C")) sk = NC_REG_C;
                if (sk == NC_REG_COUNT || !r->data[sk] ||
                    ld->crc + ld->length > r->size[sk] ||
                    ld->offset + ld->length > reg->size) {
                    snprintf(rep->errstr, LITHOGEN_ERRSTR_MAX,
                             "bad ROM_COPY in %s", g->name);
                    return LITHOGEN_ERR_ARGS;
                }
                memcpy(buf + ld->offset, r->data[sk] + ld->crc, ld->length);
                TRACK_P(ld->offset + ld->length);
                if (reg->kind == NC_REG_V2 && sk == NC_REG_V1) {
                    if (v2_all_copies == -1)
                        v2_all_copies = 1;
                    if (ld->crc < v2_copy_src)
                        v2_copy_src = ld->crc;
                } else if (reg->kind == NC_REG_V2) {
                    v2_all_copies = 0;
                }
                break;
            }
            }
        }
        free(fdata);
        (void)region_index;
        if (spill) {
            /* emu16: low-memory mode: the populated part goes to the
             * caller, and the region keeps only its size */
            uint32_t ext = reg->kind == NC_REG_V2 ? v2_extent : v1_extent;
            uint32_t n = ext ? (ext + 0xffffu) & ~0xffffu : reg->size;
            if (n > reg->size)
                n = reg->size;
            if (!opt->v_spill(reg->kind == NC_REG_V2, key, buf, n,
                              opt->v_spill_user)) {
                snprintf(rep->errstr, LITHOGEN_ERRSTR_MAX,
                         "cannot store the sound ROM cache");
                return LITHOGEN_ERR_IO;
            }
            free(buf);
            r->data[reg->kind] = NULL;
        }
    }

#undef TRACK_P
    /* round the populated extent up to 64K and record it */
    if (p_extent) {
        p_extent = (p_extent + 0xffffu) & ~0xffffu;
        if (p_extent > r->size[NC_REG_P])
            p_extent = r->size[NC_REG_P];
    }
    if (s_extent) {
        s_extent = (s_extent + 0xffffu) & ~0xffffu;
        if (s_extent > r->size[NC_REG_S])
            s_extent = r->size[NC_REG_S];
    }
    if (m_extent) {
        m_extent = (m_extent + 0xffffu) & ~0xffffu;
        if (m_extent > r->size[NC_REG_M])
            m_extent = r->size[NC_REG_M];
    }
    if (mx_extent) {
        mx_extent = (mx_extent + 0xffffu) & ~0xffffu;
        if (mx_extent > r->size[NC_REG_MX])
            mx_extent = r->size[NC_REG_MX];
    }
    r->p_extent = p_extent;
    r->s_extent = s_extent;
    r->m_extent = m_extent;
    r->mx_extent = mx_extent;

    /* V regions: like P, MAME may declare them at a mapped-window size
     * larger than the file data (diggerma, shocktr2, prototypes); the
     * .neo stores the populated extent. */
    if (v1_extent) {
        v1_extent = (v1_extent + 0xffffu) & ~0xffffu;
        if (v1_extent < r->size[NC_REG_V1])
            r->size[NC_REG_V1] = v1_extent;
    }
    if (v2_extent) {
        v2_extent = (v2_extent + 0xffffu) & ~0xffffu;
        if (v2_extent < r->size[NC_REG_V2])
            r->size[NC_REG_V2] = v2_extent;
    }

    /* Prototype boards (alpham2p, burningfp) share one physical V ROM
     * between both ADPCM buses: MAME loads it at the tail of adpcma and
     * ROM_COPYs it into adpcmb.  The .neo convention stores each file
     * once, so when V2 is entirely a copy of V1's tail, trim V1 to the
     * copy source and let V2 carry the data. */
    if (v2_all_copies == 1 &&
        v2_copy_src + r->size[NC_REG_V2] == r->size[NC_REG_V1])
        r->size[NC_REG_V1] = v2_copy_src;

    /* a cart audiocpu window with no file data (dragonsh) ships as a
     * standard empty 128K M */
    if (m_extent == 0 && !r->data[NC_REG_MX]) {
        if (r->size[NC_REG_M] > 0x20000)
            r->size[NC_REG_M] = 0x20000;
        if (r->data[NC_REG_M])
            memset(r->data[NC_REG_M], 0, r->size[NC_REG_M]);
    }

    /* a set without a usable fixed region (dragonsh) still needs a
     * standard empty S in the output */
    if (!r->data[NC_REG_S] || r->size[NC_REG_S] == 0) {
        free(r->data[NC_REG_S]);
        r->data[NC_REG_S] = (uint8_t *)calloc(0x20000, 1);
        r->size[NC_REG_S] = r->data[NC_REG_S] ? 0x20000 : 0;
    }

    /* post phase: byte swap the 16-bit big-endian program region */
    if (r->data[NC_REG_P]) {
        uint8_t *p = r->data[NC_REG_P];
        uint32_t i, n = r->size[NC_REG_P] & ~1u;
        for (i = 0; i < n; i += 2) {
            uint8_t t = p[i];
            p[i] = p[i + 1];
            p[i + 1] = t;
        }
    }
    return LITHOGEN_OK;
}

void nc_regions_free(nc_regions *r)
{
    int i;
    for (i = 0; i < NC_REG_COUNT; i++) {
        free(r->data[i]);
        r->data[i] = NULL;
    }
    free(r->cidx);                         /* emu16 */
    r->cidx = NULL;
}
